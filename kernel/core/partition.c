/* =============================================================================
 * partition.c — MBR and GPT partition tables (see partition.h).
 *
 * Arch-independent: it speaks to disks only through `struct block_device`.
 * Sector size 512 is assumed for the TABLE (both formats define their offsets
 * in 512-byte sectors on such disks); a disk with another sector size is left
 * whole and says so, rather than being parsed with the wrong arithmetic.
 * ============================================================================= */

#include "partition.h"
#include "block.h"
#include "block_cache.h"
#include "kmalloc.h"
#include "printf.h"
#include "random.h"
#include "vfs.h"
#include "lock.h"
#include "shellcmd.h"
#include <stdint.h>
#include <stddef.h>

#define PART_MAX 32
#define SEC      512u

struct part {
    struct block_device  bd;
    struct block_device* disk;
    uint64_t             start;          /* first sector on the disk        */
    int                  used;
    int                  num;            /* 1.. (logical MBR: 5..)          */
    char                 name[16];
    char                 type[24];       /* human description              */
    char                 label[40];      /* GPT name (ASCII part), or ""   */
};
static struct part g_parts[PART_MAX];
static spinlock_t  g_parts_lock = SPINLOCK_INIT;

/* ---- little-endian helpers ------------------------------------------------ */
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* CRC-32 (IEEE, reflected) — what GPT checksums its header and entries with.
 * Bitwise rather than a table: a table is 1 KiB of data for a function called
 * a handful of times per disk. */
static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

/* ---- the partition device ------------------------------------------------- */

static int p_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buf) {
    struct part* p = (struct part*)dev->priv;
    if (lba >= dev->sector_count || count > dev->sector_count - lba) return -1;
    return p->disk->read(p->disk, p->start + lba, count, buf);
}
static int p_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buf) {
    struct part* p = (struct part*)dev->priv;
    if (!p->disk->write) return -1;
    if (lba >= dev->sector_count || count > dev->sector_count - lba) return -1;
    return p->disk->write(p->disk, p->start + lba, count, buf);
}
static int p_flush(struct block_device* dev) {
    struct part* p = (struct part*)dev->priv;
    return p->disk->flush ? p->disk->flush(p->disk) : 0;
}

int part_is(const struct block_device* dev) { return dev && dev->read == p_read; }
struct block_device* part_parent(const struct block_device* dev) {
    return part_is(dev) ? ((struct part*)dev->priv)->disk : NULL;
}

static void str_put(char* dst, int cap, const char* src) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static int add_part(struct block_device* disk, int num, uint64_t start, uint64_t count,
                    const char* type, const char* label) {
    if (count == 0 || start >= disk->sector_count || count > disk->sector_count - start) {
        kprintf("part: %s partition %d (%u+%u) lies outside the disk - ignored\n",
                disk->name, num, (unsigned)start, (unsigned)count);
        return -1;
    }
    struct part* p = NULL;
    uint32_t fl = spin_lock_irqsave(&g_parts_lock);
    for (int i = 0; i < PART_MAX; i++) if (!g_parts[i].used) { p = &g_parts[i]; p->used = 1; break; }
    spin_unlock_irqrestore(&g_parts_lock, fl);
    if (!p) { kprintf("part: more than %d partitions - %s%d ignored\n", PART_MAX, disk->name, num); return -1; }

    int n = 0;
    for (; disk->name[n] && n < 10; n++) p->name[n] = disk->name[n];
    if (n && p->name[n - 1] >= '0' && p->name[n - 1] <= '9') p->name[n++] = 'p';
    if (num >= 10) p->name[n++] = (char)('0' + num / 10);
    p->name[n++] = (char)('0' + num % 10);
    p->name[n] = 0;
    p->disk = disk; p->start = start; p->num = num;
    str_put(p->type, sizeof p->type, type);
    str_put(p->label, sizeof p->label, label);
    p->bd = (struct block_device){ .name = p->name, .sector_size = disk->sector_size,
                                   .sector_count = count, .read = p_read,
                                   .write = disk->write ? p_write : NULL,
                                   .flush = p_flush, .priv = p };
    if (blk_register(&p->bd) != 0) { p->used = 0; return -1; }
    int hl = label && label[0];
    kprintf("part: %s = %s sectors %u..%u (%u MiB) %s%s%s%s\n", p->name, disk->name,
            (unsigned)start, (unsigned)(start + count - 1), (unsigned)(count / 2048),
            type, hl ? " '" : "", hl ? label : "", hl ? "'" : "");
    return 0;
}

/* ---- MBR ------------------------------------------------------------------ */

static const char* mbr_type(uint8_t t) {
    switch (t) {
    case 0x07: return "exFAT/NTFS";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x0E: case 0x06: return "FAT16";
    case 0x83: return "Linux";
    case 0x82: return "Linux swap";
    case 0xEF: return "EFI system";
    default:   return "MBR";
    }
}

static int is_extended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static int scan_mbr(struct block_device* disk, const uint8_t* s0, uint8_t* buf) {
    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* e = s0 + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t st = le32(e + 8), cnt = le32(e + 12);
        if (!type || !cnt) continue;
        if (!is_extended(type)) {
            if (add_part(disk, i + 1, st, cnt, mbr_type(type), "") == 0) found++;
            continue;
        }
        /* The extended partition: a chain of EBRs, each holding one logical
         * partition (relative to that EBR) and a link to the next (relative to
         * the extended partition's start).  Bounded, because a chain that loops
         * would otherwise hang the boot. */
        uint64_t ext = st, ebr = st;
        for (int k = 0, num = 5; k < 64; k++) {
            if (ebr >= disk->sector_count || disk->read(disk, ebr, 1, buf) != 0) break;
            if (buf[510] != 0x55 || buf[511] != 0xAA) break;
            const uint8_t* l = buf + 446;
            if (l[4] && le32(l + 12))
                if (add_part(disk, num++, ebr + le32(l + 8), le32(l + 12), mbr_type(l[4]), "") == 0) found++;
            const uint8_t* nx = buf + 446 + 16;
            if (!is_extended(nx[4]) || !le32(nx + 8)) break;
            ebr = ext + le32(nx + 8);
        }
    }
    return found;
}

/* ---- GPT ------------------------------------------------------------------ */

static const uint8_t GUID_BASIC_DATA[16] = { 0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };
static const uint8_t GUID_EFI_SYS[16]    = { 0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t GUID_LINUX_FS[16]   = { 0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47, 0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };

static int guid_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static const char* gpt_type(const uint8_t* g) {
    if (guid_eq(g, GUID_BASIC_DATA)) return "basic data";
    if (guid_eq(g, GUID_EFI_SYS))    return "EFI system";
    if (guid_eq(g, GUID_LINUX_FS))   return "Linux";
    return "GPT";
}

/* Validate the header at `lba` (into hdr) and its entry array (into *ents,
 * kmalloc'd).  0 when both CRCs hold. */
static int gpt_load(struct block_device* disk, uint64_t lba, uint8_t* hdr, uint8_t** ents,
                    uint32_t* n_out, uint32_t* esz_out) {
    *ents = NULL;
    if (lba >= disk->sector_count || disk->read(disk, lba, 1, hdr) != 0) return -1;
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (hdr[i] != (uint8_t)sig[i]) return -2;
    uint32_t hsz = le32(hdr + 12);
    if (hsz < 92 || hsz > SEC) return -3;
    uint32_t want = le32(hdr + 16);
    uint8_t tmp[SEC];
    for (uint32_t i = 0; i < hsz; i++) tmp[i] = hdr[i];
    put32(tmp + 16, 0);
    if (crc32(tmp, hsz) != want) return -4;
    uint64_t elba = le64(hdr + 72);
    uint32_t n = le32(hdr + 80), esz = le32(hdr + 84);
    if (esz < 128 || esz > 1024 || n == 0 || n > 1024) return -5;
    uint32_t bytes = n * esz, secs = (bytes + SEC - 1) / SEC;
    uint8_t* e = (uint8_t*)kmalloc((size_t)secs * SEC);
    if (!e) return -6;
    if (disk->read(disk, elba, secs, e) != 0) { kfree(e); return -7; }
    if (crc32(e, bytes) != le32(hdr + 88)) { kfree(e); return -8; }
    *ents = e; *n_out = n; *esz_out = esz;
    return 0;
}

static int scan_gpt(struct block_device* disk) {
    uint8_t hdr[SEC];
    uint8_t* ents = NULL;
    uint32_t n = 0, esz = 0;
    int rc = gpt_load(disk, 1, hdr, &ents, &n, &esz);
    if (rc != 0) {
        int rb = gpt_load(disk, disk->sector_count - 1, hdr, &ents, &n, &esz);
        if (rb != 0) {
            kprintf("part: %s has a protective MBR but no valid GPT (primary %d, "
                    "backup %d) - left whole\n", disk->name, rc, rb);
            return 0;
        }
        kprintf("part: %s's primary GPT is damaged (%d) - using the BACKUP\n", disk->name, rc);
    }
    int found = 0;
    for (uint32_t i = 0; i < n && i < 99; i++) {
        const uint8_t* e = ents + (size_t)i * esz;
        int empty = 1;
        for (int k = 0; k < 16; k++) if (e[k]) { empty = 0; break; }
        if (empty) continue;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (last < first) continue;
        char label[40];
        int m = 0;
        for (int k = 0; k < 36 && m < 39; k++) {           /* UTF-16LE, ASCII part */
            uint16_t ch = (uint16_t)(e[56 + 2 * k] | e[57 + 2 * k] << 8);
            if (!ch) break;
            label[m++] = (ch < 0x80 && ch >= 0x20) ? (char)ch : '?';
        }
        label[m] = 0;
        if (add_part(disk, (int)i + 1, first, last - first + 1, gpt_type(e), label) == 0) found++;
    }
    kfree(ents);
    return found;
}

int part_scan(struct block_device* disk) {
    if (!disk || part_is(disk) || !disk->read) return 0;
    if (disk->sector_size != SEC || disk->sector_count < 64) return 0;
    uint8_t* s0 = (uint8_t*)kmalloc(2 * SEC);
    if (!s0) return 0;
    int found = 0;
    if (disk->read(disk, 0, 1, s0) == 0 && s0[510] == 0x55 && s0[511] == 0xAA) {
        /* An exFAT / FAT boot sector ALSO ends in 55 AA.  It is told apart by
         * its file-system signature, not by guessing at the table bytes — a
         * whole-disk volume read as an MBR would produce four garbage
         * partitions, and mounting one of them is a write into the middle of
         * a filesystem. */
        int fs = (s0[3] == 'E' && s0[4] == 'X' && s0[5] == 'F' && s0[6] == 'A' && s0[7] == 'T') ||
                 (s0[82] == 'F' && s0[83] == 'A' && s0[84] == 'T') ||
                 (s0[54] == 'F' && s0[55] == 'A' && s0[56] == 'T');
        int prot = 0;
        for (int i = 0; i < 4; i++) if (s0[446 + i * 16 + 4] == 0xEE) prot = 1;
        if (prot)       found = scan_gpt(disk);
        else if (!fs)   found = scan_mbr(disk, s0, s0 + SEC);
    }
    kfree(s0);
    return found;
}

void part_forget(struct block_device* disk) {
    for (int i = 0; i < PART_MAX; i++) {
        struct part* p = &g_parts[i];
        if (!p->used || p->disk != disk) continue;
        blk_unregister(&p->bd);
        p->used = 0;
    }
}

/* ---- writing a table ------------------------------------------------------ */

static int busy(struct block_device* disk);

int part_mounted(struct block_device* disk) { return busy(disk); }
static int busy(struct block_device* disk) {
    if (vfs_mount_of_dev(disk->name)) return 1;
    for (int i = 0; i < PART_MAX; i++)
        if (g_parts[i].used && g_parts[i].disk == disk && vfs_mount_of_dev(g_parts[i].name))
            return 1;
    return 0;
}

static int wr(struct block_device* d, uint64_t lba, uint32_t n, const void* b) {
    return d->write(d, lba, n, b);
}

int part_create_one(struct block_device* disk, int gpt) {
    if (!disk || part_is(disk) || !disk->write) return -1;
    if (disk->sector_size != SEC || disk->sector_count < 8192) return -3;
    if (busy(disk)) return -2;

    uint64_t total = disk->sector_count;
    uint64_t start = 2048;                                      /* 1 MiB aligned */
    uint8_t* b = (uint8_t*)kmalloc(34 * SEC);
    if (!b) return -4;
    for (uint32_t i = 0; i < 34 * SEC; i++) b[i] = 0;

    if (!gpt) {
        uint64_t cnt = total - start;
        if (cnt > 0xFFFFFFFFull) cnt = 0xFFFFFFFFull;
        uint8_t* e = b + 446;
        e[0] = 0x00; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;     /* CHS "use LBA" */
        e[4] = 0x07;                                            /* exFAT          */
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        put32(e + 8, (uint32_t)start); put32(e + 12, (uint32_t)cnt);
        put32(b + 440, random_u32());                           /* disk signature */
        b[510] = 0x55; b[511] = 0xAA;
        if (wr(disk, 0, 1, b) != 0) { kfree(b); return -5; }
    } else {
        /* Layout: LBA 0 protective MBR, 1 header, 2..33 entries (128 x 128 B);
         * the backup entries at total-33..total-2 and the backup header at the
         * last sector.  Usable space 34..total-34; the partition starts at
         * 1 MiB and ends on a 1 MiB boundary. */
        uint64_t last_usable = total - 34;
        uint64_t end = ((last_usable + 1) / 2048) * 2048 - 1;
        uint8_t* ents = b + 2 * SEC;                            /* 32 sectors     */
        for (int k = 0; k < 16; k++) ents[k] = GUID_BASIC_DATA[k];
        random_bytes(ents + 16, 16);
        ents[16 + 7] = (uint8_t)((ents[16 + 7] & 0x0F) | 0x40);  /* v4 GUID       */
        ents[16 + 8] = (uint8_t)((ents[16 + 8] & 0x3F) | 0x80);
        put64(ents + 32, start); put64(ents + 40, end);
        const char* nm = "d-os data";
        for (int k = 0; nm[k]; k++) ents[56 + 2 * k] = (uint8_t)nm[k];
        uint32_t ecrc = crc32(ents, 128 * 128);

        uint8_t* h = b + SEC;
        const char* sig = "EFI PART";
        for (int k = 0; k < 8; k++) h[k] = (uint8_t)sig[k];
        put32(h + 8, 0x00010000u); put32(h + 12, 92);
        put64(h + 24, 1); put64(h + 32, total - 1);
        put64(h + 40, 34); put64(h + 48, last_usable);
        random_bytes(h + 56, 16);
        h[56 + 7] = (uint8_t)((h[56 + 7] & 0x0F) | 0x40);
        h[56 + 8] = (uint8_t)((h[56 + 8] & 0x3F) | 0x80);
        put64(h + 72, 2); put32(h + 80, 128); put32(h + 84, 128); put32(h + 88, ecrc);
        put32(h + 16, crc32(h, 92));

        uint8_t* m = b;                                         /* protective MBR */
        uint8_t* e = m + 446;
        e[1] = 0x00; e[2] = 0x02; e[3] = 0x00; e[4] = 0xEE; e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
        put32(e + 8, 1);
        put32(e + 12, total - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(total - 1));
        m[510] = 0x55; m[511] = 0xAA;

        if (wr(disk, 0, 34, b) != 0) { kfree(b); return -5; }

        /* The backup: the same entries, a header that names itself. */
        uint8_t bh[SEC];
        for (int k = 0; k < (int)SEC; k++) bh[k] = h[k];
        put64(bh + 24, total - 1); put64(bh + 32, 1); put64(bh + 72, total - 33);
        put32(bh + 16, 0); put32(bh + 16, crc32(bh, 92));
        if (wr(disk, total - 33, 32, ents) != 0 || wr(disk, total - 1, 1, bh) != 0) {
            kfree(b); return -5;
        }
    }
    kfree(b);
    if (disk->flush) disk->flush(disk);

    part_forget(disk);
    bcache_invalidate(disk);
    int n = part_scan(disk);
    kprintf("part: %s now has a %s table with %d partition(s)\n", disk->name,
            gpt ? "GPT" : "MBR", n);
    return n == 1 ? 0 : -6;
}

/* ---- `partitions` --------------------------------------------------------- */

static void cmd_partitions(const char* args) {
    (void)args;
    int any = 0;
    for (int i = 0; i < PART_MAX; i++) {
        struct part* p = &g_parts[i];
        if (!p->used) continue;
        any = 1;
        kprintf("  %s  on %s  start %u  %u MiB  %s%s%s%s\n", p->name, p->disk->name,
                (unsigned)p->start, (unsigned)(p->bd.sector_count / 2048), p->type,
                p->label[0] ? "  '" : "", p->label, p->label[0] ? "'" : "");
    }
    if (!any) kprintf("  no partitioned disks (whole-disk volumes are listed by `disk list`)\n");
}
SHELL_CMD(partitions) = { "partitions", "", "partition tables found on the disks",
                          SHELL_G_FS, cmd_partitions, SHELL_P_ANY };
