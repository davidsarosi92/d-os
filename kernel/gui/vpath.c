/* =============================================================================
 * vpath.c — scanline polygon filler with anti-aliasing (§M62).  See vpath.h for
 * why the parser lives on the host and only this lives in the kernel.
 *
 * Algorithm: for each output row, sample SUBSAMPLES scanlines inside it; for
 * each scanline collect the x crossings of every polygon edge, sort them, and
 * accumulate coverage between even-odd pairs.  The accumulated coverage per
 * pixel is then blended once.  That is the classic approach, and it is chosen
 * here for three properties this kernel cares about:
 *
 *   - INTEGER ONLY.  Crossings are computed in 16.16 fixed point; there is no
 *     floating point available in kernel context without saving FP state (§A2).
 *   - BOUNDED MEMORY.  One coverage row and one crossing list, both sized from
 *     the destination width and the point count, both on the stack of the
 *     caller's task — no allocation on a path that runs during boot.
 *   - NO STATE.  Nothing is remembered between calls, so it cannot be left
 *     half-initialised by a fault.
 *
 * Anti-aliasing is not decoration here: the logo IS its edges, and an aliased
 * one on a 4 K screen looks worse than none.
 * ============================================================================= */

#include "vpath.h"
#include "gfx.h"
#include <stddef.h>

#define SUBSAMPLES 4                 /* vertical samples per output row       */
#define MAX_CROSS  256               /* crossings per scanline (bounded)      */

/* Blend `src` onto `dst` with 0..255 coverage.  src-over with a solid source,
 * which is all a single-colour fill needs. */
static inline uint32_t blend(uint32_t dst, uint32_t src, int a) {
    if (a <= 0) return dst;
    if (a >= 255) return src;
    unsigned dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    unsigned sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    unsigned r = (sr * a + dr * (255 - a)) / 255;
    unsigned g = (sg * a + dg * (255 - a)) / 255;
    unsigned b = (sb * a + db * (255 - a)) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void sort_int(int* a, int n) {
    /* Insertion sort: n is the number of edge crossings on ONE scanline —
     * single digits for this artwork — and an insertion sort has no recursion,
     * no pivot and no worst case worth thinking about at that size. */
    for (int i = 1; i < n; i++) {
        int v = a[i], j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
}

void vpath_fill(struct gfx_surface* dst, const struct vpath* vp,
                int ox, int oy, int size, uint32_t colour) {
    if (!dst || !dst->px || !vp || !vp->pts || size <= 0) return;
    if (vp->scale <= 0 || vp->npoly <= 0) return;

    /* Clip the drawing box to the surface up front, so the inner loops never
     * test bounds per pixel. */
    int x0 = ox, y0 = oy, x1 = ox + size, y1 = oy + size;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    if (x0 >= x1 || y0 >= y1) return;

    int width = x1 - x0;
    if (width > 4096) width = 4096;              /* coverage row is bounded */
    static uint8_t cov[4096];                    /* one row; see the note below */

    /* `cov` is static rather than on the stack because a 4 KiB frame is more
     * than some kernel stacks here want to give a drawing helper, and this
     * function is never called from two tasks at once: the splash draws it on
     * the boot task, and the desktop would draw it on the compositor.  Stated
     * rather than assumed — if that ever stops being true, this needs a lock or
     * a caller-provided buffer. */

    for (int py = y0; py < y1; py++) {
        for (int i = 0; i < width; i++) cov[i] = 0;

        for (int sub = 0; sub < SUBSAMPLES; sub++) {
            /* Scanline position inside the logo's own coordinate box, in
             * 16.16: (pixel row + sub-offset - origin) / size * scale. */
            int64_t fy = ((int64_t)(py - oy) * SUBSAMPLES + sub * 1 + 0) ;
            int64_t sy = (fy * (int64_t)vp->scale) / ((int64_t)size * SUBSAMPLES);

            int xs[MAX_CROSS];
            int nx = 0;
            for (int p = 0; p < vp->npoly && nx < MAX_CROSS - 1; p++) {
                int s = vp->offs[p], e = vp->offs[p + 1];
                for (int k = s; k + 1 < e && nx < MAX_CROSS - 1; k++) {
                    int ay = vp->pts[k][1],     by = vp->pts[k + 1][1];
                    if ((ay <= sy && by <= sy) || (ay > sy && by > sy)) continue;
                    int ax = vp->pts[k][0],     bx = vp->pts[k + 1][0];
                    /* x where the edge crosses this scanline, back in pixels. */
                    int64_t t = ((sy - ay) * 65536) / (by - ay);
                    int64_t px = ax + (((int64_t)(bx - ax) * t) >> 16);
                    xs[nx++] = (int)(ox + (px * size) / vp->scale);
                }
            }
            if (nx < 2) continue;
            sort_int(xs, nx);

            /* Even-odd: fill between pairs.  A "d" has a hole, and even-odd is
             * what makes the hole a hole without the table carrying winding
             * directions. */
            for (int i = 0; i + 1 < nx; i += 2) {
                int a = xs[i], b = xs[i + 1];
                if (b <= x0 || a >= x1) continue;
                if (a < x0) a = x0;
                if (b > x1) b = x1;
                for (int px = a; px < b; px++) {
                    int idx = px - x0;
                    if (idx < 0 || idx >= width) continue;
                    int c = cov[idx] + (255 / SUBSAMPLES);
                    cov[idx] = (uint8_t)(c > 255 ? 255 : c);
                }
            }
        }

        uint32_t* row = dst->px + (size_t)py * (size_t)dst->stride;
        for (int i = 0; i < width; i++)
            if (cov[i]) row[x0 + i] = blend(row[x0 + i], colour, cov[i]);
    }
}
