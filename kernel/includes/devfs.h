/* =============================================================================
 * devfs.h — drivers as files under /dev.
 *
 * Each driver that wants user-space-style access creates a `struct
 * devfs_node` (statically — usually a `static` in the driver file) and calls
 * `devfs_register(&node)`.  The node carries the driver's read / write /
 * ioctl callbacks; devfs wraps them in a VFS `file_ops` adapter so that
 * `cat /dev/<name>` and friends just work.
 *
 * Why not a real filesystem (with its own `struct fs_type` mount) yet?
 * Linux's devtmpfs piggybacks on tmpfs for similar reasons: dev nodes are
 * synthetic files attached to a host directory tree.  In our case we
 * attach them under /dev (which ramfs pre-creates), giving us the dentry
 * tree machinery for free without inventing a second mount table.  When
 * we later need true mount-point semantics (e.g. dev-namespace per
 * container), wrapping this in an `fs_type` is a small refactor.
 *
 * Init ordering: drivers may call `devfs_register` from their MODULE
 * init, which runs BEFORE devfs has cached the /dev dentry.  In that
 * case the node is queued in `pending_head` and attached in
 * `devfs_init` once the FS is up.  After devfs_init, calls attach
 * immediately.
 * ============================================================================= */

#ifndef DEVFS_H
#define DEVFS_H

#include <stdint.h>
#include <stddef.h>

/* `ssize_t` ourselves — VFS already typedefs it but devfs.h is sometimes
 * included before vfs.h, so we keep a local declaration to avoid an
 * include-order foot-gun.  Both must agree on the underlying type. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif

enum devfs_kind {
    DEVFS_CHAR,                 /* stream-oriented: serial, keyboard, ... */
    DEVFS_BLOCK,                /* fixed-size addressable units (later)   */
};

struct devfs_node {
    const char*     name;       /* path component under /dev, no slashes */
    enum devfs_kind kind;

    /* Operations.  Any may be NULL — devfs returns -1 if a missing op is
     * called.  `off` is the byte offset the VFS handle is at; for
     * sequential devices like serial / keyboard it can be ignored.
     * Block devices >4 GiB require the 64-bit width. */
    ssize_t (*read) (void* ctx, void* buf, size_t n, uint64_t off);
    ssize_t (*write)(void* ctx, const void* buf, size_t n, uint64_t off);
    int     (*ioctl)(void* ctx, int cmd, void* arg);

    void* ctx;                  /* opaque, passed back into the callbacks */

    /* Internal use — link in the pending-or-live list.  Drivers should not
     * touch this; devfs.c manages it. */
    struct devfs_node* _next;
};

/* Register a node.  If devfs_init has already cached the /dev dentry, the
 * node is attached immediately; otherwise it queues until devfs_init
 * flushes the queue. */
int  devfs_register(struct devfs_node* node);

/* One-shot init.  Resolves /dev (must already exist via ramfs's mount),
 * registers built-in nodes (/dev/null, /dev/zero), and flushes any
 * driver registrations that arrived during the pre-init phase. */
void devfs_init(void);

#endif
