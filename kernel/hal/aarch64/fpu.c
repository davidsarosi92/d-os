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

#include "hal_api.h"

void hal_fpu_init_state(void* blob) { (void)blob; }
void hal_fpu_save(void* blob)       { (void)blob; }
void hal_fpu_restore(void* blob)    { (void)blob; }

/* Self-test hooks — see hal_api.h.  Reporting "no FP unit" makes `fputest`
 * print SKIP here, which is the truthful answer: with CPACR_EL1.FPEN at its
 * reset value the vector registers cannot be reached at all, so there is
 * nothing for a context switch to preserve or lose. */
int      hal_fpu_present(void)          { return 0; }
void     hal_fpu_test_stamp(uint64_t v) { (void)v; }
uint64_t hal_fpu_test_read(void)        { return 0; }
