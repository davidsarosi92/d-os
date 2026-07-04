/* =============================================================================
 * apps/basic.c — Tiny-BASIC window (M22.5, PLAN §M22.5 stage 5).
 *
 * The GUI face of kernel/core/basic.c: a TERMINAL window whose hosted
 * task runs the interpreter REPL instead of a shell
 * (gui_window_create_task).  Because it's a terminal window, the
 * whole gterm plumbing is reused: kprintf output renders into the
 * window, keystrokes reach the interpreter via the offscreen VC, and
 * the window's X button kills the interpreter task under the kthread
 * contract (basic_run polls task_should_stop, INPUT/REPL block in
 * vc_getchar — both standard kill points).  The Task Manager can stop
 * a runaway program the same way.
 *
 * SINGLETON, one static `struct basic` instance (~22 KiB — too big
 * for a 4 KiB task stack, and a static slot makes the launch → task
 * parameter handoff trivially race-free: the pending-path slot below
 * is only written while no BASIC window exists).
 *
 * File association: .bas double-clicked in the file manager arrives
 * at basic_open_path → the freshly spawned task LOADs + RUNs it, then
 * drops to the REPL prompt.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "basic.h"
#include "task.h"
#include "vc.h"
#include "printf.h"
#include <stddef.h>

static struct gui_window* bas_win = NULL;       /* singleton */
static struct basic       bas_instance;         /* static: see header */
static char               bas_pending[128];     /* autoload path handoff */

static void basic_task(void) {
    /* Our VC is the window's offscreen VC, bound as out_console by
     * gui_window_create_task before we were first scheduled. */
    struct vc* vc = (struct vc*)(task_current() ? task_current()->out_console
                                                : NULL);
    if (!vc) return;

    basic_init(&bas_instance, vc);
    basic_repl(&bas_instance, bas_pending[0] ? bas_pending : NULL);
    bas_pending[0] = 0;

    /* REPL returned (BYE) — the window stays; tell the user why the
     * prompt died.  Closing the window reaps this task normally. */
    kprintf("[interpreter exited - close the window]\n");
}

/* destroy_window fires on_close for every window kind, so the
 * singleton pattern works for TERM windows exactly like for APP ones
 * (taskman/fileman). */
static void basic_on_close(struct gui_window* win) {
    (void)win;
    bas_win = NULL;
}

static void basic_open_with(const char* path) {
    if (bas_win) {
        /* Already open: raise it.  We can't safely poke a new program
         * into a possibly-running interpreter — the user LOADs it. */
        gui_window_raise(bas_win);
        if (path && *path)
            kprintf("basic: window already open - LOAD \"%s\" there\n", path);
        return;
    }

    int i = 0;
    for (; path && path[i] && i < (int)sizeof(bas_pending) - 1; i++)
        bas_pending[i] = path[i];
    bas_pending[i] = 0;

    bas_win = gui_window_create_task("BASIC", 260, 160, 560, 380,
                                     "basic", basic_task);
    if (bas_win) gui_window_set_on_close(bas_win, basic_on_close);
    else         bas_pending[0] = 0;
}

static void basic_launch(void)                { basic_open_with(NULL); }
static void basic_open_path(const char* path) { basic_open_with(path); }

GUI_APP_ASSOC("BASIC", basic_launch, basic_open_path, "bas");
