/* =============================================================================
 * vfont.h — real typefaces, rasterised at whatever size the screen is.
 *
 * The system had ONE font: `font8x8`, 8x8, one bit per pixel, every glyph the
 * same width.  Sharp at exactly one size — and §M61 made the resolution a
 * runtime choice, so the same argument that turned the boot logo into vector
 * artwork (§M62, vpath.h) applies to text, only more so: text is most of what
 * the screen is made of, and it is the one thing the design language could not
 * express at all.
 *
 * TWO THINGS THIS IS NOT, BOTH DELIBERATE:
 *
 *   - NOT A FONT PARSER.  `scripts/ttf2font.py` reads the TrueType tables on
 *     the host and emits the arrays below.  Ring 0 gets points and integers,
 *     which is the same split vpath.h makes and for the same reason: there is
 *     no input here, so there is nothing to malform.
 *
 *   - NOT UNICODE.  `map` is indexed by BYTE, in ISO-8859-2, because that is
 *     what `font8x8[256]` was (§4.66 — ő and ű do not exist in Latin-1), what
 *     the keyboard layers produce, and what every string in this tree holds.
 *     This is a drop-in for the table it replaces; making the system UTF-8 is a
 *     separate piece of work with a much larger blast radius.
 *
 * WHY THIS IS NOT JUST vpath.c WITH A CHARACTER TABLE.  Two differences, both
 * of which decide whether text is legible:
 *
 *   - THE FILL RULE IS NON-ZERO, not even-odd.  A logo's contours never
 *     overlap; a font's do — an accented glyph is a composite, the accent is
 *     placed by transform, and nothing keeps it clear of the letter.  Even-odd
 *     turns such an overlap into a HOLE, which would have broken precisely the
 *     Hungarian glyphs §4.66 added.
 *
 *   - COVERAGE IS EXACT HORIZONTALLY.  vpath.c fills whole pixels per subsample
 *     row, which is fine for a 600 px logo and unacceptable at 15 px, where a
 *     stem is one pixel wide and the difference between 40 % and 60 % coverage
 *     is the difference between a readable letter and a smear.
 *
 * Both cost more per pixel than vpath's loop, and neither matters for
 * throughput, because a glyph is rasterised ONCE per size and then cached —
 * see vfont_draw().
 * ============================================================================= */

#ifndef VFONT_H
#define VFONT_H

#include <stdint.h>

struct gfx_surface;

/* One glyph's outline and metrics, all in FONT UNITS (0..upem), so the same
 * table serves every size. */
struct vfont_glyph {
    uint16_t first_pt;      /* index into vfont.pts                          */
    uint16_t first_ctr;     /* index into vfont.ctr_end                      */
    uint16_t nctr;          /* contour count; 0 is legal (a space)           */
    int16_t  advance;       /* pen movement, font units                      */
    int16_t  xmin, ymin;    /* the ink box — what the cached bitmap covers   */
    int16_t  xmax, ymax;
};

struct vfont {
    const int16_t (*pts)[2];
    const uint16_t* ctr_end;    /* absolute, exclusive end of each contour   */
    const struct vfont_glyph* glyphs;
    const uint16_t* map;        /* byte (ISO-8859-2) -> glyph index          */
    int nglyphs;
    int upem;
    int ascent, descent, line_gap;
    /* Non-zero for a monospaced face.  The terminal needs a fixed cell — §M58's
     * selection is addressed in grid cells and gterm's whole model is a grid —
     * so a face that IS monospaced says so here rather than every caller
     * measuring a glyph and hoping. */
    int mono_advance;
};

/* The faces the design specifies.  Three, because the design uses three and
 * collapsing them would lose the distinction it draws between a heading, a
 * paragraph and a number. */
extern const struct vfont vfont_ui;      /* Barlow — body text              */
extern const struct vfont vfont_label;   /* Barlow Condensed SemiBold       */
extern const struct vfont vfont_mono;    /* IBM Plex Mono — terminal, numbers */

/* Draw `str` with its BASELINE at `y`, returning the advance.  The baseline,
 * not the top: a proportional face has glyphs that descend (g, y, comma) and
 * ones that ascend past the cap height, so a top-left origin would make every
 * caller compute the same offset and one of them would get it wrong. */
int vfont_draw(struct gfx_surface* dst, const struct vfont* f, int px,
               int x, int y, const char* str, uint32_t colour);

/* What `vfont_draw` would advance, without drawing.  Layout MUST use this
 * rather than a character count times a cell width: that assumption is what an
 * 8x8 font let every caller make, and it is wrong for every proportional face
 * ever cut. */
int vfont_text_w(const struct vfont* f, int px, const char* str);

/* Vertical metrics at a pixel size: the distance from the baseline up to the
 * ascender, down to the descender, and the natural line pitch. */
int vfont_ascent(const struct vfont* f, int px);
int vfont_descent(const struct vfont* f, int px);
int vfont_line_h(const struct vfont* f, int px);

/* Rasterise one byte's glyph into an 8-bit coverage bitmap.  Exposed for the
 * cache's own self-test — a rasteriser verified only through the compositor is
 * one whose failures arrive as "the screen looks wrong". */
void vfont_cache_stats(unsigned* entries, unsigned* bytes, unsigned* hits,
                       unsigned* misses, unsigned* evictions);

#endif
