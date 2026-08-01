/* =============================================================================
 * uaccess.c (x86_64) — fault-safe user-memory primitives.
 *
 * Identical mechanism to the i386 twin (see kernel/hal/x86/uaccess.c for the
 * full rationale); the only differences are 64-bit table entries (.quad) and
 * the RIP the handler patches.
 * ============================================================================= */

#include "uaccess.h"

static inline uint8_t u_get_byte(uintptr_t uaddr, int* err) {
    uint8_t v = 0;
    __asm__ volatile (
        "1: movb (%[src]), %[val]\n"
        "   jmp 3f\n"
        "2: movl $1, %[e]\n"
        "3:\n"
        ".pushsection ex_table,\"a\"\n"
        ".align 8\n"
        ".quad 1b, 2b\n"
        ".popsection\n"
        : [val] "=&q" (v), [e] "+m" (*err)
        : [src] "r" (uaddr)
        : "memory");
    return v;
}

static inline void u_put_byte(uintptr_t uaddr, uint8_t v, int* err) {
    __asm__ volatile (
        "1: movb %[val], (%[dst])\n"
        "   jmp 3f\n"
        "2: movl $1, %[e]\n"
        "3:\n"
        ".pushsection ex_table,\"a\"\n"
        ".align 8\n"
        ".quad 1b, 2b\n"
        ".popsection\n"
        : [e] "+m" (*err)
        : [dst] "r" (uaddr), [val] "q" (v)
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
