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
 *   ZONE_DMA    : pfn  [0,    4096)  — first 16 MiB; legacy ISA + small DMA
 *   ZONE_NORMAL : pfn  [4096, ...)   — bulk of RAM up to BUDDY_MAX_FRAMES
 *   ZONE_HIGHMEM:                    — slot reserved; not populated
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
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Sizing and helpers.                                                        */
/* -------------------------------------------------------------------------- */

/* `0xFF` = doesn't exist; `0xFE` = allocated / interior; <=BUDDY_MAX_ORDER = */
/* head of free block at that order.  See file header.                       */
#define PS_NONE     0xFFu
#define PS_USED     0xFEu

static uint8_t page_state[BUDDY_MAX_FRAMES];

#define ZONE_DMA_FRAME_LIMIT  4096u         /* 16 MiB / 4 KiB */

struct zone {
    const char* name;
    uint32_t    start_pfn;          /* inclusive */
    uint32_t    end_pfn;            /* exclusive */
    uint32_t    managed;            /* total frames ever marked free here */
    uint32_t    free_frames;        /* dynamic count */
    uint32_t    free_lists[BUDDY_MAX_ORDER + 1];  /* head = first free pfn at order, 0 = empty */
    uint32_t    nr_at_order[BUDDY_MAX_ORDER + 1]; /* diagnostic */
    spinlock_t  lock;
};

static struct zone zones[NR_ZONES];

/* Symbols from linker.ld marking the kernel image bounds. */
extern uint8_t kernel_start[];
extern uint8_t kernel_end[];

/* -------------------------------------------------------------------------- */
/* Tiny helpers.                                                              */
/* -------------------------------------------------------------------------- */

static inline uint32_t pfn_to_phys(uint32_t pfn) { return pfn << PMM_FRAME_SHIFT; }
static inline uint32_t phys_to_pfn(uint32_t p)   { return p   >> PMM_FRAME_SHIFT; }

/* Which zone owns this pfn?  Returns zone index, or -1 if out of range. */
static int zone_of_pfn(uint32_t pfn) {
    if (pfn >= BUDDY_MAX_FRAMES) return -1;
    if (pfn <  ZONE_DMA_FRAME_LIMIT) return ZONE_DMA;
    return ZONE_NORMAL;
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

static inline uint32_t link_load(uint32_t phys) {
    return *(volatile uint32_t*)(uintptr_t)phys;
}
static inline void link_store(uint32_t phys, uint32_t next) {
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
    uintptr_t ks = (uintptr_t)kernel_start, ke = (uintptr_t)kernel_end;
    if ((uintptr_t)phys >= ks && (uintptr_t)phys < ke) {
        kprintf("PMM-GUARD: link_store 0x%x INTO kernel image [0x%x,0x%x) "
                "pfn=%u caller=%p\n", phys, (uint32_t)ks, (uint32_t)ke,
                phys >> PMM_FRAME_SHIFT, __builtin_return_address(0));
    }
    *(volatile uint32_t*)(uintptr_t)phys = next;
}

/* Push a free block of `order` onto a zone's free list.  Caller holds
 * zone->lock.  Stamps the head pfn's state with the order so coalesce
 * can recognize it as the same-order partner. */
static void zone_push(struct zone* z, uint32_t pfn, int order) {
    uint32_t phys = pfn_to_phys(pfn);
    link_store(phys, z->free_lists[order]);
    z->free_lists[order]   = phys;
    z->nr_at_order[order] += 1;
    page_state[pfn]        = (uint8_t)order;
}

/* Remove a specific (pfn, order) from the zone's free list.  O(list_len)
 * walk; only invoked during coalesce, where the buddy we're trying to
 * pull off is generally near the head.  Caller holds zone->lock. */
static int zone_remove(struct zone* z, uint32_t pfn, int order) {
    uint32_t target = pfn_to_phys(pfn);
    uint32_t prev   = 0;
    uint32_t cur    = z->free_lists[order];
    while (cur) {
        uint32_t next = link_load(cur);
        if (cur == target) {
            if (prev) link_store(prev, next);
            else      z->free_lists[order] = next;
            z->nr_at_order[order] -= 1;
            return 0;
        }
        prev = cur;
        cur  = next;
    }
    return -1;
}

/* Pop the head of a zone's free list at `order`.  Returns pfn, or 0 if
 * empty.  Caller holds zone->lock. */
static uint32_t zone_pop(struct zone* z, int order) {
    uint32_t head = z->free_lists[order];
    if (!head) return 0;
    z->free_lists[order] = link_load(head);
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
static void carve_out_range(uint32_t start, uint32_t end) {
    uint32_t s = start / PMM_FRAME_SIZE;
    uint32_t e = (end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (e > BUDDY_MAX_FRAMES) e = BUDDY_MAX_FRAMES;
    for (uint32_t i = s; i < e; i++) page_state[i] = PS_NONE;
}

/* Mark a single frame as part of an AVAILABLE region — initially we
 * tag it PS_USED (= "allocated") so seeding can later free it via
 * the normal coalescing path.  This keeps the seed loop simple. */
static void seed_mark_available(uint32_t pfn) {
    if (pfn >= BUDDY_MAX_FRAMES) return;
    /* Only flip if not already carved out. */
    if (page_state[pfn] == PS_NONE) page_state[pfn] = PS_USED;
}

/* Free a single frame into its zone via the standard coalescing path.
 * Used only by the post-init seeding loop. */
static void buddy_release_one(uint32_t pfn);

/* -------------------------------------------------------------------------- */
/* Init.                                                                      */
/* -------------------------------------------------------------------------- */

void pmm_init(void) {
    /* Set up zone descriptors. */
    zones[ZONE_DMA].name        = "DMA";
    zones[ZONE_DMA].start_pfn   = 0;
    zones[ZONE_DMA].end_pfn     = ZONE_DMA_FRAME_LIMIT;
    spin_lock_init(&zones[ZONE_DMA].lock);

    zones[ZONE_NORMAL].name     = "NORMAL";
    zones[ZONE_NORMAL].start_pfn = ZONE_DMA_FRAME_LIMIT;
    zones[ZONE_NORMAL].end_pfn  = BUDDY_MAX_FRAMES;
    spin_lock_init(&zones[ZONE_NORMAL].lock);

    zones[ZONE_HIGHMEM].name    = "HIGHMEM";
    spin_lock_init(&zones[ZONE_HIGHMEM].lock);
    /* HIGHMEM stays empty — no frames mapped to it today. */

    /* Initialize every frame as PS_NONE (doesn't exist).  The mmap walk
     * flips bits to PS_USED for frames inside AVAILABLE regions, then
     * the reservation pass carves out kernel image / low memory etc.
     * Finally the seeding loop frees the remainder. */
    for (uint32_t i = 0; i < BUDDY_MAX_FRAMES; i++) page_state[i] = PS_NONE;

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
    uintptr_t max_phys = 0;
    {
        uintptr_t wp = mbi->mmap_addr;
        uintptr_t wend = mbi->mmap_addr + mbi->mmap_length;
        int wb = 64;
        while (wp < wend && wb-- > 0) {
            const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)wp;
            if (e->type == MMAP_TYPE_AVAILABLE) {
                uint64_t hi = e->base + e->length;
                /* Cap at the buddy's static metadata range — we won't
                 * manage anything above that. */
                uint64_t cap = (uint64_t)BUDDY_MAX_FRAMES * PMM_FRAME_SIZE;
                if (hi > cap) hi = cap;
                if ((uintptr_t)hi > max_phys) max_phys = (uintptr_t)hi;
            }
            wp += e->size + 4;
        }
    }
    uintptr_t covered = hal_extend_identity_map(max_phys);
    if (covered < max_phys) {
        kprintf("pmm: identity map caps at %u MiB (RAM goes up to %u MiB) — "
                "HIGHMEM frames will be skipped\n",
                (unsigned)(covered >> 20), (unsigned)(max_phys >> 20));
    } else if (covered > (uintptr_t)1 * 1024 * 1024 * 1024) {
        kprintf("pmm: identity map extended to %u MiB\n",
                (unsigned)(covered >> 20));
    }

    /* Pass 1: tag AVAILABLE frames as PS_USED.  Anything outside an
     * AVAILABLE region stays PS_NONE.  Cap at `covered` so we never
     * try to dereference a frame the HAL didn't make reachable (i386:
     * stuck at 256 MiB until kmap lands; x86_64: extended above). */
    uint32_t cover_frames = (uint32_t)(covered / PMM_FRAME_SIZE);
    if (cover_frames > BUDDY_MAX_FRAMES) cover_frames = BUDDY_MAX_FRAMES;

    uintptr_t p   = mbi->mmap_addr;
    uintptr_t end = mbi->mmap_addr + mbi->mmap_length;
    int entry_budget = 64;
    while (p < end && entry_budget-- > 0) {
        const struct mboot_mmap_entry* e = (const struct mboot_mmap_entry*)p;
        if (e->type == MMAP_TYPE_AVAILABLE) {
            uint64_t base = e->base;
            uint64_t len  = e->length;

            /* Don't even consider regions starting above the identity
             * cap — we can't address them. */
            if (base >= (uint64_t)covered) { p += e->size + 4; continue; }
            if (base + len > (uint64_t)covered) len = (uint64_t)covered - base;

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

    /* (a) Frame 0 — NULL safety. */
    page_state[0] = PS_NONE;

    /* (b) Everything below 1 MiB (BIOS / VGA / EBDA / option ROMs). */
    carve_out_range(0, 0x100000);

    /* (c) Kernel image bounds from linker.ld. */
    carve_out_range((uint32_t)(uintptr_t)kernel_start,
                    (uint32_t)(uintptr_t)kernel_end);

    /* (d) Multiboot info + the attached memory map (lives outside
     *     the kernel image, can land anywhere in low memory). */
    carve_out_range((uint32_t)(uintptr_t)mbi,
                    (uint32_t)(uintptr_t)mbi + sizeof(struct mboot_info));
    carve_out_range(mbi->mmap_addr, mbi->mmap_addr + mbi->mmap_length);

    /* (e) AP trampoline destination + per-AP info (M18 puts these at
     *     fixed low addresses).  Reserving a generous 16 KiB window
     *     covers the trampoline (~256 bytes), the ap_info struct
     *     (4 KiB later), and slop for future expansion. */
    carve_out_range(0x8000, 0x8000 + 0x4000);

    /* Pass 3: seed the buddy free lists.  Walk every frame; for each
     * frame still in PS_USED state (i.e. AVAILABLE and not reserved),
     * release it as order-0.  The coalescing path in page_free will
     * automatically build up higher-order blocks. */
    uint32_t initially_free = 0;
    for (uint32_t pfn = 0; pfn < BUDDY_MAX_FRAMES; pfn++) {
        if (page_state[pfn] == PS_USED) {
            int z = zone_of_pfn(pfn);
            if (z >= 0) zones[z].managed++;
            buddy_release_one(pfn);
            initially_free++;
        }
    }

    kprintf("pmm: buddy ready — DMA managed=%u free=%u, NORMAL managed=%u free=%u (%u MiB total free)\n",
            zones[ZONE_DMA].managed, zones[ZONE_DMA].free_frames,
            zones[ZONE_NORMAL].managed, zones[ZONE_NORMAL].free_frames,
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
         * across DMA/NORMAL boundary. */
        if (buddy_pfn >= BUDDY_MAX_FRAMES) break;
        if (buddy_pfn <  z->start_pfn || buddy_pfn >= z->end_pfn) break;

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

/* Init-time helper — same as buddy_free_in_zone but takes a pfn
 * directly and infers the zone.  Used by the seeding loop. */
static void buddy_release_one(uint32_t pfn) {
    int zi = zone_of_pfn(pfn);
    if (zi < 0) return;
    /* Mark the frame USED so buddy_free has the right precondition,
     * then release at order 0. */
    page_state[pfn] = PS_USED;
    buddy_free_in_zone(&zones[zi], pfn, 0);
}

/* -------------------------------------------------------------------------- */
/* Public API: order-aware page_alloc / page_free.                            */
/* -------------------------------------------------------------------------- */

uint32_t page_alloc(int order, int zone_hint) {
    if (order < 0 || order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;

    /* Try the hinted zone first, then fall back per Linux convention:
     * NORMAL -> DMA on default; DMA-only on ZONE_DMA; NORMAL-only on
     * ZONE_NORMAL.  Keeps drivers that need DMA-reachable memory
     * from accidentally getting a NORMAL frame they can't address. */
    int try_order[3] = { -1, -1, -1 };
    int n = 0;

    if (zone_hint == ZONE_DMA) {
        try_order[n++] = ZONE_DMA;
    } else if (zone_hint == ZONE_NORMAL) {
        try_order[n++] = ZONE_NORMAL;
    } else {
        /* ZONE_DEFAULT or anything unrecognised. */
        try_order[n++] = ZONE_NORMAL;
        try_order[n++] = ZONE_DMA;
    }

    for (int i = 0; i < n; i++) {
        uint32_t pfn = buddy_alloc_in_zone(&zones[try_order[i]], order);
        if (pfn) return pfn_to_phys(pfn);
    }
    return PMM_ALLOC_FAIL;
}

void page_free(uint32_t phys, int order) {
    if (phys == 0) return;
    if (phys & (PMM_FRAME_SIZE - 1)) return;   /* misaligned — caller bug */

    uint32_t pfn = phys_to_pfn(phys);
    int zi = zone_of_pfn(pfn);
    if (zi < 0) return;

    buddy_free_in_zone(&zones[zi], pfn, order);
}

/* -------------------------------------------------------------------------- */
/* Legacy API wrappers — keep call sites working unchanged.                   */
/* -------------------------------------------------------------------------- */

uint32_t pmm_alloc_frame(void) {
    return page_alloc(0, ZONE_DEFAULT);
}

uint32_t pmm_alloc_contiguous(uint32_t n) {
    if (n == 0) return PMM_ALLOC_FAIL;
    if (n == 1) return page_alloc(0, ZONE_DEFAULT);

    int order = ceil_log2(n);
    if (order > BUDDY_MAX_ORDER) return PMM_ALLOC_FAIL;
    return page_alloc(order, ZONE_DEFAULT);
}

void pmm_free_frame(uint32_t addr) {
    page_free(addr, 0);
}

/* -------------------------------------------------------------------------- */
/* Stats.                                                                     */
/* -------------------------------------------------------------------------- */

uint32_t pmm_managed_frames(void) {
    return zones[ZONE_DMA].managed + zones[ZONE_NORMAL].managed;
}
uint32_t pmm_free_frames(void) {
    return zones[ZONE_DMA].free_frames + zones[ZONE_NORMAL].free_frames;
}
uint32_t pmm_used_frames(void) {
    return pmm_managed_frames() - pmm_free_frames();
}

void pmm_zone_stats(int zone, uint32_t* out_free_per_order, uint32_t* out_managed) {
    if (zone < 0 || zone >= NR_ZONES) return;
    struct zone* z = &zones[zone];

    if (out_managed) *out_managed = z->managed;
    if (out_free_per_order) {
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++)
            out_free_per_order[o] = z->nr_at_order[o];
    }
}

/* DEBUG — walk every zone's free lists following the intrusive links and
 * assert each node is frame-aligned, in-range, and page_state-tagged with its
 * order.  Prints the first anomaly (a corrupted link => a bad split/coalesce).
 * Compares the walked count against nr_at_order.  Bounded so a cyclic/garbage
 * chain can't loop forever. */
void pmm_validate(const char* tag) {
    for (int zi = 0; zi < NR_ZONES; zi++) {
        struct zone* z = &zones[zi];
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
            uint32_t cur = z->free_lists[o];
            uint32_t walked = 0;
            uint32_t guard = z->nr_at_order[o] + 4;
            while (cur) {
                if (cur & (PMM_FRAME_SIZE - 1)) {
                    kprintf("PMMCHK[%s]: z%d o%d node phys=%x NOT frame-aligned\n", tag, zi, o, cur);
                    return;
                }
                uint32_t pfn = phys_to_pfn(cur);
                if (pfn >= BUDDY_MAX_FRAMES) {
                    kprintf("PMMCHK[%s]: z%d o%d node pfn=%x OUT OF RANGE (phys=%x)\n", tag, zi, o, pfn, cur);
                    return;
                }
                if (page_state[pfn] != (uint8_t)o) {
                    kprintf("PMMCHK[%s]: z%d o%d node pfn=%x state=%u (expected %d)\n",
                            tag, zi, o, pfn, page_state[pfn], o);
                    return;
                }
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
    kprintf("pmm: managed=%u free=%u used=%u (%u/%u MiB free) | DMA: m=%u f=%u | NORMAL: m=%u f=%u\n",
            total_mgr, total_free, total_mgr - total_free,
            (total_free * 4) / 1024, (total_mgr * 4) / 1024,
            zones[ZONE_DMA].managed,    zones[ZONE_DMA].free_frames,
            zones[ZONE_NORMAL].managed, zones[ZONE_NORMAL].free_frames);
}
