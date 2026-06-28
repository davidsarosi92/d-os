/* =============================================================================
 * task_arch.c — x86_64 implementation of `hal_task_init_stack` + the
 * brand-new-task trampoline that goes with it.
 *
 * Same shape as the i386 version, with two structural changes:
 *
 *   1. Callee-saved set is rbx, rbp, r12, r13, r14, r15 (6 registers,
 *      vs. i386's 4: ebx, esi, edi, ebp).  Switch.s pops them in the
 *      order r15, r14, r13, r12, rbx, rbp — so our brand-new-task
 *      stack writes them in that reverse order at the bottom.
 *
 *   2. The trampoline reads the entry pointer out of `rbx` — same
 *      pattern as i386 with `ebx`.  rbx is the first callee-saved
 *      register `context_switch` pops; the spawn-side prefills it
 *      with the user's `entry`.  As long as no C statement between
 *      function-entry and the inline `mov %%rbx` clobbers it, the
 *      asm reads the right value.  `task_finish_first_switch` and
 *      `sti` are both leaves (extern function + no-clobber asm) so
 *      rbx survives.
 *
 * Stack alignment: the System V x86_64 ABI requires rsp to be 8 mod
 * 16 at function entry (i.e. 0 mod 16 just before the `call` that
 * gets us there).  context_switch ends with `ret`, which adds 8 to
 * rsp — so just before that `ret`, rsp must be 0 mod 16.  After 6
 * pops (48 bytes), rsp moves from initial-stack-bottom to
 * initial-stack-bottom+48.  We build the stack at a 16-aligned
 * starting point and write 8 quadwords (64 bytes); after 6 pops
 * rsp is at -64+48 = -16 relative to start (i.e. 0 mod 16), then
 * ret pops 8 more (to -8 / 8 mod 16) and jumps.  That puts rsp at
 * the required 8-mod-16 state for the trampoline.
 *
 * Living here (rather than in `kernel/core/task.c`) keeps the
 * core scheduler arch-independent — same rationale as the i386
 * version.
 * ============================================================================= */

#include "hal_api.h"
#include "task.h"
#include <stdint.h>

extern void task_exit(void) __attribute__((noreturn));

/* ---------------------------------------------------------------------------
 * task_trampoline — same logic as i386; only the inline-asm register
 * name changes (rbx vs ebx).
 * --------------------------------------------------------------------------- */
static void task_trampoline(void) {
    /* Pair the schedule() that switched into us: that schedule()
     * acquired the runqueue lock and disabled IRQs.  An established
     * task release-pairs on its own schedule()'s exit; we've never
     * been through schedule() before, so the unlock + sti happens
     * here. */
    task_finish_first_switch();

    /* New tasks start with interrupts enabled — without this the
     * timer would never fire and a never-yielding entry would soft-
     * lock the system. */
    __asm__ volatile ("sti");

    void (*entry)(void);
    __asm__ volatile ("mov %%rbx, %0" : "=r"(entry));
    entry();
    task_exit();
}

/* ---------------------------------------------------------------------------
 * hal_task_init_stack — write the canonical first-switch frame.
 *
 * Layout (high to low, i.e. order we *--sp them):
 *
 *   top  → guard (0xDEADBEEFDEADBEEF)
 *          return addr = task_trampoline      ← ret jumps here
 *          rbp value   = 0
 *          rbx value   = entry                ← trampoline reads this
 *          r12 value   = 0
 *          r13 value   = 0
 *          r14 value   = 0
 *  rsp →   r15 value   = 0                    ← first pop in switch.s
 *
 * Returned uintptr_t is the rsp the scheduler should store in
 * `task->esp` (the field name kept from i386 — see task.h).
 * --------------------------------------------------------------------------- */
uintptr_t hal_task_init_stack(void* stack_top, void (*entry)(void)) {
    uint64_t* sp = (uint64_t*)stack_top;
    *--sp = 0xDEADBEEFDEADBEEFull;                  /* guard */
    *--sp = (uint64_t)(uintptr_t)task_trampoline;   /* ret addr */
    *--sp = 0;                                       /* rbp */
    *--sp = (uint64_t)(uintptr_t)entry;             /* rbx → entry */
    *--sp = 0;                                       /* r12 */
    *--sp = 0;                                       /* r13 */
    *--sp = 0;                                       /* r14 */
    *--sp = 0;                                       /* r15 */
    return (uintptr_t)sp;
}
