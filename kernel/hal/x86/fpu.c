/* =============================================================================
 * fpu.c — per-task FPU / SSE register-file save & restore (i386).
 *
 * Why this exists: `context_switch` swaps the INTEGER context only (the
 * callee-saved GPRs and the stack pointer).  The x87/MMX/XMM register file is
 * a completely separate piece of CPU state, so without an explicit save/restore
 * every task inherits whatever the previously-running task left in it.  Two
 * ring-3 programs doing floating-point work then silently corrupt each other's
 * arithmetic — no fault, no log line, just wrong numbers.  On SMP it gets worse:
 * a task migrated to another core resumes on THAT core's register file, so the
 * corruption becomes timing-dependent and unreproducible.
 *
 * FXSAVE / FXRSTOR is the whole mechanism.  It moves the x87 state, the MMX
 * aliases and (when CR4.OSFXSR is set) the XMM registers to/from a 512-byte,
 * 16-BYTE-ALIGNED memory image.  Two rules the callers must not violate:
 *
 *   1. ALIGNMENT.  FXSAVE/FXRSTOR #GP on a misaligned address.  struct task is
 *      kcalloc'd and we do not want the scheduler to depend on the allocator's
 *      alignment, so the blob handed to us is oversized and we align INSIDE it.
 *      That keeps the arch rule here, where it belongs.
 *
 *   2. A ZERO-FILLED IMAGE IS NOT VALID.  FXRSTOR of an all-zero blob loads
 *      MXCSR = 0, which means every SIMD exception is UNMASKED — the first
 *      real FP operation afterwards raises #XF.  hal_fpu_init_state therefore
 *      writes the architectural reset values (FCW = 0x037F, MXCSR = 0x1F80)
 *      rather than trusting kcalloc's zeroing.
 *
 * Eager, not lazy: we save/restore on every switch instead of using CR0.TS and
 * a #NM handler.  At a 100 Hz tick the cost is noise, and lazy FPU switching is
 * a well-known source of subtle bugs (and, on SMP, of cross-core state leaks).
 *
 * i386 note: this kernel does not enable CR4.OSFXSR, so ring-3 code here is
 * x87-only and FXSAVE stores the x87/MMX half.  The code is identical to the
 * x86_64 twin on purpose — if SSE is ever enabled for 32-bit userland, the XMM
 * half starts being covered with no change required.
 * ============================================================================= */

#include "hal_api.h"
#include <stdint.h>

/* CPUID.01H:EDX bit 24 — FXSR (FXSAVE/FXRSTOR supported).  Every CPU that can
 * run this kernel has it, but probing costs one CPUID at boot and turns a #UD
 * on exotic hardware into a graceful "no FP context switching". */
static int fxsr_supported(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1));
    cached = (edx & (1u << 24)) ? 1 : 0;
    return cached;
}

/* The 16-byte-aligned FXSAVE area inside the task's oversized blob. */
static inline uint8_t* fxarea(void* blob) {
    return (uint8_t*)(((uintptr_t)blob + 15u) & ~(uintptr_t)15u);
}

void hal_fpu_init_state(void* blob) {
    uint8_t* a = fxarea(blob);
    for (int i = 0; i < 512; i++) a[i] = 0;
    /* FCW (offset 0) — x87 control word: all exceptions masked, extended
     * precision, round-to-nearest.  This is the value FNINIT leaves. */
    a[0] = 0x7F; a[1] = 0x03;
    /* MXCSR (offset 24) — all SIMD exceptions MASKED, round-to-nearest.
     * Getting this wrong is the classic "#XF right after the first restore". */
    a[24] = 0x80; a[25] = 0x1F; a[26] = 0x00; a[27] = 0x00;
    /* FTW (offset 4) stays 0 = every x87 register empty, which is what a fresh
     * task should see. */
}

void hal_fpu_save(void* blob) {
    if (!fxsr_supported()) return;
    __asm__ volatile ("fxsave (%0)" :: "r"(fxarea(blob)) : "memory");
}

void hal_fpu_restore(void* blob) {
    if (!fxsr_supported()) return;
    __asm__ volatile ("fxrstor (%0)" :: "r"(fxarea(blob)) : "memory");
}

/* ---------------------------------------------------------------------------
 * Self-test hooks (see hal_api.h).
 *
 * i386 keeps the pattern in the x87 stack top: `fldl` pushes it once and `fstl`
 * reads it back WITHOUT popping, so st(0) stays occupied across every yield in
 * between — which is exactly the state a context switch has to preserve.  (XMM
 * is not used here because this kernel never sets CR4.OSFXSR, so an SSE
 * instruction in ring 0 would #UD.)
 * --------------------------------------------------------------------------- */
int hal_fpu_present(void) { return fxsr_supported(); }

void hal_fpu_test_stamp(uint64_t v) {
    double d;
    __builtin_memcpy(&d, &v, sizeof d);
    __asm__ volatile ("fldl %0" :: "m"(d));
}

uint64_t hal_fpu_test_read(void) {
    double d; uint64_t v;
    __asm__ volatile ("fstl %0" : "=m"(d));   /* store, do NOT pop */
    __builtin_memcpy(&v, &d, sizeof v);
    return v;
}
