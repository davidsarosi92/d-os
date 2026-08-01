/* =============================================================================
 * fpu.c — per-task FPU / SSE register-file save & restore (x86_64).
 *
 * See the i386 twin (kernel/hal/x86/fpu.c) for the full rationale; the short
 * version is that `context_switch` swaps the INTEGER context only, so without
 * this every task inherits the previous task's x87/XMM registers.
 *
 * On x86_64 this is not a corner case: SSE2 is BASELINE in the AMD64 ABI, so
 * the compiler emits XMM instructions for floating point AND for ordinary
 * memory copies.  Every musl binary — NetSurf included — is therefore an
 * FP-using task, and a second one running concurrently (or the same one
 * migrating between cores on SMP) would silently read back another task's XMM
 * registers.  That was the known gap left open when ring-3 was first made to
 * work on an AP; this closes it.
 *
 * FXSAVE/FXRSTOR needs a 512-byte 16-byte-aligned image, and a zero-filled
 * image is NOT valid (MXCSR = 0 means every SIMD exception unmasked → #XF on
 * the first FP op).  Both rules are handled here so the scheduler can stay
 * arch-agnostic: it hands over an oversized opaque blob and we align inside it.
 *
 * FXSAVE (not XSAVE): the kernel itself is built -mno-sse and we do not enable
 * AVX for userland, so the legacy 512-byte area covers all architectural state
 * a task can actually touch.  Moving to XSAVE/XRSTOR is the follow-up whenever
 * AVX gets enabled — at which point the state size becomes CPUID-dependent and
 * HAL_FPU_STATE_SIZE has to grow with it.
 * ============================================================================= */

#include "hal_api.h"
#include <stdint.h>

/* CPUID.01H:EDX bit 24 — FXSR.  Always set on x86_64 (the instruction set
 * requires it), probed anyway so the code reads the same as the i386 twin. */
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

static inline uint8_t* fxarea(void* blob) {
    return (uint8_t*)(((uintptr_t)blob + 15u) & ~(uintptr_t)15u);
}

void hal_fpu_init_state(void* blob) {
    uint8_t* a = fxarea(blob);
    for (int i = 0; i < 512; i++) a[i] = 0;
    a[0] = 0x7F; a[1] = 0x03;                              /* FCW  = 0x037F */
    a[24] = 0x80; a[25] = 0x1F; a[26] = 0x00; a[27] = 0x00; /* MXCSR = 0x1F80 */
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
 * x86_64 uses xmm0 directly — that is the register file real user code lives
 * in here (SSE2 is baseline), so the test exercises exactly the state that was
 * being lost.  The kernel is built -mno-sse, but inline asm still assembles
 * fine and CR4.OSFXSR is set on every CPU, so ring 0 may touch XMM.
 * --------------------------------------------------------------------------- */
int hal_fpu_present(void) { return fxsr_supported(); }

/* No "xmm0" clobber: the kernel is compiled -mno-sse, so GCC refuses to accept
 * an XMM register in a clobber list (it does not model them for this target) —
 * and for the same reason it never allocates one, so there is nothing to tell
 * it about. */
void hal_fpu_test_stamp(uint64_t v) {
    __asm__ volatile ("movq %0, %%xmm0" :: "m"(v));
}

uint64_t hal_fpu_test_read(void) {
    uint64_t v;
    __asm__ volatile ("movq %%xmm0, %0" : "=m"(v));
    return v;
}
