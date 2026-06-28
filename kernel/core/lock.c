/* =============================================================================
 * lock.c — spinlock + preempt-count implementation.
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
 * preempt_count stays a plain global for now (M18 follow-up: per-CPU).
 * On a UP system we never observe it from another CPU, so the global
 * is correct; on SMP the wrong CPU might see another's count and
 * either over-eagerly skip a reschedule or pointlessly check it.
 * Tracked under §M18 follow-ups in PLAN.md.
 * ============================================================================= */

#include "lock.h"
#include "hal_api.h"
#include "atomic.h"
#include "printf.h"
#include <stdint.h>

/* Single global counter — sufficient on UP. */
static volatile int g_preempt_count = 0;

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

/* --------------------------------------------------------------------------
 * preempt_disable / preempt_enable / preempt_count
 *
 * No need for atomic operations on UP: only the kernel proper changes
 * these, and the kernel proper is the only thing the scheduler ever
 * preempts (IRQ handlers don't call disable/enable themselves).
 *
 * IRQ context can ALSO read preempt_count via schedule_check(), and it
 * does so after `cli` is implicitly in force (we're inside an IRQ), so
 * the read sees a consistent value even if the interrupted code was
 * mid-increment — because that increment compiles to a single `add` on
 * a 32-bit slot.
 * -------------------------------------------------------------------------- */
void preempt_disable(void) { g_preempt_count++; }
void preempt_enable(void)  { if (g_preempt_count > 0) g_preempt_count--; }
int  preempt_count(void)   { return g_preempt_count; }
