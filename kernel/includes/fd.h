/* =============================================================================
 * fd.h — generic open-file object behind a file descriptor (M25 stage 4+).
 *
 * Stage 3 stored raw `struct file*` (VFS handles) in the per-process fd
 * table.  Stage 4 (shared memory) and stage 5 (unix sockets) put *non-file*
 * objects behind descriptors too, so the table now holds a `struct ofile` —
 * a tagged handle wrapping exactly one of: a VFS file, a shared-memory
 * object, or a unix socket endpoint.  It carries a refcount so a descriptor
 * can be duplicated / passed between processes (SCM_RIGHTS, stage 5) while
 * the underlying object lives until the last reference closes.
 * ============================================================================= */

#ifndef FD_H
#define FD_H

#include <stdint.h>
#include <stddef.h>

struct file;                    /* vfs.h  */
struct shm;                     /* below  */
struct usock;                   /* unix socket endpoint (stage 5)          */

enum fd_kind { FD_VFS, FD_SHM, FD_SOCK, FD_NETSOCK, FD_TIMER, FD_EPOLL };

struct netsock;                 /* network (AF_INET) socket — usyscall.c        */
struct timerfd;                 /* §M53 stage 3 — a deadline behind a descriptor */
struct epoll;                   /* §M56 — a readiness set behind a descriptor   */

struct ofile {
    enum fd_kind kind;
    int          refcount;      /* # of descriptors referencing this object */
    /* §M57 — O_NONBLOCK, and it lives HERE rather than in each object because
     * it is a property of the open file DESCRIPTION, not of the thing behind
     * it: two descriptors dup'd from one another share it, and two independent
     * opens of the same file do not.  It used to exist only inside
     * `struct netsock`, so O_NONBLOCK on a pipe silently did nothing — which
     * is the failure mode an event loop is least able to survive, because
     * every fd it drains is one it must not block on. */
    int          nonblock;
    struct file* file;          /* FD_VFS  */
    struct shm*  shm;           /* FD_SHM  */
    struct usock* sock;         /* FD_SOCK */
    struct netsock* nsock;      /* FD_NETSOCK (M24 socket API) */
    struct timerfd* tfd;        /* FD_TIMER (§M53 stage 3) */
    struct epoll* ep;           /* FD_EPOLL (§M56)         */
};

/* Wrap a resource in a fresh ofile (refcount 1), or NULL on OOM. */
struct ofile* ofile_from_file(struct file* f);
struct ofile* ofile_from_shm (struct shm* s);
struct ofile* ofile_from_sock(struct usock* s);
struct ofile* ofile_from_netsock(struct netsock* s);
struct ofile* ofile_from_timerfd(struct timerfd* t);
struct ofile* ofile_from_epoll(struct epoll* e);

/* Refcount management.  ofile_unref drops the last reference → closes the
 * wrapped resource + frees the ofile. */
struct ofile* ofile_ref  (struct ofile* o);
void          ofile_unref(struct ofile* o);

/* ---- shared-memory object (stage 4) --------------------------------------- */

#define SHM_MAX_FRAMES 64       /* 64 × 4 KiB = 256 KiB max per object (plenty) */

struct shm {
    int      refcount;          /* independent of the ofile refcount: a frame
                                 * set can outlive an fd once mmap'd */
    int      nframes;
    uint32_t frames[SHM_MAX_FRAMES];   /* physical frame addresses */
};

/* Create a shared-memory object of `size` bytes (rounded up to pages), frames
 * zeroed.  Returns NULL on OOM / too large. */
struct shm* shm_create(size_t size);
struct shm* shm_ref   (struct shm* s);
/* Grow to at least `size` bytes (Linux memfd_create + ftruncate shape).  0 on
 * success; shrinking is not supported. */
int         shm_grow  (struct shm* s, size_t size);
void        shm_unref (struct shm* s);   /* frees frames at refcount 0 */

/* ---- unix socket pair + fd passing (stage 5) ------------------------------ */

int  usock_pair (struct usock** a, struct usock** b);
long usock_send (struct usock* s, const void* buf, size_t n, struct ofile* passfile);
/* Tier A.3 — `block`: when non-zero and the endpoint has nothing to receive
 * (no bytes, no passed fd) but the peer is still open, park the caller on the
 * endpoint's read wait-queue until usock_send/usock_close wakes it, then
 * re-drain.  block == 0 keeps the original non-blocking snapshot behaviour
 * (poll's drain path, single-task self-tests). */
long usock_recv (struct usock* s, void* buf, size_t n, int block,
                 struct ofile** passfile_out);
void usock_close(struct usock* s);
int  usock_can_read (struct usock* s);   /* bytes buffered? (poll POLLIN)  */
int  usock_can_write(struct usock* s);   /* peer open + space? (POLLOUT)   */
int  usock_peer_open (struct usock* s);  /* other end still there? (§M57)  */

/* Tier A.3 — poll readiness signal.  usock_send / usock_close call this
 * after changing an fd's readiness so a task blocked in a (timeout < 0)
 * poll() wakes and re-scans.  Defined in usyscall.c (owns the global
 * readiness wait-queue); declared here so the socket layer can raise it. */
void fd_readiness_signal(void);

/* THE definition of "is this descriptor ready" (§M56).
 *
 * poll(2) and epoll_wait(2) must agree exactly about this, so they share one
 * function rather than two switch statements that would drift apart the first
 * time a new fd kind appeared — which is precisely what happened to FD_NETSOCK,
 * reported as permanently ready by poll's fall-through for two milestones.
 *
 * Returns a bitmask of POLL* bits (syscall.h), whose values are deliberately
 * Linux's so that mapping to epoll's EPOLL* is the identity rather than a
 * translation table.  §M57 widened this from two 0/1 outputs to a mask because
 * readiness is not only "can I read": a hung-up peer and an error are
 * conditions a loop must be able to SEE, and POSIX reports them whether or not
 * they were requested. */
uint32_t fd_readiness(int fd);

/* Readiness of an AF_INET socket (usyscall.c owns struct netsock), as a
 * POLL* mask. */
uint32_t netsock_readiness(struct netsock* ns);

/* The one blocking loop behind both poll(2) and epoll_wait(2).
 *
 * `scan` reports how many of the caller's descriptors are currently ready (and
 * records whatever detail the caller needs); this owns the deadline, the
 * parking, and the check-then-park discipline that keeps a wakeup from being
 * lost.  `timeout_ms` follows poll's convention: <0 waits forever, 0 returns
 * the first scan, >0 is a REAL bounded wait.  Returns `scan`'s last value.
 *
 * Two copies of this loop would be two chances to get the lost-wakeup rule
 * wrong, which is why epoll does not have its own. */
int fd_readiness_wait(int (*scan)(void* ctx), void* ctx, int timeout_ms);

#endif
