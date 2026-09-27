/* =============================================================================
 * ramdisk.c — a block device made of RAM (§M87).
 *
 * WHY IT EXISTS: the disk manager can format, mount and unmount — and the only
 * disk most runs of this project have is the one the settings store lives on,
 * which is exactly the disk nobody should be reformatting to test a button.
 * virtio-blk drives one device and AHCI takes the first disk it finds, so a
 * second disk is not something the harness can simply attach.  A RAM disk is
 * a real block device (registered, cached, published in /dev) with nothing on
 * it that anybody wants to keep, on every arch, which makes format/mount/
 * unmount testable without risking the machine's own volume.  It is also
 * genuinely useful: a scratch volume that vanishes at power-off.
 *
 * BACKING: one 4 KiB frame per 8 sectors, reached through the kernel's direct
 * map — the block cache's own arrangement (`pmm_alloc_frame` hands out frames
 * the kernel can address on every arch, i386 highmem included).  A frame table
 * rather than one contiguous run, because a 64 MiB contiguous run is exactly
 * what a fragmented buddy allocator cannot promise.
 *
 * A RESERVE IS KEPT: creation is refused if it would leave less than a
 * quarter of RAM free.  A scratch disk that starves the machine it runs on is
 * the wrong trade for a feature whose whole point is being disposable.
 * ============================================================================= */

#include "block.h"
#include "block_cache.h"
#include "pmm.h"
#include "hal_api.h"
#include "kmalloc.h"
#include "printf.h"
#include "ramdisk.h"
#include <stdint.h>
#include <stddef.h>

#define RD_MAX      4
#define RD_SECTOR   512u
#define RD_PER_PAGE (4096u / RD_SECTOR)

struct ramdisk {
    struct block_device dev;
    char        name[8];
    pmm_phys_t* frames;
    uint32_t    nframes;
    int         used;
};

static struct ramdisk g_rd[RD_MAX];

static uint8_t* rd_sector(struct ramdisk* r, uint64_t lba) {
    uint64_t f = lba / RD_PER_PAGE;
    if (f >= r->nframes) return NULL;
    return (uint8_t*)phys_to_virt(r->frames[f]) + (lba % RD_PER_PAGE) * RD_SECTOR;
}

static int rd_read(struct block_device* d, uint64_t lba, uint32_t n, void* buf) {
    struct ramdisk* r = (struct ramdisk*)d->priv;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* s = rd_sector(r, lba + i);
        if (!s) return -1;
        for (uint32_t k = 0; k < RD_SECTOR; k++) out[i * RD_SECTOR + k] = s[k];
    }
    return 0;
}

static int rd_write(struct block_device* d, uint64_t lba, uint32_t n, const void* buf) {
    struct ramdisk* r = (struct ramdisk*)d->priv;
    const uint8_t* in = (const uint8_t*)buf;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t* s = rd_sector(r, lba + i);
        if (!s) return -1;
        for (uint32_t k = 0; k < RD_SECTOR; k++) s[k] = in[i * RD_SECTOR + k];
    }
    return 0;
}

static void rd_release(struct ramdisk* r) {
    for (uint32_t i = 0; i < r->nframes; i++)
        if (r->frames[i]) pmm_free_frame(r->frames[i]);
    kfree(r->frames);
    r->frames = NULL;
    r->nframes = 0;
    r->used = 0;
}

const char* ramdisk_create(uint32_t mib) {
    if (mib < 1 || mib > 256) return NULL;
    struct ramdisk* r = NULL;
    int idx = -1;
    for (int i = 0; i < RD_MAX; i++) if (!g_rd[i].used) { r = &g_rd[i]; idx = i; break; }
    if (!r) { kprintf("ramdisk: all %d slots in use\n", RD_MAX); return NULL; }

    uint32_t nframes = mib * 256u;                    /* 256 frames per MiB */
    uint32_t free = pmm_free_frames(), managed = pmm_managed_frames();
    if (free < nframes || free - nframes < managed / 4) {
        kprintf("ramdisk: %u MiB would leave less than a quarter of RAM free - refused\n",
                mib);
        return NULL;
    }
    r->frames = (pmm_phys_t*)kcalloc(nframes, sizeof(pmm_phys_t));
    if (!r->frames) return NULL;
    r->used = 1;
    for (uint32_t i = 0; i < nframes; i++) {
        pmm_phys_t f = pmm_alloc_frame();
        if (!f) { r->nframes = i; rd_release(r); return NULL; }
        r->frames[i] = f;
        /* ZEROED: a new disk that shows the previous owner's bytes is both a
         * leak between users and a "filesystem" the probe might recognise. */
        uint8_t* p = (uint8_t*)phys_to_virt(f);
        for (int k = 0; k < 4096; k++) p[k] = 0;
    }
    r->nframes = nframes;
    r->name[0] = 'r'; r->name[1] = 'a'; r->name[2] = 'm';
    r->name[3] = (char)('0' + idx); r->name[4] = 0;
    r->dev.name         = r->name;
    r->dev.sector_size  = RD_SECTOR;
    r->dev.sector_count = (uint64_t)nframes * RD_PER_PAGE;
    r->dev.read         = rd_read;
    r->dev.write        = rd_write;
    r->dev.flush        = NULL;
    r->dev.priv         = r;
    r->dev.next         = NULL;
    if (blk_register(&r->dev) != 0) { rd_release(r); return NULL; }
    return r->name;
}

int ramdisk_destroy(const char* name) {
    for (int i = 0; i < RD_MAX; i++) {
        struct ramdisk* r = &g_rd[i];
        if (!r->used) continue;
        const char* a = r->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a != *b) continue;
        if (blk_unregister(&r->dev) != 0) return -1;
        rd_release(r);
        kprintf("ramdisk: %s destroyed\n", name);
        return 0;
    }
    return -1;
}

int ramdisk_is(const char* name) {
    for (int i = 0; i < RD_MAX; i++) {
        if (!g_rd[i].used) continue;
        const char* a = g_rd[i].name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return 1;
    }
    return 0;
}
