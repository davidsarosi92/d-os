/* =============================================================================
 * lock.c — spinlock + per-CPU preempt-count implementation.
 *
 * M18 made the spinlock real: `lock cmpxchg` test-and-set with pause-
 * loop backoff, plus IRQ-save so the CPU we run on can't preempt
 * itself while holding the lock.  The previous UP stub (just cli/sti)
 * was correct on one CPU but became a no-op the moment APs joined.
 *
 * Pattern (Linux-style `spin_lock_irqsave`):
 *   1. hal_intr_save() — disable IRQs on THIS CPU and save the prior
 *      flag state.  Stops self-preemption.
 *   2. atomic_cmpxchg(&l->locked, 0, 1) until success — spin until we
 *      win the contention with peer CPUs.  `hal_cpu_pause` between
 *      attempts to relax the pipeline.
 *
 * Release does the inverse: store-release 0 to `locked` so peer CPUs'
 * acquire-load sees zero with our preceding writes visible, then
 * restore IRQ state.
 *
 * M18.6.2 — preempt_count moved per-CPU.  The previous global was
 * SMP-incorrect: `preempt_disable()` on CPU A also blocked preemption
 * on CPU B, which both starves the other core and masks real races.
 * Per-CPU means "do not reschedule THIS CPU."  Atomicity is achieved
 * by bracketing the read-modify-write in `hal_intr_save/restore` so
 * the local timer IRQ can't observe a half-update; cross-CPU access
 * is never needed (no one ever reads peers' count).
 * ============================================================================= */

#include "lock.h"
#include "hal_api.h"
#include "atomic.h"
#include "percpu.h"
#include "printf.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * spinlock_t — irqsave / irqrestore.
 *
 * The "save flags" path:
 *   pushf  → push current EFLAGS onto the stack
 *   pop fl → pop them back into a C variable
 *   cli    → mask IRQs
 *
 * Why we don't just save IF bit by bit: pushf is one instruction and gives
 * us the whole register, which means a future irqrestore can use popf to
 * put back not only IF but also any flag a debugger / instrumentation
 * might have changed.
 * -------------------------------------------------------------------------- */
uint32_t spin_lock_irqsave(spinlock_t* l) {
    /* IRQs off on THIS CPU first — prevents the timer IRQ from yanking
     * the CPU away from us mid-lock and creating a hold time so long
     * that another CPU's acquire times out.  On UP this is also the
     * only protection we need; on SMP the cmpxchg below picks up
     * cross-CPU contention. */
    uint32_t fl = hal_intr_save();

    /* Spin until we win the test-and-set.  `hal_cpu_pause` (= x86
     * `pause`) drops the CPU out of speculation-heavy mode and avoids
     * starving the lock holder on the other core.  M18 follow-up:
     * exponential backoff + queued ticket lock if we ever measure
     * pathological contention. */
    while (!atomic_cmpxchg(&l->locked, 0, 1)) {
        hal_cpu_pause();
    }
    return fl;
}

void spin_unlock_irqrestore(spinlock_t* l, uint32_t flags) {
    /* Store-release so any peer CPU that subsequently acquires the
     * lock observes every write we made inside the critical section. */
    atomic_store_release(&l->locked, 0);
    hal_intr_restore(flags);
}

void spin_unlock(spinlock_t* l) {
    /* Lock-handoff variant — see spinlock_t comment in lock.h.  Just
     * drops the lock; IRQ state is the caller's problem. */
    atomic_store_release(&l->locked, 0);
}

void spin_lock(spinlock_t* l) {
    /* Plain acquire — see lock.h.  Caller is responsible for IRQ-off. */
    while (!atomic_cmpxchg(&l->locked, 0, 1)) {
        hal_cpu_pause();
    }
}

/* --------------------------------------------------------------------------
 * preempt_disable / preempt_enable / preempt_count — per-CPU (M18.6.2).
 *
 * The counter lives in `struct percpu->preempt_count`.  Why IRQ-off
 * around the increment?  Without it, the timer IRQ could fire between
 * the load and store of `count++`, observe the not-yet-updated value
 * via schedule_check, and either preempt when it shouldn't (we wanted
 * to disable) or hold off when it shouldn't (we just enabled).
 *
 * IRQ context can ALSO call preempt_disable/enable today (e.g. an IRQ
 * handler that briefly disables preemption while walking shared
 * state).  Same path works there — IRQs are already off at handler
 * entry, hal_intr_save returns "off" and the IRQ-on restore is a
 * no-op.
 *
 * Migration safety: between `hal_intr_save` and `hal_intr_restore`
 * the CPU can't context-switch (timer IRQ is masked), so `this_cpu()`
 * is stable across the read-modify-write.  Without IRQ-off, a
 * preempt-point could land us on a different CPU mid-increment and
 * the wrong slot would be modified.
 * -------------------------------------------------------------------------- */
void preempt_disable(void) {
    uint32_t fl = hal_intr_save();
    this_cpu()->preempt_count++;
    hal_intr_restore(fl);
}

void preempt_enable(void) {
    uint32_t fl = hal_intr_save();
    struct percpu* me = this_cpu();
    if (me->preempt_count > 0) me->preempt_count--;
    hal_intr_restore(fl);
}

int preempt_count(void) {
    /* Caller is always THIS CPU's code (asking "am I in a no-preempt
     * region").  Single-load on a single-word slot; the IRQ-exit path
     * that consults this (schedule_check) is already running with
     * IF=0, so the value is stable for that consumer.  Other callers
     * use this as a heuristic and don't need a fence either. */
    return this_cpu()->preempt_count;
}
