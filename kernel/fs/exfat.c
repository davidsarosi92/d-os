/* =============================================================================
 * exfat.c — exFAT filesystem driver (read + minimal write).
 *
 * The first persistent fs in d-os.  Implements the parts of the exFAT
 * spec we need for the M12 definition-of-done:
 *
 *   - mount(dev) — read the boot sector, validate the EXFAT signature,
 *     cache geometry (cluster size, FAT location, root cluster), then
 *     scan the root directory to locate the allocation bitmap.
 *   - readdir + lookup — iterate 32-byte directory entries, recognize
 *     File / Stream Extension / File Name entry sets, decode the ASCII
 *     portion of UTF-16 filenames (max 30 chars / 2 name entries).
 *   - read — walk the cluster chain via the FAT (or contiguous when the
 *     stream extension's NoFatChain bit is set) and copy sector by
 *     sector through the block cache.
 *   - create — write an empty file: 1 File + 1 Stream + 1..2 Name
 *     entries.  No clusters allocated until first write.
 *   - write — extend the cluster chain on demand (allocation bitmap +
 *     FAT), copy bytes through the block cache.  Updates DataLength +
 *     ValidDataLength + FirstCluster in the stream extension and
 *     re-computes the file entry's SetChecksum.
 *   - close — flushes the block cache so contents survive a reboot.
 *
 * What we deliberately DON'T implement yet (out of scope for M12 DOD;
 * tracked under §M12 in PLAN.md):
 *
 *   - mkdir / unlink / rmdir.
 *   - File names >30 chars, or with non-ASCII characters.
 *   - Up-case table (case-insensitive lookup).  Lookups are case-
 *     sensitive even though exFAT semantics are not — fine as long as
 *     we both produced and consume the names.
 *   - Bitmap second cluster chain (TexFAT).
 *   - VolumeFlags ActiveFat / VolumeDirty management.
 *
 * On-disk references (Microsoft "exFAT File System Specification",
 * March 2019) are cited inline by `[exfat §X.Y]` next to the field
 * being parsed or written.
 *
 * Concurrency: single-threaded; no locks.  Eviction by the block cache
 * may write back dirty FAT/bitmap pages at unpredictable times — this
 * is fine because every metadata update is atomic at sector granularity
 * (we never write a partial sector).
 *
 * ============================================================================= */

#include "cred.h"      /* §M32 — CRED_UID_ROOT for a new owner record */
#include "vfs.h"
#include "block.h"
#include "block_cache.h"
#include "kmutex.h"
#include "kmalloc.h"
#include "printf.h"
#include "module.h"
#include "timer.h"
#include "exfat.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* On-disk constants.                                                     */
/* ---------------------------------------------------------------------- */

#define EXFAT_SECTOR_BOOT         0u                /* boot sector LBA */
#define EXFAT_ENTRY_SIZE          32u

/* Directory entry types.  High bit set means in-use; cleared means
 * deleted (we treat deleted entries as holes). */
#define EXFAT_TYPE_END            0x00u             /* end of directory */
#define EXFAT_TYPE_FILE           0x85u
#define EXFAT_TYPE_STREAM         0xC0u
#define EXFAT_TYPE_NAME           0xC1u
/* §M32 — a Vendor Extension directory entry (spec 7.8): a BENIGN secondary
 * entry, part of a file's entry set and covered by its SetChecksum, carrying a
 * 16-byte vendor GUID and 14 bytes the vendor defines.  Implementations that
 * do not know the GUID ignore it — which is exactly the property that lets
 * ownership live INSIDE the file's own entry set rather than in a side-car
 * map that must be kept in step with it. */
#define EXFAT_TYPE_VENDOR_EXT     0xE0u
#define EXFAT_TYPE_BITMAP         0x81u
#define EXFAT_TYPE_UPCASE         0x82u
#define EXFAT_TYPE_LABEL          0x83u
#define EXFAT_TYPE_INUSE_MASK     0x80u

/* File attribute bits (entry type 0x85, offset 4..5). */
#define EXFAT_ATTR_DIRECTORY      0x10u
#define EXFAT_ATTR_ARCHIVE        0x20u

/* Stream Extension entry, GeneralSecondaryFlags (offset 1). */
#define EXFAT_STREAM_ALLOC_POSSIBLE  0x01u
#define EXFAT_STREAM_NO_FAT_CHAIN    0x02u

/* FAT entry sentinels.  Anything >= EOC_FIRST is end of chain. */
#define EXFAT_FAT_EOC_FIRST       0xFFFFFFF8u
#define EXFAT_FAT_BAD             0xFFFFFFF7u
#define EXFAT_FAT_EOC             0xFFFFFFFFu

/* In-memory caps for this milestone.  Keeps stack frames small and
 * sidesteps multi-name-entry edge cases for now. */
#define EXFAT_MAX_NAME            30                /* ASCII chars excl. NUL */
#define EXFAT_MAX_NAME_ENTRIES    2                 /* 30 chars / 15 per entry */
/* File + Stream + names + our Vendor Extension. */
#define EXFAT_MAX_SET_ENTRIES     (1 + 1 + EXFAT_MAX_NAME_ENTRIES + 1)

/* d-os's ownership record.  The GUID is ours (random, fixed forever — it is
 * the format's identity); VendorDefined holds:
 *   [0..1] "dO"  [2] version 1  [3] reserved  [4..7] uid  [8..11] gid
 *   [12..13] mode (the rwx bits and the three special bits). */
static const uint8_t DOS_OWNER_GUID[16] = {
    0x7b, 0x3f, 0x91, 0x0c, 0x5e, 0xa2, 0x4d, 0x61,
    0x9a, 0x0e, 0xd0, 0x57, 0x4f, 0x53, 0x2d, 0x31,
};
struct owner_rec { int uid, gid; uint32_t mode; };

/* ---------------------------------------------------------------------- */
/* Per-mount + per-inode state.                                           */
/* ---------------------------------------------------------------------- */

struct exfat_fs {
    struct block_device* dev;
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  fat_offset;                /* in sectors */
    uint32_t  fat_length;                /* in sectors */
    uint32_t  cluster_heap_offset;       /* in sectors */
    uint32_t  cluster_count;             /* total clusters */
    uint32_t  root_cluster;

    /* Discovered at mount-time by scanning the root directory. */
    uint32_t  bitmap_cluster;
    uint64_t  bitmap_size;               /* bytes */

    /* ONE LOCK PER MOUNTED VOLUME (2026-09-25).  Every entry point from the
     * VFS takes it (the *_locked wrappers at the ops tables), so the FAT, the
     * allocation bitmap and the directory entry sets are only ever changed by
     * one task at a time.  Unlocked, `diskstorm` got 17 wrong read-backs in 80
     * rounds on 4 CPUs, and `fsck.exfat` found another file's cluster marked
     * FREE — the bitmap is a read-modify-write of shared sectors, and two
     * allocators interleaving is exactly how a live cluster gets handed out
     * twice.  Coarse on purpose: correctness first, and the disk under it
     * serialises requests anyway. */
    struct kmutex lock;
};

/* Per-inode private.  For directories, `dirent_*` fields are unused (the
 * directory's identity is its FirstCluster).  For regular files they
 * pin the location of the File entry in its parent so we can rewrite
 * the Stream Extension after a write. */
struct exfat_inode {
    struct exfat_fs* fs;
    uint32_t first_cluster;              /* 0 if file is empty */
    int      no_fat_chain;               /* 1 if contiguous (stream flag bit 1) */
    /* Parent dir entry location (regular files only). */
    uint32_t parent_first_cluster;       /* enclosing directory chain head */
    int      parent_no_fat_chain;
    uint64_t parent_size;                /* parent's DataLength when this was
                                            built: bounds a NoFatChain parent */
    uint32_t dirent_index;               /* 0-based index of File entry within parent */
    uint8_t  sec_count;                  /* SecondaryCount from File entry */
    int      has_owner_rec;              /* its set carries our Vendor Extension */
};

/* ---------------------------------------------------------------------- */
/* Little-endian readers — exFAT is always little-endian on disk and our */
/* CPU is little-endian too, so these are just typed loads.              */
/* ---------------------------------------------------------------------- */

static inline uint16_t le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t le64(const uint8_t* p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static inline void wle16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void wle32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void wle64(uint8_t* p, uint64_t v) {
    wle32(p, (uint32_t)v); wle32(p + 4, (uint32_t)(v >> 32));
}

/* ---------------------------------------------------------------------- */
/* String + memory helpers (no libc).                                     */
/* ---------------------------------------------------------------------- */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t strlen_(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static void memcpy_(void* dst, const void* src, size_t n) {
    char* d = (char*)dst; const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}
static void memset_(void* dst, int v, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)v;
}

/* ---------------------------------------------------------------------- */
/* Geometry helpers.                                                       */
/* ---------------------------------------------------------------------- */

/* First LBA backing cluster `c`.  Clusters are numbered from 2. */
static uint64_t cluster_first_lba(struct exfat_fs* fs, uint32_t c) {
    return (uint64_t)fs->cluster_heap_offset +
           (uint64_t)(c - 2) * fs->sectors_per_cluster;
}

/* ---------------------------------------------------------------------- */
/* FAT.                                                                    */
/* ---------------------------------------------------------------------- */

/* Read FAT entry for `cluster`.  Returns the next-cluster value (or
 * EOC sentinel on end of chain).  On I/O error returns EXFAT_FAT_EOC
 * so the caller terminates the walk gracefully. */
static uint32_t fat_next(struct exfat_fs* fs, uint32_t cluster) {
    uint64_t byte_off = (uint64_t)cluster * 4ull;
    uint64_t lba = fs->fat_offset + byte_off / fs->bytes_per_sector;
    uint32_t off = (uint32_t)(byte_off % fs->bytes_per_sector);
    struct bcache_buf* b = bcache_get(fs->dev, lba);
    if (!b) return EXFAT_FAT_EOC;
    uint32_t v = le32(b->data + off);
    bcache_release(b);
    return v;
}

/* Write FAT entry for `cluster`.  Returns 0 on success. */
static int fat_set(struct exfat_fs* fs, uint32_t cluster, uint32_t value) {
    uint64_t byte_off = (uint64_t)cluster * 4ull;
    uint64_t lba = fs->fat_offset + byte_off / fs->bytes_per_sector;
    uint32_t off = (uint32_t)(byte_off % fs->bytes_per_sector);
    struct bcache_buf* b = bcache_get(fs->dev, lba);
    if (!b) return -1;
    wle32(b->data + off, value);
    bcache_mark_dirty(b);
    bcache_release(b);
    return 0;
}

/* Step to the next cluster in a chain.  Returns EXFAT_FAT_EOC if we
 * fell off the end.  Honors the `no_fat_chain` shortcut. */
static uint32_t chain_next(struct exfat_fs* fs, uint32_t cur, int no_fat_chain) {
    if (no_fat_chain) {
        uint32_t nxt = cur + 1;
        if (nxt - 2 >= fs->cluster_count) return EXFAT_FAT_EOC;
        return nxt;
    }
    return fat_next(fs, cur);
}

/* Walk a cluster chain `n` steps from `start`.  Returns EXFAT_FAT_EOC
 * if the chain ends before reaching `n`. */
static uint32_t chain_skip(struct exfat_fs* fs, uint32_t start, uint32_t n,
                           int no_fat_chain) {
    uint32_t cur = start;
    for (uint32_t i = 0; i < n; i++) {
        cur = chain_next(fs, cur, no_fat_chain);
        if (cur >= EXFAT_FAT_EOC_FIRST) return EXFAT_FAT_EOC;
    }
    return cur;
}

/* Find the last cluster in a chain starting at `start`. */
static uint32_t chain_tail(struct exfat_fs* fs, uint32_t start) {
    uint32_t cur = start;
    for (;;) {
        uint32_t nxt = fat_next(fs, cur);
        if (nxt >= EXFAT_FAT_EOC_FIRST) return cur;
        cur = nxt;
    }
}

/* ---------------------------------------------------------------------- */
/* Allocation bitmap.                                                      */
/* ---------------------------------------------------------------------- */

/* Allocate one free cluster from the bitmap.  Marks the bit, returns
 * the cluster number (>= 2), or 0 if the volume is full. */
static uint32_t bitmap_alloc(struct exfat_fs* fs) {
    if (!fs->bitmap_cluster) return 0;
    uint64_t base_lba = cluster_first_lba(fs, fs->bitmap_cluster);
    uint64_t total_sectors = (fs->bitmap_size + fs->bytes_per_sector - 1)
                             / fs->bytes_per_sector;
    for (uint64_t s = 0; s < total_sectors; s++) {
        struct bcache_buf* b = bcache_get(fs->dev, base_lba + s);
        if (!b) return 0;
        for (uint32_t i = 0; i < fs->bytes_per_sector; i++) {
            uint64_t bit_base = (s * fs->bytes_per_sector + i) * 8ull;
            if (bit_base >= fs->cluster_count) {
                bcache_release(b);
                return 0;
            }
            if (b->data[i] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if ((b->data[i] & (1u << bit)) == 0) {
                    uint64_t cluster_no = bit_base + (uint32_t)bit;
                    if (cluster_no >= fs->cluster_count) {
                        bcache_release(b);
                        return 0;
                    }
                    b->data[i] |= (uint8_t)(1u << bit);
                    bcache_mark_dirty(b);
                    bcache_release(b);
                    return (uint32_t)(2 + cluster_no);
                }
            }
        }
        bcache_release(b);
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Cluster data access — get a sector buffer for byte `off` of cluster   */
/* chain rooted at `start`.  Returns the bcache_buf (caller releases)    */
/* and the within-sector byte offset.                                     */
/* ---------------------------------------------------------------------- */

static struct bcache_buf* cluster_chain_get_sector(struct exfat_fs* fs,
                                                   uint32_t start,
                                                   int no_fat_chain,
                                                   uint64_t off,
                                                   uint32_t* out_within_sector) {
    uint32_t cluster_no  = (uint32_t)(off / fs->bytes_per_cluster);
    uint32_t within_clu  = (uint32_t)(off % fs->bytes_per_cluster);
    uint32_t cur = chain_skip(fs, start, cluster_no, no_fat_chain);
    if (cur >= EXFAT_FAT_EOC_FIRST) return NULL;

    uint64_t lba = cluster_first_lba(fs, cur) +
                   within_clu / fs->bytes_per_sector;
    *out_within_sector = within_clu % fs->bytes_per_sector;
    return bcache_get(fs->dev, lba);
}

/* ---------------------------------------------------------------------- */
/* Directory iterator — yields one 32-byte entry at a time, walking the  */
/* cluster chain of a directory.                                          */
/* ---------------------------------------------------------------------- */

struct dir_iter {
    struct exfat_fs* fs;
    uint32_t first_cluster;
    int      no_fat_chain;
    /* The directory's DataLength in bytes; 0 = unbounded (follow the FAT
     * chain to its end — only valid for a FAT-chained directory).
     *
     * THE BOUND IS NOT OPTIONAL FOR A NoFatChain DIRECTORY (2026-09-25).  Such
     * a directory is a contiguous run, and cluster_chain_get_sector turns an
     * index into a cluster by ADDITION — nothing stops it walking off the end
     * into whatever the next cluster on the disk belongs to.  That is exactly
     * what happened: a directory this driver created owns one cluster, and
     * when it filled up, the slot search went on into the neighbouring
     * clusters and wrote directory entries over another file's data
     * (`dirfilltest`: 6143 of 16384 bytes of a neighbouring file destroyed,
     * and fsck.exfat reported the volume CLEAN, because the stray entries lie
     * outside every directory it walks). */
    uint64_t limit;
    /* Where the last access landed in the chain: cluster number `pos_n` of the
     * directory is disk cluster `pos_c` (valid when pos_c != 0).  A FAT-chained
     * lookup of entry i otherwise walks the FAT from the FIRST cluster every
     * time, which makes one pass over a directory quadratic in its length —
     * and a create makes two such passes (the duplicate check and the slot
     * search).  Measured before this: 1060 creates in one directory took
     * 512 s under emulation (NEXT.md #8b). */
    uint32_t pos_n, pos_c;
    uint32_t entry_index;                 /* next entry to fetch (0-based) */
};

/* Fetch the entry at `idx` into `out` (32 bytes).  Returns 0 on success,
 * non-zero on I/O error or chain end. */
/* The sector holding byte `off` of the directory, resuming the chain walk
 * from the iterator's last position when it lies at or before `off`. */
static struct bcache_buf* dir_iter_sector(struct dir_iter* it, uint64_t off,
                                          uint32_t* within) {
    struct exfat_fs* fs = it->fs;
    uint32_t n = (uint32_t)(off / fs->bytes_per_cluster);
    uint32_t cur;
    if (it->no_fat_chain) {
        cur = it->first_cluster + n;
    } else {
        uint32_t from_n = 0, from_c = it->first_cluster;
        if (it->pos_c && it->pos_n <= n) { from_n = it->pos_n; from_c = it->pos_c; }
        cur = chain_skip(fs, from_c, n - from_n, 0);
        if (cur >= EXFAT_FAT_EOC_FIRST) return NULL;
        it->pos_n = n;
        it->pos_c = cur;
    }
    uint32_t within_clu = (uint32_t)(off % fs->bytes_per_cluster);
    uint64_t lba = cluster_first_lba(fs, cur) + within_clu / fs->bytes_per_sector;
    *within = within_clu % fs->bytes_per_sector;
    return bcache_get(fs->dev, lba);
}

static int dir_entry_read(struct dir_iter* it, uint32_t idx, uint8_t* out) {
    uint64_t off = (uint64_t)idx * EXFAT_ENTRY_SIZE;
    if (it->limit && off >= it->limit) return -1;         /* past the directory */
    uint32_t within;
    struct bcache_buf* b = dir_iter_sector(it, off, &within);
    if (!b) return -1;
    memcpy_(out, b->data + within, EXFAT_ENTRY_SIZE);
    bcache_release(b);
    return 0;
}

/* Write a 32-byte entry at `idx`.  Marks the buffer dirty. */
static int dir_entry_write(struct dir_iter* it, uint32_t idx, const uint8_t* in) {
    uint64_t off = (uint64_t)idx * EXFAT_ENTRY_SIZE;
    if (it->limit && off >= it->limit) return -1;         /* past the directory */
    uint32_t within;
    struct bcache_buf* b = dir_iter_sector(it, off, &within);
    if (!b) return -1;
    memcpy_(b->data + within, in, EXFAT_ENTRY_SIZE);
    bcache_mark_dirty(b);
    bcache_release(b);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* exFAT name hash + set checksum.                                        */
/* See exFAT spec §6.3.3 and §6.3.7.                                      */
/* ---------------------------------------------------------------------- */

/* Up-case an ASCII character per the trivial Latin rule.  Real exFAT
 * uses the volume's up-case table; for the ASCII-only files we produce
 * this is interchangeable, and Linux's exfatprogs/exfat-fuse accept it
 * because they also up-case via their own table when verifying. */
static uint16_t ascii_upcase(uint16_t ch) {
    if (ch >= 'a' && ch <= 'z') return (uint16_t)(ch - 32);
    return ch;
}

/* Compute the 16-bit name hash over an ASCII name. */
static uint16_t name_hash_ascii(const char* name, int name_len) {
    uint16_t h = 0;
    for (int i = 0; i < name_len; i++) {
        uint16_t ch = ascii_upcase((uint16_t)(uint8_t)name[i]);
        /* Spec: hash over each byte of the UTF-16 encoding (low, high). */
        h = (uint16_t)(((h << 15) | (h >> 1)) + (ch & 0xff));
        h = (uint16_t)(((h << 15) | (h >> 1)) + ((ch >> 8) & 0xff));
    }
    return h;
}

/* Compute SetChecksum over a contiguous run of (sec_count + 1) entries.
 * Skips byte offsets 2 and 3 of the very first entry (the checksum
 * field itself). */
static uint16_t set_checksum(const uint8_t* entries, int total_bytes) {
    uint16_t cs = 0;
    for (int i = 0; i < total_bytes; i++) {
        if (i == 2 || i == 3) continue;
        cs = (uint16_t)(((cs << 15) | (cs >> 1)) + entries[i]);
    }
    return cs;
}

/* ---------------------------------------------------------------------- */
/* Filename decode (UTF-16 LE → ASCII).  Returns 0 if any code unit has  */
/* the high byte set (we can't represent it in our 8-bit name).          */
/* ---------------------------------------------------------------------- */

static int decode_name(const uint8_t* name_entries[EXFAT_MAX_NAME_ENTRIES],
                       int num_entries, int name_length, char* out_buf,
                       int out_cap) {
    int produced = 0;
    for (int i = 0; i < num_entries && produced < name_length; i++) {
        const uint8_t* e = name_entries[i];
        for (int k = 0; k < 15 && produced < name_length; k++) {
            uint16_t ch = le16(e + 2 + k * 2);
            if (ch & 0xFF00) return -1;            /* non-ASCII not supported */
            if (produced + 1 >= out_cap) return -1;
            out_buf[produced++] = (char)(ch & 0xFF);
        }
    }
    out_buf[produced] = 0;
    return produced;
}

/* ---------------------------------------------------------------------- */
/* Forward decls of file_ops / inode_ops tables.                          */
/* ---------------------------------------------------------------------- */

static const struct file_ops  exfat_file_ops;
static const struct file_ops  exfat_dir_ops;
static const struct inode_ops exfat_inode_ops_dir;

/* Construct an inode from a parsed (file + stream) entry pair.  `ext`
 * carries the decoded fields. */
struct parsed_file {
    uint16_t attrs;
    uint8_t  sec_count;
    uint8_t  stream_flags;
    uint8_t  name_length;
    uint32_t first_cluster;
    uint64_t data_length;
    uint32_t dirent_index;
    int      has_owner;                  /* a d-os Vendor Extension was found */
    struct owner_rec own;
};

static struct inode* build_inode(struct exfat_fs* fs,
                                 const struct parsed_file* pf,
                                 uint32_t parent_first_cluster,
                                 int parent_no_fat_chain,
                                 uint64_t parent_size) {
    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    struct exfat_inode* ei = (struct exfat_inode*)kcalloc(1, sizeof(*ei));
    if (!ino || !ei) {
        if (ino) kfree(ino);
        if (ei)  kfree(ei);
        return NULL;
    }
    ei->fs                     = fs;
    ei->first_cluster          = pf->first_cluster;
    ei->no_fat_chain           = (pf->stream_flags & EXFAT_STREAM_NO_FAT_CHAIN) ? 1 : 0;
    ei->parent_first_cluster   = parent_first_cluster;
    ei->parent_no_fat_chain    = parent_no_fat_chain;
    ei->parent_size            = parent_size;
    ei->dirent_index           = pf->dirent_index;
    ei->sec_count              = pf->sec_count;
    ei->has_owner_rec          = pf->has_owner;

    ino->size    = pf->data_length;
    ino->private = ei;
    if (pf->attrs & EXFAT_ATTR_DIRECTORY) {
        ino->type    = INODE_DIR;
        ino->ops     = &exfat_dir_ops;
        ino->dir_ops = &exfat_inode_ops_dir;
    } else {
        ino->type    = INODE_FILE;
        ino->ops     = &exfat_file_ops;
        ino->dir_ops = NULL;
    }
    /* §M32 — defaults first (a file written by another system has no owner
     * record, and root:root 0644/0755 is what it gets), then the owner record
     * from the file's own entry set when there is one. */
    vfs_inode_defaults(ino);
    if (pf->has_owner) {
        ino->owner_uid = pf->own.uid;
        ino->owner_gid = pf->own.gid;
        ino->mode      = pf->own.mode & 07777u;
    }
    return ino;
}

/* ---------------------------------------------------------------------- */
/* Generic directory scanner.                                              */
/*                                                                         */
/* Walks `(parent_first_cluster, no_fat_chain)`'s entry stream.  For each */
/* File entry set, decodes the name and invokes `visit(name, pf, ctx)`.   */
/* `visit` returns non-zero to stop iteration; the scanner returns that   */
/* value.  Returns 0 if the directory ends without a match.               */
/* ---------------------------------------------------------------------- */

typedef int (*dir_visit_fn)(const char* name, const struct parsed_file* pf,
                            void* ctx);

/* Is this entry OUR owner record?  Fills `out` if so. */
static int owner_rec_parse(const uint8_t* e, struct owner_rec* out) {
    if (e[0] != EXFAT_TYPE_VENDOR_EXT) return 0;
    for (int i = 0; i < 16; i++) if (e[2 + i] != DOS_OWNER_GUID[i]) return 0;
    const uint8_t* v = e + 18;
    if (v[0] != 'd' || v[1] != 'O' || v[2] != 1) return 0;
    out->uid  = (int)le32(v + 4);
    out->gid  = (int)le32(v + 8);
    out->mode = le16(v + 12);
    return 1;
}
static void owner_rec_build(uint8_t* e, const struct owner_rec* r) {
    memset_(e, 0, EXFAT_ENTRY_SIZE);
    e[0] = EXFAT_TYPE_VENDOR_EXT;
    e[1] = 0;                            /* GeneralSecondaryFlags: no allocation */
    for (int i = 0; i < 16; i++) e[2 + i] = DOS_OWNER_GUID[i];
    uint8_t* v = e + 18;
    v[0] = 'd'; v[1] = 'O'; v[2] = 1; v[3] = 0;
    wle32(v + 4, (uint32_t)r->uid);
    wle32(v + 8, (uint32_t)r->gid);
    wle16(v + 12, (uint16_t)(r->mode & 07777u));
}

static int scan_directory(struct exfat_fs* fs, uint32_t parent_first_cluster,
                          int parent_no_fat_chain, uint64_t limit,
                          dir_visit_fn visit, void* ctx) {
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = parent_first_cluster,
        .no_fat_chain  = parent_no_fat_chain,
        .limit         = limit,
    };

    uint32_t idx = 0;
    uint8_t entry[EXFAT_ENTRY_SIZE];
    for (;;) {
        if (dir_entry_read(&it, idx, entry) != 0) return 0;
        uint8_t type = entry[0];

        if (type == EXFAT_TYPE_END) return 0;
        if ((type & EXFAT_TYPE_INUSE_MASK) == 0) { idx++; continue; }   /* deleted */
        if (type != EXFAT_TYPE_FILE) { idx++; continue; }               /* bitmap/upcase/label */

        struct parsed_file pf = { 0 };
        pf.dirent_index = idx;
        pf.attrs        = le16(entry + 4);
        pf.sec_count    = entry[1];
        if (pf.sec_count < 2) { idx++; continue; }                       /* malformed */

        /* Read stream extension. */
        uint8_t stream[EXFAT_ENTRY_SIZE];
        if (dir_entry_read(&it, idx + 1, stream) != 0) return 0;
        if (stream[0] != EXFAT_TYPE_STREAM) { idx += 1 + pf.sec_count; continue; }
        pf.stream_flags  = stream[1];
        pf.name_length   = stream[3];
        pf.first_cluster = le32(stream + 0x14);
        pf.data_length   = le64(stream + 0x18);

        /* Read up to EXFAT_MAX_NAME_ENTRIES name entries. */
        const uint8_t* name_ptrs[EXFAT_MAX_NAME_ENTRIES];
        uint8_t name_bufs[EXFAT_MAX_NAME_ENTRIES][EXFAT_ENTRY_SIZE];
        int ne = 0;
        for (int j = 2; j <= pf.sec_count && ne < EXFAT_MAX_NAME_ENTRIES; j++) {
            if (dir_entry_read(&it, idx + j, name_bufs[ne]) != 0) return 0;
            if (name_bufs[ne][0] != EXFAT_TYPE_NAME) break;
            name_ptrs[ne] = name_bufs[ne];
            ne++;
        }
        if (ne == 0) { idx += 1 + pf.sec_count; continue; }
        /* §M32 — the rest of the set: our owner record, if any. */
        for (int j = 2 + ne; j <= pf.sec_count; j++) {
            uint8_t ve[EXFAT_ENTRY_SIZE];
            if (dir_entry_read(&it, idx + j, ve) != 0) break;
            if (owner_rec_parse(ve, &pf.own)) { pf.has_owner = 1; break; }
        }

        char nbuf[EXFAT_MAX_NAME + 1];
        if (decode_name(name_ptrs, ne, pf.name_length, nbuf, sizeof nbuf) <= 0) {
            /* Skip names we can't decode (too long / non-ASCII). */
            idx += 1 + pf.sec_count;
            continue;
        }

        int r = visit(nbuf, &pf, ctx);
        if (r) return r;
        idx += 1 + pf.sec_count;
    }
}

/* ---------------------------------------------------------------------- */
/* dir_ops — lookup.                                                       */
/* ---------------------------------------------------------------------- */

struct lookup_ctx {
    const char* target;
    struct exfat_fs* fs;
    uint32_t parent_first_cluster;
    int parent_no_fat_chain;
    uint64_t parent_size;
    struct inode* found;
};

static int lookup_visit(const char* name, const struct parsed_file* pf, void* ctx_) {
    struct lookup_ctx* ctx = (struct lookup_ctx*)ctx_;
    if (!streq_(name, ctx->target)) return 0;
    ctx->found = build_inode(ctx->fs, pf, ctx->parent_first_cluster,
                             ctx->parent_no_fat_chain, ctx->parent_size);
    return 1;
}

static int exfat_lookup(struct inode* dir, const char* name, struct inode** out) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct lookup_ctx ctx = {
        .target = name,
        .fs = dei->fs,
        .parent_first_cluster = dei->first_cluster,
        .parent_no_fat_chain = dei->no_fat_chain,
        .parent_size = dir->size,
        .found = NULL,
    };
    scan_directory(dei->fs, dei->first_cluster, dei->no_fat_chain, dir->size,
                   lookup_visit, &ctx);
    if (!ctx.found) return -1;
    *out = ctx.found;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* file_ops — read.                                                        */
/* ---------------------------------------------------------------------- */

static ssize_t exfat_read(struct file* f, void* buf, size_t n, uint64_t off) {
    struct exfat_inode* ei = (struct exfat_inode*)f->inode->private;
    struct exfat_fs* fs = ei->fs;
    if (off >= f->inode->size) return 0;
    uint64_t avail = f->inode->size - off;
    if ((uint64_t)n > avail) n = (size_t)avail;
    if (n == 0 || ei->first_cluster < 2) return 0;

    uint8_t* dst = (uint8_t*)buf;
    size_t   total = 0;
    while (n > 0) {
        uint32_t within;
        struct bcache_buf* b = cluster_chain_get_sector(fs, ei->first_cluster,
                                                        ei->no_fat_chain,
                                                        off, &within);
        if (!b) return total ? (ssize_t)total : -1;
        uint32_t chunk = fs->bytes_per_sector - within;
        if (chunk > n) chunk = (uint32_t)n;
        memcpy_(dst + total, b->data + within, chunk);
        bcache_release(b);
        total += chunk;
        off   += chunk;
        n     -= chunk;
    }
    return (ssize_t)total;
}

/* ---------------------------------------------------------------------- */
/* file_ops (directory) — readdir.                                         */
/*                                                                         */
/* Uses `f->pos` as a 0-based child index, exactly like ramfs.  Each call */
/* walks from the start (cheap: rescan to the Nth visible entry).  Less   */
/* efficient than tracking an entry-stream cursor, but it keeps state in  */
/* the simple integer the VFS layer already manages.                       */
/* ---------------------------------------------------------------------- */

struct readdir_ctx {
    uint64_t want_index;
    uint64_t cur_index;
    struct dirent* out;
    int found;
};

static int readdir_visit(const char* name, const struct parsed_file* pf, void* ctx_) {
    struct readdir_ctx* ctx = (struct readdir_ctx*)ctx_;
    if (ctx->cur_index == ctx->want_index) {
        size_t i = 0;
        while (i < sizeof(ctx->out->name) - 1 && name[i]) {
            ctx->out->name[i] = name[i]; i++;
        }
        ctx->out->name[i] = 0;
        ctx->out->type = (pf->attrs & EXFAT_ATTR_DIRECTORY) ? INODE_DIR : INODE_FILE;
        ctx->out->size = pf->data_length;
        ctx->found = 1;
        return 1;
    }
    ctx->cur_index++;
    return 0;
}

static int exfat_readdir(struct file* f, struct dirent* out) {
    struct exfat_inode* ei = (struct exfat_inode*)f->inode->private;
    struct readdir_ctx ctx = {
        .want_index = f->pos,
        .cur_index  = 0,
        .out        = out,
        .found      = 0,
    };
    scan_directory(ei->fs, ei->first_cluster, ei->no_fat_chain, f->inode->size,
                   readdir_visit, &ctx);
    if (!ctx.found) return 0;
    f->pos++;
    return 1;
}

/* ---------------------------------------------------------------------- */
/* Dir entry rewrite — used by `write` to update DataLength /             */
/* ValidDataLength / FirstCluster + recompute SetChecksum.                */
/* ---------------------------------------------------------------------- */

/* Re-fetch the file's entry-set from the parent directory, patch the
 * stream extension's mutable fields (AllocPossible / NoFatChain flag,
 * FirstCluster, ValidDataLength, DataLength) to match the current
 * in-memory state, recompute the SetChecksum on the file entry, and
 * write everything back.  Called after each `write` so a reboot picks
 * up the file's current size and cluster head. */
static int exfat_write_meta(struct inode* fi) {
    struct exfat_inode* ei = (struct exfat_inode*)fi->private;
    if (ei->parent_first_cluster < 2) return 0;

    struct dir_iter it = {
        .fs            = ei->fs,
        .first_cluster = ei->parent_first_cluster,
        .no_fat_chain  = ei->parent_no_fat_chain,
        .limit         = ei->parent_size,
    };

    int total = 1 + ei->sec_count;
    if (total > EXFAT_MAX_SET_ENTRIES) return -1;

    uint8_t buf[EXFAT_MAX_SET_ENTRIES * EXFAT_ENTRY_SIZE];
    for (int i = 0; i < total; i++) {
        if (dir_entry_read(&it, ei->dirent_index + i, buf + i * EXFAT_ENTRY_SIZE) != 0)
            return -1;
    }

    uint8_t* file_e   = buf + 0 * EXFAT_ENTRY_SIZE;
    uint8_t* stream_e = buf + 1 * EXFAT_ENTRY_SIZE;

    /* Update stream extension: alloc-possible flag, FirstCluster, sizes. */
    if (ei->first_cluster >= 2) {
        stream_e[1] |= EXFAT_STREAM_ALLOC_POSSIBLE;
    } else {
        stream_e[1] &= (uint8_t)~EXFAT_STREAM_ALLOC_POSSIBLE;
    }
    /* NoFatChain flag we manage ourselves (created files are always
     * FAT-chained; we never lay down contiguous-allocation hints). */
    stream_e[1] &= (uint8_t)~EXFAT_STREAM_NO_FAT_CHAIN;
    if (ei->no_fat_chain) stream_e[1] |= EXFAT_STREAM_NO_FAT_CHAIN;

    wle64(stream_e + 0x08, fi->size);              /* ValidDataLength */
    wle32(stream_e + 0x14, ei->first_cluster);     /* FirstCluster */
    wle64(stream_e + 0x18, fi->size);              /* DataLength */

    /* Recompute SetChecksum over the whole set, skipping the field. */
    uint16_t cs = set_checksum(buf, total * EXFAT_ENTRY_SIZE);
    wle16(file_e + 0x02, cs);

    /* Write entries back. */
    for (int i = 0; i < total; i++) {
        if (dir_entry_write(&it, ei->dirent_index + i,
                            buf + i * EXFAT_ENTRY_SIZE) != 0) return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* file_ops — write.                                                       */
/* ---------------------------------------------------------------------- */

static ssize_t exfat_write(struct file* f, const void* buf, size_t n,
                           uint64_t off) {
    struct exfat_inode* ei = (struct exfat_inode*)f->inode->private;
    struct exfat_fs* fs = ei->fs;
    if (n == 0) return 0;

    uint64_t end = off + n;
    /* Grow the cluster chain so it covers `end` bytes. */
    uint64_t clusters_have;
    if (ei->first_cluster < 2) {
        clusters_have = 0;
    } else {
        clusters_have = 1;
        if (!ei->no_fat_chain) {
            /* count via FAT walk */
            uint32_t cur = ei->first_cluster;
            for (;;) {
                uint32_t nxt = fat_next(fs, cur);
                if (nxt >= EXFAT_FAT_EOC_FIRST) break;
                cur = nxt;
                clusters_have++;
            }
        }
    }
    uint64_t clusters_need = (end + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster;
    if (clusters_need > clusters_have) {
        uint64_t to_add = clusters_need - clusters_have;
        uint32_t prev_tail = (ei->first_cluster < 2) ? 0
                                                     : chain_tail(fs, ei->first_cluster);
        for (uint64_t i = 0; i < to_add; i++) {
            uint32_t c = bitmap_alloc(fs);
            if (!c) return -1;          /* out of space; nothing written yet */
            fat_set(fs, c, EXFAT_FAT_EOC);
            if (prev_tail) {
                fat_set(fs, prev_tail, c);
            } else {
                ei->first_cluster = c;
                ei->no_fat_chain  = 0;
            }
            prev_tail = c;
        }
    }

    /* Copy bytes through bcache, sector by sector. */
    const uint8_t* src = (const uint8_t*)buf;
    size_t total = 0;
    while (n > 0) {
        uint32_t within;
        struct bcache_buf* b = cluster_chain_get_sector(fs, ei->first_cluster,
                                                        ei->no_fat_chain,
                                                        off, &within);
        if (!b) return total ? (ssize_t)total : -1;
        uint32_t chunk = fs->bytes_per_sector - within;
        if (chunk > n) chunk = (uint32_t)n;
        memcpy_(b->data + within, src + total, chunk);
        bcache_mark_dirty(b);
        bcache_release(b);
        total += chunk;
        off   += chunk;
        n     -= chunk;
    }

    /* Extend size + sync dir entry. */
    if (end > f->inode->size) f->inode->size = end;
    exfat_write_meta(f->inode);
    return (ssize_t)total;
}

/* ---------------------------------------------------------------------- */
/* file_ops.close — push dirty bcache pages to disk so a reboot sees     */
/* what we wrote.                                                         */
/* ---------------------------------------------------------------------- */

static int exfat_close(struct file* f) {
    struct exfat_inode* ei = (struct exfat_inode*)f->inode->private;
    if (ei && ei->fs) bcache_sync(ei->fs->dev);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* inode_ops — create.                                                    */
/* ---------------------------------------------------------------------- */

/* Search the parent directory's entry stream for a contiguous run of
 * `need` deleted-or-end entries.  Returns the starting index or
 * (uint32_t)-1 if we ran past the chain (caller should grow the dir). */
static uint32_t find_free_dir_slot(struct exfat_fs* fs,
                                   uint32_t parent_first_cluster,
                                   int parent_no_fat_chain, uint64_t limit,
                                   int need) {
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = parent_first_cluster,
        .no_fat_chain  = parent_no_fat_chain,
        .limit         = limit,
    };
    uint32_t run_start = 0;
    int run_len = 0;
    uint32_t idx = 0;
    for (;;) {
        uint8_t entry[EXFAT_ENTRY_SIZE];
        if (dir_entry_read(&it, idx, entry) != 0) return (uint32_t)-1;
        if (entry[0] == EXFAT_TYPE_END || (entry[0] & EXFAT_TYPE_INUSE_MASK) == 0) {
            if (run_len == 0) run_start = idx;
            run_len++;
            if (run_len >= need) return run_start;
        } else {
            run_len = 0;
        }
        idx++;
        /* The end of the directory is where dir_entry_read fails (its
         * DataLength, or the FAT chain's end).  This used to be "four
         * clusters", a guess that for a one-cluster NoFatChain directory
         * meant three clusters of SOMEBODY ELSE'S data (see struct dir_iter).
         * When nothing fits, the caller grows the directory (dir_grow). */
    }
}

/* ---------------------------------------------------------------------- */
/* §M12 completion — mkdir / rmdir / unlink.                              */
/*                                                                        */
/* These were `NULL` with the comment "not in M12 DOD" since the FS        */
/* landed, and everything they need already existed for files: the         */
/* bitmap allocator, the FAT chain writer, the directory-slot finder, the  */
/* entry-set checksum and the name hash.  What was missing was the small   */
/* amount that makes a DIRECTORY different from a file:                    */
/*                                                                        */
/*   - it owns a cluster from the moment it exists (a file may have none), */
/*     and that cluster must be ZEROED, because an all-zero entry is what  */
/*     exFAT reads as "end of directory";                                  */
/*   - its Stream Extension carries AllocationPossible + NoFatChain and a  */
/*     DataLength of one cluster, where an empty file carries zeros;       */
/*   - the File entry's attribute is DIRECTORY, not ARCHIVE.               */
/*                                                                        */
/* Deletion is the same in both directions: mark every entry of the set    */
/* "not in use" (clear bit 7 of the type byte — the entry is preserved,    */
/* which is what makes exFAT undelete possible and what fsck expects),     */
/* then release the cluster chain to the bitmap.  A directory is refused   */
/* while it still has entries, because a recursive delete is a POLICY and  */
/* belongs one layer up (vfs_unlink_recursive already implements it).      */
/* ---------------------------------------------------------------------- */

/* Zero every sector of `cluster`.  An empty directory IS a zeroed cluster. */
static int cluster_zero(struct exfat_fs* fs, uint32_t cluster) {
    uint64_t lba = cluster_first_lba(fs, cluster);
    uint32_t secs = fs->bytes_per_cluster / fs->bytes_per_sector;
    for (uint32_t i = 0; i < secs; i++) {
        struct bcache_buf* b = bcache_get(fs->dev, lba + i);
        if (!b) return -1;
        memset_(b->data, 0, fs->bytes_per_sector);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    return 0;
}

/* Clear a cluster's bit in the allocation bitmap. */
static void bitmap_free(struct exfat_fs* fs, uint32_t cluster) {
    if (!fs->bitmap_cluster || cluster < 2) return;
    uint64_t idx = (uint64_t)cluster - 2;
    if (idx >= fs->cluster_count) return;
    uint64_t byte = idx / 8, bit = idx % 8;
    uint64_t lba = cluster_first_lba(fs, fs->bitmap_cluster)
                 + byte / fs->bytes_per_sector;
    struct bcache_buf* b = bcache_get(fs->dev, lba);
    if (!b) return;
    b->data[byte % fs->bytes_per_sector] &= (uint8_t)~(1u << bit);
    bcache_mark_dirty(b);
    bcache_release(b);
}

/* Release a whole chain.  Walks the FAT when the chain is fat-linked; a
 * NoFatChain run is contiguous, so its length comes from the size. */
static void chain_free(struct exfat_fs* fs, uint32_t start, int no_fat_chain,
                       uint64_t data_length) {
    if (start < 2) return;
    if (no_fat_chain) {
        uint64_t n = (data_length + fs->bytes_per_cluster - 1) / fs->bytes_per_cluster;
        if (n == 0) n = 1;
        for (uint64_t i = 0; i < n; i++) bitmap_free(fs, (uint32_t)(start + i));
        return;
    }
    uint32_t cur = start;
    for (int guard = 0; guard < 1 << 20 && cur >= 2; guard++) {
        uint32_t nxt = fat_next(fs, cur);
        bitmap_free(fs, cur);
        fat_set(fs, cur, 0);
        if (nxt >= EXFAT_FAT_EOC_FIRST) break;
        cur = nxt;
    }
}

/* Mark the whole entry set at `slot` as not-in-use. */
static int dirent_set_delete(struct exfat_fs* fs, uint32_t dir_cluster,
                             int dir_nofat, uint64_t limit, uint32_t slot) {
    struct dir_iter it = { .fs = fs, .first_cluster = dir_cluster,
                           .no_fat_chain = dir_nofat, .limit = limit };
    uint8_t e[EXFAT_ENTRY_SIZE];
    if (dir_entry_read(&it, slot, e) != 0) return -1;
    if (e[0] != EXFAT_TYPE_FILE) return -1;
    int sec = e[1];
    for (int i = 0; i <= sec; i++) {
        if (dir_entry_read(&it, slot + i, e) != 0) return -1;
        e[0] &= (uint8_t)0x7F;          /* "not in use", entry preserved */
        if (dir_entry_write(&it, slot + i, e) != 0) return -1;
    }
    return 0;
}

/* Is this directory empty?  Any in-use File entry means no. */
/* Is the directory empty?  Answers 1 only when it has PROVED it.
 *
 * NOT A SAMPLE (2026-09-25, NEXT.md #8).  This used to look at the first 4096
 * entries and then answer "empty" — so a directory whose first 4096 slots were
 * DELETED entries (exFAT marks a removed set by clearing bit 7, it does not
 * compact) and whose live files sat behind them was reported empty, and
 * `rm` removed it: the live files' clusters stayed allocated and nothing
 * pointed at them any more.  Now the scan covers the directory's whole
 * DataLength, and every way of not finishing — a read failure before the end,
 * a length past exFAT's own 256 MiB directory limit — answers "not empty".
 * Refusing an rmdir is recoverable; orphaning a file is not. */
#define EXFAT_DIR_MAX_ENTRIES (256u * 1024u * 1024u / EXFAT_ENTRY_SIZE)
static int dir_is_empty(struct exfat_fs* fs, uint32_t cluster, int no_fat,
                        uint64_t size) {
    struct dir_iter it = { .fs = fs, .first_cluster = cluster,
                           .no_fat_chain = no_fat, .limit = size };
    uint8_t e[EXFAT_ENTRY_SIZE];
    uint64_t n = size / EXFAT_ENTRY_SIZE;
    if (n > EXFAT_DIR_MAX_ENTRIES) return 0;        /* corrupt length: refuse */
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        if (dir_entry_read(&it, i, e) != 0) return 0;   /* could not look */
        if (e[0] == 0x00) return 1;                 /* end of directory */
        if (e[0] == EXFAT_TYPE_FILE) return 0;      /* an in-use file/dir */
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * Directory growth (2026-09-25).
 *
 * A directory this driver creates starts as ONE cluster marked NoFatChain.
 * When no run of free slots is left, it gets another, ZEROED cluster (an
 * all-zero entry is "end of directory", so an unzeroed one would be a
 * directory full of whatever the disk held before).  NoFatChain describes a
 * contiguous run, and the new cluster need not be contiguous, so the first
 * growth converts the directory to a FAT chain: the existing run is written
 * into the FAT link by link (so its old entries resolve exactly as before),
 * then the new cluster is appended.  Finally the directory's own Stream
 * Extension in ITS parent gets the new DataLength and flags — exfat_write_meta
 * is that update for a file, and a directory is recorded the same way.  The
 * root directory has no parent entry; it is always FAT-chained and its size is
 * its chain, so appending is all it needs.
 *
 * Before deciding anything, the directory's in-memory view is refreshed from
 * its on-disk entry: two inodes for one directory would otherwise each believe
 * the directory still contiguous, and the second conversion would rewrite FAT
 * links the first had already pointed elsewhere.
 * ---------------------------------------------------------------------- */
static void dir_refresh(struct inode* dir) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    if (!dei || dei->parent_first_cluster < 2) return;      /* root */
    struct dir_iter it = { .fs = dei->fs, .first_cluster = dei->parent_first_cluster,
                           .no_fat_chain = dei->parent_no_fat_chain,
                           .limit = dei->parent_size };
    uint8_t st[EXFAT_ENTRY_SIZE];
    if (dir_entry_read(&it, dei->dirent_index + 1, st) != 0) return;
    if (st[0] != EXFAT_TYPE_STREAM) return;
    dei->no_fat_chain = (st[1] & EXFAT_STREAM_NO_FAT_CHAIN) ? 1 : 0;
    uint64_t len = 0;
    for (int b = 7; b >= 0; b--) len = (len << 8) | st[0x18 + b];
    if (len) dir->size = len;
}

static int dir_grow(struct inode* dir) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct exfat_fs* fs = dei->fs;
    uint32_t c = bitmap_alloc(fs);
    if (!c) return -1;                                   /* volume full */
    if (cluster_zero(fs, c) != 0) { bitmap_free(fs, c); return -1; }
    fat_set(fs, c, EXFAT_FAT_EOC);

    if (dei->no_fat_chain) {
        uint32_t n = (uint32_t)(dir->size / fs->bytes_per_cluster);
        if (n == 0) n = 1;
        for (uint32_t i = 0; i + 1 < n; i++)
            fat_set(fs, dei->first_cluster + i, dei->first_cluster + i + 1);
        fat_set(fs, dei->first_cluster + n - 1, c);
        dei->no_fat_chain = 0;
    } else {
        fat_set(fs, chain_tail(fs, dei->first_cluster), c);
    }
    if (dir->size) dir->size += fs->bytes_per_cluster;   /* root: size 0 = its chain */
    if (dei->parent_first_cluster >= 2 && exfat_write_meta(dir) != 0) return -1;
    bcache_sync(fs->dev);
    return 0;
}

/* A slot for `need` entries in `dir`, growing it once when it is full. */
static uint32_t dir_find_or_grow(struct inode* dir, int need) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    dir_refresh(dir);
    uint32_t slot = find_free_dir_slot(dei->fs, dei->first_cluster, dei->no_fat_chain,
                                       dir->size, need);
    if (slot != (uint32_t)-1) return slot;
    if (dir_grow(dir) != 0) return (uint32_t)-1;
    return find_free_dir_slot(dei->fs, dei->first_cluster, dei->no_fat_chain,
                              dir->size, need);
}

/* ----------------------------------------------------------------------
 * Build a complete directory entry SET (File + Stream + Name entries).
 *
 * Factored out of exfat_make because rename needs the identical bytes with a
 * different name: the checksum, the name hash and the 15-chars-per-entry
 * split are three places to get wrong, and having them twice would mean
 * fixing a bug in one of the copies.
 *
 * Returns the number of entries written into `set`, or -1 if the name does not
 * fit.  `set` must have room for (2 + EXFAT_MAX_NAME_ENTRIES) entries.
 * ---------------------------------------------------------------------- */
static int build_entry_set(uint8_t* set, const char* name, int name_len,
                           int is_dir, uint32_t first_cluster,
                           uint64_t data_len, uint8_t stream_flags,
                           const struct owner_rec* own) {
    int name_entries = (name_len + 14) / 15;
    if (name_len <= 0 || name_entries > EXFAT_MAX_NAME_ENTRIES) return -1;
    int sec_count = 1 + name_entries + (own ? 1 : 0);
    int total_e   = 1 + sec_count;

    memset_(set, 0, (size_t)total_e * EXFAT_ENTRY_SIZE);
    uint8_t* fe = set + 0 * EXFAT_ENTRY_SIZE;
    uint8_t* se = set + 1 * EXFAT_ENTRY_SIZE;

    fe[0] = EXFAT_TYPE_FILE;
    fe[1] = (uint8_t)sec_count;
    wle16(fe + 2, 0);                           /* checksum patched below */
    wle16(fe + 4, is_dir ? EXFAT_ATTR_DIRECTORY : EXFAT_ATTR_ARCHIVE);

    se[0] = EXFAT_TYPE_STREAM;
    se[1] = stream_flags;
    se[3] = (uint8_t)name_len;
    wle16(se + 4, name_hash_ascii(name, name_len));
    wle64(se + 0x08, data_len);                 /* ValidDataLength */
    wle32(se + 0x14, first_cluster);
    wle64(se + 0x18, data_len);                 /* DataLength      */

    int produced = 0;
    for (int j = 0; j < name_entries; j++) {
        uint8_t* ne = set + (2 + j) * EXFAT_ENTRY_SIZE;
        ne[0] = EXFAT_TYPE_NAME;
        ne[1] = 0;
        for (int k = 0; k < 15; k++) {
            uint16_t ch = 0;
            if (produced < name_len) ch = (uint8_t)name[produced++];
            wle16(ne + 2 + k * 2, ch);
        }
    }
    /* §M32 — the owner record goes AFTER the names: the spec orders a set's
     * secondaries as stream, names, then any others. */
    if (own) owner_rec_build(set + (2 + name_entries) * EXFAT_ENTRY_SIZE, own);

    wle16(fe + 2, set_checksum(set, total_e * EXFAT_ENTRY_SIZE));
    return total_e;
}

static int exfat_make(struct inode* dir, const char* name, struct inode** out,
                      int as_dir) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct exfat_fs* fs = dei->fs;

    int name_len = (int)strlen_(name);
    if (name_len == 0 || name_len > EXFAT_MAX_NAME) return -1;
    dir_refresh(dir);           /* see dir_grow: the on-disk shape is the truth */

    /* REFUSE AN EXISTING NAME.
     *
     * Without this the create path happily wrote a SECOND entry set with the
     * same name, and the newest one shadowed the old: on the second boot
     * `mkdir /mnt/store` "succeeded", produced a fresh EMPTY directory, and the
     * sixteen packages already on the disk became invisible — so everything was
     * rebuilt while `ls` from the shell (which had resolved the other one)
     * showed them present.  The symptom pointed at caching; the cause was a
     * missing existence check in the six lines above it. */
    {
        struct inode* dup = NULL;
        if (exfat_lookup(dir, name, &dup) == 0 && dup) {
            if (out) *out = dup;
            return -7;                          /* already exists */
        }
    }
    int name_entries = (name_len + 14) / 15;
    if (name_entries > EXFAT_MAX_NAME_ENTRIES)      return -1;
    int sec_count    = 1 + name_entries + 1;        /* stream + names + owner */
    int total_e      = 1 + sec_count;               /* + file entry */
    /* §M32 — every set this driver writes carries an owner record, with the
     * defaults until the VFS says otherwise (a user's create is followed by a
     * setattr), so later changes are an in-place rewrite. */
    struct owner_rec own = { CRED_UID_ROOT, CRED_GID_ROOT, as_dir ? 0755u : 0644u };

    /* Find a slot in the parent directory, growing it when it is full. */
    uint32_t slot = dir_find_or_grow(dir, total_e);
    if (slot == (uint32_t)-1) return -2;

    /* Build the entries in a local buffer. */
    uint8_t set[EXFAT_MAX_SET_ENTRIES * EXFAT_ENTRY_SIZE];

    /* A directory owns a cluster from the start; a file may own none. */
    uint32_t new_cluster = 0;
    if (as_dir) {
        new_cluster = bitmap_alloc(fs);
        if (!new_cluster) return -5;            /* volume full */
        if (cluster_zero(fs, new_cluster) != 0) {
            bitmap_free(fs, new_cluster);
            return -6;
        }
        /* One cluster, contiguous → NoFatChain.  The FAT still gets an
         * end-of-chain marker so a fat-walking tool sees a terminated chain
         * rather than whatever the cluster held before. */
        fat_set(fs, new_cluster, EXFAT_FAT_EOC);
    }

    if (build_entry_set(set, name, name_len, as_dir, new_cluster,
                        as_dir ? fs->bytes_per_cluster : 0,
                        as_dir ? 0x03 : 0x00, &own) != total_e) {
        if (new_cluster) bitmap_free(fs, new_cluster);
        return -1;
    }

    /* Write the entries to disk. */
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = dei->first_cluster,
        .no_fat_chain  = dei->no_fat_chain,
        .limit         = dir->size,
    };
    for (int i = 0; i < total_e; i++) {
        if (dir_entry_write(&it, slot + i, set + i * EXFAT_ENTRY_SIZE) != 0)
            return -3;
    }

    /* Flush so the parent dir is durable before the caller starts
     * writing into the new file (which lives in its own clusters). */
    bcache_sync(fs->dev);

    /* Build an inode mirroring the entries we just wrote. */
    struct parsed_file pf = {
        .attrs         = as_dir ? EXFAT_ATTR_DIRECTORY : EXFAT_ATTR_ARCHIVE,
        .sec_count     = (uint8_t)sec_count,
        .stream_flags  = as_dir ? 0x03 : 0,
        .name_length   = (uint8_t)name_len,
        .first_cluster = new_cluster,
        .data_length   = as_dir ? fs->bytes_per_cluster : 0,
        .dirent_index  = slot,
        .has_owner     = 1,
        .own           = own,
    };
    struct inode* ino = build_inode(fs, &pf, dei->first_cluster, dei->no_fat_chain,
                                    dir->size);
    if (!ino) return -4;
    if (out) *out = ino;
    return 0;
}

static int exfat_create(struct inode* dir, const char* name, struct inode** out) {
    return exfat_make(dir, name, out, 0);
}

static int exfat_mkdir(struct inode* dir, const char* name, struct inode** out) {
    return exfat_make(dir, name, out, 1);
}

/* Remove `name` from `dir`.  Refuses a non-empty directory (-2, the code the
 * VFS already documents for that case); a recursive delete is policy and lives
 * one layer up. */
static int exfat_unlink(struct inode* dir, const char* name,
                        struct inode* child) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct exfat_fs* fs = dei->fs;

    /* The VFS resolved the dentry and hands us the inode; look it up only if it
     * did not (an eager-tree fs would always have one — exFAT's lookup is lazy,
     * so both routes have to work). */
    struct inode* target = child;
    if (!target && (exfat_lookup(dir, name, &target) != 0 || !target)) return -1;
    struct exfat_inode* tei = (struct exfat_inode*)target->private;
    if (!tei) return -1;

    if (target->type == INODE_DIR &&
        !dir_is_empty(fs, tei->first_cluster, tei->no_fat_chain, target->size))
        return -2;                              /* not empty */

    if (dirent_set_delete(fs, dei->first_cluster, dei->no_fat_chain, dir->size,
                          tei->dirent_index) != 0)
        return -1;
    chain_free(fs, tei->first_cluster, tei->no_fat_chain, target->size);
    bcache_sync(fs->dev);
    return 0;
}

/* ----------------------------------------------------------------------
 * Rename — the last NULL in the ops table (§M12's gap, closed).
 *
 * A RENAME IS NOT AN EDIT.  The name lives across File Name entries at fifteen
 * characters each, so a different name is very often a different NUMBER of
 * entries — and SecondaryCount, the SetChecksum and the slot's extent all
 * change with it.  Patching in place would work for names that happen to round
 * the same way and corrupt the set for the ones that do not, which is the
 * worst kind of bug: correct in testing, wrong on a user's file.
 *
 * So: write a COMPLETE new entry set describing the same cluster chain, then
 * delete the old one.  The DATA is never touched — a rename must not read or
 * move a single byte of the file.
 *
 * ORDER: new first, old second.  A crash between them leaves two names for one
 * chain, which `fsck` reports as a cross-link and a human can resolve; the
 * other order leaves the chain allocated and unreferenced, i.e. the file is
 * simply gone.  Losing the name is recoverable, losing the file is not.
 * ---------------------------------------------------------------------- */
/* Write a COMPLETE new entry set for `target` under `name` (with its current
 * owner record), then delete the old one — see the rename header above for
 * why "new first, old second".  Used by rename, and by setattr for a set that
 * has no owner record yet (one written by another system, or before §M32
 * stored ownership), which needs a bigger set than the one it has. */
static int relocate_set(struct inode* dir, const char* newname, int new_len,
                        struct inode* target) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct exfat_inode* tei = (struct exfat_inode*)target->private;
    struct exfat_fs* fs = dei->fs;
    /* The new set describes exactly what the old one did — same cluster, same
     * length, same kind — with a different name. */
    int is_dir = (target->type == INODE_DIR);
    uint8_t flags = 0;
    if (tei->first_cluster) flags |= 0x01;                 /* AllocationPossible */
    if (tei->no_fat_chain)  flags |= 0x02;                 /* NoFatChain         */

    uint8_t set[EXFAT_MAX_SET_ENTRIES * EXFAT_ENTRY_SIZE];
    /* §M32 — the owner travels with the file; a rename of a file written by
     * another system gives it a record carrying what this kernel enforces. */
    struct owner_rec own = { target->owner_uid, target->owner_gid, target->mode };
    int total_e = build_entry_set(set, newname, new_len, is_dir,
                                  tei->first_cluster, target->size, flags, &own);
    if (total_e < 0) return -1;

    uint32_t slot = dir_find_or_grow(dir, total_e);
    if (slot == (uint32_t)-1) return -3;                   /* volume full */

    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = dei->first_cluster,
        .no_fat_chain  = dei->no_fat_chain,
        .limit         = dir->size,
    };
    for (int i = 0; i < total_e; i++) {
        if (dir_entry_write(&it, slot + i, set + i * EXFAT_ENTRY_SIZE) != 0)
            return -4;
    }

    /* The old set goes only once the new one is on the disk. */
    if (dirent_set_delete(fs, dei->first_cluster, dei->no_fat_chain, dir->size,
                          tei->dirent_index) != 0) {
        /* The new set is already written; leaving both would be a cross-link,
         * so undo it and report failure rather than half-renaming. */
        dirent_set_delete(fs, dei->first_cluster, dei->no_fat_chain, dir->size, slot);
        bcache_sync(fs->dev);
        return -4;
    }

    /* THE INODE STILL POINTS AT THE OLD SLOT.  Every later write goes through
     * `dirent_index` to rewrite the Stream Extension, so leaving it stale
     * would have the next write update a DELETED entry — the file would look
     * renamed and then silently stop growing. */
    tei->dirent_index = slot;
    tei->sec_count    = (uint8_t)(total_e - 1);
    tei->has_owner_rec = 1;
    /* And the parent's SHAPE, which dir_find_or_grow may just have changed:
     * the new slot can lie in a cluster the directory gained a moment ago,
     * beyond the size (and past the NoFatChain run) this inode remembers. */
    tei->parent_size         = dir->size;
    tei->parent_no_fat_chain = dei->no_fat_chain;

    bcache_sync(fs->dev);
    return 0;
}

static int exfat_rename(struct inode* dir, const char* oldname,
                        const char* newname, struct inode* child) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    if (!dei) return -1;

    int new_len = (int)strlen_(newname);
    /* -5, not -1: "the name is too long for this filesystem" is a different
     * thing from "that rename cannot be done", and a caller that cannot tell
     * them apart reports the wrong reason to the user.  (It reported "same
     * directory only?" for a 43-character name, which sent the first test
     * looking in entirely the wrong place.) */
    if (new_len == 0) return -1;
    if (new_len > EXFAT_MAX_NAME) return -5;

    struct inode* target = child;
    if (!target && (exfat_lookup(dir, oldname, &target) != 0 || !target)) return -1;
    struct exfat_inode* tei = (struct exfat_inode*)target->private;
    if (!tei) return -1;

    /* Renaming to the name it already has is success, and must NOT go through
     * the write-then-delete below — that would delete the set just written. */
    if (streq_(oldname, newname)) return 0;

    /* REFUSE AN EXISTING TARGET, the rule §4.73 paid for: a filesystem that
     * can hold one name twice does not have a namespace.  The VFS may also
     * check, but the fs owns its directory and must not depend on that. */
    {
        struct inode* dup = NULL;
        if (exfat_lookup(dir, newname, &dup) == 0 && dup) return -2;
    }

    return relocate_set(dir, newname, new_len, target);
}


/* §M32 — store the inode's owner and mode in its entry set.  In place when
 * the set already carries our record (every set this driver has written since
 * 2026-09-27); otherwise the set is rewritten one entry larger. */
static int exfat_setattr(struct inode* dir, const char* name, struct inode* child) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct exfat_inode* tei = child ? (struct exfat_inode*)child->private : NULL;
    if (!dei || !tei || !name) return -1;
    struct exfat_fs* fs = dei->fs;
    struct owner_rec own = { child->owner_uid, child->owner_gid, child->mode };

    if (tei->has_owner_rec) {
        struct dir_iter it = {
            .fs            = fs,
            .first_cluster = dei->first_cluster,
            .no_fat_chain  = dei->no_fat_chain,
            .limit         = dir->size,
        };
        int total = 1 + tei->sec_count;
        if (total > EXFAT_MAX_SET_ENTRIES) return -1;
        uint8_t buf[EXFAT_MAX_SET_ENTRIES * EXFAT_ENTRY_SIZE];
        for (int i = 0; i < total; i++)
            if (dir_entry_read(&it, tei->dirent_index + i, buf + i * EXFAT_ENTRY_SIZE) != 0)
                return -1;
        if (buf[0] != EXFAT_TYPE_FILE) return -1;     /* the slot moved under us */
        for (int k = 2; k < total; k++) {
            struct owner_rec cur;
            if (!owner_rec_parse(buf + k * EXFAT_ENTRY_SIZE, &cur)) continue;
            owner_rec_build(buf + k * EXFAT_ENTRY_SIZE, &own);
            wle16(buf + 2, set_checksum(buf, total * EXFAT_ENTRY_SIZE));
            for (int i = 0; i < total; i++)
                if (dir_entry_write(&it, tei->dirent_index + i, buf + i * EXFAT_ENTRY_SIZE) != 0)
                    return -1;
            bcache_sync(fs->dev);
            return 0;
        }
        /* Flagged but not found: fall through and rewrite it whole. */
    }
    return relocate_set(dir, name, (int)strlen_(name), child);
}

/* ---------------------------------------------------------------------- */
/* Tables.                                                                 */
/* ---------------------------------------------------------------------- */

/* ---- the volume lock at every VFS entry point (see struct exfat_fs) ---- */
static struct exfat_fs* fs_of_inode(struct inode* ino) {
    struct exfat_inode* ei = ino ? (struct exfat_inode*)ino->private : NULL;
    return ei ? ei->fs : NULL;
}
#define FS_LOCKED(fs, call) ({                                    \
        struct exfat_fs* _fs = (fs);                               \
        if (_fs) kmutex_lock(&_fs->lock);                          \
        __typeof__(call) _r = (call);                              \
        if (_fs) kmutex_unlock(&_fs->lock);                        \
        _r; })

static ssize_t exfat_read_locked(struct file* f, void* buf, size_t n, uint64_t off) {
    return FS_LOCKED(fs_of_inode(f->inode), exfat_read(f, buf, n, off));
}
static ssize_t exfat_write_locked(struct file* f, const void* buf, size_t n, uint64_t off) {
    return FS_LOCKED(fs_of_inode(f->inode), exfat_write(f, buf, n, off));
}
static int exfat_readdir_locked(struct file* f, struct dirent* out) {
    return FS_LOCKED(fs_of_inode(f->inode), exfat_readdir(f, out));
}
static int exfat_close_locked(struct file* f) {
    return FS_LOCKED(fs_of_inode(f->inode), exfat_close(f));
}
static int exfat_lookup_locked(struct inode* dir, const char* name, struct inode** out) {
    return FS_LOCKED(fs_of_inode(dir), exfat_lookup(dir, name, out));
}
static int exfat_create_locked(struct inode* dir, const char* name, struct inode** out) {
    return FS_LOCKED(fs_of_inode(dir), exfat_create(dir, name, out));
}
static int exfat_mkdir_locked(struct inode* dir, const char* name, struct inode** out) {
    return FS_LOCKED(fs_of_inode(dir), exfat_mkdir(dir, name, out));
}
static int exfat_unlink_locked(struct inode* dir, const char* name, struct inode* child) {
    return FS_LOCKED(fs_of_inode(dir), exfat_unlink(dir, name, child));
}
static int exfat_rename_locked(struct inode* dir, const char* o, const char* n, struct inode* child) {
    return FS_LOCKED(fs_of_inode(dir), exfat_rename(dir, o, n, child));
}
static int exfat_setattr_locked(struct inode* dir, const char* n, struct inode* child) {
    return FS_LOCKED(fs_of_inode(dir), exfat_setattr(dir, n, child));
}

static const struct file_ops exfat_file_ops = {
    .read    = exfat_read_locked,
    .write   = exfat_write_locked,
    .readdir = NULL,
    .close   = exfat_close_locked,
};

static const struct file_ops exfat_dir_ops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = exfat_readdir_locked,
    .close   = exfat_close_locked,
};

static const struct inode_ops exfat_inode_ops_dir = {
    .lookup = exfat_lookup_locked,
    .create = exfat_create_locked,
    .mkdir  = exfat_mkdir_locked,
    .unlink = exfat_unlink_locked,
    .rename = exfat_rename_locked,
    .setattr = exfat_setattr_locked,        /* §M32 — ownership on the volume */
};

/* ---------------------------------------------------------------------- */
/* Mount.                                                                  */
/* ---------------------------------------------------------------------- */

/* Visitor used at mount-time to find the allocation bitmap.  Walks raw
 * 32-byte entries (not the File-entry-set parser) because the bitmap
 * isn't part of a name-bearing set. */
static int mount_scan_meta(struct exfat_fs* fs) {
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = fs->root_cluster,
        .no_fat_chain  = 0,
    };
    uint32_t idx = 0;
    uint8_t entry[EXFAT_ENTRY_SIZE];
    for (;;) {
        if (dir_entry_read(&it, idx, entry) != 0) return -1;
        uint8_t type = entry[0];
        if (type == EXFAT_TYPE_END) break;
        if (type == EXFAT_TYPE_BITMAP) {
            fs->bitmap_cluster = le32(entry + 0x14);
            fs->bitmap_size    = le64(entry + 0x18);
            return 0;
        }
        if (type == EXFAT_TYPE_FILE) {
            /* Skip over a File entry-set so we keep proper alignment;
             * the bitmap should appear BEFORE the first file anyway. */
            idx += 1 + entry[1];
            continue;
        }
        idx++;
    }
    return -2;          /* no bitmap found */
}

static int exfat_mount(struct block_device* dev, struct dentry* mp) {
    if (!dev) {
        kprintf("exfat: mount requires a block device\n");
        return -1;
    }
    if (mp->inode) {
        kprintf("exfat: mountpoint already occupied\n");
        return -2;
    }

    /* Read the boot sector.  exFAT is always 512+ byte sector aligned,
     * so a single bcache_get(0) suffices regardless of geometry. */
    struct bcache_buf* boot = bcache_get(dev, EXFAT_SECTOR_BOOT);
    if (!boot) {
        kprintf("exfat: failed to read boot sector\n");
        return -3;
    }
    const uint8_t* bs = boot->data;

    /* Validate "EXFAT   " signature at offset 3. */
    static const char sig[8] = { 'E','X','F','A','T',' ',' ',' ' };
    for (int i = 0; i < 8; i++) {
        if (bs[3 + i] != (uint8_t)sig[i]) {
            kprintf("exfat: bad fs signature (not an exFAT volume)\n");
            bcache_release(boot);
            return -4;
        }
    }

    struct exfat_fs* fs = (struct exfat_fs*)kcalloc(1, sizeof(*fs));
    if (!fs) { bcache_release(boot); return -5; }

    fs->dev                     = dev;
    kmutex_init(&fs->lock, "exfat");
    fs->fat_offset              = le32(bs + 0x50);
    fs->fat_length              = le32(bs + 0x54);
    fs->cluster_heap_offset     = le32(bs + 0x58);
    fs->cluster_count           = le32(bs + 0x5C);
    fs->root_cluster            = le32(bs + 0x60);
    uint8_t bps_shift           = bs[0x6C];
    uint8_t spc_shift           = bs[0x6D];
    fs->bytes_per_sector        = 1u << bps_shift;
    fs->sectors_per_cluster     = 1u << spc_shift;
    fs->bytes_per_cluster       = fs->bytes_per_sector * fs->sectors_per_cluster;
    bcache_release(boot);

    if (fs->bytes_per_sector != dev->sector_size) {
        kprintf("exfat: bps mismatch fs=%u dev=%u\n",
                fs->bytes_per_sector, dev->sector_size);
        kfree(fs);
        return -6;
    }

    /* Scan the root for the allocation bitmap; write path needs it. */
    if (mount_scan_meta(fs) != 0) {
        kprintf("exfat: bitmap entry not found in root directory\n");
        kfree(fs);
        return -7;
    }

    /* Build the root inode and attach it to the mountpoint dentry. */
    struct inode* rino = (struct inode*)kcalloc(1, sizeof(struct inode));
    struct exfat_inode* rei = (struct exfat_inode*)kcalloc(1, sizeof(*rei));
    if (!rino || !rei) {
        if (rino) kfree(rino);
        if (rei)  kfree(rei);
        kfree(fs);
        return -8;
    }
    rei->fs                    = fs;
    rei->first_cluster         = fs->root_cluster;
    rei->no_fat_chain          = 0;
    rei->parent_first_cluster  = 0;
    rino->type                 = INODE_DIR;
    rino->size                 = 0;
    rino->ops                  = &exfat_dir_ops;
    rino->dir_ops              = &exfat_inode_ops_dir;
    rino->private              = rei;
    vfs_inode_defaults(rino);          /* §M32 — synthesised, see above */
    mp->inode                  = rino;

    kprintf("exfat: mounted dev=%s clusters=%u bps=%u spc=%u root=%u bitmap=%u (%u bytes)\n",
            dev->name, fs->cluster_count, fs->bytes_per_sector,
            fs->sectors_per_cluster, fs->root_cluster,
            fs->bitmap_cluster, (unsigned)fs->bitmap_size);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* §M87 — unmount, free space, and FORMAT.                                 */
/* ---------------------------------------------------------------------- */

static struct exfat_fs* fs_of_mp(struct dentry* mp) {
    if (!mp || !mp->inode || !mp->inode->private) return NULL;
    return ((struct exfat_inode*)mp->inode->private)->fs;
}

/* The VFS has already refused if anything below is open.  Everything the
 * volume wrote goes to the disk BEFORE the per-volume state is freed; the VFS
 * then frees the tree through exfat_evict. */
static int exfat_umount(struct dentry* mp) {
    struct exfat_fs* fs = fs_of_mp(mp);
    if (!fs) return -1;
    kmutex_lock(&fs->lock);
    int r = bcache_sync(fs->dev);
    kmutex_unlock(&fs->lock);
    if (r != 0) {
        /* A volume whose last writes did not reach the disk is NOT detached:
         * the cache still holds the only copy, and dropping it would be the
         * data loss the refusal exists to prevent. */
        kprintf("exfat: %s: write-back failed - NOT unmounting\n", fs->dev->name);
        return -1;
    }
    kfree(fs);
    return 0;
}

static void exfat_evict(struct inode* ino) {
    if (ino && ino->private) { kfree(ino->private); ino->private = NULL; }
}

/* Free space = clear bits in the allocation bitmap.  Counted rather than
 * cached: a cached counter must be updated by every allocate and free, and a
 * missed site makes the disk manager report space the volume does not have. */
static int exfat_statfs(struct dentry* mp, uint64_t* total, uint64_t* freeb) {
    struct exfat_fs* fs = fs_of_mp(mp);
    if (!fs) return -1;
    kmutex_lock(&fs->lock);
    uint64_t base = cluster_first_lba(fs, fs->bitmap_cluster);
    uint64_t nsec = (fs->bitmap_size + fs->bytes_per_sector - 1) / fs->bytes_per_sector;
    uint64_t used = 0;
    for (uint64_t sct = 0; sct < nsec; sct++) {
        struct bcache_buf* b = bcache_get(fs->dev, base + sct);
        if (!b) { kmutex_unlock(&fs->lock); return -1; }
        for (uint32_t i = 0; i < fs->bytes_per_sector; i++) {
            uint64_t bit0 = (sct * fs->bytes_per_sector + i) * 8ull;
            if (bit0 >= fs->cluster_count) break;
            uint8_t v = b->data[i];
            for (int k = 0; k < 8 && bit0 + k < fs->cluster_count; k++)
                if (v & (1u << k)) used++;
        }
        bcache_release(b);
    }
    kmutex_unlock(&fs->lock);
    if (total) *total = (uint64_t)fs->cluster_count * fs->bytes_per_cluster;
    if (freeb) *freeb = ((uint64_t)fs->cluster_count - used) * fs->bytes_per_cluster;
    return 0;
}


/* The boot-region checksum (exFAT spec §3.4): every byte of sectors 0..10
 * except VolumeFlags (106, 107) and PercentInUse (112), which change during
 * normal use and must not invalidate the checksum. */
static uint32_t boot_checksum(const uint8_t* region, uint32_t bytes) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < bytes; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + region[i];
    }
    return c;
}

static uint32_t table_checksum(const uint8_t* d, uint32_t n) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + d[i];
    return c;
}

/* A COMPRESSED up-case table (spec §7.2.5.1): 0xFFFF followed by N means "the
 * next N code points map to themselves".  So: identity up to 'a', then a..z
 * to A..Z, then identity for everything else.  60 bytes instead of 5836, and
 * complete — a code point the table does not name is not "unmapped", it is
 * inside an identity run.  (Latin-1 and Latin-2 letters stay as they are:
 * names are compared case-SENSITIVELY for those, which is a limitation, not a
 * corruption.) */
static uint32_t build_upcase(uint8_t* out) {
    uint32_t n = 0;
    wle16(out + n, 0xFFFF); n += 2; wle16(out + n, 0x0061); n += 2;
    for (uint16_t c = 'a'; c <= 'z'; c++) { wle16(out + n, (uint16_t)(c - 32)); n += 2; }
    wle16(out + n, 0xFFFF); n += 2; wle16(out + n, (uint16_t)(0x10000 - 0x7B)); n += 2;
    return n;
}

/* Write one sector through the block layer.  The cache is dropped for the
 * whole device before and after, so the format is not read back through a
 * cache of the previous filesystem. */
static int wr(struct block_device* d, uint64_t lba, const uint8_t* buf) {
    return blk_write(d, lba, 1, buf);
}

int exfat_format(struct block_device* dev, const char* label) {
    if (!dev || dev->sector_size != 512) return -1;
    uint64_t total = dev->sector_count;
    if (total < 2048) return -2;                     /* < 1 MiB: pointless */

    /* Cluster size by volume size — the table mkfs.exfat uses, rounded. */
    uint32_t spc_shift = total <= (256ull << 11) ? 3      /* <=256 MiB: 4 KiB  */
                       : total <= (32ull << 21) ? 6       /* <=32 GiB: 32 KiB  */
                       : 8;                               /* else: 128 KiB     */
    uint32_t spc = 1u << spc_shift;
    uint32_t fat_off = 128;                               /* aligned, roomy    */
    uint32_t fat_len = 1, heap_off = 0, clusters = 0;
    for (int it = 0; it < 4; it++) {                      /* converges at once */
        heap_off = ((fat_off + fat_len + spc - 1) / spc) * spc;
        clusters = (uint32_t)((total - heap_off) / spc);
        fat_len  = (uint32_t)(((uint64_t)(clusters + 2) * 4 + 511) / 512);
    }
    uint32_t cbytes   = spc * 512u;
    uint32_t bm_bytes = (clusters + 7) / 8;
    uint32_t bm_clus  = (bm_bytes + cbytes - 1) / cbytes;
    uint32_t up_clus  = 1;                                /* 60 bytes */
    uint32_t c_bitmap = 2, c_upcase = 2 + bm_clus, c_root = c_upcase + up_clus;
    uint32_t used     = bm_clus + up_clus + 1;
    if (used + 1 > clusters) return -2;

    uint8_t* sec = (uint8_t*)kcalloc(1, 512);
    uint8_t* region = (uint8_t*)kcalloc(1, 11 * 512);
    if (!sec || !region) { kfree(sec); kfree(region); return -3; }
    bcache_invalidate(dev);
    int rc = 0;

    /* ---- boot region (sectors 0..10), then its checksum sector ---- */
    uint8_t* bs = region;
    bs[0] = 0xEB; bs[1] = 0x76; bs[2] = 0x90;
    for (int i = 0; i < 8; i++) bs[3 + i] = (uint8_t)"EXFAT   "[i];
    wle64(bs + 64, 0);                                    /* PartitionOffset   */
    wle64(bs + 72, total);                                /* VolumeLength      */
    wle32(bs + 80, fat_off);
    wle32(bs + 84, fat_len);
    wle32(bs + 88, heap_off);
    wle32(bs + 92, clusters);
    wle32(bs + 96, c_root);
    wle32(bs + 100, (uint32_t)(timer_now_ns() >> 10) ^ 0x44534F53u); /* serial */
    wle16(bs + 104, 0x0100);                              /* revision 1.0      */
    wle16(bs + 106, 0);                                   /* VolumeFlags       */
    bs[108] = 9;                                          /* 512-byte sectors  */
    bs[109] = (uint8_t)spc_shift;
    bs[110] = 1;                                          /* NumberOfFats      */
    bs[111] = 0x80;
    bs[112] = 0xFF;                                       /* PercentInUse: n/a */
    for (int i = 120; i < 510; i++) bs[i] = 0xF4;         /* boot code: hlt    */
    bs[510] = 0x55; bs[511] = 0xAA;
    for (int k = 1; k <= 8; k++) {                        /* extended boot     */
        region[k * 512 + 510] = 0x55; region[k * 512 + 511] = 0xAA;
    }
    uint32_t ck = boot_checksum(region, 11 * 512);
    for (int copy = 0; copy < 2 && !rc; copy++) {         /* main + backup     */
        uint64_t base = copy ? 12 : 0;
        for (int k = 0; k < 11 && !rc; k++) rc = wr(dev, base + k, region + k * 512);
        for (int i = 0; i < 512; i += 4) wle32(sec + i, ck);
        if (!rc) rc = wr(dev, base + 11, sec);
    }

    /* ---- FAT: media + reserved, then one chain per metadata object ---- */
    for (uint32_t k = 0; k < fat_len && !rc; k++) {
        for (int i = 0; i < 512; i++) sec[i] = 0;
        for (int i = 0; i < 128; i++) {
            uint32_t e = k * 128 + (uint32_t)i, v = 0;
            if (e == 0) v = 0xFFFFFFF8u;
            else if (e == 1) v = 0xFFFFFFFFu;
            else if (e >= c_bitmap && e < c_upcase)
                v = (e + 1 < c_upcase) ? e + 1 : 0xFFFFFFFFu;
            else if (e == c_upcase || e == c_root) v = 0xFFFFFFFFu;
            wle32(sec + i * 4, v);
        }
        rc = wr(dev, fat_off + k, sec);
    }

    /* ---- the allocation bitmap: the first `used` clusters are taken ---- */
    uint64_t lba_bm = heap_off + (uint64_t)(c_bitmap - 2) * spc;
    for (uint32_t k = 0; k < bm_clus * spc && !rc; k++) {
        for (int i = 0; i < 512; i++) {
            uint32_t bit0 = (k * 512 + (uint32_t)i) * 8;
            uint8_t v = 0;
            for (int b = 0; b < 8; b++) if (bit0 + b < used) v |= (uint8_t)(1u << b);
            sec[i] = v;
        }
        rc = wr(dev, lba_bm + k, sec);
    }

    /* ---- the up-case table ---- */
    uint8_t up[64];
    uint32_t up_len = build_upcase(up);
    uint32_t up_ck  = table_checksum(up, up_len);
    uint64_t lba_up = heap_off + (uint64_t)(c_upcase - 2) * spc;
    for (uint32_t k = 0; k < spc && !rc; k++) {
        for (int i = 0; i < 512; i++) sec[i] = 0;
        if (k == 0) for (uint32_t i = 0; i < up_len; i++) sec[i] = up[i];
        rc = wr(dev, lba_up + k, sec);
    }

    /* ---- the root directory: label, bitmap, up-case, then end ---- */
    uint64_t lba_root = heap_off + (uint64_t)(c_root - 2) * spc;
    for (uint32_t k = 0; k < spc && !rc; k++) {
        for (int i = 0; i < 512; i++) sec[i] = 0;
        if (k == 0) {
            uint8_t* e = sec;
            int ll = 0;
            while (label && label[ll] && ll < 11) ll++;
            e[0] = ll ? 0x83 : 0x03;                      /* volume label      */
            e[1] = (uint8_t)ll;
            for (int i = 0; i < ll; i++) wle16(e + 2 + i * 2, (uint8_t)label[i]);
            e += 32;
            e[0] = 0x81;                                  /* allocation bitmap */
            wle32(e + 20, c_bitmap);
            wle64(e + 24, bm_bytes);
            e += 32;
            e[0] = 0x82;                                  /* up-case table     */
            wle32(e + 4, up_ck);
            wle32(e + 20, c_upcase);
            wle64(e + 24, up_len);
        }
        rc = wr(dev, lba_root + k, sec);
    }
    if (!rc) blk_flush(dev);
    bcache_invalidate(dev);
    kfree(sec);
    kfree(region);
    if (rc) { kprintf("exfat: format of %s FAILED (I/O error)\n", dev->name); return -4; }
    kprintf("exfat: formatted %s - %u clusters of %u bytes, label '%s'\n",
            dev->name, clusters, cbytes, label ? label : "");
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Module registration.                                                    */
/* ---------------------------------------------------------------------- */

static struct fs_type exfat_fs_type = {
    .name   = "exfat",
    .mount  = exfat_mount,
    .next   = NULL,
    .umount = exfat_umount,             /* §M87 */
    .evict  = exfat_evict,
    .statfs = exfat_statfs,
    /* §M32 (2026-09-27) — owner and mode live in each file's entry set, in a
     * d-os Vendor Extension entry (see DOS_OWNER_GUID). */
    .stores_ownership = 1,
};

static int exfat_module_init(void) {
    return vfs_register_fs(&exfat_fs_type);
}

MODULE("exfat", "fs", exfat_module_init);
