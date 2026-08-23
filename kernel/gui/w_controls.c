/* =============================================================================
 * w_controls.c — checkbox, radio group and slider (§M65), plus the class
 * registrations that let the LAYOUT build every M22 control by name.
 *
 * These three exist because the settings panel needed them and could not have
 * them: a `CFG_BOOL` was rendered as a text box with a "Cycle" button beside
 * it, which is what a toolkit with no checkbox looks like from the outside.
 *
 * Each control is an ordinary `struct widget` — the old vtable, unchanged —
 * plus a `struct widget_class` that carries the NEW capabilities (measure,
 * get/set value, get/set text).  Nothing here touches an existing widget_ops
 * table, which is the whole point of putting them on the class (§M58 paid for
 * inserting a field into the middle of one).
 *
 * Threading: constructors and callbacks run on the owning app-host task.
 * ============================================================================= */

#include "ui.h"
#include "widget.h"
#include "gui.h"
#include "gfx.h"
#include "keymap.h"
#include "kmalloc.h"
#include <stddef.h>

#define CCOL_TEXT      0xFFE0E0E0u
#define CCOL_DIM       0xFF8C9AAAu
#define CCOL_BOX_BG    0xFF0B1220u
#define CCOL_BOX_EDGE  0xFF3A4A5Eu
#define CCOL_MARK      0xFF6FD08Cu
#define CCOL_FILL      0xFF3D7BD8u
#define CCOL_FOCUS     0xFF3D7BD8u

static void cstr_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int cstr_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void box_outline(struct gfx_surface* s, int x, int y, int w, int h,
                        uint32_t c) {
    gfx_fill(s, x,         y,         w, 1, c);
    gfx_fill(s, x,         y + h - 1, w, 1, c);
    gfx_fill(s, x,         y,         1, h, c);
    gfx_fill(s, x + w - 1, y,         1, h, c);
}

/* ===========================================================================
 * Checkbox.
 * ========================================================================= */

struct w_checkbox {
    struct widget base;
    char text[64];
    int  checked;
};

#define CB_BOX 14

static void cb_draw(struct widget* w, struct gfx_surface* s) {
    struct w_checkbox* c = (struct w_checkbox*)w;
    int by = w->y + (w->h - CB_BOX) / 2;
    gfx_fill(s, w->x, by, CB_BOX, CB_BOX, CCOL_BOX_BG);
    box_outline(s, w->x, by, CB_BOX, CB_BOX,
                gui_window_focused_widget(w->win) == w ? CCOL_FOCUS : CCOL_BOX_EDGE);
    if (c->checked) {
        /* A tick drawn from two strokes: at 8x8-font scale a glyph "X" reads
         * as text, and this has to read as STATE. */
        for (int i = 0; i < 4; i++)
            gfx_fill(s, w->x + 3 + i, by + 7 + i, 2, 2, CCOL_MARK);
        for (int i = 0; i < 5; i++)
            gfx_fill(s, w->x + 6 + i, by + 9 - i, 2, 2, CCOL_MARK);
    }
    ui_text_clipped(s, w, w->x + CB_BOX + 6, w->y + (w->h - GFX_GLYPH_H) / 2,
                    c->text, CCOL_TEXT);
}

static void cb_toggle(struct w_checkbox* c) {
    c->checked = !c->checked;
    ui_emit(&c->base, UI_EV_TOGGLE, c->checked);
    gui_window_request_redraw(c->base.win);
}

static void cb_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    gui_window_focus_widget(w->win, w);
    cb_toggle((struct w_checkbox*)w);
}

/* Space toggles, which is the binding every toolkit uses and the reason the
 * control is focusable at all: a settings page must be usable without a mouse
 * (§M61 found that keyboard navigation had never worked in an item view). */
static void cb_key(struct widget* w, char ch) {
    if (ch == ' ' || ch == '\n') cb_toggle((struct w_checkbox*)w);
}

/* DESIGNATED initialisers, deliberately: every older ops table in the tree is
 * positional, and §M58 paid for what that costs when a field moves. */
static const struct widget_ops cb_ops = {
    .draw = cb_draw, .mouse = cb_mouse, .key = cb_key,
};

static struct widget* cb_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_checkbox* c = (struct w_checkbox*)kcalloc(1, sizeof *c);
    if (!c) return NULL;
    cstr_copy(c->text, sp->text, sizeof c->text);
    c->checked = sp->value ? 1 : 0;
    widget_init(&c->base, win, 0, 0, 120, 18, &cb_ops, NULL, 1);
    return &c->base;
}

static void cb_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    struct w_checkbox* c = (struct w_checkbox*)w;
    int text_w = cstr_len(c->text) * GFX_GLYPH_W;
    *min_w  = CB_BOX + 6 + GFX_GLYPH_W * 4;
    *pref_w = CB_BOX + 6 + text_w;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = 18;
}

static int  cb_get(struct widget* w) { return ((struct w_checkbox*)w)->checked; }
static void cb_set(struct widget* w, int v) {
    ((struct w_checkbox*)w)->checked = v ? 1 : 0;
}
static void cb_settext(struct widget* w, const char* t) {
    cstr_copy(((struct w_checkbox*)w)->text, t, sizeof ((struct w_checkbox*)w)->text);
}

WIDGET_CLASS(wc_checkbox) = {
    .name = "checkbox", .create = cb_create, .measure = cb_measure,
    .get_value = cb_get, .set_value = cb_set, .set_text = cb_settext,
};

/* ===========================================================================
 * Radio group.
 *
 * ONE widget holding N options, not N widgets that have to agree with each
 * other.  Mutual exclusion is the entire semantic of a radio group, and the
 * only way to make N independent widgets exclusive is to give one of them
 * authority over the others — at which point it is this.
 * ========================================================================= */

#define RG_MAX_OPTS 12

struct w_radio {
    struct widget base;
    char  opts[RG_MAX_OPTS][24];
    int   count;
    int   sel;
};

/* Options arrive as ONE space-separated string, because the spec is data and a
 * spec with a pointer-to-array-of-strings is not something a ring-3 client can
 * send in a flat blob (§M65's rule).  Same reason CONFIG_KEY spells its enum
 * values that way. */
static void rg_parse(struct w_radio* r, const char* list) {
    r->count = 0;
    if (!list) return;
    const char* p = list;
    while (*p && r->count < RG_MAX_OPTS) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < 23) r->opts[r->count][n++] = *p++;
        r->opts[r->count][n] = 0;
        while (*p && *p != ' ') p++;         /* skip an over-long tail */
        r->count++;
    }
}

#define RG_DOT 12
#define RG_ROW 18

static void rg_draw(struct widget* w, struct gfx_surface* s) {
    struct w_radio* r = (struct w_radio*)w;
    int focused = gui_window_focused_widget(w->win) == w;
    for (int i = 0; i < r->count; i++) {
        int y = w->y + i * RG_ROW;
        int cy = y + (RG_ROW - RG_DOT) / 2;
        gfx_fill(s, w->x, cy, RG_DOT, RG_DOT, CCOL_BOX_BG);
        box_outline(s, w->x, cy, RG_DOT, RG_DOT,
                    (focused && i == r->sel) ? CCOL_FOCUS : CCOL_BOX_EDGE);
        if (i == r->sel)
            gfx_fill(s, w->x + 3, cy + 3, RG_DOT - 6, RG_DOT - 6, CCOL_MARK);
        ui_text_clipped(s, w, w->x + RG_DOT + 6, y + (RG_ROW - GFX_GLYPH_H) / 2,
                        r->opts[i], CCOL_TEXT);
    }
}

static void rg_select(struct w_radio* r, int i) {
    if (i < 0 || i >= r->count || i == r->sel) return;
    r->sel = i;
    ui_emit(&r->base, UI_EV_CHANGE, i);
    gui_window_request_redraw(r->base.win);
}

static void rg_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)kind;
    gui_window_focus_widget(w->win, w);
    rg_select((struct w_radio*)w, ly / RG_ROW);
}

static void rg_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_radio* r = (struct w_radio*)w;
    if (kc == KC_DOWN) rg_select(r, r->sel + 1);
    if (kc == KC_UP)   rg_select(r, r->sel - 1);
}

static const struct widget_ops rg_ops = {
    .draw = rg_draw, .mouse = rg_mouse, .keycode = rg_keycode,
};

static struct widget* rg_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_radio* r = (struct w_radio*)kcalloc(1, sizeof *r);
    if (!r) return NULL;
    rg_parse(r, sp->text);
    r->sel = (sp->value >= 0 && sp->value < r->count) ? sp->value : 0;
    widget_init(&r->base, win, 0, 0, 160, r->count * RG_ROW, &rg_ops, NULL, 1);
    return &r->base;
}

static void rg_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    struct w_radio* r = (struct w_radio*)w;
    int longest = 0;
    for (int i = 0; i < r->count; i++) {
        int n = cstr_len(r->opts[i]);
        if (n > longest) longest = n;
    }
    *min_w  = RG_DOT + 6 + GFX_GLYPH_W * 4;
    *pref_w = RG_DOT + 6 + longest * GFX_GLYPH_W;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = r->count * RG_ROW;
}

static int  rg_get(struct widget* w) { return ((struct w_radio*)w)->sel; }
static void rg_set(struct widget* w, int v) {
    struct w_radio* r = (struct w_radio*)w;
    if (v >= 0 && v < r->count) r->sel = v;
}
static void rg_settext(struct widget* w, const char* t) {
    struct w_radio* r = (struct w_radio*)w;
    rg_parse(r, t);
    if (r->sel >= r->count) r->sel = 0;
}
static int rg_gettext(struct widget* w, char* out, int cap) {
    struct w_radio* r = (struct w_radio*)w;
    if (r->sel < 0 || r->sel >= r->count) { if (cap) out[0] = 0; return 0; }
    cstr_copy(out, r->opts[r->sel], cap);
    return cstr_len(out);
}

WIDGET_CLASS(wc_radio) = {
    .name = "radio", .create = rg_create, .measure = rg_measure,
    .get_value = rg_get, .set_value = rg_set,
    .set_text = rg_settext, .get_text = rg_gettext,
};

/* ===========================================================================
 * Slider — the control an integer setting wants.
 * ========================================================================= */

struct w_slider {
    struct widget base;
    int value, min, max;
};

#define SL_H 18

static void sl_draw(struct widget* w, struct gfx_surface* s) {
    struct w_slider* sl = (struct w_slider*)w;
    int track_y = w->y + SL_H / 2 - 2;
    int span = sl->max - sl->min;
    if (span <= 0) span = 1;
    int pos = (sl->value - sl->min) * (w->w - 10) / span;
    if (pos < 0) pos = 0;
    if (pos > w->w - 10) pos = w->w - 10;

    gfx_fill(s, w->x, track_y, w->w, 4, CCOL_BOX_BG);
    box_outline(s, w->x, track_y, w->w, 4, CCOL_BOX_EDGE);
    gfx_fill(s, w->x, track_y, pos + 5, 4, CCOL_FILL);
    gfx_fill(s, w->x + pos, w->y + 2, 10, SL_H - 4,
             gui_window_focused_widget(w->win) == w ? CCOL_FOCUS : CCOL_DIM);
}

static void sl_set_from_x(struct w_slider* sl, int lx) {
    int wpx = sl->base.w - 10;
    if (wpx < 1) wpx = 1;
    int span = sl->max - sl->min;
    int v = sl->min + (lx - 5) * span / wpx;
    if (v < sl->min) v = sl->min;
    if (v > sl->max) v = sl->max;
    if (v == sl->value) return;
    sl->value = v;
    ui_emit(&sl->base, UI_EV_CHANGE, v);
    gui_window_request_redraw(sl->base.win);
}

static void sl_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)ly; (void)kind;
    gui_window_focus_widget(w->win, w);
    sl_set_from_x((struct w_slider*)w, lx);
}

/* §M58's pointer stream: a slider is the control a DRAG was invented for, and
 * implementing this op is also what asks gui.c for the pointer grab. */
static void sl_pointer(struct widget* w, int lx, int ly, int phase) {
    (void)ly;
    if (phase == WPTR_PRESS || phase == WPTR_DRAG)
        sl_set_from_x((struct w_slider*)w, lx);
}

static void sl_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_slider* sl = (struct w_slider*)w;
    int step = (sl->max - sl->min) / 20;
    if (step < 1) step = 1;
    if (kc == KC_LEFT || kc == KC_RIGHT) {
        int v = sl->value + (kc == KC_RIGHT ? step : -step);
        if (v < sl->min) v = sl->min;
        if (v > sl->max) v = sl->max;
        if (v != sl->value) {
            sl->value = v;
            ui_emit(&sl->base, UI_EV_CHANGE, v);
            gui_window_request_redraw(w->win);
        }
    }
}

static const struct widget_ops sl_ops = {
    .draw = sl_draw, .mouse = sl_mouse, .keycode = sl_keycode,
    .pointer = sl_pointer,
};

static struct widget* sl_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_slider* sl = (struct w_slider*)kcalloc(1, sizeof *sl);
    if (!sl) return NULL;
    sl->min = sp->min;
    sl->max = sp->max > sp->min ? sp->max : sp->min + 1;
    sl->value = sp->value < sl->min ? sl->min
              : (sp->value > sl->max ? sl->max : sp->value);
    widget_init(&sl->base, win, 0, 0, 160, SL_H, &sl_ops, NULL, 1);
    return &sl->base;
}

static void sl_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    (void)w;
    *min_w  = 60;
    *pref_w = avail_w > 240 ? 240 : avail_w;
    *pref_h = SL_H;
}

static int  sl_get(struct widget* w) { return ((struct w_slider*)w)->value; }
static void sl_setv(struct widget* w, int v) {
    struct w_slider* sl = (struct w_slider*)w;
    if (v < sl->min) v = sl->min;
    if (v > sl->max) v = sl->max;
    sl->value = v;
}

WIDGET_CLASS(wc_slider) = {
    .name = "slider", .create = sl_create, .measure = sl_measure,
    .get_value = sl_get, .set_value = sl_setv,
};

/* ===========================================================================
 * Combo box — the control an enum with more than a handful of options wants.
 *
 * It is a radio group's twin: same data (a space-separated option list), same
 * meaning (pick exactly one), different SHAPE — a radio group costs one row per
 * option, which is right for three and wrong for ten.  The settings panel picks
 * between them by option count, which is a layout decision, not a semantic one.
 *
 * The drop-down is gui.c's window popup — the same overlay the menu bar uses.
 * ========================================================================= */

struct w_combo {
    struct widget base;
    char  opts[RG_MAX_OPTS][24];
    int   count, sel;
};

#define CO_H 20

static void co_parse(struct w_combo* c, const char* list) {
    c->count = 0;
    const char* p = list;
    while (p && *p && c->count < RG_MAX_OPTS) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < 23) c->opts[c->count][n++] = *p++;
        c->opts[c->count][n] = 0;
        while (*p && *p != ' ') p++;
        c->count++;
    }
}

static void co_draw(struct widget* w, struct gfx_surface* s) {
    struct w_combo* c = (struct w_combo*)w;
    int focused = gui_window_focused_widget(w->win) == w;
    gfx_fill(s, w->x, w->y, w->w, CO_H, CCOL_BOX_BG);
    box_outline(s, w->x, w->y, w->w, CO_H, focused ? CCOL_FOCUS : CCOL_BOX_EDGE);
    if (c->sel >= 0 && c->sel < c->count)
        ui_text_clipped(s, w, w->x + 6, w->y + (CO_H - GFX_GLYPH_H) / 2,
                        c->opts[c->sel], CCOL_TEXT);
    /* The arrow, drawn rather than written: a "v" glyph reads as a letter. */
    int ax = w->x + w->w - 14, ay = w->y + CO_H / 2 - 2;
    for (int i = 0; i < 4; i++)
        gfx_fill(s, ax + i, ay + i, 8 - 2 * i, 1, CCOL_TEXT);
}

static void co_open(struct w_combo* c) {
    char buf[POPUP_TEXT_MAX];
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        for (int k = 0; c->opts[i][k] && n < (int)sizeof buf - 2; k++)
            buf[n++] = c->opts[i][k];
        if (i + 1 < c->count && n < (int)sizeof buf - 1) buf[n++] = '\n';
    }
    buf[n] = 0;
    ui_popup_from(&c->base, 0);
    int sx = 0, sy = 0;
    gui_window_content_origin(c->base.win, &sx, &sy);
    gui_popup_open(c->base.win, sx + c->base.x, sy + c->base.y + CO_H, buf, 0);
}

static void co_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    gui_window_focus_widget(w->win, w);
    co_open((struct w_combo*)w);
}

static void co_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_combo* c = (struct w_combo*)w;
    int v = c->sel;
    if (kc == KC_DOWN) v++;
    else if (kc == KC_UP) v--;
    else if (kc == KC_ENTER) { co_open(c); return; }
    else return;
    if (v < 0 || v >= c->count || v == c->sel) return;
    c->sel = v;
    ui_emit(w, UI_EV_CHANGE, v);
    gui_window_request_redraw(w->win);
}

static void co_popup_pick(struct widget* w, int tag, int row) {
    (void)tag;
    struct w_combo* c = (struct w_combo*)w;
    if (row < 0 || row >= c->count) { gui_window_request_redraw(w->win); return; }
    if (row != c->sel) {
        c->sel = row;
        ui_emit(w, UI_EV_CHANGE, row);
    }
    gui_window_request_redraw(w->win);
}

static const struct widget_ops co_ops = {
    .draw = co_draw, .mouse = co_mouse, .keycode = co_keycode,
};

static struct widget* co_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_combo* c = (struct w_combo*)kcalloc(1, sizeof *c);
    if (!c) return NULL;
    co_parse(c, sp->text);
    c->sel = (sp->value >= 0 && sp->value < c->count) ? sp->value : -1;
    widget_init(&c->base, win, 0, 0, 160, CO_H, &co_ops, NULL, 1);
    return &c->base;
}

static void co_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    struct w_combo* c = (struct w_combo*)w;
    int longest = 0;
    for (int i = 0; i < c->count; i++) {
        int n = cstr_len(c->opts[i]);
        if (n > longest) longest = n;
    }
    *min_w  = 60;
    *pref_w = longest * GFX_GLYPH_W + 30;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = CO_H;
}

static int  co_get(struct widget* w) { return ((struct w_combo*)w)->sel; }
static void co_set(struct widget* w, int v) {
    struct w_combo* c = (struct w_combo*)w;
    if (v >= 0 && v < c->count) c->sel = v;
}
static void co_settext(struct widget* w, const char* t) {
    co_parse((struct w_combo*)w, t);
}
static int co_gettext(struct widget* w, char* out, int cap) {
    struct w_combo* c = (struct w_combo*)w;
    if (c->sel < 0 || c->sel >= c->count) { if (cap) out[0] = 0; return 0; }
    cstr_copy(out, c->opts[c->sel], cap);
    return cstr_len(out);
}

WIDGET_CLASS(wc_combo) = {
    .name = "combo", .create = co_create, .measure = co_measure,
    .get_value = co_get, .set_value = co_set,
    .set_text = co_settext, .get_text = co_gettext,
    .popup_pick = co_popup_pick,
};
