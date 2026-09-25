/* =============================================================================
 * kmap.h — reach a physical frame the kernel has no permanent mapping for.
 *
 * §M86 (2026-09-26).  On x86_64 and aarch64 every frame the PMM manages is in
 * the kernel's direct map, so a frame IS a pointer (phys_to_virt).  On i386
 * the kernel's window onto physical memory is a 1020 MiB identity map — user
 * space starts at 1 GiB and the small code model forbids moving it — so RAM
 * above that line ("highmem") can be MANAGED but not TOUCHED directly.  These
 * two calls are how it is touched: a short-lived mapping in a small per-CPU
 * window, the Linux `kmap_atomic` shape.
 *
 * THE CONTRACT, because each rule is a way to get it wrong:
 *   - Short and LIFO.  Map, touch, unmap, in that order, before anything else
 *     that maps; nested maps (a copy needs two) unmap in reverse order.
 *   - No sleeping in between.  The window slot belongs to this CPU, and the
 *     mapping pins the task to it (preemption is off until kunmap), so a
 *     sleep or a blocking lock between the two calls is a bug.
 *   - Only for frames from pmm_alloc_frame_user() (or any frame, for that
 *     matter): a directly-mapped frame comes back as its plain address and
 *     kunmap of it does nothing, so callers never ask which kind they hold.
 *
 * Which frames may be highmem: ONLY those from pmm_alloc_frame_user() — user
 * pages, whose contents the kernel reaches through a user mapping or through
 * this.  Page tables, heap pages, DMA buffers and everything else come from
 * pmm_alloc_frame(), which never returns highmem, because the code that uses
 * them dereferences the frame address directly.
 * ============================================================================= */
#ifndef KMAP_H
#define KMAP_H

#include "pmm.h"

void* kmap_frame(pmm_phys_t frame);
void  kunmap_frame(void* p);

/* Is this frame outside the kernel's direct map (i.e. reachable only through
 * kmap)?  Always 0 where every frame is directly mapped. */
int pmm_frame_is_highmem(pmm_phys_t frame);

/* A frame for a USER page: may come from highmem, falls back to low memory. */
pmm_phys_t pmm_alloc_frame_user(void);

/* Zero / copy whole frames through kmap (the two things user-page code does). */
void kmap_zero_frame(pmm_phys_t frame);
void kmap_copy_frame(pmm_phys_t dst, pmm_phys_t src);

#endif
