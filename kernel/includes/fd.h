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

/* FD_CONSOLE (§M73) is appended: the console as an OBJECT.  An empty slot
 * 0/1/2 still means "the console" (that default is what keeps every
 * non-redirecting program unchanged, §M59), but a shell saves its stdout with
 * `fcntl(1, F_DUPFD, 10)` before redirecting and restores it with
 * `dup2(10, 1)` afterwards — and an empty slot cannot be copied anywhere.  So a
 * dup of an empty std slot yields an FD_CONSOLE ofile, which reads and writes
 * exactly as the empty slot would. */
enum fd_kind { FD_VFS, FD_SHM, FD_SOCK, FD_NETSOCK, FD_TIMER, FD_EPOLL, FD_CONSOLE, FD_EVENT };

struct netsock;                 /* network (AF_INET) socket — usyscall.c        */
struct timerfd;                 /* §M53 stage 3 — a deadline behind a descriptor */
struct epoll;                   /* §M56 — a readiness set behind a descriptor   */
struct eventfd;                 /* §M90 — a counter behind a descriptor         */
struct ofile;
/* §M73 — the ofile to duplicate for `fd`: its table entry, or for an empty
 * std slot a NEW console ofile (*fresh set: the caller owns that reference). */
struct ofile* fd_dup_source(int fd, int* fresh);

struct ofile {
    enum fd_kind kind;
    int          refcount;      /* # of descriptors referencing this object */
    /* §M56.1 — O_NONBLOCK, and it lives HERE rather than in each object because
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
    struct eventfd* efd;        /* FD_EVENT (§M90)         */
};

/* Wrap a resource in a fresh ofile (refcount 1), or NULL on OOM. */
struct ofile* ofile_from_file(struct file* f);
struct ofile* ofile_from_shm (struct shm* s);
struct ofile* ofile_from_sock(struct usock* s);
struct ofile* ofile_from_netsock(struct netsock* s);
struct ofile* ofile_from_timerfd(struct timerfd* t);
struct ofile* ofile_from_epoll(struct epoll* e);
struct ofile* ofile_from_eventfd(struct eventfd* e);

/* Resolve a descriptor of the CURRENT task to its open file description, or
 * NULL if it is not open.  A live table read, not a cached pointer — which is
 * what makes it safe to compare the result against a remembered pointer
 * (§M56.2's readiness memo) rather than dereferencing the remembered one. */
struct ofile* fd_lookup(int fd);

/* §M89 — a descriptor table SHARED by the threads of one process (Linux's
 * CLONE_FILES).  A task without threads keeps its own inline table and never
 * allocates one of these.
 *
 *   fdtable_share(parent) — before creating a thread: move the parent onto a
 *       shared table (first time) and return it with one reference for the
 *       new thread;
 *   fdtable_adopt(t, ft)  — in the new thread: take that reference;
 *   fdtable_put(ft)       — give a reference back (a failed spawn).
 *
 * The table's lock serialises the slot updates (install, dup2, close), which
 * two threads can now race on.  Lookups read the slot without it, as before —
 * a descriptor closed by one thread WHILE another is inside a call on it is
 * the known gap (Linux solves it with RCU and per-file references). */
struct fdtable;
struct task;
struct fdtable* fdtable_share(struct task* parent);
void            fdtable_adopt(struct task* t, struct fdtable* ft);
void            fdtable_put(struct fdtable* ft);

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
/* §M59 — all of n bytes or none (0 = no room now, -1 = peer closed). */
long usock_send_whole(struct usock* s, const void* buf, size_t n, struct ofile* passfile);
/* Tier A.3 — `block`: when non-zero and the endpoint has nothing to receive
 * (no bytes, no passed fd) but the peer is still open, park the caller on the
 * endpoint's read wait-queue until usock_send/usock_close wakes it, then
 * re-drain.  block == 0 keeps the original non-blocking snapshot behaviour
 * (poll's drain path, single-task self-tests). */
long usock_recv (struct usock* s, void* buf, size_t n, int block,
                 struct ofile** passfile_out);
void usock_close(struct usock* s);
/* §M90 — named sockets; negative returns are Linux errnos. */
struct usock* usock_new(void);
int  usock_bind(struct usock* s, const char* name);
int  usock_listen(struct usock* s, int backlog);
int  usock_connect(struct usock* s, const char* name);
int  usock_accept(struct usock* l, int block, struct usock** out);
int  usock_is_listener(struct usock* s);
int  usock_connected(struct usock* s);
const char* usock_name(struct usock* s);
long usock_write(struct usock* s, const void* buf, size_t n, int block);
/* §M56.2 — bind the endpoint to the open file description that owns it, so a
 * readiness change can name itself.  Declared here rather than called on
 * faith: without a prototype the compiler assumes `int usock_set_owner()`,
 * which happens to pass two pointers correctly on the arches we build today
 * and is exactly the kind of luck an arch port later runs out of. */
void usock_set_owner(struct usock* s, struct ofile* o);
int  usock_can_read (struct usock* s);   /* bytes buffered? (poll POLLIN)  */
int  usock_can_write(struct usock* s);   /* peer open + space? (POLLOUT)   */
int  usock_peer_open (struct usock* s);  /* other end still there? (§M56.1)  */

/* Tier A.3 — poll readiness signal.  usock_send / usock_close call this
 * after changing an fd's readiness so a task blocked in a (timeout < 0)
 * poll() wakes and re-scans.  Defined in usyscall.c (owns the global
 * readiness wait-queue); declared here so the socket layer can raise it. */
void fd_readiness_signal(void);
/* §M90 — how many readiness events have been signalled so far (wraps).  The
 * edge for edge-triggered epoll: "something happened since I last looked". */
uint32_t fd_readiness_seq(void);

/* §M90 — close-on-exec (usyscall.c, see struct fdtable).  A descriptor is
 * created WITHOUT the bit (fd_install, dup2, F_DUPFD clear it); the ABI layer
 * sets it from O_CLOEXEC / SOCK_CLOEXEC / F_SETFD; fork copies the mask; the
 * execve commit point closes what is marked. */
struct task;
int      fd_set_cloexec(int fd, int on);     /* 0, or -1 for a bad fd */
int      fd_get_cloexec(int fd);             /* 0/1, or -1 for a bad fd */
uint32_t fd_cloexec_mask(struct task* t);
void     fd_cloexec_restore(struct task* t, uint32_t mask);
void     fd_close_on_exec(void);

/* §M56.2 — the same wake, but naming the description that changed.
 *
 * Today this is the global wake with the object recorded for diagnostics: the
 * readiness cache it was built to serve was removed (see epoll.c on why a
 * cache whose invalidation must be remembered at every mutation site is a bug
 * generator).  It is kept because naming the description that changed is
 * strictly more information than not naming it, and because passing NULL
 * degrades to exactly the old behaviour. */
void fd_readiness_changed(struct ofile* o);

/* THE definition of "is this descriptor ready" (§M56).
 *
 * poll(2) and epoll_wait(2) must agree exactly about this, so they share one
 * function rather than two switch statements that would drift apart the first
 * time a new fd kind appeared — which is precisely what happened to FD_NETSOCK,
 * reported as permanently ready by poll's fall-through for two milestones.
 *
 * Returns a bitmask of POLL* bits (syscall.h), whose values are deliberately
 * Linux's so that mapping to epoll's EPOLL* is the identity rather than a
 * translation table.  §M56.1 widened this from two 0/1 outputs to a mask because
 * readiness is not only "can I read": a hung-up peer and an error are
 * conditions a loop must be able to SEE, and POSIX reports them whether or not
 * they were requested. */
uint32_t fd_readiness(int fd);

/* The same answer for a caller that has ALREADY resolved the descriptor.
 * epoll's scan holds the ofile anyway, and paying for the lookup twice per
 * registered descriptor was, measurably, most of what a scan cost. */
uint32_t fd_readiness_of(int fd, struct ofile* o);

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
