/* =============================================================================
 * fb_backend.h — which display device is behind fb_present.h (aarch64).
 *
 * fb_present.h was written when this arch had exactly one display, and
 * virtio_gpu.c implemented it directly.  §M85's sbsa-ref has no virtio at all:
 * its display is a bochs-display on PCIe, a device with a LINEAR framebuffer
 * BAR and a register file — nothing like a virtio command queue.  So the
 * interface stays exactly as it was for its callers, and behind it one of
 * several backends answers.  The first display driver to come up registers;
 * later ones are ignored (one scanout, one console).
 * ============================================================================= */
#ifndef AARCH64_FB_BACKEND_H
#define AARCH64_FB_BACKEND_H

#include <stdint.h>
#include "fb_present.h"

struct fb_backend {
    const char* name;
    void (*flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    int  (*mode_count)(void);
    int  (*mode_get)(int index, struct fb_mode* out);
    int  (*mode_current)(struct fb_mode* out);
    int  (*mode_set)(uint32_t w, uint32_t h, uint32_t bpp);
};

/* 0 when this backend is now the display, -1 if one already is. */
int fb_backend_register(const struct fb_backend* b);
const struct fb_backend* fb_backend_active(void);

#endif
