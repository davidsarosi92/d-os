/* =============================================================================
 * ramfs.c — in-memory filesystem.
 *
 * The simplest fs that's still useful: every inode and every byte of
 * file content lives on the kernel heap.  Survives nothing beyond a
 * reboot, but lets us validate the VFS and back the config store (M5)
 * without committing to a persistent format.
 *
 * Layout per inode:
 *   - directories: `inode->private == NULL`.  Children come from the
 *     dentry tree (`dentry->children`); ramfs_readdir walks it.
 *   - files: `inode->private` points at a `struct ramfs_file` carrying
 *     a kmalloc'd content buffer with a grow-on-write policy.
 *
 * After the M12 VFS refactor:
 *   - `file_ops.read/write` are random-access — they ignore `f->pos`
 *     (which the VFS layer manages) and read/write at the given `off`.
 *   - `inode_ops` (create/mkdir) replace the old `extern ramfs_create_in`
 *     escape hatch from vfs.c.  Both vfs_create / vfs_mkdir and the
 *     mount-time bootstrap call into them by going through the inode.
 *   - ramfs has an eager dentry tree, so `lookup` is NULL — `vfs.c`
 *     never asks us to materialize a missing child.
 *
 * Concurrency: like everywhere else, single-threaded; no locks.
 * ============================================================================= */

#include "vfs.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>
#include "module.h"

/* Per-file private data; the logical size lives in `inode->size` so the VFS
 * layer can print it without poking inside.
 *
 * THE CONTENT IS A TABLE OF 4 KiB PAGES, NOT ONE BUFFER (§M89).  It used to be
 * a single kmalloc'd block that doubled on growth, which quietly capped a file
 * at the largest CONTIGUOUS block the heap can hand out (16 MiB, the buddy
 * ceiling) and needed old+new at once to grow — a Java runtime's 101 MB
 * `lib/modules` could not be created at all, and the write simply failed.
 * Pages have neither limit: only the pointer table is contiguous (8 bytes per
 * 4 KiB), and growing it copies pointers, not content.
 *
 * A NULL page is a HOLE and reads as zeros, so a file extended past its end
 * by a seek costs nothing for the gap. */
#define RFS_PAGE 4096u
struct ramfs_file {
    char** pages;      /* pages[i] holds bytes [i*RFS_PAGE, (i+1)*RFS_PAGE)   */
    size_t npages;     /* slots in `pages` (capacity of the table)             */
    int    nlink;      /* §M73 — names pointing at this file; freed at 0 */
};

static void rfs_free_pages(struct ramfs_file* rf) {
    if (!rf->pages) return;
    for (size_t i = 0; i < rf->npages; i++) if (rf->pages[i]) kfree(rf->pages[i]);
    kfree(rf->pages);
    rf->pages = NULL;
    rf->npages = 0;
}

/* ------------------------------------------------------------------- */
/* Memory helpers (no libc).                                            */
/* ------------------------------------------------------------------- */

static void memcpy_(void* dst, const void* src, size_t n) {
    char* d = (char*)dst; const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}

/* ------------------------------------------------------------------- */
/* Forward decls of file_ops / inode_ops tables (defined below).        */
/* ------------------------------------------------------------------- */

static const struct file_ops  ramfs_file_ops;
static const struct file_ops  ramfs_dir_ops;
static const struct inode_ops ramfs_inode_ops;

/* ------------------------------------------------------------------- */
/* Inode creation — the workhorse used by mkdir / create / mount-time   */
/* bootstrap.                                                            */
/* ------------------------------------------------------------------- */

static struct inode* ramfs_alloc_inode(enum inode_type type) {
    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!ino) return NULL;
    ino->type = type;
    ino->size = 0;
    vfs_inode_defaults(ino);   /* §M32 — ONE initialiser, every site */
    if (type == INODE_DIR) {
        ino->ops     = &ramfs_dir_ops;
        ino->dir_ops = &ramfs_inode_ops;
        ino->private = NULL;
    } else if (type == INODE_FILE) {
        ino->ops = &ramfs_file_ops;
        struct ramfs_file* rf = (struct ramfs_file*)kcalloc(1, sizeof(*rf));
        if (!rf) { kfree(ino); return NULL; }
        rf->pages  = NULL;
        rf->npages = 0;
        rf->nlink = 1;
        ino->private = rf;
    }
    return ino;
}

/* ------------------------------------------------------------------- */
/* inode_ops — directory mutators.                                      */
/*                                                                      */
/* `lookup` is NULL: ramfs builds its dentry tree eagerly, so a cache   */
/* miss in vfs_lookup_child means the name truly does not exist.        */
/* ------------------------------------------------------------------- */

static int rfs_create_op(struct inode* dir, const char* name,
                         struct inode** out) {
    (void)dir; (void)name;          /* dedup happens in vfs.c */
    struct inode* ino = ramfs_alloc_inode(INODE_FILE);
    if (!ino) return -1;
    *out = ino;
    return 0;
}

static int rfs_mkdir_op(struct inode* dir, const char* name,
                        struct inode** out) {
    (void)dir; (void)name;
    struct inode* ino = ramfs_alloc_inode(INODE_DIR);
    if (!ino) return -1;
    *out = ino;
    return 0;
}

static int rfs_unlink_op(struct inode* dir, const char* name,
                         struct inode* child) {
    (void)dir; (void)name;           /* emptiness checked by vfs_unlink */
    if (!child) return -1;
    if (child->type == INODE_FILE || child->type == INODE_SYMLINK) {
        struct ramfs_file* rf = (struct ramfs_file*)child->private;
        /* §M73 — another name still points here: only this one goes. */
        if (rf && rf->nlink > 1) { rf->nlink--; return 0; }
        if (rf) {
            rfs_free_pages(rf);
            kfree(rf);
        }
    }
    kfree(child);
    return 0;
}

static int rfs_rename_op(struct inode* dir, const char* oldname,
                         const char* newname, struct inode* child) {
    (void)dir; (void)oldname; (void)newname;
    /* ramfs keeps no private name index — names live in the VFS
     * dentry tree, which vfs_rename rewrites after we say yes. */
    return child ? 0 : -1;
}

static int rfs_link_op(struct inode* dir, const char* name, struct inode* target) {
    (void)dir; (void)name;
    if (!target || target->type != INODE_FILE) return -1;
    struct ramfs_file* rf = (struct ramfs_file*)target->private;
    if (!rf) return -1;
    rf->nlink++;
    return 0;
}

static ssize_t rfs_write(struct file* f, const void* buf, size_t n, uint64_t off);

/* §M89 — a symbolic link is a small file holding its target, typed so the
 * VFS knows to follow it.  Written through the ordinary write path, so its
 * storage and its freeing are exactly a file's. */
static int rfs_symlink_op(struct inode* dir, const char* name, const char* target,
                          struct inode** out) {
    (void)dir; (void)name;
    struct inode* ino = ramfs_alloc_inode(INODE_FILE);
    if (!ino) return -1;
    size_t n = 0;
    while (target[n]) n++;
    struct file tmp = { ino, NULL, 0, 0, 0 };
    if (rfs_write(&tmp, target, n, 0) != (ssize_t)n) {
        struct ramfs_file* rf = (struct ramfs_file*)ino->private;
        rfs_free_pages(rf); kfree(rf); kfree(ino);
        return -1;
    }
    ino->type = INODE_SYMLINK;
    *out = ino;
    return 0;
}

static const struct inode_ops ramfs_inode_ops = {
    .lookup = NULL,                  /* eager tree */
    .create = rfs_create_op,
    .mkdir  = rfs_mkdir_op,
    .unlink = rfs_unlink_op,         /* M22.1 — file manager Delete */
    .rename = rfs_rename_op,         /* M22.5 — file manager Rename */
    .link   = rfs_link_op,           /* §M73 — hard links */
    .symlink = rfs_symlink_op,       /* §M89 — symbolic links */
};

/* ------------------------------------------------------------------- */
/* file_ops — regular files.                                            */
/* ------------------------------------------------------------------- */

static ssize_t rfs_read(struct file* f, void* buf, size_t n, uint64_t off) {
    if (!f || !f->inode) return -1;
    struct ramfs_file* rf = (struct ramfs_file*)f->inode->private;
    if (!rf) return -1;
    if (off >= f->inode->size) return 0;        /* EOF */
    uint64_t avail = f->inode->size - off;
    size_t take = n < avail ? n : (size_t)avail;
    char* out = (char*)buf;
    size_t done = 0;
    while (done < take) {
        uint64_t pos = off + done;
        size_t pi = (size_t)(pos / RFS_PAGE), po = (size_t)(pos % RFS_PAGE);
        size_t chunk = RFS_PAGE - po;
        if (chunk > take - done) chunk = take - done;
        const char* pg = pi < rf->npages ? rf->pages[pi] : NULL;
        if (pg) memcpy_(out + done, pg + po, chunk);
        else    for (size_t i = 0; i < chunk; i++) out[done + i] = 0;   /* a hole */
        done += chunk;
    }
    return (ssize_t)take;
}

/* Make the page table hold at least `need` slots.  Doubling keeps growth
 * amortised O(1); what is copied is pointers, never content. */
static int rfs_reserve(struct ramfs_file* rf, size_t need) {
    if (need <= rf->npages) return 0;
    size_t ncap = rf->npages ? rf->npages : 4;
    while (ncap < need) ncap *= 2;
    char** np = (char**)kcalloc(ncap, sizeof(char*));
    if (!np) return -1;
    for (size_t i = 0; i < rf->npages; i++) np[i] = rf->pages[i];
    if (rf->pages) kfree(rf->pages);
    rf->pages = np;
    rf->npages = ncap;
    return 0;
}

/* Zero [from, to) where pages exist — the bytes a truncate left behind.
 * O_TRUNC only resets the size (vfs.c), so a later write beyond the new end
 * would otherwise expose the old content in the gap. */
static void rfs_zero_range(struct ramfs_file* rf, uint64_t from, uint64_t to) {
    while (from < to) {
        size_t pi = (size_t)(from / RFS_PAGE), po = (size_t)(from % RFS_PAGE);
        size_t chunk = RFS_PAGE - po;
        if (chunk > to - from) chunk = (size_t)(to - from);
        if (pi >= rf->npages) return;
        char* pg = rf->pages[pi];
        if (pg) for (size_t i = 0; i < chunk; i++) pg[po + i] = 0;
        from += chunk;
    }
}

static ssize_t rfs_write(struct file* f, const void* buf, size_t n,
                         uint64_t off) {
    if (!f || !f->inode) return -1;
    struct ramfs_file* rf = (struct ramfs_file*)f->inode->private;
    if (!rf) return -1;
    if (n == 0) return 0;

    /* ramfs lives on the kernel heap; its page index is a size_t. */
    uint64_t end = off + (uint64_t)n;
    if (end < off || end / RFS_PAGE >= (uint64_t)((size_t)-1 / sizeof(char*))) return -1;
    if (rfs_reserve(rf, (size_t)((end + RFS_PAGE - 1) / RFS_PAGE)) != 0) return -1;
    if (off > f->inode->size) rfs_zero_range(rf, f->inode->size, off);

    const char* in = (const char*)buf;
    size_t done = 0;
    while (done < n) {
        uint64_t pos = off + done;
        size_t pi = (size_t)(pos / RFS_PAGE), po = (size_t)(pos % RFS_PAGE);
        size_t chunk = RFS_PAGE - po;
        if (chunk > n - done) chunk = n - done;
        if (!rf->pages[pi]) {
            char* pg = (char*)kcalloc(1, RFS_PAGE);   /* zeroed: the rest of a new page reads as 0 */
            if (!pg) {
                /* Out of memory part way: report what landed, as a short write. */
                if (done == 0) return -1;
                break;
            }
            rf->pages[pi] = pg;
        }
        memcpy_(rf->pages[pi] + po, in + done, chunk);
        done += chunk;
    }
    uint64_t newend = off + (uint64_t)done;
    if (newend > f->inode->size) f->inode->size = newend;
    return (ssize_t)done;
}

static int rfs_close(struct file* f) {
    (void)f;
    /* Nothing to do — buffers stick to the inode for the file's lifetime. */
    return 0;
}

static const struct file_ops ramfs_file_ops = {
    .read    = rfs_read,
    .write   = rfs_write,
    .readdir = NULL,
    .close   = rfs_close,
};

/* ------------------------------------------------------------------- */
/* file_ops — directories.                                              */
/*                                                                      */
/* Iteration uses `f->pos` as a 0-based child index.  Stable across     */
/* readdir calls as long as no entries are added/removed mid-iteration. */
/* ------------------------------------------------------------------- */

static int rfs_readdir(struct file* f, struct dirent* out) {
    if (!f || !f->dentry || !out) return -1;
    struct dentry* c = f->dentry->children;
    uint64_t target = f->pos;
    for (uint64_t i = 0; c && i < target; i++) c = c->sibling;
    if (!c) return 0;                           /* end of directory */

    /* Fill the dirent.  Walk the name to copy because we don't have
     * libc strncpy. */
    size_t i = 0;
    while (i < sizeof(out->name) - 1 && c->name[i]) {
        out->name[i] = c->name[i];
        i++;
    }
    out->name[i] = 0;
    out->type = c->inode ? c->inode->type : INODE_FILE;
    out->size = c->inode ? c->inode->size : 0;

    f->pos++;
    return 1;                                   /* one entry returned */
}

static const struct file_ops ramfs_dir_ops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = rfs_readdir,
    .close   = rfs_close,
};

/* ------------------------------------------------------------------- */
/* Mount + module registration.                                         */
/* ------------------------------------------------------------------- */

/* Helper: synthesize a child via the dir_ops + attach via vfs.  Used at
 * mount time to populate the canonical top-level directories without
 * going through path resolution. */
static void bootstrap_child(struct dentry* parent, const char* name,
                            enum inode_type type) {
    struct inode* ino = ramfs_alloc_inode(type);
    if (!ino) return;
    if (!vfs_attach_child(parent, name, ino)) {
        if (ino->private) kfree(ino->private);
        kfree(ino);
    }
}

static int ramfs_mount(struct block_device* dev, struct dentry* mp) {
    (void)dev;                                  /* in-memory; no backing */

    /* Mountpoint must not already have an inode (root mounts onto a
     * blank dentry; nested mounts onto an existing empty directory). */
    if (mp->inode) return -1;

    struct inode* root_inode = ramfs_alloc_inode(INODE_DIR);
    if (!root_inode) return -2;
    mp->inode = root_inode;

    /* Pre-create the canonical top-level directories so the rest of the
     * kernel has a sensible namespace from the start.  Tiny, but it's
     * the difference between `ls /` showing nothing and looking dead vs.
     * showing `etc/  dev/  tmp/  proc/  mnt/` and feeling alive. */
    bootstrap_child(mp, "etc",  INODE_DIR);
    bootstrap_child(mp, "dev",  INODE_DIR);
    bootstrap_child(mp, "tmp",  INODE_DIR);
    bootstrap_child(mp, "proc", INODE_DIR);
    bootstrap_child(mp, "mnt",  INODE_DIR);     /* future external mounts (exFAT, ...) */

    return 0;
}

static struct fs_type ramfs_fs_type = {
    .name  = "ramfs",
    .mount = ramfs_mount,
    .next  = NULL,
};

static int ramfs_module_init(void) {
    if (vfs_register_fs(&ramfs_fs_type) != 0) return -1;
    return vfs_mount("ramfs", "/", NULL);
}

MODULE("ramfs", "fs", ramfs_module_init);
