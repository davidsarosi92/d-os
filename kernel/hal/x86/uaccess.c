/* =============================================================================
 * uaccess.c (i386) — fault-safe user-memory primitives.
 *
 * Each user-touching instruction is labelled (1:) and paired with a fixup label
 * (2:) in an `.ex_table` entry.  If the CPU faults on that instruction — the
 * range was unmapped between the caller's check and the copy, a COW page was
 * revoked, a concurrent thread munmap'd it — the #PF handler (idt.c) looks the
 * EIP up, rewrites it to the fixup, and iret's there; the fixup sets the error
 * flag and the routine returns -1.  No panic, no freeze, just -EFAULT.
 *
 * Deliberately byte-at-a-time: correctness and simplicity over throughput (the
 * buffers here are syscall arguments, not bulk I/O).  A word-wise fast path with
 * its own table entries is a later optimisation.
 * ============================================================================= */

#include "uaccess.h"

/* Read one byte from user memory.  *err is set to 1 if the access faulted. */
static inline uint8_t u_get_byte(uintptr_t uaddr, int* err) {
    uint8_t v = 0;
    __asm__ volatile (
        "1: movb (%[src]), %[val]\n"
        "   jmp 3f\n"
        "2: movl $1, %[e]\n"
        "3:\n"
        ".pushsection ex_table,\"a\"\n"
        ".align 4\n"
        ".long 1b, 2b\n"
        ".popsection\n"
        : [val] "=&q" (v), [e] "+m" (*err)
        : [src] "r" (uaddr)
        : "memory");
    return v;
}

/* Write one byte to user memory.  *err is set to 1 if the access faulted. */
static inline void u_put_byte(uintptr_t uaddr, uint8_t v, int* err) {
    __asm__ volatile (
        "1: movb %[val], (%[dst])\n"
        "   jmp 3f\n"
        "2: movl $1, %[e]\n"
        "3:\n"
        ".pushsection ex_table,\"a\"\n"
        ".align 4\n"
        ".long 1b, 2b\n"
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
