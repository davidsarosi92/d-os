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
#include "hal_api.h"   /* phys_to_virt / virt_to_phys — kernel direct map */
#include "block.h"
#include "pmm.h"
#include "printf.h"
#include "kmutex.h"
#include <stddef.h>

/* SIZED FOR A DIRECTORY, NOT FOR A FEW FILES (2026-09-25, NEXT.md #8b).
 *
 * This was 64 slots, each a whole 4 KiB frame for one 512-byte sector — 32 KiB
 * of sectors held in 256 KiB of memory.  A directory of 1060 files is 265
 * sectors, so every pass over it missed the cache on every sector, and a file
 * create makes two such passes: 1060 creates in one directory took 512 s.
 * Now a frame holds EIGHT sectors (a 512-byte slot at a 512-byte offset never
 * straddles a page, so the DMA address stays one contiguous run), 1024 slots
 * cost 128 frames = 512 KiB, and lookup goes through a hash — a linear scan of
 * 1024 slots on every access would have traded the disk reads for a loop.
 *
 * A device with sectors larger than a slot is refused, loudly, rather than
 * overrunning the slot: every device here is 512-byte today, and a 4 KiB-sector
 * disk (a future AHCI drive) is the day this needs a second slot size. */
#define BCACHE_SLOTS    1024u
#define BC_SECTOR       512u
#define FRAME_SIZE      4096u           /* PMM grain — matches one frame */
#define BC_PER_FRAME    (FRAME_SIZE / BC_SECTOR)
#define BC_BUCKETS      256u

static struct bcache_buf* buckets[BC_BUCKETS];
static inline uint32_t bc_hash(struct block_device* dev, uint64_t lba) {
    return (uint32_t)(lba ^ (lba >> 11) ^ ((uintptr_t)dev >> 4)) & (BC_BUCKETS - 1);
}
static void bc_unlink(struct bcache_buf* b) {
    struct bcache_buf** pp = &buckets[bc_hash(b->dev, b->lba)];
    while (*pp) {
        if (*pp == b) { *pp = b->hnext; b->hnext = NULL; return; }
        pp = &(*pp)->hnext;
    }
}
static void bc_link(struct bcache_buf* b) {
    uint32_t h = bc_hash(b->dev, b->lba);
    b->hnext = buckets[h];
    buckets[h] = b;
}

static struct bcache_buf slots[BCACHE_SLOTS];
static int       initialized   = 0;
static uint64_t  lru_counter   = 0;
static struct bcache_stats stats;

/* ----------------------------------------------------------------------- */
/* Init.                                                                   */
/* ----------------------------------------------------------------------- */

int bcache_init(void) {
    if (initialized) return 0;

    static pmm_phys_t frames[BCACHE_SLOTS / BC_PER_FRAME];
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        if (i % BC_PER_FRAME == 0) {
            pmm_phys_t f = pmm_alloc_frame();
            if (!f) {
                /* Roll back what we already allocated so the system can
                 * boot without a cache (the fs layer falls back to direct
                 * I/O via dev->read/write). */
                for (uint32_t j = 0; j < i / BC_PER_FRAME; j++) pmm_free_frame(frames[j]);
                for (uint32_t j = 0; j < i; j++) slots[j].data = NULL;
                kprintf("bcache: init failed at slot %u (pmm OOM)\n", i);
                return -1;
            }
            frames[i / BC_PER_FRAME] = f;
        }
        slots[i].dev      = NULL;
        slots[i].lba      = 0;
        slots[i].data     = (uint8_t*)phys_to_virt(frames[i / BC_PER_FRAME]) +
                            (i % BC_PER_FRAME) * BC_SECTOR;     /* kernel direct map */
        slots[i].hnext    = NULL;
        slots[i].refcount = 0;
        slots[i].dirty    = 0;
        slots[i].lru_tick = 0;
        slots[i].valid    = 0;
    }

    stats.slots = BCACHE_SLOTS;
    initialized = 1;
    kprintf("bcache: %u slots of %u bytes, %u KiB total\n",
            BCACHE_SLOTS, BC_SECTOR, (BCACHE_SLOTS * BC_SECTOR) / 1024u);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Internal helpers.                                                       */
/* ----------------------------------------------------------------------- */

/* Look up an existing entry for (dev, lba).  Returns NULL on miss. */
static struct bcache_buf* find_entry(struct block_device* dev, uint64_t lba) {
    for (struct bcache_buf* b = buckets[bc_hash(dev, lba)]; b; b = b->hnext)
        if (b->valid && b->dev == dev && b->lba == lba) return b;
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

/* THE CACHE'S LIST AND COUNTERS ARE SHARED BY EVERY CALLER (2026-09-25).
 * They were unlocked; a sleeping lock because a miss does disk I/O while
 * holding it.  Lock order: a filesystem's own lock, then this, then the
 * driver's.  The DATA in a buffer that has been handed out is the caller's to
 * protect (exFAT holds its volume lock for as long as it uses one). */
static struct kmutex bc_lock = KMUTEX_INIT("bcache");

static struct bcache_buf* bcache_get_unlocked(struct block_device* dev, uint64_t lba);
struct bcache_buf* bcache_get(struct block_device* dev, uint64_t lba) {
    kmutex_lock(&bc_lock);
    struct bcache_buf* b = bcache_get_unlocked(dev, lba);
    kmutex_unlock(&bc_lock);
    return b;
}

static struct bcache_buf* bcache_get_unlocked(struct block_device* dev, uint64_t lba) {
    if (!initialized || !dev || !dev->read) return NULL;
    if (dev->sector_size > BC_SECTOR) {
        static int told;
        if (!told) {
            told = 1;
            kprintf("bcache: %s has %u-byte sectors, larger than a %u-byte slot - "
                    "not cached\n", dev->name, dev->sector_size, BC_SECTOR);
        }
        return NULL;
    }

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
        if (blk_write(b->dev, b->lba, 1, b->data) != 0) {
            return NULL;                                    /* keep entry — try later */
        }
        stats.flushes++;
        b->dirty = 0;
    }

    if (b->valid) { stats.evictions++; bc_unlink(b); b->valid = 0; }

    /* Bring the requested sector in. */
    if (blk_read(dev, lba, 1, b->data) != 0) {
        b->valid = 0;                                       /* leave slot empty on I/O fail */
        return NULL;
    }

    b->dev      = dev;
    b->lba      = lba;
    b->refcount = 1;
    b->dirty    = 0;
    b->valid    = 1;
    b->lru_tick = ++lru_counter;
    bc_link(b);
    return b;
}

void bcache_release(struct bcache_buf* b) {
    if (!b) return;
    kmutex_lock(&bc_lock);
    if (b->refcount > 0) b->refcount--;
    kmutex_unlock(&bc_lock);
}

void bcache_mark_dirty(struct bcache_buf* b) {
    if (!b || !b->valid) return;
    b->dirty = 1;
}

static int bcache_sync_unlocked(struct block_device* dev);
int bcache_sync(struct block_device* dev) {
    kmutex_lock(&bc_lock);
    int rc = bcache_sync_unlocked(dev);
    kmutex_unlock(&bc_lock);
    return rc;
}

static int bcache_sync_unlocked(struct block_device* dev) {
    if (!initialized || !dev || !dev->write) return -1;
    int failed = 0;
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        struct bcache_buf* b = &slots[i];
        if (!b->valid || !b->dirty || b->dev != dev) continue;
        if (blk_write(dev, b->lba, 1, b->data) != 0) {
            failed++;
            continue;
        }
        b->dirty = 0;
        stats.flushes++;
    }
    blk_flush(dev);
    return failed ? -2 : 0;
}

/* §M87 — forget every cached sector of `dev`.  Dirty ones are written first
 * (a drop that lost a write would be silent corruption), and a slot some
 * caller still HOLDS is left alone and counted: invalidating a buffer out
 * from under its holder would hand the next reader of that LBA a stale page
 * while the holder believes it owns the only copy.  Returns the number of
 * held slots that could not be dropped (0 = the device is gone from the
 * cache). */
int bcache_invalidate(struct block_device* dev) {
    if (!initialized || !dev) return 0;
    kmutex_lock(&bc_lock);
    bcache_sync_unlocked(dev);
    int held = 0;
    for (uint32_t i = 0; i < BCACHE_SLOTS; i++) {
        struct bcache_buf* b = &slots[i];
        if (!b->valid || b->dev != dev) continue;
        if (b->refcount > 0) { held++; continue; }
        bc_unlink(b);
        b->valid = 0;
        b->dirty = 0;
        b->dev   = NULL;
    }
    kmutex_unlock(&bc_lock);
    return held;
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
