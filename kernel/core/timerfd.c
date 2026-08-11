/* =============================================================================
 * timerfd.c — see timerfd.h for what this is for and why it is a descriptor.
 *
 * Concurrency shape.  The expiry callback runs in INTERRUPT context (that is
 * the ktimer contract), so everything it touches has to be safe from there: it
 * bumps a counter, re-arms a periodic timer, and wakes waiters.  It allocates
 * nothing and blocks on nothing.
 *
 * The wake is deliberately TWO wakes: this object's own wait queue (a reader
 * blocked in read(2)) and the global poll readiness queue (a loop blocked in
 * poll(2) that has this fd among many).  Waking only the first would make a
 * timerfd invisible to exactly the callers it exists for.
 * ============================================================================= */

#include "timerfd.h"
#include "ktimer.h"
#include "timer.h"
#include "waitq.h"
#include "kmalloc.h"
#include "lock.h"
#include "fd.h"             /* fd_readiness_signal — the shared poll wake */
#include <stddef.h>

struct timerfd {
    struct ktimer   t;
    uint64_t        interval_ns;        /* 0 = one-shot                        */
    uint64_t        next_ns;            /* absolute deadline, 0 = disarmed     */
    volatile uint64_t expirations;      /* since the last read                 */
    struct waitq    wq;                 /* readers park here                   */
    spinlock_t      lock;               /* guards next_ns/interval_ns          */
};

/* ---------------------------------------------------------------------------
 * Expiry — interrupt context.
 * --------------------------------------------------------------------------- */
static void tfd_fired(struct ktimer* t) {
    struct timerfd* tf = (struct timerfd*)t->arg;
    if (!tf) return;

    uint32_t fl = spin_lock_irqsave(&tf->lock);
    tf->expirations++;
    if (tf->interval_ns) {
        /* Re-arm from the DEADLINE, not from now.  Rearming from now adds this
         * expiry's lateness to every subsequent period, so a periodic timer
         * would drift by however late the tick that noticed it was — and the
         * lateness is bounded by the tick, so the drift would be unbounded over
         * time.  Stepping the deadline forward instead keeps the phase.
         *
         * If the deadline is already in the past (the machine was busy for
         * longer than a whole period), skip forward whole periods rather than
         * firing a burst of catch-up expirations: the count already tells the
         * reader how many it missed, which is the honest way to report it. */
        uint64_t now = timer_now_ns();
        tf->next_ns += tf->interval_ns;
        if (tf->next_ns <= now) {
            uint64_t behind = (now - tf->next_ns) / tf->interval_ns + 1;
            tf->expirations += behind;
            tf->next_ns += behind * tf->interval_ns;
        }
        uint64_t next = tf->next_ns;
        spin_unlock_irqrestore(&tf->lock, fl);
        ktimer_arm(&tf->t, next, tfd_fired, tf);
    } else {
        tf->next_ns = 0;                /* one-shot: now disarmed */
        spin_unlock_irqrestore(&tf->lock, fl);
    }

    /* Wake the reader parked on this object AND anyone in poll(2) — a timerfd
     * that only woke its own reader would be invisible to the event loops it
     * exists to serve. */
    uint32_t wf = waitq_lock(&tf->wq);
    waitq_wake_all(&tf->wq);
    waitq_unlock(&tf->wq, wf);
    fd_readiness_signal();
}

/* ---------------------------------------------------------------------------
 * Object lifetime.
 * --------------------------------------------------------------------------- */
struct timerfd* timerfd_create_obj(void) {
    struct timerfd* tf = (struct timerfd*)kcalloc(1, sizeof *tf);
    if (!tf) return NULL;
    waitq_init(&tf->wq);
    spin_lock_init(&tf->lock);
    return tf;
}

void timerfd_close(struct timerfd* tf) {
    if (!tf) return;
    /* Cancel BEFORE freeing, and take the answer seriously: ktimer_cancel
     * returning 0 means the callback has already been taken off the list, so
     * nothing can still be about to touch this object.  A bare "is it armed"
     * test here would be the race, not the check (see ktimer.h). */
    ktimer_cancel(&tf->t);
    /* Anyone still parked would block forever on a freed queue; wake them so
     * they observe the closed fd through their own recheck. */
    uint32_t wf = waitq_lock(&tf->wq);
    waitq_wake_all(&tf->wq);
    waitq_unlock(&tf->wq, wf);
    kfree(tf);
}

/* ---------------------------------------------------------------------------
 * Arm / disarm / query.
 * --------------------------------------------------------------------------- */
int timerfd_set(struct timerfd* tf, int abs,
                uint64_t value_ns, uint64_t interval_ns) {
    if (!tf) return -1;

    if (value_ns == 0) {                        /* disarm */
        ktimer_cancel(&tf->t);
        uint32_t fl = spin_lock_irqsave(&tf->lock);
        tf->next_ns = 0;
        tf->interval_ns = 0;
        spin_unlock_irqrestore(&tf->lock, fl);
        return 0;
    }

    uint64_t deadline = abs ? value_ns : timer_now_ns() + value_ns;

    uint32_t fl = spin_lock_irqsave(&tf->lock);
    tf->interval_ns = interval_ns;
    tf->next_ns     = deadline;
    spin_unlock_irqrestore(&tf->lock, fl);

    /* ktimer_arm on an already-armed timer MOVES it, which is exactly the
     * semantic a re-arm wants — there is no "cancel first" step to forget. */
    ktimer_arm(&tf->t, deadline, tfd_fired, tf);
    return 0;
}

int timerfd_get(struct timerfd* tf, uint64_t* remaining_ns, uint64_t* interval_ns) {
    if (!tf) return -1;
    uint32_t fl = spin_lock_irqsave(&tf->lock);
    uint64_t next = tf->next_ns, iv = tf->interval_ns;
    spin_unlock_irqrestore(&tf->lock, fl);

    uint64_t now = timer_now_ns();
    if (remaining_ns) *remaining_ns = (next && next > now) ? (next - now) : 0;
    if (interval_ns)  *interval_ns  = iv;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Read + readiness.
 * --------------------------------------------------------------------------- */
int timerfd_can_read(struct timerfd* tf) {
    return tf && __atomic_load_n(&tf->expirations, __ATOMIC_ACQUIRE) != 0;
}

/* Take the whole count and zero it, atomically with respect to the callback. */
static uint64_t take_count(struct timerfd* tf) {
    uint32_t fl = spin_lock_irqsave(&tf->lock);
    uint64_t n = tf->expirations;
    tf->expirations = 0;
    spin_unlock_irqrestore(&tf->lock, fl);
    return n;
}

long timerfd_read(struct timerfd* tf, void* buf, size_t n, int block) {
    if (!tf || !buf || n < sizeof(uint64_t)) return -1;

    for (;;) {
        uint64_t count = take_count(tf);
        if (count) {
            *(uint64_t*)buf = count;
            return (long)sizeof(uint64_t);
        }
        if (!block) return -1;                  /* EAGAIN's shape */

        /* Park.  Re-test under the queue lock: the callback bumps the count
         * BEFORE it takes this lock to signal, so holding the lock and still
         * seeing zero means any wake can only arrive after we are parked —
         * which is what makes this free of lost wakeups (waitq.h's contract). */
        uint32_t f = waitq_lock(&tf->wq);
        if (__atomic_load_n(&tf->expirations, __ATOMIC_ACQUIRE)) {
            waitq_unlock(&tf->wq, f);
            continue;
        }
        waitq_block(&tf->wq);
        waitq_unlock(&tf->wq, f);
    }
}
