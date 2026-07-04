/* =============================================================================
 * about.c — tiny "About d-os" window (M22.2: moved out of gui.c into a
 * self-registered app; the compositor core no longer knows it exists).
 * Singleton via the on_close hook, same pattern as fileman.c.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include <stddef.h>

static struct gui_window* about_win = NULL;

static void about_on_close(struct gui_window* win) {
    (void)win;
    about_win = NULL;
}

static void about_open(void) {
    if (about_win) { gui_window_raise(about_win); return; }

    struct gui_window* w =
        gui_app_window_create("About d-os", 490, 320, 300, 150, NULL, NULL);
    if (!w) return;
    about_win = w;
    gui_window_set_on_close(w, about_on_close);

    w_label_create(w, 16, 12, 260, "d-os — hobby teaching kernel");
    w_label_create(w, 16, 34, 260, "M22.2: modular GUI — swappable");
    w_label_create(w, 16, 50, 260, "desktop shells + app registry");
    struct w_label* d = w_label_create(w, 16, 78, 260,
                                       "i386 / x86_64 - PLAN.md M22");
    if (d) d->color = 0xFF8C9AAAu;
    gui_window_request_redraw(w);
}

GUI_APP("About d-os", about_open);
