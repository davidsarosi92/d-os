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
 * M22.4 — DEAD rows no longer accumulate: every refresh starts with an
 * opportunistic reap pass that task_reap()s DEAD tasks NOT bound to a
 * VC (a window/pane teardown still holds vc->task and must do its own
 * kill+reap; reaping those here would leave dangling pointers).  A
 * VC-bound DEAD task (e.g. a pane shell killed with End task) stays
 * listed — visible state is a feature in a teaching kernel.  Refreshes
 * are also event-driven now: the compositor fires on_tick immediately
 * on any task-set change (task_set_change_hook), so a closed program
 * drops off the list within one frame instead of at the next 1 Hz beat.
 *
 * All callbacks (tick, button, listview) run on the compositor task,
 * so widget state needs no extra locking.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include "task.h"
#include "vc.h"
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

struct taskman {
    struct w_listview* lv;
    struct w_label*    status;
    int row_pids[WLIST_MAX_ITEMS];
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

/* ---- refresh ---------------------------------------------------------------- */

struct tm_iter_ctx {
    struct taskman* tm;
    int selpid;                          /* re-select this pid after rebuild */
};

static void tm_iter(const struct task* t, int is_current, void* ctx) {
    struct tm_iter_ctx* c = (struct tm_iter_ctx*)ctx;
    struct w_listview* lv = c->tm->lv;
    if (lv->count >= WLIST_MAX_ITEMS) return;

    char line[WLIST_ITEM_LEN];
    int p = 0;
    p = put_u32_pad(line, p, sizeof line, (uint32_t)t->pid, 3);
    p = put_str(line, p, sizeof line, "  ");
    p = put_str(line, p, sizeof line, state_short(t->state));
    p = put_u32_pad(line, p, sizeof line, (uint32_t)t->cpu_ms, 8);
    p = put_str(line, p, sizeof line, "ms  ");
    p = put_str(line, p, sizeof line, t->name);
    if (is_current) p = put_str(line, p, sizeof line, " *");
    line[p] = 0;

    c->tm->row_pids[lv->count] = t->pid;
    int idx = w_listview_add(lv, line, 0);
    if (idx >= 0 && t->pid == c->selpid) lv->sel = idx;
}

/* M22.4 — opportunistic reap: collect DEAD pids first (task_for_each
 * holds the master lock, and task_reap takes it too — reaping inside
 * the iteration would self-deadlock), then reap the ones not owned by
 * a live window/pane.  task_reap itself refuses tasks still current on
 * some CPU, so a task mid-death is simply retried on the next refresh. */

struct tm_dead_scan {
    int pids[WLIST_MAX_ITEMS];
    int n;
};

static void tm_dead_iter(const struct task* t, int is_current, void* ctx) {
    struct tm_dead_scan* s = (struct tm_dead_scan*)ctx;
    if (is_current || t->state != TASK_DEAD) return;
    if (s->n < WLIST_MAX_ITEMS) s->pids[s->n++] = t->pid;
}

static void tm_reap_dead(void) {
    struct tm_dead_scan s = { .n = 0 };
    task_for_each(tm_dead_iter, &s);
    for (int i = 0; i < s.n; i++)
        if (!vc_task_bound(s.pids[i]))
            task_reap(s.pids[i]);
}

static void tm_refresh(struct gui_window* win) {
    struct taskman* tm = (struct taskman*)gui_window_ctx(win);
    if (!tm || !tm->lv) return;

    tm_reap_dead();

    struct tm_iter_ctx c = { tm, -1 };
    if (tm->lv->sel >= 0 && tm->lv->sel < tm->lv->count)
        c.selpid = tm->row_pids[tm->lv->sel];

    int scroll = tm->lv->scroll;         /* keep the viewport stable */
    w_listview_clear(tm->lv);
    task_for_each(tm_iter, &c);
    if (scroll < tm->lv->count) tm->lv->scroll = scroll;

    gui_window_request_redraw(win);
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

    w_label_create(win, 8, 3, 300, "PID  ST      CPU  NAME");
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
