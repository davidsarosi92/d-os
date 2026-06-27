/* =============================================================================
 * block_cache.c — refcounted, write-back, LRU buffer cache.
 *
 * See block_cache.h for the contract.  Implementation notes:
 *
 *   - The slot pool is fixed-size (`BCACHE_SLOTS`) and statically
 *     allocated to keep boot-time discovery simple.  Per-slot backing
 *     storage is one PMM frame (4 KiB) per slot — wasteful for the
 *     usual 512-byte sector case but trivially physically contiguous,
 *     which the virtio-blk DMA path needs.
 *
 *   - The LRU clock is a single monotonically-incremented counter; the
 *     victim selection walks all slots to find the lowest `lru_tick`
 *     among refcount==0 entries.  O(N) per miss; N is 64.  Will be
 *     replaced with an actual LRU list once a profile demands it.
 *
 *   - Write-back: dirty entries that need eviction are written first
 *     (synchronously via `dev->write`).  A dirty entry that fails to
 *     write is left in place — eviction picks another victim.
 *
 *   - The cache does NOT keep a per-device list; `bcache_sync` walks
 *     every slot.  Fine while there are O(1) devices.
 *
 * ============================================================================= */

#include "block_cache.h"
#include "block.h"
#include "pmm.h"
#include "printf.h"
#include <stddef.h>

#define BCACHE_SLOTS    64u
#define FRAME_SIZE      4096u           /* PMM grain — matches one frame */

static struct bcache_buf slots[BCACHE_SLOTS];
static int       initialized   = 0;
static uint64_t  lru_counter   = 0;
static struct bcache_stats stats;

/* ----------------------------------------------------------------------- */
/* Init.                                                                   */
/* ----------------------------------------------------------------------- */

int bcache_init(void) {
    if (initialized) return 0;

    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        uint32_t f = pmm_alloc_frame();
        if (!f) {
            /* Roll back what we already allocated so the system can
             * boot without a cache (the fs layer falls back to direct
             * I/O via dev->read/write). */
            for (uint32_t j = 0; j < i; j++) {
                pmm_free_frame((uint32_t)(uintptr_t)slots[j].data);
                slots[j].data = NULL;
            }
            kprintf("bcache: init failed at slot %u (pmm OOM)\n", i);
            return -1;
        }
        slots[i].dev      = NULL;
        slots[i].lba      = 0;
        slots[i].data     = (uint8_t*)(uintptr_t)f;       /* identity-mapped */
        slots[i].refcount = 0;
        slots[i].dirty    = 0;
        slots[i].lru_tick = 0;
        slots[i].valid    = 0;
    }

    stats.slots = BCACHE_SLOTS;
    initialized = 1;
    kprintf("bcache: %u slots, %u KiB total\n",
            BCACHE_SLOTS, (BCACHE_SLOTS * FRAME_SIZE) / 1024u);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Internal helpers.                                                       */
/* ----------------------------------------------------------------------- */

/* Look up an existing entry for (dev, lba).  Returns NULL on miss. */
static struct bcache_buf* find_entry(struct block_device* dev, uint64_t lba) {
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        struct bcache_buf* b = &slots[i];
        if (b->valid && b->dev == dev && b->lba == lba) return b;
    }
    return NULL;
}

/* Pick a victim slot for a new entry.  Prefers free (invalid) slots;
 * otherwise the lowest-lru, refcount==0 entry.  Dirty victims are
 * flushed first.  Returns NULL if every slot is pinned. */
static struct bcache_buf* pick_victim(void) {
    struct bcache_buf* victim = NULL;
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        struct bcache_buf* b = &slots[i];
        if (b->refcount > 0) continue;
        if (!b->valid) return b;                            /* free slot — done */
        if (!victim || b->lru_tick < victim->lru_tick) victim = b;
    }
    return victim;
}

/* ----------------------------------------------------------------------- */
/* Public API.                                                             */
/* ----------------------------------------------------------------------- */

struct bcache_buf* bcache_get(struct block_device* dev, uint64_t lba) {
    if (!initialized || !dev || !dev->read) return NULL;

    struct bcache_buf* b = find_entry(dev, lba);
    if (b) {
        stats.hits++;
        b->refcount++;
        b->lru_tick = ++lru_counter;
        return b;
    }

    stats.misses++;
    b = pick_victim();
    if (!b) return NULL;                                    /* all pinned */

    /* If the victim is dirty, write it back before repurposing. */
    if (b->valid && b->dirty && b->dev && b->dev->write) {
        if (b->dev->write(b->dev, b->lba, 1, b->data) != 0) {
            return NULL;                                    /* keep entry — try later */
        }
        stats.flushes++;
        b->dirty = 0;
    }

    if (b->valid) stats.evictions++;

    /* Bring the requested sector in. */
    if (dev->read(dev, lba, 1, b->data) != 0) {
        b->valid = 0;                                       /* leave slot empty on I/O fail */
        return NULL;
    }

    b->dev      = dev;
    b->lba      = lba;
    b->refcount = 1;
    b->dirty    = 0;
    b->valid    = 1;
    b->lru_tick = ++lru_counter;
    return b;
}

void bcache_release(struct bcache_buf* b) {
    if (!b) return;
    if (b->refcount > 0) b->refcount--;
}

void bcache_mark_dirty(struct bcache_buf* b) {
    if (!b || !b->valid) return;
    b->dirty = 1;
}

int bcache_sync(struct block_device* dev) {
    if (!initialized || !dev || !dev->write) return -1;
    int failed = 0;
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        struct bcache_buf* b = &slots[i];
        if (!b->valid || !b->dirty || b->dev != dev) continue;
        if (dev->write(dev, b->lba, 1, b->data) != 0) {
            failed++;
            continue;
        }
        b->dirty = 0;
        stats.flushes++;
    }
    if (dev->flush) dev->flush(dev);
    return failed ? -2 : 0;
}

/* ----------------------------------------------------------------------- */
/* Stats.                                                                  */
/* ----------------------------------------------------------------------- */

void bcache_get_stats(struct bcache_stats* out) {
    if (!out) return;
    /* Refresh dynamic counters (in_use, dirty) from the live pool. */
    uint32_t in_use = 0, dirty = 0;
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        if (slots[i].refcount > 0) in_use++;
        if (slots[i].valid && slots[i].dirty) dirty++;
    }
    stats.in_use = in_use;
    stats.dirty  = dirty;
    *out = stats;
}

void bcache_print_stats(void) {
    struct bcache_stats s;
    bcache_get_stats(&s);
    kprintf("bcache: %u slots, %u in-use, %u dirty\n",
            s.slots, s.in_use, s.dirty);
    kprintf("        hits=%u misses=%u evictions=%u flushes=%u\n",
            (unsigned)s.hits, (unsigned)s.misses,
            (unsigned)s.evictions, (unsigned)s.flushes);
}
