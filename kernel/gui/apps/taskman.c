/* =============================================================================
 * taskman.c — Task Manager app (M22.3).
 *
 * Singleton window: a listview of every task (pid, state, CPU ms,
 * name), refreshed ~1 Hz through the gui_window_set_tick hook, plus an
 * "End task" button wired to task_kill.
 *
 * Kill semantics = the kthread contract (see task.h): the victim dies
 * at its next yield / task_should_stop() poll.  Guarded: pid 0 and
 * idle tasks are refused by task_kill itself; the compositor is
 * refused here by name (killing it would freeze the GUI).  Killing a
 * window's shell is allowed — the window's X button is the graceful
 * path, this is the hammer.
 *
 * M27 — the Task Manager no longer reaps: init is the universal reaper
 * (task.c), so DEAD non-owned tasks vanish on their own; window-owned
 * DEAD tasks are reaped by their window teardown (M22.6 auto-close).
 * The list is now a process TREE (children indented under their parent
 * via ppid), refreshed event-driven on any task-set change
 * (task_set_change_hook) plus the ~1 Hz tick.
 *
 * All callbacks (tick, button, listview) run on the compositor task,
 * so widget state needs no extra locking.
 * ============================================================================= */

#include "gui.h"
#include "console_plate.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "locale.h"
#include "itemview.h"
#include "task.h"
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

/* M27 — a snapshot row.  Collected on the heap (in struct taskman, not on
 * the compositor's 4 KiB stack) so the tree walk can order children under
 * parents without a second locked pass. */
struct tm_row {
    int  pid, ppid;
    enum task_state st;
    uint32_t cpu_ms;
    char name[TASK_NAME_MAX + 1];
    char is_current;
    char emitted;
};

#define TM_MAX_ROWS  WLIST_MAX_ITEMS

struct taskman {
    struct w_itemview* iv;                 /* the table (§M65's item view)     */
    struct w_label*    status;
    struct w_button*   btn_end;            /* cooperative kill (End task)      */
    struct w_button*   btn_fkill;          /* §M46 force kill                  */
    struct item_model  model;

    struct tm_row rows[TM_MAX_ROWS];       /* M27 — tree snapshot, walk order  */
    int  nrows;
    /* THE DISPLAY ORDER, SEPARATE FROM THE SNAPSHOT.  `rows` arrives in
     * whatever order task_for_each walks; the tree walk then decides what is
     * shown where, and the model answers questions about DISPLAY positions.
     * Keeping them apart is what lets the model be a plain index lookup
     * instead of re-walking the tree per cell. */
    int  order[TM_MAX_ROWS];               /* display index → rows[] index     */
    int  depth[TM_MAX_ROWS];               /* tree depth, for the NAME indent  */
    int  row_pids[TM_MAX_ROWS];            /* display index → pid              */
    int  ndisp;
    uint32_t cpu_total_ms;                 /* the footer's right-hand figure   */
};

static struct gui_window* tm_win = NULL;

/* ---- tiny formatting helpers (no snprintf in a freestanding build) --------- */

static int put_str(char* d, int p, int cap, const char* s) {
    for (; s && *s && p < cap - 1; s++) d[p++] = *s;
    return p;
}

static int put_u32_pad(char* d, int p, int cap, uint32_t v, int width) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    for (int i = n; i < width && p < cap - 1; i++) d[p++] = ' ';
    while (n && p < cap - 1) d[p++] = tmp[--n];
    return p;
}

static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static const char* state_short(enum task_state st) {
    switch (st) {
    case TASK_RUNNABLE: return "RUN ";
    case TASK_SLEEPING: return "SLP ";
    case TASK_DEAD:     return "DEAD";
    default:            return "?   ";
    }
}

/* ---- refresh ----------------------------------------------------------------
 *
 * M27 — the list is a process TREE: children are indented under their
 * parent.  DEAD tasks are no longer reaped here (init is the universal
 * reaper now — see task.c); the Task Manager purely displays.  A snapshot
 * is collected under the master lock (one pass, on the heap), then walked
 * into tree order so we never nest task_for_each. */

static void tm_collect(const struct task* t, int is_current, void* ctx) {
    struct taskman* tm = (struct taskman*)ctx;
    if (tm->nrows >= WLIST_MAX_ITEMS) return;
    struct tm_row* r = &tm->rows[tm->nrows++];
    r->pid = t->pid; r->ppid = t->ppid; r->st = t->state;
    r->cpu_ms = (uint32_t)t->cpu_ms; r->is_current = (char)is_current;
    r->emitted = 0;
    int i = 0;
    for (; t->name[i] && i < TASK_NAME_MAX; i++) r->name[i] = t->name[i];
    r->name[i] = 0;
}

/* Place one row at the next display position.  No text is built here any more:
 * a row is four CELLS the table asks for when it paints them, which is what
 * makes per-column alignment, face and colour possible at all — a pre-padded
 * string can only ever be one of those things. */
static void tm_emit(struct taskman* tm, struct tm_row* r, int depth) {
    if (tm->ndisp >= TM_MAX_ROWS) return;
    tm->order[tm->ndisp] = (int)(r - tm->rows);
    tm->depth[tm->ndisp] = depth;
    tm->row_pids[tm->ndisp] = r->pid;
    tm->ndisp++;
}

/* Emit `r` then, depth-first, its children.  The emitted flag guards
 * against ppid cycles, so recursion depth is bounded by the tree height
 * (a handful in practice) even though the array is walked at each level. */
static void tm_emit_subtree(struct taskman* tm, struct tm_row* r, int depth) {
    r->emitted = 1;
    tm_emit(tm, r, depth);
    for (int i = 0; i < tm->nrows; i++) {
        struct tm_row* c = &tm->rows[i];
        if (c->emitted || c->pid == r->pid) continue;
        if (c->ppid == r->pid) tm_emit_subtree(tm, c, depth + 1);
    }
}

/* ---- the table model ---------------------------------------------------------
 *
 * Four columns, each styled by what it MEANS — the design's §06 table: the
 * identity in the accent colour, the numbers right-aligned and monospaced so
 * digits line up, the state muted, the name in the ordinary proportional face.
 * None of that is expressible in the space-padded string this app used to
 * build, which is why the columns had to become real before the styling could.
 */

/* THE ORDER IS THE DESIGN'S: identity, name, then the numbers on the right.
 *
 * It was PID / STATE / CPU / NAME, and the name column — the one that absorbs
 * whatever width is left over — was therefore the LAST one, so its text sat
 * left-aligned against a third of the window of empty space.  With the wide
 * column in the MIDDLE the right-aligned numbers land on the right edge, which
 * is what the catalogue's table does and why it reads as full.
 *
 * It also decides what a narrow window loses: t_layout drops from the right,
 * so STATE goes first and the identity plus the name survive longest. */
enum { TM_COL_PID = 0, TM_COL_NAME, TM_COL_CPU, TM_COL_STATE, TM_NCOLS };

static int tm_m_count(void* ctx) {
    return ((struct taskman*)ctx)->ndisp;
}

static int tm_m_columns(void* ctx) { (void)ctx; return TM_NCOLS; }

static const char* tm_m_col_title(void* ctx, int col) {
    (void)ctx;
    switch (col) {
    case TM_COL_PID:   return "PID";
    case TM_COL_STATE: return "STATE";
    case TM_COL_CPU:   return "CPU";
    default:           return "NAME";
    }
}

static int tm_m_col_style(void* ctx, int col) {
    (void)ctx;
    switch (col) {
    /* The PID is the record's identity — the one column you scan down. */
    case TM_COL_PID:   return ICOL_RIGHT | ICOL_MONO | ICOL_ACCENT;
    case TM_COL_STATE: return ICOL_DIM;
    /* RIGHT and MONO together, and both are load-bearing: right-aligned so the
     * magnitudes line up, monospaced so the digits within them do.  Either
     * alone leaves a column of numbers you cannot compare at a glance. */
    case TM_COL_CPU:   return ICOL_RIGHT | ICOL_MONO;
    default:           return 0;
    }
}

static int tm_m_cell(void* ctx, int index, int col, char* out, int cap) {
    struct taskman* tm = (struct taskman*)ctx;
    out[0] = 0;
    if (index < 0 || index >= tm->ndisp) return -1;
    struct tm_row* r = &tm->rows[tm->order[index]];
    int p = 0;
    switch (col) {
    case TM_COL_PID:
        p = put_u32_pad(out, p, cap, (uint32_t)r->pid, 0);
        break;
    case TM_COL_STATE:
        p = put_str(out, p, cap, state_short(r->st));
        break;
    case TM_COL_CPU:
        p = put_u32_pad(out, p, cap, r->cpu_ms, 0);
        p = put_str(out, p, cap, " ms");
        break;
    default:
        /* THE TREE SURVIVES THE MOVE TO COLUMNS, as an indent inside the NAME
         * cell.  §M27 built the parent/child view deliberately and it is the
         * one thing a flat pid-sorted table would lose; leading spaces are the
         * only part of the old padded row that was doing real work. */
        for (int d = 0; d < tm->depth[index] && d < 12; d++)
            p = put_str(out, p, cap, "  ");
        p = put_str(out, p, cap, r->name);
        if (r->is_current) p = put_str(out, p, cap, " *");
        break;
    }
    out[p] = 0;
    return 0;
}

/* `get` still answers, so the SAME model renders in the list and grid views —
 * `taskman.view` could be a config key tomorrow without touching this app. */
static int tm_m_get(void* ctx, int index, struct item_entry* out) {
    struct taskman* tm = (struct taskman*)ctx;
    if (index < 0 || index >= tm->ndisp) return -1;
    out->label = tm->rows[tm->order[index]].name;
    out->sub = NULL;
    out->icon = ICON_CHART;
    out->dim = 0;
    return 0;
}

static int tm_pid_present(struct taskman* tm, int pid) {
    for (int i = 0; i < tm->nrows; i++) if (tm->rows[i].pid == pid) return 1;
    return 0;
}

static void tm_refresh(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    if (!tm || !tm->iv) return;

    /* THE SELECTION IS RESTORED BY PID, NOT BY POSITION.  A task appearing or
     * leaving shifts every row below it, and a selection kept as an index would
     * silently move onto a different process — which is the one place in this
     * window where being wrong is dangerous rather than untidy, since the
     * buttons next to it kill what is selected. */
    int selpid = -1;
    if (tm->iv->sel >= 0 && tm->iv->sel < tm->ndisp)
        selpid = tm->row_pids[tm->iv->sel];

    tm->nrows = 0;
    task_for_each(tm_collect, tm);

    tm->ndisp = 0;
    /* Roots first (parent not in the snapshot, or self-parent like pid 0),
     * each pulling its subtree; then any stragglers a cycle left behind. */
    for (int i = 0; i < tm->nrows; i++) {
        struct tm_row* r = &tm->rows[i];
        if (r->emitted) continue;
        if (r->pid == r->ppid || !tm_pid_present(tm, r->ppid))
            tm_emit_subtree(tm, r, 0);
    }
    for (int i = 0; i < tm->nrows; i++)
        if (!tm->rows[i].emitted) tm_emit_subtree(tm, &tm->rows[i], 0);

    tm->cpu_total_ms = 0;
    tm->iv->sel = -1;
    for (int i = 0; i < tm->ndisp; i++) {
        tm->cpu_total_ms += tm->rows[tm->order[i]].cpu_ms;
        if (tm->row_pids[i] == selpid) tm->iv->sel = i;
    }
    if (tm->iv->scroll > tm->ndisp - 1)
        tm->iv->scroll = tm->ndisp > 0 ? tm->ndisp - 1 : 0;

    /* THE FOOTER ALWAYS SAYS SOMETHING, AND IT HAS TWO SIDES.  The design's
     * table carries a summary band at all times — the count on the left, the
     * total on the right ("12 folyamat · 1 kijelölve" … "28.6 MB").  Ours was
     * left-only, so the band read as a caption rather than as the table's
     * bottom rule; and it was EMPTY except right after an action, which made
     * it look like a drawing artefact. */
    if (tm->status) {
        char sum[80];
        int p = 0;
        p = put_u32_pad(sum, p, (int)sizeof sum, (uint32_t)tm->ndisp, 0);
        p = put_str(sum, p, (int)sizeof sum, " tasks");
        if (selpid >= 0) {
            p = put_str(sum, p, (int)sizeof sum, "  -  pid ");
            p = put_u32_pad(sum, p, (int)sizeof sum, (uint32_t)selpid, 0);
            p = put_str(sum, p, (int)sizeof sum, " selected");
        }
        sum[p] = 0;
        char tot[32];
        int q = 0;
        q = put_u32_pad(tot, q, (int)sizeof tot, tm->cpu_total_ms, 0);
        q = put_str(tot, q, (int)sizeof tot, " ms cpu");
        tot[q] = 0;
        /* THE FOOTER DAMAGES ITSELF, and only when it says something new.
         * The host no longer repaints a window because its tick ran (gui.h),
         * so a label written and not damaged is a label that never updates —
         * and one damaged every second is a strip of the window blitted every
         * second to say the same thing.  The comparison is what makes the
         * common case free. */
        if (!str_eq(sum, tm->status->text) || !str_eq(tot, tm->status->text2)) {
            w_label_set(tm->status, sum);
            w_label_set_trailing(tm->status, tot);
            gui_window_request_redraw_rect(win, tm->status->base.x,
                                           tm->status->base.y,
                                           tm->status->base.w,
                                           tm->status->base.h);
        }
    }

    /* ONLY THE ROWS THAT CHANGED — and the widget owns that comparison now, so
     * every table in this system inherits it instead of each app writing its
     * own diff.  What this used to do by hand is w_itemview_refresh. */
    w_itemview_refresh(tm->iv);
}

/* ---- callbacks --------------------------------------------------------------- */

/* Put a message in the footer band and damage exactly that band.  Every status
 * write goes through here for the reason gui.h now states: nothing repaints a
 * window on the app's behalf, so an undamaged label is one nobody ever sees. */
static void tm_say(struct taskman* tm, const char* msg) {
    if (!tm || !tm->status || !tm_win) return;
    w_label_set(tm->status, msg);
    gui_window_request_redraw_rect(tm_win, tm->status->base.x,
                                   tm->status->base.y,
                                   tm->status->base.w, tm->status->base.h);
}

/* Resolve the selected pid, applying the shared guards (a selection must
 * exist; the compositor is refused — killing it would freeze the GUI).
 * Returns the pid, or -1 after setting an explanatory status message. */
static int tm_selected_pid(struct taskman* tm) {
    if (tm->iv->sel < 0 || tm->iv->sel >= tm->ndisp) {
        tm_say(tm, "select a task first");
        return -1;
    }
    int pid = tm->row_pids[tm->iv->sel];
    struct task* t = task_find(pid);
    if (t && str_eq(t->name, "compositor")) {
        tm_say(tm, "refusing: that would freeze the GUI");
        return -1;
    }
    return pid;
}

/* "End task" — the cooperative kill (kthread contract): the victim dies at its
 * next yield / task_should_stop() poll.  Cannot reclaim a wedged ring-3 task. */
static void tm_kill(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct taskman* tm = (struct taskman*)gui_window_ctx(tm_win);
    if (!tm) return;
    int pid = tm_selected_pid(tm);
    if (pid < 0) return;
    int rc = task_kill(pid);
    /* THE REFRESH FIRST, THE MESSAGE AFTER.  The other order set the label and
     * then had tm_refresh overwrite it with the summary in the same call, so
     * the answer to a button press was never on screen for a single frame. */
    tm_refresh(tm_win);
    tm_say(tm, rc == 0 ? "flagged - dies at next yield"
                       : "not found or protected (pid 0 / idle)");
}

/* §M46 "Force kill" — the hammer: sets the forced flag so a WEDGED ring-3 task
 * (a frozen browser that never yields) is torn down at its next timer
 * preemption in user mode, where task_kill's cooperative poll can never land. */
static void tm_kill_force(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct taskman* tm = (struct taskman*)gui_window_ctx(tm_win);
    if (!tm) return;
    int pid = tm_selected_pid(tm);
    if (pid < 0) return;
    int rc = task_force_kill(pid);
    tm_refresh(tm_win);
    tm_say(tm, rc == 0 ? "force-killed (dies at next preemption)"
                       : "not found or protected (pid 0 / idle)");
}

static void tm_tick(struct gui_window* win) {
    tm_refresh(win);                     /* ~1 Hz auto-refresh */
}

static void tm_layout(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    if (!tm || !tm->iv) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    /* Bottom band, top → bottom: [ button row ][ status footer ].
     * The table fills the rest — and it draws its OWN column header now, so
     * there is no separate header label to leave room for. */
    /* DERIVED FROM THE FONT.  These were 18 and 100 — one glyph-height plus
     * padding, and twelve 8 px characters — so at `gui.font_scale` 2 the label
     * was wider than its button and got clipped at BOTH ends ("nd tas" for
     * "End task").  The widest label decides the width; a button sized for a
     * text size it no longer has is a button with the text cut out of it.
     *
     * NOTE FOR THE NEXT APP: the constructor's sizes do not survive — this
     * function overwrites them on every layout, which is why fixing
     * w_button_create alone changed nothing. */
    /* A button is `control_h` tall, like every other control in the design —
     * not glyph-plus-padding.  Same argument as the row height. */
    const int BTN_H = cp_current_density()->control_h;
    const int BTN_W = 11 * cp_fw() + 12;        /* "Force kill" + padding */
    const int GAP   = 8;
    int status_y = ch - cp_fh() - 6;
    int btn_y    = status_y - BTN_H - 4;
    /* THE TABLE STARTS AT THE TOP EDGE AND RUNS TO BOTH SIDES.  The design's
     * table is a pane, not a box floating in a margin, and giving it the full
     * width is half of fixing "the right 40 % is empty" — the other half is the
     * columns themselves, which now share that width. */
    tm->iv->base.x = 0;   tm->iv->base.y = 0;
    tm->iv->base.w = cw;
    tm->iv->base.h = (btn_y - 6);
    if (tm->iv->base.h < cp_row_h() * 2) tm->iv->base.h = cp_row_h() * 2;
    /* A resize changes how many slots there are, so the cached signature stops
     * describing the pane; the widget notices and takes its whole-pane branch. */
    tm->iv->sig_valid = 0;
    /* Two action buttons below the list: End task (cooperative) + Force kill. */
    if (tm->btn_end) {
        tm->btn_end->base.x = 8;              tm->btn_end->base.y = btn_y;
        tm->btn_end->base.w = BTN_W;          tm->btn_end->base.h = BTN_H;
    }
    if (tm->btn_fkill) {
        tm->btn_fkill->base.x = 8 + BTN_W + GAP; tm->btn_fkill->base.y = btn_y;
        tm->btn_fkill->base.w = BTN_W;           tm->btn_fkill->base.h = BTN_H;
    }
    /* The footer band, as the catalogue draws it: a full-width `tray` strip
     * with a rule above and the summary in caption style. */
    tm->status->base.x = 0;  tm->status->base.y = status_y;
    tm->status->base.w = cw;  tm->status->base.h = cp_fh() + 6;
}

static void tm_on_close(struct gui_window* win) {
    (void)win;
    tm_win = NULL;
}

static void taskman_open(void) {
    if (tm_win) { gui_window_raise(tm_win); return; }

    struct taskman* tm = (struct taskman*)kcalloc(1, sizeof(*tm));
    if (!tm) return;

    struct gui_window* win =
        /* Tall enough for TEN rows at the density's row height, plus the header
         * band, the button row, the footer and the title bar.  The old height
         * was derived from the font and showed four rows once a row became a
         * design-sized 32 px. */
        gui_app_window_create("Task Manager", 300, 120,
                              58 * cp_fw() + 20,
                              10 * cp_row_h() + 3 * cp_current_density()->control_h
                                  + 3 * cp_fh() + 24,
                              tm_layout, tm);
    if (!win) { kfree(tm); return; }
    tm_win = win;
    gui_window_set_on_close(win, tm_on_close);

    /* THE HEADER IS THE TABLE'S NOW, not a label with spaces in it.
     * `"pid   st     cpu    name"` lined up under one fixed advance and under
     * nothing else — the moment §M69 brought a proportional face in, the four
     * words stopped standing over the four columns.  A view that is given the
     * column titles cannot have that problem, because it is the same code that
     * places the cells. */
    tm->model.count     = tm_m_count;
    tm->model.get       = tm_m_get;
    tm->model.ctx       = tm;
    tm->model.columns   = tm_m_columns;
    tm->model.col_title = tm_m_col_title;
    tm->model.cell      = tm_m_cell;
    tm->model.col_style = tm_m_col_style;
    tm->iv = w_itemview_create(win, 0, 0, 100, 100, &tm->model, "table", tm);

    /* Two action buttons below the list (positioned by tm_layout): the
     * cooperative "End task" and the §M46 "Force kill" hammer. */
    /* The action this window exists for gets the accent; the escalation next
     * to it stays a plate.  The design's dialog does exactly this with
     * "Formázás" and "Mégse". */
    tm->btn_end   = w_button_create(win, 8, 19 * cp_fh() + 14,
                                    10 * cp_fw() + 8, cp_fh() + 8, "End task",
                                    tm_kill, tm);
    tm->btn_fkill = w_button_create(win, 12 * cp_fw() + 16, 19 * cp_fh() + 14,
                                    12 * cp_fw() + 8, cp_fh() + 8, "Force kill",
                                    tm_kill_force, tm);
    tm->status = w_label_create(win, 0, 0, 900, "");
    if (tm->status) w_label_set_caption(tm->status, 2);   /* 2 = footer */
    /* The action this window exists for carries the accent; the escalation
     * beside it stays a plate.  The catalogue's dialog does exactly this with
     * "Formázás" (primary) next to "Mégse" (secondary). */
    if (tm->btn_end) w_button_set_emphasis(tm->btn_end, CP_BTN_PRIMARY);
    if (!tm->iv || !tm->status || !tm->btn_end || !tm->btn_fkill) {
        gui_window_close(win); return;
    }
    tm->iv->sel = -1;

    gui_window_set_tick(win, tm_tick);
    tm_layout(win);
    tm_refresh(win);
}

GUI_APP_ICON("Task Manager", taskman_open, ICON_CHART);
