/* =============================================================================
 * scrollbar.h — ONE scrollbar: geometry, painting, hit-testing (§M69).
 *
 * WHY THIS EXISTS.  There were FOUR copies of the same arithmetic — the
 * listview's, the table view's, the item-view's and the UI_SCROLL overlay's —
 * each computing a proportional thumb from `track * viewport / content` and
 * each stopping there.  None could be dragged, none answered a click on the
 * trough, and none had a way to step.  *Four implementations of a control and
 * not one of them was a control*: they were indicators drawn to look like one,
 * which is worse than no scrollbar at all, because the user reaches for it.
 *
 * Reported from use, and all three parts of the report are true: there is
 * nothing to step with, the wheel does nothing over the bar, and pressing the
 * thumb and holding does not drag it.
 *
 * ONE DELIBERATE DIVERGENCE FROM THE DESIGN, asked for explicitly.
 * widget_specs.md §10 says *"Nincs nyíl-gomb a végeken"* — no arrow buttons at
 * the ends — and this draws them.  Written down here rather than left to be
 * found by somebody holding our screen against the catalogue: it is a decision,
 * not drift.  The design's own trough behaviour ("kattintás a vályúra → a
 * hüvely középpontja odaugrik, majd fogás") is implemented as specified, and
 * the arrows sit outside the track so the thumb's travel still maps linearly
 * onto the scroll range.
 *
 * UNITS ARE THE CALLER'S.  `content`, `viewport` and `scroll` may be ROWS (the
 * listview, the table) or PIXELS (a scrolling container) — every formula here
 * is a ratio, so the two work identically and neither has to convert.  What
 * they may NOT do is mix the three.
 * ========================================================================= */

#ifndef SCROLLBAR_H
#define SCROLLBAR_H

#include <stdint.h>

struct gfx_surface;

/* Which part of the bar a point is over.  SB_NONE means "not on the bar at
 * all", which is a different answer from "on the bar, between the thumb and an
 * end" — the caller pages on the second and ignores the first. */
enum sb_part {
    SB_NONE = 0,
    SB_UP,                  /* the step-up arrow                            */
    SB_DOWN,                /* the step-down arrow                          */
    SB_THUMB,               /* grab starts here                             */
    SB_TROUGH_UP,           /* trough above the thumb → page up             */
    SB_TROUGH_DOWN,         /* trough below the thumb → page down           */
};

/* Everything the painter and the hit test both need, computed ONCE.  They used
 * to derive it separately in each widget, which is the shape §4.79 paid for in
 * the title buttons: the drawn box and the pressable box were not the same. */
struct sb_metrics {
    int  x, y, w, h;            /* the whole bar                            */
    int  arrow;                 /* arrow box height; 0 = no room for them   */
    int  track_y, track_h;      /* the trough, between the arrows           */
    int  thumb_y, thumb_h;      /* 0 height = nothing to scroll             */
    int  active;                /* non-zero when content > viewport         */
};

/* Fill `m` for a bar occupying (x,y,w,h).  Safe with degenerate input: a
 * viewport at least as big as the content yields `active == 0` and a thumb
 * filling the track, so a caller may draw it without a special case. */
void sb_metrics(struct sb_metrics* m, int x, int y, int w, int h,
                int content, int viewport, int scroll);

/* Paint trough, arrows and thumb.  `hot` is the part under the pointer (or
 * being dragged) so it can be emphasised — SB_NONE for none. */
void sb_draw(struct gfx_surface* s, const struct sb_metrics* m, int hot);

/* Which part is at (px, py), in the same coordinates the metrics were built
 * in.  SB_NONE when the point is outside the bar. */
int sb_hit(const struct sb_metrics* m, int px, int py);

/* The scroll value that puts the thumb's TOP at `thumb_top`, clamped into
 * range.  This is the drag: the caller records the offset between the press
 * and the thumb's top, then feeds `py - offset` here on every motion.
 *
 * The offset matters and is the caller's to keep: without it the thumb jumps
 * so its top meets the pointer on the first motion event, which reads as the
 * list lurching the moment you touch it. */
int sb_scroll_from_thumb(const struct sb_metrics* m, int content, int viewport,
                         int thumb_top);

/* One step and one page, in the caller's units.  A page is the viewport minus
 * one unit of overlap, floored at 1 — scrolling by a whole viewport leaves the
 * reader with no line in common between the two screens, which is how a pager
 * loses somebody's place. */
int sb_page(int viewport);

#endif /* SCROLLBAR_H */
