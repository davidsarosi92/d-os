/* =============================================================================
 * block.c — block-device registry.
 *
 * Trivial linked list of `struct block_device*` rooted at `head`.
 * Drivers self-register via `blk_register` after their `init`
 * succeeds.  Filesystems (M12+) iterate / find by name.
 *
 * No locking today; SMP-readiness will add a `spinlock` around mutate
 * paths once §M18 lands the lock primitives.
 * ============================================================================= */

#include "block.h"
#include "devfs.h"
#include "printf.h"
#include <stddef.h>

static struct block_device* head = NULL;

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ----------------------------------------------------------------------
 * /dev/<name> — published HERE, by the layer every block driver registers
 * with, rather than by each driver.
 *
 * It used to be the driver's job, and only the x86 virtio-blk driver did it:
 * `/dev/vda` existed on x86 and simply did not on aarch64, whose driver
 * registers the same abstract device and never called devfs_register.  Same
 * disk, a file on one architecture and not on the other — the "one-arch-only
 * feature" shape §M63 has now paid for three times.
 *
 * The adapter needs nothing a driver knows: sector size and the read/write
 * ops are all in `struct block_device`, so there was never a reason for this
 * code to live per-driver.  The `ctx` is the device itself.
 *
 * Sector-aligned whole sectors only.  A byte-granular view would need a bounce
 * buffer and a read-modify-write for partial sectors; refused loudly rather
 * than silently rounded, because a write that lands on the wrong sector
 * boundary corrupts a filesystem in a way nothing reports until much later.
 * ---------------------------------------------------------------------- */
#define BLK_MAX_DEVFS 8
static struct devfs_node blk_nodes[BLK_MAX_DEVFS];
static int               blk_node_count;

static ssize_t blk_devfs_read(void* ctx, void* buf, size_t n, uint64_t off) {
    struct block_device* dev = (struct block_device*)ctx;
    if (!dev || !dev->read || !dev->sector_size) return -1;
    if (off % dev->sector_size) return -1;
    if (n   % dev->sector_size) return -1;
    uint32_t count = (uint32_t)(n / dev->sector_size);
    if (dev->read(dev, off / dev->sector_size, count, buf) != 0) return -1;
    return (ssize_t)n;
}

static ssize_t blk_devfs_write(void* ctx, const void* buf, size_t n, uint64_t off) {
    struct block_device* dev = (struct block_device*)ctx;
    if (!dev || !dev->write || !dev->sector_size) return -1;
    if (off % dev->sector_size) return -1;
    if (n   % dev->sector_size) return -1;
    uint32_t count = (uint32_t)(n / dev->sector_size);
    if (dev->write(dev, off / dev->sector_size, count, (void*)buf) != 0) return -1;
    return (ssize_t)n;
}

int blk_register(struct block_device* dev) {
    if (!dev || !dev->name || !dev->read) return -1;
    if (blk_find(dev->name)) return -2;             /* duplicate name */
    dev->next = head;
    head = dev;

    /* Publish it as a file.  A failure here is not fatal: the block device
     * still works for mounts and for `blk`, it simply has no /dev entry —
     * and the log says which of the two happened. */
    if (blk_node_count < BLK_MAX_DEVFS) {
        struct devfs_node* nd = &blk_nodes[blk_node_count++];
        nd->name  = dev->name;
        nd->kind  = DEVFS_BLOCK;
        nd->read  = blk_devfs_read;
        nd->write = blk_devfs_write;
        nd->ioctl = NULL;
        nd->close = NULL;
        nd->ctx   = dev;
        nd->_next = NULL;
        devfs_register(nd);
    } else {
        kprintf("blk: %s has no /dev entry (%d already published)\n",
                dev->name, BLK_MAX_DEVFS);
    }
    kprintf("blk: %s registered (%u-byte sectors, %u sectors, ~%u MiB)\n",
            dev->name,
            dev->sector_size,
            (unsigned)dev->sector_count,
            (unsigned)((dev->sector_count * (uint64_t)dev->sector_size) /
                       (1024u * 1024u)));
    return 0;
}

struct block_device* blk_find(const char* name) {
    for (struct block_device* d = head; d; d = d->next) {
        if (streq(d->name, name)) return d;
    }
    return NULL;
}

void blk_for_each(blk_iter_fn fn, void* ctx) {
    if (!fn) return;
    for (struct block_device* d = head; d; d = d->next) fn(d, ctx);
}

void blk_list(void) {
    int n = 0;
    for (struct block_device* d = head; d; d = d->next) n++;
    kprintf("block devices (%d registered):\n", n);
    for (struct block_device* d = head; d; d = d->next) {
        kprintf("  %s  %u-byte * %u sectors (~%u MiB)\n",
                d->name, d->sector_size, (unsigned)d->sector_count,
                (unsigned)((d->sector_count * (uint64_t)d->sector_size) /
                           (1024u * 1024u)));
    }
}
