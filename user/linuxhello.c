/* =============================================================================
 * linuxhello.c — a freestanding i386 program that uses the LINUX syscall ABI
 * (write = 4, exit = 1) via `int 0x80` — the exact shape an unmodified
 * Linux/musl binary has.  It links NO d-os libc / crt0; the kernel runs it
 * under the Linux personality (task->linux_abi), routing its syscalls through
 * kernel/hal/x86/linux_abi.c.  This proves the compat layer end-to-end before
 * dropping in real (vendored, pristine) musl.
 *
 * It exercises the three things musl needs at startup:
 *   1. Linux write/exit                        (the syscall path)
 *   2. set_thread_area + %gs TLS               (the #1 startup blocker)
 *   3. a real auxv (AT_PAGESZ + AT_RANDOM)      (musl reads it in __init_libc)
 * ============================================================================= */

static long lsys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/* Linux i386 struct user_desc, exactly as musl passes it to set_thread_area. */
struct user_desc { unsigned int entry_number, base_addr, limit, flags; };

/* A one-word TLS block, read back through %gs:0. */
static unsigned int tls_word = 0xC0FFEE00;

/* SysV auxv types we check. */
#define AT_NULL    0
#define AT_PAGESZ  6
#define AT_RANDOM  25

/* The initial stack pointer, captured by the asm _start below BEFORE the C
 * prologue can move %esp — it points exactly at argc (SysV entry contract). */
unsigned long g_sp;

static void emit(const char* s) {
    long n = 0; while (s[n]) n++;
    lsys3(4, 1, (long)s, n);            /* Linux write(1, s, len) */
}

void c_main(void) {
    emit("hello from an unmodified-style Linux-ABI program (write=4, exit=1)\n");

    /* --- (2) set_thread_area + %gs TLS ------------------------------------ */
    struct user_desc u = { (unsigned int)-1, (unsigned int)&tls_word, 0xFFFFF, 0x51 };
    long tls_r = lsys3(243 /*set_thread_area*/, (long)&u, 0, 0);
    unsigned short sel = (unsigned short)((u.entry_number << 3) | 3);
    __asm__ volatile ("mov %0, %%gs" :: "r"(sel));
    unsigned int got;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(got));
    emit((tls_r == 0 && got == 0xC0FFEE00)
         ? "linux-abi: set_thread_area TLS via %gs:0 OK\n"
         : "linux-abi: set_thread_area TLS FAIL\n");

    /* --- (3) walk the auxv, the way musl's __init_libc does --------------- */
    unsigned long* sp = (unsigned long*)g_sp;
    long argc = (long)sp[0];
    unsigned long* p = &sp[1];             /* argv[0]                         */
    p += argc; p++;                        /* skip argv[] + its NULL          */
    while (*p) p++;                         /* skip envp[]                     */
    p++;                                   /* skip envp NULL -> auxv           */

    unsigned long pagesz = 0, at_random = 0;
    for (; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PAGESZ) pagesz = p[1];
        else if (p[0] == AT_RANDOM) at_random = p[1];
    }
    /* AT_RANDOM must point at 16 bytes; require them not-all-zero. */
    int rnd_ok = 0;
    if (at_random) {
        const unsigned char* r = (const unsigned char*)at_random;
        for (int i = 0; i < 16; i++) if (r[i]) { rnd_ok = 1; break; }
    }
    emit((pagesz == 4096 && rnd_ok)
         ? "linux-abi: auxv AT_PAGESZ=4096 + AT_RANDOM OK\n"
         : "linux-abi: auxv FAIL\n");

    lsys3(1, 0, 0, 0);                     /* Linux exit(0) */
    for (;;) { }
}

/* asm _start: grab the SysV entry %esp (points at argc) into g_sp, then call
 * the C body.  No crt0, no prologue in front of the capture. */
__asm__(
    ".globl _start\n"
    "_start:\n"
    "  mov %esp, g_sp\n"
    "  call c_main\n"
    "  hlt\n"
);
