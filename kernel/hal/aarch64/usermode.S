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
 *   syscall.c handles that by calling user_excursion_teleport(), which restores the
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

.section .text

/* void aarch64_enter_user(uint64_t entry_va, uint64_t user_sp, uint64_t* resume);
 * #11 — the resume point goes where the caller says (the task's exc_resume),
 * not into globals: two excursions at once used to overwrite each other's. */
.global aarch64_enter_user
aarch64_enter_user:
    mov     x9, sp
    str     x9,  [x2]                   /* resume[0] = caller SP */
    str     x30, [x2, #8]               /* resume[1] = caller LR */

    msr     sp_el0,  x1                 /* user stack      */
    msr     elr_el1, x0                 /* user entry PC   */
    mov     x2, #0                      /* SPSR = EL0t, DAIF clear */
    msr     spsr_el1, x2
    isb
    eret

/* void aarch64_user_exit_to(uint64_t sp, uint64_t lr);  — teleport back to
 * aarch64_enter_user's caller.  Runs at EL1 inside the SVC handler; abandons
 * the exception frames by installing the saved SP, then `ret`s to the saved LR. */
.global aarch64_user_exit_to
aarch64_user_exit_to:
    mov     sp, x0
    mov     x30, x1
    ret

/* -----------------------------------------------------------------------------
 * void enter_user_mode(uint64_t entry_va, uint64_t user_sp);  — Tier B, ONE-WAY.
 *
 * The concurrent-user-process EL0 entry: unlike aarch64_enter_user it saves NO
 * kernel resume context (the excursion's resume point is per task now, #11 — this path needs none,
 * concurrency).  The user-task bootstrap never returns; the task re-enters the
 * kernel only via svc/IRQ (on its own SP_EL1 = its kernel stack, tracked by
 * context_switch), and SYS_EXIT ends the task via task_exit().
 *
 *   x0 = entry_va, x1 = user_sp  (AAPCS64)
 * --------------------------------------------------------------------------- */
.global enter_user_mode
enter_user_mode:
    msr     sp_el0,  x1                 /* user stack           */
    msr     elr_el1, x0                 /* user entry PC        */
    mov     x2, #0                      /* SPSR = EL0t, DAIF clear (IRQs on) */
    msr     spsr_el1, x2
    isb
    eret                                /* → EL0, never returns here */

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

/* -----------------------------------------------------------------------------
 * void enter_user_mode_regs(struct user_regs* r);  — ONE-WAY, never returns.
 *
 * Resume EL0 with a complete register set.  fork()'s child uses this to appear
 * at exactly the instruction after the parent's `svc`, with x0 = 0.
 *
 * Layout (kernel/includes/usermode.h, aarch64 branch):
 *   [x0 + 0x00 .. 0xF0]  x0..x30
 *   [x0 + 0xF8]          SP_EL0
 *   [x0 + 0x100]         PC     → ELR_EL1
 *   [x0 + 0x108]         PSTATE → SPSR_EL1
 *
 * The pointer arrives in x0, which is itself one of the registers to restore,
 * so it is moved to x9 and x9 is restored last (through itself).  Restore the
 * register holding the frame pointer any earlier and every load after it reads
 * from whatever the child's value happened to be.
 * --------------------------------------------------------------------------- */
.global enter_user_mode_regs
enter_user_mode_regs:
    mov     x9, x0                      /* frame pointer, out of the way */

    ldr     x10, [x9, #0xF8]            /* SP_EL0  */
    msr     sp_el0, x10
    ldr     x10, [x9, #0x100]           /* PC      */
    msr     elr_el1, x10
    ldr     x10, [x9, #0x108]           /* PSTATE  */
    msr     spsr_el1, x10

    ldp     x1,  x2,  [x9, #8 * 1]
    ldp     x3,  x4,  [x9, #8 * 3]
    ldp     x5,  x6,  [x9, #8 * 5]
    ldp     x7,  x8,  [x9, #8 * 7]
    ldp     x11, x12, [x9, #8 * 11]
    ldp     x13, x14, [x9, #8 * 13]
    ldp     x15, x16, [x9, #8 * 15]
    ldp     x17, x18, [x9, #8 * 17]
    ldp     x19, x20, [x9, #8 * 19]
    ldp     x21, x22, [x9, #8 * 21]
    ldp     x23, x24, [x9, #8 * 23]
    ldp     x25, x26, [x9, #8 * 25]
    ldp     x27, x28, [x9, #8 * 27]
    ldp     x29, x30, [x9, #8 * 29]
    ldr     x10, [x9, #8 * 10]
    ldr     x0,  [x9, #8 * 0]
    /* x9 LAST: it is still the frame pointer, so it has to read its own saved
     * value THROUGH itself.  `ldr x9, [x9, ...]` is exactly that — reads, then
     * overwrites the base.  Restoring it any earlier loses the pointer and
     * every remaining load reads garbage. */
    ldr     x9,  [x9, #8 * 9]
    isb
    eret                                /* → EL0; never returns here     */
