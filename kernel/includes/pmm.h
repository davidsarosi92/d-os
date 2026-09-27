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
 * behind each cap.
 *
 * §M48 — THE FRAME CEILING IS NO LONGER A COMPILE-TIME CONSTANT.
 *
 * It used to be `BUDDY_MAX_FRAMES`, a per-arch #define that sized a static
 * `page_state[]` array in .bss.  That construction cannot scale: the metadata
 * is 1 byte per frame, so a build that supports 128 GiB would carry 33 MiB of
 * .bss on EVERY box — including the 256 MiB one that has to load it before it
 * can even run.  Raising the ceiling and keeping small machines bootable are
 * directly opposed as long as the number is baked in.
 *
 * So the ceiling is now DISCOVERED: pmm_init reads the firmware memory map,
 * asks the HAL how far the identity map reaches, and sizes the metadata to the
 * RAM this particular machine actually has (see `pmm_bootmem_alloc`).  One
 * kernel image therefore boots on 128 MiB and on 128 GiB, paying for neither
 * on the other.  The only remaining limits are real ones:
 *
 *   i386    — 32-bit paging means 32-bit physical addresses: 4 GiB, full stop.
 *             (64 GiB would need PAE — a different page-table format, not a
 *             bigger constant.)
 *   x86_64  — hal_extend_identity_map covers 512 GiB via 1 GiB PDPT pages.
 *   aarch64 — the Phase-A MMU identity-maps with 1 GiB blocks.
 */
/* Order 12 = a single contiguous alloc up to 2^12 pages = 16 MiB.  Order
 * 10 (4 MiB) capped surfaces at 1024×1024-ish; the M22.6 move to a
 * 1920×1200 desktop needs 9.2 MiB per full-screen surface (backbuffer +
 * wallpaper), so the compositor's gfx_surface_init must be able to get a
 * >4 MiB contiguous block.  Cost of the bump: two extra free-list heads
 * per zone, and page-alloc's power-of-2 rounding wastes up to ~7 MiB on a
 * 9.2 MiB surface — acceptable for a handful of full-screen surfaces (a
 * vmalloc-style scatter mapping would remove the waste; noted for later). */
#define BUDDY_MAX_ORDER     12

/* Sanity ceiling, NOT a memory-size policy.  A corrupt or hostile memory map
 * claiming petabytes must not make us try to allocate petabytes of metadata,
 * so the discovered frame count is clamped here.  1 << 26 frames = 256 GiB of
 * RAM, whose metadata is 64 MiB — comfortably above both the x86_64 identity
 * map's 512 GiB reach and any machine this kernel will meet. */
#define BUDDY_FRAME_HARD_CAP  (1u << 26)

/* The DISCOVERED frame ceiling: one past the highest pfn the PMM has metadata
 * for.  Valid only after pmm_init; zero before it.  Everything that used to
 * compare against BUDDY_MAX_FRAMES compares against this. */
extern uint32_t pmm_nr_frames;

/* Physical address width: 64 bits on EVERY arch (§M86, 2026-09-26).  i386 kept
 * it at 32 while its page tables could only hold 32-bit addresses; PAE's hold
 * 64, and a frame above 4 GiB has to be nameable before it can be managed.
 * kprintf is format-checked so a `%x` of one is a compile warning, not an
 * argument that silently shifts every later one. */
typedef uint64_t pmm_phys_t;

/* Zone identifiers, ordered by how CONSTRAINED the memory is: a lower zone can
 * satisfy anything a higher one can, but not vice versa.  A hint therefore
 * means "this zone, then fall back DOWNWARD"; ZONE_DEFAULT == ZONE_NORMAL.
 *
 *   ZONE_DMA    phys < 16 MiB   — legacy ISA DMA
 *   ZONE_DMA32  phys < 4 GiB    — anything that addresses DMA with 32 bits
 *   ZONE_NORMAL phys >= 4 GiB   — unconstrained; empty on i386 by construction
 *
 * §M48 — DMA32 is new, and it is not cosmetic.  While the frame ceiling was
 * 4 GiB, "any frame" and "a frame a 32-bit device can reach" were the same
 * thing, so no driver had to say which it wanted.  Past 4 GiB they diverge,
 * and a device handed a 40-bit address it can only store 32 bits of does not
 * fail loudly — it DMAs into whatever the truncated address happens to hit.
 * So every device buffer now names DMA32 explicitly (see the *_dma32 helpers);
 * 64-bit-capable DMA is an opt-in a driver has to earn, one driver at a time.
 */
#define ZONE_DMA            0
#define ZONE_DMA32          1
#define ZONE_NORMAL         2
/* §M86 — frames the kernel cannot address directly (i386: above its 1020 MiB
 * identity map).  Handed out ONLY on request (pmm_alloc_frame_user, kmap.h);
 * ZONE_DEFAULT never falls into it.  Empty wherever everything is mapped. */
#define ZONE_HIGHMEM        3
#define NR_ZONES            4
#define ZONE_DEFAULT        (-1)

/* Frame index of the 4 GiB line — the DMA32 / NORMAL boundary. */
#define ZONE_DMA32_FRAME_LIMIT  (1u << 20)

/* Build the buddy free lists from the multiboot mmap, reserve protected
 * regions (kernel image, low memory, multiboot info).  Call once at
 * boot after `mboot_init`. */
void pmm_init(void);

/* ---------------------------------------------------------------------------
 * Boot-time arena (§M48).
 *
 * The chicken-and-egg problem the dynamic ceiling creates: the buddy allocator
 * cannot hand out its own metadata, and the metadata's size is only known once
 * the memory map has been read.  So pmm_init first reserves one contiguous
 * arena out of the raw memory map — before any free list exists — and carves it
 * out of the pool so the buddy never sees it.  `pmm_bootmem_alloc` bump-
 * allocates from that arena.
 *
 * Intended for structures that are (a) sized from the RAM count and (b) live
 * for the lifetime of the kernel — page_state[] and kmalloc's big-alloc side
 * table.  There is no free.  Returns NULL if the arena is exhausted, and may
 * be called after pmm_init (the arena stays reserved), which is what lets
 * kmalloc_init size its own table.
 * --------------------------------------------------------------------------- */
void* pmm_bootmem_alloc(uint32_t bytes);

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
pmm_phys_t page_alloc(int order, int zone_hint);

/* §M33 — a run of 2^order frames entirely below `limit`, for a device whose
 * address register is narrower than any zone boundary.  The zones express three
 * widths and real devices are not that tidy; see the implementation for why the
 * obvious answer (ZONE_DMA) is empty on a machine with a 60 MiB kernel image.
 * PMM_ALLOC_FAIL rather than a near miss — a device truncating an address it
 * cannot hold is the failure this exists to prevent. */
pmm_phys_t page_alloc_below(int order, pmm_phys_t limit);
void       page_free (pmm_phys_t phys, int order);

/* ---------------------------------------------------------------------------
 * Legacy 1-frame / N-frame API.  Kept stable so existing drivers
 * (virtio_blk, xhci, ramfs frame slabs) don't need rewrites; internally
 * dispatches to page_alloc / page_free.
 * --------------------------------------------------------------------------- */
pmm_phys_t pmm_alloc_frame(void);

/* §M86 — seed the frames that were unreachable at pmm_init (a 32-bit kernel's
 * RAM above 4 GiB).  kernel_main calls it right after vmm_init. */
void pmm_seed_deferred(void);
pmm_phys_t pmm_alloc_contiguous(uint32_t n);
void       pmm_free_frame(pmm_phys_t addr);

/* The same two, restricted to memory a 32-bit DMA engine can address.  Use
 * these for anything whose address is handed to hardware through a 32-bit
 * register or descriptor field.  On i386 they are indistinguishable from the
 * plain versions; on a large 64-bit machine they are the difference between a
 * working device and silent memory corruption. */
pmm_phys_t pmm_alloc_frame_dma32(void);
pmm_phys_t pmm_alloc_contiguous_dma32(uint32_t n);
/* Contiguous frames reachable by a device with `addr_bits` of DMA address. */
pmm_phys_t pmm_alloc_contiguous_dma(uint32_t n, int addr_bits);

/* Release a run obtained from pmm_alloc_contiguous* .
 *
 * It takes `n` — the SAME count the allocation asked for — and not just the
 * address, because a contiguous run is one buddy block of order ceil_log2(n):
 * freeing it as `n` separate order-0 frames corrupts the allocator's
 * accounting, and there is no way to recover the order from the address alone.
 * The symmetry is the contract: free with the count you allocated with. */
void       pmm_free_contiguous(pmm_phys_t addr, uint32_t n);

/* Statistics.  `managed` is the total count of frames the PMM knows
 * about (sum of AVAILABLE mmap regions in frames).  `free` and `used`
 * always add up to `managed`. */
/* §M85 — the lowest pfn the metadata covers (0 unless RAM starts high). */
extern uint32_t pmm_pfn_base;
uint32_t pmm_managed_frames(void);
uint32_t pmm_free_frames(void);
uint32_t pmm_used_frames(void);

/* Per-zone free-block count at each order.  `out_free_per_order` must
 * point to an array of at least (BUDDY_MAX_ORDER + 1) uint32_t.  Used
 * by the `buddyinfo` shell command. */
void pmm_zone_stats(int zone, uint32_t* out_free_per_order, uint32_t* out_managed);

/* ---------------------------------------------------------------------------
 * §M19.5.3 — NUMA.  One zone set per node; page_alloc prefers the running
 * CPU's node and falls back to the others before descending a zone.  Ranges
 * come from SRAT (x86) or the device tree (ARM), are recorded with
 * pmm_numa_add_range and applied ONCE by pmm_numa_commit, which moves every
 * free block to its node.  A machine that is not NUMA is node 0 throughout.
 * --------------------------------------------------------------------------- */
struct pmm_node_info {
    uint32_t managed, free;         /* frames                                  */
    uint32_t hit, miss;             /* allocations served locally / remotely   */
};
int        pmm_numa_add_range(uint64_t base, uint64_t len, int node);
void       pmm_numa_commit(void);
int        pmm_node_count(void);
int        pmm_local_node(void);
int        pmm_node_of(pmm_phys_t phys);   /* which node owns this frame */
pmm_phys_t page_alloc_node(int order, int zone_hint, int node);
void       pmm_node_stats(int node, struct pmm_node_info* out);
void       pmm_node_zone_stats(int node, int zone, uint32_t* out_free_per_order,
                               uint32_t* out_managed);

/* §M72 — user memory, refused below `mem.reserve_kb` (pmm.c). */
pmm_phys_t pmm_alloc_frame_user_low(void);
void pmm_reserve_changed(void);
void pmm_reserve_stats(uint32_t* reserve_kb, uint32_t* refused, int* low);

/* Human-readable one-line dump.  Used by `meminfo`. */
void pmm_print_stats(void);
void pmm_validate(const char* tag);   /* DEBUG: free-list integrity walk */

#endif
