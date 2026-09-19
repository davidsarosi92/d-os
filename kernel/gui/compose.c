/* =============================================================================
 * compose.c — damage tracking, scene painting and presentation (§M70).
 *
 * Extracted from gui.c.  This file answers "WHICH PIXELS"; wm.c answers "which
 * window".  It is the code every performance measurement in §M61, §4.61 and
 * §M69 was made against, so the split was taken with a before-and-after
 * benchmark rather than on the strength of it looking tidier.
 *
 * DAMAGE IS A LIST OF DISJOINT RECTS, NOT A BOUNDING BOX.  `compose()` paints
 * and presents each rect separately, so a Task Manager refresh and a far-away
 * cursor stay two small blits instead of one huge union — measured at the time
 * as ~630 KB/frame against ~2.4-5.3 MB.  A bounding box is *true* and costs
 * more than the rects it covers; in a process list the movers are scattered
 * top and bottom, and the band IS the table.
 *
 * A MOVING WINDOW'S PIXELS DO NOT CHANGE, which is why a drag COPIES inside
 * the back buffer and only repaints the leftovers (§4.61: 1630 -> 986 ms over
 * a 40-step drag, 36 -> 22 ms per composite, under a 30 ms budget).  Four
 * rules, each a way to get it wrong:
 *
 *   1. COPY FIRST, PAINT SECOND — the vacated strip and the cursor's footprint
 *      are INSIDE the source, and the painter draws the window already moved.
 *   2. The window must be TOPMOST; anything above it would be dragged along.
 *   3. THE CURSOR COMES ALONG FOR THE RIDE.  `draw_cursor` paints into the
 *      back buffer, so the copy deposits a second cursor at old+delta — one
 *      small rect is repainted afterwards.
 *   4. DIRECTION MATTERS: `gfx_move_within` picks row/column order from the
 *      sign of the move, where a plain blit would read pixels it had already
 *      overwritten.
 *
 * The move arrives OUT OF BAND (`mv_hint`) rather than as damage, and that is
 * what makes it safe: the copy path runs only when the damage list is
 * otherwise EMPTY.  Inspecting merged damage rects cannot tell a window that
 * MOVED from one that moved AND redrew, and the failure mode of guessing is a
 * stale image nobody can explain.
 *
 * THE TIMER IS INSIDE THE WORK, not around the call.  §4.61's first version
 * accumulated BEFORE the draw and present passes and reported 15 us/frame for
 * megabytes of blitting: a measurement placed on the wrong side of the work
 * does not understate it, it reports the work as free.
 * =========================================================================== */

#include "gui_priv.h"
#include "gui.h"
#include "gui_internal.h"
#include "desktop.h"
#include "gfx.h"
#include "fb_present.h"
#include "console_plate.h"
#include "wallpaper.h"
#include "widget.h"
#include "icons.h"
#include "locale.h"
#include "version.h"
#include "task.h"
#include "timer.h"
#include "printf.h"
#include "kmalloc.h"
#include "config.h"
#include "lock.h"
#include <stdint.h>
#include <stddef.h>

/* M22.7 — damage tracking as a LIST of disjoint rects (was a single
 * bounding box).  A single box merged far-apart damages — a Task Manager
 * refresh in one corner and the cursor in another — into their bounding
 * box, so the compositor re-blitted a huge diagonal region every refresh
 * and the cursor visibly stuttered.  A list composites each small rect on
 * its own, so two disjoint updates stay two small blits.  Rects accumulate
 * under damage_lock (nested inside state_lock on the mouse path — never the
 * other way).  full/partial counters back the `gui stats` command. */
spinlock_t damage_lock;

static struct rect dmg_list[DMG_MAX];

static int         dmg_n = 0;

uint32_t frames_full = 0, frames_partial = 0;

/* Read once at gui_start rather than per rect: this is consulted inside the
 * painter's inner path, and a config lookup there would cost more than the
 * skip saves. */
int g_occlude = 1;

/* How often the skip actually fires.  Reported, because an optimisation whose
 * hit rate nobody can see is one nobody can defend when it turns out not to
 * apply to the case that hurts — which is exactly what happened to §4.61's
 * version of this. */
uint32_t occluded_rects = 0, painted_rects = 0;

uint64_t total_blit_px = 0;              /* M22.7 — avg damage/frame */

/* §perf — how much TIME the compositor spends, not just how many pixels it
 * moves.  "The desktop lags when I drag a big window" is a statement about
 * duration, and pixels are only a proxy for it: the same rectangle costs a
 * different number of milliseconds on a 4 GHz core and under emulation. */
uint64_t total_compose_ns = 0;

/* Last present's dirty rects.  A page flip has buffer-age 2: the hidden
 * buffer is stale outside the regions touched in the last TWO presents, so
 * each present copies this frame's rects ∪ last frame's rects. */
/* §M76.1/§M77.2 — non-zero while a caller is assembling a multi-rect update;
 * see gui_damage_begin and the note at the top of compose(). */
static volatile int dmg_hold;

static struct rect prev_dmg[DMG_MAX + 2];       /* +2 for the cursor rects   */

static int         prev_dmg_n = 0;

static const char* const cursor_rows[17] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X........X ",
    "X.....XXXXX",
    "X..X..X    ",
    "X.X X..X   ",
    "XX  X..X   ",
    "X    X..X  ",
    "     X..X  ",
    "      XX   ",
};

/* M22.4 — compositor-side cursor bookkeeping.  Where the cursor was
 * LAST DRAWN, updated only by compose().  Lesson learned (2026-07-04):
 * compose() snapshots the damage rect BEFORE the WM state, so an
 * IRQ-supplied cursor rect can describe an OLDER position than the
 * (cx,cy) we end up drawing — the cursor got erased at its old spot
 * but clipped away at its new one for that frame (visible flicker /
 * ghosting when gliding over contrasting chrome).  The fix: the mouse
 * IRQ never submits cursor rects at all (a glide is a bare need_frame
 * wake); compose() itself unions the previously-drawn and the freshly
 * snapshotted cursor rects into the clip region, so erase + redraw
 * always happen in the same frame with one consistent position. */
static int last_cur_x = -100, last_cur_y = -100;

static int rects_overlap(const struct rect* r, int x0, int y0, int x1, int y1) {
    return !(x0 >= r->x1 || x1 <= r->x0 || y0 >= r->y1 || y1 <= r->y0);
}
static void rect_grow(struct rect* r, int x0, int y0, int x1, int y1) {
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}
static long rect_area(const struct rect* r) {
    return (long)(r->x1 - r->x0) * (r->y1 - r->y0);
}
/* Add a damage rect: merge into an OVERLAPPING existing rect (so we never
 * composite the same pixels twice), else append; if the list is full, fold
 * it into the rect whose area grows least (bounded degradation). */
static void damage_add_locked(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    for (int i = 0; i < dmg_n; i++)
        if (rects_overlap(&dmg_list[i], x0, y0, x1, y1)) {
            rect_grow(&dmg_list[i], x0, y0, x1, y1);
            return;
        }
    if (dmg_n < DMG_MAX) {
        dmg_list[dmg_n++] = (struct rect){ x0, y0, x1, y1 };
        return;
    }
    int best = 0; long best_cost = -1;
    for (int i = 0; i < dmg_n; i++) {
        struct rect g = dmg_list[i];
        rect_grow(&g, x0, y0, x1, y1);
        long cost = rect_area(&g) - rect_area(&dmg_list[i]);
        if (best_cost < 0 || cost < best_cost) { best_cost = cost; best = i; }
    }
    rect_grow(&dmg_list[best], x0, y0, x1, y1);
}
/* §M76.1 — HOLD THE FRAME WHILE ONE UPDATE IS STILL BEING ASSEMBLED.
 *
 * Reported from use: *"the table refresh runs top to bottom in a wave, and
 * there is a little lag while it does."*  Both halves are one cause.  A refresh
 * damages its changed cells ONE AT A TIME from the app-host task, and the
 * compositor runs on ANOTHER CPU — so it wakes on the first rect and composes
 * whatever has arrived, then again, and again.  The diff walks slots in order,
 * so the partial frames march down the table.
 *
 * The rects themselves are right: §4.61 made damage a LIST of disjoint rects
 * precisely so a small refresh and a far-away cursor stay two small blits.
 * What was missing is that a caller assembling SEVERAL of them had no way to
 * say "not yet".
 *
 * A COUNTER AND NOT A FLAG, because these nest: a widget's refresh may damage
 * through a helper that brackets its own work, and a flag would let the inner
 * end release a frame the outer one is still building.
 *
 * The rects are still added immediately — only the WAKE is deferred.  So a
 * caller that forgets to end the bracket costs latency until the next
 * unbracketed damage, never a lost update. */

void gui_damage_begin(void) { __atomic_add_fetch(&dmg_hold, 1, __ATOMIC_ACQ_REL); }

void gui_damage_end(void) {
    if (__atomic_sub_fetch(&dmg_hold, 1, __ATOMIC_ACQ_REL) <= 0) {
        __atomic_store_n(&dmg_hold, 0, __ATOMIC_RELEASE);
        need_frame = 1;
    }
}

void gui_damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    uint32_t fl = spin_lock_irqsave(&damage_lock);
    damage_add_locked(x, y, x + w, y + h);
    spin_unlock_irqrestore(&damage_lock, fl);
    if (__atomic_load_n(&dmg_hold, __ATOMIC_ACQUIRE) == 0) need_frame = 1;
}
void gui_damage_all(void) {
    uint32_t fl = spin_lock_irqsave(&damage_lock);
    dmg_n = 0;                                  /* collapse to one full rect */
    damage_add_locked(0, 0, fbsurf.w, fbsurf.h);
    spin_unlock_irqrestore(&damage_lock, fl);
    need_frame = 1;
    panel_gen++;            /* M22.7-B — WM-ish change; nudge the taskbar */
}
/* Window rect + margin for border/shadow (+5 shadow, +2 safety). */
void gui_damage_win(struct gui_window* w) {
    gui_damage(w->x - 2, w->y - 2, w->w + 9, w->h + 9);
}
void gui_get_stats(uint32_t* full, uint32_t* partial, uint32_t* avg_kb) {
    if (full)    *full    = frames_full;
    if (partial) *partial = frames_partial;
    if (avg_kb) {
        uint32_t frames = frames_full + frames_partial;
        *avg_kb = frames ? (uint32_t)(total_blit_px * 4 / 1024 / frames) : 0;
    }
}
static void draw_rect_outline(struct gfx_surface* s, int x, int y, int w, int h,
                              int t, uint32_t c) {
    gfx_fill(s, x,         y,         w, t, c);
    gfx_fill(s, x,         y + h - t, w, t, c);
    gfx_fill(s, x,         y,         t, h, c);
    gfx_fill(s, x + w - t, y,         t, h, c);
}
static void draw_popup(struct gfx_surface* dst) {
    if (!popup.active) return;
    gfx_blend_fill(dst, popup.x + 4, popup.y + 4, popup.w, popup.h, COL_SHADOW);
    gfx_fill(dst, popup.x, popup.y, popup.w, popup.h, COL_POP_BG);
    draw_rect_outline(dst, popup.x, popup.y, popup.w, popup.h, 1, COL_POP_EDGE);
    for (int i = 0; i < popup.count; i++) {
        int y = popup.y + 3 + i * POPUP_ROW_H;
        if (popup.items[i][0] == '-' && !popup.items[i][1]) {
            gfx_fill(dst, popup.x + 6, y + POPUP_ROW_H / 2, popup.w - 12, 1, COL_POP_SEP);
            continue;
        }
        if (i == popup.hover)
            gfx_fill(dst, popup.x + 2, y, popup.w - 4, POPUP_ROW_H, COL_POP_HOVER);
        cp_text(dst, popup.x + 10, y + (POPUP_ROW_H - cp_fh()) / 2,
                 popup.items[i], COL_POP_TEXT);
    }
}
static void draw_cursor(struct gfx_surface* s, int cx, int cy) {
    for (int j = 0; j < 17; j++) {
        for (int i = 0; cursor_rows[j][i]; i++) {
            char p = cursor_rows[j][i];
            if (p == ' ') continue;
            int x = cx + i, y = cy + j;
            if (x < 0 || x >= s->w || y < 0 || y >= s->h) continue;
            s->px[(size_t)y * s->stride + x] = (p == 'X') ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}
/* Paint the whole scene (wallpaper → windows → rubber → panel → cursor)
 * into backsurf, clipped to one damage rect.  Called once per rect. */
static void draw_scene_rect(const struct scene_snapshot* s,
                            int rx0, int ry0, int rx1, int ry1) {
    gfx_set_clip(&backsurf, rx0, ry0, rx1 - rx0, ry1 - ry0);

    /* SKIP WHAT AN OPAQUE WINDOW COMPLETELY COVERS.
     *
     * Reported from use: *"when a window is full size, the whole thing lags."*
     * This function painted the wallpaper, then the desktop icons, then every
     * window from the bottom up — so a full-screen window meant the whole
     * screen was filled at least TWICE per frame, and everything under it was
     * pixels computed and then thrown away.
     *
     * §4.61 measured skipping the wallpaper at 2% and moved on, and that
     * measurement was right about the case it looked at: DURING A DRAG the
     * damage rect is the window UNION the strip it vacated, so it is never
     * fully covered and the skip almost never fires.  A maximized window is the
     * opposite case — the damage rect is inside the window every time — which
     * is why the same optimisation is worth nothing there and a great deal
     * here.  *A measurement answers the case it was taken on.*
     *
     * The test is deliberately strict: the topmost window must cover this rect
     * ENTIRELY, and only what is strictly below it is skipped.  A partial
     * overlap paints normally, because "mostly covered" cannot be composited
     * correctly without per-pixel coverage — and getting that wrong leaves
     * stale pixels, which is the failure this file has already paid for twice.
     *
     * `gui.occlude = 0` turns it off, so both paths stay reachable and
     * measurable rather than one becoming the only one anybody ever runs. */
    int first = 0;
    if (g_occlude) {
        for (int i = s->zn - 1; i >= 0; i--) {
            /* Opaque and covering: the CONTENT area plus its border, i.e. the
             * whole outer rectangle, which is what this function paints. */
            if (s->wx[i] <= rx0 && s->wy[i] <= ry0 &&
                s->wx[i] + s->ww[i] >= rx1 && s->wy[i] + s->wh[i] >= ry1) {
                first = i;
                break;
            }
        }
    }

    if (first == 0) {
        painted_rects++;
        gfx_blit(&backsurf, 0, 0, &wallsurf, 0, 0, wallsurf.w, wallsurf.h);
    } else {
        occluded_rects++;
    }

    /* §M64 — the background LAYER: desktop icons sit on the wallpaper and
     * under every window.  Painted per damage rect like everything else here,
     * and clipped by the same clip box, so a shortcut only costs pixels when
     * its rectangle is actually dirty. */
    if (first == 0 && shell && shell->draw_under) shell->draw_under(&backsurf);

    for (int i = first; i < s->zn; i++) {
        struct gui_window* win = s->zsnap[i];
        int x = s->wx[i], y = s->wy[i], w = s->ww[i], h = s->wh[i];
        int focused = (win == s->fsnap);

        /* §M69 gate 4 — THE MODAL BACKDROP, painted here rather than as a
         * layer of its own because "under the modal and over everything else"
         * is a position in THIS loop and nowhere else.  Whole surface, left to
         * `clip_rect` inside gfx_blend_fill to bound it to the damage rect —
         * so it costs the dirty area, not the screen (§4.61's discipline).
         *
         * The backdrop is what makes the three input gates legible.  Swallowed
         * clicks with an undimmed screen behind them are indistinguishable
         * from a machine that has stopped responding — which is the single
         * most expensive way for this to be misread. */
        if (i == s->modal_idx)
            gfx_blend_fill(&backsurf, 0, 0, backsurf.w, backsurf.h, COL_MODAL_DIM);

        gfx_blend_fill(&backsurf, x + 5, y + 5, w, h, COL_SHADOW);
        draw_rect_outline(&backsurf, x, y, w, h, BORDER,
                          focused ? COL_BORDER_F : COL_BORDER_U);
        /* Flat title plate.  `raised` when focused, `tray` when not — the two
         * surface layers the design gives a title bar, rather than two ends of
         * a gradient.  The focused/unfocused difference now lives in the
         * BORDER (accent vs line) as well, which is what carries it at a
         * glance once the fills are flat. */
        gfx_fill(&backsurf, x + BORDER, y + BORDER,
                 w - 2 * BORDER, TITLE_H - BORDER,
                 focused ? COL_TITLE_F_TOP : COL_TITLE_U_TOP);
        cp_text(&backsurf, x + 8, y + (TITLE_H - cp_fh() + BORDER) / 2,
                 lstr(win->title), COL_TITLE_TEXT);

        {
            /* THE THREE TITLE BUTTONS, AS THE DESIGN DRAWS THEM.
             *
             * They were M22's Windows chrome: a RED close box, a white filled
             * square for maximize and a hardcoded 0xFF3A4A5E behind the other
             * two.  The Console Plate demo puts all three in the SAME plate —
             * `tray` with a 1 px `line` border — and draws each glyph as a thin
             * `muted` stroke.  Uniformity is the point: the design gives the
             * close button no special colour, because a red box in the title
             * bar is a warning, and closing a window is not one.  It is the
             * ordinary way out, and it earns emphasis only under the pointer.
             *
             * Glyphs are STROKES, not font characters: an "x" from an 8x8
             * bitmap font is a letter and reads as one next to two geometric
             * marks. */
            const cp_theme* th = cp_current_theme();
            int bx, by, bw, bh;
            int hov = (win == tb_hover_win) ? tb_hover_idx : -1;

            const int nbtn = title_btn_count(win);
            for (int i = 0; i < nbtn; i++) {
                title_btn_rect(x, y, w, i, &bx, &by, &bw, &bh);
                /* Hover is the ONLY emphasis these get.  The design gives all
                 * three the same resting plate, so without it there is no
                 * feedback at all that the pointer is on a target — which is
                 * what the close button's old red box used to supply by being
                 * permanently loud. */
                int on = (i == hov);
                cp_plate(&backsurf, bx, by, bw, bh,
                         on ? th->hover : th->tray, on ? th->accent : th->line);
            }

            const cp_color g0 = (hov == TB_CLOSE) ? th->text : th->muted;
            const cp_color g1 = (hov == TB_MAX)   ? th->text : th->muted;
            const cp_color g2 = (hov == TB_MIN)   ? th->text : th->muted;

            /* close: two diagonals */
            title_btn_rect(x, y, w, TB_CLOSE, &bx, &by, &bw, &bh);
            {
                int i0 = bw / 3, i1 = bw - bw / 3 - 1;
                for (int k = i0; k <= i1; k++) {
                    gfx_fill(&backsurf, bx + k, by + k, 1, 1, g0);
                    gfx_fill(&backsurf, bx + k, by + (i0 + i1 - k), 1, 1, g0);
                }
            }
            /* maximize: a square, or two offset squares when already maximized */
            if (nbtn > TB_MAX) {
            title_btn_rect(x, y, w, TB_MAX, &bx, &by, &bw, &bh);
            if (win->maximized) {
                draw_rect_outline(&backsurf, bx + bw/3 + 1, by + bw/4, bw/2, bh/2,
                                  1, g1);
                draw_rect_outline(&backsurf, bx + bw/4, by + bw/3 + 1, bw/2, bh/2,
                                  1, g1);
            } else {
                draw_rect_outline(&backsurf, bx + bw/4, by + bh/4,
                                  bw - bw/2, bh - bh/2, 1, g1);
            }
            }
            /* minimize: one rule, centred rather than sitting on the floor of
             * the box — the design's dash is a mark, not an underline */
            if (nbtn > TB_MIN) {
            title_btn_rect(x, y, w, TB_MIN, &bx, &by, &bw, &bh);
            gfx_fill(&backsurf, bx + bw/4, by + bh/2, bw - bw/2, 1, g2);
            }
        }

        spin_lock(&win->lock);
        gfx_blit(&backsurf, x + BORDER, y + TITLE_H,
                 &win->surf, 0, 0, win->surf.w, win->surf.h);
        spin_unlock(&win->lock);

        uint32_t gc = focused ? COL_BORDER_F : COL_BORDER_U;
        for (int t = 0; t < 3; t++) {
            int o = 4 + t * 4;
            gfx_line(&backsurf, x + w - 3 - o, y + h - 4,
                                x + w - 4,     y + h - 3 - o, gc);
        }
    }

    if (s->dsnap == DRAG_RESIZE && s->dwin)
        draw_rect_outline(&backsurf, s->rrx, s->rry, s->rw, s->rh, 2, COL_RUBBER);

    /* M22.7-B — desktop chrome (taskbar always + open popup) from panelsurf. */
    if (panel_ready) {
        spin_lock(&panel_lock);
        gfx_blit(&backsurf, 0, work_h, &panelsurf, 0, work_h,
                 fbsurf.w, fbsurf.h - work_h);
        if (pnl_pop_on) {
            int py = pnl_pop_y, ph = pnl_pop_h;
            if (py < panel_strip_top) { ph -= panel_strip_top - py; py = panel_strip_top; }
            if (ph > 0)
                gfx_blit(&backsurf, pnl_pop_x, py, &panelsurf,
                         pnl_pop_x, py, pnl_pop_w, ph);
        }
        spin_unlock(&panel_lock);
        /* §M69 — and the chrome is dimmed too, in a SECOND fill rather than
         * the one above, because the panel is composited after the windows and
         * would have painted straight over it.  The design's backdrop covers
         * the whole viewport, and here that is also the honest picture: gate 2
         * swallows taskbar clicks as well, so a bright, live-looking taskbar
         * over a modal dialog would be an invitation to press something that
         * does nothing.  The modal window itself never reaches this strip
         * (windows live above `work_h`), so this cannot dim the dialog. */
        if (s->modal_idx >= 0)
            gfx_blend_fill(&backsurf, 0, work_h, fbsurf.w, fbsurf.h - work_h,
                           COL_MODAL_DIM);
    }

    /* §M65 — the window popup sits above every window (and above the panel
     * chrome, so a menu near the bottom edge is not eaten by the taskbar) and
     * below the cursor, which is always last. */
    draw_popup(&backsurf);

    draw_cursor(&backsurf, s->cx, s->cy);
}
void compose(void) {
    /* §M77.2 — DO NOT PAINT A BATCH THAT IS STILL BEING ASSEMBLED.
     *
     * `gui_damage_begin/end` defers the WAKE, which is enough when nothing else
     * is producing frames.  It is not enough during a CLICK: the pointer moves,
     * the compositor draws the cursor, and frames are being produced anyway —
     * so a compose can land between two rects of one update, take the first,
     * and leave the second for the next frame.
     *
     * Reported from use, and the symptom names the mechanism exactly: selecting
     * a row showed **two rows highlighted at once for a moment**.  A selection
     * change damages TWO rows — the one losing the highlight and the one taking
     * it — and if only one of them is in this frame, both look selected until
     * the next.
     *
     * Returning here costs at most the length of one bracket, which is a single
     * widget refresh; the wake is still pending, so the frame happens
     * immediately afterwards.  *A frame that shows half an update is worse than
     * a frame that waits for it.* */
    if (__atomic_load_n(&dmg_hold, __ATOMIC_ACQUIRE) > 0) return;

    uint64_t compose_t0 = timer_now_ns();
    /* 1. Snapshot + clear the damage LIST: anything damaged while we paint
     *    lands in the next frame. */
    struct rect rl[DMG_MAX + 2];
    int rn;
    {
        uint32_t dfl = spin_lock_irqsave(&damage_lock);
        rn = dmg_n;
        for (int i = 0; i < rn; i++) rl[i] = dmg_list[i];
        dmg_n = 0;
        spin_unlock_irqrestore(&damage_lock, dfl);
    }

    /* 2. Snapshot the WM state (shared across all rects). */
    struct scene_snapshot s;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    s.zn = 0;
    s.modal_idx = -1;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i]->minimized) continue;     /* M22.3 */
        if (zorder[i] == modal_win) { s.modal_idx = s.zn; modal_dbg_seen++; }
        s.zsnap[s.zn] = zorder[i];
        s.wx[s.zn] = zorder[i]->x;  s.wy[s.zn] = zorder[i]->y;
        s.ww[s.zn] = zorder[i]->w;  s.wh[s.zn] = zorder[i]->h;
        s.zn++;
    }
    if (modal_win && s.modal_idx < 0) modal_dbg_missing++;
    s.cx = mx; s.cy = my;
    /* Report each condition ONCE — enough to tell the two causes apart, and
     * a per-frame log would be its own denial of service on the serial line. */
    {
        static int said_seen, said_missing;
        if (modal_dbg_seen && !said_seen) {
            said_seen = 1;
            kprintf("gui: modal visible to the compositor (z-index %d of %d)\n",
                    s.modal_idx, s.zn);
        }
        if (modal_dbg_missing && !said_missing) {
            said_missing = 1;
            kprintf("gui: MODAL CLAIMED BUT NOT IN THE Z-ORDER (%d windows)\n",
                    s.zn);
        }
    }
    s.dsnap = drag; s.dwin = drag_win; s.rw = rubber_w; s.rh = rubber_h;
    s.rrx = s.rry = 0;
    if (s.dwin) { s.rrx = s.dwin->x; s.rry = s.dwin->y; }
    s.fsnap = focused_win;
    struct move_hint mh = mv_hint;
    mv_hint.active = 0;                          /* consumed exactly once     */
    spin_unlock_irqrestore(&state_lock, fl);

    /* ---- can this frame be a COPY instead of a repaint? ------------------
     *
     * Every condition below is a way the copy could produce a wrong image, so
     * each one is a fall-back to the ordinary painter rather than a fix-up:
     *
     *  - OTHER DAMAGE (rn != 0).  Something else changed and the merged rects
     *    can no longer be attributed; repaint everything.
     *  - NOT TOPMOST.  The source rectangle is only the dragged window's own
     *    pixels if nothing is drawn over it.  Anything above would be dragged
     *    along with it — a window that never moved, smearing across the screen.
     *  - MINIMISED / GONE / not the window being dragged now.
     *  - A ZERO move, which the copy would spend its whole cost on. */
    int fast = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;      /* the union (draw + present) */
    if (mh.active && mh.win && mh.win == s.dwin && s.zn > 0 &&
        s.zsnap[s.zn - 1] == mh.win &&           /* topmost visible           */
        rn == 0 &&                               /* nothing else changed      */
        (mh.nx != mh.ox || mh.ny != mh.oy)) {
        fast = 1;
        bx0 = (mh.ox < mh.nx ? mh.ox : mh.nx) - 2;
        by0 = (mh.oy < mh.ny ? mh.oy : mh.ny) - 2;
        bx1 = (mh.ox > mh.nx ? mh.ox : mh.nx) + mh.w + 7;   /* +5 shadow, +2  */
        by1 = (mh.oy > mh.ny ? mh.oy : mh.ny) + mh.h + 7;
    } else if (mh.active && mh.win) {
        /* The slow path still has to REPAINT the move, so the rects the drag
         * did not add now go in. */
        int ax0 = (mh.ox < mh.nx ? mh.ox : mh.nx) - 2;
        int ay0 = (mh.oy < mh.ny ? mh.oy : mh.ny) - 2;
        int ax1 = (mh.ox > mh.nx ? mh.ox : mh.nx) + mh.w + 7;
        int ay1 = (mh.oy > mh.ny ? mh.oy : mh.ny) + mh.h + 7;
        if (rn < DMG_MAX) rl[rn++] = (struct rect){ ax0, ay0, ax1, ay1 };
    }

    /* 3. Cursor damage from HERE (M22.4): erase the last-drawn sprite and
     *    draw the fresh one — two SEPARATE small rects, appended to the
     *    list (not unioned with far-away window damage). */
    int cur_moved = (s.cx != last_cur_x || s.cy != last_cur_y);
    int prev_cur_x = last_cur_x, prev_cur_y = last_cur_y;
    if (rn == 0 && !cur_moved && !fast) return; /* spurious wake */
    if (cur_moved && rn < DMG_MAX + 2)
        rl[rn++] = (struct rect){ CUR_DMG_X(last_cur_x), CUR_DMG_Y(last_cur_y),
                                  CUR_DMG_X(last_cur_x) + CUR_DMG_W,
                                  CUR_DMG_Y(last_cur_y) + CUR_DMG_H };
    if (cur_moved && rn < DMG_MAX + 2)
        rl[rn++] = (struct rect){ CUR_DMG_X(s.cx), CUR_DMG_Y(s.cy),
                                  CUR_DMG_X(s.cx) + CUR_DMG_W,
                                  CUR_DMG_Y(s.cy) + CUR_DMG_H };
    last_cur_x = s.cx;  last_cur_y = s.cy;

    /* 3b. THE COPY, and the small list of things that still have to be
     *     painted around it.
     *
     * Order is load-bearing and there is only one correct one: COPY FIRST.
     * The regions that need painting — the strip the window vacated, the
     * cursor's old footprint — lie INSIDE the source rectangle, and the
     * painter draws the scene as it is NOW (window already at its new
     * position).  Painting any of them before the copy would feed the copy
     * pixels that belong to the new frame, and the window would carry a band
     * of wallpaper across the screen with it. */
    int copy_x = 0, copy_y = 0, copy_w = 0, copy_h = 0;   /* what got filled  */
    if (fast) {
        int shx = mh.nx - mh.ox, shy = mh.ny - mh.oy;
        /* The valid source window: on screen at BOTH ends, and above the
         * panel — the taskbar is composited over the windows, so a source row
         * inside the panel strip holds panel pixels, not the window's. */
        int l = mh.ox, r = mh.ox + mh.w, t = mh.oy, b = mh.oy + mh.h;
        if (l < 0) l = 0;
        if (l < -shx) l = -shx;
        if (t < 0) t = 0;
        if (t < -shy) t = -shy;
        if (r > fbsurf.w) r = fbsurf.w;
        if (r > fbsurf.w - shx) r = fbsurf.w - shx;
        if (b > work_h) b = work_h;
        if (b > work_h - shy) b = work_h - shy;

        if (r - l > 0 && b - t > 0) {
            gfx_move_within(&backsurf, l, t, l + shx, t + shy, r - l, b - t);
            copy_x = l + shx; copy_y = t + shy;
            copy_w = r - l;   copy_h = b - t;
        } else {
            fast = 0;                            /* nothing worth copying     */
        }
    }

    if (fast) drag_fast++;
    if (mh.active && !fast) drag_slow++;

    if (fast) {
        /* Everything in the union EXCEPT what the copy just filled, as up to
         * four rectangles: the vacated strips, the new shadow band, and any
         * part of the destination the clipping above could not supply. */
        struct rect keep = { copy_x, copy_y, copy_x + copy_w, copy_y + copy_h };
        if (by0 < keep.y0 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ bx0, by0, bx1, keep.y0 };
        if (keep.y1 < by1 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ bx0, keep.y1, bx1, by1 };
        if (bx0 < keep.x0 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ bx0, keep.y0, keep.x0, keep.y1 };
        if (keep.x1 < bx1 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ keep.x1, keep.y0, bx1, keep.y1 };

        /* THE CURSOR CAME ALONG FOR THE RIDE.  draw_cursor paints the sprite
         * INTO the back buffer, so the source rectangle had a cursor burned
         * into it and the copy has just deposited a second one at
         * (old cursor + the move).  It is a handful of pixels and it is inside
         * the window, so repainting that one small rectangle removes it. */
        int shx = mh.nx - mh.ox, shy = mh.ny - mh.oy;
        if (rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ CUR_DMG_X(prev_cur_x) + shx,
                                      CUR_DMG_Y(prev_cur_y) + shy,
                                      CUR_DMG_X(prev_cur_x) + shx + CUR_DMG_W,
                                      CUR_DMG_Y(prev_cur_y) + shy + CUR_DMG_H };
    }

    /* 4. Clamp each rect to the screen; drop empties. */
    struct rect fr[DMG_MAX + 2];
    int fn = 0, any_full = 0;
    for (int i = 0; i < rn; i++) {
        int x0 = rl[i].x0, y0 = rl[i].y0, x1 = rl[i].x1, y1 = rl[i].y1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fbsurf.w) x1 = fbsurf.w;
        if (y1 > fbsurf.h) y1 = fbsurf.h;
        if (x1 <= x0 || y1 <= y0) continue;
        fr[fn++] = (struct rect){ x0, y0, x1, y1 };
        if (x0 == 0 && y0 == 0 && x1 == fbsurf.w && y1 == fbsurf.h) any_full = 1;
    }
    /* The copied rectangle is not painted — but it MUST be presented, or the
     * work stays in the back buffer and the screen shows the window still at
     * its old place.  It goes in as a present-only entry, remembered by index
     * so the draw pass below can step over exactly it. */
    int skip_draw = -1;
    if (fast && copy_w > 0 && copy_h > 0 && fn < DMG_MAX + 2) {
        fr[fn] = (struct rect){ copy_x, copy_y, copy_x + copy_w, copy_y + copy_h };
        skip_draw = fn++;
    }

    if (fn == 0) return;
    if (any_full) frames_full++; else frames_partial++;
    for (int k = 0; k < fn; k++)                 /* actual damage this frame */
        total_blit_px += (uint64_t)(fr[k].x1 - fr[k].x0) * (fr[k].y1 - fr[k].y0);

    /* 5. Draw pass — paint each rect into backsurf.  The copied rectangle is
     *    skipped: its pixels are already correct, and repainting them is the
     *    entire cost this path exists to avoid. */
    for (int k = 0; k < fn; k++) {
        if (k == skip_draw) continue;
        draw_scene_rect(&s, fr[k].x0, fr[k].y0, fr[k].x1, fr[k].y1);
    }
    gfx_clear_clip(&backsurf);

    /* 6. Present — blit each rect (M22.6 flip has buffer-age 2, so it also
     *    replays LAST frame's rects into the hidden buffer to complete it). */
    if (flip_ok) {
        int hidden = flip_front ^ 1;
        for (int k = 0; k < fn; k++)
            gfx_blit(&flipbuf[hidden], fr[k].x0, fr[k].y0, &backsurf,
                     fr[k].x0, fr[k].y0, fr[k].x1 - fr[k].x0, fr[k].y1 - fr[k].y0);
        for (int k = 0; k < prev_dmg_n; k++)
            gfx_blit(&flipbuf[hidden], prev_dmg[k].x0, prev_dmg[k].y0, &backsurf,
                     prev_dmg[k].x0, prev_dmg[k].y0,
                     prev_dmg[k].x1 - prev_dmg[k].x0,
                     prev_dmg[k].y1 - prev_dmg[k].y0);
        fb_flip_to(hidden);
        flip_front = hidden;
        prev_dmg_n = fn;
        for (int k = 0; k < fn; k++) prev_dmg[k] = fr[k];
    } else {
        for (int k = 0; k < fn; k++) {
            gfx_blit(&fbsurf, fr[k].x0, fr[k].y0, &backsurf,
                     fr[k].x0, fr[k].y0, fr[k].x1 - fr[k].x0, fr[k].y1 - fr[k].y0);
            /* Push the freshly-blitted rect to the scanout.  No-op on x86 (the
             * linear FB is the scanout); on aarch64 this is the virtio-gpu
             * transfer+flush that makes the compositor visible. */
            fb_present_flush(fr[k].x0, fr[k].y0,
                             fr[k].x1 - fr[k].x0, fr[k].y1 - fr[k].y0);
        }
    }

    /* At the END, after the draw pass AND the present.  The first version of
     * this accumulated before them and reported 15 microseconds a frame for
     * megabytes of blitting — it was timing the bookkeeping, which is the
     * cheapest thing in the function.  A measurement placed on the wrong side
     * of the work does not merely understate it; it says the work is free. */
    uint64_t this_ns = timer_now_ns() - compose_t0;
    total_compose_ns += this_ns;

    /* PERIODIC COMPOSE REPORT (gui.stats_ms, 0 = off).
     *
     * `gui stats` has always been able to answer "what does a frame cost" — and
     * it has to be TYPED, which this project's own harness cannot do once a GUI
     * window holds the focus (§4.74).  So the one measurement anybody actually
     * wants, the cost of compositing a MAXIMIZED window, was the one that could
     * not be taken: reaching the state to be measured destroyed the means of
     * measuring it.
     *
     * A timed log has no such problem.  The worst frame is kept separately from
     * the mean because lag is not an average — a run of cheap frames around one
     * 40 ms frame reads as comfortable and feels like a stutter. */
    static uint64_t rep_next_ms, rep_ns, rep_worst_ns;
    static unsigned rep_frames, rep_px, rep_worst_px, rep_worst_rects;
    /* RE-READ PERIODICALLY, never latched once.  The first version cached the
     * key on the first composite — and the GUI autostarts at boot, so the value
     * was read before anything could set it and the report could never be
     * turned on at all.  `setconf` also does not fire §M63's watchers (only
     * config_apply does), so a watcher would not have helped either. */
    /* EVERY frame, not every 64th.  The first attempt polled every 64 composites
     * to keep a config lookup off the hot path — but an idle desktop composites
     * about once a second, so switching the report on took a minute and looked
     * exactly like the feature not working.  A string lookup against a small
     * table is noise next to the megabytes of blitting above it; the
     * "optimisation" cost more than it saved and hid its own effect. */
    int rep_every = (int)config_get_long("gui.stats_ms", 0);
    if (rep_every > 0) {
        rep_frames++;
        rep_ns += this_ns;
        /* AREA, NOT JUST TIME.  A slow frame is either big or fixed-cost, and
         * those want opposite fixes — narrowing the damage helps the first and
         * does nothing for the second.  Without the pixel count the two are
         * indistinguishable in the log, which is how the last two attempts at
         * this were each aimed at a guess. */
        unsigned fpx = 0;
        for (int k = 0; k < fn; k++)
            fpx += (unsigned)((fr[k].x1 - fr[k].x0) * (fr[k].y1 - fr[k].y0));
        rep_px += fpx;
        if (this_ns > rep_worst_ns) {
            rep_worst_ns = this_ns;
            rep_worst_px = fpx;
            rep_worst_rects = (unsigned)fn;
        }
        uint64_t now = timer_ticks_ms();
        if (!rep_next_ms) rep_next_ms = now + (unsigned)rep_every;
        else if (now >= rep_next_ms) {
            kprintf("gui: %u frames in %u ms - %u us mean (%u kpx), "
                    "worst %u us over %u kpx in %u rect(s)\n",
                    rep_frames, (unsigned)rep_every,
                    rep_frames ? (unsigned)(rep_ns / rep_frames / 1000) : 0u,
                    rep_frames ? rep_px / rep_frames / 1000 : 0u,
                    (unsigned)(rep_worst_ns / 1000),
                    rep_worst_px / 1000, rep_worst_rects);
            rep_frames = 0; rep_ns = 0; rep_worst_ns = 0;
            rep_px = 0; rep_worst_px = 0; rep_worst_rects = 0;
            rep_next_ms = now + (unsigned)rep_every;
        }
    }
}
/* §M60 — fill the wallpaper surface from the configured source and stamp the
 * milestone label on top.  The label is drawn HERE rather than by wallpaper.c
 * on purpose: it is desktop chrome that must survive every source (a picture
 * must not swallow the version string), and its position depends on `work_h`,
 * which is the compositor's business and not the background's. */
int paint_wallpaper(void) {
    int rc = wallpaper_render(&wallsurf);

    /* Desktop milestone label — sizes itself to the string so any DOS_MILESTONE
     * length stays right-aligned (see kernel/includes/version.h). */
    int lbl_w = 0; for (const char* p = DOS_LABEL; *p; p++) lbl_w++;
    int lx = wallsurf.w - lbl_w * GFX_GLYPH_W - 12;
    int ly = work_h - GFX_GLYPH_H - 8;
    /* A photograph can be any colour under the text, so give the label its own
     * dim backing rather than trusting contrast that the gradient guaranteed
     * and an arbitrary image does not. */
    gfx_blend_fill(&wallsurf, lx - 6, ly - 4,
                   lbl_w * GFX_GLYPH_W + 12, GFX_GLYPH_H + 8, 0x80101820u);
    gfx_text(&wallsurf, lx, ly, DOS_LABEL, 0xFF9FB6C9u);
    return rc;
}
