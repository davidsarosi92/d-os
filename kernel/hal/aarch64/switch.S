/* =============================================================================
 * switch.S — kernel-mode context switch (AArch64) (M21 Phase C).
 *
 * void context_switch(uintptr_t* save_sp_to, uintptr_t new_sp);
 *
 *   AAPCS64: x0 = save_sp_to (uintptr_t*), x1 = new_sp (uintptr_t).
 *   Callee-saved GPRs that must be preserved across a call: x19..x28,
 *   x29 (FP), x30 (LR).  (We build with -mgeneral-regs-only, so there is
 *   no FP/SIMD callee-saved state to spill — the x86_64 -mno-sse analogue.)
 *
 * We push the 12 callee-saved registers onto the current kernel stack,
 * store the resulting SP into *save_sp_to, load SP from new_sp, pop the
 * 12 registers back, and `ret` — which branches to the restored x30 (LR).
 * For an established task that LR is the return address inside schedule();
 * for a brand-new task it is `task_trampoline` (see task_arch.c), whose
 * matching x19 slot carries the task entry pointer.
 *
 * Push order (pre-index, SP decreasing): x19/x20 first … x29/x30 last, so
 * at the saved SP the layout (low→high address) is:
 *   [sp+0x00] x29   [sp+0x08] x30
 *   [sp+0x10] x27   [sp+0x18] x28
 *   [sp+0x20] x25   [sp+0x28] x26
 *   [sp+0x30] x23   [sp+0x38] x24
 *   [sp+0x40] x21   [sp+0x48] x22
 *   [sp+0x50] x19   [sp+0x58] x20
 * The pop order (post-index) mirrors it exactly.  hal_task_init_stack in
 * task_arch.c writes a synthetic frame in this same layout.
 * ============================================================================= */

.section .text
.global context_switch

context_switch:
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!

    mov     x2, sp
    str     x2, [x0]              /* *save_sp_to = current SP           */
    mov     sp, x1               /* switch to the new task's stack      */

    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16
    ret                          /* branch to restored x30 (LR)         */
