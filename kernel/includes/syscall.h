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
long sys_write(int fd, const void* buf, size_t n);
long sys_read (int fd, void* buf, size_t n);
int  sys_open (const char* path, int flags);
int  sys_close(int fd);
long sys_lseek(int fd, long off, int whence);
long sys_mmap (size_t len, int fd);     /* map anon (fd<0) or a memfd's frames */
int  sys_memfd(size_t size);            /* create a shared-memory fd           */
int  sys_socketpair(int* fds);          /* fds[0],fds[1] = connected unix pair */
int  sys_pipe(int* fds);                /* fds[0]=read, fds[1]=write            */
int  sys_dup2(int oldfd, int newfd);    /* redirect a descriptor               */
int  sys_kill(int pid, int sig);        /* post a signal to a task             */
long sys_sigaction(int sig, long handler, long restorer);  /* → old handler    */
int  sys_socket(int domain, int type, int proto);          /* M24 socket API   */
int  sys_bind(int fd, int port);
long sys_sendto(int fd, const void* buf, size_t n, uint32_t ip, int port);
long sys_recvfrom(int fd, void* buf, size_t n, uint32_t* ip_out, int* port_out);
long sys_send (int fd, const void* buf, size_t n, int passfd);
long sys_recv (int fd, void* buf, size_t n, int* passfd_out);
struct pollfd;
int  sys_poll (struct pollfd* fds, int nfds, int timeout);

/* Close every user fd (>= 3) the current task opened — the exec path calls it
 * when a user program returns so open files don't leak onto the host task. */
void fd_close_all(void);

struct int_frame;
void syscall_dispatch(struct int_frame* f);

/* M34 signals (arch — hal/x86/signal.c).  signal_deliver runs on the
 * return-to-user path after each syscall; signal_sigreturn restores the
 * pre-handler context for SYS_SIGRETURN. */
void signal_deliver(struct int_frame* f);
void signal_sigreturn(struct int_frame* f);

#endif
