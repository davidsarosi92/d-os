/* =============================================================================
 * slab.c — size-class slab allocator with per-CPU magazines (M19).
 *
 * Layout of one slab (= one 4 KiB page):
 *
 *      +-------------------+   <- page base, 4 KiB aligned
 *      |  struct slab      |       magic + cache* + next/prev + free_head
 *      +-------------------+
 *      |  slot 0           |       slot_size bytes
 *      |  slot 1           |
 *      |  ...              |
 *      |  slot capacity-1  |
 *      +-------------------+   <- (page base + 4096)
 *
 * Free slots are linked through their own storage: a free slot's first
 * 2 bytes hold the index of the next free slot, or FREE_END for the
 * tail.  This is the textbook in-place free-list trick — no per-object
 * header on the slab side.
 *
 * Cache lookup at `kfree` time: `kfree(p)` masks `p` down to the page
 * base and reads `((struct slab*)page)->magic` — if it matches
 * `SLAB_MAGIC` we trust the `cache` pointer; otherwise it's not a
 * slab object (probably a page-alloc-backed large allocation; the
 * caller handles that dispatch via a side table — see kmalloc.c).
 *
 * Per-CPU magazines:
 *   - One `struct mag` per CPU, fixed array of MAG_CAPACITY pointers.
 *   - Alloc fast path: pop from this CPU's mag under IRQ-off.
 *   - Free fast path: push to this CPU's mag under IRQ-off.
 *   - Refill / flush (slow path) touches the slab lists under the
 *     cache's spinlock and moves MAG_BATCH objects in/out at a time.
 *
 * The size-class caches are static singletons.  No public
 * `slab_cache_create` yet — every kernel allocation goes through
 * `kmalloc`'s size-class table.
 * ============================================================================= */

#include "slab.h"
#include "pmm.h"
#include "lock.h"
#include "percpu.h"
#include "printf.h"
#include "hal_api.h"
#include "acpi.h"   /* ACPI_MAX_CPUS */
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Tunables.                                                                  */
/* -------------------------------------------------------------------------- */

#define SLAB_PAGE_SIZE   4096u
#define SLAB_MAGIC       0xC0DEBABEu
#define FREE_END         0xFFFFu        /* sentinel in slab free-list */

#define MAG_CAPACITY     32             /* max objects per per-CPU magazine */
#define MAG_BATCH        16             /* refill/flush batch size */

/* -------------------------------------------------------------------------- */
/* On-page slab header.  Sized to 32 bytes so the first slot starts on
 * an 8-byte aligned offset for every object size we care about.  We
 * could pack tighter for huge objects (slot_size >> 32) but the gain
 * is negligible relative to the page itself.                                */
/* -------------------------------------------------------------------------- */
struct slab {
    uint32_t           magic;        /* SLAB_MAGIC — distinguishes from page-alloc pages */
    struct slab_cache* cache;        /* back pointer to the owning cache */
    struct slab*       next;         /* list link (intrusive doubly-linked) */
    struct slab*       prev;
    uint16_t           in_use;       /* allocated objects in this slab */
    uint16_t           capacity;     /* total slots */
    uint16_t           free_head;    /* index of first free slot, or FREE_END */
    uint16_t           _pad;
};

/* Per-CPU magazine — small fixed array of object pointers. */
struct mag {
    int   count;
    void* objs[MAG_CAPACITY];
};

/* -------------------------------------------------------------------------- */
/* The cache itself.  Public via struct slab_cache* but defined here          */
/* (callers never touch its fields).                                          */
/* -------------------------------------------------------------------------- */
struct slab_cache {
    const char*  name;
    size_t       obj_size;       /* user-requested size */
    size_t       slot_size;      /* rounded-up storage size per slot */
    uint16_t     capacity;       /* objects per slab page */
    uint16_t     payload_offset; /* byte offset of slot 0 from page base */
    struct slab* slabs_partial;  /* slabs with at least one free slot */
    struct slab* slabs_full;     /* slabs with no free slots */
    uint32_t     slabs;          /* count of pages owned (partial + full) */
    uint32_t     in_use_total;   /* sum of in_use across all slabs */
    spinlock_t   lock;
    struct mag   mag[ACPI_MAX_CPUS];
};

/* -------------------------------------------------------------------------- */
/* Built-in size-class caches.  Powers of two from 16 to 2048.                */
/*                                                                            */
/* Stored size-sorted so `slab_lookup_cache` can use a linear scan and        */
/* the first match wins.  Keep the array small — adding intermediate          */
/* sizes (e.g. 96, 192) is fine if internal fragmentation becomes a           */
/* concern.                                                                   */
/* -------------------------------------------------------------------------- */
static struct slab_cache caches[] = {
    { .name = "kmalloc-16",   .obj_size = 16   },
    { .name = "kmalloc-32",   .obj_size = 32   },
    { .name = "kmalloc-64",   .obj_size = 64   },
    { .name = "kmalloc-128",  .obj_size = 128  },
    { .name = "kmalloc-256",  .obj_size = 256  },
    { .name = "kmalloc-512",  .obj_size = 512  },
    { .name = "kmalloc-1024", .obj_size = 1024 },
    { .name = "kmalloc-2048", .obj_size = 2048 },
};
#define NR_CACHES (int)(sizeof(caches) / sizeof(caches[0]))

static int slab_initialized = 0;

/* -------------------------------------------------------------------------- */
/* Small helpers.                                                             */
/* -------------------------------------------------------------------------- */

static inline size_t align_up(size_t v, size_t a) {
    return (v + (a - 1u)) & ~(a - 1u);
}

/* All slot accesses assume 8-byte alignment for downstream callers
 * (matches the System V i386 minimum, satisfies double / int64_t). */
#define SLAB_SLOT_ALIGN 8u

static struct slab* page_of(void* p) {
    return (struct slab*)((uintptr_t)p & ~(SLAB_PAGE_SIZE - 1));
}

static void* slot_addr(struct slab* s, struct slab_cache* c, uint16_t idx) {
    return (uint8_t*)s + c->payload_offset + (size_t)idx * c->slot_size;
}

/* Given a pointer inside a slab, return its slot index. */
static uint16_t slot_idx(struct slab* s, struct slab_cache* c, void* p) {
    size_t off = (uint8_t*)p - ((uint8_t*)s + c->payload_offset);
    return (uint16_t)(off / c->slot_size);
}

/* Singly-link the free slots of a brand-new slab: slot 0 -> slot 1
 * -> ... -> slot capacity-1 -> FREE_END.  The first 2 bytes of each
 * slot store the next index. */
static void slab_init_freelist(struct slab* s, struct slab_cache* c) {
    for (uint16_t i = 0; i < c->capacity; i++) {
        uint16_t* link = (uint16_t*)slot_addr(s, c, i);
        *link = (i + 1 == c->capacity) ? FREE_END : (uint16_t)(i + 1);
    }
    s->free_head = 0;
}

/* -------------------------------------------------------------------------- */
/* Slab-list management — caller holds c->lock.                               */
/* -------------------------------------------------------------------------- */

static void list_push(struct slab** head, struct slab* s) {
    s->prev = NULL;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

static void list_remove(struct slab** head, struct slab* s) {
    if (s->prev) s->prev->next = s->next;
    else         *head         = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

/* -------------------------------------------------------------------------- */
/* Acquire a brand-new slab page from the buddy and wire its header.          */
/* Caller does NOT hold the lock — page_alloc has its own.  Returns NULL      */
/* on OOM.                                                                    */
/* -------------------------------------------------------------------------- */
static struct slab* slab_grow(struct slab_cache* c) {
    uint32_t phys = page_alloc(0, ZONE_DEFAULT);
    if (!phys) return NULL;

    struct slab* s = (struct slab*)(uintptr_t)phys;
    s->magic     = SLAB_MAGIC;
    s->cache     = c;
    s->next      = NULL;
    s->prev      = NULL;
    s->in_use    = 0;
    s->capacity  = c->capacity;
    slab_init_freelist(s, c);
    return s;
}

/* Release an empty slab back to the buddy.  Caller holds c->lock and
 * has already removed s from any list.  Wipes magic to make
 * accidental access loud. */
static void slab_release(struct slab* s) {
    s->magic = 0;
    page_free((uint32_t)(uintptr_t)s, 0);
}

/* -------------------------------------------------------------------------- */
/* Slow path: refill the current CPU's magazine from the cache pool.          */
/* Returns 0 on success, -1 on OOM.  Caller must hold IRQs off.               */
/* -------------------------------------------------------------------------- */
static int mag_refill(struct slab_cache* c) {
    struct mag* m = &c->mag[this_cpu_id()];

    uint32_t fl = spin_lock_irqsave(&c->lock);

    /* Loop pulling objects out of partial slabs until we've satisfied
     * the batch or run out of partial slabs.  Allocate fresh slabs
     * if needed. */
    int target = MAG_BATCH;
    while (target > 0) {
        struct slab* s = c->slabs_partial;
        if (!s) {
            spin_unlock_irqrestore(&c->lock, fl);

            /* Allocate a fresh slab outside the lock to keep the
             * critical section short.  Re-acquire and push it. */
            struct slab* fresh = slab_grow(c);
            fl = spin_lock_irqsave(&c->lock);
            if (!fresh) break;

            list_push(&c->slabs_partial, fresh);
            c->slabs++;
            s = fresh;
        }

        /* Pop a slot. */
        uint16_t idx = s->free_head;
        if (idx == FREE_END) {
            /* Defensive: shouldn't happen if accounting is right. */
            list_remove(&c->slabs_partial, s);
            list_push(&c->slabs_full, s);
            continue;
        }
        s->free_head = *(uint16_t*)slot_addr(s, c, idx);
        s->in_use++;
        c->in_use_total++;

        m->objs[m->count++] = slot_addr(s, c, idx);
        target--;

        if (s->free_head == FREE_END) {
            /* Slab just became full. */
            list_remove(&c->slabs_partial, s);
            list_push(&c->slabs_full, s);
        }
    }

    spin_unlock_irqrestore(&c->lock, fl);

    return (m->count > 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* Slow path: flush half the magazine back to the cache's slabs.              */
/* Called when free finds the magazine full.  Caller holds IRQs off.          */
/* -------------------------------------------------------------------------- */
static void mag_flush(struct slab_cache* c) {
    struct mag* m = &c->mag[this_cpu_id()];

    uint32_t fl = spin_lock_irqsave(&c->lock);

    int to_flush = MAG_BATCH;
    if (to_flush > m->count) to_flush = m->count;

    for (int i = 0; i < to_flush; i++) {
        void* obj = m->objs[--m->count];
        struct slab* s = page_of(obj);

        /* Defensive: object must actually belong to this cache. */
        if (s->magic != SLAB_MAGIC || s->cache != c) {
            kprintf("slab: corrupt free %p (magic=%x cache=%p expected=%p)\n",
                    obj, (unsigned)s->magic, (void*)s->cache, (void*)c);
            continue;
        }

        uint16_t idx = slot_idx(s, c, obj);
        if (idx >= s->capacity) {
            kprintf("slab: free idx %u out of range (cap=%u)\n",
                    (unsigned)idx, (unsigned)s->capacity);
            continue;
        }

        /* Push back onto slab's free list.  in_use- and list-move. */
        *(uint16_t*)slot_addr(s, c, idx) = s->free_head;
        s->free_head = idx;

        int was_full = (s->in_use == s->capacity);
        s->in_use--;
        c->in_use_total--;

        if (was_full) {
            list_remove(&c->slabs_full, s);
            list_push(&c->slabs_partial, s);
        }
        if (s->in_use == 0) {
            /* Reclaim empty slab.  Keep one cached if you want lower
             * thrash; for now release straight back to the buddy so
             * the memory is visible in pmm stats. */
            list_remove(&c->slabs_partial, s);
            c->slabs--;
            slab_release(s);
        }
    }

    spin_unlock_irqrestore(&c->lock, fl);
}

/* -------------------------------------------------------------------------- */
/* Public: alloc / free.                                                      */
/* -------------------------------------------------------------------------- */

void* slab_alloc(struct slab_cache* c) {
    if (!c) return NULL;

    uint32_t fl = hal_intr_save();
    struct mag* m = &c->mag[this_cpu_id()];

    if (m->count == 0) {
        /* Slow path — refill from the cache pool.  This briefly
         * re-enables interrupts inside the lock to keep the slow
         * path simple; the mag-local count is the only state we
         * rely on staying coherent across the call. */
        if (mag_refill(c) != 0) {
            hal_intr_restore(fl);
            return NULL;
        }
        /* mag_refill ran with IRQ-off and never crossed CPUs (no
         * preemption while IRQ-off), so `m` is still valid. */
    }

    void* obj = m->objs[--m->count];
    hal_intr_restore(fl);
    return obj;
}

void slab_free(struct slab_cache* c, void* obj) {
    if (!c || !obj) return;

    uint32_t fl = hal_intr_save();
    struct mag* m = &c->mag[this_cpu_id()];

    if (m->count >= MAG_CAPACITY) {
        /* Slow path — flush back. */
        mag_flush(c);
    }

    m->objs[m->count++] = obj;
    hal_intr_restore(fl);
}

struct slab_cache* slab_cache_of(void* obj) {
    if (!obj) return NULL;
    struct slab* s = page_of(obj);
    if (s->magic != SLAB_MAGIC) return NULL;
    return s->cache;
}

struct slab_cache* slab_lookup_cache(size_t size) {
    if (!slab_initialized) return NULL;
    for (int i = 0; i < NR_CACHES; i++) {
        if (caches[i].obj_size >= size) return &caches[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Init.                                                                      */
/* -------------------------------------------------------------------------- */

void slab_init(void) {
    if (slab_initialized) return;

    for (int i = 0; i < NR_CACHES; i++) {
        struct slab_cache* c = &caches[i];
        c->slot_size      = align_up(c->obj_size, SLAB_SLOT_ALIGN);
        c->payload_offset = (uint16_t)align_up(sizeof(struct slab), SLAB_SLOT_ALIGN);
        size_t avail = SLAB_PAGE_SIZE - c->payload_offset;
        c->capacity = (uint16_t)(avail / c->slot_size);
        spin_lock_init(&c->lock);
        c->slabs_partial = NULL;
        c->slabs_full    = NULL;
        c->slabs         = 0;
        c->in_use_total  = 0;
        for (int cpu = 0; cpu < ACPI_MAX_CPUS; cpu++) {
            c->mag[cpu].count = 0;
        }
    }

    slab_initialized = 1;
    kprintf("slab: %d caches initialized (16..2048)\n", NR_CACHES);
}

/* -------------------------------------------------------------------------- */
/* Stats.                                                                     */
/* -------------------------------------------------------------------------- */

int slab_cache_count(void) { return NR_CACHES; }

void slab_cache_get_stats(int idx, struct slab_stats* out) {
    if (idx < 0 || idx >= NR_CACHES || !out) return;
    struct slab_cache* c = &caches[idx];

    uint32_t fl = spin_lock_irqsave(&c->lock);

    uint32_t mag_total = 0;
    for (int cpu = 0; cpu < ACPI_MAX_CPUS; cpu++) {
        mag_total += c->mag[cpu].count;
    }

    uint32_t total_slots = c->slabs * c->capacity;
    out->name        = c->name;
    out->obj_size    = c->obj_size;
    out->slot_size   = c->slot_size;
    out->slabs       = c->slabs;
    out->in_use_objs = c->in_use_total;
    out->free_objs   = total_slots - c->in_use_total;
    out->mag_total   = mag_total;

    spin_unlock_irqrestore(&c->lock, fl);
}
