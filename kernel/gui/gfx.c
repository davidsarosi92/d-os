/* =============================================================================
 * gfx.c — pixel surfaces + drawing primitives (M22 stage 1+2).
 *
 * All primitives clip against the destination surface, so callers never
 * pre-clamp.  Coordinates are plain ints; negative x/y simply clip away.
 *
 * The blit inner loops are straight pixel copies.  No SIMD, no
 * hand-unrolling: under QEMU/TCG the bottleneck is instruction emulation
 * anyway, and on real hardware a 1024×768 full-screen compose is ~3 MiB
 * of streaming writes — fine for a teaching kernel.  If it ever hurts,
 * damage rects (already tracked by the compositor) shrink the work
 * before micro-optimisation would.
 * ============================================================================= */

#include "gfx.h"
#include "kmalloc.h"
#include <stddef.h>

/* fb_terminal.c exports — declared here (not in a header) to keep the
 * dependency one-way, same pattern vc.c uses for the cell-rect API. */
extern int fb_get_info(volatile uint32_t** px, uint32_t* w, uint32_t* h,
                       uint32_t* pitch_bytes);
extern const uint8_t* fb_font_glyph(unsigned char ch);

/* -------------------------------------------------------------------------- */
/* Surface lifecycle.                                                          */
/* -------------------------------------------------------------------------- */

int gfx_surface_init(struct gfx_surface* s, int w, int h) {
    if (!s || w <= 0 || h <= 0) return -1;
    uint32_t* px = (uint32_t*)kmalloc((size_t)w * (size_t)h * 4);
    if (!px) return -1;
    s->w = w; s->h = h; s->stride = w;
    s->px = px;
    s->owns_px = 1;
    return 0;
}

void gfx_surface_free(struct gfx_surface* s) {
    if (!s || !s->px) return;
    if (s->owns_px) kfree(s->px);
    s->px = NULL;
    s->w = s->h = s->stride = 0;
    s->owns_px = 0;
}

int gfx_fb_surface(struct gfx_surface* out) {
    volatile uint32_t* px;
    uint32_t w, h, pitch;
    if (!out) return -1;
    if (fb_get_info(&px, &w, &h, &pitch) != 0) return -1;
    out->w = (int)w;
    out->h = (int)h;
    out->stride = (int)(pitch / 4);
    /* Drop volatile: the compositor is the only writer once the GUI owns
     * the screen, and the final blit is a plain streaming copy. */
    out->px = (uint32_t*)(uintptr_t)px;
    out->owns_px = 0;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Clipping helper — intersect (x,y,w,h) with the surface rect.  Returns 0    */
/* if nothing remains.                                                         */
/* -------------------------------------------------------------------------- */

static int clip_rect(const struct gfx_surface* s, int* x, int* y, int* w, int* h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return (*w > 0 && *h > 0);
}

/* -------------------------------------------------------------------------- */
/* Primitives.                                                                 */
/* -------------------------------------------------------------------------- */

void gfx_fill(struct gfx_surface* s, int x, int y, int w, int h, uint32_t color) {
    if (!s || !s->px) return;
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    uint32_t* row = s->px + (size_t)y * s->stride + x;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) row[i] = color;
        row += s->stride;
    }
}

void gfx_line(struct gfx_surface* s, int x0, int y0, int x1, int y1, uint32_t color) {
    if (!s || !s->px) return;
    /* Bresenham with per-pixel clipping — lines are short (decoration
     * accents), so a clever clip-first variant isn't worth the code. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (x0 >= 0 && x0 < s->w && y0 >= 0 && y0 < s->h)
            s->px[(size_t)y0 * s->stride + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_blit(struct gfx_surface* dst, int dx, int dy,
              const struct gfx_surface* src, int sx, int sy, int w, int h) {
    if (!dst || !dst->px || !src || !src->px) return;

    /* Clip against the source first (moves dx/dy in lockstep), then the
     * destination (moves sx/sy in lockstep). */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx + w > dst->w) w = dst->w - dx;
    if (dy + h > dst->h) h = dst->h - dy;
    if (w <= 0 || h <= 0) return;

    const uint32_t* sp = src->px + (size_t)sy * src->stride + sx;
    uint32_t*       dp = dst->px + (size_t)dy * dst->stride + dx;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) dp[i] = sp[i];
        sp += src->stride;
        dp += dst->stride;
    }
}

void gfx_blend_fill(struct gfx_surface* s, int x, int y, int w, int h,
                    uint32_t argb) {
    if (!s || !s->px) return;
    uint32_t a = argb >> 24;
    if (a == 0)    return;
    if (a == 255) { gfx_fill(s, x, y, w, h, argb); return; }
    if (!clip_rect(s, &x, &y, &w, &h)) return;

    /* Pre-split the source into alpha-weighted channels; the loop then
     * only weighs the destination.  +1/+... rounding skipped: an off-by-
     * one in a shadow tint is invisible. */
    uint32_t sr = ((argb >> 16) & 0xFF) * a;
    uint32_t sg = ((argb >>  8) & 0xFF) * a;
    uint32_t sb = ( argb        & 0xFF) * a;
    uint32_t na = 255 - a;

    uint32_t* row = s->px + (size_t)y * s->stride + x;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            uint32_t d = row[i];
            uint32_t r = (sr + ((d >> 16) & 0xFF) * na) / 255;
            uint32_t g = (sg + ((d >>  8) & 0xFF) * na) / 255;
            uint32_t b = (sb + ( d        & 0xFF) * na) / 255;
            row[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
        row += s->stride;
    }
}

void gfx_vgradient(struct gfx_surface* s, int x, int y, int w, int h,
                   uint32_t top, uint32_t bottom) {
    if (!s || !s->px) return;
    int oy = y;                              /* pre-clip origin for the ramp */
    int oh = h;
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    if (oh <= 1) { gfx_fill(s, x, y, w, h, top); return; }

    /* Signed channel math — the ramp may go dark→light OR light→dark. */
    int tr = (int)((top    >> 16) & 0xFF), tg = (int)((top    >> 8) & 0xFF), tb = (int)(top    & 0xFF);
    int br = (int)((bottom >> 16) & 0xFF), bg = (int)((bottom >> 8) & 0xFF), bb = (int)(bottom & 0xFF);

    uint32_t* row = s->px + (size_t)y * s->stride + x;
    for (int j = 0; j < h; j++) {
        /* Interpolate on the UNclipped ramp so partial redraws match. */
        int t = y + j - oy;
        uint32_t r = (uint32_t)(tr + (br - tr) * t / (oh - 1));
        uint32_t g = (uint32_t)(tg + (bg - tg) * t / (oh - 1));
        uint32_t b = (uint32_t)(tb + (bb - tb) * t / (oh - 1));
        uint32_t c = 0xFF000000u | (r << 16) | (g << 8) | b;
        for (int i = 0; i < w; i++) row[i] = c;
        row += s->stride;
    }
}

void gfx_text(struct gfx_surface* s, int x, int y, const char* str, uint32_t fg) {
    if (!s || !s->px || !str) return;
    for (; *str; str++, x += GFX_GLYPH_W) {
        if (x >= s->w) return;                   /* rest is off-surface */
        const uint8_t* g = fb_font_glyph((unsigned char)*str);
        for (int py = 0; py < GFX_GLYPH_H; py++) {
            int yy = y + py;
            if (yy < 0 || yy >= s->h) continue;
            uint8_t bits = g[py];
            for (int px = 0; px < GFX_GLYPH_W; px++) {
                if (!(bits & (0x80u >> px))) continue;
                int xx = x + px;
                if (xx < 0 || xx >= s->w) continue;
                s->px[(size_t)yy * s->stride + xx] = fg;
            }
        }
    }
}
