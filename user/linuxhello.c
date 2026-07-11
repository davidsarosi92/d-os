/* =============================================================================
 * linuxhello.c — a freestanding i386 program that uses the LINUX syscall ABI
 * (write = 4, exit = 1) via `int 0x80` — the exact shape an unmodified
 * Linux/musl binary has.  It links NO d-os libc / crt0 (entry = _start); the
 * kernel runs it under the Linux personality (task->linux_abi), routing its
 * syscalls through kernel/hal/x86/linux_abi.c.  This proves the compat layer
 * end-to-end before dropping in real (vendored, pristine) musl.
 * ============================================================================= */

static long lsys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

void _start(void) {
    static const char m[] =
        "hello from an unmodified-style Linux-ABI program (write=4, exit=1)\n";
    long len = (long)sizeof(m) - 1;
    lsys3(4, 1, (long)m, len);      /* Linux write(fd=1, m, len) */
    lsys3(1, 0, 0, 0);              /* Linux exit(0)             */
    for (;;) { }                    /* unreachable */
}
