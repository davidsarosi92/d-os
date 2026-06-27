/* =============================================================================
 * devfs.c — pseudo-filesystem that publishes drivers under /dev.
 *
 * Layout: when `devfs_init` runs, it
 *   1. resolves the existing `/dev` directory (created by ramfs's mount);
 *   2. registers two built-in character devices: `/dev/null` and `/dev/zero`;
 *   3. walks `pending_head` and attaches every queued driver-registered
 *      node into `/dev`.
 *
 * After init, `devfs_register` attaches new nodes immediately.
 *
 * For each node we allocate a fresh `struct inode` whose `ops` point at
 * `devfs_file_ops` (defined below) and whose `private` is the
 * `devfs_node*`.  The VFS open path then reaches our adapter, which
 * forwards each call to the node's read/write/ioctl callbacks.
 * ============================================================================= */

#include "devfs.h"
#include "vfs.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* Pending queue + cached /dev dentry.                                    */
/* ---------------------------------------------------------------------- */

static struct devfs_node* pending_head = NULL;
static struct dentry*     dev_dir      = NULL;

/* ---------------------------------------------------------------------- */
/* VFS file_ops adapter — forwards to the devfs_node callbacks.           */
/* ---------------------------------------------------------------------- */

/* file_ops signatures after the M12 VFS refactor: read/write receive the
 * byte offset directly.  The VFS layer owns `f->pos`, so devfs does NOT
 * bump it — it just forwards `off` to the node callback. */
static ssize_t adapter_read(struct file* f, void* buf, size_t n, uint64_t off) {
    if (!f || !f->inode) return -1;
    struct devfs_node* d = (struct devfs_node*)f->inode->private;
    if (!d || !d->read) return -1;
    return d->read(d->ctx, buf, n, off);
}

static ssize_t adapter_write(struct file* f, const void* buf, size_t n,
                             uint64_t off) {
    if (!f || !f->inode) return -1;
    struct devfs_node* d = (struct devfs_node*)f->inode->private;
    if (!d || !d->write) return -1;
    return d->write(d->ctx, buf, n, off);
}

static int adapter_close(struct file* f) {
    (void)f;
    return 0;
}

static const struct file_ops devfs_file_ops = {
    .read    = adapter_read,
    .write   = adapter_write,
    .readdir = NULL,                     /* not a directory */
    .close   = adapter_close,
};

/* ---------------------------------------------------------------------- */
/* Attach helpers.                                                        */
/* ---------------------------------------------------------------------- */

/* Allocate an inode + dentry pair for `node` and hang it under /dev.
 * Returns 0 on success.  Caller guarantees `dev_dir` is non-NULL. */
static int attach_node(struct devfs_node* node) {
    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!ino) return -1;

    ino->type    = INODE_DEVICE;
    ino->size    = 0;
    ino->ops     = &devfs_file_ops;
    ino->private = node;

    if (!vfs_attach_child(dev_dir, node->name, ino)) {
        kfree(ino);
        return -2;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Built-in nodes: /dev/null and /dev/zero.                               */
/*                                                                        */
/* Kept here (not in their own file) because they have no driver — they   */
/* are implemented entirely by the kernel and only exist in the devfs     */
/* sense.  Linux puts the analogues in drivers/char/mem.c.                */
/* ---------------------------------------------------------------------- */

static ssize_t null_read (void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)buf; (void)n; (void)off;
    return 0;                                    /* always EOF */
}

static ssize_t null_write(void* ctx, const void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)buf; (void)off;
    return (ssize_t)n;                           /* swallow everything */
}

static struct devfs_node node_null = {
    .name = "null",  .kind = DEVFS_CHAR,
    .read = null_read, .write = null_write, .ioctl = NULL, .ctx = NULL,
    ._next = NULL,
};

static ssize_t zero_read (void* ctx, void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;
    char* b = (char*)buf;
    for (size_t i = 0; i < n; i++) b[i] = 0;
    return (ssize_t)n;                           /* infinite zeros — caller bounds */
}

static struct devfs_node node_zero = {
    .name = "zero", .kind = DEVFS_CHAR,
    .read = zero_read, .write = null_write,      /* writes go nowhere */
    .ioctl = NULL, .ctx = NULL,
    ._next = NULL,
};

/* ---------------------------------------------------------------------- */
/* Public API.                                                            */
/* ---------------------------------------------------------------------- */

int devfs_register(struct devfs_node* node) {
    if (!node || !node->name) return -1;
    if (dev_dir) {
        return attach_node(node);                /* live: attach now */
    }
    /* Pre-init: queue.  Push to head; order doesn't matter. */
    node->_next = pending_head;
    pending_head = node;
    return 0;
}

void devfs_init(void) {
    /* Resolve /dev — must exist (ramfs creates it at mount). */
    struct file* f = vfs_open("/dev", VFS_RDONLY);
    if (!f) {
        kprintf("devfs: /dev not found — ramfs mount failed?\n");
        return;
    }
    dev_dir = f->dentry;
    vfs_close(f);                                /* dentry stays in tree */

    /* Built-ins first so their registration order is deterministic. */
    attach_node(&node_null);
    attach_node(&node_zero);

    /* Flush any driver registrations queued before init. */
    int flushed = 0;
    while (pending_head) {
        struct devfs_node* n = pending_head;
        pending_head = n->_next;
        n->_next = NULL;
        if (attach_node(n) == 0) flushed++;
    }

    kprintf("devfs: ready, /dev populated (%d driver nodes + 2 built-ins)\n",
            flushed);
}
