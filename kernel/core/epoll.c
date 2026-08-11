/* =============================================================================
 * epoll.c — see epoll.h for what this is and, more importantly, what it is not.
 *
 * The whole of the blocking behaviour is fd_readiness_wait() (fd.h): this file
 * supplies a `scan` function and owns the registered set, nothing else.  That
 * is deliberate — the check-then-park discipline that keeps a wakeup from being
 * lost is subtle enough that a second copy of it would eventually be a second
 * bug, and poll(2) already had the first copy.
 * ============================================================================= */

#include "epoll.h"
#include "fd.h"
#include "kmalloc.h"
#include "lock.h"
#include "task.h"
#include <stddef.h>

#define EPOLL_MAX_ITEMS 64      /* generous next to a machine whose largest
                                 * event loop watches a handful of fds */

/* errno values, in the shape the ABI layer passes straight through. */
#define EP_EINVAL 22
#define EP_ENOENT  2
#define EP_EEXIST 17
#define EP_ENOSPC 28

struct epitem {
    int      fd;
    uint32_t events;            /* what the caller asked to hear about */
    uint64_t data;              /* opaque cookie, handed back verbatim */
    int      used;
};

struct epoll {
    struct epitem it[EPOLL_MAX_ITEMS];
    spinlock_t    lock;         /* guards the set against a concurrent ctl */
};

struct epoll* epoll_create_obj(void) {
    struct epoll* ep = (struct epoll*)kcalloc(1, sizeof *ep);
    if (!ep) return NULL;
    spin_lock_init(&ep->lock);
    return ep;
}

void epoll_close(struct epoll* ep) {
    /* No per-item references are held: an epoll set watches descriptor NUMBERS,
     * exactly like poll's array does.  A closed-and-reused fd therefore silently
     * changes what an entry refers to — which is Linux's behaviour too, and the
     * reason epoll_ctl(DEL) before close is the documented discipline rather
     * than a nicety. */
    kfree(ep);
}

/* Caller holds ep->lock. */
static struct epitem* find_locked(struct epoll* ep, int fd) {
    for (int i = 0; i < EPOLL_MAX_ITEMS; i++)
        if (ep->it[i].used && ep->it[i].fd == fd) return &ep->it[i];
    return NULL;
}

int epoll_ctl_obj(struct epoll* ep, int op, int fd, uint32_t events, uint64_t data) {
    if (!ep || fd < 0) return -EP_EINVAL;

    /* Refuse edge-triggered rather than quietly downgrading it.  A program
     * written for EPOLLET drains each fd once per report; served
     * level-triggered it would be handed the same fd forever and spin while
     * appearing to work.  A loud -EINVAL points at the one line to change. */
    if (op != EPOLL_CTL_DEL && (events & EPOLLET)) return -EP_EINVAL;

    uint32_t f = spin_lock_irqsave(&ep->lock);
    int rc = 0;
    struct epitem* e = find_locked(ep, fd);

    switch (op) {
    case EPOLL_CTL_ADD:
        if (e) { rc = -EP_EEXIST; break; }
        for (int i = 0; i < EPOLL_MAX_ITEMS; i++) {
            if (!ep->it[i].used) {
                ep->it[i].used = 1; ep->it[i].fd = fd;
                ep->it[i].events = events; ep->it[i].data = data;
                e = &ep->it[i];
                break;
            }
        }
        if (!e) rc = -EP_ENOSPC;
        break;
    case EPOLL_CTL_MOD:
        if (!e) { rc = -EP_ENOENT; break; }
        e->events = events; e->data = data;
        break;
    case EPOLL_CTL_DEL:
        if (!e) { rc = -EP_ENOENT; break; }
        e->used = 0;
        break;
    default:
        rc = -EP_EINVAL;
        break;
    }
    spin_unlock_irqrestore(&ep->lock, f);

    /* A registration change alters what SOME waiter should be looking at, and
     * a task already parked in epoll_wait re-scans on any signal.  Without this
     * an fd added by one thread would not be noticed by another already
     * waiting until something unrelated happened to wake it. */
    if (rc == 0) fd_readiness_signal();
    return rc;
}

int epoll_count(struct epoll* ep) {
    if (!ep) return 0;
    int n = 0;
    uint32_t f = spin_lock_irqsave(&ep->lock);
    for (int i = 0; i < EPOLL_MAX_ITEMS; i++) if (ep->it[i].used) n++;
    spin_unlock_irqrestore(&ep->lock, f);
    return n;
}

/* ---------------------------------------------------------------------------
 * The scan.  Called by fd_readiness_wait, sometimes with the readiness queue
 * lock held — so it may take ep->lock (a leaf) but must not block.
 * --------------------------------------------------------------------------- */
struct ep_scan_ctx {
    struct epoll*    ep;
    struct epoll_ev* out;
    int              maxevents;
    int              n;         /* events written by the last scan */
};

/* Which bits are reported without being asked for.  Same rule as poll's, and
 * for the same reason: a program cannot decline to hear that its peer hung up,
 * because the alternative way to find out is the blocking read it built an
 * event loop to avoid.  EPOLLRDHUP is Linux's own and IS request-gated. */
#define EP_ALWAYS (EPOLLERR | EPOLLHUP)

static int ep_scan(void* c) {
    struct ep_scan_ctx* s = (struct ep_scan_ctx*)c;
    int n = 0;

    uint32_t f = spin_lock_irqsave(&s->ep->lock);
    for (int i = 0; i < EPOLL_MAX_ITEMS && n < s->maxevents; i++) {
        struct epitem* e = &s->ep->it[i];
        if (!e->used) continue;

        /* The shared definition (fd.h).  POLL* and EPOLL* deliberately have
         * the same values, so this is a mask, not a translation. */
        uint32_t rev = fd_readiness(e->fd) & (e->events | EP_ALWAYS);
        if (!rev) continue;

        s->out[n].events = rev;
        s->out[n].data   = e->data;
        n++;

        /* EPOLLONESHOT: disarm on report.  The entry stays registered — the
         * caller re-arms with MOD, which is the whole point of the flag (it
         * hands ownership of the fd to whoever took the event without a
         * remove/add round trip). */
        if (e->events & EPOLLONESHOT) e->events &= ~(EPOLLIN | EPOLLOUT);
    }
    spin_unlock_irqrestore(&s->ep->lock, f);

    s->n = n;
    return n;
}

/* Does a wait return immediately?  Deliberately implemented by asking the same
 * scan for ONE event rather than by keeping a separate "is ready" flag: a
 * second answer to the same question is a second thing to keep in step. */
#define EPOLL_MAX_DEPTH 4       /* Linux allows 5; the point is that it is
                                 * bounded, not the exact number */

int epoll_has_events(struct epoll* ep) {
    if (!ep) return 0;

    /* An epoll set is pollable, so a set may watch another set — and nothing
     * stops two sets watching each other.  Without this bound that cycle
     * recurses until the kernel stack is gone, holding a lock on every frame.
     * Refusing to descend further reports "not ready", which is the safe
     * answer: a too-deep chain simply never fires, rather than crashing the
     * machine that built it. */
    struct task* t = task_current();
    if (t) {
        if (t->epoll_depth >= EPOLL_MAX_DEPTH) return 0;
        t->epoll_depth++;
    }
    struct epoll_ev one;
    struct ep_scan_ctx c = { ep, &one, 1, 0 };
    int r = ep_scan(&c) > 0;
    if (t) t->epoll_depth--;
    return r;
}

int epoll_wait_obj(struct epoll* ep, struct epoll_ev* out, int maxevents,
                   int timeout_ms) {
    if (!ep || !out || maxevents <= 0) return -EP_EINVAL;
    struct ep_scan_ctx c = { ep, out, maxevents, 0 };
    fd_readiness_wait(ep_scan, &c, timeout_ms);
    return c.n;
}
