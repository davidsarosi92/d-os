/* =============================================================================
 * itimer.c — per-process interval timers that deliver SIGALRM (§M53 stage 3).
 *
 * The other half of the timing surface a POSIX program expects.  `timerfd`
 * serves the program that WAITS for time; `setitimer`/`alarm` serves the one
 * that wants to be INTERRUPTED by it — a watchdog around a blocking call, a
 * timeout on something with no descriptor to poll.  Both are the same deadline
 * underneath (§M53 stage 2's ktimer); only the delivery differs.
 *
 * WHY A TABLE AND NOT A FIELD IN struct task.  A timer embedded in the task
 * would have to be cancelled at exactly the right point in teardown, and the
 * cost of getting that wrong is a callback firing into freed memory — the
 * §M54 failure, one layer up.  A table keyed by PID makes the timer OUTLIVE
 * nothing: the callback looks the task up, and a pid that is gone is simply a
 * miss.  Two dozen slots is far more than a machine that has never had more
 * than a handful of processes wanting one.
 *
 * DELIVERY IS A BIT, NOT A CALL.  The callback runs in interrupt context, so it
 * does the one thing that is safe there: set the pending-signal bit atomically.
 * The task notices it on its own return-to-user path, which is where every
 * other signal is delivered too — so this adds a source of signals, not a
 * second delivery mechanism.
 * ============================================================================= */

#include "syscall.h"
#include "ktimer.h"
#include "timer.h"
#include "task.h"
#include "lock.h"
#include <stddef.h>

#define ITIMER_SLOTS 24

struct itslot {
    struct ktimer t;
    int           pid;              /* 0 = free */
    uint64_t      interval_ns;
    uint64_t      next_ns;
    int           armed;
};

static struct itslot g_it[ITIMER_SLOTS];
static spinlock_t    g_it_lock;

/* Caller holds g_it_lock. */
static struct itslot* slot_for_locked(int pid, int create) {
    struct itslot* free_slot = NULL;
    for (int i = 0; i < ITIMER_SLOTS; i++) {
        if (g_it[i].pid == pid) return &g_it[i];
        if (!free_slot && g_it[i].pid == 0) free_slot = &g_it[i];
    }
    if (!create) return NULL;
    if (free_slot) { free_slot->pid = pid; }
    return free_slot;
}

/* Interrupt context.  See the file header for why this only sets a bit. */
static void it_fired(struct ktimer* t) {
    struct itslot* s = (struct itslot*)t->arg;
    if (!s) return;

    uint32_t fl = spin_lock_irqsave(&g_it_lock);
    int pid = s->pid;
    uint64_t iv = s->interval_ns;
    if (iv) {
        /* Step the deadline forward rather than restarting from now — the same
         * anti-drift rule as timerfd's, and the reason the absolute form of
         * every timing interface in this kernel is the primitive one. */
        uint64_t now = timer_now_ns();
        s->next_ns += iv;
        if (s->next_ns <= now) s->next_ns = now + iv;
    } else {
        s->next_ns = 0;
        s->armed   = 0;
    }
    uint64_t next = s->next_ns;
    spin_unlock_irqrestore(&g_it_lock, fl);

    /* Set the pending bit directly and atomically.  sys_kill would be the
     * tidier call, but it takes the scheduler's lock and applies a ring-3
     * credential rule — neither belongs on a path that runs from a timer
     * interrupt.  This is the same bit sys_kill sets. */
    if (pid > 0) {
        struct task* t = task_find(pid);
        if (t) __atomic_or_fetch(&t->sig_pending, 1u << SIGALRM, __ATOMIC_ACQ_REL);
    }
    if (iv && next) ktimer_arm(&s->t, next, it_fired, s);
}

int sys_setitimer_ns(uint64_t value_ns, uint64_t interval_ns) {
    struct task* me = task_current();
    if (!me) return -1;

    uint32_t fl = spin_lock_irqsave(&g_it_lock);
    struct itslot* s = slot_for_locked(me->pid, value_ns != 0);
    if (!s) {
        spin_unlock_irqrestore(&g_it_lock, fl);
        /* Disarming a timer that was never armed is success, not failure —
         * that is what a program clearing its alarm expects. */
        return value_ns ? -1 : 0;
    }
    if (value_ns == 0) {                        /* disarm + release the slot */
        s->armed = 0; s->next_ns = 0; s->interval_ns = 0;
        int was = s->pid; s->pid = 0;
        spin_unlock_irqrestore(&g_it_lock, fl);
        if (was) ktimer_cancel(&s->t);
        return 0;
    }
    s->interval_ns = interval_ns;
    s->next_ns     = timer_now_ns() + value_ns;
    s->armed       = 1;
    uint64_t next  = s->next_ns;
    spin_unlock_irqrestore(&g_it_lock, fl);

    ktimer_arm(&s->t, next, it_fired, s);       /* re-arming MOVES it */
    return 0;
}

int sys_getitimer_ns(uint64_t* value_ns, uint64_t* interval_ns) {
    struct task* me = task_current();
    if (!me) return -1;
    uint64_t rem = 0, iv = 0;
    uint32_t fl = spin_lock_irqsave(&g_it_lock);
    struct itslot* s = slot_for_locked(me->pid, 0);
    if (s && s->armed) {
        uint64_t now = timer_now_ns();
        rem = (s->next_ns > now) ? (s->next_ns - now) : 0;
        iv  = s->interval_ns;
    }
    spin_unlock_irqrestore(&g_it_lock, fl);
    if (value_ns)    *value_ns    = rem;
    if (interval_ns) *interval_ns = iv;
    return 0;
}

/* Called from the task teardown path.  A timer whose owner is gone must not
 * survive it: the pid would eventually be reused and the alarm would land on
 * an unrelated process — a bug that would look like a random SIGALRM and be
 * very hard to attribute. */
void itimer_cancel_pid(int pid) {
    if (pid <= 0) return;
    uint32_t fl = spin_lock_irqsave(&g_it_lock);
    struct itslot* s = slot_for_locked(pid, 0);
    if (!s) { spin_unlock_irqrestore(&g_it_lock, fl); return; }
    s->pid = 0; s->armed = 0; s->next_ns = 0; s->interval_ns = 0;
    spin_unlock_irqrestore(&g_it_lock, fl);
    ktimer_cancel(&s->t);
}
