/* =============================================================================
 * fsnotify.h — filesystem change notification (§M90 rung 3), and inotify, the
 * Linux interface over it.  See kernel/fs/inotify.c for the design.
 *
 * The event bits ARE Linux's IN_* values, so the inotify layer hands them to
 * a guest without a translation table (the same choice syscall.h made for
 * POLL*).  Producers (the VFS, cgroupfs) report an event on an INODE; a watch
 * names an inode; a directory's watch also receives its children's events,
 * carrying the child's name — exactly inotify's model.
 * ============================================================================= */
#ifndef DOS_FSNOTIFY_H
#define DOS_FSNOTIFY_H

#include <stdint.h>
#include <stddef.h>

#define FSN_ACCESS        0x00000001u
#define FSN_MODIFY        0x00000002u
#define FSN_ATTRIB        0x00000004u
#define FSN_CLOSE_WRITE   0x00000008u
#define FSN_CLOSE_NOWRITE 0x00000010u
#define FSN_OPEN          0x00000020u
#define FSN_MOVED_FROM    0x00000040u
#define FSN_MOVED_TO      0x00000080u
#define FSN_CREATE        0x00000100u
#define FSN_DELETE        0x00000200u
#define FSN_DELETE_SELF   0x00000400u
#define FSN_MOVE_SELF     0x00000800u
#define FSN_ALL_EVENTS    0x00000FFFu
#define FSN_UNMOUNT       0x00002000u
#define FSN_Q_OVERFLOW    0x00004000u
#define FSN_IGNORED       0x00008000u
#define FSN_ISDIR         0x40000000u

struct inode;
struct dentry;
struct inotify;
struct ofile;

/* An event on `in` itself (`name` NULL) or, when `in` is a directory, on its
 * child `name`.  Callable from any context (it takes only a spinlock and never
 * allocates); returns at once when nothing is watched anywhere. */
void fsnotify_inode(struct inode* in, uint32_t mask, const char* name);

/* An event on the file at `d`: delivered to watches on the file and, with the
 * file's name, to watches on its parent directory.  The caller keeps `d`
 * valid (holds the VFS namespace lock, or an open file on it). */
void fsnotify_dentry(struct dentry* d, uint32_t mask);

/* A rename: MOVED_FROM on the old directory and MOVED_TO on the new one,
 * paired by one cookie, plus MOVE_SELF on the moved inode. */
void fsnotify_move(struct inode* odir, const char* oname, struct inode* ndir,
                   const char* nname, struct inode* child);

/* `in` is about to be freed (its last name removed): DELETE_SELF and IGNORED
 * to its watchers, and every watch on it dropped — a watch keyed by an inode
 * pointer must never outlive the inode, or the next inode at that address
 * would inherit somebody else's watch. */
void fsnotify_inode_gone(struct inode* in);

/* ---- inotify descriptors --------------------------------------------------- */

/* inotify_init1(flags): the new descriptor, or a negative Linux errno.
 * IN_NONBLOCK 0x800 and IN_CLOEXEC 0x80000 are the accepted flags (the
 * caller applies CLOEXEC). */
int  sys_inotify_init(int flags);
/* inotify_add_watch(fd, path, mask): the watch descriptor or -errno.  `path`
 * is a KERNEL copy, as the caller's view resolves it. */
int  sys_inotify_add_watch(int fd, const char* path, uint32_t mask);
int  sys_inotify_rm_watch(int fd, int wd);

long inotify_read(struct inotify* in, void* buf, size_t n, int block);
int  inotify_can_read(struct inotify* in);
int  inotify_pending_bytes(struct inotify* in);   /* FIONREAD */
void inotify_set_owner(struct inotify* in, struct ofile* o);
void inotify_close(struct inotify* in);

#endif
