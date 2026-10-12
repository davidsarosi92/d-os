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

struct ofile;   /* §M90 rung 6 — the open hook's argument */
struct inode;
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

    /* Called when a handle on this node is closed.  Optional, and appended
     * rather than inserted (§M58's positional-initialiser scar).
     *
     * It exists because a STREAM device needs a drain point: /dev/dsp buffers
     * writes up to one period so that a writer using small chunks does not
     * restart the DMA engine every few milliseconds, and without a close hook
     * the final partial period would simply never be played — the tail of
     * every sound silently missing. */
    int     (*close)(void* ctx);

    void* ctx;                  /* opaque, passed back into the callbacks */

    /* Internal use — link in the pending-or-live list.  Drivers should not
     * touch this; devfs.c manages it. */
    struct devfs_node* _next;

    /* §M90 rung 6 — APPENDED.  `open` makes an open of this node something
     * other than a stream of read/write calls: it receives the new ofile and
     * may turn it into another kind (a pty end — tty.c).  `noctty` is
     * O_NOCTTY.  0, or a negative errno that fails the open. */
    int     (*open)(void* ctx, struct ofile* o, int noctty);
    /* The Linux device number, when the name table in devfs_devnum does not
     * know it (/dev/pts/N is 136:N). */
    int      has_devnum;
    uint32_t dev_major, dev_minor;
    struct inode* _inode;       /* devfs.c: the inode it was attached as */
    /* §M90 rung 6 — APPENDED.  The inode number stat reports, 0 = derived.
     * devpts has fixed ones (ptmx 2, pty N is N+3) and runc VERIFIES the
     * ptmx handle it is given against them ("ptmx handle has wrong inode
     * number") before it trusts it with a container's console. */
    uint64_t ino;
};
struct ofile;
struct dentry;
/* §M90 rung 6 — a node in ANOTHER directory than /dev (a devpts instance):
 * the inode it became, or NULL; and its removal (open handles fail cleanly,
 * as after devfs_unregister). */
struct inode* devfs_attach_into(struct dentry* dir, struct devfs_node* node);
void          devfs_detach_from(struct dentry* dir, struct devfs_node* node);
struct inode* devfs_node_inode(struct devfs_node* node);
/* The open hook of a devfs inode: 1 = it has none, else what the hook said. */
int           devfs_open_hook(struct inode* in, struct ofile* o, int noctty);
/* The fixed inode number a node declares (0 = none). */
uint64_t      devfs_ino(struct inode* in);

/* §M90 — Linux major:minor of a devfs inode (devfs.c); 1 char, 0 block, -1 not ours. */
struct inode;
int devfs_devnum(struct inode* in, uint32_t* major, uint32_t* minor);

/* Register a node.  If devfs_init has already cached the /dev dentry, the
 * node is attached immediately; otherwise it queues until devfs_init
 * flushes the queue. */
int  devfs_register(struct devfs_node* node);
/* §M87 — withdraw a node (a device that went away).  Open handles keep
 * working in the sense that every call on them fails cleanly. */
int  devfs_unregister(struct devfs_node* node);

/* One-shot init.  Resolves /dev (must already exist via ramfs's mount),
 * registers built-in nodes (/dev/null, /dev/zero), and flushes any
 * driver registrations that arrived during the pre-init phase. */
void devfs_init(void);

#endif
