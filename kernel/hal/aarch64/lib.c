/* =============================================================================
 * lib.c — freestanding mem* primitives for the AArch64 port (M21 Phase C).
 *
 * The kernel has no libc, yet gcc is permitted to emit calls to memset /
 * memcpy / memmove / memcmp even under -ffreestanding (they are "always
 * available" library routines).  On the x86 builds the compiler happens to
 * inline these for the code we have; on AArch64 it emits real calls (e.g.
 * task.c's struct-zeroing), so the kernel must provide the symbols itself.
 *
 * The file is built with -fno-tree-loop-distribute-patterns (see the Makefile
 * aarch64 CFLAGS) so gcc does NOT "optimise" these obvious loops back into a
 * call to memset/memcpy — which would make them infinitely recursive.
 * ============================================================================= */

#include <stddef.h>
#include <stdint.h>

void* memset(void* dst, int val, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

/* -----------------------------------------------------------------------------
 * __getauxval — freestanding stub.
 *
 * libgcc's LSE-atomics init constructor (lse-init.o, pulled in via the atomic
 * helpers) calls glibc's __getauxval(AT_HWCAP) at startup to decide whether
 * the CPU has the Large System Extensions.  There is no glibc here, so we
 * provide a stub that reports "no aux vector" (0) — the init code then falls
 * back to the load-exclusive/store-exclusive atomics, which are correct on
 * every ARMv8 core.
 * --------------------------------------------------------------------------- */
unsigned long __getauxval(unsigned long type) {
    (void)type;
    return 0;
}
