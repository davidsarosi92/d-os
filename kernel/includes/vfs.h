/* =============================================================================
 * vfs.h — Virtual File System public interface.
 *
 * The VFS sits between the shell / consumer code and concrete filesystem
 * drivers (ramfs today; future: exfat, fat32, ext2, devfs).  Consumers see
 * a POSIX-shaped API — open, read, write, readdir, close, mkdir — and never
 * need to know which fs is backing a given path.
 *
 * Design after the M12 refactor:
 *
 *   - All sizes are `uint64_t` (was `size_t` / `uint32_t`).  Real
 *     filesystems hand out >4 GiB files; the API must not be the bottleneck.
 *   - `file_ops.read` / `write` take an explicit byte offset.  The VFS
 *     layer owns `file->pos` and feeds it to the fs as `off`, then bumps
 *     it by the returned byte count.  Filesystem implementations are
 *     therefore pure offset-addressed and never read `f->pos` directly —
 *     the natural shape for FAT / exFAT / NTFS / ext.
 *   - `inode_ops` carries directory mutators (lookup, create, mkdir,
 *     unlink).  ramfs registers a populated table; devfs / procfs leave
 *     it NULL (their trees are constructed at init).  exFAT will register
 *     a lazy `lookup` so the dentry tree fills on demand.
 *   - `fs_type.mount` receives the backing `block_device*`.  In-memory
 *     filesystems pass NULL via `vfs_mount(fs, path, NULL)`.
 *
 * Path conventions today:
 *   - Absolute paths only ("/foo/bar").  No CWD, no relative paths.
 *   - "/" is the root mount.
 *   - Components separated by '/', max NAME_MAX bytes each.
 *   - No symlinks, no `.`/`..`.
 *
 * ============================================================================= */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_NAME_MAX  63                /* per-component max bytes (NUL excluded) */

/* No `<sys/types.h>` in a freestanding build — supply our own. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif

/* Forward decl — block.h is the canonical owner.  We avoid pulling it
 * in here so non-storage consumers (devfs, procfs) don't transitively
 * pick up the block layer header. */
struct block_device;

/* ------------------------------------------------------------------- */
/* Inode types — what kind of object an inode represents.              */
/* ------------------------------------------------------------------- */
enum inode_type {
    INODE_FILE,
    INODE_DIR,
    INODE_DEVICE,
};

/* Open flags.  Mirror the lower bits of POSIX O_* but we own the values. */
#define VFS_RDONLY  0x01
#define VFS_WRONLY  0x02
#define VFS_RDWR    (VFS_RDONLY | VFS_WRONLY)
#define VFS_CREATE  0x04
#define VFS_TRUNC   0x08

/* Forward decls — full layouts in vfs.c. */
struct inode;
struct dentry;
struct file;

/* Returned by readdir, one per child entry. */
struct dirent {
    char            name[VFS_NAME_MAX + 1];
    enum inode_type type;
    uint64_t        size;
};

/* Per-inode operations on an open file handle.
 *
 * `read` / `write` are random-access: they receive an explicit byte
 * `off` and never touch `f->pos`.  The VFS layer is the sole owner of
 * `f->pos`: it passes the current value as `off` and bumps `f->pos`
 * by the non-negative return value after a successful call.
 *
 * `readdir` is iterator-shaped: it uses `f->pos` as an opaque cookie
 * (the implementation defines its meaning — ramfs uses a 0-based child
 * index; exFAT will use a cluster + entry offset).
 *
 * Any field may be NULL — vfs_read/vfs_write/vfs_readdir return -1 if
 * the underlying op is missing. */
struct file_ops {
    ssize_t (*read)   (struct file*, void* buf, size_t n, uint64_t off);
    ssize_t (*write)  (struct file*, const void* buf, size_t n, uint64_t off);
    int     (*readdir)(struct file*, struct dirent* out);
    int     (*close)  (struct file*);
};

/* Directory-inode operations: namespace mutators + lazy lookup.
 *
 * `lookup` is consulted on a dentry cache miss when resolving a path
 * component.  Filesystems with eager dentry trees (ramfs, devfs, procfs)
 * leave it NULL; lazy fs (exFAT) populate it.  On success the callback
 * returns 0 and writes a freshly-allocated `struct inode*` to `*out`;
 * the VFS layer attaches it under the parent dentry.  Return -1 if the
 * name does not exist.
 *
 * `create` / `mkdir` allocate a new inode of the appropriate type and
 * return it; the VFS attaches the dentry.  Return -1 on failure.
 *
 * `unlink` removes a name from the directory (regular files and empty
 * dirs for now).  Returns 0 on success.
 *
 * Any of these may be NULL — that indicates the op is not supported
 * (e.g. devfs/procfs reject create). */
struct inode_ops {
    int (*lookup)(struct inode* dir, const char* name, struct inode** out);
    int (*create)(struct inode* dir, const char* name, struct inode** out);
    int (*mkdir) (struct inode* dir, const char* name, struct inode** out);
    /* `child` is the inode being removed — passed in because eager-tree
     * filesystems (ramfs) have no private name index to look it up by;
     * the VFS resolves the dentry anyway.  The fs frees its private
     * data + the inode; the VFS frees the dentry.  (Signature fixed in
     * M22.1 when the first implementation landed.) */
    int (*unlink)(struct inode* dir, const char* name, struct inode* child);
    /* M22.5 — rename `child` from oldname to newname INSIDE `dir`
     * (same-directory only; cross-directory moves are a later
     * milestone).  The fs updates whatever private naming state it
     * keeps (ramfs: nothing — names live in VFS dentries); the VFS
     * rewrites the dentry on success.  NULL = unsupported (exFAT
     * would need a directory-entry rewrite — deferred). */
    int (*rename)(struct inode* dir, const char* oldname,
                  const char* newname, struct inode* child);
};

/* Inode — owned by the fs that created it.  `private` is fs-defined. */
struct inode {
    enum inode_type         type;
    uint64_t                size;
    void*                   private;       /* fs-private data */
    const struct file_ops*  ops;           /* per-open operations */
    const struct inode_ops* dir_ops;       /* directory mutators (NULL = read-only / non-dir) */
    /* §M32 stage 5 — ownership and permission bits.
     *
     * `mode` holds the usual rwx triples (0755, 0644, 0700).  **Mode 0 is a
     * legal value meaning "nobody may do anything", which is why it must not
     * also be what a forgotten initialiser produces** — the trick cred.h uses
     * (arrange for kcalloc's zero to be the right answer) does not work here,
     * because the safe answer would brick the machine rather than protect it.
     *
     * So every inode goes through `vfs_inode_defaults()`, the same one-
     * initialiser-every-site shape as §M49's `task_sched_defaults`, and an
     * audit checks at RUNTIME that nothing is walking around with mode 0.
     * There are six construction sites across four filesystems; the audit is
     * what makes the seventh visible. */
    int      owner_uid;
    int      owner_gid;
    uint32_t mode;
};

/* Directory entry — name + inode pointer + tree links.  We keep an
 * intrusive tree so vfs.c can walk the namespace without per-fs hooks
 * for path resolution. */
struct dentry {
    char            name[VFS_NAME_MAX + 1];
    struct inode*   inode;
    struct dentry*  parent;
    struct dentry*  children;               /* first child */
    struct dentry*  sibling;                /* next sibling under same parent */
};

/* Open file handle — per-call-to-open instance state. */
struct file {
    struct inode*  inode;
    struct dentry* dentry;                  /* useful for readdir cursor */
    uint64_t       pos;                     /* read/write cursor — owned by VFS layer */
    int            flags;
    void*          private;                 /* fs-private state */
};

/* fs_type — describes a filesystem implementation (ramfs, exfat, ...). */
struct fs_type {
    const char* name;
    /* Populate `mountpoint` (a directory dentry already in the tree)
     * with whatever inode + initial children this fs wants to expose.
     * `dev` is the backing block device, or NULL for in-memory
     * filesystems that don't need one.  Returns 0 on success. */
    int (*mount)(struct block_device* dev, struct dentry* mountpoint);
    struct fs_type* next;                   /* registry link */
    /* §M32 stage 5 — DOES THIS FILESYSTEM STORE OWNERSHIP ACROSS A REBOOT?
     *
     * A declared capability, in the shape §M33 gave driver domains: today
     * EVERY filesystem here answers 0, and that uniform answer is doing real
     * work rather than being a placeholder.  ramfs, devfs and procfs are
     * volatile, so their modes die with the machine; exFAT has no owner field,
     * no mode field and nowhere to put them, so what this kernel enforces on
     * /mnt is true until the power goes off and invisible to every other
     * operating system.
     *
     * The alternative was a side-car ownership map on the volume — a second
     * source of truth that must be written in the same operation as the file
     * or the two drift.  Declined on §M33's rule: a boundary that is not
     * enforced must not be claimed.  What IS done instead is that `mount`
     * prints this, so "are these permissions real after a reboot" is
     * answerable on the machine rather than only in a design document.
     *
     * An ext2 implementation would be the first to answer 1, which is what
     * this field is for.
     *
     * Every fs_type in the tree uses DESIGNATED initialisers, so a filesystem
     * that does not mention this field gets 0 — and 0 is the conservative
     * answer.  A new filesystem that forgets to declare is assumed NOT to
     * persist ownership, which UNDER-claims: it reports permissions as
     * advisory when they might be real, rather than promising enforcement it
     * does not deliver.  The default errs in the direction that cannot become
     * isolation theatre. */
    int stores_ownership;
};

/* ------------------------------------------------------------------- */
/* Public API.                                                          */
/* ------------------------------------------------------------------- */

/* One-time init.  Allocates the root dentry.  Call after kmalloc_init. */
void vfs_init(void);

/* Register a filesystem implementation.  After registration the fs can
 * be used as the second argument to `vfs_mount`. */
int  vfs_register_fs(struct fs_type* fs);

/* Mount filesystem `fs_name` at absolute path `path`, backed by the
 * block device registered as `dev_name`.  Pass `dev_name == NULL` for
 * in-memory filesystems (ramfs / devfs-as-fs).  `path` must be an
 * existing empty directory, or "/" for the very first mount. */
int  vfs_mount(const char* fs_name, const char* path, const char* dev_name);

/* Open a file or directory by absolute path.  Returns NULL on error.
 * Free the returned handle via `vfs_close`. */
struct file* vfs_open(const char* path, int flags);

int     vfs_close  (struct file* f);
ssize_t vfs_read   (struct file* f, void* buf, size_t n);
ssize_t vfs_write  (struct file* f, const void* buf, size_t n);
int     vfs_readdir(struct file* f, struct dirent* out);

/* Convenience operations on paths (no need to keep a file handle).
 * They dispatch through `parent_inode->dir_ops`. */
int  vfs_mkdir(const char* path);
int  vfs_create(const char* path);          /* zero-byte regular file */

/* Remove a regular file or an EMPTY directory.  Returns 0 on success,
 * -1 on resolve/ops failure, -2 if the directory is not empty.
 * Caveat (single-user teaching kernel): open file handles to the
 * removed inode are NOT tracked — close them first. */
int  vfs_unlink(const char* path);

/* M22.5 — rename WITHIN one directory: both paths must share the same
 * parent.  Returns 0, -1 on failure (unsupported fs, missing source),
 * -2 if the new name already exists. */
int  vfs_rename(const char* oldpath, const char* newpath);

/* M22.5 — copy a regular file (read/write loop through the fs ops;
 * dst is created/truncated).  Returns 0 / -1. */
int  vfs_copy(const char* src, const char* dst);

/* M22.5 — remove a file or a directory TREE (depth-limited to 8).
 * Returns 0 on success; on failure the tree may be partially removed. */
int  vfs_unlink_recursive(const char* path);

/* ---------------------------------------------------------------------------
 * §M32 stage 5 — ownership and permissions.
 * ------------------------------------------------------------------------- */

/* What a caller wants to do.  The same three bits as the mode's triples, so
 * the check is a mask test rather than a translation. */
#define VFS_PERM_READ  4
#define VFS_PERM_WRITE 2
#define VFS_PERM_EXEC  1

/* THE ONE INITIALISER.  Every filesystem calls this immediately after
 * allocating an inode — root-owned, 0755 for a directory and 0644 for
 * anything else.  See the note on `mode` in struct inode for why this cannot
 * be left to kcalloc. */
void vfs_inode_defaults(struct inode* ino);

/* May the CURRENT task do `want` to this inode?  1 = yes.
 *
 * Exported rather than private to vfs.c because the execute bit has an
 * enforcement point that `open` cannot serve: a program is opened for READING
 * in order to be run, so only the loader can distinguish "may read this file"
 * from "may run it" (§M32 stage 6, proc.c). */
int  vfs_permitted(const struct inode* ino, int want);

/* chmod/chown.  Gated: only the owner or an admin may chmod, and only an admin
 * may chown — giving a file away is how a quota or an audit trail is escaped,
 * and there is no reason an ordinary user needs it. */
int  vfs_chmod(const char* path, uint32_t mode);
int  vfs_chown(const char* path, int uid, int gid);

/* One mounted filesystem.  The VFS kept no record of its mounts before §M32,
 * so nothing could answer "which filesystem is this path on" — the question
 * ownership-persistence turns out to be. */
struct vfs_mount {
    char        path[64];
    const char* fs_name;
    int         stores_ownership;       /* copied from the fs_type at mount */
};

int  vfs_mount_count(void);
const struct vfs_mount* vfs_mount_at(int i);
/* Longest-prefix match: /mnt/foo answers with the exFAT mount, not the root. */
const struct vfs_mount* vfs_mount_for(const char* path);

/* Does the filesystem holding `path` actually STORE ownership, or are the bits
 * above only true until the power goes off?  1 / 0, or -1 when the path is on
 * no known mount.  Today every filesystem here answers 0 — see the note on
 * `fs_type.stores_ownership`, and `mount`, which prints it. */
int  vfs_ownership_is_persistent(const char* path);

/* Internal helper exposed for filesystems implementing `mount` and
 * lazy `lookup`: attach a freshly-allocated dentry+inode pair under an
 * existing dentry.  Returns the new dentry (caller does not free). */
struct dentry* vfs_attach_child(struct dentry* parent, const char* name,
                                struct inode* inode);

/* Diagnostic — used by the `ls` shell command at root. */
struct dentry* vfs_root(void);

#endif
