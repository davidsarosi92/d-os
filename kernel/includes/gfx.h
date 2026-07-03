/* =============================================================================
 * gfx.h — pixel surfaces + drawing primitives (M22 stage 1+2).
 *
 * A `gfx_surface` is a rectangle of 32-bpp ARGB pixels.  It either wraps
 * the real framebuffer (`gfx_fb_surface`) or owns an off-screen buffer
 * allocated from kmalloc (`gfx_surface_init`).  Every primitive clips
 * against the destination surface, so callers may pass partially (or
 * fully) off-surface rectangles without pre-clamping.
 *
 * Pixel format: 0xAARRGGBB.  The alpha channel is ignored by the opaque
 * primitives (fill/blit/line/text) and honoured by the `gfx_blend_*`
 * variants, which do a classic src-over with an 8-bit alpha.
 *
 * Portability: pure C on a linear 32-bpp buffer — no arch code, no port
 * I/O.  The only platform knowledge (where the framebuffer lives) comes
 * from fb_terminal.c through `fb_get_info`, which works on both i386
 * and x86_64 because the mb2→mb1 translator fills the same mboot fields.
 * ============================================================================= */

#ifndef GFX_H
#define GFX_H

#include <stdint.h>

struct gfx_surface {
    int       w, h;        /* size in pixels */
    int       stride;      /* pixels (NOT bytes) per scanline */
    uint32_t* px;          /* top-left pixel */
    int       owns_px;     /* 1 = kmalloc'd by gfx_surface_init, freeable */
};

/* Allocate an off-screen w×h surface (contents undefined).  Returns 0
 * on success, -1 on OOM / bad size. */
int  gfx_surface_init(struct gfx_surface* s, int w, int h);

/* Free an off-screen surface's pixels (no-op for framebuffer wrappers). */
void gfx_surface_free(struct gfx_surface* s);

/* Wrap the live framebuffer.  Returns 0 on success, -1 if there is no
 * usable 32-bpp framebuffer (VGA text fallback boot). */
int  gfx_fb_surface(struct gfx_surface* out);

/* Opaque primitives (alpha ignored). */
void gfx_fill(struct gfx_surface* s, int x, int y, int w, int h, uint32_t color);
void gfx_line(struct gfx_surface* s, int x0, int y0, int x1, int y1, uint32_t color);

/* Copy a w×h block from src(sx,sy) to dst(dx,dy).  Clips against both
 * surfaces.  Surfaces must not overlap (they never do today: window
 * content → backbuffer → framebuffer is a strict pipeline). */
void gfx_blit(struct gfx_surface* dst, int dx, int dy,
              const struct gfx_surface* src, int sx, int sy, int w, int h);

/* src-over alpha fill: color's AA channel blends it onto the surface.
 * AA=0xFF behaves like gfx_fill, AA=0x00 is a no-op. */
void gfx_blend_fill(struct gfx_surface* s, int x, int y, int w, int h,
                    uint32_t argb);

/* Vertical gradient fill from `top` to `bottom` color (used by the
 * compositor wallpaper; cheap per-row interpolation). */
void gfx_vgradient(struct gfx_surface* s, int x, int y, int w, int h,
                   uint32_t top, uint32_t bottom);

/* Draw NUL-terminated ASCII with the embedded 8×8 font.  Background
 * pixels are left untouched (transparent), so fill the area first if
 * a solid background is wanted. */
void gfx_text(struct gfx_surface* s, int x, int y, const char* str, uint32_t fg);

/* Glyph cell size of the embedded font (8×8) — exported so layout math
 * in the GUI does not hard-code magic 8s. */
#define GFX_GLYPH_W 8
#define GFX_GLYPH_H 8

#endif
