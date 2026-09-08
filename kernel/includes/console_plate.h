/* =============================================================================
 * console_plate.h — the Console Plate design language, as this kernel draws it.
 *
 * PROVENANCE.  The design ships in `design/` (README.md, widget_specs.md,
 * tokens.json, and the browser reference under design/reference/).  This header
 * is the kernel-side form of `design/console_plate.h`.  The design files are the
 * authority on INTENT; this file is the authority on what the framebuffer
 * painter actually emits.  When they disagree, the disagreement is written down
 * here rather than silently resolved — see "WHAT DOES NOT SURVIVE" below.
 *
 * -----------------------------------------------------------------------------
 * THE COLOURS ARE VERBATIM, AND THAT IS NOT LUCK WORTH RELYING ON TWICE
 *
 * The design states colours as 0xAARRGGBB.  `gfx_blend_fill` reads alpha from
 * bits 31..24 and `gfx_fill` writes the word straight into the pixel, so every
 * token below is passed to the painter UNCHANGED — no conversion, no swizzle.
 * Stated explicitly because the next person to add a colour will want to know
 * whether there is a conversion step to remember.  There is not.
 *
 * -----------------------------------------------------------------------------
 * THE ONE ADAPTATION: THE GEOMETRY IS RESCALED, AND WHY IT HAD TO BE
 *
 * The design is drawn for a 15 px proportional body font: 40 px controls, 40 px
 * rows, 34 px menu items.  This kernel has ONE font — `font8x8`, 8x8, one bit
 * per pixel, every glyph the same width (see DESIGN_TARGET.md §4).  There is no
 * second size and no weight.  `CP_SCALE` in the design header scales UP by an
 * integer and cannot go down.
 *
 * So the numbers below keep the design's PROPORTIONS against an 8 px font
 * instead of its absolute pixels.  A 40 px control around a 15 px font is 2.67
 * font-heights; the 24 px control below is 3.0 — very slightly airier, and 24 is
 * exactly three glyph cells, which matters because §M65's layout measures in
 * CELLS.  Cell alignment was worth more here than matching 2.67 exactly.
 *
 * Every macro carries the design's original value in its comment.  That is the
 * point of keeping them: a number that looks wrong on screen can be checked
 * against what was intended, and the browser reference in design/reference/ can
 * be opened side by side.
 *
 * TREAT THESE AS A FIRST PASS.  They are derived arithmetic, not measured
 * against a running screen.  The derivation is defensible; the result still has
 * to be looked at.
 *
 * -----------------------------------------------------------------------------
 * WHAT DOES NOT SURVIVE, AND WHAT REPLACES IT
 *
 * The design anticipated this and ships both fallback paths in
 * `design/widget_specs.md` ("Ha nincs antialiasing / alfa-blend", "Ha csak
 * bitmap font van").  Where this kernel differs from what those sections
 * ASSUME, the difference is noted — in both directions:
 *
 *   RADIUS (4 outer / 3 inner) — no curve primitive exists in gfx.c at all.
 *     The fallback applies: cut one pixel from each corner.  A full-height
 *     radius (the switch's stadium shape) needs a pre-generated 1-bit mask.
 *
 *   SHADOW BLUR (2/10/34 px) — there is no blur, and none is coming; a blur is
 *     a per-pixel convolution over a software framebuffer.  The fallback
 *     applies: a 1 px `line` border plus a 1 px offset `line_soft` band on the
 *     right and bottom edges.  `cp_shadow.blur` is KEPT in the struct and is
 *     deliberately unread — dropping the field would lose the design's
 *     statement of how deep each elevation is meant to feel, which is what a
 *     later renderer would need.
 *
 *   FONTS (Barlow / Barlow Condensed 600 / IBM Plex Mono, 11/14/15/22/26 px,
 *     0.14em tracking) — one 8x8 bitmap font, no family, no weight, no size.
 *     The fallback assumes an 8x16 mono and "a second larger bitmap size" for
 *     titles; this kernel has NEITHER, so it is poorer than the fallback
 *     expects on this axis.  What carries the label style instead is the
 *     fallback's own answer minus the sizes: UPPERCASE + tracking + `muted`.
 *     Tracking is cheap because `gfx_text` is ours — a variant that advances
 *     8+n per glyph is a small addition, not a font problem.
 *
 *   ANIMATION (switch 120 ms, theme 140 ms, spinner 850 ms, indeterminate
 *     1400 ms) — the toolkit has no animation clock; the compositor repaints
 *     damage rectangles when something changes.  The durations are kept as
 *     macros because §M53 gives us a real nanosecond clock and a spinner is a
 *     plausible first consumer, but NOTHING reads them today.  A macro nobody
 *     reads is a note, not a feature, and is labelled as such.
 *
 *   ALPHA — here this kernel is RICHER than the fallback assumes.
 *     `gfx_blend_fill` composites a rectangle at constant alpha, so the modal
 *     veil is a real 45% wash rather than the fallback's checkerboard dither,
 *     `disabled` is a real 45% composite, and the focus ring can use the alpha
 *     `focus` token as designed.  What is genuinely absent is PER-PIXEL alpha:
 *     masks, ramps, blur.  Constant-alpha rectangles we have.
 *
 *   TOUCH (CP_MIN_TOUCH_TARGET 44) — dropped.  There is no touch input on any
 *     of the three arches, and a constant nothing can reach is dead weight.
 * ============================================================================= */

#ifndef CONSOLE_PLATE_H
#define CONSOLE_PLATE_H

#include <stdint.h>

/* 0xAARRGGBB — the painter's own pixel format (see the header note). */
typedef uint32_t cp_color;

/* `blur` is recorded and NOT rendered; see "WHAT DOES NOT SURVIVE". */
typedef struct {
    int      dy, blur;
    cp_color color;                 /* carries its own alpha */
} cp_shadow;

/* The eighteen colour roles.  A widget uses at most two surface layers
 * (design/widget_specs.md §0) — that rule is what keeps the language readable,
 * and it is worth enforcing by eye in review. */
typedef struct {
    cp_color bg;         /* desktop background                                */
    cp_color surface;    /* window / panel body                               */
    cp_color sunken;     /* input, list, slider trough                        */
    cp_color raised;     /* title bar, menu, dialog, toast                    */
    cp_color tray;       /* tab tray, table header, scrollbar trough          */
    cp_color line;       /* the 1 px border                                   */
    cp_color line_soft;  /* row separator, divider                            */
    cp_color text;
    cp_color muted;      /* secondary text, icon, scrollbar thumb             */
    cp_color accent;
    cp_color on_accent;  /* text / tick drawn ON an accent fill               */
    cp_color hover;
    cp_color press;
    cp_color switch_off;
    cp_color knob;
    cp_color sel_bg;     /* selected list row                                 */
    cp_color sel_fg;
    cp_color focus;      /* focus ring — carries alpha                        */
    cp_shadow shadow_sm, shadow_md, shadow_lg;
} cp_theme;

/* DECLARED, not defined.  The design header used `static const`, which is right
 * for a single-file browser build and wrong here: every translation unit that
 * included it would get its own copy of both themes, and a theme switch would
 * then have to find all of them.  One definition lives in theme.c. */
extern const cp_theme cp_dark;
extern const cp_theme cp_light;

/* Density.  `font_body`/`font_mono` are the design's own pixel sizes, and they
 * became REAL with §M69's vector faces — under the 8x8 bitmap they were both
 * 8, recorded so the struct still said what a density is and so a second size
 * had somewhere to land.  This is that landing. */
typedef struct {
    int control_h, control_pad_x, row_h, font_body, font_mono;
} cp_density;

extern const cp_density cp_comfort;   /* design: { 40, 18, 40, 15, 14 } */
extern const cp_density cp_compact;   /* design: { 32, 14, 32, 14, 13 } */

/* The live selection.  A theme switch is one assignment, which is what the
 * design's handoff asked for and also what §M63's config watchers want. */
const cp_theme*   cp_current_theme(void);
const cp_density* cp_current_density(void);

/* The density's row height, in device pixels.  Declared here rather than in
 * widget.h (where it used to sit) because it is a DENSITY fact, and the table
 * view needs it without needing the whole toolkit. */
int cp_row_h(void);

/* ---------------------------------------------------------------------------
 * THE CONTROL SIZING CONVENTION.
 *
 * Written down once, here, because it was being decided in nine places and the
 * nine disagreed: a button measured `strlen * cp_fw() + 20` and stood 22 px
 * tall, a radio dot was a literal 12, a checkbox reported 18, and the switch
 * took the design's 48x26 through cp_px() — which is faithful to the reference
 * and, next to our type at this density, simply too big.  Reported from use in
 * exactly those words: *"the toggle is huge next to its text; the radio is
 * tiny and sometimes does not fit."*
 *
 * FOUR RULES.  A widget that follows them needs no per-app tuning, and a widget
 * that breaks one has to say why at the point it does.
 *
 *   0. THERE ARE TWO KINDS OF CONTROL, AND RULE 1 IS ONLY ABOUT ONE OF THEM.
 *      A control that STANDS BESIDE a sentence (checkbox, radio, switch) has
 *      only to be findable, and dwarfing its own explanation makes it read as
 *      the important thing on the row.  A control you AIM AT and press
 *      (button, dropdown, text box) is a TARGET, and a target is judged by how
 *      easily the hand lands on it — `cp_btn_h()`, the design's own
 *      `control_h`.
 *
 *      THIS SPLIT IS A CORRECTION, AND THE ERROR IS WORTH KEEPING.  Rule 1
 *      came from a report about a TOGGLE beside a sentence, and it was applied
 *      to every control including buttons — after which the next report was
 *      *"the buttons are a bit hard to press."*  Both reports were right; the
 *      mistake was generalising the first one past the case it described.
 *      *A rule derived from one example is only as wide as that example.*
 *
 *   1. A CONTROL THAT STANDS BESIDE TEXT IS AS TALL AS THAT TEXT, PLUS A
 *      LITTLE —
 *      `cp_ctrl_h()`, about 1.5x the text, and a switch (which stands beside a
 *      sentence rather than containing one) about 1.15x.  In a labelled row the
 *      control may be only MINIMALLY larger than its explanation; a control
 *      twice the height of the sentence it belongs to reads as the important
 *      thing on the row, and it is not.  Capped at the density's `control_h` so
 *      it can never exceed the design's own token.
 *
 *      THIS DIVERGES FROM THE REFERENCE DELIBERATELY.  widget_specs.md §1 gives
 *      a button `control_h` — 40 px against a 15 px face, 2.7x — and ported
 *      faithfully that is what produced the report this rule came from: *"the
 *      toggle is significantly taller than the 'load packages at boot' text
 *      next to it."*  Recorded here as a decision so that somebody holding our
 *      screen against the catalogue reads it as one, not as drift.
 *
 *   2. A CONTROL IS AS WIDE AS ITS CONTENT — `cp_text_w()` plus
 *      2 x `cp_ctrl_pad_x()`.  FILLING THE ROW IS THE EXCEPTION and has to be
 *      asked for (`UI_FILL_W`), because the default was the other way round and
 *      that is how the Control Panel ended up with a full-width Save button:
 *      nobody chose it, it was simply what a child of a column got.
 *
 *   3. A BUTTON'S LABEL IS CENTRED on both axes — and measured with
 *      `cp_text_w`, never `strlen * cp_fw()`, or the centring is off by the
 *      difference between a digit's advance and the real one.
 *
 *   4. ANYTHING SQUARE STANDING NEXT TO TEXT — the checkbox's box, the radio's
 *      dot, the switch's knob — is derived from `cp_fh()`, so it tracks the
 *      type instead of staying a 2007 pixel count while the type scales.
 * ------------------------------------------------------------------------- */
int cp_ctrl_h(void);
/* Rule 0's other half: the height of a control you AIM at — the design's own
 * `control_h` token (comfort 40, compact 32, through cp_px()).  Buttons,
 * dropdowns and text boxes; never a checkbox or a switch. */
int cp_btn_h(void);
int cp_ctrl_pad_x(void);
int               cp_font_scale(void);   /* 1 or 2 */
int               cp_icon_size(void);    /* 24, 32 or 48 */

/* Which typeface machinery draws the text (§M69).  BITMAP is the 8x8 font this
 * system had for its whole life; VECTOR is the design's real faces.
 *
 * IT IS A CHOICE AND NOT A DEAD FLAG.  The boot console and the terminal grid
 * still run on the bitmap path, so it has to keep working; and a face that
 * rendered wrongly would otherwise leave a machine with no legible text and no
 * way to ask it anything — the same argument §M23 makes for a taskbar button
 * that is always drawn. */
enum { CP_FONT_BITMAP = 0, CP_FONT_VECTOR = 1 };
int               cp_font_kind(void);

/* The `theme` command.  Lives in theme.c, not in a shell: shell.c is x86-only
 * and aarch64 runs its own serial_shell.c (§M24). */
void cp_cmd_theme(const char* arg);

/* --- the shape language (cp_draw.c) --------------------------------------
 * The design is not a palette: these are the forms the tokens are painted in.
 * A "plate" is a flat fill + a 1 px border with one pixel cut from each
 * corner — the design's own stand-in for radius 4 on a renderer with no curve
 * primitive.  See cp_draw.c for the four rules and where each comes from. */
struct gfx_surface;
void cp_fill_plate(struct gfx_surface* s, int x, int y, int w, int h,
                   cp_color fill);
void cp_border(struct gfx_surface* s, int x, int y, int w, int h, cp_color line);
/* §M69 — the design's DASHED 1 px border (§11's empty state).  Dashed rather
 * than solid is doing real work there: a solid box reads as a container that
 * happens to be empty, a dashed one reads as a placeholder — which is exactly
 * the distinction the empty state exists to make. */
void cp_border_dashed(struct gfx_surface* s, int x, int y, int w, int h,
                      cp_color line);

/* A rounded rectangle as a 1 px staircase — the design's own fallback for a
 * renderer with no curve primitive and no anti-aliasing (widget_specs.md, "Ha
 * nincs antialiasing").  `r` is clamped to half the smaller side, so
 * r = h/2 gives a stadium and r = w/2 on a square gives a circle: that is how
 * the switch's track, its knob and the slider's thumb are drawn.  Integer only.
 * `cp_plate_round` adds the 1 px border, which is the same shape one pixel in. */
void cp_fill_round(struct gfx_surface* s, int x, int y, int w, int h, int r,
                   cp_color fill);
void cp_plate_round(struct gfx_surface* s, int x, int y, int w, int h, int r,
                    cp_color fill, cp_color line);
void cp_plate(struct gfx_surface* s, int x, int y, int w, int h,
              cp_color fill, cp_color line);
void cp_focus_ring(struct gfx_surface* s, int x, int y, int w, int h);
void cp_shadow_edge(struct gfx_surface* s, int x, int y, int w, int h,
                    const cp_shadow* sh);
void cp_dim(struct gfx_surface* s, int x, int y, int w, int h);
void cp_label(struct gfx_surface* s, int x, int y, const char* str);
int  cp_label_width(const char* str);

/* Font metrics AT THE CURRENT SCALE.  The toolkit uses these in place of
 * GFX_GLYPH_W / GFX_GLYPH_H / gfx_text so that measuring and drawing scale
 * together; see cp_draw.c.  Code outside the toolkit (the terminal grid, the
 * boot splash) keeps the raw constants on purpose — a terminal's cell size is
 * its own contract, not a UI preference. */
/* `cp_fw` is the NOMINAL advance — the width of a digit.  Correct for a column
 * of numbers and for the terminal, APPROXIMATE for anything else, because the
 * real face is proportional (§M69).  Anything that lays out a string must use
 * `cp_text_w`; `strlen * cp_fw()` is the habit the 8x8 font taught and it is
 * wrong for every proportional typeface ever cut. */
int  cp_fw(void);
int  cp_fh(void);
int  cp_text_w(const char* str);

/* A DESIGN PIXEL, IN OURS.  The reference is drawn on a 1400 px canvas, so its
 * numbers are proportions of a screen rather than absolute pixels — see the
 * measurement in cp_draw.c.  Everything laid out from the design's figures
 * should come through here; what still does not is the raw CP_* geometry
 * below, which is 1:1 and therefore proportionally small on a wide display.
 * That is a known gap with a number behind it, not an oversight. */
int  cp_px(int design_px);

/* The chrome's heights, already in DEVICE pixels — the same conversion the
 * density gets.  Functions rather than macros because the scale is a runtime
 * fact (§M61: the resolution can change under a running desktop). */
int cp_titlebar_h(void);
int cp_panel_h(void);
int cp_taskbar_h(void);
int cp_window_btn(void);
int cp_menu_item_h(void);
int cp_tab_h(void);
int cp_scrollbar_w(void);
void cp_text(struct gfx_surface* s, int x, int y, const char* str, cp_color col);

/* The monospaced face, for the terminal grid and for the design's numeric
 * columns — which it sets in IBM Plex Mono precisely so that digits align. */
int  cp_mono_cell_w(void);

/* §M69 — THE TERMINAL CELL.  A terminal is a GRID and stays one: §M58's
 * selection is addressed in cells, and a proportional face would make "column
 * 12" meaningless.  What was wrong was not the grid but the CELL SIZE — a
 * literal 8x8 while §M69 made the type a runtime fact, so the terminal was the
 * one window still rendering at a fixed size on a screen whose resolution is a
 * runtime choice.  The design sets terminals and tabular data in IBM Plex Mono
 * for exactly this reason: a fixed advance is what a grid needs, and a fixed
 * advance is not the same thing as a fixed SIZE. */
int  cp_cell_w(void);
int  cp_cell_h(void);
void cp_mono_text(struct gfx_surface* s, int x, int y, const char* str,
                  cp_color col);

/* --- geometry, rescaled; the design's own value follows each ------------- */

#define CP_BORDER               1     /* design 1                            */
#define CP_RADIUS_OUTER        1     /* design 4  — a 1 px corner cut       */
#define CP_RADIUS_INNER        1     /* design 3  — same, inside a tray     */

#define CP_CHECKBOX            20     /* design 20                           */
#define CP_RADIO               20     /* design 20                           */
#define CP_RADIO_DOT           10     /* design 10                           */

#define CP_SWITCH_W            48     /* design 48                           */
#define CP_SWITCH_H            26     /* design 26                           */
#define CP_SWITCH_KNOB         18     /* design 18                           */
#define CP_SWITCH_INSET        3     /* design 3  — floor, not scaled       */
#define CP_SWITCH_TRAVEL       22     /* design 22 — = W - KNOB - 2*INSET    */

#define CP_SLIDER_TRACK_H      6     /* design 6                            */
#define CP_SLIDER_THUMB        22     /* design 22                           */
#define CP_PROGRESS_H          8     /* design 8                            */
#define CP_SPINNER             20     /* design 20                           */
#define CP_SPINNER_STROKE      2     /* design 2  — floor, not scaled       */

/* §M69 — WIDER THAN THE DESIGN'S 12, and that is a consequence of a divergence
 * we already made rather than a new one.  The catalogue's bar is 12 px because
 * it has NO arrow buttons and is not draggable — it is an indicator you click
 * the trough of.  Ours has arrows and a grabbable thumb, which makes it a
 * TARGET, and a 12 px target (16 device px at this density) is what produced
 * *"I cannot grab the bar, and the arrow works sometimes."*  *A control whose
 * hit area is too small is not a smaller control; it is an unreliable one.* */
#define CP_SCROLLBAR_W         16     /* design 12 + the arrows we added      */
#define CP_SCROLLBAR_INSET     2     /* design 2  — floor, not scaled       */
#define CP_SCROLLBAR_THUMB_MIN 28     /* design 28                           */

#define CP_TITLEBAR_H          38     /* design 38                           */
#define CP_PANEL_H             34     /* design 34                           */
#define CP_TASKBAR_H           46     /* design 46                           */
#define CP_WINDOW_BTN          26     /* design 26                           */
#define CP_MENU_ITEM_H         34     /* design 34                           */
#define CP_MENU_PAD            6     /* design 6                            */
#define CP_TAB_H               32     /* design 32                           */
#define CP_TAB_PAD_X           16     /* design 16                           */
#define CP_TRAY_PAD            3     /* design 3                            */
#define CP_TABLE_HEADER_H      30     /* design 30                           */

/* The focus ring does NOT scale: 2 px is already the minimum at which a ring
 * reads as a ring, and the design's rule — never leave a control without one —
 * matters more than the proportion. */
#define CP_FOCUS_RING           2     /* design 2                            */
#define CP_FOCUS_OFFSET         1     /* design 1                            */

/* --- typography ---------------------------------------------------------- */

/* One font, one size.  These exist so call sites read as intent rather than as
 * a bare 8, and so the day a second size arrives there is one place to change.
 * CP_FONT_TITLE is the one place a 2x integer scale is defensible: a title is
 * short, and blockiness there buys the hierarchy the font cannot otherwise
 * give (DESIGN_TARGET.md §4). */
/* THE DESIGN'S OWN SIZES, honoured at last (§M69).
 *
 * These were both 8 because there was one 8x8 bitmap face and no size to
 * choose — the design's numbers sat in the comments as a record of what could
 * not be done.  With outlines they are the actual render sizes, and they differ
 * per density because the design specifies them that way: a compact row does
 * not merely pack the same text tighter, it sets it a point smaller. */
#define CP_FONT_BODY           15     /* design 15 (Barlow), comfort         */
#define CP_FONT_MONO           14     /* design 14 (IBM Plex Mono), comfort  */
#define CP_FONT_BODY_COMPACT   14     /* design 14                           */
#define CP_FONT_MONO_COMPACT   13     /* design 13                           */
#define CP_FONT_LABEL           8     /* design 11 UPPERCASE, 0.14em         */
#define CP_FONT_TITLE          16     /* design 22 — 2x the 8x8 bitmap       */
#define CP_LABEL_TRACKING       1     /* extra px per glyph for the label    */
#define CP_LABEL_PX            11     /* design 11 (Barlow Condensed 600)    */

/* --- motion: RECORDED, NOT IMPLEMENTED ----------------------------------- */
/* Nothing reads these.  See "WHAT DOES NOT SURVIVE". */
#define CP_SWITCH_MS          120
#define CP_THEME_MS           140
#define CP_SPINNER_MS         850
#define CP_INDETERMINATE_MS  1400

/* --- state ---------------------------------------------------------------
 * The design's state row, in the order widget_specs.md §0 lists it.  Every
 * clickable element answers for all five; the spec is explicit that retrofitting
 * them later is the expensive path. */
enum cp_state {
    CP_REST = 0,
    CP_HOVER,
    CP_PRESSED,
    CP_FOCUSED,
    CP_DISABLED,
};

/* The design's disabled rule: composite the whole widget at 45%.  Kept as a
 * constant because it is applied by several widgets and a drifting copy would
 * make two controls disagree about what "off" looks like. */
#define CP_DISABLED_ALPHA     115     /* 45% of 255 */

#endif /* CONSOLE_PLATE_H */
