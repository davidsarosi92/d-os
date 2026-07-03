/* =============================================================================
 * widget.c — label / button / listview / textinput (M22 stage 6).
 *
 * See widget.h for the model.  Drawing is plain gfx primitives; every
 * widget draws its full rect (the window redraw fills the content
 * background first, so widgets need not erase their own old pixels).
 * ============================================================================= */

#include "widget.h"
#include "gui.h"
#include "gfx.h"
#include "kmalloc.h"
#include <stddef.h>

/* Palette — deliberately close to the window chrome in gui.c. */
#define WCOL_TEXT       0xFFE0E0E0u
#define WCOL_DIM        0xFF8C9AAAu
#define WCOL_BTN_TOP    0xFF4A5B72u
#define WCOL_BTN_BOT    0xFF334052u
#define WCOL_BTN_EDGE   0xFF5F7089u
#define WCOL_BOX_BG     0xFF0B1220u
#define WCOL_BOX_EDGE   0xFF3A4A5Eu
#define WCOL_BOX_FOCUS  0xFF3D7BD8u
#define WCOL_SEL_BG     0xFF2C5B9Eu
#define WCOL_ARROW      0xFFB8C4D2u

/* -------------------------------------------------------------------------- */
/* Generic helpers.                                                            */
/* -------------------------------------------------------------------------- */

static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

void widget_draw_all(struct widget* head, struct gfx_surface* s) {
    for (struct widget* w = head; w; w = w->next)
        if (w->ops && w->ops->draw) w->ops->draw(w, s);
}

struct widget* widget_at(struct widget* head, int lx, int ly) {
    struct widget* hit = NULL;                  /* last match wins (top-most) */
    for (struct widget* w = head; w; w = w->next)
        if (lx >= w->x && lx < w->x + w->w && ly >= w->y && ly < w->y + w->h)
            hit = w;
    return hit;
}

static void base_init(struct widget* w, struct gui_window* win,
                      int x, int y, int ww, int hh,
                      const struct widget_ops* ops, void* ctx, int focusable) {
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    w->ops = ops;
    w->win = win;
    w->ctx = ctx;
    w->focusable = focusable;
    w->next = NULL;
    gui_window_add_widget(win, w);
}

static void outline(struct gfx_surface* s, int x, int y, int w, int h, uint32_t c) {
    gfx_fill(s, x,         y,         w, 1, c);
    gfx_fill(s, x,         y + h - 1, w, 1, c);
    gfx_fill(s, x,         y,         1, h, c);
    gfx_fill(s, x + w - 1, y,         1, h, c);
}

/* -------------------------------------------------------------------------- */
/* Label.                                                                      */
/* -------------------------------------------------------------------------- */

static void label_draw(struct widget* w, struct gfx_surface* s) {
    struct w_label* l = (struct w_label*)w;
    gfx_text(s, w->x, w->y + (w->h - GFX_GLYPH_H) / 2, l->text, l->color);
}

static const struct widget_ops label_ops = { label_draw, NULL, NULL };

struct w_label* w_label_create(struct gui_window* win, int x, int y, int w,
                               const char* text) {
    struct w_label* l = (struct w_label*)kcalloc(1, sizeof(*l));
    if (!l) return NULL;
    base_init(&l->base, win, x, y, w, GFX_GLYPH_H + 4, &label_ops, NULL, 0);
    l->color = WCOL_TEXT;
    str_copy(l->text, text, (int)sizeof(l->text));
    return l;
}

void w_label_set(struct w_label* l, const char* text) {
    if (!l) return;
    str_copy(l->text, text, (int)sizeof(l->text));
}

/* -------------------------------------------------------------------------- */
/* Button.                                                                     */
/* -------------------------------------------------------------------------- */

static void button_draw(struct widget* w, struct gfx_surface* s) {
    struct w_button* b = (struct w_button*)w;
    gfx_vgradient(s, w->x, w->y, w->w, w->h, WCOL_BTN_TOP, WCOL_BTN_BOT);
    outline(s, w->x, w->y, w->w, w->h, WCOL_BTN_EDGE);
    int tw = 0;
    while (b->text[tw]) tw++;
    gfx_text(s, w->x + (w->w - tw * GFX_GLYPH_W) / 2,
             w->y + (w->h - GFX_GLYPH_H) / 2, b->text, WCOL_TEXT);
}

static void button_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    struct w_button* b = (struct w_button*)w;
    if (b->on_click) b->on_click(b, w->ctx);
}

static const struct widget_ops button_ops = { button_draw, button_mouse, NULL };

struct w_button* w_button_create(struct gui_window* win, int x, int y,
                                 int w, int h, const char* text,
                                 void (*on_click)(struct w_button*, void*),
                                 void* ctx) {
    struct w_button* b = (struct w_button*)kcalloc(1, sizeof(*b));
    if (!b) return NULL;
    base_init(&b->base, win, x, y, w, h, &button_ops, ctx, 0);
    b->on_click = on_click;
    str_copy(b->text, text, (int)sizeof(b->text));
    return b;
}

/* -------------------------------------------------------------------------- */
/* List view.  Rows of text; right-edge 12px strip = scroll arrows.            */
/* -------------------------------------------------------------------------- */

#define LV_ARROW_W 12

static int lv_visible_rows(const struct w_listview* lv) {
    int r = (lv->base.h - 4) / WLIST_ROW_H;
    return r > 0 ? r : 1;
}

static void lv_draw_arrow(struct gfx_surface* s, int cx, int cy, int up) {
    /* 7px wide triangle out of stacked hlines. */
    for (int i = 0; i < 4; i++) {
        int half = up ? i : 3 - i;
        gfx_fill(s, cx - half, cy + i, half * 2 + 1, 1, WCOL_ARROW);
    }
}

static void listview_draw(struct widget* w, struct gfx_surface* s) {
    struct w_listview* lv = (struct w_listview*)w;
    gfx_fill(s, w->x, w->y, w->w, w->h, WCOL_BOX_BG);
    outline(s, w->x, w->y, w->w, w->h, WCOL_BOX_EDGE);

    int rows = lv_visible_rows(lv);
    for (int r = 0; r < rows; r++) {
        int idx = lv->scroll + r;
        if (idx >= lv->count) break;
        int ry = w->y + 2 + r * WLIST_ROW_H;
        if (idx == lv->sel)
            gfx_fill(s, w->x + 2, ry, w->w - LV_ARROW_W - 4, WLIST_ROW_H,
                     WCOL_SEL_BG);
        gfx_text(s, w->x + 6, ry + (WLIST_ROW_H - GFX_GLYPH_H) / 2,
                 lv->items[idx], WCOL_TEXT);
    }

    /* Scroll strip. */
    int ax = w->x + w->w - LV_ARROW_W;
    gfx_fill(s, ax, w->y + 1, LV_ARROW_W - 1, w->h - 2, 0xFF16202Eu);
    lv_draw_arrow(s, ax + LV_ARROW_W / 2 - 1, w->y + 5, 1);
    lv_draw_arrow(s, ax + LV_ARROW_W / 2 - 1, w->y + w->h - 10, 0);
}

static void listview_mouse(struct widget* w, int lx, int ly, int kind) {
    struct w_listview* lv = (struct w_listview*)w;
    int rows = lv_visible_rows(lv);

    if (lx >= w->w - LV_ARROW_W) {              /* scroll strip */
        if (ly < w->h / 2) { if (lv->scroll > 0) lv->scroll--; }
        else               { if (lv->scroll + rows < lv->count) lv->scroll++; }
        return;
    }

    int r = (ly - 2) / WLIST_ROW_H;
    int idx = lv->scroll + r;
    if (r < 0 || r >= rows || idx >= lv->count) return;

    lv->sel = idx;
    if (kind == 1) {
        if (lv->on_activate) lv->on_activate(lv, idx, w->ctx);
    } else {
        if (lv->on_select) lv->on_select(lv, idx, w->ctx);
    }
}

static const struct widget_ops listview_ops = { listview_draw, listview_mouse, NULL };

struct w_listview* w_listview_create(struct gui_window* win, int x, int y,
                                     int w, int h, void* ctx) {
    struct w_listview* lv = (struct w_listview*)kcalloc(1, sizeof(*lv));
    if (!lv) return NULL;
    base_init(&lv->base, win, x, y, w, h, &listview_ops, ctx, 0);
    lv->sel = -1;
    return lv;
}

void w_listview_clear(struct w_listview* lv) {
    if (!lv) return;
    lv->count = 0;
    lv->sel = -1;
    lv->scroll = 0;
}

int w_listview_add(struct w_listview* lv, const char* text, uint8_t tag) {
    if (!lv || lv->count >= WLIST_MAX_ITEMS) return -1;
    str_copy(lv->items[lv->count], text, WLIST_ITEM_LEN);
    lv->tags[lv->count] = tag;
    return lv->count++;
}

/* -------------------------------------------------------------------------- */
/* Text input.                                                                 */
/* -------------------------------------------------------------------------- */

static void textinput_draw(struct widget* w, struct gfx_surface* s) {
    struct w_textinput* t = (struct w_textinput*)w;
    int focused = gui_widget_focused(w);
    gfx_fill(s, w->x, w->y, w->w, w->h, WCOL_BOX_BG);
    outline(s, w->x, w->y, w->w, w->h, focused ? WCOL_BOX_FOCUS : WCOL_BOX_EDGE);

    /* Right-align overflow: show the tail that fits. */
    int maxch = (w->w - 10) / GFX_GLYPH_W;
    const char* p = t->buf;
    if (t->len > maxch) p += t->len - maxch;
    gfx_text(s, w->x + 5, w->y + (w->h - GFX_GLYPH_H) / 2, p, WCOL_TEXT);

    if (focused) {                              /* caret after the text */
        int cw = t->len > maxch ? maxch : t->len;
        gfx_fill(s, w->x + 5 + cw * GFX_GLYPH_W + 1, w->y + 3, 1, w->h - 6,
                 WCOL_TEXT);
    }
}

static void textinput_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    gui_window_focus_widget(w->win, w);         /* click = take keyboard focus */
}

static void textinput_key(struct widget* w, char c) {
    struct w_textinput* t = (struct w_textinput*)w;
    if (c == '\n') {
        if (t->on_submit) t->on_submit(t, w->ctx);
        return;
    }
    if (c == '\b') {
        if (t->len > 0) t->buf[--t->len] = 0;
        return;
    }
    if (c < 0x20 || c > 0x7E) return;           /* printable ASCII only */
    if (t->len < (int)sizeof(t->buf) - 1) {
        t->buf[t->len++] = c;
        t->buf[t->len] = 0;
    }
}

static const struct widget_ops textinput_ops =
    { textinput_draw, textinput_mouse, textinput_key };

struct w_textinput* w_textinput_create(struct gui_window* win, int x, int y,
                                       int w, void* ctx) {
    struct w_textinput* t = (struct w_textinput*)kcalloc(1, sizeof(*t));
    if (!t) return NULL;
    base_init(&t->base, win, x, y, w, 16, &textinput_ops, ctx, 1);
    return t;
}

void w_textinput_set(struct w_textinput* t, const char* text) {
    if (!t) return;
    str_copy(t->buf, text, (int)sizeof(t->buf));
    t->len = 0;
    while (t->buf[t->len]) t->len++;
}
