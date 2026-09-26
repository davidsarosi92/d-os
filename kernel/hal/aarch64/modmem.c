/* =============================================================================
 * modmem.c — memory for loadable modules, within reach of the kernel (§M85).
 *
 * The kernel is compiled for the small code model: a call is a 26-bit branch
 * (+-128 MiB) and a data reference an ADRP (+-4 GiB).  A module is linked
 * against the kernel's exported symbols with those same relocations, so its
 * code must sit near the kernel image.  Since stage 4 the image runs at the top
 * of TTBR1 and the heap lives in the direct map ~509 GiB away, so modules get
 * this arena instead: part of the image's own .bss, i.e. inside its 2 MiB-
 * block mapping and a few MiB from every symbol it calls.
 *
 * A first-fit list with coalescing — modules are few, loaded rarely, and freed
 * on rmmod; nothing here is on a hot path.  The arena is executable because the
 * image mapping is (no W^X on this arch yet — stated in §M67, still true).
 * ============================================================================= */

#include <stdint.h>
#include <stddef.h>
#include "lock.h"
#include "printf.h"

#define MODMEM_SIZE (8u << 20)

struct mblk { uint32_t size; uint32_t used; };        /* header, 16-aligned */
#define HDR  16u

static uint8_t g_arena[MODMEM_SIZE] __attribute__((aligned(4096)));
static int g_init;
static spinlock_t g_lock;

static struct mblk* at(uint32_t off) { return (struct mblk*)(g_arena + off); }

void* module_mem_alloc(size_t n) {
    uint32_t fl = spin_lock_irqsave(&g_lock);
    if (!g_init) { at(0)->size = MODMEM_SIZE; at(0)->used = 0; g_init = 1; }
    uint32_t need = (uint32_t)((n + HDR + 15) & ~15u);
    void* r = NULL;
    for (uint32_t off = 0; off < MODMEM_SIZE; off += at(off)->size) {
        struct mblk* b = at(off);
        if (b->used || b->size < need) continue;
        if (b->size - need >= 64) {                     /* split */
            at(off + need)->size = b->size - need;
            at(off + need)->used = 0;
            b->size = need;
        }
        b->used = 1;
        r = g_arena + off + HDR;
        break;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    if (!r) kprintf("modmem: no room for %u bytes in the %u MiB module arena\n",
                    (unsigned)n, MODMEM_SIZE >> 20);
    return r;
}

void module_mem_free(void* p) {
    if (!p) return;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    uint32_t off = (uint32_t)((uint8_t*)p - g_arena) - HDR;
    if (off < MODMEM_SIZE) {
        at(off)->used = 0;
        /* Coalesce every run of free neighbours (one pass over a short list). */
        for (uint32_t o = 0; o < MODMEM_SIZE; o += at(o)->size)
            while (!at(o)->used && o + at(o)->size < MODMEM_SIZE && !at(o + at(o)->size)->used)
                at(o)->size += at(o + at(o)->size)->size;
    }
    spin_unlock_irqrestore(&g_lock, fl);
}
