/* =============================================================================
 * vfs.c — VFS core.
 *
 * Owns the root dentry, the registered-filesystem list, and path
 * resolution.  Concrete filesystems (ramfs, exfat, ...) plug in via
 * `vfs_register_fs` and provide their own `file_ops` per inode and
 * `inode_ops` per directory inode.
 *
 * Path resolution is a component-by-component walk through the dentry
 * tree.  Each step first looks in the cache (`parent->children`); on
 * miss, if the parent inode has `dir_ops->lookup`, the fs is asked to
 * resolve the name and (on success) the result is attached to the
 * cache.  Filesystems with eager dentry trees (ramfs, devfs, procfs)
 * never see a lookup call; lazy filesystems (exFAT) populate their
 * children incrementally.
 *
 * Pos ownership: `f->pos` lives in the VFS layer.  `vfs_read`/`vfs_write`
 * pass the current value to `file_ops.read/write` as the `off`
 * parameter and bump `f->pos` by the non-negative return value.  Fs
 * implementations are pure offset-addressed and never touch `f->pos`.
 *
 * Future revisions will add `..` / `.` resolution and non-root mount
 * points (the framework is here, but only "/" is exercised today).
 * ============================================================================= */

#include "vfs.h"
#include "block.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>

/* ------------------------------------------------------------------- */
/* Module state.                                                        */
/* ------------------------------------------------------------------- */

static struct fs_type* fs_types = NULL;
static struct dentry*  root     = NULL;

struct dentry* vfs_root(void) { return root; }

/* ------------------------------------------------------------------- */
/* String helpers — no libc.                                            */
/* ------------------------------------------------------------------- */

static int streq_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 1;
    }
    return 1;
}
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t strlen_(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static void strcpy_n(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void memcpy_(void* dst, const void* src, size_t n) {
    char* d = (char*)dst; const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}

/* ------------------------------------------------------------------- */
/* Init + fs registry.                                                  */
/* ------------------------------------------------------------------- */

void vfs_init(void) {
    if (root) return;                           /* idempotent */

    root = (struct dentry*)kcalloc(1, sizeof(struct dentry));
    if (!root) {
        kprintf("vfs: failed to allocate root dentry\n");
        return;
    }
    /* "/" by convention; parent NULL distinguishes the root. */
    root->name[0] = '/';
    root->name[1] = 0;
    root->parent  = NULL;
    root->inode   = NULL;       /* filled in by first mount */

    kprintf("vfs: initialized\n");
}

int vfs_register_fs(struct fs_type* fs) {
    if (!fs || !fs->name || !fs->mount) return -1;
    /* Refuse duplicate names. */
    for (struct fs_type* p = fs_types; p; p = p->next) {
        if (streq(p->name, fs->name)) return -2;
    }
    fs->next = fs_types;
    fs_types = fs;
    return 0;
}

/* ------------------------------------------------------------------- */
/* Path resolution.                                                     */
/* ------------------------------------------------------------------- */

/* Look up a single name component as a child of `parent`.  Returns the
 * matching dentry or NULL.  `name_len` excludes the trailing NUL/'/'.
 *
 * On cache miss, consults `parent->inode->dir_ops->lookup` (if any).
 * A successful lazy lookup is attached to the cache before returning,
 * so subsequent resolutions are O(1) over `parent->children`. */
static struct dentry* lookup_child(struct dentry* parent,
                                   const char* name, size_t name_len) {
    if (!parent) return NULL;
    /* Cache hit? */
    for (struct dentry* c = parent->children; c; c = c->sibling) {
        if (streq_n(c->name, name, name_len) && c->name[name_len] == 0) {
            return c;
        }
    }
    /* Cache miss — ask the fs to resolve, if it supports lazy lookup. */
    if (!parent->inode || !parent->inode->dir_ops ||
        !parent->inode->dir_ops->lookup) {
        return NULL;
    }
    /* Build a NUL-terminated copy of the component (lookup wants a C
     * string; the caller has only `name_len` bytes of `path` content). */
    char buf[VFS_NAME_MAX + 1];
    if (name_len > VFS_NAME_MAX) return NULL;
    for (size_t i = 0; i < name_len; i++) buf[i] = name[i];
    buf[name_len] = 0;

    struct inode* ino = NULL;
    if (parent->inode->dir_ops->lookup(parent->inode, buf, &ino) != 0) return NULL;
    if (!ino) return NULL;
    return vfs_attach_child(parent, buf, ino);
}

/* Walk a path, returning the dentry of the LAST component, or NULL on
 * any failure.  If `out_parent` is non-NULL, returns the parent dentry
 * of the last component (useful for create/mkdir). */
static struct dentry* resolve_path(const char* path,
                                   struct dentry** out_parent,
                                   const char**    out_last_name) {
    if (!path || path[0] != '/') return NULL;
    if (path[1] == 0) {                         /* exactly "/" */
        if (out_parent)    *out_parent    = NULL;
        if (out_last_name) *out_last_name = NULL;
        return root;
    }

    struct dentry* cur = root;
    const char* p = path + 1;                   /* skip leading '/' */
    const char* last_start = p;

    for (;;) {
        /* Find the next '/' or end-of-string. */
        const char* slash = p;
        while (*slash && *slash != '/') slash++;
        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0 || comp_len > VFS_NAME_MAX) return NULL;
        last_start = p;

        if (*slash == 0) {
            /* Last component — return parent + name without descending. */
            if (out_parent)    *out_parent    = cur;
            if (out_last_name) *out_last_name = last_start;
            return lookup_child(cur, last_start, comp_len);
        }

        /* Intermediate component — must exist and be a directory. */
        struct dentry* nxt = lookup_child(cur, p, comp_len);
        if (!nxt || !nxt->inode || nxt->inode->type != INODE_DIR) return NULL;
        cur = nxt;
        p = slash + 1;
    }
}

/* ------------------------------------------------------------------- */
/* Tree manipulation — used by filesystems and create/mkdir helpers.    */
/* ------------------------------------------------------------------- */

struct dentry* vfs_attach_child(struct dentry* parent, const char* name,
                                struct inode* inode) {
    if (!parent || !name || !inode) return NULL;

    struct dentry* d = (struct dentry*)kcalloc(1, sizeof(struct dentry));
    if (!d) return NULL;
    strcpy_n(d->name, name, sizeof d->name);
    d->inode    = inode;
    d->parent   = parent;
    d->sibling  = parent->children;             /* push to head */
    parent->children = d;
    return d;
}

/* ------------------------------------------------------------------- */
/* Mount.                                                               */
/* ------------------------------------------------------------------- */

int vfs_mount(const char* fs_name, const char* path, const char* dev_name) {
    if (!fs_name || !path) return -1;

    /* Find the fs implementation. */
    struct fs_type* fs = NULL;
    for (struct fs_type* p = fs_types; p; p = p->next) {
        if (streq(p->name, fs_name)) { fs = p; break; }
    }
    if (!fs) {
        kprintf("vfs_mount: fs %s not registered\n", fs_name);
        return -2;
    }

    /* Resolve the optional backing block device. */
    struct block_device* bdev = NULL;
    if (dev_name) {
        bdev = blk_find(dev_name);
        if (!bdev) {
            kprintf("vfs_mount: block device %s not found\n", dev_name);
            return -5;
        }
    }

    /* Resolve the mountpoint dentry. */
    struct dentry* mp;
    if (streq(path, "/")) {
        mp = root;
    } else {
        mp = resolve_path(path, NULL, NULL);
        if (!mp) {
            kprintf("vfs_mount: mountpoint %s not found\n", path);
            return -3;
        }
    }

    /* Detach any placeholder inode on a nested mountpoint so the fs can
     * install its own root.  The root ("/") still hands the fs a blank
     * dentry (vfs_init left mp->inode NULL there).  The previous inode
     * is leaked for now — ramfs bootstrap directories carry no payload,
     * and a proper umount path is a later milestone. */
    if (mp != root) mp->inode = NULL;

    /* Hand off to the fs to fill in the mountpoint. */
    int r = fs->mount(bdev, mp);
    if (r != 0) {
        kprintf("vfs_mount: %s->mount() failed: %d\n", fs_name, r);
        return r;
    }
    if (dev_name) kprintf("vfs: mounted %s (%s) at %s\n", fs_name, dev_name, path);
    else          kprintf("vfs: mounted %s at %s\n", fs_name, path);
    return 0;
}

/* ------------------------------------------------------------------- */
/* Open / read / write / close / readdir / mkdir / create.              */
/* ------------------------------------------------------------------- */

struct file* vfs_open(const char* path, int flags) {
    struct dentry*  parent;
    const char*     last;
    struct dentry*  d = resolve_path(path, &parent, &last);

    if (!d) {
        if ((flags & VFS_CREATE) == 0) return NULL;
        if (!parent || !last)          return NULL;
        if (vfs_create(path) != 0)     return NULL;
        d = resolve_path(path, NULL, NULL);
        if (!d) return NULL;
    }

    if (!d->inode) return NULL;

    /* VFS_TRUNC: logically empty the file by zeroing its size.  The
     * underlying buffer (if any) is left allocated — subsequent writes
     * overwrite from the start, and the next save round-trip is clean. */
    if ((flags & VFS_TRUNC) && d->inode->type == INODE_FILE) {
        d->inode->size = 0;
    }

    struct file* f = (struct file*)kcalloc(1, sizeof(struct file));
    if (!f) return NULL;
    f->inode  = d->inode;
    f->dentry = d;
    f->flags  = flags;
    f->pos    = 0;
    return f;
}

int vfs_close(struct file* f) {
    if (!f) return -1;
    if (f->inode && f->inode->ops && f->inode->ops->close) f->inode->ops->close(f);
    kfree(f);
    return 0;
}

ssize_t vfs_read(struct file* f, void* buf, size_t n) {
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->read) return -1;
    ssize_t r = f->inode->ops->read(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

ssize_t vfs_write(struct file* f, const void* buf, size_t n) {
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->write) return -1;
    ssize_t r = f->inode->ops->write(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

int vfs_readdir(struct file* f, struct dirent* out) {
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->readdir) return -1;
    return f->inode->ops->readdir(f, out);
}

/* Split a path into "parent dir path" and "last component".  Caller
 * provides a buffer for the parent path (mutable copy). */
static int split_parent(const char* path, char* parent_buf, size_t cap,
                        const char** last_out) {
    size_t len = strlen_(path);
    if (len == 0 || len >= cap)  return -1;

    /* Find the last '/'.  Path must start with '/'. */
    int last_slash = -1;
    for (size_t i = 0; i < len; i++) if (path[i] == '/') last_slash = (int)i;
    if (last_slash < 0) return -1;

    if (last_slash == 0) {
        parent_buf[0] = '/';
        parent_buf[1] = 0;
    } else {
        memcpy_(parent_buf, path, (size_t)last_slash);
        parent_buf[last_slash] = 0;
    }
    *last_out = path + last_slash + 1;
    return 0;
}

/* Dispatch a namespace mutator to the parent inode's dir_ops.  Returns
 * 0 on success.  Walks the path, attaches the freshly-created child
 * inode (returned by the fs) under the parent dentry. */
static int vfs_mutator(const char* path, int is_dir) {
    char buf[256];
    const char* last;
    if (split_parent(path, buf, sizeof buf, &last) != 0) return -1;
    if (!*last) return -1;
    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return -1;
    if (!parent->inode->dir_ops) return -1;

    int (*op)(struct inode*, const char*, struct inode**) =
        is_dir ? parent->inode->dir_ops->mkdir
               : parent->inode->dir_ops->create;
    if (!op) return -1;

    /* Refuse a duplicate name up front.  The fs may also enforce this
     * (and should, for races once SMP lands), but checking here keeps
     * the error surface uniform regardless of fs. */
    for (struct dentry* c = parent->children; c; c = c->sibling) {
        if (streq(c->name, last)) return -2;
    }

    struct inode* ino = NULL;
    int r = op(parent->inode, last, &ino);
    if (r != 0 || !ino) return r ? r : -3;
    if (!vfs_attach_child(parent, last, ino)) return -4;
    return 0;
}

int vfs_create(const char* path) { return vfs_mutator(path, 0); }
int vfs_mkdir (const char* path) { return vfs_mutator(path, 1); }

/* Remove `path` (file or empty dir).  The fs op frees the inode; we
 * then detach + free the dentry.  Mount roots refuse removal because
 * their parent belongs to a different fs (dir_ops mismatch would
 * corrupt the foreign inode's accounting). */
int vfs_unlink(const char* path) {
    char buf[256];
    const char* last;
    if (split_parent(path, buf, sizeof buf, &last) != 0) return -1;
    if (!*last) return -1;

    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return -1;
    if (!parent->inode->dir_ops || !parent->inode->dir_ops->unlink)    return -1;

    /* Find the child dentry + keep the link BEFORE it for splicing. */
    struct dentry** link = &parent->children;
    struct dentry*  d    = parent->children;
    while (d && !streq(d->name, last)) { link = &d->sibling; d = d->sibling; }
    if (!d || !d->inode) return -1;
    if (d->inode->type == INODE_DIR && d->children) return -2;   /* not empty */
    if (d->inode->type == INODE_DEVICE) return -1;               /* devfs nodes */

    int r = parent->inode->dir_ops->unlink(parent->inode, last, d->inode);
    if (r != 0) return r;

    *link = d->sibling;                          /* splice out of the tree */
    kfree(d);
    return 0;
}
