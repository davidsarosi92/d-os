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

#endif /* FB_PRESENT_H */
