/* =============================================================================
 * ktimer.c — the deadline timer list (§M53 stage 2).  See ktimer.h for the
 * contract and for why this is a plain sorted list rather than a wheel.
 * ============================================================================= */

#include "ktimer.h"
#include "percpu.h"   /* this_cpu_id: the running-callback table */
#include "hal_api.h"  /* hal_cpu_pause */
#include "timer.h"
#include "lock.h"
#include <stddef.h>

static spinlock_t      g_lock;
#define KT_MAX_CPUS 64
/* Which timer each CPU is running a callback for, if any — ktimer_cancel waits
 * on it.  Written by the expiring CPU only; read by cancellers on others. */
static struct ktimer* volatile g_running[KT_MAX_CPUS];
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

/* A CANCEL THAT RETURNS WHILE THE CALLBACK IS STILL RUNNING IS NOT A CANCEL
 * (2026-09-25).  ktimer_expire takes a timer off the list, drops the lock and
 * THEN calls it — so "not on the list" also means "possibly executing right
 * now on another CPU".  This used to return at once, and the caller, told the
 * timer was gone, returned from its function; a timer that lived in that stack
 * frame (task_sleep_until_ns, the network waits, virtio-blk's backstop) was
 * then read by a callback still running on another CPU, out of a frame already
 * reused.  Seen as `SPINLOCK STUCK` in vblk_backstop <- ktimer_expire on a
 * waitq address that was a stale stack slot.  Linux calls the fix
 * del_timer_sync; this is that.  It waits for the callback on every OTHER CPU
 * (a callback cancelling its own timer must not wait for itself), and repeats
 * if the callback re-armed the timer meanwhile. */
int ktimer_cancel(struct ktimer* t) {
    if (!t) return 0;
    int was = 0;
    int me = this_cpu_id();
    for (int round = 0; round < 8; round++) {
        uint32_t fl = spin_lock_irqsave(&g_lock);
        was |= unlink_locked(t);
        spin_unlock_irqrestore(&g_lock, fl);
        int busy;
        do {
            busy = 0;
            for (int c = 0; c < KT_MAX_CPUS; c++)
                if (c != me && __atomic_load_n(&g_running[c], __ATOMIC_ACQUIRE) == t) busy = 1;
            if (busy) hal_cpu_pause();
        } while (busy);
        if (!ktimer_armed(t)) break;          /* re-armed by its callback: again */
    }
    return was;
}

/* §M71 — the read-only form.  No lock: a single aligned int, and a caller that
 * races an arm/cancel gets one of two answers that were both true. */
int ktimer_armed(const struct ktimer* t) { return t && t->armed; }

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
        /* Lateness is measured, not assumed.  Its floor is the tick period —
         * and, under emulation, the emulator's own timer dispatch: a one-shot
         * LAPIC deadline was built, measured and removed (DOCS §4.53.1)
         * because its own interrupt arrived 1.6-1.7 ms late, no better than
         * the PIT path it would have replaced. */
        uint64_t late = now - t->deadline_ns;
        if (late > g_max_late_ns) g_max_late_ns = late;
        ktimer_fn fn = t->fn;
        int cpu = this_cpu_id();
        if (cpu >= 0 && cpu < KT_MAX_CPUS) g_running[cpu] = t;   /* see ktimer_cancel */
        spin_unlock_irqrestore(&g_lock, fl);

        if (fn) fn(t);
        if (cpu >= 0 && cpu < KT_MAX_CPUS)
            __atomic_store_n(&g_running[cpu], (struct ktimer*)0, __ATOMIC_RELEASE);
        /* `t` may already have been re-armed, freed, or reused by the callback
         * — never touch it again here. */
    }
}

/* Cancel every armed timer whose STRUCT lies in [lo, hi) — the kernel stack
 * of a task that is exiting.  Returns how many were still armed and, through
 * `first_fn`, the callback of the first, so the caller can name the bug.
 *
 * WHY THIS EXISTS (2026-09-25).  An on-stack timer is cancelled by the frame
 * that armed it, on its way out — unless the task never gets back to that
 * frame.  task_yield() EXITS a task with a pending kill, from wherever it is
 * called, and a wait loop that yields while its timer is armed (net.c's
 * no-poller fallback did) therefore left a live list entry pointing into a
 * stack that the reaper then freed and the allocator handed to somebody else.
 * When the deadline came, the tick called whatever those bytes now held: an
 * x86_64 NMI with the CPU executing inside the font tables, after a session
 * end — the §M82-session signature (NEXT.md #2b) that "locking the config
 * store" had only made rarer.  Cancelling here makes every such path safe, and
 * the caller's report makes every such path name itself. */
int ktimer_cancel_range(uintptr_t lo, uintptr_t hi, ktimer_fn* first_fn) {
    int n = 0;
    int me = this_cpu_id();
    uint32_t fl = spin_lock_irqsave(&g_lock);
    struct ktimer** pp = &g_head;
    while (*pp) {
        struct ktimer* t = *pp;
        if ((uintptr_t)t >= lo && (uintptr_t)t < hi) {
            if (!n && first_fn) *first_fn = t->fn;
            *pp = t->next;
            t->next = NULL;
            t->armed = 0;
            if (g_pending) g_pending--;
            n++;
            continue;
        }
        pp = &t->next;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    /* And a callback already RUNNING on another CPU from that range must
     * finish before the stack can be given away (ktimer_cancel's rule). */
    int busy;
    do {
        busy = 0;
        for (int c = 0; c < KT_MAX_CPUS; c++) {
            if (c == me) continue;
            uintptr_t r = (uintptr_t)__atomic_load_n(&g_running[c], __ATOMIC_ACQUIRE);
            if (r >= lo && r < hi) busy = 1;
        }
        if (busy) hal_cpu_pause();
    } while (busy);
    return n;
}

/* Zero the worst-lateness figure, so a measurement is not polluted by
 * whatever happened during boot (the worst figure is a maximum since the last
 * reset, and boot is when the machine is busiest). */
void ktimer_stats_reset(void) {
    uint32_t fl = spin_lock_irqsave(&g_lock);
    g_max_late_ns = 0;
    spin_unlock_irqrestore(&g_lock, fl);
}

void ktimer_stats(uint32_t* pending, uint64_t* fired, uint64_t* max_late_ns) {
    uint32_t fl = spin_lock_irqsave(&g_lock);
    if (pending)     *pending     = g_pending;
    if (fired)       *fired       = g_fired;
    if (max_late_ns) *max_late_ns = g_max_late_ns;
    spin_unlock_irqrestore(&g_lock, fl);
}
