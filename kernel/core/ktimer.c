/* =============================================================================
 * ktimer.c — the deadline timer list (§M53 stage 2).  See ktimer.h for the
 * contract and for why this is a plain sorted list rather than a wheel.
 * ============================================================================= */

#include "ktimer.h"
#include "timer.h"
#include "lock.h"
#include <stddef.h>

static spinlock_t      g_lock;
static struct ktimer*  g_head;          /* sorted by deadline, earliest first */
static uint32_t        g_pending;
static uint64_t        g_fired;
static uint64_t        g_max_late_ns;

/* Unlink `t` if present.  Caller holds g_lock. */
static int unlink_locked(struct ktimer* t) {
    struct ktimer** pp = &g_head;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            t->next  = NULL;
            t->armed = 0;
            if (g_pending) g_pending--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

void ktimer_arm(struct ktimer* t, uint64_t deadline_ns, ktimer_fn fn, void* arg) {
    if (!t || !fn) return;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    unlink_locked(t);                    /* re-arm = move, see the header */

    t->deadline_ns = deadline_ns;
    t->fn          = fn;
    t->arg         = arg;
    t->armed       = 1;

    struct ktimer** pp = &g_head;
    while (*pp && (*pp)->deadline_ns <= deadline_ns) pp = &(*pp)->next;
    t->next = *pp;
    *pp     = t;
    g_pending++;
    spin_unlock_irqrestore(&g_lock, fl);
}

void ktimer_arm_after(struct ktimer* t, uint64_t delay_ns, ktimer_fn fn, void* arg) {
    ktimer_arm(t, timer_now_ns() + delay_ns, fn, arg);
}

int ktimer_cancel(struct ktimer* t) {
    if (!t) return 0;
    uint32_t fl = spin_lock_irqsave(&g_lock);
    int was = unlink_locked(t);
    spin_unlock_irqrestore(&g_lock, fl);
    return was;
}

void ktimer_expire(void) {
    uint64_t now = timer_now_ns();

    for (;;) {
        /* Take ONE timer per iteration, releasing the lock before calling its
         * callback.  Holding the lock across the callback would forbid the one
         * thing callbacks most want to do — arm the next timer — and would
         * turn any callback that touches a lock into an ordering hazard
         * against every other timer user. */
        uint32_t fl = spin_lock_irqsave(&g_lock);
        struct ktimer* t = g_head;
        if (!t || t->deadline_ns > now) {
            spin_unlock_irqrestore(&g_lock, fl);
            return;
        }
        g_head   = t->next;
        t->next  = NULL;
        t->armed = 0;
        if (g_pending) g_pending--;
        g_fired++;
        /* Lateness is measured, not assumed.  Its floor is the tick period
         * until a one-shot hardware deadline replaces the periodic tick, and
         * the only way to know whether that upgrade is worth doing is to have
         * the number. */
        uint64_t late = now - t->deadline_ns;
        if (late > g_max_late_ns) g_max_late_ns = late;
        ktimer_fn fn = t->fn;
        spin_unlock_irqrestore(&g_lock, fl);

        if (fn) fn(t);
        /* `t` may already have been re-armed, freed, or reused by the callback
         * — never touch it again here. */
    }
}

void ktimer_stats(uint32_t* pending, uint64_t* fired, uint64_t* max_late_ns) {
    uint32_t fl = spin_lock_irqsave(&g_lock);
    if (pending)     *pending     = g_pending;
    if (fired)       *fired       = g_fired;
    if (max_late_ns) *max_late_ns = g_max_late_ns;
    spin_unlock_irqrestore(&g_lock, fl);
}
