/* =============================================================================
 * kmalloc.h — kernel heap allocator (size-class slab + page-alloc).
 *
 * After `kmalloc_init` (called from kernel_main once paging is on),
 * small allocations (<= 2048 B) come from per-size slab caches and
 * larger allocations come from the buddy page allocator.  Each slab
 * cache has per-CPU magazines for a lock-free common path.
 *
 * The public API has not changed since M1 — only the backend.  Drivers
 * that called `kmalloc / kfree / kcalloc` continue to work unchanged.
 *
 * Allocations are 8-byte aligned.  `kmalloc` returns 4-KiB-aligned
 * pointers for any allocation > 2048 B (because they come directly
 * from `page_alloc`); smaller allocations are 8-byte aligned but not
 * page-aligned (the slab header sits at the start of the page).
 *
 * Concurrency: slab fast paths run with IRQs off + per-CPU magazines;
 * slow paths (refill / flush / big-alloc) acquire the matching
 * spinlock.  Safe to call from IRQ context.
 * =========================================================================== */

#ifndef KMALLOC_H
#define KMALLOC_H

#include <stdint.h>
#include <stddef.h>

/* One-time init.  Wires up the slab caches and clears the big-alloc
 * side table.  Idempotent — safe to call more than once. */
void kmalloc_init(void);

/* Allocate `size` bytes.  Returns an aligned pointer, or NULL on
 * OOM / size == 0.  Pick the smallest size-class cache that fits,
 * or fall through to a buddy page allocation if size > 2048 B. */
void* kmalloc(size_t size);

/* Allocate `n * size` bytes, zeroed.  Returns NULL on overflow or OOM. */
void* kcalloc(size_t n, size_t size);

/* Return a previously allocated pointer.  Dispatches automatically
 * between slab and page-alloc based on the owning page.  Safe to
 * pass NULL (no-op). */
void  kfree(void* p);

/* Diagnostics — used by `meminfo` / `slabinfo` shell commands.
 *
 * Field semantics (synthesized across slab + big-alloc):
 *   total_bytes      = bytes of memory the heap currently owns
 *   used_bytes       = bytes handed out to live allocations
 *   free_bytes       = total_bytes - used_bytes (slack inside slabs)
 *   chunk_count      = live allocation count
 *   free_chunk_count = free objects sitting in slabs (not big-alloc) */
struct kmstat {
    size_t   total_bytes;
    size_t   used_bytes;
    size_t   free_bytes;
    uint32_t chunk_count;
    uint32_t free_chunk_count;
};
void kmalloc_stats(struct kmstat* out);

#endif
