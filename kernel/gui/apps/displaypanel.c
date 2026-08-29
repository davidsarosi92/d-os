/* =============================================================================
 * displaypanel.c — the Display settings panel and the confirm-or-revert dialog
 * (§M61).
 *
 * This is the panel §M63's registry was built for: it declares itself with
 * SETTINGS_PANEL() and the Control Panel never mentions it.
 *
 * THE DIALOG IS THE POINT.  A resolution the display cannot show is a BLACK
 * SCREEN, and nobody can click "revert" on a black screen — so the new mode is
 * applied first, a dialog opens IN IT with a live countdown, and the safe
 * outcome is the one that requires no input:
 *
 *     OK       keep the mode (and write it to config)
 *     Cancel   revert immediately
 *     timeout  revert
 *
 * The countdown runs on a **ktimer** (§M53), never on a frame counter: at a
 * mode the display cannot show there may be no frames at all, and a revert that
 * only fires while the compositor is drawing is a revert that never happens in
 * the one case it exists for.
 *
 * `gui.mode_confirm_s` tunes the timeout (default 15 s); 0 disables the dialog
 * for someone who knows their monitor and is tired of it.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "itemview.h"
#include "settings.h"
#include "config.h"
#include "fb_present.h"
#include "ktimer.h"
#include "timer.h"      /* §M53 timer_now_ns — the countdown is a deadline */
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>

/* ---------------------------------------------------------------- */
/* The confirm-or-revert dialog.                                     */
/* ---------------------------------------------------------------- */

static struct gui_window* dlg_win = NULL;
static struct w_label*    dlg_count = NULL;
static struct ktimer      dlg_timer;
static volatile int       dlg_left = 0;      /* seconds remaining */
static volatile int       dlg_tick_pending = 0;

static void dlg_close(void) {
    if (dlg_win) { gui_window_close(dlg_win); dlg_win = NULL; }
    dlg_count = NULL;
    dlg_left = 0;
}

/* §M61 fix — the countdown is derived from a DEADLINE, not from a counter.
 *
 * Reported from use: *"the countdown is very slow, those aren't seconds, or it
 * stutters."*  Both symptoms came from the same shape: the number was a
 * variable decremented by a self-re-arming timer and displayed by a separate
 * ~2 Hz window tick.  A counter like that shows the number of TICKS THAT
 * HAPPENED, not the time that passed — every missed, doubled or late firing
 * lands directly in what the user reads, and under emulation those are normal.
 *
 * A deadline cannot drift: `remaining = deadline - now`, recomputed on every
 * repaint from the §M53 nanosecond clock.  The timer's only job now is to
 * WAKE the dialog often enough to repaint, so its period is a refresh rate and
 * no longer the unit of time being displayed.  (§M53 recorded the same lesson
 * one layer down: re-arm from the stored deadline, never from `now`.) */
static uint64_t dlg_deadline_ns = 0;

static void dlg_timer_fn(struct ktimer* t) {
    (void)t;
    dlg_tick_pending = 1;
    if (timer_now_ns() < dlg_deadline_ns)
        ktimer_arm_after(&dlg_timer, 200ull * 1000ull * 1000ull, dlg_timer_fn, NULL);
}

/* Whole seconds left, rounded UP so the display reaches 0 exactly when the
 * deadline does rather than a second early. */
static int dlg_seconds_left(void) {
    uint64_t now = timer_now_ns();
    if (now >= dlg_deadline_ns) return 0;
    uint64_t ns = dlg_deadline_ns - now;
    return (int)((ns + 999999999ull) / 1000000000ull);
}

/* Runs on the compositor task (window tick) — safe to draw and to revert. */
static void dlg_tick(struct gui_window* win) {
    if (!dlg_tick_pending) return;
    dlg_tick_pending = 0;
    dlg_left = dlg_seconds_left();

    if (dlg_left <= 0) {
        /* Nobody confirmed.  Revert FIRST, then drop the dialog: the window
         * belongs to the mode being undone. */
        gui_mode_revert();
        dlg_close();
        return;
    }
    if (dlg_count) {
        char msg[64];
        int n = 0;
        const char* a = "Keeping this resolution in ";
        for (int i = 0; a[i] && n < (int)sizeof msg - 8; i++) msg[n++] = a[i];
        int v = dlg_left;
        if (v >= 10) msg[n++] = (char)('0' + v / 10);
        msg[n++] = (char)('0' + v % 10);
        msg[n++] = ' '; msg[n++] = 's';
        msg[n] = 0;
        w_label_set(dlg_count, msg);
        gui_window_request_redraw(win);
    }
}

static void dlg_ok(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    ktimer_cancel(&dlg_timer);
    gui_mode_confirm();
    /* Persist only on OK: a mode that was never confirmed must not come back
     * at the next boot — that would make one bad choice permanent. */
    {
        int w = 0, h = 0;
        if (gui_current_mode(&w, &h) == 0) {
            char v[16];
            int n = 0;
            int d[8], k = 0;
            for (int x = w; x; x /= 10) d[k++] = x % 10;
            while (k) v[n++] = (char)('0' + d[--k]);
            v[n++] = 'x';
            for (int x = h; x; x /= 10) d[k++] = x % 10;
            while (k) v[n++] = (char)('0' + d[--k]);
            v[n] = 0;
            config_apply("gui.mode", v);
        }
    }
    dlg_close();
}

static void dlg_cancel(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    ktimer_cancel(&dlg_timer);
    gui_mode_revert();
    dlg_close();
}

/* Enter = OK, Esc = Cancel.  At a broken mode the pointer is as invisible as
 * everything else, so the keyboard has to work. */
static void dlg_key(struct gui_window* win, char c) {
    (void)win;
    if (c == '\n' || c == '\r') dlg_ok(NULL, NULL);
    else if (c == 27)           dlg_cancel(NULL, NULL);
}

static void dlg_layout(struct gui_window* win) {
    /* Builds too, and its label is cached in a static — clearing the list
     * without dropping that pointer would leave the countdown writing into a
     * freed widget. */
    gui_window_clear_widgets(win);
    dlg_count = NULL;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    w_label_create(win, 10, 8, cw - 20, "Keep this display mode?");
    dlg_count = w_label_create(win, 10, 26, cw - 20, "");
    w_button_create(win, cw - 150, ch - 34, 64, 24, "OK",     dlg_ok,     NULL);
    w_button_create(win, cw - 80,  ch - 34, 70, 24, "Cancel", dlg_cancel, NULL);
}

/* Opened FROM THE COMPOSITOR once the new mode is live (gui.c calls this
 * through the applied-callback).  Two reasons, both found the hard way: the
 * requester still sees the OLD size — the change is queued — so a dialog it
 * centres lands off to one side of the new screen; and the window machinery
 * wants to be driven from the task that owns it, which is where every other
 * app window is built. */
static int dlg_secs = 15;

static int dlg_sw = 0, dlg_sh = 0;

static void dlg_build(void) {
    if (dlg_win) return;
    int ow, oh;
    gui_window_outer_for_content(320, 96, &ow, &oh);
    dlg_win = gui_app_window_create("Display", (dlg_sw - ow) / 2, (dlg_sh - oh) / 3,
                                    ow, oh, dlg_layout, NULL);
    if (!dlg_win) {          /* no window → no way to confirm → revert now */
        gui_mode_revert();
        return;
    }
    gui_window_set_tick(dlg_win, dlg_tick);
    gui_window_set_key_hook(dlg_win, dlg_key);
    dlg_deadline_ns = timer_now_ns() + (uint64_t)dlg_secs * 1000000000ull;
    dlg_left = dlg_secs;
    dlg_tick_pending = 1;
    /* 200 ms: often enough that the number changes when the second does, and
     * cheap — it repaints two labels. */
    ktimer_arm_after(&dlg_timer, 200ull * 1000ull * 1000ull, dlg_timer_fn, NULL);
}

/* Called by the compositor once the new mode is live: hand the build to a
 * fresh app-host task (gui_queue_open) rather than doing it here. */
static void dlg_open_now(int sw, int sh) {
    dlg_sw = sw; dlg_sh = sh;
    gui_queue_open(dlg_build);
}

/* Public: apply a mode with the confirm-or-revert protocol.  `force` skips the
 * dialog — the shell offers it so the headless test can drive both the confirm
 * and the expire path; *a revert nothing can trigger on purpose is a revert
 * nobody has tested.* */
int display_set_mode(int w, int h, int force) {
    int cur_w = 0, cur_h = 0;
    gui_current_mode(&cur_w, &cur_h);
    if (w == cur_w && h == cur_h) return 0;

    long secs = config_get_long("gui.mode_confirm_s", 15);
    if (force || secs <= 0) {
        gui_mode_confirm();                 /* nothing to revert to */
        return gui_request_mode(w, h);
    }
    gui_mode_arm_confirm();
    dlg_secs = (int)secs;
    gui_set_mode_applied_cb(dlg_open_now);
    return gui_request_mode(w, h);
}

/* ---------------------------------------------------------------- */
/* `mode` — the shell view.  Shell command BEFORE panel, always: a     */
/* setting with no headless path cannot be regression-tested here      */
/* (§M63's rule), and this is the setting most able to leave a user    */
/* looking at nothing.                                                 */
/* ---------------------------------------------------------------- */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void display_cmd(const char* args) {
    while (args && *args == ' ') args++;

    if (!args || !*args || streq_(args, "list")) {
        struct fb_mode cur;
        if (fb_mode_current(&cur) == 0)
            kprintf("current: %ux%u@%u\n", cur.w, cur.h, cur.bpp);
        int n = fb_mode_count();
        if (n <= 1) {
            kprintf("this display cannot change resolution\n");
            return;
        }
        kprintf("available (%d):\n", n);
        for (int i = 0; i < n; i++) {
            struct fb_mode m;
            if (fb_mode_get(i, &m) == 0) kprintf("  %ux%u\n", m.w, m.h);
        }
        kprintf("usage: mode <w>x<h> [--force] | mode confirm | mode revert\n");
        return;
    }

    if (streq_(args, "confirm")) { gui_mode_confirm(); kprintf("mode: kept\n"); return; }
    if (streq_(args, "revert"))  { gui_mode_revert();  return; }

    /* Parse WxH, then an optional --force. */
    int w = 0, h = 0;
    const char* p = args;
    while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
    if (*p != 'x' && *p != 'X') { kprintf("mode: expected <w>x<h>\n"); return; }
    p++;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    while (*p == ' ') p++;
    int force = (*p == '-');

    if (!gui_is_active()) {
        kprintf("mode: the GUI is not running (start it with `gui`)\n");
        return;
    }
    int rc = display_set_mode(w, h, force);
    if (rc != 0) { kprintf("mode: refused (%d)\n", rc); return; }
    if (force) kprintf("mode: %dx%d (forced — no confirmation)\n", w, h);
    else       kprintf("mode: %dx%d — confirm within %ld s or it reverts "
                       "(`mode confirm` / `mode revert`)\n",
                       w, h, config_get_long("gui.mode_confirm_s", 15));
}

/* ---------------------------------------------------------------- */
/* The panel: the mode list, as an item view.                        */
/* ---------------------------------------------------------------- */

static int dp_count(void* ctx) { (void)ctx; return fb_mode_count(); }

static char dp_labels[16][16];

static int dp_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    struct fb_mode m;
    if (fb_mode_get(i, &m) != 0) return -1;
    if (i >= 16) return -1;
    /* Format "1280x800" by hand — the label has to persist past this call. */
    int n = 0, d[8], k = 0;
    for (int x = m.w; x; x /= 10) d[k++] = x % 10;
    while (k) dp_labels[i][n++] = (char)('0' + d[--k]);
    dp_labels[i][n++] = 'x';
    for (int x = m.h; x; x /= 10) d[k++] = x % 10;
    while (k) dp_labels[i][n++] = (char)('0' + d[--k]);
    dp_labels[i][n] = 0;

    int cw = 0, chh = 0;
    gui_current_mode(&cw, &chh);
    out->label = dp_labels[i];
    out->sub   = (m.w == cw && m.h == chh) ? "current" : NULL;
    out->icon  = ICON_DISPLAY;
    out->dim   = 0;
    return 0;
}

static void dp_activate(void* ctx, int i) {
    (void)ctx;
    struct fb_mode m;
    if (fb_mode_get(i, &m) != 0) return;
    display_set_mode(m.w, m.h, 0);
}

static const struct item_model dp_model = {
    .count = dp_count, .get = dp_get, .activate = dp_activate, .ctx = NULL,
};

static struct gui_window* dp_win = NULL;
static void dp_on_close(struct gui_window* w) { (void)w; dp_win = NULL; }

static void dp_layout(struct gui_window* win) {
    /* Builds its widgets — replace, do not stack (gui_window_clear_widgets). */
    gui_window_clear_widgets(win);
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    if (fb_mode_count() <= 1) {
        w_label_create(win, 8, 8, cw - 16,
                       "This display cannot change resolution.");
        w_label_create(win, 8, 26, cw - 16,
                       "(virtio-gpu: see fb_present.h)");
        return;
    }
    w_label_create(win, 8, 6, cw - 16, "Double-click a resolution");
    w_itemview_create(win, 8, 24, cw - 16, ch - 32, &dp_model,
                      config_get("controlpanel.view", "list"), NULL);
}

static void display_panel_open(void) {
    if (dp_win) { gui_window_raise(dp_win); return; }
    int ow, oh;
    gui_window_outer_for_content(360, 300, &ow, &oh);
    dp_win = gui_app_window_create("Display", 200, 140, ow, oh, dp_layout, NULL);
    if (dp_win) gui_window_set_on_close(dp_win, dp_on_close);
}

SETTINGS_PANEL(sp_display) = {
    .name    = "Display",
    .summary = "screen resolution",
    .icon    = ICON_DISPLAY,
    .open    = display_panel_open,
};

CONFIG_KEY(ck_mode_confirm) = {
    .key = "gui.mode_confirm_s", .group = "Display", .type = CFG_INT,
    .def = "15",
    .help = "seconds before an unconfirmed resolution reverts (0 = no dialog)",
};
CONFIG_KEY(ck_mode) = {
    .key = "gui.mode", .group = "Display", .type = CFG_STRING, .def = "",
    .help = "confirmed resolution, e.g. 1280x800 (written by the OK button)",
};
