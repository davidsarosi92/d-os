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

#include "vfs.h"
#include "block.h"
#include "block_cache.h"
#include "kmalloc.h"
#include "printf.h"
#include "module.h"
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
    uint32_t dirent_index;               /* 0-based index of File entry within parent */
    uint8_t  sec_count;                  /* SecondaryCount from File entry */
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
    uint32_t entry_index;                 /* next entry to fetch (0-based) */
};

/* Fetch the entry at `idx` into `out` (32 bytes).  Returns 0 on success,
 * non-zero on I/O error or chain end. */
static int dir_entry_read(struct dir_iter* it, uint32_t idx, uint8_t* out) {
    uint64_t off = (uint64_t)idx * EXFAT_ENTRY_SIZE;
    uint32_t within;
    struct bcache_buf* b = cluster_chain_get_sector(it->fs, it->first_cluster,
                                                    it->no_fat_chain, off,
                                                    &within);
    if (!b) return -1;
    memcpy_(out, b->data + within, EXFAT_ENTRY_SIZE);
    bcache_release(b);
    return 0;
}

/* Write a 32-byte entry at `idx`.  Marks the buffer dirty. */
static int dir_entry_write(struct dir_iter* it, uint32_t idx, const uint8_t* in) {
    uint64_t off = (uint64_t)idx * EXFAT_ENTRY_SIZE;
    uint32_t within;
    struct bcache_buf* b = cluster_chain_get_sector(it->fs, it->first_cluster,
                                                    it->no_fat_chain, off,
                                                    &within);
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
};

static struct inode* build_inode(struct exfat_fs* fs,
                                 const struct parsed_file* pf,
                                 uint32_t parent_first_cluster,
                                 int parent_no_fat_chain) {
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
    ei->dirent_index           = pf->dirent_index;
    ei->sec_count              = pf->sec_count;

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

static int scan_directory(struct exfat_fs* fs, uint32_t parent_first_cluster,
                          int parent_no_fat_chain, dir_visit_fn visit,
                          void* ctx) {
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = parent_first_cluster,
        .no_fat_chain  = parent_no_fat_chain,
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
    struct inode* found;
};

static int lookup_visit(const char* name, const struct parsed_file* pf, void* ctx_) {
    struct lookup_ctx* ctx = (struct lookup_ctx*)ctx_;
    if (!streq_(name, ctx->target)) return 0;
    ctx->found = build_inode(ctx->fs, pf, ctx->parent_first_cluster,
                             ctx->parent_no_fat_chain);
    return 1;
}

static int exfat_lookup(struct inode* dir, const char* name, struct inode** out) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct lookup_ctx ctx = {
        .target = name,
        .fs = dei->fs,
        .parent_first_cluster = dei->first_cluster,
        .parent_no_fat_chain = dei->no_fat_chain,
        .found = NULL,
    };
    scan_directory(dei->fs, dei->first_cluster, dei->no_fat_chain,
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
    scan_directory(ei->fs, ei->first_cluster, ei->no_fat_chain,
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
    };

    int total = 1 + ei->sec_count;
    if (total > 1 + 1 + EXFAT_MAX_NAME_ENTRIES) return -1;

    uint8_t buf[(1 + 1 + EXFAT_MAX_NAME_ENTRIES) * EXFAT_ENTRY_SIZE];
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
                                   int parent_no_fat_chain, int need) {
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = parent_first_cluster,
        .no_fat_chain  = parent_no_fat_chain,
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
        /* Sanity bound: if we've walked past the first directory cluster
         * with no luck, grow logic should kick in.  For now M12 keeps it
         * simple and accepts only fitting in the existing cluster. */
        if ((uint64_t)idx * EXFAT_ENTRY_SIZE >= (uint64_t)fs->bytes_per_cluster * 4)
            return (uint32_t)-1;
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
                             int dir_nofat, uint32_t slot) {
    struct dir_iter it = { .fs = fs, .first_cluster = dir_cluster,
                           .no_fat_chain = dir_nofat };
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
static int dir_is_empty(struct exfat_fs* fs, uint32_t cluster, int no_fat) {
    struct dir_iter it = { .fs = fs, .first_cluster = cluster,
                           .no_fat_chain = no_fat };
    uint8_t e[EXFAT_ENTRY_SIZE];
    for (uint32_t i = 0; i < 4096; i++) {
        if (dir_entry_read(&it, i, e) != 0) break;
        if (e[0] == 0x00) break;                    /* end of directory */
        if (e[0] == EXFAT_TYPE_FILE) return 0;      /* an in-use file/dir */
    }
    return 1;
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
                           uint64_t data_len, uint8_t stream_flags) {
    int name_entries = (name_len + 14) / 15;
    if (name_len <= 0 || name_entries > EXFAT_MAX_NAME_ENTRIES) return -1;
    int sec_count = 1 + name_entries;
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

    wle16(fe + 2, set_checksum(set, total_e * EXFAT_ENTRY_SIZE));
    return total_e;
}

static int exfat_make(struct inode* dir, const char* name, struct inode** out,
                      int as_dir) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    struct exfat_fs* fs = dei->fs;

    int name_len = (int)strlen_(name);
    if (name_len == 0 || name_len > EXFAT_MAX_NAME) return -1;

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
    int sec_count    = 1 + name_entries;            /* stream + name entries */
    int total_e      = 1 + sec_count;               /* + file entry */

    /* Find a slot in the parent directory. */
    uint32_t slot = find_free_dir_slot(fs, dei->first_cluster, dei->no_fat_chain,
                                       total_e);
    if (slot == (uint32_t)-1) return -2;

    /* Build the entries in a local buffer. */
    uint8_t set[(1 + 1 + EXFAT_MAX_NAME_ENTRIES) * EXFAT_ENTRY_SIZE];

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
                        as_dir ? 0x03 : 0x00) != total_e) {
        if (new_cluster) bitmap_free(fs, new_cluster);
        return -1;
    }

    /* Write the entries to disk. */
    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = dei->first_cluster,
        .no_fat_chain  = dei->no_fat_chain,
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
    };
    struct inode* ino = build_inode(fs, &pf, dei->first_cluster, dei->no_fat_chain);
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
        !dir_is_empty(fs, tei->first_cluster, tei->no_fat_chain))
        return -2;                              /* not empty */

    if (dirent_set_delete(fs, dei->first_cluster, dei->no_fat_chain,
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
static int exfat_rename(struct inode* dir, const char* oldname,
                        const char* newname, struct inode* child) {
    struct exfat_inode* dei = (struct exfat_inode*)dir->private;
    if (!dei) return -1;
    struct exfat_fs* fs = dei->fs;

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

    /* The new set describes exactly what the old one did — same cluster, same
     * length, same kind — with a different name. */
    int is_dir = (target->type == INODE_DIR);
    uint8_t flags = 0;
    if (tei->first_cluster) flags |= 0x01;                 /* AllocationPossible */
    if (tei->no_fat_chain)  flags |= 0x02;                 /* NoFatChain         */

    uint8_t set[(1 + 1 + EXFAT_MAX_NAME_ENTRIES) * EXFAT_ENTRY_SIZE];
    int total_e = build_entry_set(set, newname, new_len, is_dir,
                                  tei->first_cluster, target->size, flags);
    if (total_e < 0) return -1;

    uint32_t slot = find_free_dir_slot(fs, dei->first_cluster, dei->no_fat_chain,
                                       total_e);
    if (slot == (uint32_t)-1) return -3;                   /* directory full */

    struct dir_iter it = {
        .fs            = fs,
        .first_cluster = dei->first_cluster,
        .no_fat_chain  = dei->no_fat_chain,
    };
    for (int i = 0; i < total_e; i++) {
        if (dir_entry_write(&it, slot + i, set + i * EXFAT_ENTRY_SIZE) != 0)
            return -4;
    }

    /* The old set goes only once the new one is on the disk. */
    if (dirent_set_delete(fs, dei->first_cluster, dei->no_fat_chain,
                          tei->dirent_index) != 0) {
        /* The new set is already written; leaving both would be a cross-link,
         * so undo it and report failure rather than half-renaming. */
        dirent_set_delete(fs, dei->first_cluster, dei->no_fat_chain, slot);
        bcache_sync(fs->dev);
        return -4;
    }

    /* THE INODE STILL POINTS AT THE OLD SLOT.  Every later write goes through
     * `dirent_index` to rewrite the Stream Extension, so leaving it stale
     * would have the next write update a DELETED entry — the file would look
     * renamed and then silently stop growing. */
    tei->dirent_index = slot;
    tei->sec_count    = (uint8_t)(total_e - 1);

    bcache_sync(fs->dev);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Tables.                                                                 */
/* ---------------------------------------------------------------------- */

static const struct file_ops exfat_file_ops = {
    .read    = exfat_read,
    .write   = exfat_write,
    .readdir = NULL,
    .close   = exfat_close,
};

static const struct file_ops exfat_dir_ops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = exfat_readdir,
    .close   = exfat_close,
};

static const struct inode_ops exfat_inode_ops_dir = {
    .lookup = exfat_lookup,
    .create = exfat_create,
    .mkdir  = exfat_mkdir,
    .unlink = exfat_unlink,
    .rename = exfat_rename,
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
    mp->inode                  = rino;

    kprintf("exfat: mounted dev=%s clusters=%u bps=%u spc=%u root=%u bitmap=%u (%u bytes)\n",
            dev->name, fs->cluster_count, fs->bytes_per_sector,
            fs->sectors_per_cluster, fs->root_cluster,
            fs->bitmap_cluster, (unsigned)fs->bitmap_size);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Module registration.                                                    */
/* ---------------------------------------------------------------------- */

static struct fs_type exfat_fs_type = {
    .name  = "exfat",
    .mount = exfat_mount,
    .next  = NULL,
};

static int exfat_module_init(void) {
    return vfs_register_fs(&exfat_fs_type);
}

MODULE("exfat", "fs", exfat_module_init);
