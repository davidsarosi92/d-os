/* =============================================================================
 * shell_bare.c — minimal desktop shell: no chrome at all (M22.2).
 *
 * Exists to PROVE the desktop_shell interface swaps: `setconf
 * gui.shell bare` before running `gui` boots into wallpaper + windows
 * only — no taskbar, no menu, no clock.  Apps are started from a
 * terminal window with the `launch` shell command (registry walk).
 *
 * Also the template to copy for a new shell: every callback shows the
 * minimum contract.
 * ============================================================================= */

#include "desktop.h"
#include "gfx.h"

static int scr_h = 0;

static void bare_init(int w, int h) {
    (void)w;
    scr_h = h;
}

static int bare_bottom_reserve(void) { return 0; }   /* whole screen is ours */

static void bare_draw(struct gfx_surface* back) {
    /* One hint line so a user isn't stranded without a launcher. */
    gfx_text(back, 8, scr_h - GFX_GLYPH_H - 6,
             "bare shell - use 'launch <app>' in a terminal",
             0xFF7E93A8u);
}

static int  bare_click(int x, int y)  { (void)x; (void)y; return 0; }
static void bare_motion(int x, int y) { (void)x; (void)y; }
static int  bare_second_tick(void)    { return 0; }

DESKTOP_SHELL(bare) = {
    .name           = "bare",
    .init           = bare_init,
    .bottom_reserve = bare_bottom_reserve,
    .draw           = bare_draw,
    .click          = bare_click,
    .motion         = bare_motion,
    .second_tick    = bare_second_tick,
};
