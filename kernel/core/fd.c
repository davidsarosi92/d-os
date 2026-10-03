/* =============================================================================
 * fd.c — generic open-file objects + shared-memory objects (M25 stage 4+).
 * See fd.h.  Arch-neutral: frames come from the PMM, everything else is plain
 * bookkeeping.
 * ============================================================================= */

#include "fd.h"
#include "fifo.h"
#include "hal_api.h"   /* phys_to_virt / virt_to_phys — kernel direct map */
#include "vfs.h"
#include "timerfd.h"
#include "eventfd.h"   /* §M90 */ /* §M53 stage 3 — FD_TIMER */
#include "epoll.h"   /* §M56 — FD_EPOLL         */
#include "flock.h"   /* §M90 — locks die with their description */
#include "netlink.h" /* §M90 */
#include "pmm.h"
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
    }
    o->kind = (enum fd_kind)0x0DEAD;     /* the poison, see above */
    kfree(o);
}

/* ---- shared memory -------------------------------------------------------- */

struct shm* shm_create(size_t size) {
    int n = (int)((size + 4095) / 4096);
    if (n <= 0) n = 1;
    if (n > SHM_MAX_FRAMES) return NULL;

    struct shm* s = (struct shm*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    s->refcount = 1;
    s->nframes  = n;
    for (int i = 0; i < n; i++) {
        pmm_phys_t f = pmm_alloc_frame_user_low();   /* §M72 — user memory */
        if (f == PMM_ALLOC_FAIL) {              /* OOM or reserve — unwind */
            for (int j = 0; j < i; j++) pmm_free_frame(s->frames[j]);
            kfree(s);
            return NULL;
        }
        /* Zero the frame through the identity map (frames are < 1 GiB). */
        uint8_t* p = (uint8_t*)phys_to_virt(f);
        for (int b = 0; b < 4096; b++) p[b] = 0;
        s->frames[i] = f;
    }
    return s;
}

/* Grow a shm object to at least `size` bytes (§M40).  Linux's memfd_create
 * returns a ZERO-length object that the caller then ftruncate()s to the size it
 * wants — which is exactly what a Wayland client does before handing the fd to
 * wl_shm_create_pool.  Shrinking is not supported (nothing needs it, and the
 * frames may already be mapped); an already-large-enough object succeeds. */
int shm_grow(struct shm* s, size_t size) {
    if (!s) return -1;
    int n = (int)((size + 4095) / 4096);
    if (n <= s->nframes) return 0;
    if (n > SHM_MAX_FRAMES) return -1;
    for (int i = s->nframes; i < n; i++) {
        pmm_phys_t f = pmm_alloc_frame_user_low();   /* §M72 — user memory */
        if (f == PMM_ALLOC_FAIL) return -1;     /* keep what we already grew to */
        uint8_t* p = (uint8_t*)phys_to_virt(f);
        for (int b = 0; b < 4096; b++) p[b] = 0;
        s->frames[i] = f;
        s->nframes = i + 1;
    }
    return 0;
}

struct shm* shm_ref(struct shm* s) {
    if (s) s->refcount++;
    return s;
}

void shm_unref(struct shm* s) {
    if (!s) return;
    if (--s->refcount > 0) return;
    for (int i = 0; i < s->nframes; i++) pmm_free_frame(s->frames[i]);
    kfree(s);
}
