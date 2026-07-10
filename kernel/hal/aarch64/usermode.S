/* =============================================================================
 * usermode.S — drop AArch64 from EL1 to EL0 and back (M21 Phase L).
 *
 * The ARM analogue of the x86 usermode.s ring-0 → ring-3 trick:
 *
 *   aarch64_enter_user(entry, user_sp):
 *     - stashes the current kernel SP + return address (LR) in globals,
 *     - sets SP_EL0 = user stack, ELR_EL1 = entry, SPSR_EL1 = 0 (EL0t, all
 *       DAIF clear so IRQs preempt EL0),
 *     - `eret` — the CPU is now at EL0 running `entry`.
 *
 *   The EL0 program runs until it issues `svc #0` with x8 = SYS_EXIT (1).
 *   syscall.c handles that by calling aarch64_user_exit(), which restores the
 *   stashed kernel SP + LR and `ret`s — so from the caller's point of view
 *   aarch64_enter_user ran an EL0 program and returned normally.  This bypasses
 *   the normal RESTORE_TRAPFRAME/eret-back-to-EL0 path, exactly like the x86
 *   SYS_EXIT teleport.
 *
 * The kernel stack used by the SVC/IRQ handlers is SP_EL1, which the CPU
 * selects automatically when taking an exception from EL0 — so, unlike x86
 * (which needs TSS.esp0), no separate syscall stack plumbing is required; the
 * task's own kernel stack (SP_EL1 at eret time) is used.
 * ============================================================================= */

.section .data
.balign 8
uk_sp:  .quad 0                 /* kernel SP to resume on SYS_EXIT   */
uk_lr:  .quad 0                 /* kernel return address (LR)        */

.section .text

/* void aarch64_enter_user(uint64_t entry_va, uint64_t user_sp); */
.global aarch64_enter_user
aarch64_enter_user:
    mov     x9, sp
    adrp    x10, uk_sp
    add     x10, x10, :lo12:uk_sp
    str     x9, [x10]                   /* save caller SP  */
    adrp    x11, uk_lr
    add     x11, x11, :lo12:uk_lr
    str     x30, [x11]                  /* save caller LR  */

    msr     sp_el0,  x1                 /* user stack      */
    msr     elr_el1, x0                 /* user entry PC   */
    mov     x2, #0                      /* SPSR = EL0t, DAIF clear */
    msr     spsr_el1, x2
    isb
    eret

/* void aarch64_user_exit(void);  — teleport back to aarch64_enter_user's caller.
 * Runs at EL1 inside the SVC handler; abandons the exception frames by restoring
 * the stashed SP, then `ret`s to the stashed LR. */
.global aarch64_user_exit
aarch64_user_exit:
    adrp    x10, uk_sp
    add     x10, x10, :lo12:uk_sp
    ldr     x9, [x10]
    adrp    x11, uk_lr
    add     x11, x11, :lo12:uk_lr
    ldr     x30, [x11]
    mov     sp, x9
    ret

/* -----------------------------------------------------------------------------
 * user_stub — a tiny position-independent EL0 program (copied into a user page
 * by the syscall.c self-test).  It prints a message via SYS_PRINT (x8=0, x0=str)
 * then exits via SYS_EXIT (x8=1).  PC-relative `adr` makes it runnable at any
 * user VA the loader maps it to.  Syscall ABI: x8 = number, x0..x5 = args.
 * --------------------------------------------------------------------------- */
.global user_stub_start
.global user_stub_end
user_stub_start:
    adr     x0, user_stub_msg           /* x0 = message (PC-relative) */
    mov     x8, #0                      /* SYS_PRINT */
    svc     #0
    mov     x8, #1                      /* SYS_EXIT  */
    svc     #0
0:  b       0b                          /* unreached — SYS_EXIT teleports away */
user_stub_msg:
    .asciz  "hello from EL0 (aarch64 userspace)!\n"
    .balign 4
user_stub_end:
