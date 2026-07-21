/* =============================================================================
 * dosgui.c — d-os display bridge for ring-3 graphical clients (§M42).
 *
 * See dosgui.h for the contract.  A client (NetSurf's libnsfb "dos" surface)
 * creates a WM-managed WIN_APP window, renders into its own buffer, and pushes
 * frames in with gui_window_blit — the same present path the §M26 Wayland
 * bridge uses.  Input is collected via the window's input hook into a small
 * per-window ring the client drains with dosgui_poll.
 *
 * Concurrency: the client runs on its own ring-3 task; the input hook fires
 * from the compositor's input router.  The ring is guarded by a spinlock so the
 * two never corrupt head/tail.
 * ============================================================================= */
#include "dosgui.h"
#include "gui.h"
#include "task.h"
#include "lock.h"
#include <stddef.h>

/* A handful of concurrent bridge windows is plenty (one per browser). */
#define DOSGUI_MAX      4
#define DOSGUI_EVQ      128

struct dosgui_win {
    int                used;
    struct gui_window* win;
    /* Input ring (single-producer compositor, single-consumer client). */
    struct dosgui_event evq[DOSGUI_EVQ];
    volatile int       head, tail;
    spinlock_t         lock;
};

static struct dosgui_win g_dg[DOSGUI_MAX];

/* Compositor-side input sink: translate a gui_input into a dosgui_event and
 * enqueue it (dropping the oldest if the ring is full). */
static void dosgui_input_cb(struct gui_window* w, const struct gui_input* in, void* ctx) {
    (void)w;
    struct dosgui_win* d = (struct dosgui_win*)ctx;
    if (!d || !in) return;
    spin_lock(&d->lock);
    int nt = (d->tail + 1) % DOSGUI_EVQ;
    if (nt != d->head) {                        /* space available */
        struct dosgui_event* e = &d->evq[d->tail];
        e->type    = (int32_t)in->type;
        e->keycode = (int32_t)in->keycode;
        e->pressed = (int32_t)in->pressed;
        e->x       = (int32_t)in->x;
        e->y       = (int32_t)in->y;
        d->tail = nt;
    }
    spin_unlock(&d->lock);
}

int dosgui_create(int w, int h, const char* title) {
    if (w <= 0 || h <= 0) return -1;
    int handle = -1;
    for (int i = 0; i < DOSGUI_MAX; i++) {
        if (!g_dg[i].used) { handle = i; break; }
    }
    if (handle < 0) return -1;

    struct dosgui_win* d = &g_dg[handle];
    /* A WIN_APP surface window (bare pixel surface, no hosted shell) — the same
     * kind the Wayland bridge targets.  Needs the compositor to be running. */
    d->win = gui_app_window_create(title ? title : "NetSurf",
                                   60, 40, w, h, NULL, NULL);
    if (!d->win) return -1;

    d->head = d->tail = 0;
    spin_lock_init(&d->lock);
    gui_window_set_input_hook(d->win, dosgui_input_cb, d);

    /* gui_app_window_create bound the window to THIS task (the ring-3 client
     * that issued the DOSGUI_CREATE syscall) as its host_task.  The compositor's
     * WIN_APP teardown (apply_pending) assumes a window's host_task is
     * reap_owned — it reads host_task->state after the task dies and then reaps
     * it, on the premise that init won't reap it out from under the compositor.
     * A dosgui client (e.g. NetSurf), however, is a DETACHED task (parent=init),
     * so WITHOUT this init's universal reaper races the compositor for the reap:
     * init frees/recycles the task struct, the compositor then reads a stale
     * host_task->state (never sees TASK_DEAD → the window leaks used=1) and/or
     * double-reaps a recycled pid → task-table corruption → the whole GUI wedges
     * on the NEXT open.  Claim the reap for the compositor (matching the pattern
     * dispatch_launches uses for in-kernel app-hosts) so it is the SOLE reaper. */
    struct task* client = task_current();
    if (client) task_set_reap_owned(client, 1);

    d->used = 1;
    return handle;
}

int dosgui_present(int handle, const uint32_t* px, int w, int h, int stride) {
    if (handle < 0 || handle >= DOSGUI_MAX || !g_dg[handle].used) return -1;
    if (!px || w <= 0 || h <= 0) return -1;
    /* px lives in the caller's (ring-3) address space, which is active during
     * the syscall — gui_window_blit reads it straight through. */
    gui_window_blit(g_dg[handle].win, 0, 0, px, w, h, stride);
    return 0;
}

int dosgui_poll(int handle, struct dosgui_event* out) {
    if (handle < 0 || handle >= DOSGUI_MAX || !g_dg[handle].used || !out) return -1;
    struct dosgui_win* d = &g_dg[handle];
    /* Title-bar X clicked?  Report a close event so the client quits itself;
     * once its task dies the compositor disposes the window (M22.7). */
    if (d->win && gui_window_want_close(d->win)) {
        out->type = 2; out->keycode = 0; out->pressed = 0; out->x = 0; out->y = 0;
        return 1;
    }
    int got = 0;
    spin_lock(&d->lock);
    if (d->head != d->tail) {
        *out = d->evq[d->head];
        d->head = (d->head + 1) % DOSGUI_EVQ;
        got = 1;
    }
    spin_unlock(&d->lock);
    return got;
}

void dosgui_destroy(int handle) {
    if (handle < 0 || handle >= DOSGUI_MAX || !g_dg[handle].used) return;
    struct dosgui_win* d = &g_dg[handle];
    if (d->win) gui_window_close(d->win);
    d->win = NULL;
    d->used = 0;
}
