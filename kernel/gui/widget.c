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
#include "keymap.h"          /* M22.5: KC_* keycodes for navigation */
#include "clipboard.h"       /* M22.5: Ctrl+C/V in textinput */
#include "kmalloc.h"
#include "ui.h"             /* §M65 — ui_text_clipped + the class registry */
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
    for (struct widget* w = head; w; w = w->next) {
        if (!w->ops || !w->ops->draw) continue;
        /* §M65 — A WIDGET DRAWS INSIDE ITS OWN BOX.  Enforced here, in the one
         * loop every widget's draw goes through, rather than trusted to each
         * of them: the toolkit measures a widget and then tells it a size, and
         * a draw that ignores it lands on the widget next door (the settings
         * page's help text did exactly that).  With a scrolling container the
         * clip is narrower still — its viewport — which is what makes a child
         * scrolled half out of view stop at the edge instead of spilling. */
        if (w->clip_w > 0) gfx_set_clip(s, w->clip_x, w->clip_y, w->clip_w, w->clip_h);
        else               gfx_set_clip(s, w->x, w->y, w->w, w->h);
        w->ops->draw(w, s);
        gfx_clear_clip(s);
    }
}

struct widget* widget_at(struct widget* head, int lx, int ly) {
    struct widget* hit = NULL;                  /* last match wins (top-most) */
    for (struct widget* w = head; w; w = w->next)
        if (lx >= w->x && lx < w->x + w->w && ly >= w->y && ly < w->y + w->h)
            hit = w;
    return hit;
}

/* §M65 — EXPORTED as widget_init.  It was static, so every widget outside this
 * file hand-rolled its own initialisation — and w_itemview.c's copy forgot to
 * set `win`, which is why keyboard navigation had never worked in an item view
 * (§4.70).  A constructor written by hand skips exactly the line nothing else
 * needed; one shared initialiser is how that stops happening. */
void widget_init(struct widget* w, struct gui_window* win,
                 int x, int y, int ww, int hh,
                 const struct widget_ops* ops, void* ctx, int focusable) {
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    w->ops = ops;
    w->win = win;
    w->ctx = ctx;
    w->focusable = focusable;
    w->clip_x = w->clip_y = w->clip_w = w->clip_h = 0;   /* §M65: unrestricted */
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
    /* §M65 — clipped to the label's own rect.  A label is the widget most
     * likely to be handed text longer than its box (a help line, a path), and
     * text that spills is text drawn over the widget next to it. */
    ui_text_clipped(s, w, w->x, w->y + (w->h - GFX_GLYPH_H) / 2, l->text, l->color);
}

static const struct widget_ops label_ops = {
    label_draw, NULL, NULL, NULL, NULL, NULL, NULL
};

struct w_label* w_label_create(struct gui_window* win, int x, int y, int w,
                               const char* text) {
    struct w_label* l = (struct w_label*)kcalloc(1, sizeof(*l));
    if (!l) return NULL;
    widget_init(&l->base, win, x, y, w, GFX_GLYPH_H + 4, &label_ops, NULL, 0);
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

static const struct widget_ops button_ops = {
    button_draw, button_mouse, NULL, NULL, NULL, NULL, NULL
};

struct w_button* w_button_create(struct gui_window* win, int x, int y,
                                 int w, int h, const char* text,
                                 void (*on_click)(struct w_button*, void*),
                                 void* ctx) {
    struct w_button* b = (struct w_button*)kcalloc(1, sizeof(*b));
    if (!b) return NULL;
    widget_init(&b->base, win, x, y, w, h, &button_ops, ctx, 0);
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

    gui_window_focus_widget(w->win, w);         /* M22.5: keyboard nav */

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

/* M22.5 — keep the selected row inside the viewport. */
static void lv_scroll_to_sel(struct w_listview* lv) {
    int rows = lv_visible_rows(lv);
    if (lv->sel < lv->scroll) lv->scroll = lv->sel;
    if (lv->sel >= lv->scroll + rows) lv->scroll = lv->sel - rows + 1;
    if (lv->scroll < 0) lv->scroll = 0;
}

/* M22.5 — keyboard navigation: arrows / PgUp / PgDn / Home / End move
 * the selection (firing on_select like a click does). */
static void listview_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_listview* lv = (struct w_listview*)w;
    if (lv->count == 0) return;
    int rows = lv_visible_rows(lv);
    int sel = lv->sel < 0 ? 0 : lv->sel;

    switch (kc) {
    case KC_UP:    sel--;        break;
    case KC_DOWN:  sel++;        break;
    case KC_PGUP:  sel -= rows;  break;
    case KC_PGDN:  sel += rows;  break;
    case KC_HOME:  sel = 0;      break;
    case KC_END:   sel = lv->count - 1; break;
    default: return;
    }
    if (sel < 0) sel = 0;
    if (sel >= lv->count) sel = lv->count - 1;
    if (sel == lv->sel) return;
    lv->sel = sel;
    lv_scroll_to_sel(lv);
    if (lv->on_select) lv->on_select(lv, sel, w->ctx);
}

/* M22.5 — Enter activates the selection (same as double-click). */
static void listview_key(struct widget* w, char c) {
    struct w_listview* lv = (struct w_listview*)w;
    if (c != '\n' || lv->sel < 0 || lv->sel >= lv->count) return;
    if (lv->on_activate) lv->on_activate(lv, lv->sel, w->ctx);
}

/* §M61 follow-up — the wheel, for the same reason the item view got one: with
 * no wheel a list ends at its last visible row.  Three rows per notch. */
static void listview_scroll(struct widget* w, int dz) {
    struct w_listview* lv = (struct w_listview*)w;
    lv->scroll -= dz * 3;
    if (lv->scroll < 0) lv->scroll = 0;
    if (lv->scroll > lv->count - 1) lv->scroll = lv->count > 0 ? lv->count - 1 : 0;
    gui_window_request_redraw(w->win);
}

static const struct widget_ops listview_ops = {
    listview_draw, listview_mouse, listview_key, listview_keycode, NULL, NULL,
    listview_scroll
};

struct w_listview* w_listview_create(struct gui_window* win, int x, int y,
                                     int w, int h, void* ctx) {
    struct w_listview* lv = (struct w_listview*)kcalloc(1, sizeof(*lv));
    if (!lv) return NULL;
    widget_init(&lv->base, win, x, y, w, h, &listview_ops, ctx, 1);
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

/* M22.5 — clipboard shortcuts.  No in-line cursor (the caret sits at
 * the end by design), so copy/cut act on the whole content. */
static void textinput_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    struct w_textinput* t = (struct w_textinput*)w;
    if (!(mods & KBD_MOD_CTRL_MASK)) return;
    if (kc == KC_C || kc == KC_X) {
        clipboard_set(t->buf, t->len);
        if (kc == KC_X) { t->len = 0; t->buf[0] = 0; }
    } else if (kc == KC_V) {
        char tmp[sizeof t->buf];
        int n = clipboard_get(tmp, (int)sizeof tmp);
        for (int i = 0; i < n && t->len < (int)sizeof(t->buf) - 1; i++) {
            char c = tmp[i];
            if (c < 0x20 || c > 0x7E) continue;     /* single-line box */
            t->buf[t->len++] = c;
        }
        t->buf[t->len] = 0;
    }
}

static const struct widget_ops textinput_ops = {
    textinput_draw, textinput_mouse, textinput_key, textinput_keycode, NULL, NULL, NULL
};

struct w_textinput* w_textinput_create(struct gui_window* win, int x, int y,
                                       int w, void* ctx) {
    struct w_textinput* t = (struct w_textinput*)kcalloc(1, sizeof(*t));
    if (!t) return NULL;
    widget_init(&t->base, win, x, y, w, 16, &textinput_ops, ctx, 1);
    return t;
}

void w_textinput_set(struct w_textinput* t, const char* text) {
    if (!t) return;
    str_copy(t->buf, text, (int)sizeof(t->buf));
    t->len = 0;
    while (t->buf[t->len]) t->len++;
}

/* ===========================================================================
 * §M65 — CLASS REGISTRATIONS for the M22 controls.
 *
 * The controls themselves are untouched: each class is a thin adapter that
 * builds one from a `struct ui_spec` and reports the size it wants.  That is
 * what lets the layout engine place a twenty-line-old listview next to a
 * brand-new checkbox without either knowing about the other — and what lets a
 * panel (or, later, a ring-3 client) name "listview" in DATA instead of
 * calling a function pointer it cannot marshal.
 * ========================================================================= */


static struct widget* cls_label_create(struct gui_window* win,
                                       const struct ui_spec* sp) {
    struct w_label* l = w_label_create(win, 0, 0, 120, sp->text);
    return l ? &l->base : NULL;
}
static void cls_label_measure(struct widget* w, int avail_w, int* min_w,
                              int* pref_w, int* pref_h) {
    struct w_label* l = (struct w_label*)w;
    int n = 0; while (l->text[n]) n++;
    *min_w  = GFX_GLYPH_W * 4;
    *pref_w = n * GFX_GLYPH_W;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = GFX_GLYPH_H + 4;
}
static void cls_label_settext(struct widget* w, const char* t) {
    w_label_set((struct w_label*)w, t);
}
static int cls_label_gettext(struct widget* w, char* out, int cap) {
    struct w_label* l = (struct w_label*)w;
    int i = 0; for (; l->text[i] && i < cap - 1; i++) out[i] = l->text[i];
    if (cap) out[i] = 0;
    return i;
}
WIDGET_CLASS(wc_label) = {
    .name = "label", .create = cls_label_create, .measure = cls_label_measure,
    .set_text = cls_label_settext, .get_text = cls_label_gettext,
};

/* The button's click reaches the window's ONE event sink rather than a
 * per-widget callback: (id, type) is what a boundary can carry. */
static void cls_button_click(struct w_button* b, void* ctx) {
    (void)ctx;
    ui_emit(&b->base, UI_EV_CLICK, 0);
}
static struct widget* cls_button_create(struct gui_window* win,
                                        const struct ui_spec* sp) {
    struct w_button* b = w_button_create(win, 0, 0, 90, 22, sp->text,
                                         cls_button_click, NULL);
    return b ? &b->base : NULL;
}
static void cls_button_measure(struct widget* w, int avail_w, int* min_w,
                               int* pref_w, int* pref_h) {
    struct w_button* b = (struct w_button*)w;
    int n = 0; while (b->text[n]) n++;
    *min_w  = 40;
    *pref_w = n * GFX_GLYPH_W + 20;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = 22;
}
static void cls_button_settext(struct widget* w, const char* t) {
    str_copy(((struct w_button*)w)->text, t, (int)sizeof ((struct w_button*)w)->text);
}
WIDGET_CLASS(wc_button) = {
    .name = "button", .create = cls_button_create, .measure = cls_button_measure,
    .set_text = cls_button_settext,
};

static void cls_list_select(struct w_listview* lv, int idx, void* ctx) {
    (void)ctx;
    ui_emit(&lv->base, UI_EV_CHANGE, idx);
}
static void cls_list_activate(struct w_listview* lv, int idx, void* ctx) {
    (void)ctx;
    ui_emit(&lv->base, UI_EV_ACTIVATE, idx);
}
static struct widget* cls_list_create(struct gui_window* win,
                                      const struct ui_spec* sp) {
    (void)sp;
    struct w_listview* lv = w_listview_create(win, 0, 0, 200, 120, NULL);
    if (!lv) return NULL;
    lv->on_select   = cls_list_select;
    lv->on_activate = cls_list_activate;
    return &lv->base;
}
static void cls_list_measure(struct widget* w, int avail_w, int* min_w,
                             int* pref_w, int* pref_h) {
    (void)w;
    *min_w  = 80;
    *pref_w = avail_w;                  /* a list takes what it is given */
    *pref_h = 120;                      /* …and grows by weight, not by wish */
}
static int  cls_list_get(struct widget* w) { return ((struct w_listview*)w)->sel; }
static void cls_list_set(struct widget* w, int v) {
    struct w_listview* lv = (struct w_listview*)w;
    if (v >= 0 && v < lv->count) lv->sel = v;
}
WIDGET_CLASS(wc_listview) = {
    .name = "listview", .create = cls_list_create, .measure = cls_list_measure,
    .get_value = cls_list_get, .set_value = cls_list_set,
};

static void cls_text_submit(struct w_textinput* t, void* ctx) {
    (void)ctx;
    ui_emit(&t->base, UI_EV_SUBMIT, 0);
}
static struct widget* cls_text_create(struct gui_window* win,
                                      const struct ui_spec* sp) {
    struct w_textinput* t = w_textinput_create(win, 0, 0, 160, NULL);
    if (!t) return NULL;
    if (sp->text) w_textinput_set(t, sp->text);
    t->on_submit = cls_text_submit;
    return &t->base;
}
static void cls_text_measure(struct widget* w, int avail_w, int* min_w,
                             int* pref_w, int* pref_h) {
    *min_w  = 80;
    *pref_w = avail_w;
    *pref_h = w->h > 0 ? w->h : 22;
}
static void cls_text_settext(struct widget* w, const char* t) {
    w_textinput_set((struct w_textinput*)w, t);
}
static int cls_text_gettext(struct widget* w, char* out, int cap) {
    struct w_textinput* t = (struct w_textinput*)w;
    int i = 0; for (; t->buf[i] && i < cap - 1; i++) out[i] = t->buf[i];
    if (cap) out[i] = 0;
    return i;
}
WIDGET_CLASS(wc_textinput) = {
    .name = "textinput", .create = cls_text_create, .measure = cls_text_measure,
    .set_text = cls_text_settext, .get_text = cls_text_gettext,
};
