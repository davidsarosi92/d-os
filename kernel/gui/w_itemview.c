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
#include "itemview.h"
#include "gfx.h"
#include "gui.h"
#include "keymap.h"
#include "kmalloc.h"
#include <stddef.h>

static void iv_draw(struct widget* w, struct gfx_surface* s) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view || !iv->view->draw) return;

    /* A quiet backdrop so the items read as a pane rather than as marks on the
     * window background — and so the selection wash has something to sit on. */
    gfx_fill(s, w->x, w->y, w->w, w->h, 0xFF223047u);
    iv->view->draw(s, w->x, w->y, w->w, w->h, iv->model, iv->sel, iv->scroll);
}

/* §M61 fix — pointer DRAG past the top or bottom edge scrolls, which is the
 * other half of "scroll works": with no wheel, dragging is how a mouse user
 * reaches an item that is not on screen. */
static void iv_pointer(struct widget* w, int lx, int ly, int phase) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (phase != WPTR_DRAG || !iv->view) return;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return;

    if (ly < 0 && iv->scroll > 0)              iv->scroll--;
    else if (ly > w->h && iv->scroll < n - 1)  iv->scroll++;
    else {
        int idx = iv->view->hit(lx, ly, w->w, w->h, iv->model, iv->scroll);
        if (idx >= 0) iv->sel = idx;
    }
    gui_window_request_redraw(w->win);
}

static void iv_mouse(struct widget* w, int lx, int ly, int kind) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view || !iv->view->hit) return;
    gui_window_focus_widget(w->win, w);

    int idx = iv->view->hit(lx, ly, w->w, w->h, iv->model, iv->scroll);
    if (idx >= 0) iv->sel = idx;
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
        if (iv->on_select) iv->on_select(iv, sel, w->ctx);
        gui_window_request_redraw(w->win);
    }
}

/* §M61 follow-up — the wheel.  Three items per notch is what feels like one
 * gesture; one is glacial on an eleven-row list and a page is disorienting. */
static void iv_scroll(struct widget* w, int dz) {
    struct w_itemview* iv = (struct w_itemview*)w;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return;
    iv->scroll -= dz * 3;                    /* wheel-up (positive) scrolls up */
    if (iv->scroll < 0) iv->scroll = 0;
    if (iv->scroll > n - 1) iv->scroll = n - 1;
    gui_window_request_redraw(w->win);
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
