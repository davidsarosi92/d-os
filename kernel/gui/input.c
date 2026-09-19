/* =============================================================================
 * input.c — the compositor's input router (§M70).
 *
 * Extracted from gui.c.  The rule the whole file is built on (M22.7): **THE
 * IRQ ONLY ENQUEUES.**  A mouse or keyboard interrupt records where the event
 * landed and which window it belongs to; the widget hit-test, the dispatch and
 * anything that allocates happen on the window's own app-host task.  A grid
 * re-render is thousands of glyph blits and `clipboard_set` allocates — do
 * either from the IRQ and a slow handler stalls the whole desktop.
 *
 * THE RINGS DO NOT DROP SILENTLY, and that was a real bug: both returned
 * quietly when full, and §M69 turned every motion packet into a hover, so a
 * trackpad burst filled the ring and DISCARDED THE CLICK BEHIND IT.  *A queue
 * that drops silently does not degrade under load; it becomes unpredictable,
 * which is much harder to recognise.*  Now a position REPLACES a queued
 * position, a full ring sacrifices the OLDEST hover rather than the newcomer,
 * and only when neither is possible is anything dropped — counted, and
 * announced once on the console so the next occurrence names itself.
 *
 * EVERY PRODUCER ASSIGNS A COMPLETE STRUCT.  `evq_push_ptr` and
 * `evq_push_wheel` once set seven of `struct gev`'s eight fields and left
 * `hover` as the slot's PREVIOUS tenant had it — and `dispatch_events` tests
 * that flag FIRST, so a press landing in a slot that last carried a hover
 * arrived as a pointer MOVE.  Not dropped, not counted, not visible in any
 * queue statistic: *it arrived wearing the wrong hat*, and whether it happened
 * depended only on what had occupied one slot of thirty-two — which is exactly
 * what "sometimes it works" looks like from a chair.
 *
 * A PRESS CLEARS ANY STALE GRAB.  A release can go missing for reasons this
 * file cannot prevent (a full queue, a window closing mid-gesture), and a
 * latch held forever makes the handler consume every later event while the
 * widget stays drawn in its held colour — "I click and nothing happens, then
 * suddenly it works."  *A recovery that waits for the thing that was lost is
 * not a recovery*, so a PRESS, which by definition starts a new gesture,
 * clears anything still held.
 * =========================================================================== */

#include "gui_priv.h"
#include "gui.h"
#include "gui_internal.h"
#include "gui_app.h"
#include "desktop.h"
#include "widget.h"
#include "ui.h"
#include "gfx.h"
#include "console_plate.h"
#include "clipboard.h"
#include "keymap.h"
#include "mouse.h"
#include "task.h"
#include "timer.h"
#include "printf.h"
#include "kmalloc.h"
#include "config.h"
#include "vc.h"
#include <stdint.h>
#include <stddef.h>

int evq_evict_hover(void);   /* used above its definition */

#define PEVQ_SZ 32
static struct pev        pevq[PEVQ_SZ];

static volatile uint32_t pevq_h = 0, pevq_t = 0;

struct gev {
    struct gui_window* win;
    int16_t x, y;                       /* content-relative              */
    uint8_t dbl;
    uint8_t btn, down;                  /* 0 = motion; else button + edge */
    uint8_t ptr;                        /* §M58: 0 = none, else WPTR_*+1  */
    uint8_t hover;                      /* 1 = pointer-over, no button     */
    int8_t  dz;                         /* §M61: wheel delta, 0 = none    */
};


#define EVQ_SZ 32
static struct gev        evq[EVQ_SZ];

static volatile uint32_t evq_h = 0, evq_t = 0;

#define KEYQ_SZ 32
static volatile char     keyq[KEYQ_SZ];

static volatile uint32_t keyq_h = 0, keyq_t = 0;

/* M22.5 — raw keycode queue (nav/editing keys + Ctrl shortcuts for the
 * focused APP window).  Entry: kc | mods << 8.  Same SPSC shape as
 * keyq: keyboard IRQ produces, compositor consumes. */
#define KCQ_SZ 32
static volatile uint16_t kcq[KCQ_SZ];

static volatile uint32_t kcq_h = 0, kcq_t = 0;

/* `btn` 0 = plain motion; otherwise the button index, with `down` saying
 * press or release. */
/* §M69 — COALESCE HOVERS IN THE GLOBAL RING TOO, and this is the one that
 * mattered.
 *
 * `aq` (per window) got this treatment first and the flood test still measured
 * ZERO drops — because the events were being discarded HERE, one ring earlier,
 * and never reached `aq` at all.  *An instrument placed on the wrong side of
 * the problem reports the problem as absent*, which is §4.61's lesson about
 * timing the wrong side of a blit, in a new place.
 *
 * A motion packet becomes a hover, a trackpad delivers them in bursts of
 * dozens, and this ring is 32 deep and shared by EVERY window.  `evq_push`
 * below — the one that carries a real button — then found it full and returned
 * silently.  Two queued hovers describe one pointer; the older one describes
 * where it is not any more, so the newer REPLACES it and the burst collapses
 * to one entry. */
volatile unsigned evq_dropped, evq_coalesced;

/* §M64 tail — a desktop drag in progress.  Set on a press that hit no window
 * and no chrome, cleared on release.  While set, motion and release go to the
 * DESKTOP whatever the pointer is over: a drag that ends at the first window
 * it crosses is not a drag (§M58's grab, one layer up).
 *
 * A flag and not a pointer: the desktop is not an object that can be
 * destroyed mid-drag, which is exactly why the widget grab needed to be one. */
static volatile int desk_grab = 0;

static unsigned btn_prev = 0;

/* Double-click tracking (IRQ only). */
static uint64_t lastclick_ms = 0;

static int lastclick_x = -100, lastclick_y = -100;

static struct gui_window* lastclick_win = NULL;

static uint64_t last_drag_frame_ms = 0;

static uint64_t drag_t0_ms, drag_compose0_ns;

static uint32_t drag_motions, drag_frames, drag_f0;

static uint64_t drag_px0;

/* §M58 — POINTER GRAB.  From a press on a widget that wants the pointer
 * stream until the release, motion goes to THAT widget even when the pointer
 * has left it (or the window).  Without a grab a selection stops at the
 * widget's edge, which is precisely where a user drags to.
 *
 * Written by the mouse IRQ under state_lock, read there too; the compositor
 * only ever sees the events it produces. */
static struct gui_window* grab_win = NULL;

volatile int sak_close_req = 0;  /* §M46 Ctrl+Alt+X — close/force top app */

static void evq_store(struct gev e) {
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t && evq_evict_hover()) n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) {
        if (evq_dropped++ == 0)
            kprintf("gui: INPUT EVENT DROPPED - the shared queue was full of "
                    "events that all mattered (see `gui stats`)\n");
        return;
    }
    evq[evq_h] = e;
    evq_h = n;
}
static void evq_push_hover(struct gui_window* w, int cx, int cy) {
    if (evq_h != evq_t) {
        uint32_t last = (evq_h + EVQ_SZ - 1) % EVQ_SZ;
        if (evq[last].hover && evq[last].win == w) {
            evq[last].x = (int16_t)cx;
            evq[last].y = (int16_t)cy;
            evq_coalesced++;
            return;
        }
    }
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) { evq_coalesced++; return; }   /* a stale hover, no loss */
    struct gev e = { .win = w, .x = (int16_t)cx, .y = (int16_t)cy, .hover = 1 };
    evq[evq_h] = e;
    evq_h = n;
}
/* A BUTTON IS NEVER DROPPED WHILE A HOVER COULD GO INSTEAD.  This returned
 * silently on a full ring, and the ring is full of positions — so a click
 * arriving mid-gesture simply vanished.  That is the whole of *"sometimes a
 * button click does nothing, as if it were a refresh problem"*, and of the
 * arrows and the thumb and the slider: one press or one release that never
 * happened. */
int evq_evict_hover(void) {
    for (uint32_t i = evq_t; i != evq_h; i = (i + 1) % EVQ_SZ) {
        if (!evq[i].hover) continue;
        for (uint32_t j = i; j != evq_t; j = (j + EVQ_SZ - 1) % EVQ_SZ)
            evq[j] = evq[(j + EVQ_SZ - 1) % EVQ_SZ];
        evq_t = (evq_t + 1) % EVQ_SZ;
        evq_coalesced++;
        return 1;
    }
    return 0;
}
static void evq_push(struct gui_window* w, int cx, int cy, int dbl,
                     int btn, int down) {
    struct gev e = { .win = w, .x = (int16_t)cx, .y = (int16_t)cy,
                     .dbl = (uint8_t)dbl, .btn = (uint8_t)btn,
                     .down = (uint8_t)down };
    evq_store(e);
}
/* §M58 — push a pointer PHASE event (press / drag / release) for a widget
 * window.  Separate from evq_push because the two carry different meanings
 * through the same ring and conflating them is how a click becomes a drag.
 *
 * THIS ONE CARRIES THE PRESS THAT OPERATES EVERY SCROLLBAR, SLIDER AND
 * SELECTION IN THE TREE, and it used to be the least careful of the three: it
 * left `hover` from the previous tenant of the slot (see evq_store) and it
 * returned SILENTLY on a full ring, where evq_push had been taught to sacrifice
 * a hover instead.  A phase event is never expendable — a lost press is a
 * control that ignored you, and a lost RELEASE is a grab held forever. */
static void evq_push_ptr(struct gui_window* w, int cx, int cy, int phase) {
    struct gev e = { .win = w, .x = (int16_t)cx, .y = (int16_t)cy,
                     .ptr = (uint8_t)(phase + 1) };
    evq_store(e);
}
/* §M61 follow-up — a wheel event for the window under the pointer. */
static void evq_push_wheel(struct gui_window* w, int cx, int cy, int dz) {
    struct gev e = { .win = w, .x = (int16_t)cx, .y = (int16_t)cy,
                     .dz = (int8_t)dz };
    evq_store(e);
}
void gui_wheel(int dz) {
    if (!gui_active || !dz) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    struct gui_window* win = topmost_at(mx, my);
    if (win && win->kind == WIN_APP && !win->minimized && my >= win->y + TITLE_H)
        evq_push_wheel(win, mx - win->x - BORDER, my - win->y - TITLE_H, dz);
    else if (win && win->kind == WIN_TERM && !win->minimized &&
             my >= win->y + TITLE_H) {
        /* §M58 — the wheel over a terminal moves its SCROLLBACK.  Three lines
         * per notch is the convention everywhere else and the reason is that a
         * notch is a coarse gesture: one line per notch makes reading a page of
         * history a wrist exercise.
         *
         * The IRQ only moves the offset; the re-render (thousands of glyph
         * blits) runs on the compositor through the same flag the selection
         * uses (§M22.7's split — the interrupt records, the compositor works). */
        if (gterm_view_scroll(win, dz > 0 ? 3 : -3)) term_sel_dirty = win;
    }
    spin_unlock_irqrestore(&state_lock, fl);
    need_frame = 1;
}
void pevq_push(uint8_t type, int x, int y) {
    uint32_t n = (pevq_h + 1) % PEVQ_SZ;
    if (n == pevq_t) return;
    pevq[pevq_h].type = type;
    pevq[pevq_h].x = (int16_t)x;
    pevq[pevq_h].y = (int16_t)y;
    pevq_h = n;
    need_frame = 1;
}
/* Is (x,y) over the shell's chrome — the taskbar strip or the open popup? */
static int in_panel_region(int x, int y) {
    if (y >= work_h) return 1;                  /* taskbar strip (bottom_reserve) */
    if (pnl_pop_on && x >= pnl_pop_x && x < pnl_pop_x + pnl_pop_w &&
        y >= pnl_pop_y && y < pnl_pop_y + pnl_pop_h) return 1;
    return 0;
}
/* §M46 Ctrl+Alt+X — close the top-most user app window.  A client-managed
 * (package) window's want_close makes apply_pending force-kill the client, so it
 * works even when the app is frozen; a normal app-host window gets a graceful
 * want_close.  Runs on the compositor task, so taking state_lock is safe. */
void sak_close_top_app(void) {
    struct gui_window* target = NULL;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    for (int i = zcount - 1; i >= 0; i--) {
        struct gui_window* w = zorder[i];
        if (w && w->used && w->kind == WIN_APP && !w->minimized) { target = w; break; }
    }
    if (target) target->want_close = 1;
    spin_unlock_irqrestore(&state_lock, fl);
    if (target) kprintf("gui: Ctrl+Alt+X - closing top app '%s'\n", target->title);
    else        kprintf("gui: Ctrl+Alt+X - no app window to close\n");
}
/* M22.7 — the compositor no longer touches widgets: it drains the IRQ-fed
 * global queues and re-routes each event into the target window's per-window
 * queue (aq).  The owning app-host does the widget hit-test + dispatch +
 * redraw off the compositor. */
void dispatch_events(void) {
    while (evq_t != evq_h) {
        struct gev e = evq[evq_t];
        evq_t = (evq_t + 1) % EVQ_SZ;
        struct gui_window* win = e.win;
        if (!win || !win->used || win->kind != WIN_APP) continue;
        struct app_event ae = { .type = e.hover ? AE_HOVER
                                     : (e.dz ? AE_SCROLL
                                     : (e.ptr ? AE_POINTER
                                              : (e.btn ? AE_BUTTON : AE_MOUSE))),
                                .x = e.x, .y = e.y, .dbl = e.dbl,
                                .btn = e.btn, .down = e.down,
                                .phase = (uint8_t)(e.dz ? (uint8_t)e.dz
                                                        : (e.ptr ? e.ptr - 1 : 0)) };
        aq_push(win, ae);
    }
}
void dispatch_keys(void) {
    while (keyq_t != keyq_h) {
        char c = keyq[keyq_t];
        keyq_t = (keyq_t + 1) % KEYQ_SZ;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        struct gui_window* win = focused_win;
        spin_unlock_irqrestore(&state_lock, fl);
        if (!win || !win->used || win->kind != WIN_APP) continue;
        struct app_event ae = { .type = AE_KEY, .c = c };
        aq_push(win, ae);
    }
}
/* M22.5 — raw keycode events to the focused widget (see widget.h). */
void dispatch_keycodes(void) {
    while (kcq_t != kcq_h) {
        uint16_t e = kcq[kcq_t];
        kcq_t = (kcq_t + 1) % KCQ_SZ;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        struct gui_window* win = focused_win;
        spin_unlock_irqrestore(&state_lock, fl);
        if (!win || !win->used || win->kind != WIN_APP) {
            /* §M64 tail — NOBODY ELSE WANTED IT, so the desktop gets it.
             * These keycodes used to be dropped here, which is why the icon
             * field could be reached only with a mouse.  Queued rather than
             * called: Enter activates a shortcut, and that spawns an app-host
             * task — the desktop task's job, not the compositor's. */
            pevq_push(PEV_DESK_KEY, (int)(e & 0xFF), (int)(e >> 8));
            continue;
        }
        struct app_event ae = { .type = AE_KEYCODE, .kc = (uint8_t)(e & 0xFF),
                                .mods = (uint8_t)(e >> 8) };
        aq_push(win, ae);
    }
}
/* §M40 — deliver queued input for a WIN_APP window that has an INPUT HOOK but
 * NO app-host task.
 *
 * A hook-backed window has no app-host that drains its queue: a Wayland window
 * is created by the server task, which then blocks reading its client's socket,
 * and a dosgui window (NetSurf) is client-managed with host_task cleared
 * outright.  Either way app_host_main never runs for it, so queued events
 * simply piled up and the hook was never called.  The §M26 demo hid this by
 * synthesising input directly, so it only surfaced once a REAL client asked for
 * a wl_seat.
 *
 * The compositor is the right owner: it is an ordinary task (the hook does a
 * socket send, which must not happen in the mouse IRQ) and a hook-backed window
 * has no widgets for anyone else to dispatch to. */
void pump_hostless_input(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used || win->kind != WIN_APP) continue;
        if (!win->input_hook) continue;
        while (win->aq_t != win->aq_h) {
            struct app_event e = win->aq[win->aq_t];
            win->aq_t = (win->aq_t + 1) % AQ_SZ;
            app_dispatch_event(win, &e);
        }
    }
}
/* Read the gate ONCE.  This runs on the mouse path, and a string lookup in the
 * config store per drag is a cost the feature is supposed to be measuring. */
static int drag_report_on(void) {
    static int cached = -1;
    if (cached < 0) cached = (int)config_get_long("gui.drag_stats", 0);
    return cached;
}
void gui_mouse(int dx, int dy, unsigned buttons) {
    /* M22.4 — per-motion drag damage bookkeeping (filled under the
     * lock, consumed after unlock so gui_damage isn't nested deeper
     * than it has to be). */
    int drag_moved = 0;
    /* Only the ORIGIN is still needed here: where the window came from, for
     * the move hint.  The rest of the old bookkeeping (size, destination)
     * moved into the hint itself when the drag stopped raising damage rects. */
    int drag_old_x = 0, drag_old_y = 0;
    /* M22.7 — precise structural damage: the windows whose look changed
     * (focus highlight, z-order raise, minimize) instead of the whole
     * screen.  Captured under the lock, damaged after unlock. */
    struct gui_window* clicked = NULL;
    int wm_changed = 0;                          /* the click raised/restored it */
    int force_full = 0;                          /* geometry change → full damage */

    spin_lock(&state_lock);
    struct gui_window* old_focus = focused_win;

    mx += dx;  my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= fbsurf.w) mx = fbsurf.w - 1;
    if (my >= fbsurf.h) my = fbsurf.h - 1;

    unsigned pressed  =  buttons & ~btn_prev;
    unsigned released = ~buttons &  btn_prev;
    btn_prev = buttons;

    /* §M65 — window popup hover.  A plain integer store in the IRQ; the
     * repaint is a damage request, not a draw (§M22.7's split).
     *
     * THE RECT IS THE POINT.  The first version set `need_frame` and nothing
     * else — and a frame with an EMPTY damage list repaints only the cursor's
     * own rectangle (§4.61: a pure glide is a bare wake).  So the highlight was
     * painted into the cursor's footprint and never anywhere else: dragging
     * down the menu left a trail of cursor-sized coloured patches instead of
     * moving one highlighted row.  Reported from use, and exactly the symptom
     * you would predict from "asked for a frame, claimed no area". */
    int popup_hover_moved = 0;
    if (popup.active) {
        int r = popup_row_at(mx, my);
        if (r != popup.hover) { popup.hover = r; popup_hover_moved = 1; }
    }

    /* Title-button hover.  Same shape as the popup's above: decide in the IRQ,
     * damage the two boxes that changed, let the compositor paint.  Damaging
     * only the buttons and not the title bar matters — a maximized window's
     * title bar is 1920 px wide, and repainting it on every mouse packet that
     * crosses it is the fill-rate wall §4.61 measured. */
    int tb_ndmg = 0;
    int tb_dmg[2][4];
    {
        struct gui_window* old_w = tb_hover_win;
        int old_i = tb_hover_idx;
        struct gui_window* hw = topmost_at(mx, my);
        int idx = hw ? title_btn_at_n(hw->x, hw->y, hw->w, mx, my,
                                      title_btn_count(hw)) : -1;
        struct gui_window* nw = (idx >= 0) ? hw : NULL;
        if (nw != old_w || idx != old_i) {
            /* Both rectangles are resolved HERE, under state_lock, because
             * gui_damage must be called outside it and a window pointer read
             * after the unlock may name a window that has since gone. */
            if (old_w && old_w->used && old_i >= 0) {
                int* r = tb_dmg[tb_ndmg++];
                title_btn_rect(old_w->x, old_w->y, old_w->w, old_i,
                               &r[0], &r[1], &r[2], &r[3]);
            }
            if (nw) {
                int* r = tb_dmg[tb_ndmg++];
                title_btn_rect(nw->x, nw->y, nw->w, idx, &r[0], &r[1], &r[2], &r[3]);
            }
            tb_hover_win = nw;
            tb_hover_idx = idx;
        }
    }

    /* M22.7-B — chrome hover (launcher highlight): only meaningful while the
     * popup is open; route it to the desktop task instead of running the
     * shell in the IRQ. */
    if (pnl_pop_on) pevq_push(PEV_MOTION, mx, my);

    /* §M40 — a window that forwards its input to a CLIENT (Wayland) wants the
     * whole pointer stream, not just clicks: `wl_pointer.motion` is how an
     * application tracks the cursor at all.  Widget windows deliberately get
     * motion only on click (a widget hit-test per mouse packet would be
     * pointless work), so this is gated on the hook rather than made general. */
    {
        struct gui_window* hw = topmost_at(mx, my);
        if (hw && hw->kind == WIN_APP && hw->input_hook && !hw->minimized)
            evq_push(hw, mx - hw->x - BORDER, my - hw->y - TITLE_H, 0, 0, 0);
        /* Widget windows: record the position for the host to resolve. */
        else if (hw && hw->kind == WIN_APP && hw->widgets && !hw->minimized &&
                 (dx || dy))
            evq_push_hover(hw, mx - hw->x - BORDER, my - hw->y - TITLE_H);
    }

    /* §M58 — motion while a TERMINAL selection is in progress: extend the range
     * and ask the compositor to re-render.  The comparison is what stops a
     * motionless drag from re-rendering the grid on every mouse packet. */
    if (term_sel_win && term_sel_win->used && (dx || dy)) {
        struct gui_window* tw = term_sel_win;
        int r, c;
        gterm_cell_at(tw, mx - tw->x - BORDER, my - tw->y - TITLE_H, &r, &c);
        if (r != tw->sel_br || c != tw->sel_bc || !tw->sel_on) {
            tw->sel_br = r; tw->sel_bc = c;
            tw->sel_on = (r != tw->sel_ar || c != tw->sel_ac);
            term_sel_dirty = tw;
        }
    }

    /* §M58 — motion while a widget holds the pointer grab.  Sent to the
     * GRABBING window regardless of what is under the cursor now: a selection
     * that stops at the widget's edge is not a selection. */
    if (grab_win && grab_win->used && !grab_win->minimized && (dx || dy))
        evq_push_ptr(grab_win, mx - grab_win->x - BORDER,
                     my - grab_win->y - TITLE_H, WPTR_DRAG);

    /* §M64 tail — motion while the DESKTOP holds the grab.  Queued rather than
     * acted on: moving an icon re-lays the background layer and the release
     * writes a file, and we are in the mouse IRQ under the WM lock. */
    if (desk_grab && (dx || dy)) pevq_push(PEV_DESK_DRAG, mx, my);

    /* §M58/§M59 — MIDDLE-CLICK PASTE of the primary selection into a terminal.
     * This is the entire reason the primary selection is a separate slot: you
     * select with the left button and paste with the middle one, without either
     * touching what you deliberately copied with Ctrl+C.  Recorded here, done
     * on the compositor (it reads the clipboard and pushes characters into a
     * VC — neither is IRQ work). */
    if (pressed & MOUSE_BTN_MIDDLE) {
        struct gui_window* pw = topmost_at(mx, my);
        if (pw && pw->kind == WIN_TERM && pw->vc && !pw->minimized &&
            my >= pw->y + TITLE_H)
            term_paste_win = pw;
    }

    /* Right press goes only to a client window, and only over its content —
     * the desktop chrome has no right-click behaviour to compete with. */
    if (pressed & MOUSE_BTN_RIGHT) {
        struct gui_window* rw = topmost_at(mx, my);
        if (rw && rw->kind == WIN_APP && rw->input_hook && !rw->minimized &&
            my >= rw->y + TITLE_H)
            evq_push(rw, mx - rw->x - BORDER, my - rw->y - TITLE_H, 0, 2, 1);
    }

    if (pressed & MOUSE_BTN_LEFT) {
        /* §M69 gate 2 — A MODAL DIALOG OWNS EVERY PRESS, and this has to run
         * BEFORE the chrome's first refusal below and before the desktop
         * fallthrough at the end.  Gate 1 is not enough on its own: a press
         * outside the modal makes `topmost_at` return NULL, and NULL is
         * precisely how this function recognises a click on the WALLPAPER —
         * so without this the taskbar would still open the Start menu and a
         * double-click would still launch a shortcut behind the dialog.
         *
         * Swallowed in silence, not beeped at or bounced: the backdrop is
         * already saying the rest of the screen is not available, and an
         * error for pressing a disabled thing is noise. */
        if (!modal_hit(mx, my)) goto drag_update;

        /* Desktop chrome gets first refusal.  A click over the taskbar or
         * the open popup is consumed and handed to the desktop task; a click
         * elsewhere while the popup is open also goes there (to dismiss the
         * menu) but still falls through to the windows below. */
        if (in_panel_region(mx, my)) {
            pevq_push(PEV_CLICK, mx, my);
            goto drag_update;
        }
        /* §M65 — AN OPEN POPUP OWNS THE NEXT CLICK, wherever it lands.  Inside
         * it is a choice; outside it is a dismissal — and in BOTH cases the
         * click must not also reach the window underneath, or dismissing a
         * menu would activate whatever happened to be behind it. */
        if (popup.active) {
            int row = popup_row_at(mx, my);
            struct gui_window* ow = popup.owner;
            int tag = popup.tag;
            popup.active = 0;
            popup.hover = -1;
            if (ow && ow->used) {
                struct app_event e = {0};
                e.type = AE_POPUP;
                e.x = (int16_t)row;         /* -1 = dismissed without a choice */
                e.y = (int16_t)tag;
                aq_push(ow, e);
            }
            spin_unlock(&state_lock);
            gui_damage_all();
            return;
        }

        if (pnl_pop_on) pevq_push(PEV_CLICK, mx, my);   /* dismiss, then windows */

        struct gui_window* win = topmost_at(mx, my);
        clicked = win;                          /* for precise structural damage */
        if (win) {
            wm_changed = gui_wm_focus_raise_locked(win);

            /* ONE hit test, shared with the painter (title_btn_rect) — these
             * three used to be open-coded here with different gaps and a
             * different row, so the pressable box was not the drawn box. */
            int tb = title_btn_at_n(win->x, win->y, win->w, mx, my,
                                    title_btn_count(win));
            int in_close = (tb == TB_CLOSE);
            int in_max   = (tb == TB_MAX);                   /* M22.5 */
            int in_min   = (tb == TB_MIN);
            if (in_close) {
                /* Second click on an already-requested close = "force it".
                 * Runs in the mouse IRQ, so this is a plain volatile store; the
                 * compositor acts on it (task_force_kill takes locks we must
                 * not take here). */
                if (win->want_close) win->close_force_now = 1;
                win->want_close = 1;
            } else if (in_max) {
                toggle_maximize_locked(win);                 /* M22.5 */
                force_full = 1;                              /* geometry change */
            } else if (in_min) {
                win->minimized = 1;
                struct gui_window* nf = top_visible_locked();
                focused_win = nf;
                if (nf && nf->kind == WIN_TERM) vc_focus(nf->vc);
            } else if (my < win->y + TITLE_H) {
                /* M22.5 — double-click on the title bar toggles
                 * maximize; a single click starts a drag (disabled
                 * while maximized). */
                uint64_t now = timer_ticks_ms();
                int dbl = (win == lastclick_win &&
                           now - lastclick_ms < 400 &&
                           mx - lastclick_x < 6 && lastclick_x - mx < 6 &&
                           my - lastclick_y < 6 && lastclick_y - my < 6);
                lastclick_ms = now;
                lastclick_x = mx; lastclick_y = my;
                lastclick_win = win;
                /* §M69 — a modal has no maximize BUTTON, so the double-click
                 * shortcut for it must go too: leaving the gesture would be a
                 * hidden way to reach a state the visible controls deny, which
                 * is the same defect as a menu item with no keyboard route,
                 * inverted. */
                if (dbl && title_btn_count(win) > TB_MAX) {
                    toggle_maximize_locked(win);
                    force_full = 1;                          /* geometry change */
                } else if (!win->maximized) {
                    drag     = DRAG_MOVE;
                    drag_win = win;
                    grab_dx  = mx - win->x;
                    grab_dy  = my - win->y;
                    drag_t0_ms = timer_ticks_ms();
                    drag_compose0_ns = total_compose_ns;
                    drag_px0 = total_blit_px;
                    drag_f0  = frames_full + frames_partial;
                    drag_motions = drag_frames = 0;
                    drag_fast = drag_slow = 0;
                }
            } else if (!win->maximized &&
                       mx >= win->x + win->w - GRIP &&
                       my >= win->y + win->h - GRIP) {
                drag     = DRAG_RESIZE;
                drag_win = win;
                rubber_w = win->w;
                rubber_h = win->h;
            } else if (win->kind == WIN_APP) {
                int cxr = mx - win->x - BORDER;
                int cyr = my - win->y - TITLE_H;
                uint64_t now = timer_ticks_ms();
                int dbl = (win == lastclick_win &&
                           now - lastclick_ms < 400 &&
                           mx - lastclick_x < 6 && lastclick_x - mx < 6 &&
                           my - lastclick_y < 6 && lastclick_y - my < 6);
                lastclick_ms = now;
                lastclick_x = mx; lastclick_y = my;
                lastclick_win = win;
                /* Motion first (so the client's pointer is where the click
                 * happened), then the press itself.  A widget window ignores
                 * the button event; a client window needs both. */
                    /* §M69 — the DISPATCH end of the click probe; the other is
                 * in ps2_mouse.c.  See there for why both are needed. */
                if (gui_input_debug())
                    kprintf("gui: press dispatched to '%s' at %d,%d%s\n",
                            win->title, cxr, cyr, dbl ? " (double)" : "");
                evq_push(win, cxr, cyr, dbl, 0, 0);
                evq_push(win, cxr, cyr, dbl, 1, 1);
                /* §M58 — and the phase stream, plus the grab that keeps it
                 * coming after the pointer leaves the widget. */
                evq_push_ptr(win, cxr, cyr, WPTR_PRESS);
                grab_win = win;
            } else if (win->kind == WIN_TERM && win->cells) {
                /* §M58 — TEXT SELECTION in a terminal window.  A terminal is
                 * not a widget window, so this cannot ride the widget pointer
                 * path; it works directly on the CELL GRID the compositor
                 * already owns.  Anchor here, extend in the motion handler,
                 * copy on release — the IRQ only ever records a range.
                 *
                 * The branch had to be added: content clicks were gated on
                 * `kind == WIN_APP`, so a press inside a terminal reached
                 * nothing at all.  That is why every command's output — the
                 * text people most want to copy — was the one thing that could
                 * not be selected. */
                int cxr = mx - win->x - BORDER;
                int cyr = my - win->y - TITLE_H;
                gterm_cell_at(win, cxr, cyr, &win->sel_ar, &win->sel_ac);
                win->sel_br = win->sel_ar;
                win->sel_bc = win->sel_ac;
                if (win->sel_on) { win->sel_on = 0; term_sel_dirty = win; }
                term_sel_win = win;
            }
        } else {
            /* §M64 — nothing under the pointer: this is a click on the DESKTOP
             * itself.  Hand it to the shell's desktop_click through the same
             * queue the panel uses, because we are in the mouse IRQ with the
             * WM lock held and a shortcut activation opens files and spawns
             * tasks (M22.7's rule, and §M49 found the same class of bug when
             * a console was bound outside its spawn).
             *
             * Double-click is detected HERE rather than in the shell: the
             * timestamps and the previous click position already live in this
             * file, and a second copy of the rule would drift from the title
             * bar's. */
            uint64_t now = timer_ticks_ms();
            int dbl = (lastclick_win == NULL &&
                       now - lastclick_ms < 400 &&
                       mx - lastclick_x < 6 && lastclick_x - mx < 6 &&
                       my - lastclick_y < 6 && lastclick_y - my < 6);
            lastclick_ms = now;
            lastclick_x = mx; lastclick_y = my;
            lastclick_win = NULL;
            pevq_push(dbl ? PEV_DESK_DBL : PEV_DESK_CLICK, mx, my);
            /* §M64 tail — and the phase stream, plus the grab.  The press is
             * pushed AFTER the click so the shell has already updated its
             * selection when the drag begins: a drag moves the icon the user
             * just pressed, and the two must agree on which one that is. */
            pevq_push(PEV_DESK_PRESS, mx, my);
            desk_grab = 1;
        }
    }

drag_update:
    if (drag == DRAG_MOVE && drag_win) {
        /* M22.4 — rect-bounded drag damage: remember the old outer rect
         * so the post-unlock path can damage old ∪ new instead of the
         * whole screen.  Before this fix every motion event during a
         * drag raised gui_damage_all() — a full 1280×800 recompose +
         * ~4 MB blit per event, which made the scene "swim".
         *
         * §perf — THROTTLE: only actually move + damage the window every
         * DRAG_FRAME_MS; intermediate motions just advance the cursor (the
         * post-unlock `else` branch sets need_frame for a cheap cursor-only
         * recompose).  This caps the big per-move blit to ~33 fps so a fast
         * drag of a large window can't monopolise the CPU. */
        int tgt_x = mx - grab_dx, tgt_y = my - grab_dy;
        if (tgt_x < -(drag_win->w - 40)) tgt_x = -(drag_win->w - 40);
        if (tgt_x > fbsurf.w - 40)       tgt_x = fbsurf.w - 40;
        if (tgt_y < 0)                   tgt_y = 0;
        if (tgt_y > work_h - TITLE_H)    tgt_y = work_h - TITLE_H;
        uint64_t now = timer_ticks_ms();
        drag_motions++;
        if ((tgt_x != drag_win->x || tgt_y != drag_win->y) &&
            (uint64_t)(now - last_drag_frame_ms) >= DRAG_FRAME_MS) {
            drag_frames++;
            last_drag_frame_ms = now;
            drag_old_x = drag_win->x;  drag_old_y = drag_win->y;
            drag_win->x = tgt_x;  drag_win->y = tgt_y;
            drag_moved = 1;
            /* Publish the move.  If several motions coalesce before the
             * compositor runs, the LAST one wins and the origin stays the
             * position the screen actually shows — an intermediate origin
             * would name pixels that were never on screen. */
            if (!mv_hint.active || mv_hint.win != drag_win) {
                mv_hint.ox = drag_old_x;  mv_hint.oy = drag_old_y;
            }
            mv_hint.active = 1;
            mv_hint.win = drag_win;
            mv_hint.nx = tgt_x;  mv_hint.ny = tgt_y;
            mv_hint.w  = drag_win->w;  mv_hint.h = drag_win->h;
        }
        /* else: coalesce — the window catches up on the next allowed frame;
         * the cursor still glides (need_frame set below). */
    } else if (drag == DRAG_RESIZE && drag_win) {
        rubber_w = mx - drag_win->x + 2;
        rubber_h = my - drag_win->y + 2;
        if (rubber_w < MIN_W) rubber_w = MIN_W;
        if (rubber_h < MIN_H) rubber_h = MIN_H;
        if (rubber_w > fbsurf.w) rubber_w = fbsurf.w;
        if (rubber_h > work_h)   rubber_h = work_h;
    }

    /* Button RELEASE.  A client needs the up edge as much as the down one —
     * without it a link click never completes and a drag never ends. */
    if (released & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT)) {
        struct gui_window* rw = topmost_at(mx, my);
        if (rw && rw->kind == WIN_APP && rw->input_hook && !rw->minimized &&
            my >= rw->y + TITLE_H)
            evq_push(rw, mx - rw->x - BORDER, my - rw->y - TITLE_H, 0,
                     (released & MOUSE_BTN_LEFT) ? 1 : 2, 0);
    }

    if ((released & MOUSE_BTN_LEFT) && term_sel_win) {
        /* Finish the selection.  The COPY happens on the compositor task
         * (clipboard_set allocates), so all this does is mark it ready. */
        if (term_sel_win->sel_on) term_sel_copy = term_sel_win;
        term_sel_win = NULL;
    }

    if ((released & MOUSE_BTN_LEFT) && desk_grab) {
        pevq_push(PEV_DESK_RELEASE, mx, my);
        desk_grab = 0;
    }

    if ((released & MOUSE_BTN_LEFT) && grab_win) {
        if (grab_win->used)
            evq_push_ptr(grab_win, mx - grab_win->x - BORDER,
                         my - grab_win->y - TITLE_H, WPTR_RELEASE);
        grab_win = NULL;
    }

    if (released & MOUSE_BTN_LEFT) {
        if (drag == DRAG_MOVE && drag_win && drag_report_on()) {
            uint32_t ms  = (uint32_t)(timer_ticks_ms() - drag_t0_ms);
            uint32_t cms = (uint32_t)((total_compose_ns - drag_compose0_ns) / 1000000ull);
            uint32_t kb  = (uint32_t)(((total_blit_px - drag_px0) * 4) / 1024);
            uint32_t fr  = (frames_full + frames_partial) - drag_f0;
            kprintf("gui: drag %dx%d - %u motions, %u moved, %u frames "
                    "(%u copied, %u repainted), %u KB, %u ms in compose of "
                    "%u ms elapsed\n",
                    drag_win->w, drag_win->h, drag_motions, drag_frames,
                    fr, drag_fast, drag_slow, kb, cms, ms);
        }
        if (drag == DRAG_RESIZE) force_full = 1;       /* geometry changes */
        if (drag == DRAG_RESIZE && drag_win &&
            (rubber_w != drag_win->w || rubber_h != drag_win->h)) {
            drag_win->pending_w = rubber_w;
            drag_win->pending_h = rubber_h;
        }
        drag     = DRAG_NONE;
        drag_win = NULL;
    }

    /* M22.7 — damage policy.  A resize rubber band spans the window and
     * shrinks/grows, so it keeps the full recompose (rare — only while
     * dragging the grip).  A press/release only changes focus + z-order:
     * damage just the affected windows (old focus un-highlights, the
     * clicked window raises + highlights) instead of the whole 9 MB
     * screen.  DRAG_MOVE damages old∪new; a pure glide is a bare wake. */
    struct gui_window* new_focus = focused_win;
    int resizing   = (drag == DRAG_RESIZE);
    int structural = (pressed || released) && !resizing && !force_full;
    int pop_x = popup.x, pop_y = popup.y, pop_w = popup.w, pop_h = popup.h;
    spin_unlock(&state_lock);

    /* Outside the lock: gui_damage takes damage_lock, and nesting it inside
     * state_lock is the one ordering this file does not allow. */
    if (popup_hover_moved) gui_damage(pop_x, pop_y, pop_w, pop_h);
    for (int i = 0; i < tb_ndmg; i++)
        gui_damage(tb_dmg[i][0], tb_dmg[i][1], tb_dmg[i][2], tb_dmg[i][3]);

    if (resizing || force_full) {
        gui_damage_all();                       /* rubber band / geometry apply */
    } else if (structural) {
        /* §M69 — "STRUCTURAL" HAS TO MEAN *SOMETHING CHANGED*, NOT *A BUTTON
         * MOVED*, and this line is most of a report: *"in Appearance the wheel
         * scrolls fine, but clicking the scrollbar or its arrows freezes it
         * for a couple of seconds and then works or does not."*
         *
         * The first of these three used to be UNCONDITIONAL — it damaged the
         * focused window on every press AND every release, wherever the
         * pointer was, whether or not the focus had moved.  So a click inside
         * the window you are already working in cost TWO full-window repaints
         * before any handler ran: measured on the Appearance panel at 431 kpx
         * and 35-60 ms of compositing each, on a machine whose compositor also
         * draws the cursor.  *That is why the wheel felt fine and the bar did
         * not — the wheel has no button transition, so it never paid this.*
         *
         * A focus HIGHLIGHT changes only when the focus does; a raise or a
         * restore is reported by the WM (`wm_changed`); and a click that
         * lands on a window which is neither is exactly the case that needs
         * nothing.  `clicked` keeps its own arm for the third case: a
         * minimize button pressed on a window that was not focused. */
        int hit = 0;
        int focus_moved = (new_focus != old_focus);
        if (focus_moved && old_focus && old_focus->used) {
            gui_damage_win(old_focus); hit = 1;
        }
        if (focus_moved && new_focus && new_focus->used) {
            gui_damage_win(new_focus); hit = 1;
        }
        if (clicked && clicked->used && (wm_changed || clicked != new_focus)) {
            gui_damage_win(clicked); hit = 1;
        }
        panel_gen++;                            /* taskbar buttons may change */
        if (!hit) need_frame = 1;               /* click on empty desktop */
    } else if (drag_moved) {
        /* NO damage rects for the move itself — the hint carries it, and
         * compose decides whether it can copy the image instead of repainting
         * it.  Damaging here as well would defeat the "nothing else changed"
         * test the fast path is built on.  What is still needed is a FRAME:
         * need_frame wakes the compositor without claiming any area. */
        need_frame = 1;
    } else {
        need_frame = 1;                         /* cursor glide only */
    }
}
/* M22.3 — Alt-Tab.  Raw keycode hook, runs in the keyboard IRQ before
 * keymap translation.  Rotate the top window to the bottom, then
 * activate the new top visible window — repeated presses cycle.
 *
 * M22.5 — the same hook also feeds the widget layer: navigation /
 * editing keys (arrows, Home/End, Delete, PgUp/PgDn, Insert) and
 * Ctrl+letter shortcuts are consumed here and queued as raw keycode
 * events whenever the focused window is an APP window.  TERMINAL
 * windows are untouched (their shells are char-based; unmapped
 * keycodes keep dying in keymap_translate as before). */
static int kc_is_nav(uint8_t kc) {
    return kc >= KC_INSERT && kc <= KC_UP;      /* 0x49..0x52 block */
}
int gui_raw_key(uint8_t keycode, uint8_t mods) {
    if (!gui_active) return 0;

    /* §M65 — ESCAPE CLOSES AN OPEN POPUP, and consumes the key.  A menu you
     * can only dismiss with the mouse is a menu that traps a keyboard user;
     * the owner still hears about it (row -1) so a combo can put its old value
     * back rather than leaving the control in a half-open state. */
    if (popup.active && keycode == KC_ESC) {
        struct gui_window* ow = popup.owner;
        int tag = popup.tag;
        popup.active = 0;
        popup.hover = -1;
        if (ow && ow->used) {
            struct app_event e = {0};
            e.type = AE_POPUP;
            e.x = -1;
            e.y = (int16_t)tag;
            aq_push(ow, e);
        }
        gui_damage_all();
        return 1;
    }

    /* §M69 gate 3 + THE ESCAPE HATCH.  Both are about a modal window, and both
     * sit above every other binding in this function — but BELOW the popup's
     * Escape above, because a combo opened inside a dialog must close with the
     * first Esc and leave the dialog standing.
     *
     * Alt-Tab is refused outright: modality is a claim on the focus, and a
     * window switcher that can walk away from it makes the claim advisory.
     *
     * Esc asks the modal to CLOSE, from the compositor, rather than being left
     * to the dialog's own key hook.  That hook runs on the dialog's app-host
     * task — so if that task ever wedges, the hook is exactly the thing that
     * will not run, and the desktop would be locked behind a dialog with no
     * way out.  Routing it through `want_close` reuses the X button's path,
     * including its second-press force-kill (§4.38.1: the escalation is the
     * USER's), so a wedged dialog costs two presses rather than a reboot. */
    if (modal_win && modal_win->used) {
        if (keycode == KC_TAB && (mods & KBD_MOD_LALT)) return 1;
        if (keycode == KC_ESC) {
            if (modal_win->want_close) modal_win->close_force_now = 1;
            modal_win->want_close = 1;
            need_frame = 1;
            return 1;
        }
    }

    /* §M58/§M59 — COPY AND PASTE IN A TERMINAL WINDOW, from the keyboard.
     *
     * Reported from use: *"I can't manage with the clipboard, the selection
     * doesn't work either."*  Both worked in the automated test — and that test
     * pasted with the MIDDLE BUTTON, which a trackpad does not have.  A feature
     * whose only trigger is a button the user's hardware lacks is, from where
     * they sit, a feature that does not exist.
     *
     * So the keyboard route, which is what people reach for anyway:
     *   Ctrl+Shift+C / Ctrl+Insert — copy the selection to the clipboard
     *   Ctrl+Shift+V / Shift+Insert — paste the clipboard into the terminal
     *
     * SHIFT is what keeps Ctrl+C free to remain the interrupt: a shell's Ctrl+C
     * must not become "copy" just because something happens to be selected —
     * that would make the most important key on a terminal depend on invisible
     * state.  (The same reason every terminal emulator picked this binding.) */
    {
        struct gui_window* tw = focused_win;
        if (tw && tw->used && tw->kind == WIN_TERM && tw->cells) {
            int ctrl  = (mods & KBD_MOD_CTRL_MASK) != 0;
            int shift = (mods & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
            int copy  = (ctrl && shift && keycode == KC_C) ||
                        (ctrl && keycode == KC_INSERT);
            int paste = (ctrl && shift && keycode == KC_V) ||
                        (shift && keycode == KC_INSERT);
            if (copy)  { term_sel_copy_to_clip = tw; need_frame = 1; return 1; }
            if (paste) { term_paste_win = tw;        need_frame = 1; return 1; }

            /* §M58 — SHIFT+PgUp/PgDn walks the scrollback a page at a time.
             * Shift is load-bearing for the same reason it is on copy: plain
             * PgUp/PgDn belong to whatever is running IN the terminal (an
             * editor, a pager), and stealing them would break those programs
             * in a way the user cannot see or turn off. */
            if (shift && (keycode == KC_PGUP || keycode == KC_PGDN)) {
                int page = tw->rows > 2 ? tw->rows - 2 : 1;
                if (gterm_view_scroll(tw, keycode == KC_PGUP ? page : -page))
                    term_sel_dirty = tw;
                need_frame = 1;
                return 1;
            }

            /* Anything else TYPED means the user is done reading history: snap
             * back to the live bottom, exactly as every terminal does — output
             * appearing somewhere the user cannot see is how a shell looks
             * broken. */
            /* (Bare modifiers never reach here — the PS/2 driver consumes
             * shift/ctrl/alt make+break codes before translation — so this
             * needs no exception for them.) */
            if (tw->view_off > 0) {
                tw->view_off = 0;
                term_sel_dirty = tw;
                need_frame = 1;
            }
        }
    }

    if (keycode != KC_TAB || !(mods & KBD_MOD_LALT)) {
        /* Widget-bound keycodes?  focused_win is an atomic pointer
         * read; kind/used are stable for live windows. */
        struct gui_window* win = focused_win;
        /* §M40 — widget windows only want the keycodes their widgets act on
         * (nav + Ctrl-letter); a window forwarding to a CLIENT wants EVERY key,
         * because `wl_keyboard.key` carries raw keycodes and the application
         * does its own interpretation. */
        if (win && win->used && win->kind == WIN_APP &&
            (win->input_hook || kc_is_nav(keycode) ||
             ((mods & KBD_MOD_CTRL_MASK) &&
              keycode >= KC_A && keycode <= KC_Z))) {
            uint32_t n = (kcq_h + 1) % KCQ_SZ;
            if (n != kcq_t) {
                kcq[kcq_h] = (uint16_t)(keycode | ((uint16_t)mods << 8));
                kcq_h = n;
            }
            need_frame = 1;
            /* A client window takes the raw keycode AND must still let the
             * keymap run: returning 1 here consumed the key outright, so the
             * cooked character was never produced and a client with no keymap
             * of its own (NetSurf) could not receive letters at all.  Report
             * "not consumed" for those, so ps2_keyboard goes on to translate
             * and the character arrives as a second event. */
            return win->input_hook ? 0 : 1;
        }

        /* §M64 tail — NO WINDOW WANTED IT: the desktop is the focus of last
         * resort.  These keycodes were dropped here, which is why the icon
         * field could only be reached with a mouse.
         *
         * ENTER AND ESCAPE ARE GATED ON THE DESKTOP ACTUALLY HAVING A
         * SELECTION, and that gate is not fussiness: the GUI suppresses the
         * boot shell's console but keys still reach its VC, so consuming
         * Enter unconditionally would make it impossible to submit a shell
         * command while the desktop is up — including for the test harness
         * that drives this build.  So the rule is the one a person already
         * expects: select something and the desktop has the keyboard; clear
         * the selection and it goes back.  The arrows are safe either way —
         * nothing else was ever going to receive them. */
        int nav = (keycode == KC_LEFT || keycode == KC_RIGHT ||
                   keycode == KC_UP   || keycode == KC_DOWN  ||
                   keycode == KC_HOME || keycode == KC_END);
        int claim = (desk_focus && (keycode == KC_ENTER || keycode == KC_ESC));
        if (nav || claim) {
            uint32_t n = (kcq_h + 1) % KCQ_SZ;
            if (n != kcq_t) {
                kcq[kcq_h] = (uint16_t)(keycode | ((uint16_t)mods << 8));
                kcq_h = n;
            }
            need_frame = 1;
            /* Consume only what we CLAIMED.  An arrow key nothing else uses
             * may as well fall through; Enter must not, or it would both open
             * a shortcut and submit a shell line. */
            return claim ? 1 : 0;
        }
        return 0;
    }
    spin_lock(&state_lock);
    if (zcount >= 2) {
        /* Demote the currently ACTIVE (top visible) window to the
         * bottom, then activate the next visible one — repeated
         * presses walk the whole visible set.  Rotating the raw top
         * would stall on minimized windows parked at the top of the
         * z-order. */
        struct gui_window* cur = top_visible_locked();
        if (cur) {
            int i;
            for (i = 0; i < zcount && zorder[i] != cur; i++) ;
            for (; i > 0; i--) zorder[i] = zorder[i - 1];
            zorder[0] = cur;
            struct gui_window* nf = top_visible_locked();
            if (nf) gui_wm_focus_raise_locked(nf);
        }
    }
    spin_unlock(&state_lock);
    gui_damage_all();
    return 1;
}
int gui_kbd_hook(char c) {
    if (!gui_active) return 0;
    struct gui_window* win = focused_win;
    if (!win || win->kind != WIN_APP) return 0;
    uint32_t n = (keyq_h + 1) % KEYQ_SZ;
    if (n != keyq_t) {
        keyq[keyq_h] = c;
        keyq_h = n;
    }
    return 1;
}


/* The desktop's pointer queue, popped by the desktop task.  Bounds the
 * single-producer/single-consumer ring to this file. */
int pevq_pop(struct pev* out) {
    if (pevq_t == pevq_h) return 0;
    *out = pevq[pevq_t];
    pevq_t = (pevq_t + 1) % PEVQ_SZ;
    return 1;
}
