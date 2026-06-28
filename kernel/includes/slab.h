/* =============================================================================
 * slab.h — size-class slab allocator with per-CPU magazines (M19).
 *
 * Sits between `kmalloc` (the public façade) and `page_alloc` (the
 * buddy).  Each cache hands out fixed-size objects from a pool of
 * page-backed slabs.  Caches at the standard powers of two from
 * 16 B up to 2048 B; allocations larger than 2048 B bypass the slab
 * and go straight to `page_alloc`.
 *
 * Per-CPU magazines: every cache carries a small array (one slot per
 * CPU) holding up to MAG_CAPACITY pre-fetched free objects.  The
 * common alloc/free path pops/pushes from this array under IRQ-off,
 * never touching the global cache lock.  Only refill (empty mag) and
 * flush (full mag) walk the cache's slab list under the cache lock.
 *
 * Each cache is a static singleton; no `slab_cache_create` is exposed
 * yet (the kernel only uses size-classed kmalloc today).  The lookup
 * is by size: `slab_lookup_cache(size)` returns the smallest cache
 * that fits, or NULL if the size exceeds the largest cache.
 *
 * Concurrency: per-cache spinlock for slab-list operations; per-CPU
 * magazine accesses are IRQ-off (the per-CPU index is stable across
 * an IRQ-off window because preemption and migration are gated by
 * the same IF flag).
 * ============================================================================= */

#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration; defined in slab.c. */
struct slab_cache;

/* Initialize all built-in size-class caches.  Idempotent.  Must run
 * after `pmm_init` (caches lazy-fetch their first slab from the
 * buddy allocator on the first alloc, so technically post-init is
 * enough — but doing the descriptor wiring here keeps boot order
 * easy to audit). */
void slab_init(void);

/* Locate the smallest size-class cache that fits an allocation of
 * `size` bytes.  Returns NULL if `size` is larger than the biggest
 * cache (caller falls back to page_alloc). */
struct slab_cache* slab_lookup_cache(size_t size);

/* Allocate / free one object from a specific cache.  These are the
 * fast paths; `kmalloc` calls them after picking the right cache. */
void* slab_alloc(struct slab_cache* c);
void  slab_free (struct slab_cache* c, void* obj);

/* Given a pointer that might or might not come from the slab, find
 * the owning cache by inspecting the page header.  Returns the
 * cache* if it's a slab object, NULL otherwise.  Used by `kfree`. */
struct slab_cache* slab_cache_of(void* obj);

/* Stats for `slabinfo`.  `n_caches` is the count of built-in caches;
 * each `slab_cache_stats` call reports one of them. */
struct slab_stats {
    const char* name;
    size_t      obj_size;
    size_t      slot_size;
    uint32_t    slabs;        /* total slab pages held */
    uint32_t    in_use_objs;  /* allocated objects */
    uint32_t    free_objs;    /* free objects (in slabs + mags) */
    uint32_t    mag_total;    /* objects currently in any magazine */
};
int  slab_cache_count(void);
void slab_cache_get_stats(int idx, struct slab_stats* out);

#endif
