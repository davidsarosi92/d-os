/* =============================================================================
 * task_arch.c — AArch64 implementation of hal_task_init_stack + the
 * brand-new-task trampoline (M21 Phase C).
 *
 * Same shape as the x86 versions; only the register set + calling
 * convention change:
 *
 *   - Callee-saved set pushed by context_switch (switch.S) is x19..x30
 *     (12 registers).  A brand-new task's synthetic stack writes them in
 *     the exact layout switch.S pops, with x30 = task_trampoline (so the
 *     final `ret` lands there) and x19 = the task entry pointer (so the
 *     trampoline can recover it — the ARM analogue of i386 ebx / x86_64
 *     rbx).
 *
 *   - The trampoline reads x19 into a C local as its VERY FIRST action,
 *     before any call that could reuse x19 as a scratch/callee-saved
 *     temporary.  Once captured into `entry`, x19 is free.
 *
 * SP alignment: AArch64 requires SP to be 16-byte aligned at every
 * instruction that uses it as a base (and the AAPCS requires it at public
 * interfaces).  We write exactly 12 doublewords (96 bytes) from a
 * 16-aligned stack_top, so the resulting SP stays 16-aligned, and every
 * ldp in switch.S sees an aligned SP.
 *
 * Living here (not in kernel/core/task.c) keeps the core scheduler
 * arch-independent — same rationale as the x86 ports.
 * ============================================================================= */

#include "hal_api.h"
#include "task.h"
#include <stdint.h>

extern void task_exit(void) __attribute__((noreturn));

/* Slot indices into the synthetic frame (see switch.S layout comment). */
enum {
    SLOT_X29 = 0, SLOT_X30,
    SLOT_X27,     SLOT_X28,
    SLOT_X25,     SLOT_X26,
    SLOT_X23,     SLOT_X24,
    SLOT_X21,     SLOT_X22,
    SLOT_X19,     SLOT_X20,
    SLOT_COUNT
};

static void task_trampoline(void) {
    /* Recover the entry pointer from x19 FIRST — before task_finish_first_
     * switch() (a call) can reuse x19. */
    void (*entry)(void);
    __asm__ volatile ("mov %0, x19" : "=r"(entry));

    /* Pair the schedule() that switched into us: it acquired the runqueue
     * lock + disabled IRQs.  A brand-new task has never been through
     * schedule()'s exit, so it releases here. */
    task_finish_first_switch();

    /* New tasks start with IRQs enabled (DAIFclr #2 = unmask IRQ) — the
     * ARM analogue of the x86 `sti`; without it the timer never preempts a
     * never-yielding entry and the system soft-locks. */
    __asm__ volatile ("msr daifclr, #2" ::: "memory");

    entry();
    task_exit();
}

uintptr_t hal_task_init_stack(void* stack_top, void (*entry)(void)) {
    uint64_t* sp = (uint64_t*)stack_top;
    sp -= SLOT_COUNT;                       /* reserve the 12-slot frame     */

    for (int i = 0; i < SLOT_COUNT; i++) sp[i] = 0;
    sp[SLOT_X30] = (uint64_t)(uintptr_t)task_trampoline;  /* ret target      */
    sp[SLOT_X19] = (uint64_t)(uintptr_t)entry;            /* trampoline reads */

    return (uintptr_t)sp;
}
