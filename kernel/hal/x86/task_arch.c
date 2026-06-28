/* =============================================================================
 * task_arch.c — x86 implementation of `hal_task_init_stack` + the
 * brand-new-task trampoline that goes with it.
 *
 * These two pieces are inherently x86 ABI:
 *
 *   - The stack layout we lay down for the first `context_switch` must
 *     match what `context_switch` (switch.s) pops: 4 callee-saved
 *     registers in the order ebx, esi, edi, ebp, then a return address.
 *     That's System V i386 calling convention.
 *
 *   - `task_trampoline` reads the entry pointer out of `ebx` — a
 *     register choice that's only meaningful on x86.  We picked ebx
 *     because it's the first callee-saved register `context_switch`
 *     pops; the spawn-side prefills it with the user's `entry`.
 *
 *   - The trampoline `sti`s before calling `entry`, which is how
 *     brand-new tasks become preemptible.  Other arches will do the
 *     equivalent (cpsie i, etc.) in their own trampolines.
 *
 * Living here (rather than in `kernel/core/task.c`) keeps the
 * core scheduler arch-independent: it just calls
 * `hal_task_init_stack` and `context_switch` and never touches
 * registers by name.
 * ============================================================================= */

#include "hal_api.h"
#include "task.h"
#include <stdint.h>

/* On task exit the trampoline must end the task cleanly.  task_exit
 * lives in task.c (portable) and is `noreturn`. */
extern void task_exit(void) __attribute__((noreturn));

/* ---------------------------------------------------------------------------
 * task_trampoline — the address `context_switch` first `ret`s to on a
 * brand-new task.  Mirrors the schedule()-side IRQ-enable that an
 * established task gets for free when its own schedule() call returns.
 *
 * Layout reminder (set up by hal_task_init_stack):
 *
 *   esp on entry  →  ebx = entry pointer  (we pop into a local var below)
 *                    esi = 0
 *                    edi = 0
 *                    ebp = 0
 *                    ret addr = task_trampoline (we're here)
 *
 * By the time we run, context_switch has already popped the 4 callee-
 * saved regs.  We `mov %%ebx, ...` to extract the entry pointer that
 * was sitting in ebx, sti to permit preemption, and call entry().
 * --------------------------------------------------------------------------- */
static void task_trampoline(void) {
    /* Lock-handoff finish (M18).  The schedule() that switched into
     * us acquired the runqueue lock and disabled IRQs; an established
     * task release-pairs that on its own schedule()'s exit, but we've
     * never been through schedule() before, so we have to perform
     * the matching unlock + sti by hand. */
    task_finish_first_switch();

    /* New tasks always start with interrupts enabled — without this
     * `sti` the timer would never fire on us, and a never-yielding
     * entry would soft-lock the system. */
    __asm__ volatile ("sti");

    void (*entry)(void);
    __asm__ volatile ("mov %%ebx, %0" : "=r"(entry));
    entry();
    task_exit();
}

/* ---------------------------------------------------------------------------
 * hal_task_init_stack — write the canonical "first-switch" stack frame
 * downward from `stack_top` and return the value to store in
 * `task->esp`.
 *
 * The caller (task_spawn in task.c) owns the stack memory and the
 * `struct task`; we only fill in the stack bytes.
 * --------------------------------------------------------------------------- */
uintptr_t hal_task_init_stack(void* stack_top, void (*entry)(void)) {
    uint32_t* sp = (uint32_t*)stack_top;
    *--sp = 0xDEADBEEFu;                          /* guard if entry returns despite task_exit */
    *--sp = (uint32_t)(uintptr_t)task_trampoline; /* return addr for ret */
    *--sp = 0;                                    /* ebp */
    *--sp = 0;                                    /* edi */
    *--sp = 0;                                    /* esi */
    *--sp = (uint32_t)(uintptr_t)entry;           /* ebx → carries entry to trampoline */
    return (uintptr_t)sp;
}
