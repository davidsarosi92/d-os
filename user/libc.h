/* =============================================================================
 * libc.h — d-os in-tree minimal user C library (M25 stage 7).
 *
 * Just enough of a freestanding libc to link a real compiled-C user program
 * against the kernel's syscall ABI (syscall.h numbers) — string helpers, an
 * mmap-backed malloc, puts/printf, and thin syscall wrappers.  Not hosted-
 * conformant; a teaching libc.
 * ============================================================================= */

#ifndef DOS_ULIBC_H
#define DOS_ULIBC_H

typedef __SIZE_TYPE__ size_t;

/* Thin syscall wrappers (numbers match kernel/includes/syscall.h). */
long  write(int fd, const void* buf, size_t n);
long  read (int fd, void* buf, size_t n);
int   open (const char* path, int flags);
int   close(int fd);
void  exit (int code);
void* mmap (size_t len, int fd);            /* fd<0 = anonymous */
int   getpid(void);

/* String / memory. */
size_t strlen(const char* s);
void*  memset(void* d, int c, size_t n);
void*  memcpy(void* d, const void* s, size_t n);

/* Heap — bump allocator over mmap (no free). */
void* malloc(size_t n);

/* Formatted output. */
int puts  (const char* s);
int printf(const char* fmt, ...);

#endif
