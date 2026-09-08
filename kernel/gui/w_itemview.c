/* =============================================================================
 * w_itemview.c — the window-side half of the item view (§M63).
 *
 * itemview.h keeps the LAYOUTS stateless on purpose: selection and scroll
 * belong to the thing being looked at, not to the way it is arranged.  This
 * widget is one such thing — a window's view of a model.  The desktop is
 * another, and it holds its own selection without going through here, which is
 * exactly why the view API takes a surface and an origin instead of a window.
 *
 * Keyboard navigation lives here for the same reason: arrows and Enter move a
 * SELECTION, and the selection is the widget's, so a new layout never has to
 * reimplement them.
 * ============================================================================= */

#include "widget.h"
#include "console_plate.h"
#include "scrollbar.h"
#include "ui.h"
#include "config.h"
#include "printf.h"
#include "itemview.h"
#include "gfx.h"
#include "gui.h"
#include "keymap.h"
#include "kmalloc.h"
#include <stddef.h>

/* ---- precise damage + the content diff ------------------------------------
 *
 * Everything below exists because this widget used to answer every event with
 * `gui_window_request_redraw(w->win)` — the WHOLE window, chrome included, for
 * a selection moving one row.  That was affordable only while the app host
 * repainted the window anyway; once it stopped (gui.h: a tick damages what it
 * changed), a view that does not damage itself is a view that stops updating.
 */

/* One slot's damage rect, in window coordinates.  Asked of the VIEW — `rect()`
 * already knows where an item lands, so this works for a grid without knowing
 * anything about rows or columns, which is the point of swappable layouts.
 * A view without `rect` gets the whole widget: honest, and there is none. */
static void iv_damage_item(struct w_itemview* iv, int i) {
    struct widget* w = &iv->base;
    int x, y, ww, hh;
    if (i < 0) return;
    if (!iv->view || !iv->view->rect ||
        iv->view->rect(i, w->w, w->h, iv->model, iv->scroll, &x, &y, &ww, &hh) != 0)
        return;
    if (y >= w->h || y + hh <= 0) return;              /* scrolled out of sight */
    if (y < 0) { hh += y; y = 0; }
    if (y + hh > w->h) hh = w->h - y;
    if (hh <= 0) return;
    gui_window_request_redraw_rect(w->win, w->x + x, w->y + y, ww, hh);
}

static void iv_damage_all(struct w_itemview* iv) {
    struct widget* w = &iv->base;
    gui_window_request_redraw_rect(w->win, w->x, w->y, w->w, w->h);
}

/* What is drawn in the slot showing item `i`, as one string.
 *
 * The leading byte is the SELECTION, which is what makes one comparison cover
 * both "the numbers moved" and "another row is chosen" — see widget.h. */
static void iv_signature(struct w_itemview* iv, int i, char* out, int cap) {
    int p = 0;
    out[p++] = (i == iv->sel) ? '>' : ' ';
    int cols = 1;
    if (iv->model && iv->model->columns && iv->model->cell) {
        cols = iv->model->columns(iv->model->ctx);
        if (cols < 1) cols = 1;
        if (cols > 8) cols = 8;
    }
    for (int c = 0; c < cols && p < cap - 2; c++) {
        char buf[64];
        item_cell_text(iv->model, i, c, buf, (int)sizeof buf);
        for (int k = 0; buf[k] && p < cap - 2; k++) out[p++] = buf[k];
        out[p++] = '\t';                     /* so "ab|c" and "a|bc" differ */
    }
    out[p] = 0;
}

static int iv_sig_differs(const char* a, const char* b) {
    for (int i = 0; i < IV_SIG_LEN; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

void w_itemview_refresh(struct w_itemview* iv) {
    if (!iv || !iv->model || !iv->model->count) return;
    struct widget* w = &iv->base;
    int total = iv->model->count(iv->model->ctx);
    int slots = (iv->view && iv->view->page) ? iv->view->page(w->w, w->h) : 1;
    if (slots > IV_SLOTS) slots = IV_SLOTS;
    if (slots < 0) slots = 0;

    /* A SCROLL MOVED EVERY SLOT, and so did a first draw or a resize.  Repaint
     * the pane once and re-take the whole signature: reporting thirty separate
     * changed slots would be true and would cost more than the one rect they
     * add up to. */
    if (!iv->sig_valid || iv->scroll != iv->sig_scroll || slots != iv->sig_slots) {
        for (int r = 0; r < slots; r++) {
            int i = iv->scroll + r;
            if (i < total) iv_signature(iv, i, iv->rowsig[r], IV_SIG_LEN);
            else           iv->rowsig[r][0] = 0;
        }
        iv->sig_slots = slots;
        iv->sig_scroll = iv->scroll;
        iv->sig_valid = 1;
        iv_damage_all(iv);
        return;
    }

    for (int r = 0; r < slots; r++) {
        int i = iv->scroll + r;
        char sig[IV_SIG_LEN];
        if (i < total) iv_signature(iv, i, sig, IV_SIG_LEN);
        else           sig[0] = 0;
        if (!iv_sig_differs(sig, iv->rowsig[r])) continue;

        for (int k = 0; k < IV_SIG_LEN; k++) {
            iv->rowsig[r][k] = sig[k];
            if (!sig[k]) break;
        }
        /* A SLOT THAT LOST ITS ITEM still has to be painted over: the old row
         * is sitting there and nothing else will ever cover it.  `rect` is
         * asked for the index that WOULD live there, which every view computes
         * from the slot rather than from the item. */
        iv_damage_item(iv, i);
    }
}

static int iv_sb(struct w_itemview* iv, struct sb_metrics* m);
static int iv_scroll_max(struct w_itemview* iv, int n);

static void iv_draw(struct widget* w, struct gfx_surface* s) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view || !iv->view->draw) return;

    /* A quiet backdrop so the items read as a pane rather than as marks on the
     * window background — and so the selection wash has something to sit on. */
    /* §M69 — the theme's list surface, not a hardcoded dark blue.  This one
     * literal is what made the file manager's pane stay dark under the light
     * theme — *"the colours are not in harmony, the blue is very telling."* */
    gfx_fill(s, w->x, w->y, w->w, w->h, cp_current_theme()->sunken);
    iv->view->draw(s, w->x, w->y, w->w, w->h, iv->model, iv->sel, iv->scroll);

    /* §M69 — and the scrollbar, painted by the WIDGET.  The view reserved the
     * strip and told us where it is; the widget owns `scroll` and the grab, so
     * it owns the only state a usable bar has (which part is held). */
    struct sb_metrics m;
    if (iv_sb(iv, &m)) sb_draw(s, &m, iv->sb_part);
}

/* THE SCROLL CEILING, in the view's own unit.
 *
 * Three answers, and conflating the last two is the bug this exists to stop:
 *   - the view has no `.scrollbar` op   → it never told us; fall back to items
 *   - the op says "everything fits"     → the ceiling is ZERO.  Nothing may
 *                                         scroll, and the wheel must do
 *                                         literally nothing.
 *   - the op reports a range            → content - viewport
 *
 * The Control Panel showed what the middle case costs: everything fitted, the
 * view drew no bar, and the wheel still scrolled — using an ITEM count as if
 * it were a ROW count — so most of the icons left the screen with nothing on
 * screen able to bring them back. */
static int iv_scroll_max(struct w_itemview* iv, int n) {
    if (!iv->view || !iv->view->scrollbar) return n > 0 ? n - 1 : 0;
    struct sb_metrics m;
    if (!iv_sb(iv, &m)) return 0;              /* it fits — do not move */
    int max = iv->sb_content - iv->sb_viewport;
    return max > 0 ? max : 0;
}

/* The bar's metrics in WINDOW coordinates, or 0 if this view has none.  One
 * definition for the painter and every input path — §4.79's lesson, where the
 * drawn box and the pressable box were computed apart and disagreed. */
static int iv_sb(struct w_itemview* iv, struct sb_metrics* m) {
    struct widget* w = &iv->base;
    if (!iv->view || !iv->view->scrollbar) return 0;
    int bx, by, bw, bh, content, viewport;
    if (!iv->view->scrollbar(w->w, w->h, iv->model, iv->scroll,
                             &bx, &by, &bw, &bh, &content, &viewport))
        return 0;
    sb_metrics(m, w->x + bx, w->y + by, bw, bh, content, viewport, iv->scroll);
    iv->sb_content = content;
    iv->sb_viewport = viewport;
    return 1;
}

/* §M61 fix — pointer DRAG past the top or bottom edge scrolls, which is the
 * other half of "scroll works": with no wheel, dragging is how a mouse user
 * reaches an item that is not on screen. */
static void iv_pointer(struct widget* w, int lx, int ly, int phase) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view) return;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return;
    int px = w->x + lx, py = w->y + ly;
    struct sb_metrics m;
    int has_sb = iv_sb(iv, &m);

    /* §M69 — THE SCROLLBAR GETS THE STREAM FIRST, and once a press has landed
     * on it the whole gesture belongs to it: gui.c grabs the pointer for this
     * widget, so without the `sb_part` latch a drag that wandered a few pixels
     * off the twelve-pixel-wide bar would start selecting rows instead. */
    if (phase == WPTR_PRESS && gui_input_debug())
        kprintf("iv: press at %d,%d  bar=%s part=%d scroll=%d/%d\n", px, py,
                has_sb ? "yes" : "NO", has_sb ? sb_hit(&m, px, py) : -1,
                iv->scroll, iv->sb_content);
    /* §M69 — stale-latch recovery; see ui.c. */
    if (phase == WPTR_PRESS) iv->sb_part = 0;

    if (phase == WPTR_PRESS && has_sb) {
        int part = sb_hit(&m, px, py);
        if (part != SB_NONE) {
            iv->sb_part = part;
            iv->sb_grab_dy = py - m.thumb_y;
            int was = iv->scroll;
            int max = iv->sb_content - iv->sb_viewport;
            if (max < 0) max = 0;
            switch (part) {
            case SB_UP:          iv->scroll--; break;
            case SB_DOWN:        iv->scroll++; break;
            case SB_TROUGH_UP:   iv->scroll -= sb_page(iv->sb_viewport); break;
            case SB_TROUGH_DOWN: iv->scroll += sb_page(iv->sb_viewport); break;
            default: break;                       /* SB_THUMB: grab only */
            }
            if (iv->scroll < 0)   iv->scroll = 0;
            if (iv->scroll > max) iv->scroll = max;
            if (iv->scroll != was) w_itemview_refresh(iv);
            else gui_window_request_redraw(w->win);   /* the held emphasis */
            return;
        }
    }

    if (phase == WPTR_DRAG && iv->sb_part) {
        if (iv->sb_part != SB_THUMB) return;      /* an arrow steps once */
        if (!has_sb) return;
        int ns = sb_scroll_from_thumb(&m, iv->sb_content, iv->sb_viewport,
                                      py - iv->sb_grab_dy);
        if (ns != iv->scroll) { iv->scroll = ns; w_itemview_refresh(iv); }
        return;
    }

    if (phase == WPTR_RELEASE) {
        if (iv->sb_part) { iv->sb_part = 0; gui_window_request_redraw(w->win); }
        return;
    }

    if (phase != WPTR_DRAG) return;

    /* §M61 — dragging past an edge scrolls, which is the other half of "scroll
     * works" for a mouse with no wheel. */
    int max = iv_scroll_max(iv, n);
    if (ly < 0 && iv->scroll > 0)             iv->scroll--;
    else if (ly > w->h && iv->scroll < max)   iv->scroll++;
    else {
        int idx = iv->view->hit(lx, ly, w->w, w->h, iv->model, iv->scroll);
        if (idx >= 0) iv->sel = idx;
    }
    w_itemview_refresh(iv);       /* the diff covers both: a scroll and a move */
}

static void iv_mouse(struct widget* w, int lx, int ly, int kind) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view || !iv->view->hit) return;
    gui_window_focus_widget(w->win, w);

    /* §M69 — a click on the bar is the pointer path's; it must not also pick
     * the row that happens to lie behind it. */
    {
        struct sb_metrics m;
        if (iv_sb(iv, &m) && sb_hit(&m, w->x + lx, w->y + ly) != SB_NONE) return;
    }

    int idx = iv->view->hit(lx, ly, w->w, w->h, iv->model, iv->scroll);
    if (idx >= 0) iv->sel = idx;
    /* The selection is part of the signature, so the diff repaints the row that
     * lost the highlight and the row that took it — two rows, not a window. */
    w_itemview_refresh(iv);
    if (iv->on_select) iv->on_select(iv, idx, w->ctx);

    /* kind 1 = double click → activate.  Single click only selects: a settings
     * category that opened on one click would fire while the user was still
     * deciding, and the desktop's rule (§M64) is the same one. */
    if (kind == 1 && idx >= 0 && iv->model && iv->model->activate)
        iv->model->activate(iv->model->ctx, idx);
}

/* §M61 fix — keep the SELECTION visible.
 *
 * Reported from use: *"scroll doesn't work in the resolution list."*  It did
 * not: `scroll` existed in the widget and in every view's signature, and
 * NOTHING EVER CHANGED IT — arrow keys moved the selection off the bottom of
 * the pane and the items below simply could not be reached.  There is no wheel
 * to fall back on either (this PS/2 driver decodes the 3-byte packet, no
 * wheel), so the selection has to carry the viewport with it.
 *
 * Asked of the VIEW rather than computed here: `rect()` already knows where an
 * item lands, so "is it inside the box" needs no assumption about rows,
 * columns or item height — which is the whole point of the layouts being
 * swappable. */
static void iv_ensure_visible(struct w_itemview* iv, struct widget* w) {
    if (!iv->view || !iv->view->rect || iv->sel < 0) return;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return;

    for (int guard = 0; guard < n + 2; guard++) {
        int x, y, ww, hh;
        if (iv->view->rect(iv->sel, w->w, w->h, iv->model, iv->scroll,
                           &x, &y, &ww, &hh) == 0 && y >= 0 && y + hh <= w->h)
            return;                              /* fully visible — done */
        if (iv->sel < iv->scroll) {
            if (iv->scroll == 0) return;
            iv->scroll--;                        /* selection above the pane */
        } else {
            if (iv->scroll >= n - 1) return;
            iv->scroll++;                        /* below it (or clipped) */
        }
    }
}

static void iv_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->model || !iv->model->count) return;
    int n = iv->model->count(iv->model->ctx);
    if (n <= 0) return;

    int page = (iv->view && iv->view->page) ? iv->view->page(w->w, w->h) : 1;
    /* One row of the layout: for a grid that is the page divided by the number
     * of visible rows, and asking the view for a rect is more honest than
     * guessing a column count here. */
    int row = 1;
    if (iv->view && iv->view->rect) {
        int x0, y0, ww, hh, x1, y1, w1, h1;
        if (iv->view->rect(0, w->w, w->h, iv->model, iv->scroll, &x0, &y0, &ww, &hh) == 0) {
            for (int i = 1; i < n; i++) {
                if (iv->view->rect(i, w->w, w->h, iv->model, iv->scroll,
                                   &x1, &y1, &w1, &h1) != 0) break;
                if (y1 != y0) { row = i; break; }    /* first item on row 2 */
            }
        }
    }

    int sel = iv->sel;
    switch (kc) {
        case KC_LEFT:  sel--; break;
        case KC_RIGHT: sel++; break;
        case KC_UP:    sel -= row; break;
        case KC_DOWN:  sel += row; break;
        case KC_HOME:  sel = 0; break;
        case KC_END:   sel = n - 1; break;
        case KC_PGUP:  sel -= page; break;
        case KC_PGDN:  sel += page; break;
        case KC_ENTER:
            if (iv->sel >= 0 && iv->model->activate)
                iv->model->activate(iv->model->ctx, iv->sel);
            return;
        default: return;
    }
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    if (sel != iv->sel) {
        iv->sel = sel;
        iv_ensure_visible(iv, w);
        w_itemview_refresh(iv);
        if (iv->on_select) iv->on_select(iv, sel, w->ctx);
    }
}

/* §M61 follow-up — the wheel.  Three items per notch is what feels like one
 * gesture; one is glacial on an eleven-row list and a page is disorienting. */
static int iv_scroll(struct widget* w, int dz) {
    struct w_itemview* iv = (struct w_itemview*)w;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return 0;
    /* §M69 — the ceiling is the VIEW's, in the VIEW's unit; see
     * iv_scroll_max.  It used to be `n - 1` items, which is what let the wheel
     * scroll a two-row grid by three rows. */
    int max = iv_scroll_max(iv, n);
    int was = iv->scroll;
    iv->scroll -= dz * ui_wheel_lines();     /* wheel-up (positive) scrolls up */
    if (iv->scroll < 0) iv->scroll = 0;
    if (iv->scroll > max) iv->scroll = max;
    if (iv->scroll == was) return 0;      /* at the end — let the page have it */
    w_itemview_refresh(iv);   /* a changed scroll takes the whole-pane branch */
    return 1;
}

static const struct widget_ops itemview_ops = {
    iv_draw, iv_mouse, NULL, iv_keycode, NULL, iv_pointer, iv_scroll
};

struct w_itemview* w_itemview_create(struct gui_window* win, int x, int y,
                                     int w, int h,
                                     const struct item_model* model,
                                     const char* view_name, void* ctx) {
    struct w_itemview* iv = (struct w_itemview*)kcalloc(1, sizeof *iv);
    if (!iv) return NULL;
    iv->base.x = x; iv->base.y = y; iv->base.w = w; iv->base.h = h;
    iv->base.ops = &itemview_ops;
    iv->base.ctx = ctx;
    iv->base.focusable = 1;
    /* §M63 fix — `win` is what `gui_window_focus_widget(w->win, w)` and
     * `gui_window_request_redraw(w->win)` are given, and this hand-written
     * constructor did not set it.  Every other widget goes through widget.c's
     * shared `widget_init`, which does; writing a constructor by hand skipped
     * the one line nothing else needed.  The symptom was specific and
     * confusing: the mouse worked (selection followed clicks) and the KEYBOARD
     * did nothing, because focus was set on a NULL window and the keycode had
     * no focused widget to reach. */
    iv->base.win = win;
    iv->model = model;
    iv->view  = item_view_by_name(view_name);
    iv->sel = -1;
    iv->scroll = 0;
    gui_window_add_widget(win, &iv->base);
    return iv;
}
