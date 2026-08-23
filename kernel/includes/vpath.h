/* =============================================================================
 * vpath.h — filled vector polygons, rendered at any size (§M62).
 *
 * The boot logo has to look right at 640×480 and at 3840×2160, and §M61 made
 * the resolution a runtime choice — so a bitmap is wrong by construction: it is
 * sharp at exactly one size.  The artwork is therefore stored as POLYGONS in a
 * resolution-independent fixed-point box and rasterised at the size actually
 * needed.
 *
 * WHAT IS NOT HERE, DELIBERATELY: an SVG parser.  Ring 0 would gain XML, a path
 * grammar, transforms and styles — everything that makes SVG general and none
 * of what a logo needs.  `scripts/svg2paths.py` does that work at BUILD time
 * and emits the table below; this side is a scanline filler with no parser,
 * which cannot fail on malformed input because there is no input to malform.
 *
 * Coordinates are integers (0..`scale`) because the kernel has no floating
 * point available without saving FP state on every context switch (§A2).
 * ============================================================================= */

#ifndef VPATH_H
#define VPATH_H

#include <stdint.h>

struct gfx_surface;

struct vpath {
    const int16_t (*pts)[2];    /* every polygon's points, concatenated     */
    const uint16_t* offs;       /* npoly+1 offsets into pts[]               */
    int npoly;
    int npts;
    int scale;                  /* the coordinate box: 0..scale on both axes */
};

/* Fill `vp` into `dst`, scaled to fit `size` pixels square with its top-left at
 * (x, y), in `colour`.  Anti-aliased: the glyph edges are what a logo IS, and
 * an aliased logo at 4 K looks worse than no logo.
 *
 * Uses the EVEN-ODD rule, which is what makes counters (the hole in a "d") come
 * out as holes without the table carrying winding directions. */
void vpath_fill(struct gfx_surface* dst, const struct vpath* vp,
                int x, int y, int size, uint32_t colour);

#endif
