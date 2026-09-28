/* =============================================================================
 * gui_mode.c — changing the screen resolution while the desktop runs
 * (§M70; §M61).
 *
 * Extracted from gui.c.  The mode set itself is ONE call behind the
 * `fb_present.h` seam; everything here is the work ABOVE it — backbuffer,
 * wallpaper, panel strip, chrome layout, window clamping, and telling
 * client-managed windows their canvas moved.
 *
 * TWO RULES, both about not ending at a black screen: map the new frame BEFORE
 * switching, and READ THE GEOMETRY BACK — the device CLAMPS what it cannot do,
 * so believing the write leaves the kernel drawing at a size the display is not
 * showing.
 *
 * THE CONFIRM-OR-REVERT TIMER IS NOT OPTIONAL, and it is why this is more than
 * a wrapper around fb_mode_set: a mode the display cannot show is a black
 * screen, and nobody clicks "revert" on one.  So the change is applied, a
 * dialog opens IN THE NEW MODE with a ktimer countdown, and the NO-INPUT
 * outcome is the safe one.  Never a frame counter — a mode that shows nothing
 * produces no frames.
 * =========================================================================== */

#include "gui_priv.h"
#include "gui.h"
#include "gui_internal.h"
#include "gfx.h"
#include "fb_present.h"
#include "console_plate.h"
#include "wallpaper.h"
#include "widget.h"
#include "task.h"
#include "timer.h"
#include "printf.h"
#include "kmalloc.h"
#include "pmm.h"
#include "config.h"
#include "lock.h"
#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
 * §M61 — CHANGING THE RESOLUTION WHILE THE DESKTOP RUNS.
 *
 * The mode set itself is one call into the display backend.  The WORK is
 * everything above it: the backbuffer, the wallpaper and the panel are all
 * sized from the old screen, the shell's chrome layout was computed once, and
 * every window's position may now be off-screen.
 *
 * It runs on the COMPOSITOR TASK, between frames.  A mode set while compose()
 * is mid-blit writes into a buffer that is about to be freed, so the request is
 * queued and applied here — the same shape as every other structural change in
 * this file (apply_pending).
 * ========================================================================== */

static volatile int mode_req_w = 0, mode_req_h = 0;
/* §M61 — told AFTER the new mode is live, on the compositor task.  The confirm
 * dialog has to be created here and not by the requester: it must be centred on
 * the NEW screen (the requester still sees the old size, because the change is
 * queued) and it must be built on the task that owns the window machinery. */
static void (*mode_applied_cb)(int w, int h) = NULL;

/* Geometry saved before a mode change, so a REVERT restores the desktop and
 * not merely the resolution: windows clamped into a small screen must not stay
 * clamped when the big one comes back. */
struct saved_geom { int used, x, y, w, h; };
static struct saved_geom mode_saved[GUI_MAX_WINDOWS];
static int  mode_prev_w = 0, mode_prev_h = 0;
static int  mode_pending_confirm = 0;

void gui_set_mode_applied_cb(void (*fn)(int w, int h)) { mode_applied_cb = fn; }

int gui_request_mode(int w, int h) {
    if (!gui_active) return -1;
    if (w < 320 || h < 200) return -2;
    mode_req_w = w; mode_req_h = h;
    need_frame = 1;
    return 0;
}

int gui_current_mode(int* w, int* h) {
    if (!gui_active) return -1;
    if (w) *w = fbsurf.w;
    if (h) *h = fbsurf.h;
    return 0;
}

/* Re-establish every screen-sized thing after the display changed size. */
static int mode_rebuild_surfaces(void) {
    struct gfx_surface newfb;
    if (gfx_fb_surface(&newfb) != 0) return -1;

    /* Allocate the new buffers BEFORE freeing the old ones: an OOM must leave a
     * working desktop, not a compositor with no backbuffer. */
    struct gfx_surface nback, nwall;
    if (gfx_surface_init(&nback, newfb.w, newfb.h) != 0) return -2;
    if (gfx_surface_init(&nwall, newfb.w, newfb.h) != 0) {
        gfx_surface_free(&nback);
        return -3;
    }

    gfx_surface_free(&backsurf);
    gfx_surface_free(&wallsurf);
    fbsurf   = newfb;
    scanout  = newfb;                   /* §M88 — one output: the same surface */
    scanout.owns_px = 0;
    backsurf = nback;
    wallsurf = nwall;
    flip_ok  = 0;                       /* the flip belonged to the old size */

    /* The page flip's second buffer is derived from the geometry, so it has to
     * be re-established — and if it cannot be, the single-buffer path is still
     * correct (it only shears). */
    {
        volatile uint32_t* b0; volatile uint32_t* b1;
        if (fb_flip_init(&b0, &b1) == 0) {
            for (int i = 0; i < 2; i++) { flipbuf[i] = fbsurf; flipbuf[i].owns_px = 0; }
            flipbuf[0].px = (uint32_t*)(uintptr_t)b0;
            flipbuf[1].px = (uint32_t*)(uintptr_t)b1;
            flip_front = 0;
            flip_ok = 1;
        }
    }

    /* Chrome: the shell recomputes its layout from the new size. */
    if (shell && shell->init) shell->init(fbsurf.w, fbsurf.h);
    work_h = fbsurf.h -
             ((shell && shell->bottom_reserve) ? shell->bottom_reserve() : 0);
    gmax_cols = fbsurf.w / cp_cell_w();
    gmax_rows = fbsurf.h / cp_cell_h();

    /* The panel strip is screen-addressed and screen-wide. */
    {
        int reserve  = fbsurf.h - work_h;
        int strip_h  = reserve + PANEL_POPUP_MAX;
        if (strip_h > fbsurf.h) strip_h = fbsurf.h;
        uint32_t* nbuf = (uint32_t*)kmalloc((size_t)fbsurf.w * strip_h * 4);
        if (nbuf) {
            spin_lock(&panel_lock);
            uint32_t* old = panel_buf;
            panel_buf = nbuf;
            panel_strip_top = fbsurf.h - strip_h;
            panelsurf.w = fbsurf.w;
            panelsurf.h = fbsurf.h;
            panelsurf.stride = fbsurf.w;
            panelsurf.px = panel_buf - (size_t)panel_strip_top * fbsurf.w;
            panelsurf.owns_px = 0;
            gfx_set_clip(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h);
            gfx_fill(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h, COL_WALL_BOT);
            panel_ready = 1;
            spin_unlock(&panel_lock);
            if (old) kfree(old);
        }
    }

    paint_wallpaper();
    return 0;
}

/* Clamp every window into the new screen.  A window at x=1700 on a 1024-wide
 * display is unreachable — and unreachable is indistinguishable from lost. */
static void mode_clamp_windows(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* w = &windows[i];
        if (!w->used) continue;
        if (w->w > fbsurf.w) w->w = fbsurf.w;
        if (w->h > work_h)   w->h = work_h;
        if (w->x + w->w > fbsurf.w) w->x = fbsurf.w - w->w;
        if (w->y + w->h > work_h)   w->y = work_h - w->h;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        /* A client-managed window must be TOLD, or it keeps painting at the old
         * size — §4.60 built exactly this notification for the resize grip, and
         * a mode change is the same event from a different cause. */
        if (w->kind == WIN_APP && w->client_pid) {
            w->pending_w = w->w;
            w->pending_h = w->h;
        }
    }
}

void apply_mode_change(void) {
    int rw = mode_req_w, rh = mode_req_h;
    if (!rw || !rh) return;
    mode_req_w = mode_req_h = 0;
    /* §M88 — REFUSED, and said so, while more than one monitor is in use: the
     * rebuild below re-derives the desktop from the primary alone and would
     * drop the second monitor out of it.  Per-output mode setting is PLAN
     * §M88's later rung. */
    if (multi_out) {
        kprintf("gui: resolution changes with more than one monitor are not "
                "supported yet - unchanged\n");
        return;
    }

    int prev_w = fbsurf.w, prev_h = fbsurf.h;
    if (fb_mode_set((uint32_t)rw, (uint32_t)rh, 32) != 0) {
        kprintf("gui: display refused %dx%d - unchanged\n", rw, rh);
        return;
    }

    /* Save the geometry BEFORE clamping, so a revert restores the desktop and
     * not just the resolution.
     *
     * The guard is "a confirm is pending AND nothing is saved yet".  It read
     * `!mode_pending_confirm` at first — the exact inverse — so the one case
     * that needs the snapshot (a provisional change, about to be confirmed or
     * reverted) was the one case that never took it, `mode_prev_w` stayed 0 and
     * `gui_mode_revert` returned immediately.  The dialog counted down, said
     * the right things, and reverted nothing. */
    if (mode_pending_confirm && !mode_prev_w) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            mode_saved[i].used = windows[i].used;
            mode_saved[i].x = windows[i].x; mode_saved[i].y = windows[i].y;
            mode_saved[i].w = windows[i].w; mode_saved[i].h = windows[i].h;
        }
        mode_prev_w = prev_w; mode_prev_h = prev_h;
    }

    if (mode_rebuild_surfaces() != 0) {
        kprintf("gui: out of memory resizing to %dx%d - reverting\n", rw, rh);
        fb_mode_set((uint32_t)prev_w, (uint32_t)prev_h, 32);
        mode_rebuild_surfaces();
        return;
    }
    mode_clamp_windows();
    gui_damage_all();
    kprintf("gui: mode %dx%d\n", fbsurf.w, fbsurf.h);
    if (mode_pending_confirm && mode_applied_cb)
        mode_applied_cb(fbsurf.w, fbsurf.h);
}

/* Restore the mode + window geometry saved before the last change.
 *
 * QUEUED, for the same reason the change itself is: it reallocates the
 * backbuffer, and doing that from the dialog's app-host task while the
 * compositor is mid-compose frees the buffer out from under it. */
static volatile int mode_revert_req = 0;

void gui_mode_revert(void) {
    if (!gui_active || !mode_prev_w) return;
    mode_revert_req = 1;
    need_frame = 1;
}

void apply_mode_revert(void) {
    if (!mode_revert_req) return;
    mode_revert_req = 0;
    if (!mode_prev_w) return;
    if (fb_mode_set((uint32_t)mode_prev_w, (uint32_t)mode_prev_h, 32) != 0) return;
    mode_rebuild_surfaces();
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!mode_saved[i].used || !windows[i].used) continue;
        windows[i].x = mode_saved[i].x; windows[i].y = mode_saved[i].y;
        windows[i].w = mode_saved[i].w; windows[i].h = mode_saved[i].h;
        if (windows[i].kind == WIN_APP && windows[i].client_pid) {
            windows[i].pending_w = windows[i].w;
            windows[i].pending_h = windows[i].h;
        }
    }
    mode_pending_confirm = 0;
    mode_prev_w = mode_prev_h = 0;
    gui_damage_all();
    kprintf("gui: reverted to %dx%d\n", fbsurf.w, fbsurf.h);
}

void gui_mode_confirm(void) { mode_pending_confirm = 0; mode_prev_w = mode_prev_h = 0; }
void gui_mode_arm_confirm(void) { mode_pending_confirm = 1; }
