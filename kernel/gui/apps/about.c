/* =============================================================================
 * about.c — "About d-os" (M22.2: moved out of gui.c into a self-registered
 * app; the compositor core no longer knows it exists).
 *
 * REWRITTEN 2026-09-28, found by the translation sweep.  The window still said
 * "M22.2: modular GUI" and "i386 / x86_64 - PLAN.md M22" — two years and three
 * architectures out of date — and placed four labels at literal pixel rows
 * (12/34/50/78) measured for the 8x8 font, so at a runtime face 16 px apart
 * they overlapped.  Now: the version is the SAME string the wallpaper draws
 * (version.h, so it cannot drift again), the labels are laid out by the
 * toolkit, and every sentence is a catalogue key.  The machine's own details
 * live on the Control Panel's "System information" page, which this points at
 * rather than repeating — two places listing the CPU would disagree the first
 * time one of them was edited.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "ui.h"
#include "console_plate.h"
#include "version.h"
#include <stddef.h>

static struct gui_window* about_win = NULL;

static void about_layout(struct gui_window* win) {
    if (ui_node_count(win) > 0) { ui_layout(win); return; }
    static const struct ui_spec spec[] = {
        { .id = 1, .cls = "label", .text = "d-os",                    .flags = UI_FILL_W },
        { .id = 2, .cls = "label", .text = DOS_LABEL,                  .flags = UI_FILL_W },
        { .id = 3, .cls = "label", .text = "about.tagline",            .flags = UI_FILL_W },
        { .id = 4, .cls = "label", .text = "about.more",               .flags = UI_FILL_W },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), NULL, NULL);
    struct w_label* v = (struct w_label*)ui_by_id(win, 2);
    if (v) v->role = WLBL_MUTED;           /* theme-following, not a captured colour */
    struct w_label* m = (struct w_label*)ui_by_id(win, 4);
    if (m) m->role = WLBL_MUTED;
}

static void about_open(void) {
    gui_app_open(&(struct gui_app_spec){
        .title = "About d-os",
        .content_w = cp_px(420), .content_h = cp_px(150),
        .layout = about_layout,
        .slot = &about_win,
    });
}

GUI_APP_ICON("About d-os", about_open, ICON_INFO);
