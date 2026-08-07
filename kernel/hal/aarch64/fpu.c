/* =============================================================================
 * fpu.c — per-task FP/SIMD state (aarch64).
 *
 * Deliberately a no-op TODAY, and this comment is the justification.
 *
 * On AArch64 access to the FP/SIMD unit is gated by CPACR_EL1.FPEN.  Its reset
 * value is 0b00 = "trap FP/SIMD at BOTH EL0 and EL1", and nothing in this port
 * ever writes CPACR_EL1 — so the vector registers are not merely unused, they
 * are UNREACHABLE.  Both sides are built to match: the kernel and the in-tree
 * user libc compile with -mgeneral-regs-only (see the Makefile), so no FP or
 * NEON instruction is emitted for either.
 *
 * There is therefore no FP/SIMD state that a context switch could lose: a task
 * cannot put anything in those registers in the first place.  Saving 528 bytes
 * of unreachable register file on every switch would be pure cost, and — worse
 * — the save itself would have to touch the FP unit, which would TRAP.  Doing
 * nothing is the correct implementation, not a shortcut.
 *
 * WHEN THIS MUST BE FILLED IN: the moment anything on this arch wants real FP
 * in userland (a musl/AArch64 port, any of the §M38+ support libraries — all of
 * which use floating point).  That change has two halves and BOTH are required:
 *
 *   1. Enable the unit: set CPACR_EL1.FPEN = 0b11 during EL1 bring-up, per CPU
 *      (it is a per-CPU system register — the same trap that made x86_64 `-smp`
 *      collapse, see DOCS §8).
 *   2. Fill in the functions below: save/restore Q0..Q31 (32 × 16 B) plus FPCR
 *      and FPSR — 528 bytes, which is why HAL_FPU_STATE_SIZE is sized for it
 *      already.  `stp q0, q1, [x0, #0]` … and `mrs x1, fpcr` / `mrs x1, fpsr`.
 *
 * Enabling (1) without (2) is the dangerous combination: FP would start working
 * and silently corrupt across task switches, exactly the bug the x86 twins of
 * this file exist to prevent.
 * ============================================================================= */

/* =============================================================================
 * FILLED IN (A2, 2026-08-07) — and the file above called the shot exactly.
 *
 * The trigger was the one it predicted: an aarch64 musl binary.  musl's
 * `memset` opens with `dup v0.16b, w1`, a NEON instruction, so the very first
 * string operation of libc startup trapped.  The symptom was misleading —
 * an EL0 synchronous exception with FAR_EL1 = 0, which reads exactly like a
 * null dereference and is nothing of the kind; the ESR class was "access to
 * SIMD/FP trapped".  Disassembling the faulting address settled it in one step
 * where guessing at a null pointer would have cost an evening.
 *
 * Both halves are here, because the file was right that one without the other
 * is worse than neither:
 *   1. hal_fpu_enable_this_cpu() sets CPACR_EL1.FPEN = 0b11, called from BOTH
 *      the BSP and the AP bring-up paths.
 *   2. save/restore of Q0..Q31 + FPCR + FPSR below.
 * ============================================================================= */

#include "hal_api.h"
#include <stdint.h>

/* Q0..Q31 (32 x 16 B) + FPCR + FPSR = 528 bytes, aligned to 16 inside the
 * oversized blob the core hands us (HAL_FPU_STATE_SIZE is 576 for exactly
 * this reason — the same arrangement x86_64 uses for its FXSAVE image). */
static uint8_t* fpu_area(void* blob) {
    return (uint8_t*)(((uintptr_t)blob + 15u) & ~(uintptr_t)15u);
}

void hal_fpu_enable_this_cpu(void) {
    /* CPACR_EL1.FPEN (bits 21:20) = 0b11 — do not trap FP/SIMD at EL0 or EL1.
     * A per-CPU system register: every core sets its own, or the second core
     * runs with the reset value and every FP instruction there traps while the
     * first core is fine.  That asymmetry is the nastiest possible version of
     * this bug, so both bring-up paths call this. */
    uint64_t v;
    __asm__ volatile ("mrs %0, cpacr_el1" : "=r"(v));
    v |= (3ULL << 20);
    __asm__ volatile ("msr cpacr_el1, %0\nisb" :: "r"(v) : "memory");
}

void hal_fpu_init_state(void* blob) {
    /* A zeroed image is a valid starting state: all vector registers zero,
     * FPCR/FPSR zero = round-to-nearest with every exception masked.  Unlike
     * x86's FXSAVE (where an all-zero MXCSR unmasks every SIMD exception and
     * faults on the first instruction), zero is genuinely safe here. */
    uint8_t* p = fpu_area(blob);
    for (int i = 0; i < 528; i++) p[i] = 0;
}

void hal_fpu_save(void* blob) {
    uint8_t* p = fpu_area(blob);
    __asm__ volatile (
        "stp q0,  q1,  [%0, #16 * 0]\n"
        "stp q2,  q3,  [%0, #16 * 2]\n"
        "stp q4,  q5,  [%0, #16 * 4]\n"
        "stp q6,  q7,  [%0, #16 * 6]\n"
        "stp q8,  q9,  [%0, #16 * 8]\n"
        "stp q10, q11, [%0, #16 * 10]\n"
        "stp q12, q13, [%0, #16 * 12]\n"
        "stp q14, q15, [%0, #16 * 14]\n"
        "stp q16, q17, [%0, #16 * 16]\n"
        "stp q18, q19, [%0, #16 * 18]\n"
        "stp q20, q21, [%0, #16 * 20]\n"
        "stp q22, q23, [%0, #16 * 22]\n"
        "stp q24, q25, [%0, #16 * 24]\n"
        "stp q26, q27, [%0, #16 * 26]\n"
        "stp q28, q29, [%0, #16 * 28]\n"
        "stp q30, q31, [%0, #16 * 30]\n"
        "mrs x9, fpcr\n"
        "mrs x10, fpsr\n"
        /* Two `str`, not one `stp`: stp's scaled 7-bit immediate tops out at
         * 504 for 64-bit registers, and the control words sit at 512. */
        "str x9,  [%0, #512]\n"
        "str x10, [%0, #520]\n"
        :: "r"(p) : "x9", "x10", "memory");
}

void hal_fpu_restore(void* blob) {
    uint8_t* p = fpu_area(blob);
    __asm__ volatile (
        "ldp q0,  q1,  [%0, #16 * 0]\n"
        "ldp q2,  q3,  [%0, #16 * 2]\n"
        "ldp q4,  q5,  [%0, #16 * 4]\n"
        "ldp q6,  q7,  [%0, #16 * 6]\n"
        "ldp q8,  q9,  [%0, #16 * 8]\n"
        "ldp q10, q11, [%0, #16 * 10]\n"
        "ldp q12, q13, [%0, #16 * 12]\n"
        "ldp q14, q15, [%0, #16 * 14]\n"
        "ldp q16, q17, [%0, #16 * 16]\n"
        "ldp q18, q19, [%0, #16 * 18]\n"
        "ldp q20, q21, [%0, #16 * 20]\n"
        "ldp q22, q23, [%0, #16 * 22]\n"
        "ldp q24, q25, [%0, #16 * 24]\n"
        "ldp q26, q27, [%0, #16 * 26]\n"
        "ldp q28, q29, [%0, #16 * 28]\n"
        "ldp q30, q31, [%0, #16 * 30]\n"
        "ldr x9,  [%0, #512]\n"
        "ldr x10, [%0, #520]\n"
        "msr fpcr, x9\n"
        "msr fpsr, x10\n"
        :: "r"(p) : "x9", "x10", "memory");
}

/* Self-test hooks — see hal_api.h.  Reporting "no FP unit" makes `fputest`
 * print SKIP here, which is the truthful answer: with CPACR_EL1.FPEN at its
 * reset value the vector registers cannot be reached at all, so there is
 * nothing for a context switch to preserve or lose. */
int      hal_fpu_present(void)          { return 0; }
void     hal_fpu_test_stamp(uint64_t v) { (void)v; }
uint64_t hal_fpu_test_read(void)        { return 0; }
