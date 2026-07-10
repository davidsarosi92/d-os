/* =============================================================================
 * vectors.S — AArch64 EL1 exception vector table + trampolines (M21).
 *
 * The AArch64 vector table is the ARM analogue of the x86 IDT, but its shape
 * is fixed by the architecture: a single 2 KiB-aligned table of 16 entries,
 * each entry a 128-byte (0x80) slot of code (NOT a descriptor).  The 16 slots
 * are four groups of four {Synchronous, IRQ, FIQ, SError}:
 *
 *   0x000  Current EL, SP_EL0     (we never run on SP0 → unused)
 *   0x200  Current EL, SP_ELx     (kernel-mode faults + IRQs land here)
 *   0x400  Lower EL, AArch64      (EL0 syscalls/faults — future userspace)
 *   0x600  Lower EL, AArch32      (we never run AArch32 → unused)
 *
 * Each slot saves a trapframe (x0..x30 + ELR_EL1 + SPSR_EL1) on the current
 * stack, calls the C dispatcher with (type, *trapframe), then restores and
 * `eret`s.  This mirrors the x86 isr_stubs.s → C isr dispatcher split.
 *
 * VBAR_EL1 is pointed at `vector_table` from exceptions_init() (exceptions.c).
 *
 * Trapframe layout (grows down; 272 bytes, 16-byte aligned):
 *   [sp+0x00 .. +0xF0]  x0..x30 (x30 paired with a padding slot)
 *   [sp+0x100]          ELR_EL1  (faulting/return PC)
 *   [sp+0x108]          SPSR_EL1 (saved processor state)
 * ============================================================================= */

/* ---- trapframe save/restore -------------------------------------------------
 * 17 register-pairs = 272 bytes.  stp/ldp move two 8-byte regs at once. */

.macro SAVE_TRAPFRAME
    sub     sp,  sp,  #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    stp     x30, xzr, [sp, #16 * 15]
    mrs     x9,  elr_el1
    mrs     x10, spsr_el1
    stp     x9,  x10, [sp, #16 * 16]
.endm

.macro RESTORE_TRAPFRAME
    ldp     x9,  x10, [sp, #16 * 16]
    msr     elr_el1,  x9
    msr     spsr_el1, x10
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp,  sp,  #272
.endm

/* Exception "type" codes handed to the C dispatcher.  Kept in sync with the
 * enum in exceptions.c. */
.equ EXC_SYNC,   0
.equ EXC_IRQ,    1
.equ EXC_FIQ,    2
.equ EXC_SERROR, 3

/* One 0x80 vector slot that branches to a named trampoline. */
.macro VECTOR label
    .balign 0x80
    b       \label
.endm

/* ---- the table -------------------------------------------------------------- */

.section .text
.balign 0x800
.global vector_table
vector_table:
    /* Group 0: Current EL with SP_EL0 (unused — we always run EL1h/SP_ELx). */
    VECTOR el1_sp0_sync
    VECTOR el1_sp0_irq
    VECTOR el1_sp0_fiq
    VECTOR el1_sp0_serror

    /* Group 1: Current EL with SP_ELx (kernel faults + IRQs). */
    VECTOR el1_sync
    VECTOR el1_irq
    VECTOR el1_fiq
    VECTOR el1_serror

    /* Group 2: Lower EL using AArch64 (EL0 — future userspace). */
    VECTOR el0_sync
    VECTOR el0_irq
    VECTOR el0_fiq
    VECTOR el0_serror

    /* Group 3: Lower EL using AArch32 (never — we do not run AArch32). */
    VECTOR el0_32_sync
    VECTOR el0_32_irq
    VECTOR el0_32_fiq
    VECTOR el0_32_serror

/* ---- trampolines -----------------------------------------------------------
 * Each sets x0 = type, then falls into the shared save/dispatch/restore. */

el1_sp0_sync:
el1_sync:
el0_sync:
el0_32_sync:
    SAVE_TRAPFRAME
    mov     x0, #EXC_SYNC
    b       exc_dispatch

el1_sp0_irq:
el1_irq:
el0_irq:
el0_32_irq:
    SAVE_TRAPFRAME
    mov     x0, #EXC_IRQ
    b       exc_dispatch

el1_sp0_fiq:
el1_fiq:
el0_fiq:
el0_32_fiq:
    SAVE_TRAPFRAME
    mov     x0, #EXC_FIQ
    b       exc_dispatch

el1_sp0_serror:
el1_serror:
el0_serror:
el0_32_serror:
    SAVE_TRAPFRAME
    mov     x0, #EXC_SERROR
    b       exc_dispatch

/* Shared tail: x0 already holds the type, x1 = pointer to the trapframe. */
exc_dispatch:
    mov     x1, sp
    bl      aarch64_exception_handler
    RESTORE_TRAPFRAME
    eret
