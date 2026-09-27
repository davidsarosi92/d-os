/* =============================================================================
 * pmm.c — zoned buddy physical-memory allocator (M19).
 *
 * Replaces the M1 bitmap PMM.  The public API (`pmm_alloc_frame`,
 * `pmm_alloc_contiguous`, `pmm_free_frame`, the stats getters) is
 * unchanged so existing drivers keep working.  Internally everything
 * is now a buddy allocator over per-zone free lists.
 *
 * ---------------------------------------------------------------------
 * Data layout
 * ---------------------------------------------------------------------
 *
 * One byte of state per physical frame in `page_state[]`:
 *
 *   0xFF      — frame doesn't exist (BIOS-reserved, beyond memory,
 *               or in a region we've explicitly carved out)
 *   0xFE      — frame allocated (head or interior of any block)
 *   0..MAX    — head of a FREE buddy block of order = page_state[pfn].
 *               Only the head carries this; interior frames keep 0xFE
 *               on alloc and revert to 0xFE-with-list-presence on
 *               free (intermediate states are hidden inside locked
 *               critical sections).
 *
 * Each zone owns `BUDDY_MAX_ORDER + 1` singly-linked free lists.  The
 * link is stored INSIDE the free page itself — the first 4 bytes of
 * a free block hold the physical address of the next free block at
 * the same order (or 0 for end-of-list).  This is the textbook
 * "freelist threaded through free pages" trick and costs no extra
 * metadata.
 *
 * Buddy address: for a block at `pfn` of order `O`, the buddy is at
 *   pfn XOR (1 << O)
 * The buddy address is valid as long as the block is properly aligned
 * to its size (which we enforce by always splitting from the head down).
 *
 * ---------------------------------------------------------------------
 * Zone layout
 * ---------------------------------------------------------------------
 *
 *   ZONE_DMA    : pfn  [0,      4096)   — first 16 MiB; legacy ISA + small DMA
 *   ZONE_DMA32  : pfn  [4096,   1<<20)  — up to 4 GiB; 32-bit-addressable DMA
 *   ZONE_NORMAL : pfn  [1<<20,  ...)    — above 4 GiB; CPU-only memory
 *
 * Boundary handling: during init we never emit a free block that
 * straddles a zone boundary, and during coalesce we refuse to merge
 * across one — the buddy address is rejected if it sits in a
 * different zone.
 *
 * ---------------------------------------------------------------------
 * Concurrency
 * ---------------------------------------------------------------------
 *
 * Each zone has its own spinlock (M18 cmpxchg).  All alloc/free calls
 * are IRQ-safe (spin_lock_irqsave).  Per-zone locks let the BSP and an
 * AP allocate at the same time as long as they hit different zones —
 * the common ZONE_NORMAL case still serializes, but that's a
 * single-line fix later if it shows up in profiles (per-CPU magazines
 * are the bigger win and live in the slab layer).
 * ============================================================================= */

#include "pmm.h"
#include "lock.h"
#include "multiboot.h"
#include "hal_api.h"
#include "printf.h"
#include "percpu.h"
#include "config.h"
#include "settings.h"
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Sizing and helpers.                                                        */
/* -------------------------------------------------------------------------- */

/* `0xFF` = doesn't exist; `0xFE` = allocated / interior; <=BUDDY_MAX_ORDER = */
/* head of free block at that order.  See file header.                       */
#define PS_NONE     0xFFu
#define PS_USED     0xFEu

/* §M48 — sized at boot from the firmware memory map, not by a #define.  See
 * pmm.h for why the compile-time array had to go.  NULL until pmm_init runs;
 * every indexed access is guarded by `pfn < pmm_nr_frames`, which is 0 until
 * then, so a pre-init caller reads nothing. */
static uint8_t* page_state;
uint32_t pmm_nr_frames;                     /* one past the highest managed pfn */
/* §M85 stage 4 — the LOWEST managed pfn.  Metadata covers [pmm_pfn_base,
 * pmm_nr_frames) only: a machine whose RAM starts at 1 TiB (sbsa-ref) would
 * otherwise need 256 MiB of state for the terabyte BELOW its RAM, where there
 * is nothing.  PFNs stay absolute everywhere; the tables are allocated for the
 * span and their POINTERS are offset by the base, so `page_state[pfn]` is
 * unchanged at every use.  0 on every machine whose RAM starts low. */
uint32_t pmm_pfn_base;

/* Boot-time bump arena backing page_state[] and kmalloc's side table. */
static uintptr_t bootmem_next, bootmem_end;

#define ZONE_DMA_FRAME_LIMIT  4096u         /* 16 MiB / 4 KiB */

struct zone {
    const char* name;
    uint32_t    start_pfn;          /* inclusive */
    uint32_t    end_pfn;            /* exclusive */
    uint32_t    managed;            /* total frames ever marked free here */
    uint32_t    free_frames;        /* dynamic count */
    pmm_phys_t  free_lists[BUDDY_MAX_ORDER + 1];  /* head = phys of first free block, 0 = empty */
    uint32_t    nr_at_order[BUDDY_MAX_ORDER + 1]; /* diagnostic */
    spinlock_t  lock;
    int         node;               /* §M19.5.3 — the NUMA node it belongs to */
    int         type;               /* ZONE_DMA .. ZONE_HIGHMEM              */
};

/* §M19.5.3 — ONE ZONE SET PER NUMA NODE.
 *
 * Until SRAT (or a device tree's numa-node-id) is read, every frame belongs to
 * node 0 and only zones_n[0] is populated — which is also the permanent shape
 * of every machine that is not NUMA.  pmm_numa_commit() then moves each free
 * block into its own node's zone set and, from that point on, a block NEVER
 * straddles two nodes (seeding stops at a node boundary, coalescing refuses to
 * merge across one — the same two rules that already keep a block inside one
 * zone).
 *
 * WHY THE READING HAPPENS LATE AND IS APPLIED BY MOVING BLOCKS.  pmm_init runs
 * before the ACPI tables can be read: acpi_init maps them through the VMM and
 * allocates, i.e. it needs the allocator this would configure.  Parsing SRAT
 * by hand in pmm_init would be a second ACPI walker (checksum, RSDT/XSDT, the
 * affinity structures) kept in step with the real one.  A handful of free
 * blocks moved once at boot costs microseconds; a second parser costs forever.
 * Frames already ALLOCATED when the move happens stay where they are and are
 * freed into their correct node later, because page_free asks node_of_pfn. */
#define PMM_MAX_NODES   8
#define PMM_MAX_RANGES 16
static struct zone zones_n[PMM_MAX_NODES][NR_ZONES];

struct node_range { uint32_t start, end; int node; };
static struct node_range g_nrng[PMM_MAX_RANGES];
static int g_nrng_n;                /* ranges known (0 = not NUMA)            */
static int g_nr_nodes = 1;          /* nodes with memory, highest id + 1      */
/* Allocations that got memory from the node they asked for, and those that
 * had to fall back to another — Linux's numa_hit/numa_miss.  Relaxed atomics:
 * they are statistics, and a lock here would serialise every allocation. */
static uint32_t g_node_hit[PMM_MAX_NODES], g_node_miss[PMM_MAX_NODES];

/* Symbols from linker.ld marking the kernel image bounds. */
extern uint8_t kernel_start[];
extern uint8_t kernel_end[];

/* §M85 stage 4 — the kernel image's PHYSICAL extent.  `kernel_start` is a link
 * address: equal to the physical one on x86 (identity-mapped image), and a
 * virtual address in the top of TTBR1 on aarch64, where the image runs at a
 * fixed VA wherever it was loaded.  Every carve-out and conflict check here is
 * about physical memory, so it asks kptr_phys. */
#define KIMG_PS  ((uint64_t)kptr_phys(kernel_start))
#define KIMG_PE  ((uint64_t)kptr_phys(kernel_end))

/* The boot memory map as a POINTER (multiboot.h, mboot_mmap_ptr). */
#define MMAP_P(mbi)   mboot_mmap_ptr(mbi)
#define MMAP_PS(mbi)  ((uint64_t)kptr_phys((const void*)MMAP_P(mbi)))

/* -------------------------------------------------------------------------- */
/* Tiny helpers.                                                              */
/* -------------------------------------------------------------------------- */

static inline pmm_phys_t pfn_to_phys(uint32_t pfn) { return (pmm_phys_t)pfn << PMM_FRAME_SHIFT; }
static inline uint32_t   phys_to_pfn(pmm_phys_t p)  { return (uint32_t)(p >> PMM_FRAME_SHIFT); }

/* §M86 — the first frame the kernel cannot reach directly.  Equal to
 * pmm_nr_frames wherever the direct map covers all RAM (x86_64, aarch64). */
static uint32_t pmm_direct_end_pfn;

/* Which zone owns this pfn?  Returns zone index, or -1 if out of range. */
static int zone_of_pfn(uint32_t pfn) {
    if (pfn >= pmm_nr_frames || pfn < pmm_pfn_base) return -1;
    if (pfn >= pmm_direct_end_pfn)     return ZONE_HIGHMEM;
    if (pfn <  ZONE_DMA_FRAME_LIMIT)   return ZONE_DMA;
    if (pfn <  ZONE_DMA32_FRAME_LIMIT) return ZONE_DMA32;
    return ZONE_NORMAL;
}

/* §M19.5.3 — which node owns this pfn?  0 for anything no affinity range
 * names (and for everything on a machine that is not NUMA), because a frame
 * must belong SOMEWHERE and node 0 is where it lived before NUMA was read. */
static int g_numa_live;             /* ranges are in force (set by commit)   */
static int node_of_pfn(uint32_t pfn) {
    if (!g_numa_live) return 0;
    for (int i = 0; i < g_nrng_n; i++)
        if (pfn >= g_nrng[i].start && pfn < g_nrng[i].end) return g_nrng[i].node;
    return 0;
}

/* The zone a pfn's frame is managed by, or NULL outside the managed span. */
static struct zone* zone_for_pfn(uint32_t pfn) {
    int zi = zone_of_pfn(pfn);
    if (zi < 0) return 0;
    return &zones_n[node_of_pfn(pfn)][zi];
}

/* Bump-allocate from the boot arena.  Word-aligned; no free.  See pmm.h. */
void* pmm_bootmem_alloc(uint32_t bytes) {
    uintptr_t p = (bootmem_next + 15u) & ~(uintptr_t)15u;
    if (!bootmem_end || p + bytes > bootmem_end) {
        kprintf("pmm: bootmem exhausted (%u bytes requested)\n", bytes);
        return 0;
    }
    bootmem_next = p + bytes;
    return phys_to_virt(p);   /* callers want a kernel pointer, not a phys */
}

/* Ceiling log2: smallest k such that (1 << k) >= n.  0 → 0, 1 → 0,
 * 2 → 1, 3 → 2, 4 → 2, ..., 17 → 5.  Used to translate the legacy
 * N-frame contig API into an order. */
static int ceil_log2(uint32_t n) {
    if (n <= 1) return 0;
    int k = 0;
    uint32_t v = 1;
    while (v < n) { v <<= 1; k++; }
    return k;
}

/* -------------------------------------------------------------------------- */
/* Intrusive free list — link stored in the page itself.                      */
/*                                                                            */
/* We trust that all our frames live in the kernel's 256 MiB identity         */
/* map, so `phys` == kernel-accessible virtual.  When that stops being        */
/* true (e.g. HIGHMEM lands) the link-store will need a kmap-style            */
/* temporary mapping.                                                         */
/* -------------------------------------------------------------------------- */

/* The intrusive free-list link lives IN the free frame.  A highmem frame has
 * no permanent address, so its link is reached through kmap — which is what
 * lets the ordinary buddy manage highmem at all (§M86). */
#include "kmap.h"
static inline int frame_is_direct(pmm_phys_t phys) {
    return (phys >> PMM_FRAME_SHIFT) < pmm_direct_end_pfn;
}
/* Slot 0 of a free block is its NEXT link, slot 1 its PREV link (2026-09-26:
 * the list is doubly linked so a removal is O(1) — see zone_remove). */
static inline pmm_phys_t link_load_at(pmm_phys_t phys, int slot) {
    if (!frame_is_direct(phys)) {
        volatile pmm_phys_t* p = (volatile pmm_phys_t*)kmap_frame(phys);
        pmm_phys_t v = p[slot];
        kunmap_frame((void*)p);
        return v;
    }
    return ((volatile pmm_phys_t*)phys_to_virt(phys))[slot];
}
static inline pmm_phys_t link_load(pmm_phys_t phys) { return link_load_at(phys, 0); }
static inline void link_store_at(pmm_phys_t phys, int slot, pmm_phys_t v);
static inline void link_store_at(pmm_phys_t phys, int slot, pmm_phys_t next) {
    /* Invariant guard (cheap, permanent).  The intrusive free-list link is
     * written INTO the freed page, so a free frame must NEVER alias the live
     * kernel image.  If this ever fires, the buddy pool wrongly contains a
     * kernel-image frame (a coalesce-across-carve / missing-reservation bug) and
     * this very write is what would smash a .data pointer — catch it loudly at
     * the write site (with the caller) instead of chasing a delayed #PF.
     * (§M39 buddy-corruption investigation: with the current tree this stays
     * silent even under forced early order-6..8 allocation sweeps — the carve
     * pass provably excludes the image, so the buddy is exonerated.  The guard
     * remains as a regression detector.) */
    /* Compared at PHYSICAL width (2026-09-26).  Casting phys to uintptr_t
     * truncated a PAE frame above 4 GiB on i386, so 0x203bad000 "landed in"
     * the image at 0x3bad000 — a false alarm on every such free, and noise
     * of exactly the kind that hides the real one this guard exists for. */
    pmm_phys_t ks = (pmm_phys_t)KIMG_PS,
               ke = (pmm_phys_t)KIMG_PE;
    if (phys >= ks && phys < ke) {
        kprintf("PMM-GUARD: link_store 0x%llx INTO kernel image [0x%x,0x%x) "
                "pfn=%u caller=%p\n", (unsigned long long)phys, (uint32_t)ks,
                (uint32_t)ke, (unsigned)(phys >> PMM_FRAME_SHIFT),
                __builtin_return_address(0));
    }
    if (!frame_is_direct(phys)) {
        volatile pmm_phys_t* p = (volatile pmm_phys_t*)kmap_frame(phys);
        p[slot] = next;
        kunmap_frame((void*)p);
        return;
    }
    ((volatile pmm_phys_t*)phys_to_virt(phys))[slot] = next;
}
static inline void link_store(pmm_phys_t phys, pmm_phys_t next) { link_store_at(phys, 0, next); }

/* Push a free block of `order` onto a zone's free list.  Caller holds
 * zone->lock.  Stamps the head pfn's state with the order so coalesce
 * can recognize it as the same-order partner. */
static void zone_push(struct zone* z, uint32_t pfn, int order) {
    pmm_phys_t phys = pfn_to_phys(pfn);
    pmm_phys_t head = z->free_lists[order];
    link_store_at(phys, 0, head);
    link_store_at(phys, 1, 0);
    if (head) link_store_at(head, 1, phys);
    z->free_lists[order]   = phys;
    z->nr_at_order[order] += 1;
    page_state[pfn]        = (uint8_t)order;
}

/* Remove a specific (pfn, order) from the zone's free list.  Caller holds
 * zone->lock and has checked page_state[pfn] == order (the block IS free at
 * that order, so it is on that list).
 *
 * O(1), BECAUSE THE LIST IS DOUBLY LINKED (2026-09-26).  This used to walk the
 * list from its head to find the predecessor — "the buddy we're trying to pull
 * off is generally near the head".  It is not, once a list is long: a busy
 * order-0 list holds thousands of blocks, and the walk ran with the zone lock
 * held and interrupts OFF.  Highmem made it visible — every step is a kmap
 * there — as `!! PIT STARVED` during `excstorm` at -m 3G (and never at 1G):
 * the timer interrupt held off for hundreds of milliseconds by a list walk. */
static int zone_remove(struct zone* z, uint32_t pfn, int order) {
    pmm_phys_t target = pfn_to_phys(pfn);
    pmm_phys_t next = link_load_at(target, 0);
    pmm_phys_t prev = link_load_at(target, 1);
    if (prev) link_store_at(prev, 0, next);
    else if (z->free_lists[order] == target) z->free_lists[order] = next;
    else return -1;                              /* not on this list: refuse */
    if (next) link_store_at(next, 1, prev);
    z->nr_at_order[order] -= 1;
    return 0;
}

/* Pop the head of a zone's free list at `order`.  Returns pfn, or 0 if
 * empty.  Caller holds zone->lock. */
static uint32_t zone_pop(struct zone* z, int order) {
    pmm_phys_t head = z->free_lists[order];
    if (!head) return 0;
    pmm_phys_t next = link_load(head);
    z->free_lists[order] = next;
    if (next) link_store_at(next, 1, 0);
    z->nr_at_order[order] -= 1;
    return phys_to_pfn(head);
}

/* -------------------------------------------------------------------------- */
/* Init helpers.                                                              */
/* -------------------------------------------------------------------------- */

/* Mark every frame in a byte range as allocated/non-existent so the
 * subsequent buddy seeding skips them.  `start` rounds down, `end`
 * rounds up — when in doubt we err on the side of NOT handing out a
 * partially-protected frame. */
static void carve_out_range(pmm_phys_t start, pmm_phys_t end) {
    uint32_t s = (uint32_t)(start / PMM_FRAME_SIZE);
    uint32_t e = (uint32_t)((end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    if (e > pmm_nr_frames) e = pmm_nr_frames;
    if (s < pmm_pfn_base) s = pmm_pfn_base;
    for (uint32_t i = s; i < e; i++) page_state[i] = PS_NONE;
}

/* Mark a single frame as part of an AVAILABLE region — initially we
 * tag it PS_USED (= "allocated") so seeding can later free it via
 * the normal coalescing path.  This keeps the seed loop simple. */
static void seed_mark_available(uint32_t pfn) {
    if (pfn >= pmm_nr_frames || pfn < pmm_pfn_base) return;
    /* Only flip if not already carved out. */
    if (page_state[pfn] == PS_NONE) page_state[pfn] = PS_USED;
}

/* -------------------------------------------------------------------------- */
/* Init.                                                                      */
/* -------------------------------------------------------------------------- */

/* Does [s, e) overlap any range the kernel must not scribble on?  Used only
 * while placing the boot arena, before page_state exists to answer it. */
static int boot_range_conflicts(const struct mboot_info* mbi,
                                uint64_t s, uint64_t e) {
    struct { uint64_t s, e; } bad[] = {
        { 0, 0x100000 },                                        /* BIOS / VGA / EBDA */
        { KIMG_PS, KIMG_PE },                                     /* kernel image */
        { kptr_phys(mbi), kptr_phys(mbi) + sizeof(*mbi) },        /* multiboot info */
        { MMAP_PS(mbi), MMAP_PS(mbi) + mbi->mmap_length },        /* the map itself */
        { 0x8000, 0x8000 + 0x4000 },                             /* AP trampoline */
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (s < bad[i].e && bad[i].s < e) return 1;
    return 0;
}

/* Reserve one contiguous `size`-byte span out of the raw memory map for the
 * boot arena.  Walks AVAILABLE regions and, within each, slides the candidate
 * window past any conflicting range until it either fits or runs off the end.
 * Returns the base address, or 0 if nothing fits. */
static uintptr_t bootmem_reserve(const struct mboot_info* mbi,
                                 uint64_t covered, uint32_t size) {
    uintptr_t p = MMAP_P(mbi), end = MMAP_P(mbi) + mbi->mmap_length;
    int budget = 64;
    while (p < end && budget-- > 0) {
        const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)p;
        p += e->size + 4;
        if (e->type != MMAP_TYPE_AVAILABLE) continue;

        uint64_t rs = (e->base + PMM_FRAME_SIZE - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
        uint64_t re = e->base + e->length;
        if (re > covered) re = covered;          /* must be reachable to write */

        /* Slide past conflicts.  Bounded by the conflict count, so the loop
         * terminates even if the region is riddled with reservations. */
        for (int tries = 0; tries < 8 && rs + size <= re; tries++) {
            if (!boot_range_conflicts(mbi, rs, rs + size)) return (uintptr_t)rs;
            /* Jump to just past the kernel image / map / low memory, whichever
             * we are currently colliding with; re-align and retry. */
            uint64_t nxt = rs + PMM_FRAME_SIZE;
            uint64_t cand[] = {
                0x100000, KIMG_PE,
                kptr_phys(mbi) + sizeof(*mbi),
                MMAP_PS(mbi) + mbi->mmap_length, 0x8000 + 0x4000,
            };
            for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
                if (cand[i] > rs && cand[i] > nxt) nxt = cand[i];
            rs = (nxt + PMM_FRAME_SIZE - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
        }
    }
    return 0;
}

/* Cover every usable frame in [from, to) with the largest aligned blocks that
 * fit, zone by zone (see pass 3 in pmm_init).  Returns the frames freed. */
static uint32_t pmm_deferred_from;
static uint32_t seed_range(uint32_t from, uint32_t to) {
    uint32_t freed = 0;
    uint32_t pfn = from;
    while (pfn < to) {
        if (page_state[pfn] != PS_USED) { pfn++; continue; }

        struct zone* z = zone_for_pfn(pfn);
        if (!z) { pfn++; continue; }

        /* A run stops at a zone boundary AND at a node boundary (§M19.5.3). */
        uint32_t run_end = pfn;
        while (run_end < z->end_pfn && run_end < to && page_state[run_end] == PS_USED &&
               node_of_pfn(run_end) == z->node)
            run_end++;

        while (pfn < run_end) {
            /* Largest order that is both alignment-legal at pfn and fits. */
            int order = BUDDY_MAX_ORDER;
            while (order > 0) {
                uint32_t sz = 1u << order;
                if ((pfn & (sz - 1)) == 0 && pfn + sz <= run_end) break;
                order--;
            }
            uint32_t sz = 1u << order;
            uint32_t fl = spin_lock_irqsave(&z->lock);
            zone_push(z, pfn, order);
            z->free_frames += sz;
            z->managed     += sz;
            spin_unlock_irqrestore(&z->lock, fl);
            freed += sz;
            pfn += sz;
        }
    }
    return freed;
}

/* §M86 — seed the frames pmm_init could not reach (see there).  Called once,
 * right after vmm_init; a no-op wherever nothing was deferred. */
void pmm_seed_deferred(void) {
    if (!page_state || pmm_deferred_from >= pmm_nr_frames) return;
    uint32_t n = seed_range(pmm_deferred_from, pmm_nr_frames);
    pmm_deferred_from = pmm_nr_frames;
    kprintf("pmm: %u MiB above 4 GiB seeded through kmap — HIGHMEM m=%u f=%u\n",
            (n * 4) / 1024, zones_n[0][ZONE_HIGHMEM].managed,
            zones_n[0][ZONE_HIGHMEM].free_frames);
}

/* §M86 — the physical-address ceiling the paging hardware can express.  The
 * arch answers (i386: 4 GiB without PAE, 36+ bits with); the default is
 * "no limit beyond the metadata cap". */
uint64_t hal_phys_limit(void) __attribute__((weak));
uint64_t hal_phys_limit(void) { return ~0ull; }

void pmm_init(void) {
    const struct mboot_info* mbi = mboot_get_info();
    if (!mbi || (mbi->flags & MBI_FLAG_MMAP) == 0 || mbi->mmap_length == 0) {
        kprintf("pmm: no memory map — PMM disabled\n");
        return;
    }

    /* M19.5.1 — find the highest physical address among AVAILABLE
     * regions, then ask the HAL to extend its identity map there.
     * On x86_64 this installs 1 GiB pages in PDPT for memory above the
     * boot-time 1 GiB cap; on i386 it's a no-op (kmap deferred).
     *
     * We do this BEFORE the marking pass below so that all frames we
     * subsequently dereference (zero, free-list-link) are reachable
     * through the kernel virtual address space. */
    uint64_t max_phys = 0, min_phys = ~0ull;
    {
        uintptr_t wp = MMAP_P(mbi);
        uintptr_t wend = MMAP_P(mbi) + mbi->mmap_length;
        int wb = 64;
        while (wp < wend && wb-- > 0) {
            const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)wp;
            if (e->type == MMAP_TYPE_AVAILABLE) {
                uint64_t hi = e->base + e->length;
                /* Clamp to the sanity ceiling — a bogus map must not size
                 * gigabytes of metadata.  This is the ONLY fixed limit left. */
                /* §M86 — what the paging hardware can express: i386 without
                 * PAE stops at 4 GiB whatever RAM exists.  (The metadata cap
                 * is applied to the SPAN below, once the base is known.) */
                if (hi > hal_phys_limit()) hi = hal_phys_limit();
                if (hi > max_phys) max_phys = hi;
                if (e->length && e->base < min_phys) min_phys = e->base;
            }
            wp += e->size + 4;
        }
    }
    uint64_t covered = hal_extend_identity_map((uintptr_t)max_phys);
    if (covered > max_phys) covered = max_phys;   /* never claim past real RAM */
    if (covered < max_phys) {
        /* §M86 — no longer skipped: managed as ZONE_HIGHMEM and reached
         * through kmap (kmap.h).  Only user pages come from there. */
        kprintf("pmm: direct map ends at %u MiB (RAM goes up to %u MiB) — "
                "the rest is HIGHMEM, for user pages\n",
                (unsigned)(covered >> 20), (unsigned)(max_phys >> 20));
    } else if (covered > (uint64_t)1 * 1024 * 1024 * 1024) {
        kprintf("pmm: identity map extended to %u MiB\n",
                (unsigned)(covered >> 20));
    }

    /* §M48 — the frame ceiling, discovered.  Everything reachable gets
     * metadata; nothing beyond it does.  §M85 — and nothing BELOW the lowest
     * RAM either: the base is 1 GiB-aligned, which keeps every buddy block
     * (at most 2^BUDDY_MAX_ORDER frames) aligned in absolute pfns too. */
    if (min_phys == ~0ull || min_phys < (1ull << 30)) min_phys = 0;
    pmm_pfn_base  = (uint32_t)((min_phys & ~((1ull << 30) - 1)) / PMM_FRAME_SIZE);
    pmm_nr_frames = (uint32_t)(max_phys / PMM_FRAME_SIZE);
    if (pmm_nr_frames - pmm_pfn_base > BUDDY_FRAME_HARD_CAP)
        pmm_nr_frames = pmm_pfn_base + BUDDY_FRAME_HARD_CAP;
    pmm_direct_end_pfn = (uint32_t)(covered / PMM_FRAME_SIZE);
    if (pmm_direct_end_pfn > pmm_nr_frames) pmm_direct_end_pfn = pmm_nr_frames;

    /* Boot arena, sized for every structure that scales with RAM:
     *   page_state[]            1 byte  / frame  (pmm)
     *   big_alloc_order[]       1 byte  / frame  (kmalloc)
     *   g_cow_ref[]             2 bytes / frame  (vmm, fork refcounts)
     * plus a page of slack for whatever comes next. */
    uint32_t arena = (pmm_nr_frames - pmm_pfn_base) * 4u + PMM_FRAME_SIZE;
    uintptr_t arena_base = bootmem_reserve(mbi, covered, arena);
    if (!arena_base) {
        kprintf("pmm: cannot place %u KiB boot arena — PMM disabled\n", arena >> 10);
        pmm_nr_frames = 0;
        return;
    }
    bootmem_next = arena_base;
    bootmem_end  = arena_base + arena;

    page_state = (uint8_t*)pmm_bootmem_alloc(pmm_nr_frames - pmm_pfn_base);
    if (!page_state) { pmm_nr_frames = 0; return; }
    page_state -= pmm_pfn_base;              /* index by ABSOLUTE pfn */

    kprintf("pmm: %u MiB directly mapped of %u MiB, %u frames, %u KiB metadata at %p\n",
            (unsigned)(covered >> 20), (unsigned)(max_phys >> 20), pmm_nr_frames,
            arena >> 10, (void*)arena_base);

    /* Set up zone descriptors — the same four bounds for every node; which
     * node a frame belongs to is node_of_pfn's answer, not the zone's. */
    static const char* const znames[NR_ZONES] = { "DMA", "DMA32", "NORMAL", "HIGHMEM" };
    uint32_t zs[NR_ZONES], ze[NR_ZONES];
    zs[ZONE_DMA]     = 0;
    ze[ZONE_DMA]     = ZONE_DMA_FRAME_LIMIT;
    zs[ZONE_DMA32]   = ZONE_DMA_FRAME_LIMIT;
    ze[ZONE_DMA32]   = pmm_direct_end_pfn < ZONE_DMA32_FRAME_LIMIT
                     ? pmm_direct_end_pfn : ZONE_DMA32_FRAME_LIMIT;
    /* NORMAL is empty whenever the machine has 4 GiB or less — which is ALWAYS
     * on i386, where 32-bit page tables cannot express a higher address. */
    zs[ZONE_NORMAL]  = ZONE_DMA32_FRAME_LIMIT;
    ze[ZONE_NORMAL]  = pmm_direct_end_pfn > ZONE_DMA32_FRAME_LIMIT
                     ? pmm_direct_end_pfn : ZONE_DMA32_FRAME_LIMIT;
    /* §M86 — everything past the direct map.  Empty on x86_64 and aarch64,
     * where the direct map covers all RAM. */
    zs[ZONE_HIGHMEM] = pmm_direct_end_pfn;
    ze[ZONE_HIGHMEM] = pmm_nr_frames;
    for (int nd = 0; nd < PMM_MAX_NODES; nd++)
        for (int zi = 0; zi < NR_ZONES; zi++) {
            struct zone* z = &zones_n[nd][zi];
            z->name = znames[zi]; z->start_pfn = zs[zi]; z->end_pfn = ze[zi];
            z->node = nd; z->type = zi;
            spin_lock_init(&z->lock);
        }

    /* Initialize every frame as PS_NONE (doesn't exist).  The mmap walk
     * flips bits to PS_USED for frames inside AVAILABLE regions, then
     * the reservation pass carves out kernel image / low memory etc.
     * Finally the seeding loop frees the remainder. */
    for (uint32_t i = pmm_pfn_base; i < pmm_nr_frames; i++) page_state[i] = PS_NONE;

    /* Pass 1: tag AVAILABLE frames as PS_USED.  Anything outside an
     * AVAILABLE region stays PS_NONE.  Cap at `covered` so we never
     * try to dereference a frame the HAL didn't make reachable (i386:
     * stuck at 256 MiB until kmap lands; x86_64: extended above). */
    /* §M86 — every frame is managed now, reachable or not; only the SEEDING
     * of an unreachable frame goes through kmap (link_store). */
    uint32_t cover_frames = pmm_nr_frames;

    uintptr_t p   = MMAP_P(mbi);
    uintptr_t end = MMAP_P(mbi) + mbi->mmap_length;
    int entry_budget = 64;
    while (p < end && entry_budget-- > 0) {
        const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)p;
        if (e->type == MMAP_TYPE_AVAILABLE) {
            uint64_t base = e->base;
            uint64_t len  = e->length;

            uint64_t top = (uint64_t)pmm_nr_frames * PMM_FRAME_SIZE;
            if (base >= top) { p += e->size + 4; continue; }
            if (base + len > top) len = top - base;

            uint32_t first = (uint32_t)((base + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
            uint32_t last  = (uint32_t)((base + len) / PMM_FRAME_SIZE);
            if (last > cover_frames) last = cover_frames;
            for (uint32_t i = first; i < last; i++) seed_mark_available(i);
        }
        p += e->size + 4;
    }

    /* Pass 2: re-carve protected regions.  Each carve writes PS_NONE
     * unconditionally so even AVAILABLE-marked frames inside the
     * carve-out range disappear from the pool. */

    /* (a0) Every NON-available map entry wins over an available one it
     *      overlaps (§M86 stage 4, 2026-09-26).  Pass 1 only ever SKIPPED
     *      them, which is enough while the map's entries are disjoint (the
     *      common x86 firmware case) and wrong the moment a reservation sits
     *      inside a RAM range — which is how a device tree describes one: the
     *      /memory node covers the whole bank and /memreserve/ carves pieces
     *      out of it.  Seeding those frames hands firmware's memory to the
     *      allocator. */
    {
        uintptr_t q = MMAP_P(mbi);
        int budget = 64;
        while (q < end && budget-- > 0) {
            const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)q;
            if (e->type != MMAP_TYPE_AVAILABLE && e->length)
                carve_out_range((pmm_phys_t)e->base, (pmm_phys_t)(e->base + e->length));
            q += e->size + 4;
        }
    }

    /* (a) Frame 0 — NULL safety. */
    if (pmm_pfn_base == 0) page_state[0] = PS_NONE;

    /* (b) Everything below 1 MiB (BIOS / VGA / EBDA / option ROMs). */
    carve_out_range(0, 0x100000);

    /* (c) Kernel image bounds from linker.ld. */
    carve_out_range((pmm_phys_t)KIMG_PS, (pmm_phys_t)KIMG_PE);

    /* (d) Multiboot info + the attached memory map (lives outside
     *     the kernel image, can land anywhere in low memory). */
    carve_out_range((pmm_phys_t)kptr_phys(mbi),
                    (pmm_phys_t)kptr_phys(mbi) + sizeof(struct mboot_info));
    carve_out_range(MMAP_PS(mbi), MMAP_PS(mbi) + mbi->mmap_length);

    /* (e) AP trampoline destination + per-AP info (M18 puts these at
     *     fixed low addresses).  Reserving a generous 16 KiB window
     *     covers the trampoline (~256 bytes), the ap_info struct
     *     (4 KiB later), and slop for future expansion. */
    carve_out_range(0x8000, 0x8000 + 0x4000);

    /* (f) §M48 — the boot arena itself.  page_state[] lives inside it, so
     *     handing any of it to the buddy would let a later allocation
     *     overwrite the very table that tracks allocations. */
    carve_out_range((pmm_phys_t)arena_base, (pmm_phys_t)arena_base + arena);

    /* Pass 3: seed the buddy free lists.
     *
     * §M48 — this used to release every usable frame individually at order 0
     * and let the coalescing path in page_free build the higher orders back
     * up.  Correct, and fine at 256 MiB (65k frames); at 128 GiB it is 33.8
     * MILLION frames each walking up to BUDDY_MAX_ORDER merge steps, with a
     * free-list search per step.  That turned boot into minutes of pure
     * bookkeeping to reconstruct a structure we can simply emit directly.
     *
     * So: find each maximal run of usable frames and cover it with the
     * largest ALIGNED power-of-two blocks that fit — precisely the shape
     * coalescing would have converged on, without the intermediate work.
     * Runs stop at zone boundaries, which is what keeps a block from
     * straddling one (the coalescing path refuses those merges for the same
     * reason).  Frames in a run are already PS_USED from pass 1, so only the
     * block head needs stamping, and zone_push does that. */
    /* §M86 — on a 32-bit kernel, a frame at or above 4 GiB cannot be reached
     * before paging is on (seeding writes a link INTO each free block, and a
     * 32-bit pointer cannot name it).  Those are seeded by pmm_seed_deferred()
     * right after vmm_init, through kmap. */
    uint32_t seed_to = pmm_nr_frames;
    if (sizeof(void*) == 4 && seed_to > (1u << 20)) seed_to = 1u << 20;
    uint32_t initially_free = seed_range(pmm_pfn_base, seed_to);
    pmm_deferred_from = seed_to;

    kprintf("pmm: buddy ready — DMA m=%u f=%u, DMA32 m=%u f=%u, NORMAL m=%u f=%u, "
            "HIGHMEM m=%u f=%u (%u MiB total free)\n",
            zones_n[0][ZONE_DMA].managed,    zones_n[0][ZONE_DMA].free_frames,
            zones_n[0][ZONE_DMA32].managed,  zones_n[0][ZONE_DMA32].free_frames,
            zones_n[0][ZONE_NORMAL].managed, zones_n[0][ZONE_NORMAL].free_frames,
            zones_n[0][ZONE_HIGHMEM].managed, zones_n[0][ZONE_HIGHMEM].free_frames,
            (initially_free * 4) / 1024);
}

/* -------------------------------------------------------------------------- */
/* Buddy core: alloc / free / split / coalesce.                                */
/* -------------------------------------------------------------------------- */

/* Try to allocate from a specific zone at exact `order`.  Returns the
 * pfn of the head, or 0 on failure.  Caller does NOT hold the lock —
 * we acquire it here.  If the requested order is empty, we split a
 * larger block: pop at the smallest non-empty order > requested, push
 * the buddy halves down to the requested order. */
static uint32_t buddy_alloc_in_zone(struct zone* z, int order) {
    if (order < 0 || order > BUDDY_MAX_ORDER) return 0;

    uint32_t fl = spin_lock_irqsave(&z->lock);

    int o = order;
    while (o <= BUDDY_MAX_ORDER && z->free_lists[o] == 0) o++;
    if (o > BUDDY_MAX_ORDER) {
        spin_unlock_irqrestore(&z->lock, fl);
        return 0;
    }

    uint32_t pfn = zone_pop(z, o);

    /* Split down: while we're bigger than requested, give the upper
     * half back to a smaller-order free list. */
    while (o > order) {
        o--;
        uint32_t buddy_pfn = pfn + (1u << o);
        zone_push(z, buddy_pfn, o);
    }

    /* Mark the allocated head + interior as USED. */
    uint32_t span = 1u << order;
    for (uint32_t i = 0; i < span; i++) page_state[pfn + i] = PS_USED;
    z->free_frames -= span;

    spin_unlock_irqrestore(&z->lock, fl);
    return pfn;
}

/* Free a block of `order` into the right zone, coalescing with the
 * buddy if it's free at the same order, recursively up to MAX_ORDER. */
static void buddy_free_in_zone(struct zone* z, uint32_t pfn, int order) {
    if (order < 0 || order > BUDDY_MAX_ORDER) return;

    uint32_t fl = spin_lock_irqsave(&z->lock);

    uint32_t span = 1u << order;
    z->free_frames += span;

    while (order < BUDDY_MAX_ORDER) {
        uint32_t buddy_pfn = pfn ^ (1u << order);

        /* Buddy must exist and be in the SAME zone — never coalesce
         * across DMA/NORMAL boundary — and on the SAME node (§M19.5.3): a
         * merged block spanning two nodes would be handed out as local to
         * one of them while half of it is remote. */
        if (buddy_pfn >= pmm_nr_frames || buddy_pfn < pmm_pfn_base) break;
        if (buddy_pfn <  z->start_pfn || buddy_pfn >= z->end_pfn) break;
        if (g_numa_live && node_of_pfn(buddy_pfn) != z->node) break;

        /* Buddy must be free at the same order. */
        if (page_state[buddy_pfn] != (uint8_t)order) break;

        /* Remove buddy from the free list, merge, retry at order+1. */
        if (zone_remove(z, buddy_pfn, order) != 0) break;
        if (buddy_pfn < pfn) pfn = buddy_pfn;
        order++;
    }

    zone_push(z, pfn, order);

    spin_unlock_irqrestore(&z->lock, fl);
    (void)span;
}

/* -------------------------------------------------------------------------- */
/* Public API: order-aware page_alloc / page_free.                            */
/* -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * §M33 — allocate a run the DEVICE can actually address.
 *
 * WHY THE ZONES ARE NOT ENOUGH.  They express three widths — 16 MiB, 4 GiB, and
 * unconstrained — and real devices are not that tidy.  The first driver written
 * against the driver-runtime API addresses 28 bits; ZONE_DMA satisfies that and
 * is EMPTY on this system, because the kernel image is 60 MiB and occupies
 * everything below 16 MiB.  So the zone that fits is unusable and the zone that
 * is usable does not fit, and a caller with a bit-width constraint had no way to
 * say so.
 *
 * This walks the free list at the requested order for the first block that fits
 * under `limit`.  It is a LINEAR SCAN and that is deliberate: this is the rare
 * path (a narrow device allocating once at bring-up), and making the common path
 * pay for it — by sorting the free lists, say — would be a real cost for a case
 * that happens a handful of times per boot.
 *
 * Returns PMM_ALLOC_FAIL rather than something that does not fit.  A device
 * silently truncating an address it cannot hold is precisely the failure being
 * prevented, and handing back a near-miss would reintroduce it.
 * -------------------------------------------------------------------------- */
pmm_phys_t page_alloc_below(int order, pmm_phys_t limit) {
    if (order < 0 || order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;
    uint64_t bytes = (uint64_t)4096 << order;

    /* Lowest zone first: its memory is the scarcest and the most likely to fit,
     * which is the opposite of page_alloc's preference and correct here. */
    const int order_zones[3] = { ZONE_DMA, ZONE_DMA32, ZONE_NORMAL };
    for (int zn = 0; zn < 3 * g_nr_nodes; zn++) {
        int zi = zn / g_nr_nodes;
        struct zone* z = &zones_n[zn % g_nr_nodes][order_zones[zi]];
        uint32_t fl = spin_lock_irqsave(&z->lock);
        /* Blocks of exactly this order first; then split a larger one whose
         * base already fits, since the lower half of a fitting block fits. */
        for (int o = order; o <= BUDDY_MAX_ORDER; o++) {
            for (pmm_phys_t cur = z->free_lists[o]; cur; cur = link_load(cur)) {
                if ((uint64_t)cur + bytes - 1 > (uint64_t)limit) continue;
                /* zone_remove returns 0 on SUCCESS.  This read `if (!…) continue`,
                 * i.e. it skipped exactly the blocks it had just removed — each
                 * fitting block left its free list and was never handed out: a
                 * leak per call, found while making removal O(1) (2026-09-26). */
                if (zone_remove(z, phys_to_pfn(cur), o) != 0) continue;
                /* Give back the halves we do not need, highest first so the
                 * free lists keep the shape buddy_free expects. */
                for (int k = o; k > order; k--)
                    zone_push(z, phys_to_pfn(cur) + (1u << (k - 1)), k - 1);
                spin_unlock_irqrestore(&z->lock, fl);
                return cur;
            }
        }
        spin_unlock_irqrestore(&z->lock, fl);
    }
    /* Quiet on purpose: the caller reports the refusal with the driver's name
     * and its address width, which is the part somebody can act on.  A second
     * message here would only say the same thing without the context. */
    return PMM_ALLOC_FAIL;
}

/* §M19.5.3 — the node the running CPU sits on; 0 until NUMA is known. */
int pmm_local_node(void) {
    if (g_nr_nodes <= 1) return 0;
    int n = this_cpu()->numa_node;
    return (n >= 0 && n < g_nr_nodes) ? n : 0;
}

pmm_phys_t page_alloc(int order, int zone_hint) {
    return page_alloc_node(order, zone_hint, pmm_local_node());
}

/* NODE-major: the preferred node's zones first (in the usual downward order),
 * then the other nodes' — EXCEPT ZONE_DMA, the 16 MiB ISA zone, which is taken
 * only after every node's higher zones are exhausted.
 *
 * THE FIRST VERSION WAS ZONE-MAJOR AND WRONG ON THE COMMONEST LAYOUT.  It
 * reasoned that DMA32 is precious (a frame spent there is one a 32-bit device
 * can no longer get) and so preferred ANOTHER node's NORMAL memory to the
 * local node's DMA32.  But node 0 is where low memory is — on a real two-socket
 * machine and in every QEMU -numa layout — so node 0's CPUs NEVER received
 * local memory while any other node had some above 4 GiB: measured on x86_64
 * -m 6G across three nodes, `numatest` asked node 0 and got node 1.  Linux
 * removed zone-ordered zonelists (4.14) for this reason.  DMA32 on a 64-bit
 * machine is 4 GiB and its narrow consumers allocate at bring-up; ZONE_DMA is
 * the genuinely scarce one, and it keeps its last-resort place. */
pmm_phys_t page_alloc_node(int order, int zone_hint, int node) {
    if (node < 0 || node >= g_nr_nodes) node = 0;
    if (order < 0 || order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;

    /* Fall back DOWNWARD from the hint — a lower zone satisfies every
     * constraint a higher one does.  Crucially it never goes UP: a caller that
     * asked for DMA32 must not be handed a 40-bit address just because NORMAL
     * has room, which is exactly the corruption the zone split exists to
     * prevent.  The scarcer memory is spent last. */
    int try_order[4] = { -1, -1, -1, -1 };
    int n = 0;

    if (zone_hint == ZONE_HIGHMEM) {
        /* Only on request: a highmem frame has no permanent address. */
        try_order[n++] = ZONE_HIGHMEM;
        try_order[n++] = ZONE_NORMAL;
        try_order[n++] = ZONE_DMA32;
        try_order[n++] = ZONE_DMA;
    } else if (zone_hint == ZONE_DMA) {
        try_order[n++] = ZONE_DMA;
    } else if (zone_hint == ZONE_DMA32) {
        try_order[n++] = ZONE_DMA32;
        try_order[n++] = ZONE_DMA;
    } else {
        /* ZONE_NORMAL, ZONE_DEFAULT, or anything unrecognised. */
        try_order[n++] = ZONE_NORMAL;
        try_order[n++] = ZONE_DMA32;
        try_order[n++] = ZONE_DMA;
    }

    /* Pass 0: every zone but DMA, node by node (preferred first).
     * Pass 1: ZONE_DMA, node by node. */
    for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < g_nr_nodes; k++) {
        int nd = k == 0 ? node : (k <= node ? k - 1 : k);   /* preferred first */
        for (int i = 0; i < n; i++) {
            if ((try_order[i] == ZONE_DMA) != (pass == 1)) continue;
            uint32_t pfn = buddy_alloc_in_zone(&zones_n[nd][try_order[i]], order);
            if (!pfn) continue;
            if (g_nr_nodes > 1)
                __atomic_add_fetch(nd == node ? &g_node_hit[node] : &g_node_miss[node],
                                   1, __ATOMIC_RELAXED);
            return pfn_to_phys(pfn);
        }
    }
    return PMM_ALLOC_FAIL;
}

void page_free(pmm_phys_t phys, int order) {
    if (phys == 0) return;
    if (phys & (PMM_FRAME_SIZE - 1)) return;   /* misaligned — caller bug */

    uint32_t pfn = phys_to_pfn(phys);
    struct zone* z = zone_for_pfn(pfn);
    if (!z) return;

    buddy_free_in_zone(z, pfn, order);
}

/* -------------------------------------------------------------------------- */
/* Legacy API wrappers — keep call sites working unchanged.                   */
/* -------------------------------------------------------------------------- */

pmm_phys_t pmm_alloc_frame(void) {
    return page_alloc(0, ZONE_DEFAULT);
}

/* §M86 — see kmap.h.  Highmem first: low memory is what the kernel itself can
 * use, so a user page taking it when highmem has room wastes the scarcer
 * resource. */
int pmm_frame_is_highmem(pmm_phys_t frame) { return !frame_is_direct(frame); }

/* ---------------------------------------------------------------------------
 * §M72 — THE RESERVE.
 *
 * A reserve is only real if something is REFUSED.  What is refused here is
 * USER memory: every page a program's address space asks for comes through
 * pmm_alloc_frame_user*() (anonymous mmap, brk, the stack, ELF segments, COW
 * copies, memfd/shm), and those stop at `mem.reserve_kb`.  Everything else —
 * page tables, the heap, the compositor's surfaces, a crash report, the fault
 * path — keeps drawing, because the reserve exists to keep THE MACHINE able to
 * answer while something is eating it.  The intent is carried by the ENTRY
 * POINT rather than a flag on every call: "who may not take the last frames"
 * is one short list (this function's callers), which is the §M67 export-list
 * argument applied to memory.
 *
 * THE RESERVE IS RESIDENT PHYSICAL MEMORY, and nothing here ever backs it with
 * or measures it against a disk (§M74's rules): a reserve redeemable only
 * through I/O needs memory to collect.
 *
 * It ANNOUNCES the crossing once, not per refusal — a low-memory condition
 * that prints per page buries the log exactly when the log is the only
 * instrument left — and again when memory is back above the reserve with a
 * margin (so a hover at the line does not become a stream).  Not an OOM killer:
 * choosing a victim is the user's escalation (Task Manager, `fkill`, and §M72's
 * pause), and the reserve's job is to keep the machine able to ask. */
static volatile int g_reserve_frames = -1;
static volatile int g_low;                /* 1 while below the reserve */
static uint32_t g_refused;                /* user frames refused, lifetime */

static int reserve_frames(void) {
    int r = g_reserve_frames;
    if (r < 0) {
        long kb = config_get_long("mem.reserve_kb", 4096);
        if (kb < 0) kb = 0;
        r = (int)(kb / 4);
        g_reserve_frames = r;
    }
    return r;
}

static int reserve_allows(uint32_t n) {
    uint32_t fr = pmm_free_frames();
    uint32_t r  = (uint32_t)reserve_frames();
    if (fr >= r + n) {
        if (g_low && fr > r + r / 4 + 64 &&
            __atomic_exchange_n(&g_low, 0, __ATOMIC_ACQ_REL))
            kprintf("mem: free memory is back above the reserve (%u KiB free, "
                    "reserve %u KiB)\n", fr * 4, r * 4);
        return 1;
    }
    __atomic_add_fetch(&g_refused, 1, __ATOMIC_RELAXED);
    if (!__atomic_exchange_n(&g_low, 1, __ATOMIC_ACQ_REL))
        kprintf("mem: LOW MEMORY - %u KiB free is at the %u KiB reserve; programs' "
                "new memory is refused until some is released (the system keeps "
                "the reserve)\n", fr * 4, r * 4);
    return 0;
}

void pmm_reserve_changed(void) { g_reserve_frames = -1; }
void pmm_reserve_stats(uint32_t* reserve_kb, uint32_t* refused, int* low) {
    if (reserve_kb) *reserve_kb = (uint32_t)reserve_frames() * 4;
    if (refused) *refused = __atomic_load_n(&g_refused, __ATOMIC_RELAXED);
    /* OBSERVED, not remembered (audit rule 2): the latch only moves on the
     * next user allocation, and "below" read from it after the hog was
     * killed would report a state that no longer exists. */
    if (low) *low = pmm_free_frames() < (uint32_t)reserve_frames();
}

pmm_phys_t pmm_alloc_frame_user(void) {
    if (!reserve_allows(1)) return PMM_ALLOC_FAIL;
    return page_alloc(0, ZONE_HIGHMEM);
}

/* The same, for a user frame the kernel must reach through a plain pointer
 * (memfd/shm, zeroed and copied by the kernel): never highmem. */
pmm_phys_t pmm_alloc_frame_user_low(void) {
    if (!reserve_allows(1)) return PMM_ALLOC_FAIL;
    return page_alloc(0, ZONE_DEFAULT);
}

/* The portable half of kmap: where every frame is directly mapped (x86_64,
 * aarch64) a frame's address IS its pointer.  i386's vmm.c overrides these. */
void* kmap_frame(pmm_phys_t frame) __attribute__((weak));
void* kmap_frame(pmm_phys_t frame) { return phys_to_virt(frame); }
void  kunmap_frame(void* p) __attribute__((weak));
void  kunmap_frame(void* p) { (void)p; }

void kmap_zero_frame(pmm_phys_t frame) {
    uint32_t* d = (uint32_t*)kmap_frame(frame);
    for (uint32_t i = 0; i < PMM_FRAME_SIZE / 4; i++) d[i] = 0;
    kunmap_frame(d);
}
void kmap_copy_frame(pmm_phys_t dst, pmm_phys_t src) {
    const uint32_t* s = (const uint32_t*)kmap_frame(src);
    uint32_t* d = (uint32_t*)kmap_frame(dst);
    for (uint32_t i = 0; i < PMM_FRAME_SIZE / 4; i++) d[i] = s[i];
    kunmap_frame(d);                               /* LIFO: last mapped first */
    kunmap_frame((void*)s);
}

pmm_phys_t pmm_alloc_contiguous(uint32_t n) {
    if (n == 0) return PMM_ALLOC_FAIL;
    if (n == 1) return page_alloc(0, ZONE_DEFAULT);

    int order = ceil_log2(n);
    if (order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;
    return page_alloc(order, ZONE_DEFAULT);
}

void pmm_free_frame(pmm_phys_t addr) {
    page_free(addr, 0);
}

pmm_phys_t pmm_alloc_frame_dma32(void) {
    return page_alloc(0, ZONE_DMA32);
}

/* §M85 — contiguous frames a device with `addr_bits` of DMA address can
 * reach.  A 64-bit-capable controller takes any frame; only a narrower one is
 * confined to DMA32.  Drivers that asked for DMA32 unconditionally could not
 * run at all on a machine with no memory below 4 GiB (sbsa-ref: RAM at 1 TiB),
 * although their hardware would have reached any address. */
pmm_phys_t pmm_alloc_contiguous_dma(uint32_t n, int addr_bits) {
    if (n == 0) return PMM_ALLOC_FAIL;
    int order = ceil_log2(n);
    if (order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;
    return page_alloc(order, addr_bits > 32 ? ZONE_NORMAL : ZONE_DMA32);
}

pmm_phys_t pmm_alloc_contiguous_dma32(uint32_t n) {
    if (n == 0) return PMM_ALLOC_FAIL;
    int order = ceil_log2(n);
    if (order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;
    return page_alloc(order, ZONE_DMA32);
}

void pmm_free_contiguous(pmm_phys_t addr, uint32_t n) {
    if (addr == PMM_ALLOC_FAIL || n == 0) return;
    /* The order the ALLOCATION used — see the header: a contiguous run is one
     * buddy block, and the free must name the same block. */
    int order = ceil_log2(n);
    if (order > BUDDY_MAX_ORDER) return;
    page_free(addr, order);
}

/* -------------------------------------------------------------------------- */
/* §M19.5.3 — NUMA: learn which node owns which memory, then re-home blocks.   */
/* -------------------------------------------------------------------------- */

static int g_numa_committed;

/* Record one affinity range (from SRAT on x86, a device tree's numa-node-id on
 * ARM).  Takes effect at pmm_numa_commit(); refused afterwards, because a range
 * learned later would contradict blocks that are already on their node's
 * lists.  Returns 0, or -1 when the table is full or the node id too large —
 * said by the caller, which knows where the range came from. */
int pmm_numa_add_range(uint64_t base, uint64_t len, int node) {
    if (g_numa_committed || node < 0 || node >= PMM_MAX_NODES || !len) return -1;
    if (g_nrng_n == PMM_MAX_RANGES) return -1;
    uint64_t s = base / PMM_FRAME_SIZE, e = (base + len) / PMM_FRAME_SIZE;
    if (s >= pmm_nr_frames) return 0;             /* beyond managed RAM: nothing */
    if (e > pmm_nr_frames) e = pmm_nr_frames;
    if (e <= s) return 0;
    g_nrng[g_nrng_n].start = (uint32_t)s;
    g_nrng[g_nrng_n].end   = (uint32_t)e;
    g_nrng[g_nrng_n].node  = node;
    g_nrng_n++;
    return 0;
}

/* The node every frame of [pfn, pfn + 2^order) belongs to, or -1 when a range
 * boundary falls inside the block (so it must be split). */
static int block_node(uint32_t pfn, int order) {
    uint32_t end = pfn + (1u << order);
    for (int i = 0; i < g_nrng_n; i++) {
        if (g_nrng[i].start > pfn && g_nrng[i].start < end) return -1;
        if (g_nrng[i].end   > pfn && g_nrng[i].end   < end) return -1;
    }
    return node_of_pfn(pfn);
}

/* Put a free block onto its node's list for zone type `zi`, splitting it
 * where a node boundary crosses it.  Caller holds node 0's zone lock; the
 * target's is taken here (lock order: node 0 first, which every caller of
 * this obeys and nothing else nests). */
static void rehome_block(int zi, uint32_t pfn, int order) {
    int nd = block_node(pfn, order);
    if (nd < 0) {                                  /* straddles: halve it */
        rehome_block(zi, pfn, order - 1);
        rehome_block(zi, pfn + (1u << (order - 1)), order - 1);
        return;
    }
    struct zone* z = &zones_n[nd][zi];
    uint32_t fl = 0;
    if (nd) fl = spin_lock_irqsave(&z->lock);
    zone_push(z, pfn, order);
    z->free_frames += 1u << order;
    if (nd) spin_unlock_irqrestore(&z->lock, fl);
}

/* Apply the recorded ranges: every free block leaves node 0 for its own node,
 * and `managed` is recounted per node from page_state (a frame the allocator
 * knows about is any frame not PS_NONE).  The recount is CHECKED against the
 * old total — a mismatch means a frame was lost or invented in the move, and
 * that is said loudly rather than left to be found as a leak.  A machine with
 * no ranges, or with every range on node 0, stays exactly as it was. */
void pmm_numa_commit(void) {
    if (g_numa_committed) return;
    g_numa_committed = 1;
    int top = 0;
    for (int i = 0; i < g_nrng_n; i++) if (g_nrng[i].node > top) top = g_nrng[i].node;
    if (!g_nrng_n || top == 0) { g_nrng_n = 0; return; }

    uint32_t managed_before = pmm_managed_frames(), free_before = pmm_free_frames();
    g_numa_live = 1;                /* from here node_of_pfn answers from ranges */
    for (int zi = 0; zi < NR_ZONES; zi++) {
        struct zone* z0 = &zones_n[0][zi];
        uint32_t fl = spin_lock_irqsave(&z0->lock);
        pmm_phys_t heads[BUDDY_MAX_ORDER + 1];
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
            heads[o] = z0->free_lists[o];
            z0->free_lists[o] = 0;
            z0->nr_at_order[o] = 0;
        }
        z0->free_frames = 0;
        g_nr_nodes = top + 1;       /* published under the lock of the first zone */
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
            for (pmm_phys_t cur = heads[o]; cur; ) {
                pmm_phys_t next = link_load(cur);      /* push overwrites the link */
                rehome_block(zi, phys_to_pfn(cur), o);
                cur = next;
            }
        spin_unlock_irqrestore(&z0->lock, fl);
    }

    /* Recount managed per node. */
    for (int nd = 0; nd < g_nr_nodes; nd++)
        for (int zi = 0; zi < NR_ZONES; zi++) zones_n[nd][zi].managed = 0;
    for (uint32_t pfn = pmm_pfn_base; pfn < pmm_nr_frames; pfn++) {
        if (page_state[pfn] == PS_NONE) continue;
        struct zone* z = zone_for_pfn(pfn);
        if (z) z->managed++;
    }
    uint32_t managed_after = pmm_managed_frames(), free_after = pmm_free_frames();
    if (managed_after != managed_before || free_after != free_before)
        kprintf("pmm: !! NUMA re-home changed the totals: managed %u -> %u, "
                "free %u -> %u\n", managed_before, managed_after, free_before, free_after);
    for (int nd = 0; nd < g_nr_nodes; nd++) {
        struct pmm_node_info ni;
        pmm_node_stats(nd, &ni);
        kprintf("pmm: NUMA node %d: %u MiB (%u MiB free)\n",
                nd, (ni.managed * 4) / 1024, (ni.free * 4) / 1024);
    }
}

/* -------------------------------------------------------------------------- */
/* Stats.                                                                     */
/* -------------------------------------------------------------------------- */

static uint32_t sum_zones(int free) {
    uint32_t t = 0;
    for (int nd = 0; nd < g_nr_nodes; nd++)
        for (int zi = 0; zi < NR_ZONES; zi++)
            t += free ? zones_n[nd][zi].free_frames : zones_n[nd][zi].managed;
    return t;
}
uint32_t pmm_managed_frames(void) { return sum_zones(0); }
uint32_t pmm_free_frames(void)    { return sum_zones(1); }
uint32_t pmm_used_frames(void) {
    return pmm_managed_frames() - pmm_free_frames();
}

/* Summed over every node — the view every caller had before NUMA. */
void pmm_zone_stats(int zone, uint32_t* out_free_per_order, uint32_t* out_managed) {
    if (zone < 0 || zone >= NR_ZONES) return;
    if (out_managed) *out_managed = 0;
    if (out_free_per_order)
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++) out_free_per_order[o] = 0;
    for (int nd = 0; nd < g_nr_nodes; nd++) {
        struct zone* z = &zones_n[nd][zone];
        if (out_managed) *out_managed += z->managed;
        if (out_free_per_order)
            for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
                out_free_per_order[o] += z->nr_at_order[o];
    }
}

/* §M19.5.3 — the per-node views. */
int pmm_node_of(pmm_phys_t phys) { return node_of_pfn(phys_to_pfn(phys)); }
int pmm_node_count(void) { return g_nr_nodes; }
void pmm_node_zone_stats(int node, int zone, uint32_t* out_free_per_order,
                         uint32_t* out_managed) {
    if (node < 0 || node >= g_nr_nodes || zone < 0 || zone >= NR_ZONES) return;
    struct zone* z = &zones_n[node][zone];
    if (out_managed) *out_managed = z->managed;
    if (out_free_per_order)
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++) out_free_per_order[o] = z->nr_at_order[o];
}
void pmm_node_stats(int node, struct pmm_node_info* out) {
    if (!out) return;
    out->managed = out->free = out->hit = out->miss = 0;
    if (node < 0 || node >= g_nr_nodes) return;
    for (int zi = 0; zi < NR_ZONES; zi++) {
        out->managed += zones_n[node][zi].managed;
        out->free    += zones_n[node][zi].free_frames;
    }
    out->hit  = __atomic_load_n(&g_node_hit[node],  __ATOMIC_RELAXED);
    out->miss = __atomic_load_n(&g_node_miss[node], __ATOMIC_RELAXED);
}

/* DEBUG — walk every zone's free lists following the intrusive links and
 * assert each node is frame-aligned, in-range, and page_state-tagged with its
 * order.  Prints the first anomaly (a corrupted link => a bad split/coalesce).
 * Compares the walked count against nr_at_order.  Bounded so a cyclic/garbage
 * chain can't loop forever. */
void pmm_validate(const char* tag) {
    for (int zn = 0; zn < NR_ZONES * g_nr_nodes; zn++) {
        int zi = zn % NR_ZONES;
        struct zone* z = &zones_n[zn / NR_ZONES][zi];
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
            pmm_phys_t cur = z->free_lists[o];
            pmm_phys_t prevnode = 0;
            uint32_t walked = 0;
            uint32_t guard = z->nr_at_order[o] + 4;
            while (cur) {
                if (cur & (PMM_FRAME_SIZE - 1)) {
                    kprintf("PMMCHK[%s]: z%d o%d node phys=%llx NOT frame-aligned\n", tag, zi, o,
                            (unsigned long long)cur);
                    return;
                }
                uint32_t pfn = phys_to_pfn(cur);
                if (pfn >= pmm_nr_frames || pfn < pmm_pfn_base) {
                    kprintf("PMMCHK[%s]: z%d o%d node pfn=%x OUT OF RANGE (phys=%llx)\n", tag, zi, o, pfn,
                            (unsigned long long)cur);
                    return;
                }
                if (node_of_pfn(pfn) != z->node) {
                    kprintf("PMMCHK[%s]: node %d z%d o%d block pfn=%x belongs to node %d\n",
                            tag, z->node, zi, o, pfn, node_of_pfn(pfn));
                    return;
                }
                if (page_state[pfn] != (uint8_t)o) {
                    kprintf("PMMCHK[%s]: z%d o%d node pfn=%x state=%u (expected %d)\n",
                            tag, zi, o, pfn, page_state[pfn], o);
                    return;
                }
                /* The PREV link must name the node we came from (doubly
                 * linked since 2026-09-26; a stale prev corrupts the next
                 * O(1) removal silently). */
                if (link_load_at(cur, 1) != prevnode) {
                    kprintf("PMMCHK[%s]: z%d o%d node pfn=%x has prev=%x, expected %x\n",
                            tag, zi, o, pfn, (unsigned)link_load_at(cur, 1),
                            (unsigned)prevnode);
                    return;
                }
                prevnode = cur;
                cur = link_load(cur);
                if (++walked > guard) {
                    kprintf("PMMCHK[%s]: z%d o%d chain OVERRUNS nr_at_order=%u (cycle?)\n",
                            tag, zi, o, z->nr_at_order[o]);
                    return;
                }
            }
            if (walked != z->nr_at_order[o]) {
                kprintf("PMMCHK[%s]: z%d o%d walked=%u != nr_at_order=%u\n",
                        tag, zi, o, walked, z->nr_at_order[o]);
                return;
            }
        }
    }
    kprintf("PMMCHK[%s]: all free lists consistent\n", tag);
}

void pmm_print_stats(void) {
    uint32_t total_mgr  = pmm_managed_frames();
    uint32_t total_free = pmm_free_frames();
    uint32_t zm[NR_ZONES], zf[NR_ZONES];
    for (int zi = 0; zi < NR_ZONES; zi++) {
        zm[zi] = zf[zi] = 0;
        for (int nd = 0; nd < g_nr_nodes; nd++) {
            zm[zi] += zones_n[nd][zi].managed;
            zf[zi] += zones_n[nd][zi].free_frames;
        }
    }
    kprintf("pmm: managed=%u free=%u used=%u (%u/%u MiB free) | DMA: m=%u f=%u | DMA32: m=%u f=%u | NORMAL: m=%u f=%u | HIGHMEM: m=%u f=%u\n",
            total_mgr, total_free, total_mgr - total_free,
            (total_free * 4) / 1024, (total_mgr * 4) / 1024,
            zm[ZONE_DMA], zf[ZONE_DMA], zm[ZONE_DMA32], zf[ZONE_DMA32],
            zm[ZONE_NORMAL], zf[ZONE_NORMAL], zm[ZONE_HIGHMEM], zf[ZONE_HIGHMEM]);
    {
        uint32_t rkb, refused; int low;
        pmm_reserve_stats(&rkb, &refused, &low);
        kprintf("pmm: reserve %u KiB for the system (mem.reserve_kb) - %s, %u user "
                "frame(s) refused so far\n", rkb, low ? "BELOW it now" : "above it",
                refused);
    }
    if (g_nr_nodes > 1)
        for (int nd = 0; nd < g_nr_nodes; nd++) {
            struct pmm_node_info ni;
            pmm_node_stats(nd, &ni);
            kprintf("pmm: node %d: %u MiB, %u MiB free, local %u, fell back %u\n",
                    nd, (ni.managed * 4) / 1024, (ni.free * 4) / 1024, ni.hit, ni.miss);
        }
}

/* §M72 — the reserve is a machine setting (a user store may not lower it). */
CONFIG_KEY(ck_mem_reserve) = {
    .key = "mem.reserve_kb", .group = "System", .type = CFG_INT, .def = "4096",
    .help = "memory kept back for the system: programs' new memory is refused "
            "below this much free (KiB)",
};
static void reserve_watch(const char* key, const char* value) {
    (void)key; (void)value;
    pmm_reserve_changed();
}
CONFIG_WATCH(pmm_reserve_watch) = {
    .prefix  = "mem.reserve_kb",
    .changed = reserve_watch,
};
