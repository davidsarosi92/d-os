/* =============================================================================
 * timerfd.h — a deadline you can WAIT ON alongside everything else (§M53 st.3).
 *
 * WHY A DESCRIPTOR AND NOT A CALLBACK.  §M53 stage 2 gave the kernel deadline
 * timers, and `clock_nanosleep` gave a program a way to wait for one — but only
 * by doing nothing else while it waits.  A real event loop cannot afford that:
 * it is already blocked in `poll` on sockets and pipes, and "wake me in 20 ms"
 * has to arrive through the SAME wait, or the loop has to choose between being
 * responsive to I/O and being punctual.  Wrapping the timer in a descriptor is
 * what makes the two commensurable — the timeout becomes just another readable
 * fd, and the loop keeps one blocking point instead of two.
 *
 * That is also precisely what an `epoll`-shaped loop needs, which is why this
 * is the piece that had to land before the async work rather than after it.
 *
 * SEMANTICS (Linux's, deliberately — this is the shape libraries expect):
 *   - a read yields a u64 EXPIRATION COUNT and resets it to zero, so a loop
 *     that fell behind learns HOW FAR behind rather than silently coalescing;
 *   - a periodic timer keeps counting while nobody reads, for the same reason;
 *   - the fd is readable exactly while that count is non-zero.
 *
 * The count is what makes a missed tick observable instead of invisible.  A
 * timer that quietly dropped the ticks nobody collected would let a program
 * drift with no way to notice — the same failure §M53 stage 1 found in the
 * millisecond clock, one layer up.
 * ============================================================================= */

#ifndef TIMERFD_H
#define TIMERFD_H

#include <stdint.h>
#include <stddef.h>

struct timerfd;

/* Create a disarmed timer object.  NULL on OOM. */
struct timerfd* timerfd_create_obj(void);
void            timerfd_close(struct timerfd* tf);

/* Arm (or, with value_ns == 0, disarm).  `abs` selects an absolute deadline on
 * the timer_now_ns() timeline instead of a delay from now — the same
 * distinction, and for the same reason, as clock_nanosleep's: a periodic loop
 * written against a relative rearm DRIFTS by however long each iteration took.
 * `interval_ns` != 0 makes it periodic.  Returns 0, or -1 on a bad object. */
int timerfd_set(struct timerfd* tf, int abs,
                uint64_t value_ns, uint64_t interval_ns);

/* Report the time remaining until the next expiry and the interval (both 0 for
 * a disarmed timer) — Linux's timerfd_gettime, which reports REMAINING time
 * rather than the deadline that was set. */
int timerfd_get(struct timerfd* tf, uint64_t* remaining_ns, uint64_t* interval_ns);

/* Read the expiration count into an 8-byte buffer and reset it.  `block`
 * parks the caller until at least one expiry has happened; without it an
 * empty timer returns -1 (EAGAIN's shape).  `n` must be >= 8. */
long timerfd_read(struct timerfd* tf, void* buf, size_t n, int block);

/* poll(2) readiness: non-zero while a read would succeed without blocking. */
int timerfd_can_read(struct timerfd* tf);

#endif
