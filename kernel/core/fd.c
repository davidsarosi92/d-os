/* =============================================================================
 * fd.c — generic open-file objects + shared-memory objects (M25 stage 4+).
 * See fd.h.  Arch-neutral: frames come from the PMM, everything else is plain
 * bookkeeping.
 * ============================================================================= */

#include "fd.h"
#include "fifo.h"
#include "bpf.h"
#include "hal_api.h"   /* phys_to_virt / virt_to_phys — kernel direct map */
#include "vfs.h"
#include "timerfd.h"
#include "eventfd.h"   /* §M90 */ /* §M53 stage 3 — FD_TIMER */
#include "epoll.h"   /* §M56 — FD_EPOLL         */
#include "flock.h"   /* §M90 — locks die with their description */
#include "netlink.h" /* §M90 */
#include "pmm.h"
#include "pcache.h"   /* §M90 — a memfd that was exec'd */
#include "kmalloc.h"
#include "printf.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

/* unix socket teardown lives in the (stage-5) socket module; declared weakly
 * here so FD_SOCK unref links before that module exists. */
void usock_close(struct usock* s) __attribute__((weak));
void usock_close(struct usock* s) { (void)s; }

/* network-socket teardown lives in usyscall.c (M24 socket API); weak here so
 * FD_NETSOCK unref links even in builds that don't pull it in. */
void netsock_close(struct netsock* s) __attribute__((weak));
void netsock_close(struct netsock* s) { (void)s; }

/* ---- ofile ---------------------------------------------------------------- */

static struct ofile* ofile_alloc(enum fd_kind k) {
    struct ofile* o = (struct ofile*)kcalloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = k;
    o->refcount = 1;
    return o;
}

void fd_readiness_changed(struct ofile* o) {
    (void)o;                    /* see fd.h — the wake is global today */
    fd_readiness_signal();
}

/* §M90 — an eBPF program behind a descriptor (bpf.c); takes a reference. */
struct bpf_prog_ref;
void bpf_prog_ref_get(struct bpf_prog* p);
struct ofile* ofile_from_bpf(struct bpf_prog* p) {
    if (!p) return NULL;
    struct ofile* o = ofile_alloc(FD_BPF);
    if (o) { bpf_prog_ref_get(p); o->bpf = p; }
    return o;
}
struct ofile* ofile_from_file(struct file* f) {
    if (!f) return NULL;
    struct ofile* o = ofile_alloc(FD_VFS);
    if (o) o->file = f;
    return o;
}
struct ofile* ofile_from_shm(struct shm* s) {
    if (!s) return NULL;
    struct ofile* o = ofile_alloc(FD_SHM);
    if (o) o->shm = shm_ref(s);
    return o;
}
struct ofile* ofile_from_sock(struct usock* s) {
    struct ofile* o = ofile_alloc(FD_SOCK);
    /* §M56.2 — the object learns its description here, at the one point where
     * the two are joined.  Doing it anywhere else would mean a window in which
     * a readiness change cannot name itself. */
    if (o) { o->sock = s; usock_set_owner(s, o); }
    return o;
}
struct ofile* ofile_from_netsock(struct netsock* s) {
    struct ofile* o = ofile_alloc(FD_NETSOCK);
    if (o) o->nsock = s;
    return o;
}

struct ofile* ofile_from_timerfd(struct timerfd* t) {
    struct ofile* o = ofile_alloc(FD_TIMER);
    if (o) { o->tfd = t; timerfd_set_owner(t, o); }
    return o;
}

struct ofile* ofile_from_eventfd(struct eventfd* e) {
    struct ofile* o = ofile_alloc(FD_EVENT);
    if (o) { o->efd = e; eventfd_set_owner(e, o); }
    return o;
}

struct ofile* ofile_from_netlink(struct nlsock* s) {
    struct ofile* o = ofile_alloc(FD_NETLINK);
    if (o) { o->nl = s; nl_set_owner(s, o); }
    return o;
}

struct ofile* ofile_from_epoll(struct epoll* e) {
    struct ofile* o = ofile_alloc(FD_EPOLL);
    if (o) o->ep = e;
    return o;
}

/* §M90 — ATOMIC.  A plain ++/-- was safe while one task owned a table; since
 * §M89 the threads of a process share one, and a Go program dups, passes and
 * closes descriptors from several threads at once — two CPUs racing a plain
 * decrement lose one, and the object is then freed under a live descriptor
 * (or never freed). */
struct ofile* ofile_ref(struct ofile* o) {
    if (o) __atomic_add_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL);
    return o;
}

struct ofile* fd_dup_source(int fd, int* fresh) {
    *fresh = 0;
    struct ofile* o = fd_lookup(fd);
    if (o || fd < 0 || fd > 2) return o;
    o = ofile_alloc(FD_CONSOLE);
    if (o) *fresh = 1;
    return o;
}

void ofile_unref(struct ofile* o) {
    if (!o) return;
    /* §M90 — a released description is POISONED (kind 0x0DEAD) before it is
     * freed, so a stale pointer that is closed again is caught HERE, named
     * with its callers, instead of freeing twice and corrupting whatever the
     * heap hands out next (it surfaced as a garbage struct file in vfs_close). */
    if ((int)o->kind == 0x0DEAD) {
        kprintf("!! ofile %p released AGAIN (stale descriptor) from %p\n", (void*)o,
                __builtin_return_address(0));
        return;
    }
    int left = __atomic_sub_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL);
    if (left > 0) return;
    if (left < 0) {
        kprintf("!! ofile %p refcount underflow (kind %d) from %p\n", (void*)o, (int)o->kind,
                __builtin_return_address(0));
        return;
    }
    switch (o->kind) {
        case FD_VFS:  flock_release(o); if (o->file) vfs_close(o->file);   break;
        case FD_SHM:  if (o->shm)  shm_unref(o->shm);    break;
        case FD_SOCK: if (o->sock) usock_close(o->sock); break;
        case FD_NETSOCK: if (o->nsock) netsock_close(o->nsock); break;
        case FD_TIMER:   if (o->tfd)   timerfd_close(o->tfd);    break;
        case FD_EPOLL:   if (o->ep)    epoll_close(o->ep);       break;
        case FD_CONSOLE: break;                   /* nothing behind it */
        case FD_EVENT:   if (o->efd)   eventfd_close(o->efd);    break;
        case FD_NETLINK: if (o->nl)    nl_close(o->nl);          break;
        case FD_FIFO:    fifo_detach(o); if (o->file) vfs_close(o->file); break;
        case FD_BPF:     bpf_prog_put(o->bpf); break;
    }
    o->kind = (enum fd_kind)0x0DEAD;     /* the poison, see above */
    kfree(o);
}

/* ---- shared memory -------------------------------------------------------- */

static void shm_cache_drop(struct shm* s);
/* §M90 — make room for `n` frames in the list (it grows by doubling). */
static int shm_reserve(struct shm* s, int n) {
    if (n <= s->cap) return 0;
    if (n > SHM_MAX_FRAMES) return -1;
    int nc = s->cap ? s->cap : 16;
    while (nc < n) nc *= 2;
    if (nc > SHM_MAX_FRAMES) nc = SHM_MAX_FRAMES;
    uint64_t* nf = (uint64_t*)kcalloc((size_t)nc, sizeof *nf);
    if (!nf) return -1;
    for (int i = 0; i < s->nframes; i++) nf[i] = s->frames[i];
    kfree(s->frames);
    s->frames = nf;
    s->cap = nc;
    return 0;
}
/* Hold frames up to page `n` (exclusive), zeroed.  0 or -1 (OOM / limit). */
static int shm_fill(struct shm* s, int n) {
    if (n <= s->nframes) return 0;
    if (shm_reserve(s, n) != 0) return -1;
    for (int i = s->nframes; i < n; i++) {
        pmm_phys_t f = pmm_alloc_frame_user_low();   /* §M72 — user memory */
        if (f == PMM_ALLOC_FAIL) return -1;          /* keep what we already have */
        uint8_t* p = (uint8_t*)phys_to_virt(f);
        for (int b = 0; b < 4096; b++) p[b] = 0;
        s->frames[i] = (uint64_t)f;
        s->nframes = i + 1;
    }
    return 0;
}

struct shm* shm_create(size_t size) {
    uint64_t n = (size + 4095) / 4096;
    if (n == 0) n = 1;
    if (n > SHM_MAX_FRAMES) return NULL;
    struct shm* s = (struct shm*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    s->refcount = 1;
    if (shm_fill(s, (int)n) != 0) {                  /* OOM or reserve — unwind */
        for (int j = 0; j < s->nframes; j++) pmm_free_frame((pmm_phys_t)s->frames[j]);
        kfree(s->frames);
        kfree(s);
        return NULL;
    }
    s->size = size;
    return s;
}

/* Grow a shm object to at least `size` bytes (§M40).  Linux's memfd_create
 * returns a ZERO-length object that the caller then ftruncate()s to the size it
 * wants — which is exactly what a Wayland client does before handing the fd to
 * wl_shm_create_pool. */
int shm_grow(struct shm* s, size_t size) {
    if (!s) return -1;
    return shm_truncate(s, size) == 0 || (uint64_t)size <= s->size ? 0 : -1;
}

int shm_truncate(struct shm* s, uint64_t size) {
    if (!s) return -22;
    shm_cache_drop(s);
    if (size > s->size && (s->seals & SHM_SEAL_GROW))   return -1;   /* EPERM */
    if (size < s->size && (s->seals & SHM_SEAL_SHRINK)) return -1;
    uint64_t n = (size + 4095) / 4096;
    if (n > SHM_MAX_FRAMES) return -27;                              /* EFBIG */
    if (shm_fill(s, (int)n) != 0) return -12;
    /* Bytes past a shrink read back as zero after a later grow, as on Linux. */
    if (size < s->size) {
        for (uint64_t o = size; o < s->size; ) {
            uint8_t* p = (uint8_t*)shm_kptr(s, o);
            uint64_t room = 4096 - (o % 4096);
            if (room > s->size - o) room = s->size - o;
            if (p) for (uint64_t b = 0; b < room; b++) p[b] = 0;
            o += room;
        }
    }
    s->size = size;
    return 0;
}

void* shm_kptr(struct shm* s, uint64_t off) {
    if (!s) return NULL;
    uint64_t fi = off / 4096;
    if (fi >= (uint64_t)s->nframes) return NULL;
    return (uint8_t*)phys_to_virt((pmm_phys_t)s->frames[fi]) + (off % 4096);
}

size_t shm_read(struct shm* s, uint64_t off, void* dst, size_t n) {
    if (!s || off >= s->size) return 0;
    if (n > s->size - off) n = (size_t)(s->size - off);
    size_t done = 0;
    while (done < n) {
        uint8_t* p = (uint8_t*)shm_kptr(s, off + done);
        if (!p) break;
        size_t room = 4096 - (size_t)((off + done) % 4096);
        if (room > n - done) room = n - done;
        for (size_t b = 0; b < room; b++) ((uint8_t*)dst)[done + b] = p[b];
        done += room;
    }
    return done;
}

static void shm_cache_drop(struct shm* s) {
    if (s && s->ino && s->ino->pc_id) pcache_invalidate(s->ino);
}
long shm_write(struct shm* s, uint64_t off, const void* src, size_t n) {
    if (!s) return -9;
    shm_cache_drop(s);
    if (s->seals & SHM_SEAL_WRITE) return -1;                          /* EPERM */
    uint64_t end = off + n;
    if (end > s->size) {
        if (s->seals & SHM_SEAL_GROW) return -1;
        if ((end + 4095) / 4096 > SHM_MAX_FRAMES) return -27;          /* EFBIG */
        if (shm_fill(s, (int)((end + 4095) / 4096)) != 0) return -12;
    }
    size_t done = 0;
    while (done < n) {
        uint8_t* p = (uint8_t*)shm_kptr(s, off + done);
        if (!p) break;
        size_t room = 4096 - (size_t)((off + done) % 4096);
        if (room > n - done) room = n - done;
        for (size_t b = 0; b < room; b++) p[b] = ((const uint8_t*)src)[done + b];
        done += room;
    }
    if (off + done > s->size) s->size = off + done;
    return (long)done;
}

/* §M90 — exec of a memfd.  The ELF loader reads a `struct file` through its
 * inode's ops; this gives the shm object one, read-only, with no dentry (a
 * memfd has no name in any directory).  Created once per object, freed with
 * it.  The page cache may keep copies of its pages under the inode's id; any
 * change to the bytes drops them (shm_cache_drop), so a later exec of the
 * same memfd cannot run stale code. */
static ssize_t shm_file_read(struct file* f, void* buf, size_t n, uint64_t off) {
    struct shm* s = f && f->inode ? (struct shm*)f->inode->private : NULL;
    return s ? (ssize_t)shm_read(s, off, buf, n) : -1;
}
static const struct file_ops shm_file_ops = { .read = shm_file_read };
struct file* shm_file(struct shm* s) {
    if (!s) return NULL;
    if (!s->ino) {
        struct inode* in = (struct inode*)kcalloc(1, sizeof *in);
        if (!in) return NULL;
        vfs_inode_defaults(in);
        in->type = INODE_FILE;
        in->ops = &shm_file_ops;
        in->private = s;
        in->mode = 0755;
        s->ino = in;
    }
    s->ino->size = s->size;
    struct file* f = (struct file*)kcalloc(1, sizeof *f);
    if (!f) return NULL;
    f->inode = s->ino;
    f->flags = VFS_RDONLY;
    f->magic = VFS_FILE_MAGIC;
    s->ino->opens++;
    /* No reference of its own: the loader reads the whole image while the
     * caller's descriptor still holds the object (execveat(fd) /
     * /proc/self/fd/N), and copies or caches every page it maps — nothing it
     * builds points back into this object once the exec is done. */
    return f;
}

struct shm* shm_ref(struct shm* s) {
    if (s) __atomic_add_fetch(&s->refcount, 1, __ATOMIC_ACQ_REL);
    return s;
}

void shm_unref(struct shm* s) {
    if (!s) return;
    if (__atomic_sub_fetch(&s->refcount, 1, __ATOMIC_ACQ_REL) > 0) return;
    shm_cache_drop(s);
    for (int i = 0; i < s->nframes; i++) pmm_free_frame((pmm_phys_t)s->frames[i]);
    kfree(s->frames);
    kfree(s->ino);
    kfree(s);
}
