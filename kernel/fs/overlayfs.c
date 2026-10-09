/* =============================================================================
 * overlayfs.c — a union of directory trees, Linux's overlayfs (§M90 rung 4).
 * 2026-10-09.
 *
 * WHY.  Docker's default storage driver, overlay2, keeps every image layer as
 * a plain directory and shows a container its image as ONE tree: the image's
 * layers read-only underneath, a writable layer of its own on top.  Without
 * this kernel feature Docker falls back to its `vfs` driver, which COPIES the
 * whole image for every container (and every build step).
 *
 * WHAT IT IS — the contract of Linux's overlayfs, reduced to what a container
 * runtime relies on:
 *
 *   mount("overlay", merged, "overlay", 0,
 *         "lowerdir=L1:L2:…,upperdir=U,workdir=W")
 *
 *   - LOOKUP goes top-down: the upper layer U, then L1, L2… (leftmost is the
 *     highest).  A NON-DIRECTORY found anywhere hides everything below it.
 *     DIRECTORIES of the same name MERGE, down to the first OPAQUE one (its
 *     trusted.overlay.opaque = "y", kept here as VFS_IF_OPAQUE).
 *   - A WHITEOUT — a character device 0:0 (INODE_WHITEOUT here) — in a layer
 *     says "this name is deleted" and hides it in every layer below.
 *   - READING a file reads the topmost copy.  WRITING (open for write or
 *     truncate, chmod, chown) first COPIES IT UP: its parent directories are
 *     made in U, then the file with its data and attributes; from then on the
 *     upper copy is the file.
 *   - CREATE / MKDIR / SYMLINK land in U (over a whiteout: the whiteout goes,
 *     and a directory made there is OPAQUE, so the deleted one's lower
 *     contents do not reappear).
 *   - UNLINK / RMDIR remove the upper copy and, when a lower layer still has
 *     the name, leave a whiteout.  A directory is empty when its MERGED view
 *     is (whiteouts do not count).
 *   - RENAME of a file copies it up and renames in U (whiteout at the old
 *     name if a lower has it).  A DIRECTORY that exists in a lower layer is
 *     refused with EXDEV — Linux's answer without redirect_dir, and the one
 *     every tool (mv, Docker's archive code) handles by copying.
 *   - READDIR merges the layers, hides whiteouts and stops at opaque layers.
 *   - Without upperdir the overlay is read-only (every write is EROFS).
 *
 * WHAT IT IS NOT, stated:
 *   - Hard links INTO the overlay are refused (Linux supports them through
 *     copy-up of the source).  Hard links inside a layer directory are fine.
 *   - No redirect_dir, metacopy, index, xino, nfs_export, volatile: the
 *     options are accepted when they ask for the default and otherwise
 *     refused BY NAME.
 *   - The layers must not change underneath a mounted overlay (Linux calls
 *     that undefined too): nodes keep pointers to the layers' dentries.
 *   - Inode numbers are the overlay's own (st_ino differs from the layer's).
 *   - The workdir is checked and given its `work` subdirectory, as Linux
 *     does; copy-ups are not staged through it (a copy-up here happens under
 *     the namespace lock, so nobody can observe a half-made file).
 *
 * SHAPE.  Every name the overlay shows is an `ovl_node`: the VFS inode (first,
 * so the VFS's inode pointer IS the node) plus the dentries of the layers that
 * hold it — `up` in U, `lo[]` in the lowers, topmost first.  Nodes are made by
 * lookup / create and freed by unlink, or by the unmount's tree walk.  Every
 * operation the VFS calls with its namespace lock held (lookup, the
 * mutators, readdir) acts on the layers through the dentry-level
 * vfs_*_locked calls, never through a path; read and write take no lock and
 * call the layer's file operations directly.
 * ============================================================================= */

#include "vfs.h"
#include "kmalloc.h"
#include "printf.h"
#include "module.h"
#include "pcache.h"
#include "fsnotify.h"
#include "pmm.h"
#include "kmap.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>

#define OVL_MAX_LOWER 32

struct ovl_mount {
    struct dentry* upper;                 /* NULL: a read-only overlay */
    struct dentry* work;
    struct dentry* lower[OVL_MAX_LOWER];
    int            nlower;
};

struct ovl_node {
    struct inode      in;                 /* FIRST: the VFS's inode pointer is the node */
    struct ovl_mount* m;
    struct ovl_node*  parent;
    struct ovl_node*  kids;               /* nodes made under this directory */
    struct ovl_node*  next;               /* sibling in parent->kids */
    char              name[VFS_NAME_MAX + 1];
    struct dentry*    up;                 /* in the upper layer, or NULL */
    struct dentry*    lo[OVL_MAX_LOWER];  /* in the lower layers, topmost first */
    int               nlo;
    int               is_root;
};

static const struct file_ops  ovl_file_ops;
static const struct file_ops  ovl_dir_ops;
static const struct inode_ops ovl_inode_ops;

/* ---- helpers ------------------------------------------------------------------ */

static size_t s_len(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }
static int s_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void s_cpy(char* d, const char* s, size_t cap) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static struct ovl_node* N(struct inode* in) { return (struct ovl_node*)in; }

/* The layer dentry that answers for a node: upper if it has one. */
static struct dentry* top(const struct ovl_node* n) {
    return n->up ? n->up : (n->nlo ? n->lo[0] : NULL);
}

/* A struct file for calling a LAYER's file operations directly.  Never opened
 * through the VFS, so never closed through it: no open count, no magic close. */
static void fake_file(struct file* rf, struct dentry* d, int flags) {
    for (size_t i = 0; i < sizeof *rf; i++) ((uint8_t*)rf)[i] = 0;
    rf->inode = d->inode;
    rf->dentry = d;
    rf->flags = flags;
    rf->magic = VFS_FILE_MAGIC;
}

static int is_whiteout(const struct dentry* d) { return d && d->inode && d->inode->type == INODE_WHITEOUT; }
static int is_dir(const struct dentry* d) { return d && d->inode && d->inode->type == INODE_DIR; }
static int is_opaque(const struct dentry* d) { return d && d->inode && (d->inode->vflags & VFS_IF_OPAQUE); }

/* The node's attributes, from its top layer. */
static void sync_attrs(struct ovl_node* n) {
    struct dentry* t = top(n);
    if (!t || !t->inode) return;
    n->in.size      = t->inode->size;
    n->in.mode      = t->inode->mode;
    n->in.owner_uid = t->inode->owner_uid;
    n->in.owner_gid = t->inode->owner_gid;
}

static void set_type(struct ovl_node* n) {
    struct dentry* t = top(n);
    enum inode_type ty = t && t->inode ? t->inode->type : INODE_FILE;
    n->in.type = ty;
    n->in.dir_ops = NULL;
    if (ty == INODE_DIR) {
        n->in.ops = &ovl_dir_ops;
        n->in.dir_ops = &ovl_inode_ops;
    } else if (ty == INODE_DEVICE) {
        /* A device node in an image layer: the device's own operations. */
        n->in.ops = t->inode->ops;
        n->in.private = t->inode->private;
    } else if (ty == INODE_FIFO) {
        n->in.ops = NULL;                 /* a FIFO's ends are the VFS's own */
    } else {
        n->in.ops = &ovl_file_ops;        /* files and symbolic links */
    }
    sync_attrs(n);
}

static struct ovl_node* node_new(struct ovl_mount* m, struct ovl_node* parent, const char* name) {
    struct ovl_node* n = (struct ovl_node*)kcalloc(1, sizeof *n);
    if (!n) return NULL;
    vfs_inode_defaults(&n->in);
    n->m = m;
    n->parent = parent;
    s_cpy(n->name, name, sizeof n->name);
    if (parent) { n->next = parent->kids; parent->kids = n; }
    return n;
}

static void node_unlink_kid(struct ovl_node* n) {
    if (!n->parent) return;
    struct ovl_node** pp = &n->parent->kids;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    n->next = NULL;
}

/* A node already made for `name` under `d` (a name whose unlink the VFS has
 * deferred until its last close still has one, and must stay hidden). */
static struct ovl_node* kid(struct ovl_node* d, const char* name) {
    for (struct ovl_node* k = d->kids; k; k = k->next) if (s_eq(k->name, name)) return k;
    return NULL;
}

/* ---- lookup ------------------------------------------------------------------- */

/* What `name` under directory `d` is in the layers: fills up/lo/nlo.
 * 1 = it exists, 0 = it does not (absent or whited out). */
static int probe(struct ovl_node* d, const char* name, struct dentry** up_out,
                 struct dentry** lo, int* nlo_out) {
    struct dentry* up = NULL;
    int nlo = 0, stop = 0;
    if (d->up) {
        struct dentry* c = vfs_child_locked(d->up, name);
        if (c && c->inode) {
            if (is_whiteout(c)) return 0;
            up = c;
            if (!is_dir(c) || is_opaque(c)) stop = 1;
        }
        if (is_opaque(d->up)) stop = 1;
    }
    for (int i = 0; !stop && i < d->nlo; i++) {
        struct dentry* c = vfs_child_locked(d->lo[i], name);
        if (!c || !c->inode) continue;
        if (is_whiteout(c)) break;
        if (is_dir(c)) {
            if (!up && nlo > 0 && !is_dir(lo[0])) break;
            lo[nlo++] = c;
            if (is_opaque(c)) break;
        } else {
            if (up || nlo > 0) break;     /* a non-directory below anything is hidden */
            lo[nlo++] = c;
            break;
        }
        if (nlo >= OVL_MAX_LOWER) break;
    }
    if (up_out) *up_out = up;
    if (nlo_out) *nlo_out = nlo;
    return up || nlo;
}

static int ovl_lookup(struct inode* dir, const char* name, struct inode** out) {
    struct ovl_node* d = N(dir);
    struct ovl_node* k = kid(d, name);
    if (k && k->in.unlink_dir) return -1;           /* removed, still open: hidden */
    if (k) { *out = &k->in; return 0; }
    struct dentry* up; struct dentry* lo[OVL_MAX_LOWER]; int nlo;
    if (!probe(d, name, &up, lo, &nlo)) return -1;
    struct ovl_node* n = node_new(d->m, d, name);
    if (!n) return -1;
    n->up = up;
    for (int i = 0; i < nlo; i++) n->lo[i] = lo[i];
    n->nlo = nlo;
    set_type(n);
    *out = &n->in;
    return 0;
}

/* ---- the merged directory listing ------------------------------------------- */

struct ovl_ent { char name[VFS_NAME_MAX + 1]; enum inode_type type; uint64_t size; int hidden; };
struct ovl_list { struct ovl_ent* e; int n, cap; };

static int list_has(struct ovl_list* l, const char* name) {
    for (int i = 0; i < l->n; i++) if (s_eq(l->e[i].name, name)) return 1;
    return 0;
}
static int list_add(struct ovl_list* l, const struct dirent* de, int hidden) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        struct ovl_ent* ne = (struct ovl_ent*)kmalloc((size_t)nc * sizeof *ne);
        if (!ne) return -1;
        for (int i = 0; i < l->n; i++) ne[i] = l->e[i];
        kfree(l->e);
        l->e = ne; l->cap = nc;
    }
    struct ovl_ent* x = &l->e[l->n++];
    s_cpy(x->name, de->name, sizeof x->name);
    x->type = de->type; x->size = de->size; x->hidden = hidden;
    return 0;
}

/* One layer directory's names into the list (first sighting wins). */
static int list_layer(struct ovl_list* l, struct dentry* dir) {
    if (!dir || !dir->inode || !dir->inode->ops || !dir->inode->ops->readdir) return 0;
    struct file rf;
    fake_file(&rf, dir, VFS_RDONLY);
    struct dirent de;
    for (int guard = 0; guard < 100000; guard++) {
        int r = dir->inode->ops->readdir(&rf, &de);
        if (r <= 0) break;
        if (s_eq(de.name, ".") || s_eq(de.name, "..")) continue;
        if (list_has(l, de.name)) continue;
        if (list_add(l, &de, de.type == INODE_WHITEOUT) != 0) return -1;
    }
    return 0;
}

/* The merged view of directory node `d`; namespace lock held. */
static int list_build(struct ovl_node* d, struct ovl_list* l) {
    l->e = NULL; l->n = l->cap = 0;
    int stop = 0;
    if (d->up) {
        if (list_layer(l, d->up) != 0) return -1;
        if (is_opaque(d->up)) stop = 1;
    }
    for (int i = 0; !stop && i < d->nlo; i++) {
        if (list_layer(l, d->lo[i]) != 0) return -1;
        if (is_opaque(d->lo[i])) stop = 1;
    }
    /* A name whose unlink waits for its last close is gone already. */
    for (struct ovl_node* k = d->kids; k; k = k->next) {
        if (!k->in.unlink_dir) continue;
        for (int i = 0; i < l->n; i++) if (s_eq(l->e[i].name, k->name)) l->e[i].hidden = 1;
    }
    return 0;
}

static int list_visible(struct ovl_list* l) {
    int n = 0;
    for (int i = 0; i < l->n; i++) if (!l->e[i].hidden) n++;
    return n;
}

/* ---- copy-up ------------------------------------------------------------------ */

static ssize_t layer_read(struct dentry* d, void* buf, size_t n, uint64_t off) {
    if (!d || !d->inode || !d->inode->ops || !d->inode->ops->read) return -1;
    struct file rf;
    fake_file(&rf, d, VFS_RDONLY);
    return d->inode->ops->read(&rf, buf, n, off);
}
static ssize_t layer_write(struct dentry* d, const void* buf, size_t n, uint64_t off) {
    if (!d || !d->inode || !d->inode->ops || !d->inode->ops->write) return -1;
    struct file rf;
    fake_file(&rf, d, VFS_WRONLY);
    return d->inode->ops->write(&rf, buf, n, off);
}

/* Remove a whiteout (and only a whiteout) at `name` in upper dir `ud`. */
static int drop_whiteout(struct dentry* ud, const char* name) {
    struct dentry* c = vfs_child_locked(ud, name);
    if (!is_whiteout(c)) return 0;
    return vfs_remove_locked(ud, name) == 0 ? 1 : -1;
}

/* Give node `n` (and its ancestors) a copy in the upper layer.  `data` = 0
 * skips the file contents (the open truncates).  0, or -30 EROFS (no upper
 * layer), -1 on failure.  Namespace lock held. */
static int copy_up(struct ovl_node* n, int data) {
    if (n->up) return 0;
    if (!n->m->upper || !n->parent) return -30;
    int r = copy_up(n->parent, 1);
    if (r) return r;
    struct dentry* pd = n->parent->up;
    struct dentry* src = n->nlo ? n->lo[0] : NULL;
    if (!src || !src->inode) return -1;
    if (drop_whiteout(pd, n->name) < 0) return -1;
    struct dentry* nd = NULL;
    switch (src->inode->type) {
    case INODE_DIR:
        r = vfs_make_locked(pd, n->name, 1, &nd);
        break;
    case INODE_FIFO:
        r = vfs_make_locked(pd, n->name, 2, &nd);
        break;
    case INODE_SYMLINK: {
        char tgt[256];
        ssize_t k = layer_read(src, tgt, sizeof tgt - 1, 0);
        if (k <= 0) return -1;
        tgt[k] = 0;
        r = vfs_symlink_locked(pd, n->name, tgt, &nd);
        break;
    }
    case INODE_FILE: {
        r = vfs_make_locked(pd, n->name, 0, &nd);
        if (r || !data) break;
        uint8_t* buf = (uint8_t*)kmalloc(4096);
        if (!buf) { r = -1; break; }
        uint64_t off = 0, size = src->inode->size;
        while (off < size) {
            size_t want = size - off < 4096 ? (size_t)(size - off) : 4096;
            ssize_t k = layer_read(src, buf, want, off);
            if (k <= 0) break;
            if (layer_write(nd, buf, (size_t)k, off) != k) { r = -1; break; }
            off += (uint64_t)k;
        }
        kfree(buf);
        break;
    }
    default:
        return -1;                      /* a device node is not copied up */
    }
    if (r) {
        if (nd) vfs_remove_locked(pd, n->name);
        return r == -2 ? -1 : r;
    }
    nd->inode->mode      = src->inode->mode;
    nd->inode->owner_uid = src->inode->owner_uid;
    nd->inode->owner_gid = src->inode->owner_gid;
    n->up = nd;
    sync_attrs(n);
    return 0;
}

/* ---- directory operations (namespace lock held) ---------------------------------- */

static int make_in(struct inode* dir, const char* name, int kind, const char* target,
                   struct inode** out) {
    struct ovl_node* d = N(dir);
    struct dentry* up; struct dentry* lo[OVL_MAX_LOWER]; int nlo;
    if (probe(d, name, &up, lo, &nlo)) return -2;            /* EEXIST */
    int r = copy_up(d, 1);
    if (r) return r;
    int had_whiteout = drop_whiteout(d->up, name);
    if (had_whiteout < 0) return -1;
    struct dentry* nd = NULL;
    r = target ? vfs_symlink_locked(d->up, name, target, &nd) : vfs_make_locked(d->up, name, kind, &nd);
    if (r) return r;
    /* A directory made where a deleted one was: opaque, so the deleted one's
     * lower contents stay deleted. */
    if (kind == 1 && had_whiteout) nd->inode->vflags |= VFS_IF_OPAQUE;
    struct ovl_node* k = kid(d, name);
    if (k) { node_unlink_kid(k); }                /* a stale node of a deleted name */
    struct ovl_node* n = node_new(d->m, d, name);
    if (!n) { vfs_remove_locked(d->up, name); return -1; }
    n->up = nd;
    set_type(n);
    *out = &n->in;
    return 0;
}

static int ovl_create(struct inode* dir, const char* name, struct inode** out) {
    return make_in(dir, name, 0, NULL, out);
}
static int ovl_mkdir(struct inode* dir, const char* name, struct inode** out) {
    return make_in(dir, name, 1, NULL, out);
}
static int ovl_symlink(struct inode* dir, const char* name, const char* target, struct inode** out) {
    return make_in(dir, name, 0, target, out);
}

static int ovl_unlink(struct inode* dir, const char* name, struct inode* child) {
    struct ovl_node* d = N(dir);
    struct ovl_node* c = N(child);
    if (!c) return -1;
    if (c->in.type == INODE_DIR) {
        struct ovl_list l;
        if (list_build(c, &l) != 0) { kfree(l.e); return -1; }
        int vis = list_visible(&l);
        kfree(l.e);
        if (vis) return -2;                                    /* ENOTEMPTY */
    }
    int need_whiteout = c->nlo > 0;
    if ((need_whiteout || c->up) && !d->m->upper) return -30;
    if (need_whiteout) { int r = copy_up(d, 1); if (r) return r; }
    if (c->up) {
        if (c->in.type == INODE_DIR) {
            /* only whiteouts are left in it: they go with it */
            for (int guard = 0; guard < 100000 && c->up->children; guard++) {
                struct dentry* w = c->up->children;
                if (!is_whiteout(w) || vfs_remove_locked(c->up, w->name) != 0) break;
            }
        }
        if (vfs_remove_locked(d->up, name) != 0) return -1;
    }
    if (need_whiteout && vfs_make_locked(d->up, name, 3, NULL) != 0) return -1;
    node_unlink_kid(c);
    /* Free what the node owns (nothing but itself; layer dentries are the
     * layers').  Its own children were removed first — a directory goes
     * only when empty, and the VFS has already dropped their dentries. */
    for (struct ovl_node* k = c->kids; k; ) { struct ovl_node* nx = k->next; k->parent = NULL; k = nx; }
    kfree(c);
    return 0;
}

static int do_rename(struct ovl_node* od, const char* oname, struct ovl_node* nd,
                     const char* nname, struct ovl_node* c) {
    if (c->in.type == INODE_DIR && c->nlo > 0) return -18;   /* EXDEV: no redirect_dir */
    struct dentry* up; struct dentry* lo[OVL_MAX_LOWER]; int nlo;
    if (probe(nd, nname, &up, lo, &nlo)) return -2;           /* exists */
    int r = copy_up(c, 1);
    if (!r && nd != od) r = copy_up(nd, 1);
    if (r) return r;
    if (drop_whiteout(nd->up, nname) < 0) return -1;
    if (vfs_rename_locked(od->up, oname, nd->up, nname) != 0) return -1;
    if (c->nlo > 0 && vfs_make_locked(od->up, oname, 3, NULL) != 0) return -1;
    c->nlo = 0;
    s_cpy(c->name, nname, sizeof c->name);
    if (nd != od) {
        node_unlink_kid(c);
        c->parent = nd;
        c->next = nd->kids; nd->kids = c;
    }
    return 0;
}
static int ovl_rename(struct inode* dir, const char* oldname, const char* newname, struct inode* child) {
    return do_rename(N(dir), oldname, N(dir), newname, N(child));
}
static int ovl_move(struct inode* odir, const char* oname, struct inode* ndir,
                    const char* nname, struct inode* child) {
    return do_rename(N(odir), oname, N(ndir), nname, N(child));
}

/* chmod / chown: the VFS has changed the node; the upper copy follows. */
static int ovl_setattr(struct inode* dir, const char* name, struct inode* child) {
    (void)dir; (void)name;
    struct ovl_node* c = N(child);
    int r = copy_up(c, 1);
    if (r) return r;
    struct inode* u = c->up->inode;
    u->mode = c->in.mode;
    u->owner_uid = c->in.owner_uid;
    u->owner_gid = c->in.owner_gid;
    struct dentry* pu = c->up->parent;
    if (pu && pu->inode && pu->inode->dir_ops && pu->inode->dir_ops->setattr)
        return pu->inode->dir_ops->setattr(pu->inode, c->up->name, u);
    return 0;
}

static const struct inode_ops ovl_inode_ops = {
    .lookup  = ovl_lookup,
    .create  = ovl_create,
    .mkdir   = ovl_mkdir,
    .unlink  = ovl_unlink,
    .rename  = ovl_rename,
    .setattr = ovl_setattr,
    .link    = NULL,                 /* hard links into an overlay: refused */
    .symlink = ovl_symlink,
    .move    = ovl_move,
};

/* ---- file operations ---------------------------------------------------------- */

/* Opening for writing (or truncating) copies the file up first. */
static int ovl_open(struct file* f) {
    struct ovl_node* n = N(f->inode);
    if (n->in.type != INODE_FILE) return 0;
    int trunc = (f->flags & VFS_TRUNC) != 0;
    if (!(f->flags & (VFS_WRONLY | VFS_TRUNC))) return 0;
    vfs_ns_lock();
    int r = copy_up(n, !trunc);
    vfs_ns_unlock();
    if (r == 0 && trunc && n->up) {
        n->up->inode->size = 0;
        pcache_invalidate(n->up->inode);
        n->in.size = 0;
    }
    return r;
}

static ssize_t ovl_read(struct file* f, void* buf, size_t n, uint64_t off) {
    struct dentry* t = top(N(f->inode));
    return t ? layer_read(t, buf, n, off) : -1;
}

static ssize_t ovl_write(struct file* f, const void* buf, size_t n, uint64_t off) {
    struct ovl_node* o = N(f->inode);
    if (!o->up) {
        vfs_ns_lock();
        int r = copy_up(o, 1);
        vfs_ns_unlock();
        if (r) return -1;
    }
    ssize_t w = layer_write(o->up, buf, n, off);
    if (w > 0) o->in.size = o->up->inode->size;
    return w;
}

static int ovl_close(struct file* f) {
    if (f->private) {
        struct ovl_list* l = (struct ovl_list*)f->private;
        kfree(l->e);
        kfree(l);
        f->private = NULL;
    }
    return 0;
}

/* Called under the namespace lock (vfs_readdir).  The merged list is made at
 * the first call and kept for the open directory, as Linux's overlay keeps
 * its merged cache per open, so the positions stay stable while it is read. */
static int ovl_readdir(struct file* f, struct dirent* out) {
    struct ovl_node* d = N(f->inode);
    struct ovl_list* l = (struct ovl_list*)f->private;
    if (!l) {
        l = (struct ovl_list*)kcalloc(1, sizeof *l);
        if (!l) return -1;
        if (list_build(d, l) != 0) { kfree(l->e); kfree(l); return -1; }
        f->private = l;
    }
    while (f->pos < (uint64_t)l->n) {
        struct ovl_ent* e = &l->e[f->pos++];
        if (e->hidden) continue;
        s_cpy(out->name, e->name, sizeof out->name);
        out->type = e->type;
        out->size = e->size;
        return 1;
    }
    return 0;
}

static const struct file_ops ovl_file_ops = {
    .read = ovl_read, .write = ovl_write, .readdir = NULL, .close = ovl_close, .open = ovl_open,
};
static const struct file_ops ovl_dir_ops = {
    .read = NULL, .write = NULL, .readdir = ovl_readdir, .close = ovl_close, .open = NULL,
};

/* ---- mount -------------------------------------------------------------------- */

/* The value of `key=` in a comma-separated option string, into `out`. */
static int opt_value(const char* data, const char* key, char* out, size_t cap) {
    size_t kl = s_len(key);
    const char* p = data;
    while (*p) {
        const char* e = p;
        while (*e && *e != ',') e++;
        if ((size_t)(e - p) > kl && p[kl] == '=') {
            size_t i = 0;
            while (i < kl && p[i] == key[i]) i++;
            if (i == kl) {
                size_t n = (size_t)(e - p) - kl - 1;
                if (n >= cap) return -1;
                for (size_t j = 0; j < n; j++) out[j] = p[kl + 1 + j];
                out[n] = 0;
                return 1;
            }
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* Every option must be one we honour.  Three take a value; the rest are the
 * DEFAULTS Docker and other tools spell out explicitly (index=off, …), and
 * are accepted exactly as written.  Anything that asks for a feature this
 * overlay lacks (redirect_dir=on, metacopy=on, …) is refused BY NAME. */
static int opts_known(const char* data) {
    static const char* keyed[] = { "lowerdir", "upperdir", "workdir", NULL };
    static const char* exact[] = { "index=off", "redirect_dir=off", "redirect_dir=nofollow",
                                   "metacopy=off", "xino=off", "xino=auto", "nfs_export=off",
                                   "uuid=off", "uuid=null", "userxattr", "volatile", "rw", NULL };
    const char* p = data;
    while (*p) {
        const char* e = p;
        while (*e && *e != ',') e++;
        size_t len = (size_t)(e - p);
        int good = 0;
        for (int i = 0; keyed[i] && !good; i++) {
            size_t n = s_len(keyed[i]);
            if (len > n && p[n] == '=') {
                size_t j = 0;
                while (j < n && p[j] == keyed[i][j]) j++;
                good = j == n;
            }
        }
        for (int i = 0; exact[i] && !good; i++) {
            size_t n = s_len(exact[i]);
            if (len != n) continue;
            size_t j = 0;
            while (j < n && p[j] == exact[i][j]) j++;
            good = j == n;
        }
        if (!good && len) {
            char bad[48]; size_t n = len < sizeof bad - 1 ? len : sizeof bad - 1;
            for (size_t j = 0; j < n; j++) bad[j] = p[j];
            bad[n] = 0;
            kprintf("overlay: option '%s' not supported\n", bad);
            return 0;
        }
        p = *e ? e + 1 : e;
    }
    return 1;
}

static struct dentry* layer_dir(const char* path) {
    struct dentry* d = vfs_resolve_locked(path);
    return is_dir(d) ? d : NULL;
}

static int ovl_mount_data(struct dentry* mp, const char* data) {
    if (!opts_known(data)) return -22;
    struct ovl_mount* m = (struct ovl_mount*)kcalloc(1, sizeof *m);
    char* buf = (char*)kmalloc(4096);
    if (!m || !buf) { kfree(m); kfree(buf); return -12; }
    int r = -22;
    if (opt_value(data, "lowerdir", buf, 4096) != 1 || !buf[0]) {
        kprintf("overlay: no lowerdir\n");
        goto fail;
    }
    for (char* p = buf; *p; ) {
        char* e = p;
        while (*e && *e != ':') e++;
        char save = *e; *e = 0;
        if (m->nlower >= OVL_MAX_LOWER) { kprintf("overlay: more than %d lower layers\n", OVL_MAX_LOWER); goto fail; }
        struct dentry* d = layer_dir(p);
        if (!d) { kprintf("overlay: lowerdir '%s' is not a directory\n", p); r = -2; goto fail; }
        m->lower[m->nlower++] = d;
        *e = save;
        p = *e ? e + 1 : e;
    }
    int hu = opt_value(data, "upperdir", buf, 4096);
    if (hu == 1) {
        m->upper = layer_dir(buf);
        if (!m->upper) { kprintf("overlay: upperdir '%s' is not a directory\n", buf); r = -2; goto fail; }
        if (opt_value(data, "workdir", buf, 4096) != 1 || !(m->work = layer_dir(buf))) {
            kprintf("overlay: an upperdir needs a workdir\n");
            goto fail;
        }
        /* Linux keeps its scratch space in workdir/work; make it so the
         * layout is the one tools expect (Docker removes it with the layer). */
        if (!vfs_child_locked(m->work, "work")) vfs_make_locked(m->work, "work", 1, NULL);
    }
    struct ovl_node* root = node_new(m, NULL, "");
    if (!root) { r = -12; goto fail; }
    root->is_root = 1;
    root->up = m->upper;
    for (int i = 0; i < m->nlower; i++) root->lo[i] = m->lower[i];
    root->nlo = m->nlower;
    set_type(root);
    root->in.private = m;
    mp->inode = &root->in;
    kfree(buf);
    return 0;
fail:
    kfree(buf);
    kfree(m);
    return r;
}

static int ovl_mount_plain(struct block_device* dev, struct dentry* mp) {
    (void)dev; (void)mp;
    kprintf("overlay: a mount needs lowerdir=… options\n");
    return -22;
}

static int ovl_umount(struct dentry* mp) {
    /* The VFS frees every node (they are the dentries' inodes) and the root;
     * the mount record is the root's. */
    if (mp && mp->inode) kfree(mp->inode->private);
    if (mp && mp->inode) mp->inode->private = NULL;
    return 0;
}

static void ovl_evict(struct inode* in) { (void)in; }

static struct fs_type ovl_fs_type = {
    .name       = "overlay",
    .mount      = ovl_mount_plain,
    .next       = NULL,
    .umount     = ovl_umount,
    .evict      = ovl_evict,
    .mount_data = ovl_mount_data,
};

static int overlayfs_module_init(void) { return vfs_register_fs(&ovl_fs_type); }
MODULE("overlayfs", "fs", overlayfs_module_init);

/* ---- `ovltest` — the contract above, each rule made to show itself ---------
 *
 * Two lower layers (l1 above l2), an upper and a work directory under
 * /tmp/ovt, mounted at /tmp/ovt/m.  l1 hides l2's `a` with its own, deletes
 * l2's `gone` with a whiteout and replaces l2's `od` with an OPAQUE directory;
 * `d` exists in both and must merge.  Every check reads the merged view AND,
 * where it matters, the layers underneath — a copy-up that wrote into a lower
 * layer would pass a merged-view-only test. */
#include "shellcmd.h"

static int t_write(const char* path, const char* text, int flags) {
    struct file* f = vfs_open(path, flags);
    if (!f) return -1;
    ssize_t r = vfs_write(f, text, s_len(text));
    vfs_close(f);
    return r == (ssize_t)s_len(text) ? 0 : -1;
}
static int t_read(const char* path, char* out, size_t cap) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { out[0] = 0; return -1; }
    ssize_t r = vfs_read(f, out, cap - 1);
    vfs_close(f);
    out[r > 0 ? r : 0] = 0;
    return r < 0 ? -1 : 0;
}
/* How many names a directory lists (-1 when `must` is missing or `mustnot`
 * present). */
static int t_count(const char* path, const char* must, const char* mustnot) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    struct dirent de;
    int n = 0, saw = must ? 0 : 1, bad = 0;
    while (vfs_readdir(f, &de) > 0) {
        if (s_eq(de.name, ".") || s_eq(de.name, "..")) continue;
        n++;
        if (must && s_eq(de.name, must)) saw = 1;
        if (mustnot && s_eq(de.name, mustnot)) bad = 1;
    }
    vfs_close(f);
    return (saw && !bad) ? n : -1;
}
static int t_type(const char* path) {
    int d;
    struct inode* in = vfs_watch_inode(path, 0, &d);   /* resolves without following */
    return in ? (int)in->type : -1;
}

static void rm_tree(const char* p) { vfs_unlink_recursive(p); }

static void cmd_ovltest(const char* args) {
    (void)args;
    int ok = 1;
    char buf[64];
    #define OT(cond, what) do { int _c = (cond); kprintf("ovl: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    vfs_umount("/tmp/ovt/m");
    vfs_umount("/tmp/ovt/r");
    rm_tree("/tmp/ovt");
    vfs_mkdir("/tmp");
    const char* dirs[] = { "/tmp/ovt", "/tmp/ovt/l1", "/tmp/ovt/l2", "/tmp/ovt/u", "/tmp/ovt/w",
                           "/tmp/ovt/m", "/tmp/ovt/r", "/tmp/ovt/l2/d", "/tmp/ovt/l1/d",
                           "/tmp/ovt/l2/od", "/tmp/ovt/l1/od", NULL };
    for (int i = 0; dirs[i]; i++) vfs_mkdir(dirs[i]);
    t_write("/tmp/ovt/l2/a", "low2-a", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l1/a", "low1-a", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l1/b", "low1-b", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l2/gone", "gone", VFS_WRONLY | VFS_CREATE);
    OT(vfs_mkwhiteout("/tmp/ovt/l1/gone") == 0, "a whiteout in l1");
    t_write("/tmp/ovt/l2/d/x", "x", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l2/d/y", "y", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l1/d/z", "z", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l2/od/old", "old", VFS_WRONLY | VFS_CREATE);
    t_write("/tmp/ovt/l1/od/new", "new", VFS_WRONLY | VFS_CREATE);
    OT(vfs_inode_flag("/tmp/ovt/l1/od", 0, VFS_IF_OPAQUE, 1) == 0, "l1/od marked opaque");

    OT(vfs_mount_data("overlay", "/tmp/ovt/m",
       "lowerdir=/tmp/ovt/l1:/tmp/ovt/l2,upperdir=/tmp/ovt/u,workdir=/tmp/ovt/w") == 0, "mount");
    OT(vfs_mount_data("overlay", "/tmp/ovt/r", "lowerdir=/tmp/ovt/l1,redirect_dir=on") != 0,
       "an unsupported option is refused");
    OT(t_read("/tmp/ovt/m/a", buf, sizeof buf) == 0 && s_eq(buf, "low1-a"), "the higher lower layer wins (a)");
    OT(t_read("/tmp/ovt/m/b", buf, sizeof buf) == 0 && s_eq(buf, "low1-b"), "a file only in l1 (b)");
    OT(t_type("/tmp/ovt/m/gone") < 0, "a whiteout hides the lower file (gone)");
    OT(t_count("/tmp/ovt/m/d", "z", NULL) == 3, "directories merge (d = x y z)");
    OT(t_count("/tmp/ovt/m/od", "new", "old") == 1, "an opaque directory stops the merge (od = new)");
    OT(t_count("/tmp/ovt/m", "a", "gone") == 4, "the root lists a b d od, not gone");

    /* copy-up */
    OT(t_write("/tmp/ovt/m/b", "upper-b", VFS_WRONLY | VFS_TRUNC) == 0, "write b (truncating)");
    OT(t_read("/tmp/ovt/m/b", buf, sizeof buf) == 0 && s_eq(buf, "upper-b"), "b reads the new data");
    OT(t_read("/tmp/ovt/u/b", buf, sizeof buf) == 0 && s_eq(buf, "upper-b"), "b was copied up");
    OT(t_read("/tmp/ovt/l1/b", buf, sizeof buf) == 0 && s_eq(buf, "low1-b"), "the lower b is untouched");
    OT(t_write("/tmp/ovt/m/a", "X", VFS_WRONLY) == 0 &&
       t_read("/tmp/ovt/m/a", buf, sizeof buf) == 0 && s_eq(buf, "Xow1-a"),
       "a write without truncation keeps the copied data");
    OT(t_write("/tmp/ovt/m/d/z", "Z", VFS_WRONLY) == 0 && t_type("/tmp/ovt/u/d") == INODE_DIR,
       "a copy-up makes the parent directory in the upper layer");
    OT(vfs_chmod("/tmp/ovt/m/d/y", 0600) == 0 && t_type("/tmp/ovt/u/d/y") == INODE_FILE,
       "chmod copies up");

    /* create, unlink, whiteouts */
    OT(t_write("/tmp/ovt/m/newf", "n", VFS_WRONLY | VFS_CREATE) == 0 && t_type("/tmp/ovt/u/newf") == INODE_FILE,
       "a new file lands in the upper layer");
    OT(vfs_unlink("/tmp/ovt/m/d/x") == 0 && t_type("/tmp/ovt/u/d/x") == INODE_WHITEOUT &&
       t_type("/tmp/ovt/l2/d/x") == INODE_FILE, "unlink of a lower file leaves a whiteout");
    OT(t_count("/tmp/ovt/m/d", "z", "x") == 2, "d lists y z");
    OT(vfs_unlink("/tmp/ovt/m/d") != 0, "rmdir of a non-empty merged directory is refused");
    OT(vfs_unlink("/tmp/ovt/m/d/y") == 0 && vfs_unlink("/tmp/ovt/m/d/z") == 0, "empty d");
    OT(vfs_unlink("/tmp/ovt/m/d") == 0 && t_type("/tmp/ovt/u/d") == INODE_WHITEOUT &&
       t_type("/tmp/ovt/m/d") < 0, "rmdir leaves a whiteout");
    OT(vfs_mkdir("/tmp/ovt/m/d") == 0 && t_count("/tmp/ovt/m/d", NULL, NULL) == 0 &&
       vfs_inode_flag("/tmp/ovt/u/d", 0, VFS_IF_OPAQUE, 0) == 1,
       "mkdir over a whiteout makes an empty opaque directory");
    OT(vfs_unlink("/tmp/ovt/m/newf") == 0 && t_type("/tmp/ovt/u/newf") < 0,
       "unlink of an upper-only file leaves no whiteout");

    /* rename */
    OT(vfs_rename("/tmp/ovt/m/b", "/tmp/ovt/m/b2") == 0 &&
       t_read("/tmp/ovt/m/b2", buf, sizeof buf) == 0 && s_eq(buf, "upper-b") &&
       t_type("/tmp/ovt/m/b") < 0 && t_type("/tmp/ovt/u/b") == INODE_WHITEOUT,
       "rename of a copied-up file whites out the old name");
    OT(vfs_rename("/tmp/ovt/m/od", "/tmp/ovt/m/od2") == -18, "rename of a lower directory is EXDEV");

    /* read-only overlay */
    OT(vfs_mount_data("overlay", "/tmp/ovt/r", "lowerdir=/tmp/ovt/l1:/tmp/ovt/l2") == 0,
       "a read-only overlay mounts");
    OT(t_read("/tmp/ovt/r/a", buf, sizeof buf) == 0 && s_eq(buf, "low1-a"), "and reads");
    OT(t_write("/tmp/ovt/r/c", "c", VFS_WRONLY | VFS_CREATE) != 0 && t_type("/tmp/ovt/l1/c") < 0,
       "and refuses a create without touching the layer");

    /* An UNRELATED file's cached page, held the way a MAP_SHARED mapping
     * holds it, must stay coherent across the unmount: the unmount used to
     * drop the whole page cache, after which writes no longer reached mapped
     * pages (containerd's bbolt read a stale freelist and panicked). */
    /* A bind INTO the overlay (as runc binds /etc/hosts into a container's
     * root) must end with the unmount: a record left naming a freed dentry
     * was a write into freed memory at the next unbind. */
    int nb0 = vfs_bind_count();
    t_write("/tmp/ovt-hosts", "127.0.0.1 x", VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    OT(vfs_bind("/tmp/ovt-hosts", "/tmp/ovt/m/a") == 0 && vfs_bind_count() == nb0 + 1,
       "a file bound into the overlay");
    t_write("/tmp/ovt-pc", "AAAA", VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    struct file* pf = vfs_open("/tmp/ovt-pc", VFS_RDONLY);
    pmm_phys_t fr = 0;
    int held = pf && pcache_map_page(pf, 0, &fr) == 0;
    OT(vfs_umount("/tmp/ovt/m") == 0 && vfs_umount("/tmp/ovt/r") == 0, "unmount both");
    if (held) {
        t_write("/tmp/ovt-pc", "BBBB", VFS_WRONLY);
        const uint8_t* pg = (const uint8_t*)kmap_frame(fr);
        int coherent = pg && pg[0] == 'B';
        kunmap_frame((void*)pg);
        if (vmm_frame_unshare(fr)) pmm_free_frame(fr);
        OT(coherent, "another file's mapped page stays coherent across the unmount");
    } else {
        OT(0, "another file's mapped page stays coherent across the unmount (no page)");
    }
    if (pf) vfs_close(pf);
    vfs_unlink("/tmp/ovt-pc");
    OT(vfs_bind_count() == nb0, "the unmount ended the bind into it");
    vfs_unlink("/tmp/ovt-hosts");
    OT(t_read("/tmp/ovt/l1/a", buf, sizeof buf) == 0 && s_eq(buf, "low1-a") &&
       t_read("/tmp/ovt/u/a", buf, sizeof buf) == 0 && s_eq(buf, "Xow1-a"),
       "after the unmount the layers hold exactly the changes");
    rm_tree("/tmp/ovt");
    #undef OT
    kprintf("ovl: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(ovltest) = { "ovltest", "", "overlayfs: merge, whiteouts, opaque dirs, copy-up, rename, read-only",
                       SHELL_G_TEST, cmd_ovltest, SHELL_P_ANY };
