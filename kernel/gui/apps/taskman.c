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
#include "gui_app.h"
#include "widget.h"
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

struct taskman {
    struct w_listview* lv;
    struct w_label*    status;
    int row_pids[WLIST_MAX_ITEMS];
    struct tm_row rows[WLIST_MAX_ITEMS];   /* M27 — tree snapshot */
    int  nrows;
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

static void tm_emit(struct taskman* tm, struct tm_row* r, int depth, int selpid) {
    char line[WLIST_ITEM_LEN];
    int p = 0;
    p = put_u32_pad(line, p, sizeof line, (uint32_t)r->pid, 3);
    p = put_str(line, p, sizeof line, " ");
    p = put_str(line, p, sizeof line, state_short(r->st));
    p = put_u32_pad(line, p, sizeof line, r->cpu_ms, 7);
    p = put_str(line, p, sizeof line, "ms ");
    for (int d = 0; d < depth && d < 12; d++)          /* indent = tree depth */
        p = put_str(line, p, sizeof line, "  ");
    p = put_str(line, p, sizeof line, r->name);
    if (r->is_current) p = put_str(line, p, sizeof line, " *");
    line[p] = 0;

    if (tm->lv->count < WLIST_MAX_ITEMS) {
        tm->row_pids[tm->lv->count] = r->pid;
        int idx = w_listview_add(tm->lv, line, 0);
        if (idx >= 0 && r->pid == selpid) tm->lv->sel = idx;
    }
}

/* Emit `r` then, depth-first, its children.  The emitted flag guards
 * against ppid cycles, so recursion depth is bounded by the tree height
 * (a handful in practice) even though the array is walked at each level. */
static void tm_emit_subtree(struct taskman* tm, struct tm_row* r,
                            int depth, int selpid) {
    r->emitted = 1;
    tm_emit(tm, r, depth, selpid);
    for (int i = 0; i < tm->nrows; i++) {
        struct tm_row* c = &tm->rows[i];
        if (c->emitted || c->pid == r->pid) continue;
        if (c->ppid == r->pid) tm_emit_subtree(tm, c, depth + 1, selpid);
    }
}

static int tm_pid_present(struct taskman* tm, int pid) {
    for (int i = 0; i < tm->nrows; i++) if (tm->rows[i].pid == pid) return 1;
    return 0;
}

static void tm_refresh(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    if (!tm || !tm->lv) return;

    int selpid = -1;
    if (tm->lv->sel >= 0 && tm->lv->sel < tm->lv->count)
        selpid = tm->row_pids[tm->lv->sel];
    int scroll = tm->lv->scroll;         /* keep the viewport stable */

    tm->nrows = 0;
    task_for_each(tm_collect, tm);

    w_listview_clear(tm->lv);
    /* Roots first (parent not in the snapshot, or self-parent like pid 0),
     * each pulling its subtree; then any stragglers a cycle left behind. */
    for (int i = 0; i < tm->nrows; i++) {
        struct tm_row* r = &tm->rows[i];
        if (r->emitted) continue;
        if (r->pid == r->ppid || !tm_pid_present(tm, r->ppid))
            tm_emit_subtree(tm, r, 0, selpid);
    }
    for (int i = 0; i < tm->nrows; i++)
        if (!tm->rows[i].emitted) tm_emit_subtree(tm, &tm->rows[i], 0, selpid);

    if (scroll < tm->lv->count) tm->lv->scroll = scroll;
    /* M22.7 — a ~1 Hz refresh repaints ONLY the listview (its CPU-ms column
     * ticks constantly), not the whole window chrome (title, buttons, status
     * label don't change) — a smaller blit per second. */
    gui_window_request_redraw_rect(win, tm->lv->base.x, tm->lv->base.y,
                                   tm->lv->base.w, tm->lv->base.h);
}

/* ---- callbacks --------------------------------------------------------------- */

static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void tm_kill(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct taskman* tm = (struct taskman*)gui_window_ctx(tm_win);
    if (!tm) return;
    if (tm->lv->sel < 0 || tm->lv->sel >= tm->lv->count) {
        w_label_set(tm->status, "select a task first");
        return;
    }
    int pid = tm->row_pids[tm->lv->sel];
    struct task* t = task_find(pid);
    if (t && str_eq(t->name, "compositor")) {
        w_label_set(tm->status, "refusing: that would freeze the GUI");
        return;
    }
    if (task_kill(pid) == 0)
        w_label_set(tm->status, "flagged - dies at next yield");
    else
        w_label_set(tm->status, "not found or protected (pid 0 / idle)");
    tm_refresh(tm_win);
}

static void tm_tick(struct gui_window* win) {
    tm_refresh(win);                     /* ~1 Hz auto-refresh */
}

static void tm_layout(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    if (!tm || !tm->lv) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    tm->lv->base.x = 8;   tm->lv->base.y = 26;
    tm->lv->base.w = cw - 16;
    tm->lv->base.h = ch - 26 - 30;
    tm->status->base.x = 8;  tm->status->base.y = ch - 16;
    tm->status->base.w = cw - 16;
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
        gui_app_window_create("Task Manager", 300, 120, 460, 380, tm_layout, tm);
    if (!win) { kfree(tm); return; }
    tm_win = win;
    gui_window_set_on_close(win, tm_on_close);

    w_label_create(win, 8, 3, 300, "PID ST    CPU  NAME (tree)");
    w_button_create(win, 330, 2, 100, 18, "End task", tm_kill, tm);
    tm->lv     = w_listview_create(win, 8, 26, 440, 300, tm);
    tm->status = w_label_create(win, 8, 340, 440, "");
    if (!tm->lv || !tm->status) { gui_window_close(win); return; }
    tm->status->color = 0xFF8C9AAAu;

    gui_window_set_tick(win, tm_tick);
    tm_layout(win);
    tm_refresh(win);
}

GUI_APP("Task Manager", taskman_open);
