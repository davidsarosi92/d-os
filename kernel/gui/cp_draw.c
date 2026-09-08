/* =============================================================================
 * cp_draw.c — the Console Plate SHAPE language.
 *
 * theme.c carries the colours.  This file carries the forms they are painted
 * in, and it exists because a design is not a palette: swapping the tokens
 * under M22's gradients produced the right hues in the wrong shapes, which
 * looks like a recoloured old toolkit rather than the new one.
 *
 * FOUR RULES, ALL FROM design/widget_specs.md, ALL SMALL:
 *
 *   1. EVERY surface is a PLATE: a flat fill, a 1 px `line` border, and one
 *      pixel cut from each corner.  The design asks for radius 4; there is no
 *      curve primitive (DESIGN_TARGET.md §2), and its own fallback section
 *      names the substitute — "minden sarokban hagyj ki 1 pixelt".  At the 24 px
 *      control height this tree uses, a 4 px radius would consume a sixth of
 *      the control anyway.
 *
 *   2. FLAT, NOT GRADIENT.  The design README states the language is built
 *      "1px keretekkel és négy felületréteggel — nem gradiensekkel".  Every
 *      `gfx_vgradient` in the chrome was a gradient nobody asked for.
 *
 *   3. THE SHADOW IS AN EDGE.  `cp_shadow.blur` is recorded and unrenderable
 *      (no per-pixel alpha).  The fallback is a 1 px offset band on the right
 *      and bottom — enough to lift a popup off what it covers, which is the
 *      whole job an elevation does here.
 *
 *   4. DISABLED IS A COMPOSITE, NOT A COLOUR.  45 % over the finished widget,
 *      which `gfx_blend_fill` does exactly.  Tinting each element separately
 *      would need a disabled variant of every token and would still drift.
 *
 * WHY THE FOCUS RING IS DRAWN LAST AND OUTSIDE.  widget_specs.md §0 puts it on
 * top of everything and OUTSIDE the border, offset 1 px, and is explicit that a
 * control must never be left without one.  Drawn inside, it would be mistaken
 * for a border style; drawn first, the fill would cover it.
 * ============================================================================= */

#include "console_plate.h"
#include "gfx.h"
#include "vfont.h"
#include "gui.h"
#include <stddef.h>

static int use_vector(void) { return cp_font_kind() == CP_FONT_VECTOR; }

/* THE DESIGN'S PIXELS ARE NOT OUR PIXELS, AND THE DIFFERENCE IS MEASURABLE.
 *
 * The reference is drawn on a 1400 px canvas: its 15 px body text occupies
 * 1.07 % of the screen width, with a measured x-height of 8 px and an ink range
 * of 14 px (design.png rows 404..417).  Taking that 15 as an absolute pixel
 * count on a 1920 px screen makes the same text 0.73 % of the width — 27 %
 * smaller than intended, which is exactly what "the letters have shrunk"
 * describes.  The first attempt did take it as absolute, which is the same
 * mistake as the earlier 2x one and in the same family: comparing a number
 * against the wrong denominator.
 *
 * So the design's numbers are treated as what they are — proportions of a
 * canvas — and the factor is DERIVED from the framebuffer at run time, because
 * §M61 made the resolution a runtime choice and a constant here would be wrong
 * again at the next mode set.
 *
 * FLOORED AT 100 %: on a 1280 px display the arithmetic asks for 91 %, and
 * shrinking text below the design's own absolute size buys nothing on the
 * machine that can least afford it.  CAPPED AT 200 % so a 4K desktop does not
 * end up with a title bar you could park in. */
#define CP_DESIGN_CANVAS_W 1400

static int ui_scale_pct(void) {
    int w = gui_screen_w();
    if (w <= 0) return 100;                  /* before the GUI exists */
    int pct = w * 100 / CP_DESIGN_CANVAS_W;
    if (pct < 100) pct = 100;
    if (pct > 200) pct = 200;
    return pct;
}

/* A design pixel, in ours. */
int cp_px(int design_px) {
    return (design_px * ui_scale_pct() + 50) / 100;
}

/* The density already arrives in DEVICE pixels (see cp_current_density), so
 * this must NOT convert again — the double-scale trap, which this file has
 * already sprung once today. */
static int body_px(void) {
    const cp_density* d = cp_current_density();
    int px = d->font_body * cp_font_scale();
    return px < 6 ? 6 : px;
}

/* A plate: flat fill with the four corner pixels omitted.  Same three-band
 * construction icons.c uses for its tiles — three fills, no per-pixel work. */
void cp_fill_plate(struct gfx_surface* s, int x, int y, int w, int h,
                   cp_color fill) {
    if (w < 3 || h < 3) { gfx_fill(s, x, y, w, h, fill); return; }
    gfx_fill(s, x + 1, y,         w - 2, 1,     fill);
    gfx_fill(s, x,     y + 1,     w,     h - 2, fill);
    gfx_fill(s, x + 1, y + h - 1, w - 2, 1,     fill);
}

/* The 1 px border that goes with it — the corners stay empty, which is what
 * makes the cut read as a corner rather than as a missing pixel. */
void cp_border(struct gfx_surface* s, int x, int y, int w, int h,
               cp_color line) {
    if (w < 3 || h < 3) return;
    gfx_fill(s, x + 1,     y,         w - 2, 1,     line);
    gfx_fill(s, x + 1,     y + h - 1, w - 2, 1,     line);
    gfx_fill(s, x,         y + 1,     1,     h - 2, line);
    gfx_fill(s, x + w - 1, y + 1,     1,     h - 2, line);
}

void cp_plate(struct gfx_surface* s, int x, int y, int w, int h,
              cp_color fill, cp_color line) {
    cp_fill_plate(s, x, y, w, h, fill);
    cp_border(s, x, y, w, h, line);
}

/* A ROUNDED RECTANGLE, AS A STAIRCASE — the primitive DESIGN_TARGET.md §2 lists
 * as absent and three controls need before they can exist.
 *
 * `cp_fill_plate` above is the radius-1 case, hand-written; a switch is a
 * STADIUM (radius = height/2) and a knob is a circle (radius = size/2), and
 * neither can be approximated by cutting one pixel.  The design anticipated
 * exactly this renderer and says so in its own fallback section: *"radius 4 →
 * sarok-levágás: 1px lépcső"* — so a staircase is the specified answer here,
 * not a compromise invented at implementation time.
 *
 * INTEGER ONLY (§A2 — no FP in kernel context): each row's horizontal inset is
 * r - isqrt(r² - dy²), one integer square root per row of the corners, and the
 * middle rows have none.  It draws ONE fill per row rather than one per pixel,
 * so a 26 px switch is 26 fills.
 *
 * The design's OTHER suggestion — a 1-bit mask generated at build time — was
 * declined: it fixes the radius at build time, and §M61 made the resolution a
 * runtime choice, so every mask would be right at one density and wrong at the
 * other.  That is the §M62 argument for the vector logo, one control down. */
static int cp_isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

void cp_fill_round(struct gfx_surface* s, int x, int y, int w, int h, int r,
                   cp_color fill) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1) { gfx_fill(s, x, y, w, h, fill); return; }
    for (int i = 0; i < h; i++) {
        int dy;
        if (i < r)            dy = r - i;          /* rows inside the top arc */
        else if (i >= h - r)  dy = r - (h - 1 - i);/* …and the bottom one     */
        else                  dy = 0;
        int inset = dy ? r - cp_isqrt(r * r - dy * dy) : 0;
        if (inset * 2 >= w) continue;
        gfx_fill(s, x + inset, y + i, w - 2 * inset, 1, fill);
    }
}

/* Fill + 1 px border in one call, which is what every caller actually wants:
 * the border is the SAME rounded shape one pixel smaller, so drawing it as an
 * outline would need the inset table twice and get the corners subtly wrong. */
void cp_plate_round(struct gfx_surface* s, int x, int y, int w, int h, int r,
                    cp_color fill, cp_color line) {
    cp_fill_round(s, x, y, w, h, r, line);
    if (w > 2 && h > 2)
        cp_fill_round(s, x + 1, y + 1, w - 2, h - 2, r - 1, fill);
}

/* The focus ring: 2 px, 1 px outside the widget's own box, in the `focus`
 * token — which carries alpha, so this is one of the few places the design's
 * real value is used rather than its no-alpha fallback. */
void cp_focus_ring(struct gfx_surface* s, int x, int y, int w, int h) {
    const cp_color c = cp_current_theme()->focus;
    const int o = CP_FOCUS_OFFSET, t = CP_FOCUS_RING;
    int rx = x - o - t, ry = y - o - t;
    int rw = w + 2 * (o + t), rh = h + 2 * (o + t);
    for (int i = 0; i < t; i++) {
        gfx_blend_fill(s, rx + i,          ry + i,          rw - 2 * i, 1, c);
        gfx_blend_fill(s, rx + i,          ry + rh - 1 - i, rw - 2 * i, 1, c);
        gfx_blend_fill(s, rx + i,          ry + i,          1, rh - 2 * i, c);
        gfx_blend_fill(s, rx + rw - 1 - i, ry + i,          1, rh - 2 * i, c);
    }
}

/* Elevation, as an edge rather than a blur (rule 3). */
void cp_shadow_edge(struct gfx_surface* s, int x, int y, int w, int h,
                    const cp_shadow* sh) {
    const cp_color c = sh ? sh->color : cp_current_theme()->shadow_md.color;
    const int d = sh && sh->dy > 0 ? (sh->dy > 3 ? 3 : sh->dy) : 1;
    gfx_blend_fill(s, x + d, y + h, w, d, c);          /* under  */
    gfx_blend_fill(s, x + w, y + d, d, h, c);          /* right  */
}

/* Disabled: composite the finished widget at 45 % (rule 4). */
void cp_dim(struct gfx_surface* s, int x, int y, int w, int h) {
    const cp_color bg = cp_current_theme()->surface;
    gfx_blend_fill(s, x, y, w, h,
                   (cp_color)(((cp_color)CP_DISABLED_ALPHA << 24) |
                              (bg & 0x00FFFFFFu)));
}

/* The label style: UPPERCASE, tracked, `muted`.
 *
 * The design asks for Barlow Condensed 600 at 11 px with 0.14em tracking.  Of
 * those four properties a single 8x8 bitmap font can honour exactly one —
 * tracking — and the design's own bitmap fallback says so: the meaning is
 * carried by "nagybetű + betűrés + muted szín", not by the typeface. */
static int label_px(void) {
    int px = cp_px(CP_LABEL_PX) * cp_font_scale();
    return px < 6 ? 6 : px;
}

void cp_label(struct gfx_surface* s, int x, int y, const char* str) {
    char up[96];
    int i = 0;
    for (; str && str[i] && i < (int)sizeof up - 1; i++) {
        char c = str[i];
        up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    up[i] = 0;

    if (!use_vector()) {
        gfx_text_scaled(s, x, y, up, cp_current_theme()->muted,
                        1, CP_LABEL_TRACKING);
        return;
    }

    /* Barlow Condensed SemiBold, which is what the design actually asks for
     * and what the bitmap font could honour none of.  Drawn a character at a
     * time because the tracking is part of the style, not of the face — and a
     * tracked string is the one thing vfont_draw deliberately does not do, so
     * that its advance stays exactly the font's own. */
    const int px = label_px();
    const cp_color c = cp_current_theme()->muted;
    const int base = y + vfont_ascent(&vfont_label, px);
    int pen = x;
    char one[2] = { 0, 0 };
    for (int k = 0; k < i; k++) {
        one[0] = up[k];
        pen += vfont_draw(s, &vfont_label, px, pen, base, one, c);
        pen += CP_LABEL_TRACKING;
    }
}

/* Width of a tracked label, so callers can lay one out without knowing the
 * tracking constant — the mistake that would otherwise be made in each of them
 * separately and differently. */
int cp_label_width(const char* str) {
    int n = 0;
    while (str && str[n]) n++;
    if (!use_vector()) return n * (GFX_GLYPH_W + CP_LABEL_TRACKING);

    char up[96];
    int i = 0;
    for (; str && str[i] && i < (int)sizeof up - 1; i++) {
        char c = str[i];
        up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    up[i] = 0;
    return vfont_text_w(&vfont_label, label_px(), up) + i * CP_LABEL_TRACKING;
}

/* Monospaced text — the terminal's grid, and the design's numeric columns,
 * which it sets in IBM Plex Mono precisely so that digits line up.  A separate
 * entry point rather than a flag because the CELL WIDTH is the whole point: a
 * caller that wants columns needs to be able to ask for one. */
int cp_mono_cell_w(void) {
    if (!use_vector()) return GFX_GLYPH_W * cp_font_scale();
    const cp_density* d = cp_current_density();
    return vfont_text_w(&vfont_mono, d->font_mono * cp_font_scale(), "0");
}

void cp_mono_text(struct gfx_surface* s, int x, int y, const char* str,
                  cp_color col) {
    if (!use_vector()) {
        gfx_text_scaled(s, x, y, str, col, cp_font_scale(), 0);
        return;
    }
    const cp_density* d = cp_current_density();
    int px = d->font_mono * cp_font_scale();
    vfont_draw(s, &vfont_mono, px, x, y + vfont_ascent(&vfont_mono, px),
               str, col);
}

/* --- the font metrics -----------------------------------------------------
 *
 * These are what the toolkit uses instead of GFX_GLYPH_W / GFX_GLYPH_H /
 * gfx_text, so that the measurement and the drawing move together — the only
 * way the two cannot disagree.  §M65's layout measures everything through
 * them, which is what made swapping the typeface underneath possible at all.
 *
 * THE REAL FACE IS PROPORTIONAL, AND THAT IS THE WHOLE DIFFICULTY.  With the
 * 8x8 bitmap every caller could measure a string as `strlen * cp_fw()`, and a
 * good many did.  That is wrong for every proportional face ever cut, so
 * `cp_text_w()` exists and is what callers must use; `cp_fw()` survives as the
 * NOMINAL advance for the arithmetic that is genuinely about cells (a column
 * of digits, a terminal), and is documented as approximate for anything else.
 *
 * `gui.font` selects vector or bitmap.  Not decoration: the bitmap path is the
 * one the boot console and the terminal grid still use, and a face that failed
 * to render would otherwise take the machine to a blank screen with no way to
 * ask it why. */
int cp_fw(void) {
    if (!use_vector()) return GFX_GLYPH_W * cp_font_scale();
    /* The advance of a digit: monospaced in every face worth using, which is
     * what makes it the honest nominal width rather than an average nobody
     * can predict. */
    int w = vfont_text_w(&vfont_ui, body_px(), "0");
    return w > 0 ? w : GFX_GLYPH_W;
}

int cp_fh(void) {
    if (!use_vector()) return GFX_GLYPH_H * cp_font_scale();
    return vfont_line_h(&vfont_ui, body_px());
}

/* What a string will actually occupy.  Every layout decision that ends in an
 * ellipsis, a centred caption or a right-aligned number has to come through
 * here. */
int cp_text_w(const char* str) {
    if (!use_vector()) {
        int n = 0;
        while (str && str[n]) n++;
        return n * GFX_GLYPH_W * cp_font_scale();
    }
    return vfont_text_w(&vfont_ui, body_px(), str);
}

/* `y` is the TOP of the line, not the baseline.
 *
 * vfont_draw takes a baseline, which is the right primitive — a proportional
 * face has glyphs that descend and glyphs that do not, and only the baseline is
 * common to them.  But every caller in this tree was written against a bitmap
 * font whose origin is its top-left, so the conversion happens once, here,
 * rather than in ninety call sites of which one would get it wrong. */
void cp_text(struct gfx_surface* s, int x, int y, const char* str,
             cp_color col) {
    if (!use_vector()) {
        gfx_text_scaled(s, x, y, str, col, cp_font_scale(), 0);
        return;
    }
    int px = body_px();
    vfont_draw(s, &vfont_ui, px, x, y + vfont_ascent(&vfont_ui, px), str, col);
}

/* The density's row height — see WLIST_ROW_H in widget.h for why a row is not
 * sized from the font. */
int cp_row_h(void) { return cp_current_density()->row_h; }

/* Rule 1 of the control convention (console_plate.h): derived from the TEXT,
 * capped by the design's token.
 *
 * THE PADDING IS DELIBERATELY SMALL, AND THIS DIVERGES FROM THE REFERENCE ON
 * PURPOSE.  widget_specs.md §1 gives a button `control_h` — 40 px against a
 * 15 px body face, i.e. 2.7x the text it is labelled with.  Ported faithfully
 * that produced the report this rule came from: *"the toggle is significantly
 * taller than the 'load packages at boot' text next to it."*  The instruction
 * was explicit — in a labelled row the control may be only MINIMALLY larger
 * than its explanation — so the padding here is 4 design px a side, giving
 * about 1.5x rather than 2.7x.
 *
 * WRITTEN DOWN AS A DIVERGENCE rather than left to be rediscovered: somebody
 * comparing this against the catalogue will find our controls shorter, and
 * that is a decision, not drift.  The cap remains, so at a large type size the
 * two meet and the design's own height takes over. */
int cp_ctrl_h(void) {
    int h = cp_fh() + 2 * cp_px(4);
    int cap = cp_current_density()->control_h;
    return h < cap ? h : cap;
}

/* Rule 0 — a PRESSABLE control's height: the design's token, unmodified.
 * `cp_ctrl_h()` above deliberately undercuts it for controls that stand beside
 * a sentence; a button is not one of those, and shrinking it there is what
 * made the toolbars hard to hit. */
void cp_border_dashed(struct gfx_surface* s, int x, int y, int w, int h,
                      cp_color line) {
    if (w <= 2 || h <= 2) return;
    /* Dash 4 on, 4 off, in DESIGN pixels so the rhythm survives a density
     * change — a dash measured in device pixels turns into a dotted line at
     * one resolution and a nearly solid one at another. */
    const int d = cp_px(4);
    if (d < 1) return;
    for (int i = 1; i < w - 1; i++) {
        if ((i / d) & 1) continue;
        gfx_fill(s, x + i, y, 1, 1, line);
        gfx_fill(s, x + i, y + h - 1, 1, 1, line);
    }
    for (int j = 1; j < h - 1; j++) {
        if ((j / d) & 1) continue;
        gfx_fill(s, x, y + j, 1, 1, line);
        gfx_fill(s, x + w - 1, y + j, 1, 1, line);
    }
}

int cp_cell_w(void) { return cp_mono_cell_w(); }
/* A little leading, or descenders touch the row below and a wall of text stops
 * being readable — the one place a terminal differs from a table, which gets
 * its air from the row separators instead. */
int cp_cell_h(void) { return cp_fh() + cp_px(2); }

int cp_btn_h(void) {
    int h = cp_current_density()->control_h;
    /* Never smaller than the beside-text height: on a hypothetical density
     * whose token is tiny, a button must still not be the smallest thing on
     * the row. */
    int floor = cp_ctrl_h();
    return h > floor ? h : floor;
}

/* Rule 2's padding: the design's own `control_pad_x`, already in device px. */
int cp_ctrl_pad_x(void) {
    int p = cp_current_density()->control_pad_x;
    return p > 0 ? p : cp_px(12);
}
