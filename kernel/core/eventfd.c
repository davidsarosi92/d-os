/* =============================================================================
 * eventfd.c — a counter behind a descriptor (Linux eventfd, §M90).
 *
 * Why it exists here: Go's runtime wakes its network poller through one (the
 * netpoller is epoll + an eventfd), so no Go program — docker, dockerd,
 * containerd — got past its first file open without it ("runtime: eventfd
 * failed").  It is also the cheapest cross-thread wake-up a C program has.
 *
 * Semantics (eventfd(2)): a 64-bit counter.  write(8 bytes) adds; read(8)
 * returns the counter and zeroes it — or, with EFD_SEMAPHORE, returns 1 and
 * decrements.  A read of 0 blocks (EAGAIN when non-blocking); a write that
 * would carry the counter past 0xFFFFFFFFFFFFFFFE blocks likewise.  Readable
 * while the counter is non-zero; writable while one more can be added.
 *
 * The shape is timerfd.c's: one waitq for both directions (a reader parks
 * until a write, a writer until a read), and every change also calls
 * fd_readiness_changed so poll/epoll waiters on it see the transition.
 * ============================================================================= */

#include "eventfd.h"
#include "fd.h"
#include "kmalloc.h"
#include "waitq.h"
#include "lock.h"
#include <stdint.h>
#include <stddef.h>

#define EFD_MAX 0xFFFFFFFFFFFFFFFEull

struct eventfd {
    uint64_t       count;
    int            semaphore;
    spinlock_t     lock;
    struct waitq   wq;
    struct ofile*  owner;
};

struct eventfd* eventfd_create_obj(uint64_t init, int semaphore) {
    struct eventfd* e = (struct eventfd*)kcalloc(1, sizeof *e);
    if (!e) return NULL;
    e->count = init;
    e->semaphore = semaphore ? 1 : 0;
    spin_lock_init(&e->lock);
    waitq_init(&e->wq);
    return e;
}

void eventfd_set_owner(struct eventfd* e, struct ofile* o) { if (e) e->owner = o; }

void eventfd_close(struct eventfd* e) {
    if (!e) return;
    uint32_t f = waitq_lock(&e->wq);
    waitq_wake_all(&e->wq);
    waitq_unlock(&e->wq, f);
    kfree(e);
}

static void efd_changed(struct eventfd* e) {
    uint32_t f = waitq_lock(&e->wq);
    waitq_wake_all(&e->wq);
    waitq_unlock(&e->wq, f);
    fd_readiness_changed(e->owner);
}

long eventfd_read(struct eventfd* e, void* buf, size_t n, int block) {
    if (!e || !buf) return -22;
    if (n < sizeof(uint64_t)) return -22;                      /* EINVAL */
    for (;;) {
        uint32_t fl = spin_lock_irqsave(&e->lock);
        if (e->count) {
            uint64_t v;
            if (e->semaphore) { v = 1; e->count--; }
            else              { v = e->count; e->count = 0; }
            spin_unlock_irqrestore(&e->lock, fl);
            *(uint64_t*)buf = v;
            efd_changed(e);                                    /* a writer may proceed */
            return (long)sizeof(uint64_t);
        }
        spin_unlock_irqrestore(&e->lock, fl);
        if (!block) return -11;                                /* EAGAIN */
        uint32_t f = waitq_lock(&e->wq);
        if (!__atomic_load_n(&e->count, __ATOMIC_ACQUIRE)) waitq_block(&e->wq);
        waitq_unlock(&e->wq, f);
    }
}

long eventfd_write(struct eventfd* e, const void* buf, size_t n, int block) {
    if (!e || !buf) return -22;
    if (n < sizeof(uint64_t)) return -22;
    uint64_t v = *(const uint64_t*)buf;
    if (v == 0xFFFFFFFFFFFFFFFFull) return -22;
    for (;;) {
        uint32_t fl = spin_lock_irqsave(&e->lock);
        if (e->count + v <= EFD_MAX && e->count + v >= e->count) {
            e->count += v;
            spin_unlock_irqrestore(&e->lock, fl);
            if (v) efd_changed(e);
            return (long)sizeof(uint64_t);
        }
        spin_unlock_irqrestore(&e->lock, fl);
        if (!block) return -11;
        uint32_t f = waitq_lock(&e->wq);
        if (__atomic_load_n(&e->count, __ATOMIC_ACQUIRE) + v > EFD_MAX) waitq_block(&e->wq);
        waitq_unlock(&e->wq, f);
    }
}

int eventfd_can_read(struct eventfd* e)  { return e && __atomic_load_n(&e->count, __ATOMIC_ACQUIRE) != 0; }
int eventfd_can_write(struct eventfd* e) { return e && __atomic_load_n(&e->count, __ATOMIC_ACQUIRE) < EFD_MAX; }
