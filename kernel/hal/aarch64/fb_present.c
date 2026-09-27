/* =============================================================================
 * fb_present.c (aarch64) — fb_present.h, routed to whichever display came up.
 *
 * See fb_backend.h for why this layer exists.  With no display at all (a
 * serial-only boot) every call is the honest no-op: nothing to flush, one mode
 * that cannot change, no page flip.
 * ============================================================================= */

#include "fb_backend.h"
#include <stddef.h>

static const struct fb_backend* g_fb;

int fb_backend_register(const struct fb_backend* b) {
    if (g_fb || !b) return -1;
    g_fb = b;
    return 0;
}
const struct fb_backend* fb_backend_active(void) { return g_fb; }

/* Every backend here keeps the pixels in RAM that is already mapped, so there
 * is nothing to map. */
int fb_present_map(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }

void fb_present_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (g_fb && g_fb->flush) g_fb->flush(x, y, w, h);
}

/* No hardware page flip on either backend: gui.c keeps its single-buffer blit
 * followed by a flush of the dirty rect. */
int  fb_flip_init(volatile uint32_t** buf0, volatile uint32_t** buf1) {
    (void)buf0; (void)buf1; return -1;
}
void fb_flip_to(int idx) { (void)idx; }

int fb_mode_count(void) { return (g_fb && g_fb->mode_count) ? g_fb->mode_count() : 1; }
int fb_mode_current(struct fb_mode* out) {
    if (g_fb && g_fb->mode_current) return g_fb->mode_current(out);
    if (!out) return -1;
    out->w = out->h = out->bpp = 0;
    return 0;
}
int fb_mode_get(int index, struct fb_mode* out) {
    if (g_fb && g_fb->mode_get) return g_fb->mode_get(index, out);
    return index == 0 ? fb_mode_current(out) : -1;
}
int fb_mode_set(uint32_t w, uint32_t h, uint32_t bpp) {
    return (g_fb && g_fb->mode_set) ? g_fb->mode_set(w, h, bpp) : -2;
}
