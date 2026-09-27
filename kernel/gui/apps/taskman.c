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
#include "ui.h"          /* §M75 — ui_class_find for the chart */
#include "locale.h"
#include "printf.h"
#include "config.h"
#include "itemview.h"
extern unsigned iv_stat_pane, iv_stat_cells, iv_stat_rows, iv_stat_colmove;
#include "task.h"
#include "swap.h"
#include "timer.h"       /* §M75 — the delta is over a CLOCK  */
#include "percpu.h"      /* §M75 — smp_ncpus: CPU%% is scaled by core count */
#include "sysmon.h"      /* §M75 — the four charts + the total  */
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

/* M27 — a snapshot row.  Collected on the heap (in struct taskman, not on
 * the compositor's 4 KiB stack) so the tree walk can order children under
 * parents without a second locked pass. */
struct tm_row {
    int  pid, ppid;
    enum task_state st;
    uint32_t cpu_ms;        /* CUMULATIVE since the task started               */
    uint32_t mem_kb;        /* private resident (§M75) — 0 for a kernel thread */
    uint32_t cpu_permille;  /* share of the machine SINCE THE LAST REFRESH     */
    char name[TASK_NAME_MAX + 1];
    char is_current;
    char emitted;
    char swapped;           /* §M72 stage 3 — paused with pages in the store */
};

/* §M75 — the previous refresh's CPU totals, so the percentage is a DELTA.
 *
 * KEYED BY PID, and here that is right where it was wrong for damage: §M69's
 * row diff compares BY SLOT because "does what will be drawn here differ from
 * what is drawn here" is the only question damage has an answer to.  A rate is
 * the opposite question — it is about ONE TASK over time, and comparing slot i
 * against slot i would attribute a departed task's CPU to whatever moved up
 * into its row.  Different questions, different keys. */
struct tm_prev { int pid; uint32_t cpu_ms; };

#define TM_MAX_ROWS  WLIST_MAX_ITEMS

struct taskman {
    struct w_itemview* iv;                 /* the table (§M65's item view)     */
    struct w_label*    status;
    struct w_button*   btn_end;            /* cooperative kill (End task)      */
    struct w_button*   btn_fkill;          /* §M46 force kill                  */
    struct item_model  model;

    struct tm_row rows[TM_MAX_ROWS];       /* M27 — tree snapshot, walk order  */
    int  nrows;

    /* §M75 — the delta baseline, and the wall-clock instant it was taken at.
     * THE INSTANT COMES FROM A CLOCK, NEVER FROM A COUNT OF TICKS: the host
     * refresh is nominally 1 Hz and under emulation is often not, and §M61
     * already paid for reading a tick count as if it were a duration. */
    struct tm_prev prev[TM_MAX_ROWS];
    int      nprev;
    uint64_t prev_ms;

    uint64_t total_mem_kb;                 /* summed over the rows, for the footer */
    struct widget* charts[SYSMON_NSERIES]; /* §M75 — four 1 Hz views of the ring   */
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
    /* §M72 — its own word, never RUN or SLP: a paused program that looks
     * like a wedged one gets force-killed by the person looking at it. */
    case TASK_STOPPED:  return "STOP";
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
    r->swapped = t->swapped_pages != 0;
    /* §M75 — `task_cpu_ms_now` and not `t->cpu_ms`: the raw field is credited
     * only at a context switch, so a task executing right now is short by
     * however long it has been on its CPU.  Small, and it is the quantity a
     * PERCENTAGE is the difference of — two samples each missing a different
     * in-flight slice are wrong by the difference, not by the slice. */
    r->cpu_ms = (uint32_t)task_cpu_ms_now(t);
    r->is_current = (char)is_current;
    r->emitted = 0;
    r->cpu_permille = 0;                    /* filled from the previous sample */

    /* Safe here because this runs inside `task_for_each`, which holds the lock
     * that keeps the space from being torn down under the walk (vmm.h). */
    uint64_t priv = 0;
    task_mem_bytes(t, &priv, NULL, NULL);
    r->mem_kb = (uint32_t)(priv / 1024u);
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
/* §M76.2 — CHILDREN COME OUT IN PID ORDER, and the reason is a report rather
 * than tidiness: *"the lower part of the table changes for a moment and then
 * goes back — practically a flicker."*
 *
 * MEASURED FIRST: the row count oscillates between **20 and 23** over five
 * seconds.  This machine's cron starts and reaps `audit`, `driver-rescan` and
 * `tick-log` continuously, each living milliseconds, and the 2-3 Hz refresh
 * catches some of them.  So the flicker is not a rendering fault — the table is
 * telling the truth about a busy machine.
 *
 * WHAT WAS FIXABLE IS WHERE THE TRUTH LANDS.  The children used to be emitted
 * in the master list's LINK order, so a task born a moment ago could appear
 * ANYWHERE among its parent's children — pushing every sibling after it, and
 * their whole subtrees, down a row.  In pid order the newest task always sorts
 * LAST among its siblings, so its arrival and departure move as little as the
 * shape of the tree allows.
 *
 * IT DOES NOT ABOLISH THE FLICKER AND IS NOT MEANT TO: a row inserted anywhere
 * still displaces what follows it.  Removing it entirely would mean not showing
 * short-lived tasks, which is hiding rather than ordering — and the one moment
 * a task manager must not lie is when something is spawning in a loop.
 *
 * O(n^2) over the sibling scan, which it already was; n is the task count. */
static void tm_emit_subtree(struct taskman* tm, struct tm_row* r, int depth) {
    r->emitted = 1;
    tm_emit(tm, r, depth);
    for (;;) {
        struct tm_row* next = NULL;
        for (int i = 0; i < tm->nrows; i++) {
            struct tm_row* c = &tm->rows[i];
            if (c->emitted || c->pid == r->pid) continue;
            if (c->ppid != r->pid) continue;
            if (!next || c->pid < next->pid) next = c;
        }
        if (!next) break;
        tm_emit_subtree(tm, next, depth + 1);
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
/* §M75 — six columns, and the ORDER decides what a narrow window loses,
 * because `t_layout` drops from the RIGHT (§M69).  Identity first, then what
 * the process is doing NOW; the cumulative figure and the state are what go.
 *
 * MEM and CPU% ANSWER DIFFERENT QUESTIONS FROM CPU(ms), which is why all three
 * are here rather than the new ones replacing the old: `cpu_ms` is CPU
 * CONSUMED since the task started, so a process that ran hard an hour ago and
 * has slept since outranks one pinning a core right now.  Both facts are
 * useful and neither substitutes for the other.
 *
 * A SEVENTH — OWNER — IS SPECIFIED AND DELIBERATELY ABSENT: §M32's ownership
 * does not exist yet, and a column reading `system` for everything looks
 * implemented while proving nothing (§M33's "isolation theatre", one window
 * over).  It is missing rather than blank. */
enum { TM_COL_PID = 0, TM_COL_NAME, TM_COL_MEM, TM_COL_CPUPCT,
       TM_COL_CPU, TM_COL_STATE, TM_NCOLS };

static int tm_m_count(void* ctx) {
    return ((struct taskman*)ctx)->ndisp;
}

static int tm_m_columns(void* ctx) { (void)ctx; return TM_NCOLS; }

static const char* tm_m_col_title(void* ctx, int col) {
    (void)ctx;
    switch (col) {
    case TM_COL_PID:    return "PID";
    case TM_COL_STATE:  return "STATE";
    case TM_COL_MEM:    return "MEM";
    case TM_COL_CPUPCT: return "CPU%";
    case TM_COL_CPU:    return "TIME";
    default:            return "NAME";
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
    /* Same reasoning as CPU(ms): right-aligned so magnitudes line up,
     * monospaced so the digits inside them do.  CPU% is ACCENT as well —
     * it is the column a person opens this window to read. */
    case TM_COL_MEM:    return ICOL_RIGHT | ICOL_MONO;
    case TM_COL_CPUPCT: return ICOL_RIGHT | ICOL_MONO | ICOL_ACCENT;
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
        /* §M72 stage 3 — SWAP, not STOP, for a paused program whose memory is
         * in the store: resuming it costs a read of everything it held, and a
         * STOP that is secretly a SWAP hides that its first moment back is
         * slow. */
        p = put_str(out, p, cap, r->swapped && r->st == TASK_STOPPED ? "SWAP"
                                                                     : state_short(r->st));
        break;
    case TM_COL_CPU:
        p = put_u32_pad(out, p, cap, r->cpu_ms, 0);
        p = put_str(out, p, cap, " ms");
        break;
    case TM_COL_MEM:
        /* A KERNEL THREAD PRINTS "-", NOT "0 KB".  Zero is what a process
         * holding nothing would show, and this machine runs ~40 tasks with no
         * address space at all — a column of zeroes would read as forty
         * processes that had somehow released their memory. */
        if (r->mem_kb == 0) { p = put_str(out, p, cap, "-"); break; }
        p = put_u32_pad(out, p, cap, r->mem_kb, 0);
        p = put_str(out, p, cap, " KB");
        break;
    case TM_COL_CPUPCT:
        /* Tenths, printed as a decimal — this kernel's printf has no float and
         * a bare tenths figure reads as ten times the truth. */
        p = put_u32_pad(out, p, cap, r->cpu_permille / 10u, 0);
        p = put_str(out, p, cap, ".");
        p = put_u32_pad(out, p, cap, r->cpu_permille % 10u, 0);
        p = put_str(out, p, cap, " %");
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

/* §M72 stage 3 — the Evict helper's hand-off (see tm_evict). */
static volatile int  tm_evict_pid = -1;
static char          tm_evict_msg[96];
static volatile int  tm_evict_done;
static void tm_say(struct taskman* tm, const char* msg);

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
    /* §M76.2 — roots in pid order too, for the same reason as the children:
     * a root that appears must not be able to land above the ones already
     * there. */
    for (;;) {
        struct tm_row* root = NULL;
        for (int i = 0; i < tm->nrows; i++) {
            struct tm_row* r = &tm->rows[i];
            if (r->emitted) continue;
            if (!(r->pid == r->ppid || !tm_pid_present(tm, r->ppid))) continue;
            if (!root || r->pid < root->pid) root = r;
        }
        if (!root) break;
        tm_emit_subtree(tm, root, 0);
    }
    for (int i = 0; i < tm->nrows; i++)
        if (!tm->rows[i].emitted) tm_emit_subtree(tm, &tm->rows[i], 0);

    /* §M75 — the CPU percentage, over the time that ACTUALLY passed.
     *
     * The first refresh has no baseline, so every row reads 0.0 % and that is
     * the honest answer rather than a made-up one: a rate needs two points,
     * and publishing the first absolute as a rate would open the window with a
     * spike the size of each task's lifetime. */
    uint64_t now_ms   = timer_ticks_ms();
    uint64_t span_ms  = (tm->prev_ms && now_ms > tm->prev_ms) ? now_ms - tm->prev_ms : 0;
    int      ncpu     = smp_ncpus() > 0 ? smp_ncpus() : 1;

    tm->cpu_total_ms = 0;
    tm->total_mem_kb = 0;
    tm->iv->sel = -1;
    for (int i = 0; i < tm->ndisp; i++) {
        struct tm_row* r = &tm->rows[tm->order[i]];
        tm->cpu_total_ms += r->cpu_ms;
        tm->total_mem_kb += r->mem_kb;

        if (span_ms) {
            for (int j = 0; j < tm->nprev; j++) {
                if (tm->prev[j].pid != r->pid) continue;
                /* A pid that was reaped and REUSED between two refreshes would
                 * give a nonsense delta; `task_cpu_permille` clamps at the
                 * machine's capacity, so the worst case saturates visibly
                 * rather than printing a plausible wrong number. */
                uint32_t d = (r->cpu_ms > tm->prev[j].cpu_ms)
                           ? r->cpu_ms - tm->prev[j].cpu_ms : 0;
                r->cpu_permille = task_cpu_permille(d, span_ms, ncpu);
                break;
            }
        }
        if (tm->row_pids[i] == selpid) tm->iv->sel = i;
    }

    /* Record the baseline for the NEXT refresh. */
    tm->nprev = 0;
    for (int i = 0; i < tm->ndisp && tm->nprev < TM_MAX_ROWS; i++) {
        struct tm_row* r = &tm->rows[tm->order[i]];
        tm->prev[tm->nprev].pid    = r->pid;
        tm->prev[tm->nprev].cpu_ms = r->cpu_ms;
        tm->nprev++;
    }
    tm->prev_ms = now_ms;
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
        /* §M75 — THE TOTAL, asked for precisely: under the table and NOT part
         * of it.  A total ROW would sort with the rows, scroll out of view
         * with them, and be selectable — three behaviours that are wrong for a
         * summary and all three inherited automatically from being a row.  As
         * the footer's trailing text it is simply always there.
         *
         * The MACHINE's CPU comes from §M75's sampler rather than from summing
         * the rows: the rows' percentages are each a delta over this window's
         * own refresh interval, and adding them up would double-count anything
         * that started or ended inside it.  The two figures are also a useful
         * cross-check — a table whose rows sum to far less than the machine's
         * load is a table missing a process. */
        char tot[64];
        int q = 0;
        q = put_str(tot, q, (int)sizeof tot, "TOTAL ");
        q = put_u32_pad(tot, q, (int)sizeof tot, (uint32_t)tm->total_mem_kb, 0);
        q = put_str(tot, q, (int)sizeof tot, " KB  -  cpu ");
        {
            uint32_t c = sysmon_latest(SYSMON_CPU);   /* tenths of a percent */
            q = put_u32_pad(tot, q, (int)sizeof tot, c / 10u, 0);
            q = put_str(tot, q, (int)sizeof tot, ".");
            q = put_u32_pad(tot, q, (int)sizeof tot, c % 10u, 0);
            q = put_str(tot, q, (int)sizeof tot, " %");
        }
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

    /* LAST, so nothing above overwrites the answer in the same call (the
     * footer did exactly that to every button message once — §M69). */
    if (tm_evict_done) { tm_evict_done = 0; tm_say(tm, tm_evict_msg); }
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

/* §M72 "Pause / Resume" — ONE button that does whichever the selected task
 * needs (the Device Manager's "Move" argument: a pair would always have one
 * half greyed out, and a control whose usual state is disabled is one nobody
 * learns).  A paused program keeps all its memory and resumes exactly where
 * it stopped; this is the rung below "End task". */
/* §M72 stage 3 — Evict runs on its OWN task: writing a program's memory out
 * takes seconds (1.8 s for 8 MiB measured), and this window's host must keep
 * drawing and answering meanwhile.  The helper posts its result back as DATA
 * (§M22.7: a window's widgets belong to its host task), consumed by the tick. */

static void tm_evict_task(void) {
    int pid = tm_evict_pid;
    struct swap_report rep;
    int rc = swap_stop_evict(pid, &rep);
    int n = 0;
    const char* a = rc == 0 ? "evicted - " : "paused, memory kept - ";
    while (*a && n < 60) tm_evict_msg[n++] = *a++;
    if (rc == 0) {
        char num[12]; int k = 0; uint32_t v = rep.evicted * 4u;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
        while (k && n < 80) tm_evict_msg[n++] = num[--k];
        a = " KB freed";
    } else {
        a = rep.why ? rep.why : "nothing could be written out";
    }
    while (*a && n < 95) tm_evict_msg[n++] = *a++;
    tm_evict_msg[n] = 0;
    tm_evict_done = 1;
    tm_evict_pid = -1;
}

static void tm_evict(void) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(tm_win);
    if (!tm) return;
    int pid = tm_selected_pid(tm);
    if (pid < 0) return;
    if (tm_evict_pid >= 0) { tm_say(tm, "an eviction is already running"); return; }
    struct task* t = task_find(pid);
    if (!t || !t->user_task) { tm_say(tm, "only programs can be paused"); return; }
    tm_evict_pid = pid;
    if (!task_spawn_detached("tm-evict", tm_evict_task)) {
        tm_evict_pid = -1;
        tm_say(tm, "could not start the eviction");
        return;
    }
    tm_say(tm, "pausing and writing its memory out...");
}

static void tm_pause(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct taskman* tm = (struct taskman*)gui_window_ctx(tm_win);
    if (!tm) return;
    int pid = tm_selected_pid(tm);
    if (pid < 0) return;
    struct task* t = task_find(pid);
    const char* msg;
    if (t && t->state == TASK_STOPPED) {
        msg = task_cont(pid) == 0 ? "resumed" : "could not resume";
    } else {
        int rc = task_stop(pid);
        msg = rc == 0  ? "pausing - it stops at its next moment in user mode"
            : rc == -3 ? "kernel threads cannot be paused"
            : rc == -2 ? "not yours" : "not found";
    }
    tm_refresh(tm_win);
    tm_say(tm, msg);
}

/* §M75 — WHAT THE REFRESH COSTS, reported rather than reasoned about.
 *
 * Reported from use: *"with the Task Manager running the CPU sits at 75 %."*
 * Confirmed here at 71.4 % on an otherwise idle -smp 4 box.  §M69 spent two
 * attempts aiming at a guess before `gui.stats_ms` existed, and the lesson it
 * wrote down applies exactly: a window whose cost is unmeasured is one nobody
 * can defend.  So the tick counts itself and says so every five seconds —
 * TICKS PER SECOND as well as microseconds per refresh, because "the refresh
 * is slow" and "the refresh runs far too often" are different faults that look
 * identical from a chair. */
static uint64_t tmd_ns, tmd_since;
static uint32_t tmd_rows_lo = 0xFFFFu, tmd_rows_hi;
static uint32_t tmd_ticks;

static void tm_tick(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    uint64_t t0 = timer_now_ns();
    tm_refresh(win);                     /* ~1 Hz auto-refresh */
    tmd_ns += timer_now_ns() - t0;
    tmd_ticks++;
    /* §M76.2 — the ROW COUNT per refresh.  Reported from use: *"the lower part
     * of the table changes for a moment and then goes back."*  If short-lived
     * tasks are being caught by the refresh, the count oscillates; if it is
     * steady, the flicker is something else entirely.  One number tells the
     * two apart, and guessing between them is what §M69 paid for twice. */
    if (tm) {
        if ((uint32_t)tm->ndisp < tmd_rows_lo) tmd_rows_lo = (uint32_t)tm->ndisp;
        if ((uint32_t)tm->ndisp > tmd_rows_hi) tmd_rows_hi = (uint32_t)tm->ndisp;
    }
    /* Gated on `gui.stats_ms`, the switch that already means "tell me what the
     * GUI costs" — one key rather than a second one nobody would find, and it
     * keeps an ordinary log free of a line every five seconds. */
    if (!tmd_since) tmd_since = t0;
    else if (config_get_long("gui.stats_ms", 0) > 0 &&
             timer_now_ns() - tmd_since > 5000000000ull) {
        uint64_t span_ms = (timer_now_ns() - tmd_since) / 1000000ull;
        kprintf("taskman: %u refreshes in %u ms (%u/s), %u us mean, %u %% of one cpu"
                ", rows %u..%u, iv pane=%u (colmove %u) cells=%u rows=%u\n",
                tmd_ticks, (unsigned)span_ms,
                (unsigned)(tmd_ticks * 1000u / (span_ms ? span_ms : 1)),
                (unsigned)(tmd_ns / 1000ull / (tmd_ticks ? tmd_ticks : 1)),
                (unsigned)(tmd_ns / 10000ull / (span_ms ? span_ms : 1)),
                (unsigned)tmd_rows_lo, (unsigned)tmd_rows_hi,
                iv_stat_pane, iv_stat_colmove, iv_stat_cells, iv_stat_rows);
        iv_stat_pane = iv_stat_colmove = iv_stat_cells = iv_stat_rows = 0;
        tmd_rows_lo = 0xFFFFu; tmd_rows_hi = 0;
        tmd_since = timer_now_ns(); tmd_ns = 0; tmd_ticks = 0;
    }

    /* §M75 — EACH CHART DAMAGES ONLY ITSELF.  A scrolling chart changes every
     * pixel it owns once a second, so four of them are four small rects; the
     * one thing they must never do is mark the WINDOW.  §M69 spent a milestone
     * removing exactly that cost (an unconditional full-window repaint two or
     * three times a second, on top of every carefully damaged row), and four
     * 1 Hz charts are the most natural way to put it straight back.  gui.h's
     * contract since then is that a tick damages what it changed — this is
     * that contract, four times. */
    if (tm) for (int i = 0; i < SYSMON_NSERIES; i++)
        if (tm->charts[i]) w_chart_refresh(tm->charts[i]);
}

/* §M81 — COMPOSED.  The last hand-placed window with real structure, and the
 * one that needed two capabilities the layout engine did not have.
 *
 * What it replaces is a function of pure arithmetic: `status_y = ch - cp_fh()
 * - 6`, `btn_y = status_y - BTN_H - 4`, `charts_y = btn_y - 6 - CHART_H`, a
 * chart strip whose widths were `(cw - gap * (N + 1)) / N`, and a table sized
 * from whichever of those survived.  Each number encoded a decision the spec
 * below states instead.
 *
 * THE TWO NEW FLAGS ARE BOTH HERE, and both came from this window:
 *
 *   UI_EDGE       the table and the footer are PANES.  The design's table is
 *                 not a box floating in a margin, and the footer is a
 *                 full-width `tray` strip with a rule above it — a margin turns
 *                 that into a floating bar.
 *   UI_DROP_TIGHT the chart strip goes entirely when the window is too short,
 *                 rather than being squeezed: *a chart below some height is a
 *                 box with a header and no room for a line, which reads as a
 *                 broken control, while its absence reads as a small window.*
 *                 The old code did this by hand and parked the charts at
 *                 x = -10000, because "this toolkit has no visibility flag".
 *                 Now it has one that means what it says.
 *
 * The widgets are re-fetched after every build: `ui_build` REBUILDS (§M81), so
 * the pointers the tick and the refresh use are only valid until the next
 * layout.  That is the same contract every composed panel here follows. */
enum {
    TM_ID_TABLE = 1, TM_ID_CHARTS, TM_ID_ROW, TM_ID_END, TM_ID_FKILL, TM_ID_PAUSE,
    TM_ID_EVICT,
    TM_ID_STATUS, TM_ID_CHART0,     /* …CHART0 + SYSMON_NSERIES - 1 */
};

static void tm_ui_event(struct gui_window* win, int id, int type, int value,
                        void* ctx) {
    (void)win; (void)value;
    struct taskman* tm = (struct taskman*)ctx;
    if (type != UI_EV_CLICK) return;
    if (id == TM_ID_END)   tm_kill(NULL, tm);
    if (id == TM_ID_FKILL) tm_kill_force(NULL, tm);
    if (id == TM_ID_PAUSE) tm_pause(NULL, tm);
    if (id == TM_ID_EVICT) tm_evict();
}

static void tm_layout(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    if (!tm) return;

    struct ui_spec spec[10 + SYSMON_NSERIES];
    int n = 0;
    spec[n++] = (struct ui_spec){ .id = TM_ID_TABLE, .cls = "view",
        .text = "table", .weight = 1, .flags = UI_EDGE | UI_FOCUSABLE };
    spec[n++] = (struct ui_spec){ .id = TM_ID_CHARTS, .cls = "box",
        .flags = UI_ROW | UI_FILL_W | UI_DROP_TIGHT };
    for (int i = 0; i < SYSMON_NSERIES; i++)
        spec[n++] = (struct ui_spec){ .id = TM_ID_CHART0 + i,
            .parent = TM_ID_CHARTS, .cls = "chart", .value = i,
            .weight = 1, .flags = UI_FILL_W };
    spec[n++] = (struct ui_spec){ .id = TM_ID_ROW, .cls = "box",
        .flags = UI_ROW | UI_FILL_W };
    spec[n++] = (struct ui_spec){ .id = TM_ID_END, .parent = TM_ID_ROW,
        .cls = "button", .text = "End task" };
    spec[n++] = (struct ui_spec){ .id = TM_ID_PAUSE, .parent = TM_ID_ROW,
        .cls = "button", .text = "Pause / Resume" };
    spec[n++] = (struct ui_spec){ .id = TM_ID_EVICT, .parent = TM_ID_ROW,
        .cls = "button", .text = "Evict" };
    spec[n++] = (struct ui_spec){ .id = TM_ID_FKILL, .parent = TM_ID_ROW,
        .cls = "button", .text = "Force kill" };
    spec[n++] = (struct ui_spec){ .id = TM_ID_STATUS, .cls = "label",
        .flags = UI_EDGE };

    ui_build(win, spec, n, tm_ui_event, tm);

    tm->iv        = (struct w_itemview*)ui_by_id(win, TM_ID_TABLE);
    tm->btn_end   = (struct w_button*)  ui_by_id(win, TM_ID_END);
    tm->btn_fkill = (struct w_button*)  ui_by_id(win, TM_ID_FKILL);
    tm->status    = (struct w_label*)   ui_by_id(win, TM_ID_STATUS);
    for (int i = 0; i < SYSMON_NSERIES; i++)
        tm->charts[i] = ui_by_id(win, TM_ID_CHART0 + i);

    if (tm->iv) {
        w_itemview_set_model(&tm->iv->base, &tm->model, tm);
        tm->iv->sel = -1;
    }
    if (tm->status) w_label_set_caption(tm->status, 2);   /* 2 = footer */
    /* The action this window exists for carries the accent; the escalation
     * beside it stays a plate.  The catalogue's dialog does exactly this with
     * "Formázás" (primary) next to "Mégse" (secondary). */
    if (tm->btn_end) w_button_set_emphasis(tm->btn_end, CP_BTN_PRIMARY);

    tm_refresh(win);
}

static void taskman_open(void) {
    struct taskman* tm = (struct taskman*)kcalloc(1, sizeof(*tm));
    if (!tm) return;

    struct gui_app_spec spec = {
        .title = "Task Manager",
        /* Tall enough for TEN rows at the density's row height, plus the header
         * band, the button row, the footer and the title bar.  The old height
         * was derived from the font and showed four rows once a row became a
         * design-sized 32 px. */
        .content_w = 58 * cp_fw() + 20,
        .content_h = 10 * cp_row_h() + 3 * cp_current_density()->control_h
                         + 3 * cp_fh() + 24,
        .layout = tm_layout, .ctx = tm, .slot = &tm_win,
    };
    struct gui_window* win = gui_app_open(&spec);
    if (!win) { kfree(tm); return; }

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

    /* §M81 — THE WIDGETS ARE THE LAYOUT'S NOW, not this function's.
     *
     * They used to be built here and merely REPOSITIONED by `tm_layout`, which
     * is the other thing `on_layout` means in this tree — and it is why the old
     * layout had to write `base.x`/`base.w` into six widgets by hand.  A
     * composed window builds in its layout hook, so a resize rebuilds and the
     * spec is the only description of the window that exists.
     *
     * Nothing is checked for NULL here any more either: a missing widget used
     * to close the window, and with the toolkit building them the honest
     * failure is `ui_build` reporting the class it could not find. */
    gui_window_set_tick(win, tm_tick);
    tm_layout(win);
    tm_refresh(win);
}

GUI_APP_ESSENTIAL("Task Manager", taskman_open, ICON_CHART);
