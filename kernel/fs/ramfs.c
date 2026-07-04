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

/* Per-file private data — capacity grows as needed; logical size lives
 * in `inode->size` so the VFS layer can print it without poking inside. */
struct ramfs_file {
    char*  data;
    size_t cap;
};

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
    if (type == INODE_DIR) {
        ino->ops     = &ramfs_dir_ops;
        ino->dir_ops = &ramfs_inode_ops;
        ino->private = NULL;
    } else if (type == INODE_FILE) {
        ino->ops = &ramfs_file_ops;
        struct ramfs_file* rf = (struct ramfs_file*)kcalloc(1, sizeof(*rf));
        if (!rf) { kfree(ino); return NULL; }
        rf->data = NULL;
        rf->cap  = 0;
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
    if (child->type == INODE_FILE) {
        struct ramfs_file* rf = (struct ramfs_file*)child->private;
        if (rf) {
            if (rf->data) kfree(rf->data);
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

static const struct inode_ops ramfs_inode_ops = {
    .lookup = NULL,                  /* eager tree */
    .create = rfs_create_op,
    .mkdir  = rfs_mkdir_op,
    .unlink = rfs_unlink_op,         /* M22.1 — file manager Delete */
    .rename = rfs_rename_op,         /* M22.5 — file manager Rename */
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
    memcpy_(buf, rf->data + off, take);
    return (ssize_t)take;
}

static ssize_t rfs_write(struct file* f, const void* buf, size_t n,
                         uint64_t off) {
    if (!f || !f->inode) return -1;
    struct ramfs_file* rf = (struct ramfs_file*)f->inode->private;
    if (!rf) return -1;

    /* ramfs lives entirely on the kernel heap (32-bit `size_t`).  A
     * write past the addressable range can't actually be served — clip
     * and let the caller see a short write. */
    if (off > (uint64_t)(size_t)-1) return 0;
    size_t off32  = (size_t)off;
    size_t needed = off32 + n;
    if (needed < off32) return -1;              /* size_t overflow */

    /* Grow buffer if we don't have room.  Doubling keeps writes amortized
     * O(1) and avoids the realloc-per-byte behavior a naive resize would
     * produce. */
    if (needed > rf->cap) {
        size_t new_cap = rf->cap ? rf->cap : 64;
        while (new_cap < needed) new_cap *= 2;
        char* nd = (char*)kmalloc(new_cap);
        if (!nd) return -1;
        if (rf->data) memcpy_(nd, rf->data, (size_t)f->inode->size);
        if (rf->data) kfree(rf->data);
        rf->data = nd;
        rf->cap  = new_cap;
    }

    memcpy_(rf->data + off32, buf, n);
    uint64_t end = off + (uint64_t)n;
    if (end > f->inode->size) f->inode->size = end;
    return (ssize_t)n;
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
