/* =============================================================================
 * shell_greeter.c — the desktop shell of a machine NOBODY IS SIGNED IN TO
 * (§M82, 2026-09-27).
 *
 * Before this, signing out left a complete desktop running for nobody — the
 * taskbar, the Start menu with every program, the machine's desktop icons —
 * and put the sign-in window over it as a modal.  The modal kept a mouse from
 * reaching them; it did not make them go away, and "covered" is not "absent".
 * A sign-in screen should show what a person needs to sign in and nothing
 * that can be used before they have.
 *
 * So with no account in the session, gui_start picks THIS shell (pick_shell,
 * gui.c): wallpaper, a strip with the time, the sign-in window — and no
 * launcher, no icons, no tray controls.  The compositor also refuses to start
 * any program while it is up (the launch drain), because the other doors —
 * Ctrl+Alt+Del's Task Manager, a `launch` typed somewhere — would otherwise
 * open one as SYSTEM behind the sign-in window.
 *
 * LOCK AND SIGN-IN ARE THE SAME SURFACE IN TWO STATES: the lock window over a
 * running session (that session's shell underneath), or over this shell when
 * there is no session.  One window, one submit path, two backdrops.
 * ============================================================================= */

#include "desktop.h"
#include "gfx.h"
#include "console_plate.h"
#include "locale.h"
#include "rtc.h"

static int scr_w, scr_h;
static char g_clock[24];

static int greeter_strip_h(void) { return cp_taskbar_h(); }

static void greeter_init(int w, int h) { scr_w = w; scr_h = h; g_clock[0] = 0; }
static int  greeter_bottom_reserve(void) { return greeter_strip_h(); }

static void greeter_draw(struct gfx_surface* back) {
    const cp_theme* th = cp_current_theme();
    int sh = greeter_strip_h();
    int y = scr_h - sh;
    gfx_fill(back, 0, y, scr_w, sh, th->raised);
    gfx_fill(back, 0, y, scr_w, 1, th->line);
    int ty = y + (sh - cp_fh()) / 2;
    cp_text(back, cp_px(14), ty, lstr("greeter.title"), th->muted);
    if (g_clock[0]) {
        int len = 0;
        while (g_clock[len]) len++;
        cp_mono_text(back, scr_w - cp_px(14) - len * cp_mono_cell_w(), ty, g_clock, th->text);
    }
}

static int  greeter_click(int x, int y)  { (void)x; (void)y; return 1; }   /* nothing here */
static void greeter_motion(int x, int y) { (void)x; (void)y; }

static void put2(char* s, int* p, unsigned v) { s[(*p)++] = (char)('0' + v / 10 % 10); s[(*p)++] = (char)('0' + v % 10); }

static int greeter_second_tick(void) {
    struct rtc_time t;
    char s[24];
    int p = 0;
    if (rtc_read(&t) != 0) {
        const char* nc = lstr("tray.noclock");
        while (nc[p] && p < (int)sizeof s - 1) { s[p] = nc[p]; p++; }
    } else {
        put2(s, &p, t.hour); s[p++] = ':'; put2(s, &p, t.min);
    }
    s[p] = 0;
    for (int i = 0; i <= p; i++) {
        if (g_clock[i] != s[i]) {
            for (int j = 0; j <= p; j++) g_clock[j] = s[j];
            return 1;
        }
    }
    return 0;
}

DESKTOP_SHELL(greeter) = {
    .name           = "greeter",
    .init           = greeter_init,
    .bottom_reserve = greeter_bottom_reserve,
    .draw           = greeter_draw,
    .click          = greeter_click,
    .motion         = greeter_motion,
    .second_tick    = greeter_second_tick,
};
