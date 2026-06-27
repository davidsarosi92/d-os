/* =============================================================================
 * kmalloc.c — kernel heap allocator (block free-list, K&R style).
 *
 * Layout of the heap:
 *
 *      KHEAP_START
 *      +---------------+--------+---------------+--------+
 *      |  hdr   |       payload      |  hdr   |   payload  | ...
 *      +---------------+--------+---------------+--------+
 *      ^               ^
 *      chunk           pointer returned to caller
 *
 * Each chunk has an inline header `struct chunk` containing its total
 * size (header + payload), a "free" flag, and a forward link to the
 * next chunk.  Chunks are laid out contiguously in memory; the `next`
 * link is therefore redundant with `(char*)c + c->size`, but we keep it
 * for cheap traversal and to make a future allocator that interleaves
 * free chunks in arbitrary order easier to retrofit.
 *
 * `kmalloc(n)` walks the list looking for the first chunk that is free
 * and at least `n + sizeof(chunk)` bytes large; if much bigger, the
 * chunk is split in two so the leftover stays available.
 *
 * `kfree(p)` finds the header right before `p`, marks it free, and
 * tries to coalesce with its forward neighbor.  Backward coalescing
 * requires a list walk to find the predecessor (the inline link is
 * forward-only); for a heap with hundreds of chunks this is fine — if
 * it ever becomes a hotspot we'll add a back link or a free-list
 * indexed by size class.
 *
 * Failure modes we deliberately leave loud (kprintf to console + serial)
 * so a misbehaving caller is visible during development.
 * =========================================================================== */

#include "kmalloc.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Tunables.                                                                   */
/* -------------------------------------------------------------------------- */

/* Allocations are aligned to 8 bytes — sufficient for double-precision
 * floats and 64-bit integers, and matches the System V i386 minimum.   */
#define KMALLOC_ALIGN 8u

/* If a candidate free chunk is larger than the requested size by at
 * least this many bytes, split it so the leftover stays usable.  Below
 * this threshold we hand out the whole chunk unchanged (avoids a
 * proliferation of tiny unusable fragments). */
#define KMALLOC_SPLIT_MIN 32u

/* -------------------------------------------------------------------------- */
/* Chunk header layout.                                                        */
/* -------------------------------------------------------------------------- */

struct chunk {
    size_t        size;         /* total size, header + payload, in bytes  */
    uint32_t      free;         /* 1 = available, 0 = in use               */
    struct chunk* next;         /* next chunk in physical order            */
    /* payload follows immediately after this header                       */
};

/* Round up to the nearest multiple of KMALLOC_ALIGN. */
static inline size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

/* -------------------------------------------------------------------------- */
/* Module state.                                                               */
/* -------------------------------------------------------------------------- */

static struct chunk* heap_head = NULL;          /* first chunk in the heap */
static int           initialized = 0;

/* -------------------------------------------------------------------------- */
/* Init — map the heap region and plant the initial single free chunk.        */
/* -------------------------------------------------------------------------- */

void kmalloc_init(void) {
    if (initialized) return;

    /* Pull KHEAP_SIZE / 4 KiB frames from the PMM and map each into the
     * heap window with `vmm_map`.  4 KiB at a time keeps the API simple
     * and lets the PMM hand out non-contiguous physical frames. */
    for (uint32_t off = 0; off < KHEAP_SIZE; off += 0x1000) {
        uint32_t phys = pmm_alloc_frame();
        if (phys == 0) {
            kprintf("kmalloc: PMM exhausted while mapping heap (got %u/%u bytes)\n",
                    off, KHEAP_SIZE);
            break;
        }
        if (vmm_map(KHEAP_START + off, phys, VMM_WRITABLE) != 0) {
            kprintf("kmalloc: vmm_map failed at virt %p\n", (void*)(KHEAP_START + off));
            pmm_free_frame(phys);
            break;
        }
    }

    /* Plant a single all-free chunk covering the entire heap. */
    heap_head = (struct chunk*)KHEAP_START;
    heap_head->size = KHEAP_SIZE;
    heap_head->free = 1;
    heap_head->next = NULL;

    initialized = 1;
    kprintf("kmalloc: heap %u KiB @ %p\n", KHEAP_SIZE / 1024, (void*)KHEAP_START);
}

/* -------------------------------------------------------------------------- */
/* Allocation.                                                                 */
/* -------------------------------------------------------------------------- */

void* kmalloc(size_t size) {
    if (!initialized || size == 0) return NULL;

    /* Required total size: header + payload, rounded up so the next
     * chunk's header still lands aligned. */
    size_t need = align_up(size + sizeof(struct chunk), KMALLOC_ALIGN);

    for (struct chunk* c = heap_head; c != NULL; c = c->next) {
        if (!c->free || c->size < need) continue;

        /* Big enough — split if there's enough leftover to bother. */
        if (c->size >= need + sizeof(struct chunk) + KMALLOC_SPLIT_MIN) {
            struct chunk* tail = (struct chunk*)((char*)c + need);
            tail->size = c->size - need;
            tail->free = 1;
            tail->next = c->next;

            c->size = need;
            c->next = tail;
        }
        c->free = 0;
        return (void*)((char*)c + sizeof(struct chunk));
    }

    /* Out of memory.  A grow-on-demand policy could ask the PMM for
     * fresh frames here; for now we just fail. */
    return NULL;
}

void* kcalloc(size_t n, size_t size) {
    /* Multiplication overflow guard. */
    if (n != 0 && size > (size_t)-1 / n) return NULL;
    size_t total = n * size;

    void* p = kmalloc(total);
    if (!p) return NULL;

    /* Manual zero-fill — no memset in our libc-less environment. */
    char* b = (char*)p;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return p;
}

/* -------------------------------------------------------------------------- */
/* Free + coalesce.                                                            */
/* -------------------------------------------------------------------------- */

void kfree(void* p) {
    if (!p) return;

    /* Sanity: pointer must be inside the heap. */
    if ((uintptr_t)p < KHEAP_START + sizeof(struct chunk) ||
        (uintptr_t)p >= KHEAP_START + KHEAP_SIZE) {
        kprintf("kfree: pointer %p outside heap\n", p);
        return;
    }

    struct chunk* c = (struct chunk*)((char*)p - sizeof(struct chunk));
    if (c->free) {
        kprintf("kfree: double-free of %p\n", p);
        return;
    }
    c->free = 1;

    /* Forward coalesce — cheap because we already have c->next. */
    if (c->next && c->next->free) {
        c->size += c->next->size;
        c->next  = c->next->next;
    }

    /* Backward coalesce — find the chunk whose `next` is `c`.  O(n) walk;
     * acceptable for now, see the file header comment. */
    for (struct chunk* w = heap_head; w; w = w->next) {
        if (w->next == c && w->free) {
            w->size += c->size;
            w->next  = c->next;
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Stats.                                                                      */
/* -------------------------------------------------------------------------- */

void kmalloc_stats(struct kmstat* out) {
    if (!out) return;

    out->total_bytes      = KHEAP_SIZE;
    out->used_bytes       = 0;
    out->free_bytes       = 0;
    out->chunk_count      = 0;
    out->free_chunk_count = 0;

    for (struct chunk* c = heap_head; c; c = c->next) {
        out->chunk_count++;
        if (c->free) {
            out->free_bytes += c->size;
            out->free_chunk_count++;
        } else {
            out->used_bytes += c->size;
        }
    }
}
