/* =============================================================================
 * block.h — abstract block device interface.
 *
 * A "block device" is anything that exposes a flat array of fixed-size
 * sectors with read and write at sector-LBA granularity.  Concrete
 * implementations today: virtio-blk (under QEMU).  Coming later:
 * AHCI/SATA, NVMe, USB mass storage.
 *
 * Filesystems (ext2, exFAT, ...) sit on top of this interface — they
 * never talk to a specific driver directly.  When §M11 ships with
 * virtio-blk and §M12 adds exFAT, the only coupling between them is
 * this struct.
 *
 * Concurrency: single-threaded kernel today; no locking.  When SMP
 * lands (§M18), driver implementations gain per-device locks around
 * their hardware request queues.  Filesystems and callers do not need
 * to worry about that.
 * ============================================================================= */

#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

struct block_device {
    const char* name;                       /* e.g. "vda", "sda", "nvme0n1" */
    uint32_t    sector_size;                /* almost always 512 today */
    uint64_t    sector_count;

    /* Read `count` consecutive sectors starting at `lba` into `buf`.
     * Returns 0 on success, non-zero on I/O error.  `buf` must be large
     * enough for count * sector_size bytes. */
    int (*read) (struct block_device* dev, uint64_t lba, uint32_t count, void* buf);

    /* Write `count` consecutive sectors.  Same contract as read. */
    int (*write)(struct block_device* dev, uint64_t lba, uint32_t count, const void* buf);

    /* Optional flush — push pending writes to media.  May be NULL on
     * devices that don't cache writes. */
    int (*flush)(struct block_device* dev);

    void* priv;                             /* driver-private state */
    struct block_device* next;              /* registry link */
};

/* ---------------------------------------------------------------------------
 * §M75 — ISSUE A REQUEST THROUGH HERE, NOT THROUGH `dev->read` DIRECTLY.
 *
 * The struct above is a table of function pointers and every caller in the
 * tree reached into it: the block cache, the exFAT probe, the `blk` shell
 * command, two boot self-tests.  So there was **no place a request passed
 * through**, and therefore nowhere to count one — the task manager's "I/O
 * operations" chart had no source, and neither would a future scheduler,
 * rate limiter, error counter or slow-device warning.
 *
 * *A layer whose callers all bypass it is not a layer; it is a naming
 * convention.*  These three wrappers are the layer, and the counting is the
 * first thing that needed it rather than the reason it should exist.
 *
 * They add one indirect call to a path that already crosses to hardware, and
 * they are the ONLY place `struct block_device`'s ops should be invoked.
 * Calling `dev->read` still compiles — nothing can prevent that in C — but it
 * now means "deliberately not counted".
 *
 * EXACTLY ONE CALLER IS LEFT THAT WAY, and the reason is worth keeping: the
 * aarch64 boot self-test in `main_entry.c` writes and reads a sector to prove
 * the DRIVER works.  A driver test that went through this layer would report a
 * bug in these wrappers as a bug in the driver — and it runs at boot, so its
 * two operations would also be the first entries in a machine's I/O totals
 * without any filesystem having asked for them.
 * ------------------------------------------------------------------------- */
int blk_read (struct block_device* dev, uint64_t lba, uint32_t count, void* buf);
int blk_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buf);
int blk_flush(struct block_device* dev);   /* 0 when the device has no flush op */

/* Cumulative since boot, machine-wide.  Sectors as well as operations because
 * the two answer different questions: one large request and a thousand small
 * ones move the same data at wildly different cost, and a chart of operations
 * alone cannot tell a busy disk from a badly-used one. */
struct blk_stats {
    uint64_t reads, writes, flushes;        /* operations */
    uint64_t sectors_read, sectors_written;
    uint64_t errors;                        /* ops whose driver returned != 0 */
};
void blk_get_stats(struct blk_stats* out);

/* Append a device to the registry.  Each driver picks an unused name
 * (typical convention: virtio = "vda", "vdb", ...; SATA = "sda";
 * NVMe = "nvme0n1").  Returns 0 on success. */
int  blk_register(struct block_device* dev);
/* §M87 — withdraw a device that is going away.  Not mounted is the CALLER's
 * precondition.  0 on success, -1 when it was not registered. */
int  blk_unregister(struct block_device* dev);

/* Look up by name.  Returns NULL if not registered. */
struct block_device* blk_find(const char* name);

/* Iterate every registered device.  Used by `/proc/blocks` once that
 * lands and by the `lsblk`-style shell command. */
typedef void (*blk_iter_fn)(struct block_device* dev, void* ctx);
void blk_for_each(blk_iter_fn fn, void* ctx);

/* Diagnostic — print registry to console.  Backs the `lsblk` shell
 * command. */
void blk_list(void);

/* Mount the first registered device that carries filesystem `fs` at `at`.
 *
 * WHY THIS IS NOT "MOUNT vda": the boot path named the device by hand, which
 * was correct while virtio-blk was the only block driver in the tree and became
 * wrong the moment a second one existed.  On a PHYSICAL machine there is no
 * `vda` — there is an AHCI disk registered as `sda` — so a hard-coded name
 * means the driver written to give real hardware a persistent volume registers
 * one that nothing ever mounts.  The AHCI work would have looked finished and
 * delivered nothing, which is §M64's shortcut bug in a new place: true about
 * the mechanism, false about the outcome.
 *
 * Returns 0 and writes the device's name through `out_name` (when non-NULL) on
 * success; non-zero when no registered device carries that filesystem. */
int blk_mount_first(const char* fs, const char* at, const char** out_name);

#endif
