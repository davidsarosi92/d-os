/* =============================================================================
 * kmalloc.c — kernel heap allocator façade (M19).
 *
 * Two-layer dispatch:
 *
 *   size <= 2048 B  →  slab cache (slab_lookup_cache → slab_alloc)
 *   size  > 2048 B  →  page_alloc(order) where order = ceil_log2(pages)
 *
 * `kfree(p)` figures out which path by inspecting the page that
 * `p` lives in:
 *   1. Mask `p` to the page boundary.
 *   2. Ask the slab layer if the page is a slab page; if yes,
 *      slab_free routes it back into the magazine / cache.
 *   3. Otherwise look up `big_alloc_order[pfn]` (a side table sized to
 *      pmm_nr_frames) for the order of the page-alloc-backed
 *      allocation; if found, page_free at that order.
 *   4. Otherwise complain — the pointer wasn't from us.
 *
 * The side table is 1 byte per frame, sized at boot to pmm_nr_frames.  It
 * holds 0xFF for "not a kmalloc-big-alloc page" and 0..BUDDY_MAX_ORDER
 * for the head of one.  Interior frames stay 0xFF — kfree only looks
 * up the head pfn (== p >> 12 for a returned page-aligned pointer).
 *
 * Why no per-object header?  The slab side identifies its objects by
 * the page they live in (the slab header has SLAB_MAGIC).  For big
 * allocations the page is always 4 KiB aligned because page_alloc
 * returns frame addresses.  So we don't need any in-band tagging.
 *
 * Backwards compat: `kmalloc_init` is still called from kernel_main;
 * it just initializes the slab subsystem.  `kmalloc_stats` reports a
 * synthesized view that's roughly comparable to the old block-heap
 * counters (total_bytes / used_bytes / free_bytes / chunk_count).
 * ============================================================================= */

#include "kmalloc.h"
#include "hal_api.h"   /* phys_to_virt / virt_to_phys — kernel direct map */
#include "slab.h"
#include "pmm.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Big-allocation side table.  See file header for the layout.                */
/* -------------------------------------------------------------------------- */

#define BIG_NONE 0xFFu

/* §M48 — sized to the RAM this machine actually has, out of the PMM's boot
 * arena.  It used to be a static array dimensioned by a compile-time frame
 * cap, which forced a choice between "supports big machines" and "fits on
 * small ones"; pmm_nr_frames removes the choice.  NULL (and nr == 0) until
 * kmalloc_init, and every access is bounds-checked against nr, so a kfree
 * that somehow ran early just takes the "not ours" path. */
static uint8_t* big_alloc_order;
static uint32_t big_alloc_nr;

static uint32_t big_allocs   = 0;
static uint32_t big_bytes    = 0;     /* sum of order_to_bytes for live big allocs */

static int initialized = 0;

/* -------------------------------------------------------------------------- */
/* Small helpers.                                                             */
/* -------------------------------------------------------------------------- */

/* ceil_log2(bytes-to-pages) — gives the order argument for page_alloc. */
static int order_for_bytes(size_t bytes) {
    size_t pages = (bytes + 4095u) / 4096u;
    if (pages <= 1) return 0;
    int order = 0;
    size_t v = 1;
    while (v < pages) { v <<= 1; order++; }
    return order;
}

/* -------------------------------------------------------------------------- */
/* Init.                                                                      */
/* -------------------------------------------------------------------------- */

void kmalloc_init(void) {
    if (initialized) return;

    /* Mark the entire side table as "not a big-alloc page".  Done
     * explicitly even though fresh memory may read as zero — 0x00 is a
     * valid order (= one frame), so we'd misidentify every never-touched
     * frame as a 1-page big-alloc page.  Manual fill it is. */
    big_alloc_nr    = pmm_nr_frames;
    big_alloc_order = (uint8_t*)pmm_bootmem_alloc(big_alloc_nr);
    if (!big_alloc_order) {
        /* Without the side table kfree cannot tell a big allocation from a
         * stray pointer, so every >2 KiB block would leak.  Say so loudly
         * rather than limp: this only happens if the arena was mis-sized. */
        kprintf("kmalloc: FATAL — no boot arena for the %u KiB big-alloc table\n",
                big_alloc_nr >> 10);
        big_alloc_nr = 0;
        return;
    }
    for (uint32_t i = 0; i < big_alloc_nr; i++) big_alloc_order[i] = BIG_NONE;

    slab_init();
    initialized = 1;

    kprintf("kmalloc: slab + page_alloc backend ready\n");
}

/* -------------------------------------------------------------------------- */
/* Allocation.                                                                */
/* -------------------------------------------------------------------------- */

void* kmalloc(size_t size) {
    if (!initialized || size == 0) return NULL;

    /* Fast path: size class cache. */
    struct slab_cache* c = slab_lookup_cache(size);
    if (c) return slab_alloc(c);

    /* Slow path: page-alloc backed.  Round up to the smallest order
     * that fits, allocate from the buddy, record in the side table. */
    int order = order_for_bytes(size);
    pmm_phys_t phys = page_alloc(order, ZONE_DEFAULT);
    if (!phys) return NULL;

    /* The table spans every frame the PMM manages, so a frame it just handed
     * us is in range by construction — checked anyway, because leaking one
     * block beats corrupting whatever follows the table. */
    if ((phys >> 12) >= big_alloc_nr) {
        kprintf("kmalloc: frame %u outside the %u-frame side table\n",
                phys >> 12, big_alloc_nr);
        page_free(phys, order);
        return NULL;
    }
    big_alloc_order[phys >> 12] = (uint8_t)order;
    big_allocs++;
    big_bytes += (1u << order) * 4096u;

    return phys_to_virt(phys);
}

void* kcalloc(size_t n, size_t size) {
    /* Multiplication overflow guard. */
    if (n != 0 && size > (size_t)-1 / n) return NULL;
    size_t total = n * size;

    void* p = kmalloc(total);
    if (!p) return NULL;

    /* Zero-fill — no memset in libc-less env. */
    char* b = (char*)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}

/* -------------------------------------------------------------------------- */
/* Free.                                                                      */
/* -------------------------------------------------------------------------- */

void kfree(void* p) {
    if (!p) return;
    if (!initialized) {
        kprintf("kfree: called before init (p=%p)\n", p);
        return;
    }

    /* Try slab first — its dispatch is the cheapest (page magic
     * read).  Slab objects are NEVER page-aligned because the slab
     * header eats the first 32 bytes, so a page-aligned p can't be
     * a slab object. */
    struct slab_cache* c = slab_cache_of(p);
    if (c) {
        slab_free(c, p);
        return;
    }

    /* Otherwise check the big-alloc side table.  p must be 4 KiB
     * aligned for this to make sense. */
    if (((uintptr_t)p & 0xFFFu) != 0) {
        kprintf("kfree: pointer %p not in any known allocation\n", p);
        return;
    }

    uint32_t pfn = (uint32_t)(virt_to_phys(p) >> 12);
    if (pfn >= big_alloc_nr || big_alloc_order[pfn] == BIG_NONE) {
        kprintf("kfree: pointer %p not from kmalloc\n", p);
        return;
    }

    int order = big_alloc_order[pfn];
    big_alloc_order[pfn] = BIG_NONE;
    if (big_allocs) big_allocs--;
    if (big_bytes >= (1u << order) * 4096u) big_bytes -= (1u << order) * 4096u;

    page_free(virt_to_phys(p), order);
}

/* -------------------------------------------------------------------------- */
/* Stats — synthesized over slab + page_alloc.                                */
/*                                                                            */
/* `chunk_count` and `free_chunk_count` map awkwardly onto the slab           */
/* world (objects, not chunks), but we keep the field names to avoid          */
/* breaking the existing `meminfo` shell command.  Map:                       */
/*   total_bytes      = slab_pages * 4 KiB + big_bytes                        */
/*   used_bytes       = sum(slab in-use * slot_size) + big_bytes              */
/*   free_bytes       = slab_pages * 4 KiB - used_slab + big slack            */
/*   chunk_count      = sum(slab in-use objs) + big_allocs                    */
/*   free_chunk_count = sum(slab free objs)                                   */
/* -------------------------------------------------------------------------- */
void kmalloc_stats(struct kmstat* out) {
    if (!out) return;

    out->total_bytes      = 0;
    out->used_bytes       = 0;
    out->free_bytes       = 0;
    out->chunk_count      = 0;
    out->free_chunk_count = 0;

    int n = slab_cache_count();
    for (int i = 0; i < n; i++) {
        struct slab_stats s;
        slab_cache_get_stats(i, &s);
        out->total_bytes      += (size_t)s.slabs * 4096u;
        out->used_bytes       += (size_t)s.in_use_objs * s.slot_size;
        out->chunk_count      += s.in_use_objs;
        out->free_chunk_count += s.free_objs;
    }
    out->total_bytes += big_bytes;
    out->used_bytes  += big_bytes;
    out->chunk_count += big_allocs;
    out->free_bytes   = (out->total_bytes > out->used_bytes)
                        ? (out->total_bytes - out->used_bytes) : 0;
}
