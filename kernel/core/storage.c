/* =============================================================================
 * storage.c — disk management: what each disk is, and the things you can do
 * to one (§M87).
 *
 * Asked for directly: *"disk management in the Control Panel, with its
 * functions."*  Until now storage was one decision made at boot — mount the
 * first exFAT disk at /mnt — plus `lsblk` for looking.  What a person expects
 * from disk management is the set this file provides, each with its refusal
 * spelled out:
 *
 *   LIST      every block device: size, what filesystem is on it (read from
 *             the disk, not assumed), where it is mounted, used and free space
 *   MOUNT     an exFAT disk at /media/<name> (or a path you give)
 *   UNMOUNT   refused while files are open on it, while a mount is nested in
 *             it, or while a subsystem depends on it — naming which
 *   FORMAT    a fresh exFAT filesystem; refused on a mounted disk (unmount
 *             first — formatting under a live mount is the one operation here
 *             that can destroy data nobody meant to touch)
 *   RAM DISK  create / remove a scratch disk (ramdisk.c says why it exists)
 *   SYNC      write every cached sector back now
 *
 * ONE QUERY (`storage_query`) feeds both the Control Panel's Disks page and
 * the `disk list` command, for the reason the device manager's `devices`
 * command walks the panel's own model: every automated check here is a grep
 * over a serial log, and a command that assembled the same facts by a second
 * route could pass while the panel showed something else.
 * ============================================================================= */

#include "storage.h"
#include "block.h"
#include "partition.h"
#include "block_cache.h"
#include "vfs.h"
#include "exfat.h"
#include "ramdisk.h"
#include "shellcmd.h"
#include "cmd_util.h"
#include "kmalloc.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

static int sq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

/* What is ON the disk, by signature.  READ, not assumed: a disk the boot path
 * did not mount is not therefore empty, and "unknown" and "empty" are
 * different answers (one is somebody's data in a format we do not speak, the
 * other is safe to format).  The table is the whole of it; a new filesystem is
 * a row. */
const char* storage_probe_fs(struct block_device* dev) {
    if (!dev || dev->sector_size != 512) return "unknown";
    uint8_t* b = (uint8_t*)kmalloc(1024);
    if (!b) return "unknown";
    const char* r = "unknown";
    if (blk_read(dev, 0, 1, b) != 0) { kfree(b); return "unreadable"; }
    int zero = 1;
    for (int i = 0; i < 512; i++) if (b[i]) { zero = 0; break; }
    if (b[3] == 'E' && b[4] == 'X' && b[5] == 'F' && b[6] == 'A' && b[7] == 'T')
        r = "exfat";
    else if (b[3] == 'N' && b[4] == 'T' && b[5] == 'F' && b[6] == 'S')
        r = "ntfs";
    else if (b[82] == 'F' && b[83] == 'A' && b[84] == 'T' && b[85] == '3' && b[86] == '2')
        r = "fat32";
    else if (b[54] == 'F' && b[55] == 'A' && b[56] == 'T')
        r = "fat";
    else if (b[510] == 0x55 && b[511] == 0xAA && !zero)
        r = "partitioned";                       /* an MBR: we do not parse it */
    else {
        /* ext2/3/4: magic 0xEF53 at byte 56 of the superblock at 1024. */
        if (blk_read(dev, 2, 1, b) == 0 && b[56] == 0x53 && b[57] == 0xEF) r = "ext";
        else if (zero) r = "empty";
    }
    kfree(b);
    return r;
}

struct sq_ctx { struct storage_info* out; int max, n; };

static void sq_one(struct block_device* d, void* vctx) {
    struct sq_ctx* c = (struct sq_ctx*)vctx;
    if (c->n >= c->max) return;
    struct storage_info* s = &c->out[c->n++];
    scopy(s->name, d->name, sizeof s->name);
    s->bytes     = d->sector_count * d->sector_size;
    s->removable = ramdisk_is(d->name);
    const struct vfs_mount* m = vfs_mount_of_dev(d->name);
    s->mounted   = m != NULL;
    scopy(s->mount, m ? m->path : "", sizeof s->mount);
    s->hold      = m ? m->hold : NULL;
    s->open_files = m ? m->open_files : 0;
    s->total = s->free = 0;
    s->have_space = m && vfs_statfs(m->path, &s->total, &s->free) == 0;
    /* A mounted disk's filesystem is the one it was mounted AS — reading the
     * boot sector under a live mount would go around the cache the fs is
     * writing through. */
    s->fs = m ? m->fs_name : storage_probe_fs(d);
}

int storage_query(struct storage_info* out, int max) {
    struct sq_ctx c = { out, max, 0 };
    blk_for_each(sq_one, &c);
    return c.n;
}

/* ----------------------- operations ---------------------------------------- */

const char* storage_error(int rc) {
    switch (rc) {
    case STOR_OK:          return "done";
    case STOR_ENODEV:      return "no such disk";
    case STOR_EMOUNTED:    return "the disk is mounted - unmount it first";
    case STOR_ENOTMOUNTED: return "the disk is not mounted";
    case STOR_EBUSY_OPEN:  return "files are open on it";
    case STOR_EBUSY_NEST:  return "another volume is mounted inside it";
    case STOR_EHELD:       return "the system depends on it";
    case STOR_ENOFS:       return "no filesystem this system can mount";
    case STOR_EIO:         return "I/O error";
    case STOR_ENOTREMOVABLE: return "only a RAM disk can be removed";
    case STOR_ENOMEM:      return "not enough memory";
    case STOR_ECANNOT:     return "this filesystem cannot be unmounted";
    }
    return "failed";
}

int storage_mount(const char* devname, const char* path) {
    struct block_device* d = blk_find(devname);
    if (!d) return STOR_ENODEV;
    if (vfs_mount_of_dev(devname)) return STOR_EMOUNTED;
    if (!sq(storage_probe_fs(d), "exfat")) return STOR_ENOFS;
    char p[64];
    if (path && path[0]) {
        scopy(p, path, sizeof p);
        vfs_mkdir(p);                               /* fine if it exists */
    } else {
        /* /media/<name>: the conventional place for a volume that is not the
         * system's own, and never /mnt, which the boot path owns. */
        vfs_mkdir("/media");
        int n = 0;
        const char* pre = "/media/";
        for (int i = 0; pre[i]; i++) p[n++] = pre[i];
        for (int i = 0; devname[i] && n < (int)sizeof p - 1; i++) p[n++] = devname[i];
        p[n] = 0;
        vfs_mkdir(p);
    }
    return vfs_mount("exfat", p, devname) == 0 ? STOR_OK : STOR_EIO;
}

int storage_umount(const char* dev_or_path) {
    const struct vfs_mount* m = vfs_mount_of_dev(dev_or_path);
    char path[64];
    if (m) scopy(path, m->path, sizeof path);
    else   scopy(path, dev_or_path, sizeof path);
    switch (vfs_umount(path)) {
    case 0:  return STOR_OK;
    case -1: return blk_find(dev_or_path) ? STOR_ENOTMOUNTED : STOR_ENODEV;
    case -2: return STOR_EBUSY_OPEN;
    case -3: return STOR_EBUSY_NEST;
    case -4: return STOR_EHELD;
    default: return STOR_ECANNOT;
    }
}

int storage_format(const char* devname, const char* label) {
    struct block_device* d = blk_find(devname);
    if (!d) return STOR_ENODEV;
    if (vfs_mount_of_dev(devname) || part_mounted(d)) return STOR_EMOUNTED;
    int r = exfat_format(d, label);
    /* A whole-disk volume over what was a partition table: the old partition
     * devices would keep pointing into the middle of the new filesystem. */
    if (r == 0 && !part_is(d)) part_forget(d);
    return r == 0 ? STOR_OK : r == -3 ? STOR_ENOMEM : STOR_EIO;
}

int storage_remove(const char* devname) {
    struct block_device* d = blk_find(devname);
    if (!d) return STOR_ENODEV;
    if (!ramdisk_is(devname)) return STOR_ENOTREMOVABLE;
    if (vfs_mount_of_dev(devname) || part_mounted(d)) return STOR_EMOUNTED;
    return ramdisk_destroy(devname) == 0 ? STOR_OK : STOR_EIO;
}

struct sync_ctx { int failed; };
static void sync_one(struct block_device* d, void* c) {
    if (d->write && bcache_sync(d) != 0) ((struct sync_ctx*)c)->failed++;
}
int storage_sync_all(void) {
    struct sync_ctx c = { 0 };
    blk_for_each(sync_one, &c);
    return c.failed ? STOR_EIO : STOR_OK;
}

/* ----------------------- `disk` -------------------------------------------- */

/* Sizes in the unit a person reads: KiB, MiB or GiB with one decimal. */
void storage_fmt_size(uint64_t b, char* out, int cap) {
    const char* unit = "KiB";
    uint64_t div = 1024;
    if (b >= (1ull << 30)) { unit = "GiB"; div = 1ull << 30; }
    else if (b >= (1ull << 20)) { unit = "MiB"; div = 1ull << 20; }
    uint64_t whole = b / div, tenth = (b % div) * 10 / div;
    char d[24]; int k = 0, n = 0;
    if (!whole) d[k++] = '0';
    while (whole && k < 20) { d[k++] = (char)('0' + whole % 10); whole /= 10; }
    while (k && n < cap - 1) out[n++] = d[--k];
    if (n < cap - 3) { out[n++] = '.'; out[n++] = (char)('0' + tenth); }
    if (n < cap - 1) out[n++] = ' ';
    for (int i = 0; unit[i] && n < cap - 1; i++) out[n++] = unit[i];
    out[n] = 0;
}

static void col(const char* s, int w) {
    int n = 0;
    for (; s[n]; n++) kprintf("%c", s[n]);
    for (; n < w; n++) kprintf(" ");
    kprintf(" ");
}

static void disk_list(void) {
    struct storage_info si[8];
    int n = storage_query(si, 8);
    col("DISK", 6); col("SIZE", 10); col("FS", 11); col("MOUNTED", 12);
    col("FREE", 20); kprintf("NOTE\n");
    for (int i = 0; i < n; i++) {
        char sz[16], fr[16], tt[16], line[40];
        storage_fmt_size(si[i].bytes, sz, sizeof sz);
        col(si[i].name, 6); col(sz, 10); col(si[i].fs, 11);
        col(si[i].mounted ? si[i].mount : "-", 12);
        if (si[i].have_space) {
            storage_fmt_size(si[i].free, fr, sizeof fr);
            storage_fmt_size(si[i].total, tt, sizeof tt);
            int k = 0;
            for (int j = 0; fr[j]; j++) line[k++] = fr[j];
            line[k++] = ' '; line[k++] = 'o'; line[k++] = 'f'; line[k++] = ' ';
            for (int j = 0; tt[j] && k < 38; j++) line[k++] = tt[j];
            line[k] = 0;
            col(line, 20);
        } else {
            col("-", 20);
        }
        if (si[i].hold) kprintf("used by %s", si[i].hold);
        else if (si[i].removable) kprintf("RAM disk");
        kprintf("\n");
    }
    if (!n) kprintf("no disks\n");
}

static void cmd_disk(const char* args) {
    char buf[128];
    int i = 0;
    for (; args && args[i] && i < (int)sizeof buf - 1; i++) buf[i] = args[i];
    buf[i] = 0;
    char* w[4] = { 0 };
    int n = 0;
    char* s = buf;
    while (*s && n < 4) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        w[n++] = s;
        while (*s && *s != ' ') s++;
    }
    int rc = -100;
    if (n == 0 || sq(w[0], "list"))       { disk_list(); return; }
    if (sq(w[0], "mount") && n >= 2)      rc = storage_mount(w[1], n >= 3 ? w[2] : NULL);
    else if (sq(w[0], "umount") && n >= 2) rc = storage_umount(w[1]);
    else if (sq(w[0], "format") && n >= 2) rc = storage_format(w[1], n >= 3 ? w[2] : "");
    else if (sq(w[0], "remove") && n >= 2) rc = storage_remove(w[1]);
    else if (sq(w[0], "partition") && n >= 3 && (sq(w[2], "gpt") || sq(w[2], "mbr"))) {
        /* One partition spanning the disk, then `disk format <dev>1`. */
        struct block_device* d = blk_find(w[1]);
        int r = d ? part_create_one(d, sq(w[2], "gpt")) : -1;
        kprintf("disk: partition: %s\n", r == 0 ? "done - format it with `disk format <dev>1`" :
                r == -2 ? "the disk or one of its partitions is mounted" :
                r == -3 ? "not a 512-byte-sector disk of at least 4 MiB" :
                !d ? "no such device" : "failed");
        return;
    }
    else if (sq(w[0], "sync"))             rc = storage_sync_all();
    else if (sq(w[0], "ramdisk") && n >= 2) {
        uint32_t mb = 0;
        cmd_parse_uint(w[1], &mb);
        const char* nm = ramdisk_create(mb);
        if (nm) kprintf("disk: created %s (%u MiB)\n", nm, mb);
        else    kprintf("disk: could not create a %u MiB RAM disk\n", mb);
        return;
    }
    if (rc == -100) {
        kprintf("usage: disk [list | mount <dev> [path] | umount <dev|path> |\n"
                "             format <dev> [label] | partition <dev> gpt|mbr |\n"
                "             ramdisk <MiB> | remove <dev> | sync]\n");
        return;
    }
    if (rc == STOR_EHELD) {
        const struct vfs_mount* m = vfs_mount_of_dev(w[1]);
        if (!m) m = vfs_mount_for(w[1]);
        kprintf("disk: %s: %s (%s)\n", w[0], storage_error(rc),
                m && m->hold ? m->hold : "?");
        return;
    }
    kprintf("disk: %s: %s\n", w[0], storage_error(rc));
}

SHELL_CMD(disk) = { "disk", "[list|mount|umount|format|ramdisk|remove|sync]",
                    "disk management: mount, unmount, format, RAM disks",
                    SHELL_G_FS, cmd_disk, SHELL_P_ADMIN };
