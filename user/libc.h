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

/* Threads (M35). */
int   thread_create(int (*fn)(void*), void* arg);   /* → tid */
int   thread_join(int tid);                         /* waitpid the thread */
int   futex(int* uaddr, int op, int val);
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

/* Thread-local storage (M35).  set_tls() points %gs at a per-thread block;
 * tls_load4 reads the int at %gs:4 (a demo accessor — the compiler's __thread
 * ABI layering lands with the libc port).  */
void set_tls(void* tp);
static inline int tls_load4(void) { int v; __asm__ volatile ("movl %%gs:4, %0" : "=r"(v)); return v; }

/* POSIX syscall breadth (M36).  These structs match the kernel's kstat /
 * kutsname / ktimespec (syscall.h). */
struct stat    { unsigned size; int type; int mode; };   /* type 0=file,1=dir,2=dev */
struct utsname { char sysname[65], nodename[65], release[65], version[65], machine[65]; };
struct timespec { unsigned sec; unsigned nsec; };
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
int  stat (const char* path, struct stat* out);
int  fstat(int fd, struct stat* out);
long getdents(int fd, void* buf, unsigned cap);
int  uname(struct utsname* out);
int  clock_gettime(int which, struct timespec* out);
int  nanosleep_ms(unsigned ms);
extern int errno;
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

/* Network sockets (M24).  Addresses are host-order IPv4 + port ints (no
 * struct sockaddr yet — a teaching-ABI simplification). */
#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
int   socket(int domain, int type, int proto);
int   bind_port(int fd, int port);          /* bind a local UDP port */
int   connect_ip(int fd, unsigned ip, int port);  /* TCP connect (host-order ip) */
long  sendto(int fd, const void* buf, size_t n, unsigned ip, int port);
long  recvfrom(int fd, void* buf, size_t n, unsigned* ip, int* port);
#define IPV4(a,b,c,d) (((unsigned)(a)<<24)|((unsigned)(b)<<16)|((unsigned)(c)<<8)|(unsigned)(d))

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
