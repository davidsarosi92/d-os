/* =============================================================================
 * lock.h — kernel locking primitives + preempt-count.
 *
 * d-os is uniprocessor today, but the API here is shaped for the SMP
 * future (PLAN §SMP).  Three pieces:
 *
 *   1. struct spinlock      — opaque lock, opaque flags.
 *   2. spin_lock_irqsave    — disables IRQs, "acquires" the lock, returns
 *                             a token to feed to spin_unlock_irqrestore.
 *   3. preempt_disable/enable — bracket short hot paths that must not be
 *                             preempted by the timer-driven scheduler.
 *
 * Why the API is shaped like this:
 *
 *   - On UP, what makes a critical section atomic against the timer IRQ
 *     is `cli` — there is no other CPU racing us.  We package that into
 *     spin_lock_irqsave + spin_unlock_irqrestore so that the call sites
 *     read like Linux's irqsave pattern and will keep working unchanged
 *     when a real test-and-set spinlock implementation lands for SMP.
 *
 *   - preempt_disable is the cheaper option when we just need to ban the
 *     timer-driven scheduler from picking another task (e.g. during an
 *     allocator merge that briefly leaves the freelist inconsistent), but
 *     don't actually need IRQs masked off.  IRQs still arrive and run their
 *     handler; only the deferred `schedule_check()` at IRQ exit gets skipped.
 *
 * Memory model: writes inside a spin_lock_irqsave block are visible to any
 * other code once the matching unlock-restore has run, since on UP the only
 * other reader is an IRQ context (impossible while IF=0) or non-preempt code
 * (we synchronously released the lock first).
 * ============================================================================= */

#ifndef LOCK_H
#define LOCK_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * spinlock_t — opaque to callers.  Today the `locked` field is purely
 * diagnostic (we'd notice a recursive acquire) and only `cli/sti` does
 * the real work.  When SMP arrives, `locked` becomes a test-and-set
 * primitive; the API does not change.
 * --------------------------------------------------------------------------- */
typedef struct spinlock {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock_init(spinlock_t* l) { l->locked = 0; }

/* Disable IRQs (saving the prior EFLAGS.IF) and "acquire" the lock.  The
 * returned token is opaque — pass it verbatim to spin_unlock_irqrestore.
 *
 * Today the only state that matters is the saved IF, since UP cannot
 * race itself once IRQs are off.  On SMP this also spins on `l->locked`. */
uint32_t spin_lock_irqsave(spinlock_t* l);

/* Release the lock and restore IF to what it was before the matching
 * spin_lock_irqsave.  `flags` must be the value that call returned. */
void spin_unlock_irqrestore(spinlock_t* l, uint32_t flags);

/* ---------------------------------------------------------------------------
 * preempt_disable / preempt_enable
 *
 * Cheap "do not let the timer scheduler pick another task right now"
 * counter.  Reentrant: increments nest, only the outermost matching
 * enable allows preemption again.  See schedule_check() in task.h for the
 * consumer side — it observes preempt_count and refuses to context-switch
 * if it's non-zero.
 *
 * Use this around short kernel critical sections that don't actually
 * need IRQs masked (e.g. you're walking a tree and an IRQ taking a snapshot
 * is fine, but a context switch mid-walk would corrupt your state).
 *
 * Today `preempt_count` is a single global because we're UP.  On SMP it
 * moves to a per-CPU slot. */
void preempt_disable(void);
void preempt_enable(void);
int  preempt_count(void);

#endif
