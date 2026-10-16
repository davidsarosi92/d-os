/* newapimusl.c — §M90: the "new" Linux process and path calls, as Linux
 * answers them.  Prints "newapi: PASS" or the first failing check.
 *
 *   pidfd    pidfd_open, waitid(P_PIDFD) (ECHILD for a non-child — the exact
 *            probe Go's runtime makes), pidfd_send_signal (0 probes; ESRCH
 *            once the process has exited), poll readiness on exit,
 *            clone(CLONE_PIDFD) and clone3's pidfd
 *   clone3   the fork shape (exit_signal, pidfd), the THREAD shape on a new
 *            stack (x86_64 and arm64: a few instructions of asm), and its
 *            argument checks (CSIGNAL in flags, stack without size, an
 *            oversized struct with non-zero tail)
 *   openat2  RESOLVE_IN_ROOT (absolute link and ".." stay inside),
 *            RESOLVE_BENEATH (EXDEV), RESOLVE_NO_SYMLINKS (ELOOP),
 *            RESOLVE_NO_MAGICLINKS (ELOOP), and the struct-size rules
 *   open_tree + mount_setattr + move_mount — a detached read-only copy of a
 *            directory attached elsewhere (as root)
 *
 * Every check is one a kernel without the feature FAILS: ENOSYS is reported
 * as a failure, never skipped. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mount.h>
#include <sched.h>
#include <sys/prctl.h>

#define NR_pidfd_send_signal 424
#define NR_open_tree         428
#define NR_move_mount        429
#define NR_pidfd_open        434
#define NR_clone3            435
#define NR_openat2           437
#define NR_mount_setattr     442
#ifndef P_PIDFD
#define P_PIDFD 3
#endif
#define CLONE_PIDFD_     0x00001000
#define RESOLVE_NO_XDEV       0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS   0x04
#define RESOLVE_BENEATH       0x08
#define RESOLVE_IN_ROOT       0x10

struct clone_args_ {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size,
             tls, set_tid, set_tid_size, cgroup;
};
struct open_how_ { uint64_t flags, mode, resolve; };
struct mount_attr_ { uint64_t attr_set, attr_clr, propagation, userns_fd; };

static int g_fails;
static void check(int ok, const char* what) {
    if (!ok) { printf("newapi: FAIL %s (errno %d)\n", what, errno); g_fails++; }
    else     printf("newapi: ok   %s\n", what);
}
static long sc(long n, long a, long b, long c, long d, long e) {
    return syscall(n, a, b, c, d, e);
}

/* ---- pidfd ---------------------------------------------------------------- */
static void t_pidfd(void) {
    int me = (int)sc(NR_pidfd_open, getpid(), 0, 0, 0, 0);
    check(me >= 0, "pidfd_open(self)");
    if (me < 0) return;
    check(fcntl(me, F_GETFD) & FD_CLOEXEC, "a pidfd is close-on-exec");
    siginfo_t si;
    errno = 0;
    long r = syscall(SYS_waitid, P_PIDFD, me, &si, WEXITED, 0);
    check(r == -1 && errno == ECHILD, "waitid(P_PIDFD, own pidfd) is ECHILD (Go's probe)");
    check(sc(NR_pidfd_send_signal, me, 0, 0, 0, 0) == 0, "pidfd_send_signal(self, 0)");
    errno = 0;
    check(sc(NR_pidfd_send_signal, me, 0, 0, 1, 0) == -1 && errno == EINVAL,
          "pidfd_send_signal with unknown flags is EINVAL");
    close(me);

    pid_t c = fork();
    if (c == 0) { usleep(300000); _exit(7); }
    int pf = (int)sc(NR_pidfd_open, c, 0, 0, 0, 0);
    check(pf >= 0, "pidfd_open(child)");
    struct pollfd p = { pf, POLLIN, 0 };
    check(poll(&p, 1, 0) == 0, "a live child's pidfd is not readable");
    p.revents = 0;
    check(poll(&p, 1, 3000) == 1 && (p.revents & POLLIN), "the pidfd becomes readable when the child exits");
    memset(&si, 0, sizeof si);
    r = syscall(SYS_waitid, P_PIDFD, pf, &si, WEXITED, 0);
    check(r == 0 && si.si_pid == c && si.si_status == 7 && si.si_code == CLD_EXITED,
          "waitid(P_PIDFD) reaps exactly that child, status 7");
    errno = 0;
    check(sc(NR_pidfd_send_signal, pf, SIGTERM, 0, 0, 0) == -1 && errno == ESRCH,
          "pidfd_send_signal to a reaped process is ESRCH");
    close(pf);

    /* pidfd_send_signal really delivers */
    c = fork();
    if (c == 0) { for (;;) pause(); }
    pf = (int)sc(NR_pidfd_open, c, 0, 0, 0, 0);
    check(sc(NR_pidfd_send_signal, pf, SIGKILL, 0, 0, 0) == 0, "pidfd_send_signal(child, SIGKILL)");
    int st = 0;
    check(waitpid(c, &st, 0) == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
          "the child died of that SIGKILL");
    close(pf);

    /* clone(CLONE_PIDFD): the pidfd lands where parent_tid would */
    int cfd = -1;
    long k = syscall(SYS_clone, (long)(CLONE_PIDFD_ | SIGCHLD), 0L, (long)&cfd, 0L, 0L);
    if (k == 0) _exit(3);
    check(k > 0 && cfd >= 0, "clone(CLONE_PIDFD) gives the parent a pidfd");
    if (k > 0 && cfd >= 0) {
        memset(&si, 0, sizeof si);
        r = syscall(SYS_waitid, P_PIDFD, cfd, &si, WEXITED, 0);
        check(r == 0 && si.si_pid == k && si.si_status == 3, "...and waitid on it reaps that child");
        close(cfd);
    }
    errno = 0;
    k = syscall(SYS_clone, (long)(CLONE_PIDFD_ | CLONE_PARENT_SETTID | SIGCHLD), 0L, (long)&cfd, 0L, 0L);
    if (k == 0) _exit(0);
    check(k == -1 && errno == EINVAL, "CLONE_PIDFD with CLONE_PARENT_SETTID is EINVAL");
}

/* ---- clone3 ----------------------------------------------------------------- */
static volatile int g_thread_ran;
#if defined(__x86_64__) || defined(__aarch64__)
static void thread_body(void) {
    g_thread_ran = 1;
    syscall(SYS_exit, 0);
    for (;;) ;
}
/* clone3 on a new stack: the child must not return into a C frame that lives
 * on the parent's stack, so the split happens in asm and the child calls
 * thread_body directly. */
static long clone3_thread(struct clone_args_* a) {
    long r;
#if defined(__x86_64__)
    register void (*fn)(void) __asm__("rbx") = thread_body;
    __asm__ volatile ("syscall\n\t"
                      "test %%rax, %%rax\n\t"
                      "jnz 1f\n\t"
                      "and $-16, %%rsp\n\t"
                      "call *%%rbx\n\t"
                      "ud2\n"
                      "1:"
                      : "=a"(r)
                      : "a"((long)NR_clone3), "D"(a), "S"(sizeof *a), "r"(fn)
                      : "rcx", "r11", "memory");
#else
    register long x8 __asm__("x8") = NR_clone3;
    register long x0 __asm__("x0") = (long)a;
    register long x1 __asm__("x1") = (long)sizeof *a;
    register void (*fn)(void) __asm__("x19") = thread_body;
    __asm__ volatile ("svc #0\n\t"
                      "cbnz x0, 1f\n\t"
                      "blr x19\n\t"
                      "brk #0\n"
                      "1:"
                      : "+r"(x0)
                      : "r"(x8), "r"(x1), "r"(fn)
                      : "x30", "memory");
    r = x0;
#endif
    return r;
}
#endif

static void t_clone3(void) {
    struct clone_args_ a;
    memset(&a, 0, sizeof a);
    int pfd = -1;
    a.flags = CLONE_PIDFD_;
    a.pidfd = (uint64_t)(uintptr_t)&pfd;
    a.exit_signal = SIGCHLD;
    long r = syscall(NR_clone3, &a, sizeof a);
    if (r == 0) _exit(5);
    check(r > 0 && pfd >= 0, "clone3 fork shape with a pidfd");
    if (r > 0) {
        int st = 0;
        check(waitpid((pid_t)r, &st, 0) == r && WIFEXITED(st) && WEXITSTATUS(st) == 5,
              "...the clone3 child exits 5 to its parent");
    }
    if (pfd >= 0) close(pfd);

    memset(&a, 0, sizeof a);
    a.flags = SIGCHLD;                                  /* CSIGNAL must be 0 in flags */
    errno = 0;
    check(syscall(NR_clone3, &a, sizeof a) == -1 && errno == EINVAL, "clone3 refuses CSIGNAL bits in flags");
    memset(&a, 0, sizeof a);
    a.exit_signal = SIGCHLD; a.stack = 0x1000;          /* a stack with no size */
    errno = 0;
    check(syscall(NR_clone3, &a, sizeof a) == -1 && errno == EINVAL, "clone3 refuses a stack without a size");
    unsigned char big[sizeof a + 8];
    memset(big, 0, sizeof big);
    ((struct clone_args_*)big)->exit_signal = SIGCHLD;
    big[sizeof a + 3] = 1;
    errno = 0;
    check(syscall(NR_clone3, big, sizeof big) == -1 && errno == E2BIG,
          "clone3 refuses a larger struct with a non-zero tail (E2BIG)");

#if defined(__x86_64__) || defined(__aarch64__)
    /* the thread shape */
    static volatile int ctid = -1;
    char* stk = malloc(65536);
    memset(&a, 0, sizeof a);
    a.flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
              CLONE_SYSVSEM | CLONE_CHILD_CLEARTID | CLONE_PARENT_SETTID;
    int ptid = 0;
    ctid = -1;
    a.parent_tid = (uint64_t)(uintptr_t)&ptid;
    a.child_tid  = (uint64_t)(uintptr_t)&ctid;
    a.stack = (uint64_t)(uintptr_t)stk;
    a.stack_size = 65536;
    g_thread_ran = 0;
    long tid = clone3_thread(&a);
    check(tid > 0 && ptid == tid, "clone3 thread shape returns the tid (and PARENT_SETTID)");
    for (int i = 0; i < 300 && (!g_thread_ran || ctid != 0); i++) usleep(10000);
    check(g_thread_ran, "...the thread ran on its own stack");
    check(ctid == 0, "...and CLEARTID cleared the tid word when it exited");
#endif
}

/* ---- openat2 ---------------------------------------------------------------- */
static int put(const char* path, const char* text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write(fd, text, strlen(text));
    close(fd);
    return 0;
}
static int o2(int dirfd, const char* p, uint64_t flags, uint64_t res) {
    struct open_how_ h = { flags, 0, res };
    return (int)syscall(NR_openat2, dirfd, p, &h, sizeof h);
}
static int reads(int fd, const char* want) {
    char b[64] = {0};
    int n = (int)read(fd, b, sizeof b - 1);
    close(fd);
    return n > 0 && strncmp(b, want, strlen(want)) == 0;
}
static void t_openat2(void) {
    mkdir("/tmp", 0755);
    mkdir("/tmp/oa", 0755);
    mkdir("/tmp/oa/root", 0755);
    mkdir("/tmp/oa/root/etc", 0755);
    mkdir("/tmp/oa/root/sub", 0755);
    put("/tmp/oa/root/etc/passwd", "inside");
    put("/tmp/oa/outside", "outside");
    unlink("/tmp/oa/root/link"); symlink("/etc/passwd", "/tmp/oa/root/link");
    unlink("/tmp/oa/root/sub/up"); symlink("../../outside", "/tmp/oa/root/sub/up");
    int root = open("/tmp/oa/root", O_RDONLY | O_DIRECTORY);
    check(root >= 0, "open the scope directory");
    int fd;

    fd = o2(root, "link", O_RDONLY, RESOLVE_IN_ROOT);
    check(fd >= 0 && reads(fd, "inside"), "IN_ROOT: an absolute link resolves inside the root");
    fd = o2(root, "/etc/passwd", O_RDONLY, RESOLVE_IN_ROOT);
    check(fd >= 0 && reads(fd, "inside"), "IN_ROOT: an absolute path starts at the root");
    errno = 0;
    fd = o2(root, "sub/up", O_RDONLY, RESOLVE_IN_ROOT);
    check(fd == -1 && errno == ENOENT, "IN_ROOT: \"..\" through a link stops at the root");
    fd = o2(root, "sub/up", O_RDONLY, 0);
    check(fd >= 0 && reads(fd, "outside"), "no RESOLVE flags: the same link leaves (plain openat)");
    errno = 0;
    fd = o2(root, "link", O_RDONLY, RESOLVE_BENEATH);
    check(fd == -1 && errno == EXDEV, "BENEATH: an absolute link is EXDEV");
    errno = 0;
    fd = o2(root, "../outside", O_RDONLY, RESOLVE_BENEATH);
    check(fd == -1 && errno == EXDEV, "BENEATH: \"..\" past the root is EXDEV");
    fd = o2(root, "sub/../etc/passwd", O_RDONLY, RESOLVE_BENEATH);
    check(fd >= 0 && reads(fd, "inside"), "BENEATH: \"..\" that stays inside is fine");
    errno = 0;
    fd = o2(root, "link", O_RDONLY, RESOLVE_NO_SYMLINKS);
    check(fd == -1 && errno == ELOOP, "NO_SYMLINKS: a symlink is ELOOP");
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/self/fd/%d/etc/passwd", root);
    fd = o2(AT_FDCWD, mp, O_RDONLY, 0);
    check(fd >= 0 && reads(fd, "inside"), "no flags: a /proc magic link is followed");
    errno = 0;
    fd = o2(AT_FDCWD, mp, O_RDONLY, RESOLVE_NO_MAGICLINKS);
    check(fd == -1 && errno == ELOOP, "NO_MAGICLINKS: a /proc magic link is ELOOP");
    fd = o2(root, "newfile", O_WRONLY | O_CREAT, RESOLVE_IN_ROOT);
    check(fd >= 0, "IN_ROOT with O_CREAT makes the last component");
    if (fd >= 0) close(fd);
    check(access("/tmp/oa/root/newfile", F_OK) == 0, "...where it should be");

    struct open_how_ h = { O_RDONLY, 0, 0x40 };
    errno = 0;
    check(syscall(NR_openat2, root, "link", &h, sizeof h) == -1 && errno == EINVAL,
          "an unknown RESOLVE bit is EINVAL");
    h.resolve = 0;
    errno = 0;
    check(syscall(NR_openat2, root, "etc/passwd", &h, 23) == -1 && errno == EINVAL,
          "a struct smaller than open_how is EINVAL");
    unsigned char big[32];
    memset(big, 0, sizeof big);
    memcpy(big, &h, sizeof h);
    big[30] = 1;
    errno = 0;
    check(syscall(NR_openat2, root, "etc/passwd", big, sizeof big) == -1 && errno == E2BIG,
          "a larger struct with a non-zero tail is E2BIG");
    close(root);
}

/* ---- open_tree / move_mount ------------------------------------------------- */
static void t_tree(void) {
    mkdir("/tmp/ot", 0755);
    mkdir("/tmp/ot/src", 0755);
    mkdir("/tmp/ot/dst", 0755);
    put("/tmp/ot/src/f", "tree");
    int t = (int)sc(NR_open_tree, AT_FDCWD, (long)"/tmp/ot/src", 1 /* OPEN_TREE_CLONE */, 0, 0);
    check(t >= 0, "open_tree(OPEN_TREE_CLONE)");
    if (t < 0) return;
    struct mount_attr_ ma = { 1 /* MOUNT_ATTR_RDONLY */, 0, 0, 0 };
    check(syscall(NR_mount_setattr, t, "", 0x1000 /* AT_EMPTY_PATH */, &ma, sizeof ma) == 0,
          "mount_setattr(RDONLY) on the detached tree");
    check(access("/tmp/ot/dst/f", F_OK) == -1, "a detached tree is not visible anywhere yet");
    check(sc(NR_move_mount, t, (long)"", AT_FDCWD, (long)"/tmp/ot/dst", 0x4 /* F_EMPTY_PATH */) == 0,
          "move_mount attaches it");
    int fd = open("/tmp/ot/dst/f", O_RDONLY);
    check(fd >= 0 && reads(fd, "tree"), "the attached tree shows the source's file");
    errno = 0;
    fd = open("/tmp/ot/dst/f", O_WRONLY);
    check(fd == -1 && errno == EROFS, "...read-only, as mount_setattr asked (EROFS)");
    if (fd >= 0) close(fd);
    close(t);
    umount2("/tmp/ot/dst", 0);
    int p = (int)sc(NR_open_tree, AT_FDCWD, (long)"/tmp/ot/src", 0, 0, 0);
    errno = 0;
    check(p >= 0 && sc(NR_move_mount, p, (long)"", AT_FDCWD, (long)"/tmp/ot/dst", 0x4) == -1 &&
          errno == EINVAL, "move_mount of a non-detached (plain O_PATH) tree is EINVAL");
    if (p >= 0) close(p);
}


/* ---- seccomp ------------------------------------------------------------------ */
struct sock_filter_ { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sock_fprog_  { unsigned short len; struct sock_filter_* filter; };
#define BPF_STMT_(c, k)        { (unsigned short)(c), 0, 0, (unsigned)(k) }
#define BPF_JUMP_(c, k, t, f)  { (unsigned short)(c), (unsigned char)(t), (unsigned char)(f), (unsigned)(k) }
#define LD_W_ABS   0x20
#define JEQ_K      0x15
#define RET_K      0x06
#define RET_ALLOW  0x7fff0000u
#define RET_ERRNO(e) (0x00050000u | (e))
#define RET_TRAP   0x00030000u
#define RET_KILLP  0x80000000u
#define RET_LOG    0x7ffc0000u
#if defined(__x86_64__)
#define MY_ARCH 0xC000003Eu
#elif defined(__aarch64__)
#define MY_ARCH 0xC00000B7u
#else
#define MY_ARCH 0x40000003u
#endif
#define NR_seccomp_ SYS_seccomp

/* "if (arch != mine) kill; if (nr == which) return act; return ALLOW" */
static long load_one(long nr, unsigned act, unsigned long flags) {
    struct sock_filter_ f[] = {
        BPF_STMT_(LD_W_ABS, 4),                    /* arch */
        BPF_JUMP_(JEQ_K, MY_ARCH, 1, 0),
        BPF_STMT_(RET_K, RET_KILLP),
        BPF_STMT_(LD_W_ABS, 0),                    /* nr */
        BPF_JUMP_(JEQ_K, (unsigned)nr, 0, 1),
        BPF_STMT_(RET_K, act),
        BPF_STMT_(RET_K, RET_ALLOW),
    };
    struct sock_fprog_ p = { (unsigned short)(sizeof f / sizeof f[0]), f };
    return syscall(NR_seccomp_, 1 /* SET_MODE_FILTER */, flags, &p);
}
static int child_result(pid_t c, int* sig) {
    int st = 0;
    if (waitpid(c, &st, 0) != c) return -1;
    if (WIFSIGNALED(st)) { *sig = WTERMSIG(st); return -2; }
    *sig = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static volatile int g_sys;
static void on_sigsys(int s, siginfo_t* si, void* uc) { (void)uc; if (s == SIGSYS && si->si_code == 1) g_sys = 1; }

static void t_seccomp(void) {
    int sig;
    unsigned avail = RET_ALLOW;
    check(syscall(NR_seccomp_, 2 /* GET_ACTION_AVAIL */, 0, &avail) == 0, "GET_ACTION_AVAIL(ALLOW) = 0");
    avail = 0x7fc00000u;
    errno = 0;
    check(syscall(NR_seccomp_, 2, 0, &avail) == -1 && errno == EOPNOTSUPP,
          "GET_ACTION_AVAIL(USER_NOTIF) = EOPNOTSUPP (no listener side)");

    /* an unprivileged task without no_new_privs may not install a filter */
    pid_t c = fork();
    if (c == 0) {
        if (setuid(1000) != 0) _exit(10);
        errno = 0;
        long r = load_one(SYS_getppid, RET_ERRNO(13), 0);
        _exit(r == -1 && errno == EACCES ? 0 : 1);
    }
    check(child_result(c, &sig) == 0, "no_new_privs or CAP_SYS_ADMIN is required (EACCES)");

    /* ERRNO, the arch check, /proc/self/status, inheritance across fork */
    c = fork();
    if (c == 0) {
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(11);
        if (load_one(SYS_getppid, RET_ERRNO(13), 0) != 0) _exit(12);
        errno = 0;      /* syscall(): musl's getppid() cannot fail, so it sets no errno */
        if (syscall(SYS_getppid) != -1 || errno != EACCES) _exit(13);
        if (getpid() <= 0) _exit(14);                      /* everything else allowed */
        if (prctl(PR_GET_SECCOMP, 0, 0, 0, 0) != 2) _exit(15);
        char b[2048] = {0};
        int fd = open("/proc/self/status", O_RDONLY);
        if (fd < 0) _exit(16);
        read(fd, b, sizeof b - 1); close(fd);
        if (!strstr(b, "Seccomp:\t2") || !strstr(b, "NoNewPrivs:\t1")) _exit(17);
        pid_t g = fork();
        if (g == 0) { errno = 0; _exit(syscall(SYS_getppid) == -1 && errno == EACCES ? 0 : 1); }
        int st = 0;
        waitpid(g, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) _exit(18);
        _exit(0);
    }
    int r = child_result(c, &sig);
    check(r == 0, "ERRNO action, other calls allowed, Seccomp: 2 in status, a forked child inherits it");
    if (r) printf("newapi:      (child said %d sig %d)\n", r, sig);

    /* the most restrictive result of the chain wins */
    c = fork();
    if (c == 0) {
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (load_one(SYS_getppid, RET_LOG, 0) != 0) _exit(20);          /* older: LOG */
        if (load_one(SYS_getppid, RET_ERRNO(7), 0) != 0) _exit(21);     /* newer: ERRNO */
        if (load_one(SYS_getpid, RET_ALLOW, 0) != 0) _exit(22);         /* newest: allows */
        errno = 0;
        _exit(syscall(SYS_getppid) == -1 && errno == 7 ? 0 : 23);
    }
    r = child_result(c, &sig);
    check(r == 0, "a chain of three: the most restrictive answer (ERRNO over LOG and ALLOW) wins");

    /* TRAP: SIGSYS with si_code SYS_SECCOMP, the call returns ENOSYS */
    c = fork();
    if (c == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_sigsys;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSYS, &sa, NULL);
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (load_one(SYS_getppid, RET_TRAP, 0) != 0) _exit(30);
        errno = 0;
        long v = syscall(SYS_getppid);
        _exit(v == -1 && errno == ENOSYS && g_sys ? 0 : 31);
    }
    r = child_result(c, &sig);
    check(r == 0, "TRAP posts SIGSYS (SYS_SECCOMP) and the call returns ENOSYS");

    /* KILL_PROCESS: killed by SIGSYS, whatever handler it has */
    c = fork();
    if (c == 0) {
        signal(SIGSYS, SIG_IGN);
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        if (load_one(SYS_getppid, RET_KILLP, 0) != 0) _exit(40);
        syscall(SYS_getppid);
        _exit(41);
    }
    r = child_result(c, &sig);
    check(r == -2 && sig == SIGSYS, "KILL_PROCESS: the process dies of SIGSYS (even with SIGSYS ignored)");

    /* strict mode: read/write/exit only */
    c = fork();
    if (c == 0) {
        if (prctl(PR_SET_SECCOMP, 1 /* STRICT */, 0, 0, 0) != 0) _exit(50);
        syscall(SYS_write, 1, "", 0);                       /* allowed */
        syscall(SYS_getpid);                                /* not: SIGKILL */
        syscall(SYS_exit, 51);
    }
    r = child_result(c, &sig);
    check(r == -2 && (sig == SIGKILL || sig == SIGSYS), "strict mode: anything but read/write/exit is fatal");

    /* a malformed filter is refused at load */
    c = fork();
    if (c == 0) {
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        struct sock_filter_ bad[] = { BPF_JUMP_(JEQ_K, 0, 5, 5), BPF_STMT_(RET_K, RET_ALLOW) };
        struct sock_fprog_ p = { 2, bad };
        errno = 0;
        long v = syscall(NR_seccomp_, 1, 0, &p);
        if (!(v == -1 && errno == EINVAL)) _exit(60);
        struct sock_filter_ bad2[] = { BPF_STMT_(LD_W_ABS, 70), BPF_STMT_(RET_K, RET_ALLOW) };
        p.filter = bad2;
        errno = 0;
        v = syscall(NR_seccomp_, 1, 0, &p);
        _exit(v == -1 && errno == EINVAL ? 0 : 61);
    }
    r = child_result(c, &sig);
    check(r == 0, "an out-of-range jump or load is refused at load (EINVAL)");
}


/* ---- noexec ------------------------------------------------------------------ */
#include <sys/mman.h>
static void t_noexec(void) {
    mkdir("/tmp/nx", 0755);
    check(mount("tmpfs", "/tmp/nx", "tmpfs", MS_NOEXEC, NULL) == 0, "mount tmpfs with MS_NOEXEC");
    int fd = open("/tmp/nx/prog", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    write(fd, "\177ELF-not-really", 15);
    close(fd);
    chmod("/tmp/nx/prog", 0755);
    pid_t c = fork();
    if (c == 0) {
        char* av[] = { "prog", NULL };
        execve("/tmp/nx/prog", av, NULL);
        _exit(errno == EACCES ? 0 : 1);
    }
    int sig = 0;
    check(child_result(c, &sig) == 0, "execve of a file on a noexec mount is EACCES");
    fd = open("/tmp/nx/prog", O_RDONLY);
    void* m = mmap(NULL, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    check(m == MAP_FAILED && errno == EPERM, "mmap(PROT_EXEC) of it is EPERM");
    m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    check(m != MAP_FAILED, "mmap(PROT_READ) of it is fine");
    if (m != MAP_FAILED) munmap(m, 4096);
    close(fd);
    check(mount(NULL, "/tmp/nx", NULL, MS_REMOUNT, NULL) == 0, "remount without MS_NOEXEC");
    c = fork();
    if (c == 0) {
        char* av[] = { "prog", NULL };
        execve("/tmp/nx/prog", av, NULL);
        _exit(errno == EACCES ? 1 : 0);                 /* now a format error, not EACCES */
    }
    check(child_result(c, &sig) == 0, "...after which exec is no longer refused for noexec");
    umount2("/tmp/nx", 0);
}


/* ---- a Linux parent's death: reparent, never kill; PR_SET_PDEATHSIG ---------- */
static volatile int g_pdeath;
static void on_usr2(int s) { (void)s; g_pdeath = 1; }
static void t_reparent(void) {
    int p[2];
    if (pipe(p) != 0) { check(0, "pipe"); return; }
    pid_t a = fork();
    if (a == 0) {
        pid_t b = fork();
        if (b == 0) {
            signal(SIGUSR2, on_usr2);
            prctl(PR_SET_PDEATHSIG, SIGUSR2, 0, 0, 0);
            pid_t first = getppid();
            for (int i = 0; i < 200 && getppid() == first; i++) usleep(10000);
            for (int i = 0; i < 100 && !g_pdeath; i++) usleep(10000);
            char r[3] = { getppid() != first ? 'R' : 'r', g_pdeath ? 'D' : 'd', 0 };
            write(p[1], r, 2);
            _exit(0);
        }
        usleep(100000);
        _exit(0);                                     /* the parent goes first */
    }
    close(p[1]);
    int st = 0;
    waitpid(a, &st, 0);
    char r[3] = {0};
    int n = (int)read(p[0], r, 2);
    close(p[0]);
    check(n == 2, "the grandchild outlived its exited parent (it was not killed with it)");
    check(n == 2 && r[0] == 'R', "...and was reparented (getppid changed)");
    check(n == 2 && r[1] == 'D', "...and got its PR_SET_PDEATHSIG signal");
}


/* ---- execve from a non-leader thread takes over the leader's pid (NEXT #16) -- */
#include <pthread.h>
/* A minimal static ELF whose whole program is exit_group(getpid() & 0xff). */
__attribute__((unused)) static int write_tiny_elf(const char* path) {
#if defined(__x86_64__)
    static const unsigned char code[] = {
        0xb8, 39, 0, 0, 0,          /* mov eax, 39 (getpid)        */
        0x0f, 0x05,                 /* syscall                     */
        0x89, 0xc7,                 /* mov edi, eax                */
        0xb8, 231, 0, 0, 0,         /* mov eax, 231 (exit_group)   */
        0x0f, 0x05 };
    const unsigned short machine = 62;
#elif defined(__aarch64__)
    static const unsigned char code[] = {
        0x88, 0x15, 0x80, 0xd2,     /* mov x8, #172 (getpid)       */
        0x01, 0x00, 0x00, 0xd4,     /* svc #0                      */
        0xc8, 0x0b, 0x80, 0xd2,     /* mov x8, #94 (exit_group)    */
        0x01, 0x00, 0x00, 0xd4 };   /* svc #0                      */
    const unsigned short machine = 183;
#else
    return -1;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
    unsigned char img[64 + 56 + sizeof code];
    memset(img, 0, sizeof img);
    const uint64_t base = 0x400000, entry = base + 64 + 56;
    memcpy(img, "\177ELF", 4);
    img[4] = 2; img[5] = 1; img[6] = 1;                 /* 64-bit, LE, v1 */
    *(uint16_t*)(img + 16) = 2;                         /* ET_EXEC */
    *(uint16_t*)(img + 18) = machine;
    *(uint32_t*)(img + 20) = 1;
    *(uint64_t*)(img + 24) = entry;
    *(uint64_t*)(img + 32) = 64;                        /* phoff */
    *(uint16_t*)(img + 52) = 64;                        /* ehsize */
    *(uint16_t*)(img + 54) = 56;                        /* phentsize */
    *(uint16_t*)(img + 56) = 1;                         /* phnum */
    unsigned char* ph = img + 64;
    *(uint32_t*)(ph + 0) = 1;                           /* PT_LOAD */
    *(uint32_t*)(ph + 4) = 5;                           /* R + X */
    *(uint64_t*)(ph + 16) = base;                       /* vaddr */
    *(uint64_t*)(ph + 24) = base;                       /* paddr */
    *(uint64_t*)(ph + 32) = sizeof img;                 /* filesz */
    *(uint64_t*)(ph + 40) = sizeof img;                 /* memsz */
    *(uint64_t*)(ph + 48) = 0x1000;                     /* align */
    memcpy(img + 64 + 56, code, sizeof code);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) return -1;
    int ok = write(fd, img, sizeof img) == (ssize_t)sizeof img;
    close(fd);
    chmod(path, 0755);
    return ok ? 0 : -1;
#endif
}
__attribute__((unused)) static void* exec_from_thread(void* a) {
    (void)a;
    char* av[] = { "tiny", NULL };
    execve("/tmp/tinyexit", av, NULL);
    _exit(77);                                          /* exec failed */
}
static void t_thread_exec(void) {
#if defined(__x86_64__) || defined(__aarch64__)
    if (write_tiny_elf("/tmp/tinyexit") != 0) { check(0, "write the tiny ELF"); return; }
    pid_t p = fork();
    if (p == 0) {
        pthread_t th;
        pthread_create(&th, NULL, exec_from_thread, NULL);
        for (;;) pause();                               /* the leader waits to be replaced */
    }
    int st = 0;
    pid_t w = waitpid(p, &st, 0);
    check(w == p && WIFEXITED(st), "a thread's execve: the process exits normally (the leader's death is not reported)");
    check(w == p && WIFEXITED(st) && WEXITSTATUS(st) == (p & 0xff),
          "...and the new program runs under the PROCESS's pid (it took the leader's)");
    if (w == p && WIFEXITED(st) && WEXITSTATUS(st) != (p & 0xff))
        printf("newapi:      (exit status %d, process pid %d)\n", WEXITSTATUS(st), (int)p);
#endif
}

/* Run in a FORKED child (as killmusl does): the program itself may be hosted
 * by a kernel task (a shell excursion), and its children and threads must
 * belong to an ordinary process. */
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pid_t r = fork();
    if (r == 0) {
        t_pidfd();
        t_clone3();
        t_openat2();
        t_tree();
        t_seccomp();
        t_noexec();
        t_reparent();
        t_thread_exec();
        printf("newapi: %s (%d failed)\n", g_fails ? "FAIL" : "PASS", g_fails);
        _exit(g_fails ? 1 : 0);
    }
    int st = 0;
    waitpid(r, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}
