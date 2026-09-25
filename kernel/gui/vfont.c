/* =============================================================================
 * vfont.c — glyph rasteriser and cache.  See vfont.h for why this is not simply
 * vpath.c with a character table.
 *
 * THE SHAPE OF THE THING: rasterise once, blit many.  A glyph is turned into an
 * 8-bit coverage bitmap the first time it is asked for at a given size, and
 * every later draw is a blend of that bitmap.  Which is what makes the quality
 * decisions affordable — non-zero winding and exact horizontal coverage are
 * both more work per pixel than vpath's loop, and neither is on the hot path,
 * because the hot path is the cache hit.
 *
 * WITHOUT THE CACHE THIS WOULD BE UNUSABLE, and not by a small margin: the
 * terminal alone paints thousands of glyphs per frame, each needing an edge
 * walk per scanline.  The 8x8 font it replaces was a table lookup and eight
 * shifts.
 *
 * INTEGER ONLY, like everything else drawn in ring 0 (§A2): coordinates are
 * 16.16 fixed point.  The font's own units are integers, the scale is 16.16,
 * and the product of a 1000-unit coordinate with the largest scale this renders
 * at fits an int32 with room to spare — checked rather than assumed, because
 * the failure would be a glyph that is correct at 15 px and folds in half at 96.
 * ============================================================================= */

#include "vfont.h"
#include "gfx.h"
#include "kmalloc.h"
#include "lock.h"
#include "klog.h"
#include <stddef.h>

#define SUB          5      /* vertical subsamples per output row              */
#define MAX_GLYPH_W  192    /* bounds the stack accumulator below              */
#define MAX_CROSS    128    /* crossings on one scanline; a glyph is not busy   */

#define CACHE_SLOTS  1024   /* open-addressed, power of two                    */
#define CACHE_BYTES  (1024 * 1024)

struct centry {
    const struct vfont* f;
    uint16_t gid, px;
    int16_t  w, h, bx, by;   /* bitmap size and its offset from the pen        */
    uint8_t* bm;
    uint32_t stamp;          /* for eviction; see pick_victim                  */
    /* Tasks blitting from this entry right now (2026-09-25).  get_glyph hands
     * the entry out AFTER dropping the lock, and the caller reads `bm`, `w`
     * and `h` without it — so another task's eviction could free the bitmap,
     * or hand the slot to a different glyph, mid-blit.  The desktop, the
     * compositor and every app-host draw text concurrently.  An entry with
     * users is never evicted; vfont_draw drops its use after the blit. */
    uint16_t users;
};

static struct centry cache[CACHE_SLOTS];
static spinlock_t cache_lock = SPINLOCK_INIT;
static unsigned cache_n, cache_bytes, cache_hit, cache_miss, cache_evict;
static uint32_t cache_clock;

/* ------------------------------------------------------------------------- */

/* A font-unit coordinate into DEVICE 16.16.
 *
 * `sc` is already 16.16, and a font coordinate is a plain integer, so the
 * product IS 16.16 and there is nothing to shift.  The first version ran this
 * through a `fixmul` that shifted the result down by 16 — the helper is right
 * for multiplying two fixed-point numbers and wrong for scaling an integer by
 * one — and then the callers shifted AGAIN to get pixels.  Every glyph came out
 * three pixels square: the layout advanced correctly, the boxes were the right
 * height, and not one letter was visible.  A scale applied twice does not look
 * like a scale bug; it looks like the text never got drawn. */
static inline int32_t dev16(int p, int sc) { return (int32_t)p * (int32_t)sc; }

/* Rasterise one glyph at `px` into a freshly allocated coverage bitmap.
 *
 * Returns NULL for a glyph with no ink (a space), which is not a failure and
 * must not be treated as one — the advance still applies. */
static uint8_t* raster(const struct vfont* f, const struct vfont_glyph* g,
                       int px, int* out_w, int* out_h, int* out_bx, int* out_by) {
    *out_w = *out_h = *out_bx = *out_by = 0;
    if (!g->nctr) return NULL;

    /* font units -> pixels, 16.16 */
    int sc = (int)(((int64_t)px << 16) / (f->upem ? f->upem : 1000));

    /* The ink box in device pixels.  Device y runs DOWN and font y runs UP, so
     * the top of the box comes from ymax.  One pixel of slack on each side
     * absorbs the anti-aliased edge, which would otherwise be clipped off — a
     * glyph missing its outermost 30 % row looks thin rather than broken, which
     * is the kind of defect that survives review. */
    int x0 = (dev16(g->xmin, sc) >> 16) - 1;
    int x1 = (dev16(g->xmax, sc) >> 16) + 2;
    int y0 = (-dev16(g->ymax, sc) >> 16) - 1;
    int y1 = (-dev16(g->ymin, sc) >> 16) + 2;

    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || w > MAX_GLYPH_W || h > MAX_GLYPH_W * 2) return NULL;

    uint8_t* bm = kmalloc((size_t)w * h);
    if (!bm) return NULL;
    for (int i = 0; i < w * h; i++) bm[i] = 0;

    /* Every edge is transformed into device 16.16 ONCE, so the scanline loop
     * below is pure device space with no per-row division by the scale. */
    int32_t acc[MAX_GLYPH_W];

    for (int py = 0; py < h; py++) {
        for (int i = 0; i < w; i++) acc[i] = 0;

        for (int s = 0; s < SUB; s++) {
            /* sample row centre, device 16.16 */
            int32_t ys = ((int32_t)py << 16) + (int32_t)((65536 * (2 * s + 1)) / (2 * SUB));

            int32_t xs[MAX_CROSS];
            int8_t  dir[MAX_CROSS];
            int nx = 0;

            for (unsigned c = 0; c < g->nctr && nx < MAX_CROSS - 1; c++) {
                unsigned s0 = (c == 0) ? g->first_pt
                                       : f->ctr_end[g->first_ctr + c - 1];
                unsigned e0 = f->ctr_end[g->first_ctr + c];
                for (unsigned k = s0; k + 1 < e0 && nx < MAX_CROSS - 1; k++) {
                    int32_t ay = -dev16(f->pts[k][1], sc)     - ((int32_t)y0 << 16);
                    int32_t by = -dev16(f->pts[k + 1][1], sc) - ((int32_t)y0 << 16);
                    if ((ay <= ys) == (by <= ys)) continue;
                    int32_t ax = dev16(f->pts[k][0], sc)     - ((int32_t)x0 << 16);
                    int32_t bx = dev16(f->pts[k + 1][0], sc) - ((int32_t)x0 << 16);
                    int32_t t = (int32_t)((((int64_t)(ys - ay)) << 16) / (by - ay));
                    xs[nx]  = ax + (int32_t)((((int64_t)(bx - ax)) * t) >> 16);
                    dir[nx] = (by > ay) ? 1 : -1;
                    nx++;
                }
            }
            if (nx < 2) continue;

            /* insertion sort, carrying the direction with its crossing — sorting
             * the two arrays separately is a bug that only shows on glyphs with
             * more than two contours crossing one scanline */
            for (int i = 1; i < nx; i++) {
                int32_t v = xs[i];
                int8_t  d = dir[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > v) {
                    xs[j + 1] = xs[j];
                    dir[j + 1] = dir[j];
                    j--;
                }
                xs[j + 1] = v;
                dir[j + 1] = d;
            }

            /* NON-ZERO: a span stays open while the winding is not zero.  Under
             * even-odd, two overlapping contours punch a hole — which is what
             * an accented composite would do to exactly the Latin-2 glyphs. */
            int wind = 0;
            int32_t start = 0;
            for (int i = 0; i < nx; i++) {
                if (wind == 0) start = xs[i];
                wind += dir[i];
                if (wind != 0) continue;

                int32_t a = start, b = xs[i];
                if (b <= 0 || a >= ((int32_t)w << 16)) continue;
                if (a < 0) a = 0;
                if (b > ((int32_t)w << 16)) b = (int32_t)w << 16;

                int i0 = a >> 16, i1 = b >> 16;
                if (i0 == i1) {
                    if (i0 < w) acc[i0] += b - a;
                } else {
                    /* EXACT horizontal coverage: the partial pixel at each end
                     * is what makes a one-pixel stem look like a stem instead
                     * of a staircase.  vpath.c fills whole pixels, which is
                     * fine for a 600 px logo and not for 15 px text. */
                    if (i0 < w) acc[i0] += 65536 - (a & 0xFFFF);
                    for (int i2 = i0 + 1; i2 < i1 && i2 < w; i2++)
                        acc[i2] += 65536;
                    if (i1 < w) acc[i1] += b & 0xFFFF;
                }
            }
        }

        uint8_t* row = bm + (size_t)py * w;
        for (int i = 0; i < w; i++) {
            int v = (acc[i] / SUB) >> 8;          /* 0..256 */
            row[i] = (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
        }
    }

    *out_w = w;
    *out_h = h;
    *out_bx = x0;
    *out_by = y0;
    return bm;
}

/* ------------------------------------------------------------------------- */

static unsigned slot_of(const struct vfont* f, unsigned gid, unsigned px) {
    unsigned h = (unsigned)(uintptr_t)f;
    h = h * 2654435761u + gid * 40503u + px * 2246822519u;
    return h & (CACHE_SLOTS - 1);
}

/* Evict the least recently used entry.  A clock stamp rather than a real LRU
 * list: the list would need updating on every HIT, which is the one path that
 * must stay cheap, and at this scale the difference between "least recently
 * used" and "old" is not worth a pointer per entry. */
static int evict_one_locked(void) {
    unsigned best = CACHE_SLOTS;
    uint32_t oldest = 0xFFFFFFFFu;
    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        if (!cache[i].bm || cache[i].users) continue;
        if (cache[i].stamp < oldest) { oldest = cache[i].stamp; best = i; }
    }
    if (best == CACHE_SLOTS) return 0;          /* everything is in use */
    cache_bytes -= (unsigned)(cache[best].w * cache[best].h);
    kfree(cache[best].bm);
    cache[best].bm = NULL;
    cache[best].f = NULL;
    cache_n--;
    cache_evict++;
    return 1;
}

static void glyph_put(struct centry* e) {
    if (!e) return;
    spin_lock(&cache_lock);
    if (e->users) e->users--;
    spin_unlock(&cache_lock);
}

/* Find or build the cached bitmap for one glyph.  The rasterisation happens
 * OUTSIDE the lock: it allocates, and holding a spinlock across kmalloc nests
 * the heap's lock inside this one on a path the compositor takes for every
 * character on the screen. */
static struct centry* get_glyph(const struct vfont* f, unsigned gid, int px) {
    unsigned start = slot_of(f, gid, (unsigned)px);

    spin_lock(&cache_lock);
    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        struct centry* e = &cache[(start + i) & (CACHE_SLOTS - 1)];
        if (!e->f) break;                        /* empty: definitely absent */
        if (e->f == f && e->gid == gid && e->px == px) {
            e->stamp = ++cache_clock;
            e->users++;
            cache_hit++;
            spin_unlock(&cache_lock);
            return e;
        }
    }
    cache_miss++;
    spin_unlock(&cache_lock);

    int w, h, bx, by;
    uint8_t* bm = raster(f, &f->glyphs[gid], px, &w, &h, &bx, &by);

    spin_lock(&cache_lock);
    /* Another task may have inserted the same glyph while we were rasterising.
     * Its copy is as good as ours, so ours is dropped rather than replacing a
     * bitmap somebody may be blitting from this instant. */
    for (unsigned i = 0; i < CACHE_SLOTS; i++) {
        struct centry* e = &cache[(start + i) & (CACHE_SLOTS - 1)];
        if (e->f == f && e->gid == gid && e->px == px) {
            e->users++;
            spin_unlock(&cache_lock);
            if (bm) kfree(bm);
            return e;
        }
        if (e->f) continue;

        /* Over budget with every candidate in use: go over rather than spin —
         * the entries being blitted will be evictable a moment from now. */
        while (bm && cache_bytes + (unsigned)(w * h) > CACHE_BYTES &&
               evict_one_locked()) { }

        e->f = f;
        e->gid = (uint16_t)gid;
        e->px = (uint16_t)px;
        e->w = (int16_t)w;  e->h = (int16_t)h;
        e->bx = (int16_t)bx; e->by = (int16_t)by;
        e->bm = bm;
        e->stamp = ++cache_clock;
        e->users = 1;
        cache_n++;
        if (bm) cache_bytes += (unsigned)(w * h);
        spin_unlock(&cache_lock);
        return e;
    }

    /* Table full of live entries and every probe collided: evict and retry
     * once.  Returning NULL here would drop a character silently. */
    evict_one_locked();
    spin_unlock(&cache_lock);
    if (bm) kfree(bm);
    return NULL;
}

/* ------------------------------------------------------------------------- */

/* THREE DIVISIONS PER PIXEL WAS THE PRICE OF THE NEW TYPEFACE.
 *
 * The glyph cache removed the cost of RASTERISING a glyph; it does nothing
 * about the cost of BLENDING one, and text is most of what a desktop paints.
 * The first version divided by 255 three times per covered pixel — on i386
 * under emulation an integer divide is the most expensive thing in this loop by
 * a wide margin, and the compositor's mean frame went from §4.61's measured
 * 22 ms to 38 ms.
 *
 * The lerp form needs no division: d + ((s - d) * a >> 8).  It is a 255ths-vs
 * -256ths approximation, so a fully covered pixel would land one step short of
 * the source colour — which is why a == 255 keeps its early exit.  Everything
 * between is off by at most one level of eight bits, invisible, and the shape
 * of the glyph is carried by WHERE the coverage is, not by its last bit.
 *
 * IT WAS NOT SHOWN TO HELP, AND THAT IS RECORDED HERE RATHER THAN QUIETLY
 * DROPPED.  The theory was that this loop explained the compositor's mean frame
 * rising from §4.61's 22 ms to 38 ms.  Measured: 38.7/39.7/40.1 ms before,
 * 38.0/38.1/38.1 ms after — and two runs of the SAME binary gave 38.0/38.1/38.1
 * and 40.5/44.3/45.3, a 19 % spread.  The change is inside the noise, so TEXT
 * BLENDING IS NOT THE DOMINANT COST and the frame time is still unexplained.
 * §M56.2's lesson in a new costume: measure both sides, and a benchmark whose
 * noise exceeds its effect cannot support a claim either way.  Kept because it
 * is strictly less work and simpler code, NOT because it was proven faster —
 * the next person should not read this as a solved problem. */
static inline uint32_t blend(uint32_t d, uint32_t s, int a) {
    if (a <= 0) return d;
    if (a >= 255) return s;
    int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
    return 0xFF000000u
         | (unsigned)((dr + (((sr - dr) * a) >> 8)) << 16)
         | (unsigned)((dg + (((sg - dg) * a) >> 8)) << 8)
         | (unsigned)( db + (((sb - db) * a) >> 8));
}

static void blit_glyph(struct gfx_surface* s, struct centry* e,
                       int penx, int baseline, uint32_t col) {
    if (!e || !e->bm) return;
    int dx = penx + e->bx, dy = baseline + e->by;

    /* Clipped to the surface's CLIP BOX, not merely its bounds — §M65 made
     * widget_draw_all the one place a clip is set, and text that ignored it
     * would scroll out over the panel exactly as the labels did there. */
    int cx0 = s->clip_x0, cy0 = s->clip_y0, cx1 = s->clip_x1, cy1 = s->clip_y1;

    /* Clip the COLUMN RANGE once, not once per pixel.  The inner loop ran a
     * pair of comparisons for every pixel of every glyph to discover, almost
     * always, that it was inside — the same shape as vpath.c clipping its box
     * up front so its inner loops never test bounds. */
    int xs = dx < cx0 ? cx0 - dx : 0;
    int xe = dx + e->w > cx1 ? cx1 - dx : e->w;
    if (xs >= xe) return;

    for (int y = 0; y < e->h; y++) {
        int py = dy + y;
        if (py < cy0 || py >= cy1) continue;
        const uint8_t* src = e->bm + (size_t)y * e->w;
        uint32_t* row = s->px + (size_t)py * (size_t)s->stride + dx;
        for (int x = xs; x < xe; x++) {
            int a = src[x];
            if (a) row[x] = blend(row[x], col, a);
        }
    }
}

/* ------------------------------------------------------------------------- */

static inline unsigned gid_of(const struct vfont* f, unsigned char c) {
    unsigned g = f->map[c];
    return (g < (unsigned)f->nglyphs) ? g : 0;
}

static inline int advance_px(const struct vfont* f, unsigned gid, int px) {
    int a = f->mono_advance ? f->mono_advance : f->glyphs[gid].advance;
    return (int)(((int64_t)a * px + f->upem / 2) / (f->upem ? f->upem : 1000));
}

int vfont_text_w(const struct vfont* f, int px, const char* str) {
    if (!f || !str || px <= 0) return 0;
    int w = 0;
    for (const unsigned char* p = (const unsigned char*)str; *p; p++)
        w += advance_px(f, gid_of(f, *p), px);
    return w;
}

int vfont_draw(struct gfx_surface* dst, const struct vfont* f, int px,
               int x, int y, const char* str, uint32_t colour) {
    if (!dst || !dst->px || !f || !str || px <= 0) return 0;
    int pen = x;
    for (const unsigned char* p = (const unsigned char*)str; *p; p++) {
        unsigned gid = gid_of(f, *p);
        struct centry* e = get_glyph(f, gid, px);
        blit_glyph(dst, e, pen, y, colour);
        glyph_put(e);
        pen += advance_px(f, gid, px);
    }
    return pen - x;
}

int vfont_ascent(const struct vfont* f, int px) {
    return (int)(((int64_t)f->ascent * px) / (f->upem ? f->upem : 1000));
}

int vfont_descent(const struct vfont* f, int px) {
    return (int)(((int64_t)(-f->descent) * px) / (f->upem ? f->upem : 1000));
}

int vfont_line_h(const struct vfont* f, int px) {
    return (int)(((int64_t)(f->ascent - f->descent + f->line_gap) * px)
                 / (f->upem ? f->upem : 1000));
}

void vfont_cache_stats(unsigned* entries, unsigned* bytes, unsigned* hits,
                       unsigned* misses, unsigned* evictions) {
    if (entries)   *entries   = cache_n;
    if (bytes)     *bytes     = cache_bytes;
    if (hits)      *hits      = cache_hit;
    if (misses)    *misses    = cache_miss;
    if (evictions) *evictions = cache_evict;
}
