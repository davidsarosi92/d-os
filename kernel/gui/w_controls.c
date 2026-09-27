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
#include "console_plate.h"
#include "locale.h"
#include "gui.h"
#include "gfx.h"
#include "keymap.h"
#include "kmalloc.h"
#include <stddef.h>

/* THE PALETTE IS THE THEME NOW, READ LIVE.
 *
 * These were seven literals.  They are still seven names used exactly where
 * they were, but each expands to a read of the current Console Plate theme —
 * so a theme switch needs no invalidation path and no second copy of the
 * palette, and no call site changed.  That is the whole reason the toolkit
 * kept its colours in one place to begin with.
 *
 * Read PER USE rather than cached in a local: `gui.theme` can change from the
 * Appearance panel between two draws, and a cached pointer is a rule somebody
 * has to remember to refresh.  The cost is a load through a pointer that is
 * hot in cache; the fills around it move megabytes.
 *
 * The mapping is the design's own vocabulary (design/widget_specs.md §0):
 * `sunken` is the input/list surface, `line` is the 1 px border, `accent`
 * carries the filled state, and `focus` is the ring — which is the one token
 * that legitimately has alpha, and `gfx_blend_fill` composites it. */
#define CCOL_TEXT      (cp_current_theme()->text)
#define CCOL_DIM       (cp_current_theme()->muted)
#define CCOL_BOX_BG    (cp_current_theme()->sunken)
#define CCOL_BOX_EDGE  (cp_current_theme()->line)
#define CCOL_MARK      (cp_current_theme()->accent)
#define CCOL_FILL      (cp_current_theme()->accent)
#define CCOL_FOCUS     (cp_current_theme()->focus)

static void cstr_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int cstr_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

/* Console Plate border — see cp_draw.c. */
static void box_outline(struct gfx_surface* s, int x, int y, int w, int h,
                        uint32_t c) {
    cp_border(s, x, y, w, h, c);
}

/* ===========================================================================
 * Checkbox.
 * ========================================================================= */

struct w_checkbox {
    struct widget base;
    /* §M87 — 128, not 64: the generic settings panel puts a key's HELP here,
     * and at 64 the longer ones were cut — which also cut the catalogue key,
     * so the Hungarian panel showed those in English (found by `locale
     * missing`, which recorded the truncated key). */
    char text[128];
    int  checked;
};

/* Rule 4: a square standing next to text is derived from the type. */
#define CB_BOX (cp_fh() + cp_px(4))

/* A thick line segment, drawn as a walk along the longer axis.  gfx_line is
 * one pixel wide and the design's tick is 2 px at a 20 px box — which is a
 * PROPORTION, so it has to grow with the box like everything else here. */
static void cb_stroke(struct gfx_surface* s, int x0, int y0, int x1, int y1,
                      int th, uint32_t col) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = adx > ady ? adx : ady;
    if (steps <= 0) steps = 1;
    for (int i = 0; i <= steps; i++)
        gfx_fill(s, x0 + dx * i / steps - th / 2,
                    y0 + dy * i / steps - th / 2, th, th, col);
}

static void cb_draw(struct widget* w, struct gfx_surface* s) {
    struct w_checkbox* c = (struct w_checkbox*)w;
    const cp_theme* t = cp_current_theme();
    int by = w->y + (w->h - CB_BOX) / 2;
    int focused = gui_window_focused_widget(w->win) == w;

    /* §M69 — THE DESIGN'S OWN CHECKBOX (widget_specs.md §2), which this was
     * not in two separate ways.
     *
     * Reported from use: *"the tick is small and not the right size in the
     * box."*  It was drawn from literals — 3, 7, 4, 6, 9, 5 — measured for the
     * 8x8 font, so at a runtime face the mark stayed ~11 px inside a ~26 px
     * box and sat off-centre.  The SECOND error is the one the report could
     * not see: the spec says a checked box is filled `accent` with the tick in
     * `on_accent`, and ours kept the `sunken` fill and drew an accent tick —
     * so "on" read as a decorated "off" rather than as a filled state. */
    cp_fill_round(s, w->x, by, CB_BOX, CB_BOX, cp_px(4),
                  c->checked ? t->accent : t->sunken);
    cp_plate_round(s, w->x, by, CB_BOX, CB_BOX, cp_px(4),
                   c->checked ? t->accent : t->sunken,
                   focused ? t->focus : t->line);
    if (c->checked) {
        /* The spec's own path on a 20x20 box: (5,11) -> (8,14) -> (15,6),
         * scaled.  Kept as that arithmetic rather than as pre-computed numbers
         * so the next reader can check it against the catalogue. */
        const int B = CB_BOX;
        int th = cp_px(2);
        if (th < 2) th = 2;
        int ax = w->x + 5 * B / 20, ay = by + 11 * B / 20;
        int bx = w->x + 8 * B / 20, byy = by + 14 * B / 20;
        int cx = w->x + 15 * B / 20, cy = by + 6 * B / 20;
        cb_stroke(s, ax, ay, bx, byy, th, t->on_accent);
        cb_stroke(s, bx, byy, cx, cy, th, t->on_accent);
    }
    ui_text_clipped(s, w, w->x + CB_BOX + cp_px(11),
                    w->y + (w->h - cp_fh()) / 2, lstr(c->text), CCOL_TEXT);
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
    /* MEASURED IN THE FACE IT IS DRAWN IN, and TALL AS A CONTROL.
     *
     * This was `cstr_len * cp_fw()` and a literal 18 — the two habits the 8x8
     * font taught, and console_plate.h warns about the first in capitals.  With
     * a proportional face the width is wrong for every label, and 18 px is less
     * than half the design's `control_h`, which is what made the checkbox row
     * in the widget gallery clip its own descenders against the row above. */
    int text_w = cp_text_w(lstr(c->text));
    *min_w  = CB_BOX + cp_px(11) + cp_fw() * 4;
    *pref_w = CB_BOX + cp_px(11) + text_w;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = cp_ctrl_h();
}

static int  cb_get(struct widget* w) { return ((struct w_checkbox*)w)->checked; }
static void cb_set(struct widget* w, int v) {
    ((struct w_checkbox*)w)->checked = v ? 1 : 0;
}
static void cb_settext(struct widget* w, const char* t) {
    cstr_copy(((struct w_checkbox*)w)->text, t, sizeof ((struct w_checkbox*)w)->text);
}

WIDGET_CLASS(wc_checkbox) = {
    .ops = &cb_ops,
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

/* Rule 4: derived from the type, not a 2007 pixel count.  At the measured
 * density a literal 12 px next to ~20 px text is the "tiny radio" reported
 * from use — and it did not grow when the font did. */
/* §M69 — THE SAME SIZE AS THE CHECKBOX.  widget_specs.md gives both a 20 px
 * box (§2) and a 20 px circle (§3), i.e. they are deliberately identical, and
 * ours were `cp_fh() + cp_px(4)` and `cp_fh() - 2` — about 26 against 18.
 * Reported from use as *"there is a big difference between the radio and the
 * checkbox size."*  Two controls that stand in the same column of the same
 * form have to agree, and the design says so; one definition now. */
#define RG_DOT CB_BOX
#define RG_ROW cp_ctrl_h()      /* rule 1 */

static void rg_draw(struct widget* w, struct gfx_surface* s) {
    struct w_radio* r = (struct w_radio*)w;
    int focused = gui_window_focused_widget(w->win) == w;
    for (int i = 0; i < r->count; i++) {
        int y = w->y + i * RG_ROW;
        int cy = y + (RG_ROW - RG_DOT) / 2;
        /* §M69 — A RADIO IS A CIRCLE (widget_specs.md §3), and this drew a
         * SQUARE — i.e. a checkbox.  That is not a styling slip: the shape is
         * the whole affordance.  A square says "several of these may be on"
         * and a round one says "exactly one", so a radio group drawn as
         * checkboxes tells the user the opposite of what the control does —
         * the same defect §M69 fixed one control over, where a boolean setting
         * that applies instantly was rendered as a checkbox promising "when
         * you confirm".
         *
         * Drawable only because the switch needed a stadium first: radius
         * RG_DOT/2 makes `cp_fill_round`'s staircase a disc.  No new
         * primitive, and the same 1 px approximation the design specifies. */
        const int rad = RG_DOT / 2;
        cp_fill_round(s, w->x, cy, RG_DOT, RG_DOT, rad, CCOL_BOX_BG);
        cp_plate_round(s, w->x, cy, RG_DOT, RG_DOT, rad, CCOL_BOX_BG,
                       (focused && i == r->sel) ? CCOL_FOCUS : CCOL_BOX_EDGE);
        if (i == r->sel) {
            /* The spec's own answer: an `accent` disc of d=10 on the 20 px
             * circle — half the diameter, at any density. */
            int d = RG_DOT / 2;
            if (d < 2) d = 2;
            cp_fill_round(s, w->x + (RG_DOT - d) / 2, cy + (RG_DOT - d) / 2,
                          d, d, d / 2, CCOL_MARK);
        }
        ui_text_clipped(s, w, w->x + RG_DOT + 6, y + (RG_ROW - cp_fh()) / 2,
                        lstr(r->opts[i]), CCOL_TEXT);
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
    /* PIXELS, NOT CHARACTERS.  `longest` used to be a character count that the
     * caller multiplied by cp_fw() — correct under a fixed advance and wrong
     * under the proportional face §M69 brought in.  Measured directly, so the
     * variable's unit and its use cannot drift apart again. */
    int longest = 0;
    for (int i = 0; i < r->count; i++) {
        int n = cp_text_w(r->opts[i]);
        if (n > longest) longest = n;
    }
    *min_w  = RG_DOT + 6 + cp_fw() * 4;
    *pref_w = RG_DOT + 6 + longest;
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
    .ops = &rg_ops,
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

/* §M69 — RULE 0: A SLIDER IS AIMED AT AND THEN DRAGGED, so its widget is a
 * TARGET and takes the design's control height.  It was a literal 18 — an
 * 8x8-era count — which at a runtime face is a thin band inside a taller row,
 * and *"the slider cannot be dragged, however I click on it"* is what a thin
 * band feels like.  The TRACK stays slim (the design's 4 px) and is centred in
 * the control; what grew is the part you can hit. */
#define SL_H       cp_btn_h()
#define SL_TRACK   cp_px(4)
#define SL_KNOB_W  cp_px(10)

static void sl_draw(struct widget* w, struct gfx_surface* s) {
    struct w_slider* sl = (struct w_slider*)w;
    /* Everything derived from the widget's OWN height, so the knob fills the
     * target the user is aiming at instead of a fraction of it. */
    int kw = SL_KNOB_W, tr = SL_TRACK;
    int track_y = w->y + (w->h - tr) / 2;
    int span = sl->max - sl->min;
    if (span <= 0) span = 1;
    int pos = (sl->value - sl->min) * (w->w - kw) / span;
    if (pos < 0) pos = 0;
    if (pos > w->w - kw) pos = w->w - kw;

    gfx_fill(s, w->x, track_y, w->w, tr, CCOL_BOX_BG);
    box_outline(s, w->x, track_y, w->w, tr, CCOL_BOX_EDGE);
    gfx_fill(s, w->x, track_y, pos + kw / 2, tr, CCOL_FILL);
    cp_fill_round(s, w->x + pos, w->y + cp_px(2), kw, w->h - 2 * cp_px(2),
                  cp_px(3),
                  gui_window_focused_widget(w->win) == w ? CCOL_FOCUS : CCOL_DIM);
}

static void sl_set_from_x(struct w_slider* sl, int lx) {
    int wpx = sl->base.w - SL_KNOB_W;
    if (wpx < 1) wpx = 1;
    int span = sl->max - sl->min;
    int v = sl->min + (lx - SL_KNOB_W / 2) * span / wpx;
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
/* §M81 — WH_REPAINT everywhere is EXACTLY what this returned before the op had
 * a return value: the host set `ran = 1` whenever a pointer op existed.  Kept
 * identical on purpose, so the only behaviour this step changes is the
 * container's.  Narrowing it to WH_DAMAGED is a real saving and a separate
 * measurement — the slider's own callback may write into a label. */
static int sl_pointer(struct widget* w, int lx, int ly, int phase) {
    (void)ly;
    if (phase == WPTR_PRESS || phase == WPTR_DRAG)
        sl_set_from_x((struct w_slider*)w, lx);
    return WH_REPAINT;
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
    .ops = &sl_ops,
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

#define CO_H cp_btn_h()   /* rule 0: a dropdown is aimed at */   /* rule 1 — was a literal 20 */

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
        ui_text_clipped(s, w, w->x + 6, w->y + (CO_H - cp_fh()) / 2,
                        lstr(c->opts[c->sel]), CCOL_TEXT);
    /* The arrow, drawn rather than written: a "v" glyph reads as a letter. */
    int ax = w->x + w->w - 14, ay = w->y + CO_H / 2 - 2;
    for (int i = 0; i < 4; i++)
        gfx_fill(s, ax + i, ay + i, 8 - 2 * i, 1, CCOL_TEXT);
}

static void co_open(struct w_combo* c) {
    char buf[POPUP_TEXT_MAX];
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        /* §M69 — the popup shows the TRANSLATION; the selection comes back as
         * an INDEX, so the stored value is still the option string the
         * descriptor declared.  What is displayed and what is written are
         * deliberately different things here. */
        const char* o = lstr(c->opts[i]);
        for (int k = 0; o[k] && n < (int)sizeof buf - 2; k++)
            buf[n++] = o[k];
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
    int longest = 0;                          /* pixels — see rg_measure */
    for (int i = 0; i < c->count; i++) {
        int n = cp_text_w(c->opts[i]);
        if (n > longest) longest = n;
    }
    *min_w  = 60;
    *pref_w = longest + cp_px(30);
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
    .ops = &co_ops,
    .name = "combo", .create = co_create, .measure = co_measure,
    .get_value = co_get, .set_value = co_set,
    .set_text = co_settext, .get_text = co_gettext,
    .popup_pick = co_popup_pick,
};

/* ===========================================================================
 * Switch (widget_specs.md §4).
 *
 * NOT A CHECKBOX WITH A DIFFERENT SKIN, and the spec says why in one word:
 * "azonnali hatás" — a switch means the change has ALREADY happened, a
 * checkbox means it will happen when the form is confirmed.  A settings panel
 * that renders both as the same control is telling the user the wrong thing
 * about when their machine changed.
 *
 * The design's own no-alpha fallback is what makes it drawable here: the track
 * is a stadium (radius = height/2) and the knob a circle, both through
 * cp_fill_round's staircase.  There is no ANIMATION — the toolkit has no
 * animation clock (the compositor repaints damage rects; DESIGN_TARGET.md §6
 * says so), and the spec's 120 ms slide is therefore a jump.  Written down
 * rather than faked with a timer nobody would drive.
 * ========================================================================= */

struct w_switch {
    struct widget base;
    char text[128];                 /* §M87 — see w_checkbox */
    int  on;
};

/* THE ASPECT IS THE DESIGN'S (48:26); THE HEIGHT IS THE CONVENTION'S.
 *
 * Taking both from cp_px() is faithful to the reference and produced a 66x36
 * track beside ~20 px text — reported from use as "the toggle is huge next to
 * its text", and rule 1 exists because of it.  Deriving the height and keeping
 * the ratio means the switch still LOOKS like the design's switch; it just
 * stops dominating the sentence it belongs to. */
/* A SWITCH SITS BESIDE ITS TEXT RATHER THAN CONTAINING IT, so it is measured
 * against the text directly and not through cp_ctrl_h(): a button holds its
 * label and needs room around it, a toggle stands next to a sentence and only
 * has to be findable.  Two design pixels above the cap height is about 1.15x
 * the text — which is what "minimally larger" means when the thing beside it
 * is a sentence. */
static int sw_track_h(void) { return cp_fh() + cp_px(2); }
static int sw_track_w(void) { return sw_track_h() * 48 / 26; }

static void sw_draw(struct widget* w, struct gfx_surface* s) {
    struct w_switch* sw = (struct w_switch*)w;
    const cp_theme* t = cp_current_theme();
    const int tw = sw_track_w(), th = sw_track_h();
    int ty = w->y + (w->h - th) / 2;

    cp_plate_round(s, w->x, ty, tw, th, th / 2,
                   sw->on ? t->accent : t->switch_off, t->line);
    /* Knob: 3 px inset both ends, so its travel is the track minus twice that
     * minus its own width — derived, never a second constant that has to agree
     * with the first. */
    const int inset = cp_px(3);
    const int kd = th - 2 * inset;
    int kx = sw->on ? w->x + tw - inset - kd : w->x + inset;
    cp_plate_round(s, kx, ty + inset, kd, kd, kd / 2, t->knob, t->line);

    if (sw->text[0])
        ui_text_clipped(s, w, w->x + tw + cp_px(8),
                        w->y + (w->h - cp_fh()) / 2, sw->text, CCOL_TEXT);
    if (gui_window_focused_widget(w->win) == w)
        cp_focus_ring(s, w->x, ty, tw, th);
}

static void sw_toggle(struct w_switch* sw) {
    sw->on = !sw->on;
    ui_emit(&sw->base, UI_EV_TOGGLE, sw->on);
    gui_window_request_redraw(sw->base.win);
}

/* The WHOLE ROW is clickable, which the spec asks for explicitly — a 48x26
 * target beside a full-width label is the control being harder to hit than the
 * text explaining it. */
static void sw_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    gui_window_focus_widget(w->win, w);
    sw_toggle((struct w_switch*)w);
}

static void sw_key(struct widget* w, char ch) {
    if (ch == ' ' || ch == '\n') sw_toggle((struct w_switch*)w);
}

static const struct widget_ops sw_ops = {
    .draw = sw_draw, .mouse = sw_mouse, .key = sw_key,
};

static struct widget* sw_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_switch* sw = (struct w_switch*)kcalloc(1, sizeof *sw);
    if (!sw) return NULL;
    cstr_copy(sw->text, sp->text, sizeof sw->text);
    sw->on = sp->value ? 1 : 0;
    widget_init(&sw->base, win, 0, 0, sw_track_w(), sw_track_h(), &sw_ops, NULL, 1);
    return &sw->base;
}

static void sw_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    struct w_switch* sw = (struct w_switch*)w;
    *min_w  = sw_track_w();
    *pref_w = sw_track_w() + (sw->text[0] ? cp_px(8) + cp_text_w(sw->text) : 0);
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = cp_ctrl_h();
}

static int  sw_get(struct widget* w) { return ((struct w_switch*)w)->on; }
static void sw_set(struct widget* w, int v) { ((struct w_switch*)w)->on = v ? 1 : 0; }
static void sw_settext(struct widget* w, const char* t) {
    cstr_copy(((struct w_switch*)w)->text, t, sizeof ((struct w_switch*)w)->text);
}

WIDGET_CLASS(wc_switch) = {
    .ops = &sw_ops,
    .name = "switch", .create = sw_create, .measure = sw_measure,
    .get_value = sw_get, .set_value = sw_set, .set_text = sw_settext,
};

/* ===========================================================================
 * Segmented control / tabs (widget_specs.md §5).
 *
 * ONE class for both, because the spec gives them one entry and one drawing:
 * a tray with N items, the active one raised.  What differs is where they are
 * used, not what they are — and a second class would be a second place for the
 * arrow-key stepping and the active-item arithmetic to disagree.
 * ========================================================================= */

struct w_segmented {
    struct widget base;
    char  opts[RG_MAX_OPTS][24];
    int   count;
    int   sel;
};

static void seg_parse(struct w_segmented* g, const char* list) {
    const char* p = list;
    g->count = 0;
    while (p && *p && g->count < RG_MAX_OPTS) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < 23) g->opts[g->count][n++] = *p++;
        g->opts[g->count][n] = 0;
        g->count++;
    }
}

#define SEG_PAD  cp_px(3)      /* the tray's inner margin, per the spec */

/* Every item is the same width, so the boxes the painter draws and the boxes
 * the hit test compares against come from ONE expression — §M69's title-button
 * lesson, where a painter and a hit test computed the same box differently and
 * the top edge of every button was dead. */
static int seg_item_w(const struct w_segmented* g, int w) {
    int n = g->count > 0 ? g->count : 1;
    return (w - 2 * SEG_PAD) / n;
}

static void seg_draw(struct widget* w, struct gfx_surface* s) {
    struct w_segmented* g = (struct w_segmented*)w;
    const cp_theme* t = cp_current_theme();
    cp_plate_round(s, w->x, w->y, w->w, w->h, cp_px(4), t->tray, t->line);

    int iw = seg_item_w(g, w->w);
    int ih = w->h - 2 * SEG_PAD;
    for (int i = 0; i < g->count; i++) {
        int ix = w->x + SEG_PAD + i * iw;
        int iy = w->y + SEG_PAD;
        if (i == g->sel)
            cp_fill_round(s, ix, iy, iw, ih, cp_px(3), t->raised);
        else if (w->hovered)
            cp_fill_round(s, ix, iy, iw, ih, cp_px(3), t->hover);
        /* Centred: a segmented control's items are equal-width boxes, so
         * left-aligned text inside them reads as a ragged list.
         *
         * NO gfx_set_clip HERE, and the first version had one — §M65's rule,
         * which this control promptly broke: the clip is ALREADY in force,
         * set by widget_draw_all to the widget's box or to the enclosing
         * VIEWPORT, and gfx_set_clip REPLACES.  Setting a per-item clip and
         * clearing it threw the viewport away, so a segmented control scrolled
         * out of sight drew its labels across the panel — which is exactly
         * what the gallery's first screenshot showed: three words floating on
         * the window background with no tray under them, because the tray was
         * a fill drawn while the viewport clip still applied and the text was
         * not. */
        int tx = ix + (iw - cp_text_w(g->opts[i])) / 2;
        ui_text_clipped(s, w, tx, iy + (ih - cp_fh()) / 2, g->opts[i],
                        i == g->sel ? t->text : t->muted);
    }
    if (gui_window_focused_widget(w->win) == w)
        cp_focus_ring(s, w->x, w->y, w->w, w->h);
}

static void seg_select(struct w_segmented* g, int i) {
    if (i < 0 || i >= g->count || i == g->sel) return;
    g->sel = i;
    ui_emit(&g->base, UI_EV_CHANGE, i);
    gui_window_request_redraw(g->base.win);
}

static void seg_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)ly; (void)kind;
    struct w_segmented* g = (struct w_segmented*)w;
    gui_window_focus_widget(w->win, w);
    int iw = seg_item_w(g, w->w);
    if (iw <= 0) return;
    seg_select(g, (lx - SEG_PAD) / iw);
}

/* The GROUP is one tab stop and the arrows step within it — the spec's rule,
 * and the reason a segmented control is one widget rather than N buttons. */
static void seg_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_segmented* g = (struct w_segmented*)w;
    if (kc == KC_LEFT)  seg_select(g, g->sel - 1);
    if (kc == KC_RIGHT) seg_select(g, g->sel + 1);
}

static const struct widget_ops seg_ops = {
    .draw = seg_draw, .mouse = seg_mouse, .keycode = seg_keycode,
};

static struct widget* seg_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_segmented* g = (struct w_segmented*)kcalloc(1, sizeof *g);
    if (!g) return NULL;
    seg_parse(g, sp->text);
    g->sel = (sp->value >= 0 && sp->value < g->count) ? sp->value : 0;
    widget_init(&g->base, win, 0, 0, 160, cp_ctrl_h(), &seg_ops, NULL, 1);
    return &g->base;
}

static void seg_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                        int* pref_h) {
    struct w_segmented* g = (struct w_segmented*)w;
    int widest = 0;
    for (int i = 0; i < g->count; i++) {
        int n = cp_text_w(g->opts[i]);
        if (n > widest) widest = n;
    }
    int item = widest + 2 * cp_ctrl_pad_x();    /* rule 2 */
    *min_w  = g->count * (cp_fw() * 3) + 2 * SEG_PAD;
    *pref_w = g->count * item + 2 * SEG_PAD;
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = cp_ctrl_h();
}

static int  seg_get(struct widget* w) { return ((struct w_segmented*)w)->sel; }
static void seg_set(struct widget* w, int v) {
    struct w_segmented* g = (struct w_segmented*)w;
    if (v >= 0 && v < g->count) g->sel = v;
}
static void seg_settext(struct widget* w, const char* t) {
    seg_parse((struct w_segmented*)w, t);
}
static int seg_gettext(struct widget* w, char* out, int cap) {
    struct w_segmented* g = (struct w_segmented*)w;
    if (g->sel < 0 || g->sel >= g->count) { if (cap) out[0] = 0; return 0; }
    cstr_copy(out, g->opts[g->sel], cap);
    return cstr_len(out);
}

WIDGET_CLASS(wc_segmented) = {
    .ops = &seg_ops,
    .name = "segmented", .create = seg_create, .measure = seg_measure,
    .get_value = seg_get, .set_value = seg_set,
    .set_text = seg_settext, .get_text = seg_gettext,
};

/* ===========================================================================
 * Progress bar (widget_specs.md §9).
 *
 * DETERMINATE ONLY, and that is a statement about this toolkit rather than a
 * shortcut.  The spec's indeterminate variant is a 33 % band sweeping
 * -100 % → 300 % over 1400 ms, and the spinner is a rotating arc: both are
 * ANIMATIONS, and there is no animation clock here — the compositor repaints
 * damage rects and nothing ticks a widget between events.  Drawing a static
 * band and calling it indeterminate would say "work is happening" with a
 * picture that cannot stop saying it, which is worse than not offering it.
 * ========================================================================= */

struct w_progress {
    struct widget base;
    int value, min, max;
};

static void pg_draw(struct widget* w, struct gfx_surface* s) {
    struct w_progress* p = (struct w_progress*)w;
    const cp_theme* t = cp_current_theme();
    const int bh = cp_px(8);
    int by = w->y + (w->h - bh) / 2;
    cp_plate_round(s, w->x, by, w->w, bh, bh / 2, t->sunken, t->line);

    int span = p->max - p->min;
    int v = p->value;
    if (v < p->min) v = p->min;
    if (v > p->max) v = p->max;
    int fw = span > 0 ? (w->w - 2) * (v - p->min) / span : 0;
    /* A zero-width fill would be a rounded shape one pixel wide, which reads as
     * a dot at the left end rather than as "nothing done yet". */
    if (fw > 1) cp_fill_round(s, w->x + 1, by + 1, fw, bh - 2, (bh - 2) / 2,
                              t->accent);
}

static const struct widget_ops pg_ops = { .draw = pg_draw };

static struct widget* pg_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_progress* p = (struct w_progress*)kcalloc(1, sizeof *p);
    if (!p) return NULL;
    p->min = sp->min; p->max = sp->max > sp->min ? sp->max : sp->min + 100;
    p->value = sp->value;
    /* NOT focusable: it takes no input, and a Tab stop on a control that
     * cannot be operated is a dead step in every keyboard traversal. */
    widget_init(&p->base, win, 0, 0, 160, cp_px(8), &pg_ops, NULL, 0);
    return &p->base;
}

static void pg_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    (void)w;
    *min_w  = cp_px(60);
    *pref_w = avail_w;
    *pref_h = cp_px(8);
}

static int  pg_get(struct widget* w) { return ((struct w_progress*)w)->value; }
static void pg_set(struct widget* w, int v) {
    ((struct w_progress*)w)->value = v;
}

WIDGET_CLASS(wc_progress) = {
    .ops = &pg_ops,
    .name = "progress", .create = pg_create, .measure = pg_measure,
    .get_value = pg_get, .set_value = pg_set,
};
