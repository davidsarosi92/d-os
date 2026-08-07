/* =============================================================================
 * syscall.h — minimal syscall ABI.
 *
 * Calling convention — arch-specific registers, shared numbers:
 *   i386 / x86_64:  number → EAX/RAX, args → EBX/RBX.., trigger `int 0x80`,
 *                   return → EAX/RAX (on iret-back).
 *   aarch64:        number → x8, args → x0..x5, trigger `svc #0`,
 *                   return → x0 (on eret-back).
 * Each arch has its own dispatcher (kernel/hal/<arch>/syscall.c) that reads its
 * trapframe; only the numbers below are shared.
 *
 * Syscall numbers (kept tiny on purpose; this is a teaching set):
 *   0  SYS_PRINT  EBX = const char* — print null-terminated string to console
 *   1  SYS_EXIT  — return to the wrap caller in kernel mode (M6 plumbing)
 *
 * The `print` syscall reads the string from a user-mode address; the
 * kernel walks it directly (we still have the identity map for the
 * kernel address range and the user code's pages are USER-mapped, so
 * supervisor reads work).
 * ============================================================================= */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stddef.h>
#include <stdint.h>

/* Syscall numbers (shared across arches; each arch reads its own trapframe). */
#define SYS_PRINT   0       /* legacy M6: arg0 = char* → console (kept)        */
#define SYS_EXIT    1       /* return to the enter_user_mode_wrap caller       */
#define SYS_WRITE   2       /* (fd, buf, n)  → bytes written / -1              */
#define SYS_READ    3       /* (fd, buf, n)  → bytes read / -1                 */
#define SYS_OPEN    4       /* (path, flags) → new fd / -1                     */
#define SYS_CLOSE   5       /* (fd)          → 0 / -1                          */
#define SYS_LSEEK   6       /* (fd, off, whence) → new pos / -1                */
#define SYS_MMAP    7       /* (len, fd) → user VA / -1  (fd<0 = anonymous)    */
#define SYS_MEMFD   8       /* (size)    → new fd for a shared-memory object   */
#define SYS_SOCKETPAIR 9    /* (int fds[2]) → 0 / -1  (connected unix pair)    */
#define SYS_SEND   10       /* (fd, buf, n, passfd) → bytes / -1  (passfd<0=none) */
#define SYS_RECV   11       /* (fd, buf, n, int* passfd_out) → bytes / -1      */
#define SYS_POLL   12       /* (struct pollfd*, nfds, timeout) → #ready        */
#define SYS_GETPID 13       /* () → calling task's pid (Tier B)                */
#define SYS_FORK   14       /* () → child pid in parent, 0 in child (M34)      */
#define SYS_WAITPID 15      /* (pid, int* status) → reaped pid / -1  (M34)     */
#define SYS_EXECVE 16       /* (path, argv) → replaces image; -1 on failure    */
#define SYS_PIPE   17       /* (int fds[2]) → 0; fds[0]=read end, fds[1]=write */
#define SYS_DUP2   18       /* (oldfd, newfd) → newfd / -1                     */
#define SYS_KILL   19       /* (pid, sig) → 0 / -1  (M34)                      */
#define SYS_SIGACTION 20    /* (sig, handler, restorer) → old handler          */
#define SYS_SIGRETURN 21    /* () — restore context after a signal handler     */
#define SYS_SOCKET 22       /* (domain, type, proto) → fd  (M24 socket API)    */
#define SYS_CONNECT 23      /* (fd, ip, port) → 0    (TCP — next slice)        */
#define SYS_SENDTO 24       /* (fd, buf, n, ip, port) → bytes / -1  (UDP)      */
#define SYS_RECVFROM 25     /* (fd, buf, n, u32* ip, int* port) → bytes / -1   */
#define SYS_BIND   26       /* (fd, port) → 0 / -1                             */
#define SYS_CLONE  27       /* (entry, stack) → tid  (M35 thread)              */
#define SYS_FUTEX  28       /* (uaddr, op, val) → 0 / -1  (M35)                */
#define SYS_SET_TLS 29      /* (base) → %gs selector  (M35 thread-local storage) */
/* M36 — POSIX syscall breadth (the surface a real libc sits on). */
#define SYS_STAT   30       /* (path, struct kstat*) → 0 / -1                  */
#define SYS_FSTAT  31       /* (fd, struct kstat*)   → 0 / -1                  */
#define SYS_GETDENTS 32     /* (fd, buf, cap) → bytes of packed dir records    */
#define SYS_UNAME  33       /* (struct kutsname*) → 0                          */
#define SYS_CLOCK_GETTIME 34/* (which, struct ktimespec*) → 0                  */
#define SYS_NANOSLEEP 35    /* (ms) → 0  (millisecond sleep, simplified)       */
#define SYS_GETRANDOM 36    /* (buf, n, flags) → bytes  (§M39 CSPRNG)          */

/* M36 shared structs (kernel + libc agree on the layout). */
struct kstat {
    uint32_t size;
    int      type;          /* 0=file, 1=dir, 2=device (matches inode_type)    */
    int      mode;          /* rwx bits placeholder (0644/0755)                */
};
struct kutsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65];
};
struct ktimespec { uint32_t sec; uint32_t nsec; };
#define CLOCK_REALTIME  0   /* wall clock (from the RTC)                       */
#define CLOCK_MONOTONIC 1   /* since boot (from the timer)                     */

/* A getdents record: reclen(2) + type(1) + NUL-terminated name.  Iterate by
 * advancing `off` by reclen. */
struct kdirent { uint16_t reclen; uint8_t type; char name[]; };

/* futex ops (M35). */
#define FUTEX_WAIT  0       /* block iff *uaddr == val                         */
#define FUTEX_WAKE  1       /* wake waiters on uaddr                           */

/* socket() domain / type (M24). */
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

/* Signals (M34) — a small POSIX-shaped set. */
#define NSIG       32
#define SIGINT     2
#define SIGKILL    9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGTERM    15
#define SIGCHLD    17
#define SIG_DFL    0        /* default action (terminate or ignore)            */
#define SIG_IGN    1        /* ignore                                          */

/* poll(2) events (Linux values). */
#define POLLIN      0x001   /* readable                                        */
#define POLLOUT     0x004   /* writable                                        */

struct pollfd {
    int   fd;
    short events;           /* requested (POLLIN/POLLOUT)                      */
    short revents;          /* returned                                        */
};

/* lseek `whence`. */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* Portable syscall handlers (kernel/core/usyscall.c).  Each arch's dispatcher
 * pulls the number + args out of its trapframe and calls these; they operate
 * on the current task's per-process fd table (task->fds).  fds 0/1/2 are the
 * implicit console stdin/stdout/stderr. */
long sys_print(const char* user_str);   /* SYS_PRINT — validated user string */
long sys_write(int fd, const void* buf, size_t n);
long sys_read (int fd, void* buf, size_t n);
int  sys_open (const char* path, int flags);
int  sys_close(int fd);
long sys_lseek(int fd, long off, int whence);
long sys_mmap (size_t len, int fd);     /* map anon (fd<0) or a memfd's frames */
/* §M37 — full mmap (addr+MAP_FIXED, prot→VMM flags, file-backed at offset) for
 * the Linux ABI / ld.so loading shared objects. */
long sys_mmap_full(uintptr_t addr, size_t len, int prot, int flags,
                   int fd, uint64_t offset);
long sys_mprotect(uintptr_t addr, size_t len, int prot);  /* §M37 */
int  sys_memfd(size_t size);            /* create a shared-memory fd           */
int  sys_socketpair(int* fds);          /* fds[0],fds[1] = connected unix pair */
int  sys_pipe(int* fds);                /* fds[0]=read, fds[1]=write            */
int  sys_dup2(int oldfd, int newfd);    /* redirect a descriptor               */
int  sys_kill(int pid, int sig);        /* post a signal to a task             */
long sys_sigaction(int sig, long handler, long restorer);  /* → old handler    */
int  sys_socket(int domain, int type, int proto);          /* M24 socket API   */
int  sys_bind(int fd, int port);
/* O_NONBLOCK on a socket.  The value a non-blocking recv returns when there is
 * nothing to read: Linux's EAGAIN, so a personality layer can pass it straight
 * through to its client.  (The native d-os libc only ever tests for < 0.) */
#define SOCK_EAGAIN 11
/* Which kind of object an fd names (values match enum fd_kind in fd.h): a
 * personality layer routes recvmsg/sendmsg by this. */
#define FDK_VFS     0
#define FDK_SHM     1
#define FDK_SOCK    2
#define FDK_NETSOCK 3
int  sys_fd_kind(int fd);                     /* → FDK_*, or -1 if not open */
int  sys_memfd_resize(int fd, size_t size);   /* ftruncate() on a memfd      */
int  sys_dupfd(int fd, int minfd);            /* F_DUPFD / F_DUPFD_CLOEXEC  */
int  sys_socket_setnonblock(int fd, int on);
int  sys_socket_getnonblock(int fd);          /* → 0/1, or -1 if not a socket */
int  sys_connect(int fd, uint32_t ip, int port);           /* TCP handshake    */
long sys_futex(int* uaddr, int op, int val);               /* M35              */
struct kstat; struct kutsname; struct ktimespec;
int  sys_stat(const char* path, struct kstat* out);        /* M36              */
int  sys_fstat(int fd, struct kstat* out);
long sys_getdents(int fd, void* buf, size_t cap);
long sys_getdents64(int fd, void* buf, size_t cap);   /* Linux dirent64 (linux_abi) */
int  sys_uname(struct kutsname* out);
int  sys_clock_gettime(int which, struct ktimespec* out);
int  sys_nanosleep(unsigned ms);
long sys_getrandom(void* buf, size_t n, unsigned flags);   /* §M39 */
long sys_sendto(int fd, const void* buf, size_t n, uint32_t ip, int port);
long sys_recvfrom(int fd, void* buf, size_t n, uint32_t* ip_out, int* port_out);
long sys_send (int fd, const void* buf, size_t n, int passfd);
long sys_recv (int fd, void* buf, size_t n, int* passfd_out);
struct pollfd;
int  sys_poll (struct pollfd* fds, int nfds, int timeout);

/* ---------------------------------------------------------------------------
 * *_k cores — the KERNEL-pointer entry points of the handlers above.
 *
 * The sys_* functions gate every pointer argument as a RING-3 pointer (§1.1:
 * a bad user pointer must return an error, never fault the kernel).  A few
 * handlers are also called from inside the kernel with KERNEL destinations —
 * the Linux personality fills a d-os struct and marshals it into the foreign
 * layout itself (kstat → stat64, ktimespec → timespec, ip/port → sockaddr_in).
 * Those callers use these ungated cores; anything reachable from ring 3 keeps
 * going through the gated wrapper.  `kpath` is a KERNEL string — copy a user
 * path in with copy_str_from_user first.
 * ------------------------------------------------------------------------- */
int  sys_stat_k(const char* kpath, struct kstat* out);
int  sys_fstat_k(int fd, struct kstat* out);
int  sys_clock_gettime_k(int which, struct ktimespec* out);
long sys_recvfrom_k(int fd, void* buf, size_t n, uint32_t* ip_out, int* port_out);
/* recv with the PAYLOAD going to ring 3 and any passed descriptor coming back in
 * a kernel local (the Linux personality marshals it into SCM_RIGHTS itself). */
long sys_recv_u(int fd, uintptr_t ubuf, size_t n, int* kpassfd_out);

/* Bulk-payload cores (§1.1 layer 3).  The gated wrappers above stage ring-3
 * payloads through a kernel chunk and then call these, so the VFS / socket /
 * console layers below never dereference a user pointer.  Call them directly
 * only with KERNEL buffers — e.g. the Linux personality's sendmsg, which
 * gathers the client's iovecs into a kernel array first. */
long sys_write_k(int fd, const void* buf, size_t n);
long sys_read_k (int fd, void* buf, size_t n);
long sys_send_k (int fd, const void* buf, size_t n, int passfd);
long sys_recv_k (int fd, void* buf, size_t n, int* passfd_out);
long sys_sendto_k(int fd, const void* buf, size_t n, uint32_t ip, int port);
long sys_getdents_k  (int fd, void* buf, size_t cap);
long sys_getdents64_k(int fd, void* buf, size_t cap);
int  sys_poll_k(struct pollfd* fds, int nfds, int timeout);

/* recvfrom with a RING-3 payload buffer but KERNEL (ip, port) out-parameters —
 * the shape a foreign personality needs when it marshals the source address
 * into its own sockaddr layout.  Validates + bounces the payload itself. */
long sys_recvfrom_u(int fd, uintptr_t ubuf, size_t n, uint32_t* ip_out, int* port_out);

/* Close every user fd (>= 3) the current task opened — the exec path calls it
 * when a user program returns so open files don't leak onto the host task. */
void fd_close_all(void);

struct int_frame;
void syscall_dispatch(struct int_frame* f);

/* M34 signals (arch — hal/x86/signal.c).  signal_deliver runs on the
 * return-to-user path after each syscall; signal_sigreturn restores the
 * pre-handler context for SYS_SIGRETURN. */
#if defined(__aarch64__)
/* The "interrupt frame" is per-arch: x86 has struct int_frame, AArch64 has the
 * trapframe vectors.S builds (x0..x30 + ELR + SPSR).  Declaring these with the
 * x86 type on every arch made the ARM definitions conflict with their own
 * header — the signature has to follow the frame, not the other way round. */
struct trapframe;
void signal_deliver(struct trapframe* f);
void signal_sigreturn(struct trapframe* f);
#else
void signal_deliver(struct int_frame* f);
void signal_sigreturn(struct int_frame* f);
#endif

/* M36 / §M41 — Linux i386 syscall-ABI dispatch (hal/x86/linux_abi.c).  The
 * native dispatcher routes here for a process with the Linux personality. */
#if defined(__aarch64__)
/* Same reason as signal_deliver above: the trap frame is per-arch, so the
 * signature has to follow it. */
void linux_syscall_dispatch(struct trapframe* f);
#else
void linux_syscall_dispatch(struct int_frame* f);
#endif

#endif
