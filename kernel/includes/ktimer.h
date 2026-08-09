/* =============================================================================
 * ktimer.h — deadline timers, in nanoseconds (§M53 stage 2).
 *
 * WHAT THIS ADDS THAT THE SCHEDULER DID NOT HAVE.  Everything time-based in the
 * kernel so far was a POLL: `task_msleep` parked a task and a sweep on every
 * tick asked "is anyone due yet".  That answers one question ("wake sleeping
 * tasks") at one resolution (the tick) and nothing else.  A timer that fires a
 * CALLBACK at a nanosecond deadline is the primitive the rest of the timing
 * work is built out of — precise sleeps, `timerfd`, POSIX interval timers,
 * socket and poll timeouts — and each of those would otherwise grow its own
 * private sweep.
 *
 * STRUCTURE: a single sorted list, protected by one lock.  Deliberately not a
 * hierarchical wheel: a wheel earns its complexity at thousands of pending
 * timers, and the honest number here is a handful.  Insertion is O(n) in that
 * handful and expiry is O(1) per fired timer, which is the right shape until a
 * measurement says otherwise.  The interface does not expose the list, so
 * changing it later costs nothing above this line.
 *
 * WHERE THE CALLBACK RUNS: interrupt context, on whichever CPU noticed the
 * deadline, with the timer lock NOT held (so a callback may arm another timer).
 * That means a callback must be short and must not block — the same contract as
 * an ISR.  Anything heavier belongs on the workqueue (§M49), which is exactly
 * what `work_submit` from a callback is for.
 *
 * OWNERSHIP: the caller owns the `struct ktimer` storage and must keep it alive
 * until the timer has fired or been cancelled.  `ktimer_cancel` returns whether
 * it actually removed a pending timer, which is the only safe way to know a
 * callback will not still run — a bare "is it armed" test would be a race.
 * ============================================================================= */

#ifndef KTIMER_H
#define KTIMER_H

#include <stdint.h>

struct ktimer;
typedef void (*ktimer_fn)(struct ktimer* t);

struct ktimer {
    uint64_t       deadline_ns;   /* absolute, on the timer_now_ns() timeline */
    ktimer_fn      fn;
    void*          arg;           /* caller's context; the kernel never reads it */
    struct ktimer* next;          /* list link — private to ktimer.c            */
    int            armed;         /* private; use ktimer_cancel to test+remove  */
};

/* Arm `t` to fire at absolute `deadline_ns`.  Re-arming an already-armed timer
 * moves it, which is what a repeating timer wants and what a caller resetting a
 * timeout expects; there is no "already armed" error to forget to check.
 * A deadline already in the past fires at the next expiry pass rather than
 * being rejected — "late" is a normal outcome for a timer, "ignored" is not. */
void ktimer_arm(struct ktimer* t, uint64_t deadline_ns, ktimer_fn fn, void* arg);

/* Convenience: arm relative to now. */
void ktimer_arm_after(struct ktimer* t, uint64_t delay_ns, ktimer_fn fn, void* arg);

/* Remove a pending timer.  Returns 1 if it was still pending (so its callback
 * will NOT run), 0 if it had already fired or was never armed.  Safe to call on
 * a timer that has fired. */
int ktimer_cancel(struct ktimer* t);

/* Fire everything now due.  Called from the timer-interrupt exit path; safe to
 * call from any CPU and at any rate — it does nothing when nothing is due. */
void ktimer_expire(void);

/* Diagnostics for the `ktimer` shell command: how many are pending, how many
 * have ever fired, and the worst observed lateness (fire time minus deadline).
 * Lateness is the number that says whether the timer service is actually
 * meeting its deadlines or merely keeping a list — the tick period is its floor
 * until a one-shot hardware deadline replaces the periodic tick. */
void ktimer_stats(uint32_t* pending, uint64_t* fired, uint64_t* max_late_ns);

#endif
