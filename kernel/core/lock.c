/* =============================================================================
 * lock.c — UP implementation of the lock.h API.
 *
 * The "spinlock" is a stub on UP: there's no other CPU to contend with,
 * so the lock body is just `cli` (atomicity vs. our own IRQ handlers) and
 * the unlock restores EFLAGS.IF.  The `locked` field on struct spinlock
 * exists only so we can spot a recursive acquire from the same context —
 * a real bug we'd otherwise discover much later.
 *
 * preempt_count is a plain global for now.  When SMP lands it becomes a
 * per-CPU slot keyed off the current APIC ID; the API in lock.h doesn't
 * change.
 * ============================================================================= */

#include "lock.h"
#include "hal_api.h"
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
    /* M17: arch-specific irq-save behind hal_intr_save.  On x86 this
     * still expands to one pushf/pop/cli pair; on ARM it'll be a
     * cpsid + flag read. */
    uint32_t fl = hal_intr_save();

    /* On UP, with IRQs masked, the only way `l->locked` could already be 1
     * is if we recursively acquired the same lock from the same context.
     * That's a programming error — flag it loudly. */
    if (l->locked) {
        kprintf("spin_lock_irqsave: recursive acquire on %p\n", (void*)l);
    }
    l->locked = 1;
    return fl;
}

void spin_unlock_irqrestore(spinlock_t* l, uint32_t flags) {
    l->locked = 0;
    /* Restore IRQ state as it was before the matching irqsave. */
    hal_intr_restore(flags);
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
