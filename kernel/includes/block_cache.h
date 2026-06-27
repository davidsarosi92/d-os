/* =============================================================================
 * block_cache.h — refcounted, write-back, LRU buffer cache.
 *
 * The cache sits between filesystems and the block layer.  Filesystems
 * call `bcache_get(dev, lba)` to obtain a `struct bcache_buf*` whose
 * `data` field holds the sector contents; subsequent reads of the same
 * LBA reuse the cached buffer without going to the device.  Writes are
 * applied to the buffer in memory and marked dirty; `bcache_sync(dev)`
 * writes dirty buffers back to the device.
 *
 * Semantics (kept narrow on purpose; M12 wants the simplest cache that
 * lets exFAT avoid pounding the disk):
 *   - Single sector per buffer.  Sized at `dev->sector_size`; a slot
 *     backs each entry with a full frame (4 KiB), pmm-allocated for
 *     guaranteed physical contiguity in case the underlying driver needs
 *     to DMA directly out of `b->data` (virtio-blk does).
 *   - Refcounting: `get` raises refcount, `release` lowers it.  An entry
 *     can only be evicted while at refcount 0 — callers must release
 *     promptly.
 *   - Write-back: `mark_dirty` records that the in-memory copy diverges
 *     from disk.  Dirty entries are NEVER silently evicted; eviction of
 *     a dirty entry triggers an implicit sync write first.  Explicit
 *     `bcache_sync(dev)` flushes every dirty entry belonging to `dev`.
 *   - LRU: a monotonic tick is recorded on each access; the eviction
 *     victim is the lowest-tick, refcount-0 entry.
 *
 * Concurrency: like everywhere else, single-threaded today; no locks.
 * The struct layout already reserves room for a per-slot mutex once
 * §M18 lands; for now the lock is implicit (the whole kernel runs
 * cooperatively).
 *
 * Future work (not in M12):
 *   - Multi-sector buffers (currently 1 sector each), to amortize device
 *     latency for sequential reads.
 *   - Per-device cache so a thrashing fs on one disk doesn't evict
 *     critical entries from another.
 *   - Read-ahead policy.
 *
 * ============================================================================= */

#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include <stdint.h>
#include <stddef.h>

struct block_device;

struct bcache_buf {
    struct block_device* dev;       /* owning device */
    uint64_t  lba;                  /* sector LBA */
    uint8_t*  data;                 /* `dev->sector_size` bytes of content */

    /* Internal — callers should not poke these directly. */
    int       refcount;             /* >0 while a caller holds it */
    int       dirty;                /* set by bcache_mark_dirty */
    uint64_t  lru_tick;             /* monotonic access timestamp */
    int       valid;                /* 0 until first read populates `data` */
};

/* One-shot init.  Allocates the slot pool and per-slot frames.  Must
 * run after pmm + kmalloc are up.  Returns 0 on success. */
int  bcache_init(void);

/* Acquire the buffer for sector `lba` on `dev`.  Brings the sector in
 * from disk on cache miss.  Returns NULL on I/O error or cache exhaustion
 * (every slot already pinned).  Caller MUST pair with `bcache_release`. */
struct bcache_buf* bcache_get(struct block_device* dev, uint64_t lba);

/* Release a buffer obtained via `bcache_get`.  Lowers the refcount; the
 * entry remains in cache (and can be evicted later) but the contents
 * survive until then. */
void bcache_release(struct bcache_buf* b);

/* Mark `b` as dirty — its contents differ from what's on disk.  The
 * write goes out either on the next `bcache_sync` or when `b` is
 * evicted to make room. */
void bcache_mark_dirty(struct bcache_buf* b);

/* Flush every dirty buffer owned by `dev` to disk.  Does NOT evict —
 * the buffers remain in cache, but their `dirty` flag is cleared on
 * success.  Returns 0 if all writes succeed, non-zero if any failed
 * (in which case the corresponding `dirty` flag is left set). */
int  bcache_sync(struct block_device* dev);

/* Diagnostic snapshot — used by `bctest` and a future /proc/blockcache. */
struct bcache_stats {
    uint32_t slots;
    uint32_t in_use;       /* refcount > 0 */
    uint32_t dirty;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t flushes;
};
void bcache_get_stats(struct bcache_stats* out);

/* Print cache stats to the console (backs the `bctest` reporting). */
void bcache_print_stats(void);

#endif
