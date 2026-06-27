/* =============================================================================
 * kmalloc.h — kernel heap allocator.
 *
 * After `kmalloc_init` (called from kernel_main once paging is on) the
 * heap lives at virtual `KHEAP_START` and currently has a fixed size
 * (`KHEAP_SIZE`).  Future revisions may grow on demand by mapping more
 * frames from the PMM when the free list runs dry; the public API does
 * not change.
 *
 * Allocations are 8-byte aligned.  Each chunk carries a small header
 * directly before the returned pointer; do NOT free a pointer that did
 * not come from `kmalloc` — the header decode would walk into garbage.
 *
 * Concurrency: single-threaded today.  Once we have IRQ-safe allocations
 * (e.g. a driver wants to allocate from inside an ISR) the alloc/free
 * critical sections must wrap in `hal_intr_save / restore`.
 * =========================================================================== */

#ifndef KMALLOC_H
#define KMALLOC_H

#include <stdint.h>

/* `size_t` is provided by the freestanding `<stddef.h>`. */
#include <stddef.h>

/* Heap location and initial extent — exposed so other subsystems can
 * sanity-check pointers during debugging. */
#define KHEAP_START   0xD0000000u
#define KHEAP_SIZE    (4u * 1024u * 1024u)      /* 4 MiB */

/* One-time init.  Maps KHEAP_SIZE bytes of virtual memory starting at
 * KHEAP_START using fresh frames from the PMM, then plants a single
 * "all free" chunk covering the whole region. */
void kmalloc_init(void);

/* Allocate `size` bytes.  Returns an 8-byte aligned pointer to at least
 * `size` usable bytes, or NULL if no chunk fits.  `size == 0` returns
 * NULL by convention (consistent with the spirit of malloc(0) being
 * implementation-defined; we pick the more useful one). */
void* kmalloc(size_t size);

/* Allocate `n * size` bytes, zeroed.  Returns NULL on overflow or OOM. */
void* kcalloc(size_t n, size_t size);

/* Return a previously allocated pointer to the heap.  Coalesces with
 * adjacent free neighbors so future allocations can use the merged
 * region.  Safe to pass NULL (no-op). */
void  kfree(void* p);

/* Diagnostics — used by `meminfo` / `kmstat` shell commands. */
struct kmstat {
    size_t   total_bytes;
    size_t   used_bytes;
    size_t   free_bytes;
    uint32_t chunk_count;
    uint32_t free_chunk_count;
};
void kmalloc_stats(struct kmstat* out);

#endif
