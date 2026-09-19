/* =============================================================================
 * w_box.c — the container, as a widget (§M81 step 1).
 *
 * See widget.h's `struct w_box` for WHY.  The short version: the toolkit had
 * TWO hierarchies for one window — an `ui_node` tree that knew about nesting
 * and did the layout, and a flat `struct widget` list that did the drawing and
 * the hit-testing.  A container lived only in the first, so it could neither
 * draw nor be pressed, and both of those had to be bolted on from outside:
 * `ui_draw_overlay()` called from both window redraw paths, and
 * `ui_pointer_at()` asked before the widget lookup with a hand-rolled grab
 * latch of its own.
 *
 * Every line in this file used to live in ui.c under one of those two names.
 * What changed is not the arithmetic — the scrollbar's metrics still come from
 * scrollbar.c, unchanged — but WHO IS ASKED: the box is in the window's widget
 * list now, so `widget_draw_all` draws it and `win->grabw` grabs it, on exactly
 * the same terms as a list or a slider.
 *
 * THE LATCH IS GONE, AND THAT IS THE CLEAREST RECEIPT.  `ui_pointer_at` kept
 * `sb_node` plus a rule making it self-healing, because a release that never
 * arrives must not leave a container consuming every later event (§M69 shipped
 * that fix after it was reported as *"I click and nothing happens, then
 * suddenly it works"*).  `win->grabw` already has that property for every other
 * widget — a press overwrites it, a release clears it — so the second copy is
 * deleted rather than ported.  *A mechanism that exists twice is a mechanism
 * that can disagree with itself.*
 *
 * THREADING: everything here runs on the owning app-host task, like the rest of
 * the toolkit.  Nothing may be called from an IRQ.
 * ============================================================================= */

#include "widget.h"
#include "ui.h"
#include "gui.h"
#include "gfx.h"
#include "scrollbar.h"
#include "console_plate.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>

/* The bar, in the box's own (viewport-relative) coordinates.  ONE definition
 * for the painter, the hit test and the damage rect — the shape §4.79 paid for
 * in the title buttons, where the drawn box and the pressable box had been
 * computed separately and the top edge of every button was dead. */
static void box_bar(const struct w_box* b, struct sb_metrics* m) {
    int bw = cp_scrollbar_w();
    sb_metrics(m, b->vx + b->vw - bw, b->vy, bw, b->vh,
               b->content_h, b->vh, b->scroll);
}

static int box_scrolls(const struct w_box* b) {
    return (b->flags & UI_SCROLL) && b->vh > 0 && b->content_h > b->vh;
}

/* ---------------------------------------------------------------------------
 * Geometry, handed down by the layout.
 * ------------------------------------------------------------------------- */

void w_box_placed(struct widget* w, int x, int y, int cw, int ch, int content_h) {
    struct w_box* b = (struct w_box*)w;
    if (!b) return;
    b->vx = x; b->vy = y; b->vw = cw; b->vh = ch;
    b->content_h = content_h;

    int max = content_h - ch;
    if (max < 0) max = 0;
    if (b->scroll > max) b->scroll = max;
    if (b->scroll < 0)   b->scroll = 0;

    /* THE WIDGET RECT IS THE BAR, OR NOTHING.  A container with nothing to
     * scroll must be invisible to both the draw loop and the hit test, or it
     * would swallow presses that belong to the window underneath its gaps —
     * see widget.h for what that costs. */
    if (box_scrolls(b)) {
        struct sb_metrics m;
        box_bar(b, &m);
        w->x = m.x; w->y = m.y; w->w = m.w; w->h = m.h;
    } else {
        w->x = x; w->y = y; w->w = 0; w->h = 0;
        b->sb_part = 0;
    }
}

int w_box_scroll(const struct widget* w) {
    return w ? ((const struct w_box*)w)->scroll : 0;
}

int w_box_content(const struct widget* w) {
    return w ? ((const struct w_box*)w)->content_h : 0;
}

/* Damage the VIEWPORT — what moved — rather than the window.  The bar lives
 * inside it (the layout reserves the strip from the children's width), so one
 * rect covers both. */
static void box_damage_viewport(struct w_box* b) {
    gui_window_request_redraw_rect(b->base.win, b->vx, b->vy, b->vw, b->vh);
}

/* Damage the BAR alone: a sixteen-pixel strip against the viewport's quarter of
 * a megapixel.  What changes when a press latches or a release lets go is which
 * part is drawn held, and nothing under it moves. */
static void box_damage_bar(struct w_box* b) {
    struct sb_metrics m;
    box_bar(b, &m);
    gui_window_request_redraw_rect(b->base.win, m.x, m.y, m.w, m.h);
}

int w_box_scroll_by(struct widget* w, int dl) {
    struct w_box* b = (struct w_box*)w;
    if (!b || !(b->flags & UI_SCROLL)) return 0;
    int before = b->scroll;
    b->scroll += dl;
    if (b->scroll < 0) b->scroll = 0;
    /* Re-ARRANGE, not re-layout: a scroll changes no SIZE, and measuring a
     * label costs a catalogue lookup plus a per-glyph width.  ui_reflow also
     * clamps the scroll against the current viewport, which is why the upper
     * bound is not applied here. */
    ui_reflow(b->base.win);
    if (b->scroll == before) return 0;
    box_damage_viewport(b);
    return 1;
}

/* ---------------------------------------------------------------------------
 * The ops.
 * ------------------------------------------------------------------------- */

static void box_draw(struct widget* w, struct gfx_surface* s) {
    struct w_box* b = (struct w_box*)w;
    if (!box_scrolls(b)) return;
    struct sb_metrics m;
    box_bar(b, &m);
    sb_draw(s, &m, b->sb_part);
}

/* Press / drag / release on the bar.
 *
 * The host has already decided this event is ours — `widget_at` only returns a
 * box where its rect is the bar — so there is no containment test to repeat
 * here, and no latch to keep: `win->grabw` holds the box for the whole gesture
 * because this op exists. */
static int box_pointer(struct widget* w, int lx, int ly, int phase) {
    struct w_box* b = (struct w_box*)w;
    if (!box_scrolls(b)) return WH_IGNORED;

    /* The host hands out widget-LOCAL coordinates; the metrics are in window
     * coordinates, which is what the rest of the toolkit uses. */
    int x = lx + w->x, y = ly + w->y;

    struct sb_metrics m;
    box_bar(b, &m);

    if (phase == WPTR_RELEASE) {
        if (!b->sb_part) return WH_IGNORED;
        b->sb_part = 0;
        box_damage_bar(b);              /* the held highlight, and no more */
        return WH_DAMAGED;
    }

    if (phase == WPTR_DRAG) {
        if (b->sb_part != SB_THUMB) return WH_DAMAGED;   /* ours, nothing to do */
        int ns = sb_scroll_from_thumb(&m, b->content_h, b->vh, y - b->sb_grab_dy);
        if (ns != b->scroll) {
            b->scroll = ns;
            ui_reflow(b->base.win);
            box_damage_viewport(b);
        }
        return WH_DAMAGED;
    }

    /* PRESS. */
    int part = sb_hit(&m, x, y);
    /* §M69's probe, carried over rather than lost with `ui_pointer_at`.  It was
     * added after a round of diagnosis went the wrong way: *"I cannot grab the
     * bar"* has two causes — the press never arrived, or it arrived and matched
     * nothing — and they live in different files.  One line separates them, and
     * the bar's measured rect is what caught the real fault last time (a 16 px
     * target with 16 px arrows, too small to aim at). */
    if (gui_input_debug())
        kprintf("box: press at %d,%d over bar %d,%d %dx%d part=%d "
                "content=%d scroll=%d\n",
                x, y, m.x, m.y, m.w, m.h, part, b->content_h, b->scroll);
    if (part == SB_NONE) return WH_IGNORED;

    b->sb_part    = part;
    b->sb_grab_dy = y - m.thumb_y;

    int was = b->scroll, step = cp_row_h();
    switch (part) {
    case SB_UP:          b->scroll -= step;            break;
    case SB_DOWN:        b->scroll += step;            break;
    case SB_TROUGH_UP:   b->scroll -= sb_page(b->vh);  break;
    case SB_TROUGH_DOWN: b->scroll += sb_page(b->vh);  break;
    default: break;                                     /* SB_THUMB: grab only */
    }
    int max = b->content_h - b->vh;
    if (b->scroll < 0)   b->scroll = 0;
    if (b->scroll > max) b->scroll = max;

    /* The viewport when the content moved; otherwise only the bar, whose
     * pressed part is now drawn held — an arrow at the end of its travel has to
     * answer the click without repainting anything under it. */
    if (b->scroll != was) { ui_reflow(b->base.win); box_damage_viewport(b); }
    else                  { box_damage_bar(b); }
    return WH_DAMAGED;
}

/* A wheel notch delivered straight to the bar.  Over the CONTENT the notch
 * reaches a child first and `ui_scroll_at` walks out to us — a flat widget list
 * has no parent link, which is the one thing about the old node tree this step
 * does not replace. */
static int box_scroll_op(struct widget* w, int dz) {
    if (!box_scrolls((struct w_box*)w)) return 0;
    return w_box_scroll_by(w, dz > 0 ? -ui_wheel_step() : ui_wheel_step());
}

static const struct widget_ops box_ops = {
    .draw = box_draw, .pointer = box_pointer, .scroll = box_scroll_op,
};

/* ---------------------------------------------------------------------------
 * The class.
 * ------------------------------------------------------------------------- */

static struct widget* box_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_box* b = (struct w_box*)kcalloc(1, sizeof *b);
    if (!b) return NULL;
    b->flags = sp ? sp->flags : 0;
    /* Zero-sized and unfocusable: it takes part in the draw loop and the hit
     * test only once the layout gives it a bar. */
    widget_init(&b->base, win, 0, 0, 0, 0, &box_ops, NULL, 0);
    return &b->base;
}

/* NO `measure` OP, deliberately.  ui.c measures a container from its children
 * (a row sums along its axis, a grid aligns its first column, a viewport asks
 * for what it is given) and that arithmetic is the layout's, not the widget's.
 * A measure here would be a second answer to one question. */
WIDGET_CLASS(cls_box) = {
    .ops = &box_ops,
    .name = "box",
    .create = box_create,
};
