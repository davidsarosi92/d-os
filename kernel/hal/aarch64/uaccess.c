/* =============================================================================
 * uaccess.c (aarch64) — fault-safe user-memory primitives.
 *
 * Same mechanism as the x86 twins (see kernel/hal/x86/uaccess.c): the ldrb/strb
 * that touches EL0 memory is registered in `.ex_table`, and the EL1 data-abort
 * handler (exceptions.c) rewrites ELR_EL1 to the fixup instead of halting.
 *
 * Note EL1 can read EL0 pages directly here (no PAN/uaccess-window dance — d-os
 * does not set PSTATE.PAN), so the access itself is a plain load/store; the
 * table is purely about surviving a fault.
 * ============================================================================= */

#include "uaccess.h"

static inline uint8_t u_get_byte(uintptr_t uaddr, int* err) {
    uint8_t v = 0;
    __asm__ volatile (
        "1: ldrb %w[val], [%[src]]\n"
        "   b 3f\n"
        "2: mov %w[e], #1\n"
        "3:\n"
        ".pushsection ex_table,\"a\"\n"
        ".align 3\n"
        ".quad 1b, 2b\n"
        ".popsection\n"
        : [val] "=&r" (v), [e] "+r" (*err)
        : [src] "r" (uaddr)
        : "memory");
    return v;
}

static inline void u_put_byte(uintptr_t uaddr, uint8_t v, int* err) {
    __asm__ volatile (
        "1: strb %w[val], [%[dst]]\n"
        "   b 3f\n"
        "2: mov %w[e], #1\n"
        "3:\n"
        ".pushsection ex_table,\"a\"\n"
        ".align 3\n"
        ".quad 1b, 2b\n"
        ".popsection\n"
        : [e] "+r" (*err)
        : [dst] "r" (uaddr), [val] "r" (v)
        : "memory");
}

int uaccess_copy_in(void* dst, uintptr_t user_src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    int err = 0;
    for (size_t i = 0; i < n; i++) {
        d[i] = u_get_byte(user_src + i, &err);
        if (err) return -1;
    }
    return 0;
}

int uaccess_copy_out(uintptr_t user_dst, const void* src, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    int err = 0;
    for (size_t i = 0; i < n; i++) {
        u_put_byte(user_dst + i, s[i], &err);
        if (err) return -1;
    }
    return 0;
}

long uaccess_str_in(char* dst, uintptr_t user_src, size_t max) {
    if (max == 0) return -1;
    int err = 0;
    for (size_t i = 0; i + 1 < max; i++) {
        char c = (char)u_get_byte(user_src + i, &err);
        if (err) return -1;
        dst[i] = c;
        if (c == 0) return (long)i;
    }
    dst[max - 1] = 0;
    return (long)(max - 1);
}
