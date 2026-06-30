/* =============================================================================
 * pmm.h — Physical Memory Manager: zoned buddy allocator.
 *
 * Hands out physical frames (4 KiB) or larger power-of-2 page blocks
 * (`page_alloc(order)`) from per-zone free lists.  Memory is split into
 * zones so allocations with physical-address constraints land in the
 * right region:
 *
 *   ZONE_DMA    : pfn  < 4096  (i.e. phys < 16 MiB) — legacy ISA DMA
 *   ZONE_NORMAL : pfn >= 4096                       — the bulk of RAM
 *
 * (HIGHMEM > 256 MiB is reserved as a structural slot but not yet
 *  managed — the i386 identity map only covers 256 MiB and we have no
 *  pressure for more yet.  M19 leaves the abstraction extensible.)
 *
 * Public API is split in two layers:
 *
 *   page_alloc / page_free        — new, order-aware.  Use these for
 *                                   anything larger than a single frame.
 *   pmm_alloc_frame / *_contiguous / pmm_free_frame
 *                                 — legacy 1-frame / N-frame API.
 *                                   Wired on top of page_alloc internally
 *                                   so existing callers don't change.
 *
 * Initialization requires that `mboot_init` has already been called —
 * the PMM walks the multiboot memory map to discover usable frames.
 *
 * Concurrency: each zone has its own spinlock.  All paths are SMP-safe
 * (M18 cmpxchg spinlocks).
 * ============================================================================= */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE      4096
#define PMM_FRAME_SHIFT     12
#define PMM_ALLOC_FAIL      0u      /* OOM sentinel — frame 0 is always reserved */

/* Buddy allocator parameters.  See pmm.c file header for the rationale
 * behind each cap.  Bumping BUDDY_MAX_FRAMES raises the supported
 * physical-memory ceiling (it's a .bss cost: 1 byte per frame).
 *
 * M19.5.1 — per-arch cap.  i386 stays at 1 GiB (identity map is fixed
 * at 256 MiB by vmm.c; the cap is generous since the mmap caps it
 * further if RAM is smaller).  x86_64 bumps to 4 GiB because
 * hal_extend_identity_map can grow the identity map to that range
 * cheaply via 1 GiB PDPT pages.  Real >4 GiB systems would bump
 * further; the page_state[] cost is 1 MiB at 4 GiB, 4 MiB at 16 GiB. */
#define BUDDY_MAX_ORDER     10
#if defined(__x86_64__)
#  define BUDDY_MAX_FRAMES  (1u << 20)      /* 4 GiB cap, 1 MiB metadata */
#else
#  define BUDDY_MAX_FRAMES  (1u << 18)      /* 1 GiB cap, 256 KiB metadata */
#endif

/* Zone identifiers.  Allocators that don't care should pass
 * ZONE_DEFAULT — which falls back NORMAL → DMA → fail. */
#define ZONE_DMA            0
#define ZONE_NORMAL         1
#define ZONE_HIGHMEM        2      /* reserved; not populated today */
#define NR_ZONES            3
#define ZONE_DEFAULT        (-1)

/* Build the buddy free lists from the multiboot mmap, reserve protected
 * regions (kernel image, low memory, multiboot info).  Call once at
 * boot after `mboot_init`. */
void pmm_init(void);

/* ---------------------------------------------------------------------------
 * Order-aware API.  An "order" is a log2 page count: order 0 = one
 * frame (4 KiB), order 1 = 2 frames (8 KiB), ..., order 10 = 1024
 * frames (4 MiB).  Returns the physical base address of the block, or
 * PMM_ALLOC_FAIL on OOM.
 *
 * `zone_hint`: ZONE_DMA / ZONE_NORMAL / ZONE_DEFAULT.  DEFAULT tries
 * NORMAL first then falls back to DMA; DMA returns DMA-only; NORMAL
 * returns NORMAL-only.
 * --------------------------------------------------------------------------- */
uint32_t page_alloc(int order, int zone_hint);
void     page_free (uint32_t phys, int order);

/* ---------------------------------------------------------------------------
 * Legacy 1-frame / N-frame API.  Kept stable so existing drivers
 * (virtio_blk, xhci, ramfs frame slabs) don't need rewrites; internally
 * dispatches to page_alloc / page_free.
 * --------------------------------------------------------------------------- */
uint32_t pmm_alloc_frame(void);
uint32_t pmm_alloc_contiguous(uint32_t n);
void     pmm_free_frame(uint32_t addr);

/* Statistics.  `managed` is the total count of frames the PMM knows
 * about (sum of AVAILABLE mmap regions in frames).  `free` and `used`
 * always add up to `managed`. */
uint32_t pmm_managed_frames(void);
uint32_t pmm_free_frames(void);
uint32_t pmm_used_frames(void);

/* Per-zone free-block count at each order.  `out_free_per_order` must
 * point to an array of at least (BUDDY_MAX_ORDER + 1) uint32_t.  Used
 * by the `buddyinfo` shell command. */
void pmm_zone_stats(int zone, uint32_t* out_free_per_order, uint32_t* out_managed);

/* Human-readable one-line dump.  Used by `meminfo`. */
void pmm_print_stats(void);

#endif
