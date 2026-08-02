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
#include "crash.h"      /* §M47 — record a probable deadlock */
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
/* --------------------------------------------------------------------------
 * Deadlock observability.  A plain spinlock that spins for an absurd number of
 * iterations is almost certainly deadlocked (on UP: a lock held by a task that
 * was preempted, spun on from a context that can't yield — an IRQ-off critical
 * section — so the holder never runs again).  Such a hang used to be totally
 * silent (the box just freezes).  Now, past a threshold WAY above any legitimate
 * contention, we emit the lock address + the caller's return address straight to
 * the serial port — lock-free port I/O that works even with IRQs disabled — so a
 * capture pins the exact lock + site.  We keep spinning afterwards (re-arming so
 * it re-prints), so this only ever adds output; it never changes behaviour on a
 * healthy system.
 * -------------------------------------------------------------------------- */
/* The threshold has ONE hard requirement: it must fire before the ~4 s hardware
 * watchdog resets the box, or the most useful message in the system never gets
 * printed.  400000000 was calibrated on i386 and MISSED that deadline on
 * x86_64 (slower per-iteration under TCG): a real GUI deadlock rebooted the
 * machine with `SPINLOCK STUCK` never appearing, which is exactly the "it just
 * froze and restarted" experience this detector exists to explain.  Measured
 * 2026-08-02: at 20000000 the report lands well inside the watchdog window on
 * x86_64, and a false positive costs only a log line (we keep spinning either
 * way — the detector never changes behaviour). */
#define SPIN_STUCK_THRESHOLD 20000000UL

extern void serial_putchar(char c);
extern void serial_write(const char* s);

static void spin_serial_hex(uintptr_t v) {
    serial_write("0x");
    for (int i = (int)(sizeof(v) * 2) - 1; i >= 0; i--) {
        int nyb = (int)((v >> (i * 4)) & 0xF);
        serial_putchar(nyb < 10 ? (char)('0' + nyb) : (char)('a' + nyb - 10));
    }
}

static inline void spin_acquire(spinlock_t* l, void* caller) {
    unsigned long spins = 0;
    while (!atomic_cmpxchg(&l->locked, 0, 1)) {
        hal_cpu_pause();
        if (++spins >= SPIN_STUCK_THRESHOLD) {
            serial_write("\n!! SPINLOCK STUCK lock=");
            spin_serial_hex((uintptr_t)l);
            serial_write(" caller=");
            spin_serial_hex((uintptr_t)caller);
            serial_write(" — probable deadlock\n");
            /* §M47 — also record it.  The serial line above is lock-free and
             * always works; the record is what a GUI/file/network sink can
             * surface to the user later. */
            crash_report(CRASH_DEADLOCK, -1, "spinlock", (uintptr_t)caller,
                         (uintptr_t)l, 0, "spinlock spun past the sanity limit");
            spins = 0;                       /* re-arm: keep reporting while stuck */
        }
    }
}

uint32_t spin_lock_irqsave(spinlock_t* l) {
    /* IRQs off on THIS CPU first — prevents the timer IRQ from yanking
     * the CPU away from us mid-lock and creating a hold time so long
     * that another CPU's acquire times out.  On UP this is also the
     * only protection we need; on SMP the cmpxchg below picks up
     * cross-CPU contention. */
    uint32_t fl = hal_intr_save();
    spin_acquire(l, __builtin_return_address(0));
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
    spin_acquire(l, __builtin_return_address(0));
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
