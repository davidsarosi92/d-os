/* =============================================================================
 * pmm.h — Physical Memory Manager public interface.
 *
 * Hands out 4 KiB page frames of physical memory.  Built on top of a
 * bitmap: one bit per frame, 1 = in use, 0 = free.  A request for a frame
 * scans the bitmap for the first clear bit, sets it, and returns the
 * frame's base physical address.
 *
 * Initialization requires that `mboot_init` has already been called — the
 * PMM walks the multiboot memory map to learn which ranges are usable.
 *
 * Concurrency: all PMM operations currently run from the main (non-IRQ)
 * context.  If an interrupt handler ever needs to allocate, add cli/sti
 * (or irq_save/restore) around the critical section.
 * ============================================================================= */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE      4096
#define PMM_FRAME_SHIFT     12
#define PMM_ALLOC_FAIL      0u      /* returned on out-of-memory — 0 is a safe sentinel
                                     * because frame 0 is always marked in-use */

/* Build the bitmap, pre-populate it from the mmap, reserve protected
 * regions (kernel image, low memory, multiboot info).  Call once at boot
 * after `mboot_init`. */
void pmm_init(void);

/* Allocate one 4 KiB frame.  Returns its physical base address, or
 * PMM_ALLOC_FAIL (0) if no free frame exists. */
uint32_t pmm_alloc_frame(void);

/* Allocate `n` physically-contiguous frames.  Returns the base
 * physical address (4 KiB aligned), or PMM_ALLOC_FAIL on failure.
 * Used by drivers that hand the device a single page-aligned base
 * (legacy virtio queues, DMA buffers, ...).  Scans the bitmap linearly
 * for `n` consecutive zero bits — O(MAX_FRAMES) worst case but fine
 * for the handful of contiguous allocations the kernel does at boot. */
uint32_t pmm_alloc_contiguous(uint32_t n);

/* Free a previously allocated frame.  `addr` must be 4 KiB aligned. */
void pmm_free_frame(uint32_t addr);

/* Statistics.  `managed` is the total count of frames the PMM knows about
 * (sum of AVAILABLE mmap regions in frames).  `free` and `used` always
 * add up to `managed`. */
uint32_t pmm_managed_frames(void);
uint32_t pmm_free_frames(void);
uint32_t pmm_used_frames(void);

/* Dump a human-readable summary to the console.  Used by `meminfo`. */
void pmm_print_stats(void);

#endif
