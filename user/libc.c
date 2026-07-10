/* =============================================================================
 * libc.c — d-os in-tree minimal user libc (M25 stage 7).  See libc.h.
 *
 * Every OS interaction goes through `int 0x80` with the shared syscall
 * numbers.  i386 ABI: eax = number, ebx/ecx/edx = args, return in eax.
 * ============================================================================= */

#include "libc.h"
#include <stdarg.h>

/* Mirror of the kernel syscall numbers (kernel/includes/syscall.h). */
#define SYS_EXIT   1
#define SYS_WRITE  2
#define SYS_READ   3
#define SYS_OPEN   4
#define SYS_CLOSE  5
#define SYS_MMAP   7

static long syscall3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

long  write(int fd, const void* buf, size_t n) { return syscall3(SYS_WRITE, fd, (long)buf, (long)n); }
long  read (int fd, void* buf, size_t n)       { return syscall3(SYS_READ,  fd, (long)buf, (long)n); }
int   open (const char* path, int flags)       { return (int)syscall3(SYS_OPEN, (long)path, flags, 0); }
int   close(int fd)                            { return (int)syscall3(SYS_CLOSE, fd, 0, 0); }
void  exit (int code)                          { syscall3(SYS_EXIT, code, 0, 0); for (;;) {} }
void* mmap (size_t len, int fd)                { long r = syscall3(SYS_MMAP, (long)len, fd, 0);
                                                 return (r <= 0) ? (void*)0 : (void*)r; }

size_t strlen(const char* s) { size_t i = 0; while (s[i]) i++; return i; }

void* memset(void* d, int c, size_t n) {
    unsigned char* p = (unsigned char*)d;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}
void* memcpy(void* d, const void* s, size_t n) {
    unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}

/* Bump allocator: one anonymous mmap arena, handed out in aligned chunks.
 * No free — fine for short-lived test clients. */
static char*  heap_ptr  = 0;
static size_t heap_left = 0;
void* malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;                 /* 16-byte align */
    if (n > heap_left) {
        size_t grab = n > 65536 ? n : 65536;    /* 64 KiB arenas */
        char* a = (char*)mmap(grab, -1);
        if (!a) return 0;
        heap_ptr = a; heap_left = grab;
    }
    void* r = heap_ptr;
    heap_ptr += n; heap_left -= n;
    return r;
}

/* ---- formatted output ----------------------------------------------------- */

static void put_uint(unsigned long v) {
    char b[24]; int i = 0;
    if (v == 0) { write(1, "0", 1); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) write(1, &b[i], 1);
}
static void put_int(long v) {
    if (v < 0) { write(1, "-", 1); put_uint((unsigned long)(-v)); }
    else       put_uint((unsigned long)v);
}
static void put_hex(unsigned long v) {
    static const char hx[] = "0123456789abcdef";
    char b[16]; int i = 0;
    if (v == 0) { write(1, "0", 1); return; }
    while (v) { b[i++] = hx[v & 0xF]; v >>= 4; }
    while (i--) write(1, &b[i], 1);
}

int puts(const char* s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }

int printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { write(1, fmt, 1); continue; }
        fmt++;
        switch (*fmt) {
            case 's': { const char* s = va_arg(ap, const char*); write(1, s, strlen(s)); break; }
            case 'd': case 'i': put_int(va_arg(ap, int)); break;
            case 'u': put_uint(va_arg(ap, unsigned int)); break;
            case 'x': put_hex(va_arg(ap, unsigned int)); break;
            case 'c': { char c = (char)va_arg(ap, int); write(1, &c, 1); break; }
            case '%': write(1, "%", 1); break;
            default:  write(1, fmt, 1); break;
        }
    }
    va_end(ap);
    return 0;
}
