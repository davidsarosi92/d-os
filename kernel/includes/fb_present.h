/* =============================================================================
 * fb_present.h — framebuffer presentation backend (M21 Phase I).
 *
 * The linear-framebuffer console (fb_terminal.c) and the GUI compositor render
 * pixels into a plain 32-bpp memory buffer.  How those pixels reach the screen
 * is arch/device-specific, and that glue is the only part of fb_terminal that
 * was ever x86-only (the Bochs-VBE port I/O + the vmm identity map).  Hoisting
 * it behind this interface makes the renderer itself portable, so the *same*
 * fb_terminal.c links on both x86 and aarch64.  Each platform provides one
 * implementation:
 *
 *   x86      → kernel/hal/x86/fb_present.c  — VRAM mapped with 4 MiB PSE PDEs
 *              via the vmm; the linear FB is scanned out directly, so a write
 *              is instantly visible (flush is a no-op).  Also hosts the
 *              Bochs-VBE double-buffer page flip used by the compositor.
 *   aarch64  → kernel/hal/aarch64/virtio_gpu.c — the FB is ordinary RAM (already
 *              mapped Normal-WB by the boot page tables, so map() is a no-op),
 *              but virtio-gpu is a command device: the guest's writes are not
 *              seen until an explicit TRANSFER_TO_HOST_2D + RESOURCE_FLUSH, so
 *              flush() actually does the work.
 * ============================================================================= */

#ifndef FB_PRESENT_H
#define FB_PRESENT_H

#include <stdint.h>

/* Map the framebuffer's physical window [phys, phys+size) writable into the
 * kernel address space.  Returns 0 on success, non-zero on failure.
 *   x86     — identity-maps the VRAM BAR with 4 MiB PSE PDEs via the vmm.
 *   aarch64 — the RAM-backed buffer is already mapped, so this is a no-op. */
int  fb_present_map(uint64_t phys, uint64_t size);

/* Push a dirty rectangle (pixel coordinates) from the framebuffer memory to
 * the scanned-out display.  Every fb_terminal render primitive calls this for
 * the region it touched.
 *   x86     — no-op: the linear FB is the scanout, writes are already live.
 *   aarch64 — virtio-gpu TRANSFER_TO_HOST_2D (rect) + RESOURCE_FLUSH (rect). */
void fb_present_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* =============================================================================
 * §M61 — MODE SETTING.
 *
 * The resolution used to be a CONSTANT: `dd 1920 / dd 1200 / dd 32` in the
 * multiboot header (the x86 boot.s files) and `FB_WIDTH 1280` on ARM.
 * Changing it meant editing assembly and rebuilding.  This is the same seam
 * M21 carved for the flush difference, extended to the one other thing that is
 * genuinely device-specific: telling the display to become a different size.
 *
 *   x86     — Bochs-VBE DISPI registers (the same register file the page flip
 *             already drives), so the framebuffer BAR does not move and only
 *             the geometry + pitch change.
 *   aarch64 — virtio-gpu: a new 2D resource and a new scanout, which also
 *             needs a new CONTIGUOUS framebuffer allocation.
 *
 * A backend that cannot mode-set reports exactly one mode (the current one)
 * and refuses the rest — `fb_mode_count() == 1` is how a caller knows the
 * feature is absent, rather than by a build-time #ifdef. */
struct fb_mode { uint16_t w, h, bpp; };

/* How many modes this display can be asked for, and what they are. */
int  fb_mode_count(void);
int  fb_mode_get(int index, struct fb_mode* out);          /* 0 = ok */
int  fb_mode_current(struct fb_mode* out);

/* Switch the display.  On success the framebuffer's geometry as reported by
 * `fb_get_info` has already been updated, and the caller is responsible for
 * everything above it (surfaces, layout, clients).  Returns 0 on success, and
 * leaves the previous mode untouched on failure — a mode set that half-worked
 * is a black screen nobody can recover from. */
int  fb_mode_set(uint32_t w, uint32_t h, uint32_t bpp);

/* Tell the framebuffer console that the display's geometry (and possibly its
 * base address) has changed.  Implemented by the portable fb_terminal, called
 * by whichever backend just changed the mode — declared HERE rather than as a
 * local `extern` in each backend, which is how the two copies were kept in
 * step by hand until the aarch64 one needed it too. */
void fb_adopt_mode(volatile uint32_t* px, uint32_t w, uint32_t h,
                   uint32_t pitch_bytes);

#endif /* FB_PRESENT_H */
