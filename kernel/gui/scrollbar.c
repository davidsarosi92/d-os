/* =============================================================================
 * scrollbar.c — the one scrollbar (§M69).  See scrollbar.h for why.
 * ========================================================================= */

#include "scrollbar.h"
#include "console_plate.h"
#include "gfx.h"
#include <stddef.h>

/* An arrow box is square, so its height is the bar's width — and it only
 * appears if what remains is still a usable track.  A bar shorter than that
 * gets no arrows rather than three touching boxes: *a control too small to aim
 * at is not a smaller control, it is a different and worse one.* */
static int arrow_h_for(int w, int h) {
    /* TALLER THAN IT IS WIDE.  A square arrow box is as small as the bar, and
     * the bar is narrow by design; height is the axis with room to spare, so
     * spending it here costs the track a little and makes the button
     * reliably hittable.  Reported from use: *"clicking the arrow works
     * sometimes."*  Half again is enough to feel deliberate without eating a
     * short track. */
    int a = w + w / 2;
    if (a < 6) return 0;
    if (h - 2 * a < CP_SCROLLBAR_THUMB_MIN) {
        a = w;                              /* short bar: square, or none */
        if (h - 2 * a < CP_SCROLLBAR_THUMB_MIN) return 0;
    }
    return a;
}

void sb_metrics(struct sb_metrics* m, int x, int y, int w, int h,
                int content, int viewport, int scroll) {
    if (!m) return;
    m->x = x; m->y = y; m->w = w; m->h = h;
    m->arrow = arrow_h_for(w, h);
    m->track_y = y + m->arrow + CP_SCROLLBAR_INSET;
    m->track_h = h - 2 * m->arrow - 2 * CP_SCROLLBAR_INSET;
    if (m->track_h < 0) m->track_h = 0;

    m->active = (content > viewport && viewport > 0);
    if (!m->active) {
        /* Full-length thumb: the honest picture of "all of it is on screen",
         * and it means every caller can paint unconditionally. */
        m->thumb_y = m->track_y;
        m->thumb_h = m->track_h;
        return;
    }

    int th = (int)(((long)m->track_h * viewport) / content);
    if (th < CP_SCROLLBAR_THUMB_MIN) th = CP_SCROLLBAR_THUMB_MIN;
    if (th > m->track_h) th = m->track_h;

    int span = content - viewport;                  /* > 0 here */
    if (scroll < 0) scroll = 0;
    if (scroll > span) scroll = span;
    m->thumb_h = th;
    m->thumb_y = m->track_y + (int)(((long)(m->track_h - th) * scroll) / span);
}

/* A triangle, drawn as a stack of centred rows — integer only (§A2), and the
 * same shape whichever way it points.  Not a font glyph: an arrowhead from an
 * 8x8 bitmap is a CHARACTER and reads as one next to geometric marks, which is
 * the argument §M64 already made for the window buttons. */
static void arrow_glyph(struct gfx_surface* s, int cx, int cy, int size,
                        int up, cp_color col) {
    int half = size / 2;
    if (half < 1) return;
    for (int i = 0; i < half; i++) {
        int wdt = 2 * i + 1;
        int yy = up ? cy - half / 2 + i : cy + half / 2 - i;
        gfx_fill(s, cx - i, yy, wdt, 1, col);
    }
}

void sb_draw(struct gfx_surface* s, const struct sb_metrics* m, int hot) {
    if (!s || !m || m->w <= 0 || m->h <= 0) return;
    const cp_theme* t = cp_current_theme();

    gfx_fill(s, m->x, m->y, m->w, m->h, t->tray);
    /* The design's 1 px separator down the LEFT edge (§10) — it is what stops
     * the bar reading as part of the list's last column. */
    gfx_fill(s, m->x, m->y, 1, m->h, t->line_soft);

    if (m->arrow > 0) {
        int size = m->w - 2 * CP_SCROLLBAR_INSET;
        /* An arrow that cannot move anything is DIMMED rather than hidden:
         * a control that disappears at the end of its range moves everything
         * next to it, and the user loses the target they were aiming at. */
        cp_color up_c   = (hot == SB_UP)   ? t->text : t->muted;
        cp_color down_c = (hot == SB_DOWN) ? t->text : t->muted;
        if (!m->active) up_c = down_c = t->line;

        if (hot == SB_UP)
            gfx_fill(s, m->x + 1, m->y, m->w - 1, m->arrow, t->hover);
        if (hot == SB_DOWN)
            gfx_fill(s, m->x + 1, m->y + m->h - m->arrow, m->w - 1, m->arrow,
                     t->hover);
        arrow_glyph(s, m->x + m->w / 2, m->y + m->arrow / 2, size, 1, up_c);
        arrow_glyph(s, m->x + m->w / 2, m->y + m->h - m->arrow / 2, size, 0,
                    down_c);
    }

    if (m->thumb_h <= 0) return;
    /* Radius 4 per §10; cp_fill_round draws it as the design's own 1 px
     * staircase, so this is the specified fallback rather than something
     * invented at draw time. */
    cp_fill_round(s, m->x + CP_SCROLLBAR_INSET, m->thumb_y,
                  m->w - 2 * CP_SCROLLBAR_INSET, m->thumb_h, cp_px(4),
                  (hot == SB_THUMB) ? t->text : t->muted);
}

int sb_hit(const struct sb_metrics* m, int px, int py) {
    if (!m) return SB_NONE;
    if (px < m->x || px >= m->x + m->w) return SB_NONE;
    if (py < m->y || py >= m->y + m->h) return SB_NONE;
    if (m->arrow > 0) {
        if (py < m->y + m->arrow)            return SB_UP;
        if (py >= m->y + m->h - m->arrow)    return SB_DOWN;
    }
    if (py < m->thumb_y)                     return SB_TROUGH_UP;
    if (py >= m->thumb_y + m->thumb_h)       return SB_TROUGH_DOWN;
    return SB_THUMB;
}

int sb_scroll_from_thumb(const struct sb_metrics* m, int content, int viewport,
                         int thumb_top) {
    if (!m || !m->active) return 0;
    int room = m->track_h - m->thumb_h;
    int span = content - viewport;
    if (room <= 0 || span <= 0) return 0;
    int off = thumb_top - m->track_y;
    if (off < 0) off = 0;
    if (off > room) off = room;
    /* Rounded, not truncated: with a long list one thumb pixel is many rows,
     * and truncation makes the thumb drift upward relative to the pointer
     * across a drag — a small error that accumulates in one direction is the
     * one a user notices. */
    return (int)(((long)off * span + room / 2) / room);
}

int sb_page(int viewport) {
    int p = viewport - 1;
    return p < 1 ? 1 : p;
}
