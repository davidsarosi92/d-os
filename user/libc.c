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
#define SYS_GETPID 13
#define SYS_FORK   14
#define SYS_WAITPID 15
#define SYS_EXECVE 16
#define SYS_PIPE   17
#define SYS_DUP2   18
#define SYS_KILL   19
#define SYS_SIGACTION 20
#define SYS_SOCKET 22
#define SYS_CONNECT 23
#define SYS_SENDTO 24
#define SYS_RECVFROM 25
#define SYS_BIND   26

/* One syscall path, three arches.  x86 (i386 + x86_64) trap via `int 0x80`
 * (rax/eax = number, rbx/ecx/edx = args); aarch64 traps via `svc #0` with the
 * kernel's x8 = number, x0..x2 = args ABI.  The kernel dispatchers read exactly
 * these registers. */
static long syscall3(long n, long a, long b, long c) {
    long r;
#if defined(__aarch64__)
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2)
                      : "memory");
    r = x0;
#else   /* i386 + x86_64: int 0x80, num in (r/e)ax, args in (r/e)bx/cx/dx */
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
#endif
    return r;
}

/* Five-argument syscall (sendto/recvfrom).  Same trap paths as syscall3, with
 * two more argument registers (x86: esi/edi; aarch64: x3/x4). */
static long syscall5(long n, long a, long b, long c, long d, long e) {
    long r;
#if defined(__aarch64__)
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                      : "memory");
    r = x0;
#else
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                      : "memory");
#endif
    return r;
}

long  write(int fd, const void* buf, size_t n) { return syscall3(SYS_WRITE, fd, (long)buf, (long)n); }
long  read (int fd, void* buf, size_t n)       { return syscall3(SYS_READ,  fd, (long)buf, (long)n); }
int   open (const char* path, int flags)       { return (int)syscall3(SYS_OPEN, (long)path, flags, 0); }
int   close(int fd)                            { return (int)syscall3(SYS_CLOSE, fd, 0, 0); }
void  exit (int code)                          { syscall3(SYS_EXIT, code, 0, 0); for (;;) {} }
void* mmap (size_t len, int fd)                { long r = syscall3(SYS_MMAP, (long)len, fd, 0);
                                                 return (r <= 0) ? (void*)0 : (void*)r; }
int   getpid(void)                             { return (int)syscall3(SYS_GETPID, 0, 0, 0); }
int   fork  (void)                             { return (int)syscall3(SYS_FORK, 0, 0, 0); }
int   waitpid(int pid, int* status)            { return (int)syscall3(SYS_WAITPID, pid, (long)status, 0); }
int   execv (const char* path, char* const argv[]) { return (int)syscall3(SYS_EXECVE, (long)path, (long)argv, 0); }
int   pipe  (int fds[2])                       { return (int)syscall3(SYS_PIPE, (long)fds, 0, 0); }
int   dup2  (int oldfd, int newfd)             { return (int)syscall3(SYS_DUP2, oldfd, newfd, 0); }

extern void __sig_trampoline(void);            /* crt0.s */
sighandler_t signal(int sig, sighandler_t h) {
    return (sighandler_t)syscall3(SYS_SIGACTION, sig, (long)h, (long)__sig_trampoline);
}
int   kill  (int pid, int sig)                 { return (int)syscall3(SYS_KILL, pid, sig, 0); }
int   raise (int sig)                          { return kill(getpid(), sig); }

int   socket(int domain, int type, int proto)  { return (int)syscall3(SYS_SOCKET, domain, type, proto); }
int   bind_port(int fd, int port)              { return (int)syscall3(SYS_BIND, fd, port, 0); }
int   connect_ip(int fd, unsigned ip, int port) { return (int)syscall3(SYS_CONNECT, fd, (long)ip, port); }
long  sendto(int fd, const void* buf, size_t n, unsigned ip, int port) {
    return syscall5(SYS_SENDTO, fd, (long)buf, (long)n, (long)ip, port);
}
long  recvfrom(int fd, void* buf, size_t n, unsigned* ip, int* port) {
    return syscall5(SYS_RECVFROM, fd, (long)buf, (long)n, (long)ip, (long)port);
}

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
