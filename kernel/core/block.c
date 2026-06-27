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
#include "printf.h"
#include <stddef.h>

static struct block_device* head = NULL;

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int blk_register(struct block_device* dev) {
    if (!dev || !dev->name || !dev->read) return -1;
    if (blk_find(dev->name)) return -2;             /* duplicate name */
    dev->next = head;
    head = dev;
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
