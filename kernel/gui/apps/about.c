/* =============================================================================
 * about.c — tiny "About d-os" window (M22.2: moved out of gui.c into a
 * self-registered app; the compositor core no longer knows it exists).
 * Singleton via the on_close hook, same pattern as fileman.c.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "console_plate.h"
#include <stddef.h>

static struct gui_window* about_win = NULL;

static void about_open(void) {
    /* This window has no `on_layout`: its labels are created once, right here.
     * So the "already open" case has to RETURN rather than fall through — the
     * slot makes gui_app_open raise and hand back the same window, and building
     * a second set of labels on it is exactly the duplicate-widget bug §M70
     * found in the resize path.  Apps with a layout fn (every panel) do not
     * need this, because their widgets are rebuilt from scratch each time. */
    if (about_win) { gui_window_raise(about_win); return; }

    struct gui_window* w = gui_app_open(&(struct gui_app_spec){
        .title = "About d-os",
        .content_w = cp_px(300), .content_h = cp_px(150),
        .slot = &about_win,
    });
    if (!w) return;

    w_label_create(w, 16, 12, 260, "d-os - hobby teaching kernel");
    w_label_create(w, 16, 34, 260, "M22.2: modular GUI - swappable");
    w_label_create(w, 16, 50, 260, "desktop shells + app registry");
    struct w_label* d = w_label_create(w, 16, 78, 260,
                                       "i386 / x86_64 - PLAN.md M22");
    if (d) d->role = WLBL_MUTED;   /* theme-following, not a captured colour */
    gui_window_request_redraw(w);
}

GUI_APP_ICON("About d-os", about_open, ICON_INFO);
