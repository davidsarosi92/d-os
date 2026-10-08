/* =============================================================================
 * inotify.c — filesystem change notification and the Linux inotify interface.
 * §M90 rung 3, 2026-10-08.
 *
 * WHY.  Two consumers in the Docker chain could not work without it, and both
 * said so in their logs: containerd watches each container's memory.events to
 * learn of an OOM kill (that is how `docker inspect` comes to say OOMKilled
 * and how `docker run` reports exit 137 as an OOM), and dockerd's CDI device
 * registry watches /etc/cdi and /var/run/cdi.  Before this, inotify_init1 was
 * ENOSYS and both degraded with a warning — acceptable while memory.max was
 * not enforced, not once it is.
 *
 * MODEL — inotify's own, nothing invented:
 *   - a WATCH is (instance, inode, mask, wd).  It names an INODE, not a path:
 *     rename the file and the watch follows it; remove it and the watch ends
 *     (DELETE_SELF, then IGNORED).
 *   - a DIRECTORY's watch also receives its children's events, each carrying
 *     the child's name (fsnotify_dentry does the double delivery).
 *   - an instance owns a QUEUE of `struct inotify_event` records, read whole
 *     (a buffer too small for the next one is EINVAL, as on Linux), with a
 *     consecutive identical event coalesced and a single IN_Q_OVERFLOW (wd
 *     -1) when the queue is full.
 *
 * WHAT PRODUCES EVENTS.  The VFS: create / mkdir / mknod / symlink / link
 * (CREATE), unlink / rmdir (DELETE, and DELETE_SELF on the victim), rename
 * (MOVED_FROM / MOVED_TO with one cookie, MOVE_SELF), write (MODIFY), open
 * (OPEN), close (CLOSE_WRITE / CLOSE_NOWRITE), chmod / chown (ATTRIB).  And
 * cgroupfs, for files that change BY THEMSELVES: memory.events after an OOM
 * kill, pids.events after a refused fork.
 *
 * WHAT DOES NOT, said so a quiet watch is not misread:
 *   - IN_ACCESS (reads) is never generated: a read is the hottest path in the
 *     VFS and nothing in the chain asks for it.
 *   - Writes that bypass vfs_write — a page dirtied through a shared mapping,
 *     a block device written underneath a filesystem, procfs/sysfs values
 *     that change by themselves (other than the cgroup files above) — produce
 *     no MODIFY.  Linux has the same blind spot for mmap writes.
 *   - IN_UNMOUNT is not generated (unmounting a watched tree drops nothing).
 *   - IN_EXCL_UNLINK is accepted and has no effect.
 *
 * LOCKING.  One spinlock (irqsave) covers every watch and every queue.  It is
 * a leaf apart from the waitq lock taken inside it to wake a reader, so
 * fsnotify_inode() may be called from anywhere — under the VFS namespace
 * mutex, or from cgroupfs's OOM path inside a page-fault allocation.  Nothing
 * here allocates while delivering: each instance's queue is a fixed buffer.
 *
 * COST WHEN UNUSED.  g_nwatch is read first, without the lock: a machine with
 * no watch pays one load per VFS mutation.
 * ============================================================================= */

#include "fsnotify.h"
#include "fd.h"
#include "vfs.h"
#include "waitq.h"
#include "lock.h"
#include "task.h"
#include "kmalloc.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

#define IN_ONLYDIR      0x01000000u
#define IN_DONT_FOLLOW  0x02000000u
#define IN_EXCL_UNLINK  0x04000000u
#define IN_MASK_CREATE  0x10000000u
#define IN_MASK_ADD     0x20000000u
#define IN_ONESHOT      0x80000000u
#define IN_VALID_FLAGS  (FSN_ALL_EVENTS | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK | \
                         IN_MASK_CREATE | IN_MASK_ADD | IN_ONESHOT)

#define IN_WATCH_MAX    512          /* across the machine (Linux: per user, 8192) */
#define IN_QBYTES       16384        /* per instance; Linux's default is 16384 events */
#define IN_EVHDR        16           /* sizeof(struct inotify_event), every arch */

struct inotify {
    struct waitq   wq;
    struct ofile*  owner;
    int            next_wd;
    uint32_t       used;              /* bytes queued */
    uint32_t       last_off;          /* offset of the newest queued event */
    int            has_last;
    int            overflowed;        /* an IN_Q_OVERFLOW is queued */
    uint8_t        q[IN_QBYTES];
};

struct watch { struct inotify* in; struct inode* ino; uint32_t mask; int wd; };

static struct watch g_w[IN_WATCH_MAX];
static volatile int g_nwatch;
static spinlock_t   g_lock = SPINLOCK_INIT;
static uint32_t     g_cookie;

/* ---- the queue ------------------------------------------------------------- */

static size_t s_len(const char* s) { size_t n = 0; while (s && s[n]) n++; return n; }

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Is the newest queued event (wd, mask, cookie, name) exactly this one?
 * inotify coalesces such a repeat — a writer doing a thousand small writes
 * leaves one MODIFY, not a thousand. */
static int same_as_last(struct inotify* in, int wd, uint32_t mask, uint32_t cookie, const char* name) {
    if (!in->has_last) return 0;
    const uint8_t* e = in->q + in->last_off;
    if ((int)get32(e) != wd || get32(e + 4) != mask || get32(e + 8) != cookie) return 0;
    uint32_t len = get32(e + 12);
    size_t nl = s_len(name);
    if (!name) return len == 0;
    if (len < nl + 1) return 0;
    for (size_t i = 0; i < nl; i++) if (e[IN_EVHDR + i] != (uint8_t)name[i]) return 0;
    return e[IN_EVHDR + nl] == 0;
}

/* Append one event.  Caller holds g_lock.  Returns 1 when something was
 * queued (the reader must be woken). */
static int enqueue(struct inotify* in, int wd, uint32_t mask, uint32_t cookie, const char* name) {
    if (in->overflowed) return 0;
    if (same_as_last(in, wd, mask, cookie, name)) return 0;
    size_t nl = name ? s_len(name) : 0;
    /* The name is NUL-terminated and padded so the next record stays aligned
     * to the header's size — Linux pads to sizeof(struct inotify_event). */
    uint32_t len = name ? (uint32_t)((nl + 1 + IN_EVHDR - 1) & ~(size_t)(IN_EVHDR - 1)) : 0;
    uint32_t need = IN_EVHDR + len;
    if (in->used + need > IN_QBYTES - IN_EVHDR) {
        /* Full: one overflow record (wd -1), and nothing more until read. */
        uint8_t* e = in->q + in->used;
        put32(e, (uint32_t)-1); put32(e + 4, FSN_Q_OVERFLOW); put32(e + 8, 0); put32(e + 12, 0);
        in->last_off = in->used; in->has_last = 1;
        in->used += IN_EVHDR;
        in->overflowed = 1;
        return 1;
    }
    uint8_t* e = in->q + in->used;
    put32(e, (uint32_t)wd); put32(e + 4, mask); put32(e + 8, cookie); put32(e + 12, len);
    for (uint32_t i = 0; i < len; i++) e[IN_EVHDR + i] = i < nl ? (uint8_t)name[i] : 0;
    in->last_off = in->used; in->has_last = 1;
    in->used += need;
    return 1;
}

static void wake(struct inotify* in) {
    uint32_t f = waitq_lock(&in->wq);
    waitq_wake_all(&in->wq);
    waitq_unlock(&in->wq, f);
}

/* Remove watch slot i.  Caller holds g_lock. */
static void drop_slot(int i) {
    int last = g_nwatch - 1;
    if (i != last) g_w[i] = g_w[last];
    g_w[last].in = NULL; g_w[last].ino = NULL;
    __atomic_store_n(&g_nwatch, last, __ATOMIC_RELEASE);
}

int vfs_watch_active(void) { return __atomic_load_n(&g_nwatch, __ATOMIC_ACQUIRE) != 0; }

/* ---- delivery -------------------------------------------------------------- */

#define WOKEN_MAX 16

static void deliver(struct inode* ino, uint32_t mask, const char* name, uint32_t cookie) {
    if (!ino || !__atomic_load_n(&g_nwatch, __ATOMIC_ACQUIRE)) return;
    struct inotify* woken[WOKEN_MAX];
    int nw = 0, more = 0;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_nwatch; i++) {
        struct watch* w = &g_w[i];
        if (w->ino != ino || !(w->mask & mask & FSN_ALL_EVENTS)) continue;
        struct inotify* in = w->in;
        int q = enqueue(in, w->wd, mask, cookie, name);
        if (w->mask & IN_ONESHOT) {
            q |= enqueue(in, w->wd, FSN_IGNORED, 0, NULL);
            drop_slot(i);
            i--;                              /* the slot now holds another watch */
        }
        if (q) {
            int seen = 0;
            for (int k = 0; k < nw; k++) if (woken[k] == in) seen = 1;
            if (!seen) { if (nw < WOKEN_MAX) woken[nw++] = in; else more = 1; }
        }
    }
    for (int k = 0; k < nw; k++) wake(woken[k]);
    spin_unlock_irqrestore(&g_lock, fl);
    /* Readiness for poll/epoll is one global wake today (fd.c); a single call
     * covers every instance, including any past WOKEN_MAX. */
    if (nw || more) fd_readiness_changed(NULL);
}

void fsnotify_inode(struct inode* in, uint32_t mask, const char* name) {
    deliver(in, mask, name, 0);
}

void fsnotify_dentry(struct dentry* d, uint32_t mask) {
    if (!d || !__atomic_load_n(&g_nwatch, __ATOMIC_ACQUIRE)) return;
    uint32_t m = mask;
    if (d->inode && d->inode->type == INODE_DIR) m |= FSN_ISDIR;
    deliver(d->inode, m, NULL, 0);
    if (d->parent && d->parent != d && d->parent->inode) deliver(d->parent->inode, m, d->name, 0);
}

void fsnotify_move(struct inode* odir, const char* oname, struct inode* ndir,
                   const char* nname, struct inode* child) {
    if (!__atomic_load_n(&g_nwatch, __ATOMIC_ACQUIRE)) return;
    uint32_t isdir = (child && child->type == INODE_DIR) ? FSN_ISDIR : 0;
    uint32_t cookie = __atomic_add_fetch(&g_cookie, 1, __ATOMIC_RELAXED);
    if (!cookie) cookie = __atomic_add_fetch(&g_cookie, 1, __ATOMIC_RELAXED);
    deliver(odir, FSN_MOVED_FROM | isdir, oname, cookie);
    deliver(ndir, FSN_MOVED_TO | isdir, nname, cookie);
    deliver(child, FSN_MOVE_SELF | isdir, NULL, 0);
}

void fsnotify_inode_gone(struct inode* ino) {
    if (!ino || !__atomic_load_n(&g_nwatch, __ATOMIC_ACQUIRE)) return;
    struct inotify* woken[WOKEN_MAX];
    int nw = 0;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_nwatch; i++) {
        struct watch* w = &g_w[i];
        if (w->ino != ino) continue;
        struct inotify* in = w->in;
        if (w->mask & FSN_DELETE_SELF) enqueue(in, w->wd, FSN_DELETE_SELF, 0, NULL);
        enqueue(in, w->wd, FSN_IGNORED, 0, NULL);
        drop_slot(i);
        i--;
        int seen = 0;
        for (int k = 0; k < nw; k++) if (woken[k] == in) seen = 1;
        if (!seen && nw < WOKEN_MAX) woken[nw++] = in;
    }
    for (int k = 0; k < nw; k++) wake(woken[k]);
    spin_unlock_irqrestore(&g_lock, fl);
    fd_readiness_changed(NULL);
}

/* ---- the descriptor --------------------------------------------------------- */

void inotify_set_owner(struct inotify* in, struct ofile* o) { if (in) in->owner = o; }

int inotify_can_read(struct inotify* in) {
    return in && __atomic_load_n(&in->used, __ATOMIC_ACQUIRE) != 0;
}

int inotify_pending_bytes(struct inotify* in) {
    return in ? (int)__atomic_load_n(&in->used, __ATOMIC_ACQUIRE) : 0;
}

long inotify_read(struct inotify* in, void* buf, size_t n, int block) {
    if (!in || !buf) return -22;
    for (;;) {
        uint32_t fl = spin_lock_irqsave(&g_lock);
        if (in->used) {
            /* Whole events only, as many as fit. */
            uint32_t take = 0;
            while (take < in->used) {
                uint32_t rec = IN_EVHDR + get32(in->q + take + 12);
                if (take + rec > n) break;
                take += rec;
            }
            if (!take) { spin_unlock_irqrestore(&g_lock, fl); return -22; }   /* EINVAL */
            uint8_t* d = (uint8_t*)buf;
            for (uint32_t i = 0; i < take; i++) d[i] = in->q[i];
            for (uint32_t i = take; i < in->used; i++) in->q[i - take] = in->q[i];
            in->used -= take;
            if (in->has_last) {
                if (in->last_off >= take) in->last_off -= take; else in->has_last = 0;
            }
            if (!in->used) { in->overflowed = 0; in->has_last = 0; }
            spin_unlock_irqrestore(&g_lock, fl);
            return (long)take;
        }
        spin_unlock_irqrestore(&g_lock, fl);
        if (!block) return -11;                                /* EAGAIN */
        if (task_should_stop()) return -4;                     /* EINTR */
        uint32_t f = waitq_lock(&in->wq);
        if (!__atomic_load_n(&in->used, __ATOMIC_ACQUIRE)) waitq_block(&in->wq);
        waitq_unlock(&in->wq, f);
    }
}

void inotify_close(struct inotify* in) {
    if (!in) return;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_nwatch; i++)
        if (g_w[i].in == in) { drop_slot(i); i--; }
    spin_unlock_irqrestore(&g_lock, fl);
    wake(in);
    kfree(in);
}

static struct inotify* inotify_of_fd(int fd) {
    struct ofile* o = fd_lookup(fd);
    return (o && o->kind == FD_INOTIFY) ? o->ino : NULL;
}

int sys_inotify_init(int flags) {
    if (flags & ~(0x800 | 0x80000)) return -22;
    struct inotify* in = (struct inotify*)kcalloc(1, sizeof *in);
    if (!in) return -12;
    waitq_init(&in->wq);
    struct ofile* o = ofile_from_inotify(in);
    if (!o) { kfree(in); return -12; }
    if (flags & 0x800) o->nonblock = 1;
    int fd = fd_install_k(o);
    if (fd < 0) { ofile_unref(o); return -24; }               /* EMFILE */
    return fd;
}

int sys_inotify_add_watch(int fd, const char* path, uint32_t mask) {
    struct inotify* in = inotify_of_fd(fd);
    if (!in) return -9;                                        /* EBADF */
    if ((mask & ~IN_VALID_FLAGS) || !(mask & FSN_ALL_EVENTS)) return -22;
    if ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)) return -22;
    int is_dir = 0;
    struct inode* ino = vfs_watch_inode(path, !(mask & IN_DONT_FOLLOW), &is_dir);
    if (!ino) return -2;                                       /* ENOENT */
    if ((mask & IN_ONLYDIR) && !is_dir) return -20;            /* ENOTDIR */
    if (!vfs_permitted(ino, VFS_PERM_READ)) return -13;        /* EACCES */
    uint32_t keep = mask & (FSN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);
    uint32_t fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_nwatch; i++) {
        if (g_w[i].in == in && g_w[i].ino == ino) {
            int wd = g_w[i].wd;
            if (mask & IN_MASK_CREATE) { spin_unlock_irqrestore(&g_lock, fl); return -17; }
            g_w[i].mask = (mask & IN_MASK_ADD) ? (g_w[i].mask | keep) : keep;
            spin_unlock_irqrestore(&g_lock, fl);
            return wd;
        }
    }
    if (g_nwatch >= IN_WATCH_MAX) { spin_unlock_irqrestore(&g_lock, fl); return -28; }  /* ENOSPC */
    int wd = ++in->next_wd;
    struct watch* w = &g_w[g_nwatch];
    w->in = in; w->ino = ino; w->mask = keep; w->wd = wd;
    __atomic_store_n(&g_nwatch, g_nwatch + 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_lock, fl);
    return wd;
}

int sys_inotify_rm_watch(int fd, int wd) {
    struct inotify* in = inotify_of_fd(fd);
    if (!in) return -9;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    for (int i = 0; i < g_nwatch; i++) {
        if (g_w[i].in == in && g_w[i].wd == wd) {
            enqueue(in, wd, FSN_IGNORED, 0, NULL);
            drop_slot(i);
            wake(in);
            spin_unlock_irqrestore(&g_lock, fl);
            fd_readiness_changed(NULL);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_lock, fl);
    return -22;                                                /* EINVAL */
}

/* ---- `inotifytest` — the contract above, each step able to fail ------------ */
#include "shellcmd.h"
#include "printf.h"

static int it_next(uint8_t* buf, long n, long* off, int* wd, uint32_t* mask, const char** name) {
    if (*off + IN_EVHDR > n) return 0;
    uint8_t* e = buf + *off;
    *wd = (int)get32(e); *mask = get32(e + 4);
    uint32_t len = get32(e + 12);
    *name = len ? (const char*)(e + IN_EVHDR) : "";
    *off += IN_EVHDR + len;
    return 1;
}
static int s_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Read everything queued and report whether an event with `mask` (and, when
 * given, `name`) is among it. */
static int it_saw(int fd, uint32_t mask, const char* name) {
    struct ofile* o = fd_lookup(fd);
    if (!o) return 0;
    uint8_t buf[512];
    long n = inotify_read(o->ino, buf, sizeof buf, 0);
    if (n <= 0) return 0;
    long off = 0; int wd; uint32_t m; const char* nm; int hit = 0;
    while (it_next(buf, n, &off, &wd, &m, &nm))
        if ((m & mask) && (!name || s_eq(nm, name))) hit = 1;
    return hit;
}

static void cmd_inotifytest(const char* args) {
    (void)args;
    int ok = 1;
    #define IT(cond, what) do { int _c = (cond); kprintf("inotify: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    vfs_unlink("/tmp/inotest/f2");
    vfs_unlink("/tmp/inotest/f");
    vfs_unlink("/tmp/inotest");
    vfs_mkdir("/tmp");
    IT(vfs_mkdir("/tmp/inotest") == 0, "mkdir /tmp/inotest");
    int fd = sys_inotify_init(0x800);
    IT(fd >= 0, "inotify_init1(IN_NONBLOCK)");
    if (fd < 0) { kprintf("inotify: FAIL\n"); return; }
    int wd = sys_inotify_add_watch(fd, "/tmp/inotest", FSN_ALL_EVENTS);
    IT(wd > 0, "watch the directory");
    IT(sys_inotify_add_watch(fd, "/tmp/inotest", FSN_CREATE) == wd, "re-adding returns the same wd");
    IT(sys_inotify_add_watch(fd, "/tmp/inotest", FSN_ALL_EVENTS | IN_MASK_ADD) == wd, "IN_MASK_ADD widens it");
    IT(sys_inotify_add_watch(fd, "/tmp/no-such", FSN_MODIFY) == -2, "a missing path is ENOENT");
    struct ofile* o = fd_lookup(fd);
    uint8_t tiny[8];
    IT(o && inotify_read(o->ino, tiny, sizeof tiny, 0) == -11, "an empty queue is EAGAIN");
    struct file* f = vfs_open("/tmp/inotest/f", VFS_WRONLY | VFS_CREATE);
    IT(f != NULL, "create a file");
    IT(o && inotify_read(o->ino, tiny, sizeof tiny, 0) == -22, "a buffer smaller than the event is EINVAL");
    IT(it_saw(fd, FSN_CREATE, "f"), "CREATE f");
    vfs_write(f, "x", 1); vfs_write(f, "y", 1); vfs_write(f, "z", 1);
    IT(o && inotify_pending_bytes(o->ino) == 32, "three writes coalesce into one MODIFY");
    IT(it_saw(fd, FSN_MODIFY, "f"), "MODIFY f");
    vfs_close(f);
    IT(it_saw(fd, FSN_CLOSE_WRITE, "f"), "CLOSE_WRITE f");
    int wf = sys_inotify_add_watch(fd, "/tmp/inotest/f", FSN_MODIFY | FSN_DELETE_SELF | FSN_MOVE_SELF);
    IT(wf > 0 && wf != wd, "watch the file itself");
    IT(vfs_rename("/tmp/inotest/f", "/tmp/inotest/f2") == 0, "rename f -> f2");
    {
        uint8_t buf[512];
        long n = inotify_read(o->ino, buf, sizeof buf, 0);
        long off = 0; int w; uint32_t m; const char* nm;
        uint32_t c_from = 0, c_to = 0; int self = 0;
        while (n > 0 && it_next(buf, n, &off, &w, &m, &nm)) {
            if ((m & FSN_MOVED_FROM) && s_eq(nm, "f"))  c_from = 1;
            if ((m & FSN_MOVED_TO)   && s_eq(nm, "f2")) c_to = 1;
            if ((m & FSN_MOVE_SELF) && w == wf) self = 1;
        }
        IT(c_from && c_to, "MOVED_FROM f + MOVED_TO f2");
        IT(self, "MOVE_SELF on the file's own watch");
    }
    IT(vfs_unlink("/tmp/inotest/f2") == 0, "unlink f2");
    {
        uint8_t buf[512];
        long n = inotify_read(o->ino, buf, sizeof buf, 0);
        long off = 0; int w; uint32_t m; const char* nm;
        int del = 0, dself = 0, ign = 0;
        while (n > 0 && it_next(buf, n, &off, &w, &m, &nm)) {
            if ((m & FSN_DELETE) && s_eq(nm, "f2")) del = 1;
            if ((m & FSN_DELETE_SELF) && w == wf) dself = 1;
            if ((m & FSN_IGNORED) && w == wf) ign = 1;
        }
        IT(del, "DELETE f2 on the directory");
        IT(dself && ign, "DELETE_SELF + IGNORED end the file's watch");
    }
    IT(sys_inotify_rm_watch(fd, wf) == -22, "the ended watch is gone (EINVAL)");
    IT(sys_inotify_rm_watch(fd, wd) == 0 && it_saw(fd, FSN_IGNORED, NULL), "rm_watch queues IGNORED");
    /* cgroupfs: a file that changes by itself */
    vfs_unlink("/sys/fs/cgroup/inotest");
    if (vfs_mkdir("/sys/fs/cgroup/inotest") == 0) {
        int wc = sys_inotify_add_watch(fd, "/sys/fs/cgroup/inotest/pids.events", FSN_MODIFY);
        IT(wc > 0, "watch a cgroup's pids.events");
        vfs_unlink("/sys/fs/cgroup/inotest");
        IT(it_saw(fd, FSN_IGNORED, NULL), "rmdir of the cgroup ends that watch");
    }
    vfs_unlink("/tmp/inotest");
    sys_close(fd);
    #undef IT
    kprintf("inotify: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(inotifytest) = { "inotifytest", "", "inotify: watches, events, coalescing, removal",
                           SHELL_G_TEST, cmd_inotifytest, SHELL_P_ANY };
