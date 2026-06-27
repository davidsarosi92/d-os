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

/* Append a device to the registry.  Each driver picks an unused name
 * (typical convention: virtio = "vda", "vdb", ...; SATA = "sda";
 * NVMe = "nvme0n1").  Returns 0 on success. */
int  blk_register(struct block_device* dev);

/* Look up by name.  Returns NULL if not registered. */
struct block_device* blk_find(const char* name);

/* Iterate every registered device.  Used by `/proc/blocks` once that
 * lands and by the `lsblk`-style shell command. */
typedef void (*blk_iter_fn)(struct block_device* dev, void* ctx);
void blk_for_each(blk_iter_fn fn, void* ctx);

/* Diagnostic — print registry to console.  Backs the `lsblk` shell
 * command. */
void blk_list(void);

#endif
