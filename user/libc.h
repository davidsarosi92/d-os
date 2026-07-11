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
int   fork  (void);                         /* → child pid in parent, 0 in child */
int   waitpid(int pid, int* status);        /* block for a child; → reaped pid */
int   execv (const char* path, char* const argv[]);  /* replace image; no return on success */
int   pipe  (int fds[2]);                   /* fds[0]=read, fds[1]=write */
int   dup2  (int oldfd, int newfd);         /* redirect a descriptor */

/* Signals (M34). */
typedef void (*sighandler_t)(int);
sighandler_t signal(int sig, sighandler_t handler);
int   kill  (int pid, int sig);
int   raise (int sig);
#define SIGINT  2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17

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
