/* =============================================================================
 * epoll.h — a readiness SET you can wait on, kept by the kernel (§M56).
 *
 * WHY, GIVEN THAT poll(2) ALREADY WORKS.  poll's interface hands the kernel the
 * whole watch list on every call, so the cost of one wait is proportional to
 * how many descriptors the program is watching rather than to how many became
 * ready.  For a shell watching three fds that is irrelevant; for the event
 * loops this kernel now has to host — a Wayland client, a browser fetcher, a
 * server — it is the difference between a loop that scales and one that does
 * not.  epoll splits the operation in two: the SET is registered once and lives
 * in the kernel, and the WAIT reports only what changed.
 *
 * WHAT THIS IS NOT.  Our epoll_wait still SCANS the registered set rather than
 * being driven by per-fd callbacks, so the asymptotics are poll's.  The win
 * here is the interface — a program written against epoll runs unmodified, and
 * the set stops being copied across the syscall boundary on every iteration.
 * Making the wait genuinely O(ready) needs per-fd wait queues with callback
 * registration, which is a change to every fd kind and is deliberately NOT
 * bundled in with the interface.  Saying so plainly is better than implying an
 * efficiency the code does not have.
 *
 * LEVEL-TRIGGERED ONLY.  EPOLLET is accepted and ignored... no: it is
 * REJECTED.  Silently treating an edge-triggered registration as
 * level-triggered would work — a level-triggered loop is a superset — but a
 * program written for EPOLLET drains each fd exactly once per report, and a
 * level-triggered kernel would then hand it the same fd forever.  It would
 * "work" and spin.  An honest -EINVAL sends the author to the one line that
 * needs changing.
 *
 * Readiness itself is NOT defined here: it comes from fd_readiness() in fd.h,
 * which poll(2) uses too.  Two definitions of "ready" would drift.
 * ============================================================================= */

#ifndef EPOLL_H
#define EPOLL_H

#include <stdint.h>

struct epoll;

/* epoll_ctl operations — Linux's numbers, because these travel through the
 * guest ABI unchanged. */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

/* Event bits — Linux's, and deliberately the same values as POLLIN/POLLOUT so
 * the readiness mapping is an identity rather than a translation table. */
#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLET      0x80000000u        /* rejected — see the header */
#define EPOLLONESHOT 0x40000000u

/* One reported event, in KERNEL layout.  The guest's `struct epoll_event` is
 * NOT this: its size differs per ABI (12 bytes on i386 and amd64, 16 on arm64
 * — Linux packs it on x86_64 specifically so the 32- and 64-bit layouts match,
 * and does not on arm64).  That translation belongs in the ABI layer, which is
 * the only place that knows which guest is calling. */
struct epoll_ev {
    uint32_t events;
    uint64_t data;
};

struct epoll* epoll_create_obj(void);
void          epoll_close(struct epoll* ep);

/* op is EPOLL_CTL_*.  Returns 0, or a negative errno-shaped code:
 * -EINVAL (bad op / EPOLLET), -ENOENT (MOD/DEL of an unregistered fd),
 * -EEXIST (ADD of a registered fd), -ENOSPC (set full). */
int epoll_ctl_obj(struct epoll* ep, int op, int fd, uint32_t events, uint64_t data);

/* Wait for readiness in the set.  `timeout_ms` follows poll's convention
 * (<0 forever, 0 poll, >0 a real bounded wait).  Returns the number of events
 * written to `out`, 0 on timeout. */
int epoll_wait_obj(struct epoll* ep, struct epoll_ev* out, int maxevents,
                   int timeout_ms);

/* How many descriptors are registered — backs `epolltest` and diagnostics. */
int epoll_count(struct epoll* ep);

#endif
