/* =============================================================================
 * linux_abi.c — Linux x86_64 syscall-ABI compatibility layer (M36 / §M41),
 * x86_64 sibling of kernel/hal/x86/linux_abi.c.
 *
 * Runs an UNMODIFIED x86_64 musl/Linux binary by providing the Linux x86_64
 * system-call ABI it expects, keeping musl pristine.  Two things differ from
 * the i386 file, everything else is the same idea:
 *
 *   1. ENTRY: x86_64 musl issues the `syscall` INSTRUCTION (not int 0x80).
 *      syscall_entry.s traps it, fabricates an int-frame with int_no=0x81, and
 *      idt.c routes here.  Register convention is the SysV/Linux one:
 *          rax = number
 *          rdi, rsi, rdx, r10, r8, r9 = args 0..5
 *          rax = return value (written back into f->rax)
 *
 *   2. NUMBERS + STRUCTS: the x86_64 Linux syscall table (unistd_64.h) and the
 *      x86_64 `struct stat` (st_nlink precedes st_mode; all fields 64-bit) —
 *      NOT i386's int-0x80 numbers / stat64.  TLS is arch_prctl(ARCH_SET_FS)
 *      onto the FS.base MSR, not i386's set_thread_area/%gs.
 *
 * Kept deliberately isolated: the native d-os ABI (int 0x80, syscall.c) is
 * untouched; this is the single place the Linux x86_64 number space + struct
 * translations live.
 *
 * Reference: arch/x86/entry/syscalls/syscall_64.tbl.
 * ============================================================================= */

#include "lnx_signal.h"   /* §M89 */
#include "syscall.h"
#include "idt.h"
#include "task.h"
#include "abi.h"      /* §M50 — the shared guest-ABI translation engine */
#include "percpu.h"     /* smp_ncpus — sched_getaffinity */
#include "printf.h"
#include "hal_api.h"
#include "vfs.h"
#include "vmm.h"      /* §1.1 — vmm_user_access_ok / copy_str_from_user */
#include "proc.h"          /* proc_fork / proc_execve / proc_clone */
#include "usermode.h"      /* struct user_regs (fork snapshot)     */
#include "dosgui.h"        /* §M42 display bridge (create/present/poll)  */
#include <stdint.h>
#include <stddef.h>

/* Excursion teleport-back (shared with the native path, usermode.s). */
#include "usermode.h"

/* ---- Linux x86_64 syscall numbers (unistd_64.h) --------------------------- */
#define LNX_read              0
#define LNX_write             1
#define LNX_open              2
#define LNX_close             3
#define LNX_stat              4
#define LNX_fstat             5
#define LNX_lseek             8
#define LNX_pread64          17
#define LNX_pwrite64         18
#define LNX_mmap              9
#define LNX_mprotect         10
#define LNX_munmap           11
#define LNX_brk              12
#define LNX_rt_sigaction     13
#define LNX_rt_sigprocmask   14
#define LNX_ioctl            16
#define LNX_readv            19
#define LNX_writev           20
#define LNX_pipe             22
#define LNX_dup2             33
#define LNX_pipe2           293
#define LNX_getpid           39
#define LNX_poll              7   /* musl's DNS resolver waits on the UDP socket
                                   * with poll(); struct pollfd is byte-identical
                                   * to ours on both arches.                     */
#define LNX_socket           41
#define LNX_connect          42
#define LNX_sendto           44
#define LNX_recvfrom         45
#define LNX_sendmsg          46
#define LNX_recvmsg          47
#define LNX_shutdown         48
#define LNX_bind             49
#define LNX_listen           50
#define LNX_getsockname      51
#define LNX_getpeername      52
#define LNX_setsockopt       54
#define LNX_getsockopt       55
#define LNX_clone            56
/* clone() flag bits we care about (linux/sched.h). */
#define LNX_CLONE_VM                0x00000100
#define LNX_CLONE_PARENT_SETTID     0x00100000
#define LNX_CLONE_CHILD_CLEARTID    0x00200000
#define LNX_CLONE_CHILD_SETTID      0x01000000
#define LNX_fork             57
#define LNX_vfork            58   /* §M73 — served as a fork (see the case) */
#define LNX_execve           59
#define LNX_exit             60
#define LNX_wait4            61
#define LNX_kill             62
#define LNX_uname            63
#define LNX_fcntl            72
#define LNX_getdents         78
#define LNX_unlink           87
#define LNX_gettimeofday     96
#define LNX_getuid          102
#define LNX_getgid          104
#define LNX_geteuid         107
#define LNX_getegid         108
#define LNX_arch_prctl      158
#define LNX_gettid          186
#define LNX_futex           202
#define LNX_getdents64      217
#define LNX_set_tid_address 218
#define LNX_clock_gettime   228
#define LNX_exit_group      231
#define LNX_openat          257
#define LNX_set_robust_list 273
#define LNX_getrandom       318
/* §M40 — a Wayland client's shm pool: memfd_create gives a zero-length object,
 * ftruncate sizes it, then the fd travels over SCM_RIGHTS. */
#define LNX_ftruncate        77
#define LNX_memfd_create    319
#define LNX_membarrier      324

#define LNX_ENOSYS  38
#define LNX_EINVAL  22
#define LNX_readlink         89
#define LNX_readlinkat      267
#define LNX_access           21
#define LNX_faccessat       269
#define LNX_madvise          28
#define LNX_mincore          27
/* Mesa sizes its thread pools from the CPU affinity mask; a bare -ENOSYS makes
 * it compute a nonsense CPU count and then dereference null. */
#define LNX_sched_setaffinity 203
#define LNX_sched_getaffinity 204
#define LNX_nanosleep        35
#define LNX_clock_nanosleep 230
#define LNX_sched_yield      24
/* §M42 d-os display-bridge syscalls (well above the Linux range, so no clash). */
#define LNX_DOSGUI_CREATE  0xD050
#define LNX_DOSGUI_PRESENT 0xD051
#define LNX_DOSGUI_POLL    0xD052
#define LNX_DOSGUI_DESTROY 0xD053
#define LNX_ENOTTY  25
#define LNX_ENOENT   2
#define LNX_EFAULT  14
#define LNX_ENOMEM  12
#define PAGE_SIZE   4096

/* arch_prctl subfunction codes (asm/prctl.h). */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* Linux O_* open flags (asm-generic/fcntl.h — same values on i386 + x86_64). */
#define LO_WRONLY   00000001
#define LO_RDWR     00000002
#define LO_CREAT    00000100
#define LO_TRUNC    00001000
#define LO_ACCMODE  00000003
#define LAT_FDCWD   (-100)


/* x86_64 iovec — 64-bit base + length. */
struct lnx_iovec { void* iov_base; uint64_t iov_len; };

/* ---- SCM_RIGHTS (§M40) ------------------------------------------------------
 * Passing a file descriptor over a UNIX socket is how a Wayland client hands the
 * compositor its shm pool, so libwayland cannot create a single buffer without
 * it.  Linux carries the descriptor in the msghdr's ANCILLARY data:
 *
 *     struct cmsghdr { size_t cmsg_len; int cmsg_level; int cmsg_type; }
 *     followed by the payload, each block padded to sizeof(long).
 *
 * `size_t` and the alignment differ between i386 and x86_64, which is precisely
 * why this struct is declared per-arch rather than shared — the same C text
 * yields the right 12-byte/4-aligned and 16-byte/8-aligned layouts.
 *
 * d-os already moves descriptors between tasks (usock.c, M25 stage 5); this is
 * only the Linux-shaped wrapper around sys_send_k/sys_recv's `passfd`. */
#define LNX_SOL_SOCKET  1
#define LNX_SCM_RIGHTS  1

/* Defined further down with the other ring-3 pointer checks. */
static int lnx_r_ok(uintptr_t uptr, uintptr_t len);
static int lnx_w_ok(uintptr_t uptr, uintptr_t len);

struct lnx_cmsghdr {
    unsigned long cmsg_len;      /* header + payload, unpadded */
    int           cmsg_level;
    int           cmsg_type;
};

#define LNX_CMSG_ALIGN(n) (((n) + sizeof(long) - 1) & ~(unsigned long)(sizeof(long) - 1))

/* Pull the FIRST descriptor out of a client's control buffer, or -1.  The
 * control buffer is client memory, so it is validated before being read. */
static int lnx_cmsg_take_fd(const void* ctl, unsigned long ctllen) {
    if (!ctl || ctllen < sizeof(struct lnx_cmsghdr)) return -1;
    if (!lnx_r_ok((uintptr_t)ctl, ctllen)) return -1;
    const struct lnx_cmsghdr* c = (const struct lnx_cmsghdr*)ctl;
    if (c->cmsg_level != LNX_SOL_SOCKET || c->cmsg_type != LNX_SCM_RIGHTS)
        return -1;
    if (c->cmsg_len < LNX_CMSG_ALIGN(sizeof *c) + sizeof(int)) return -1;
    if (c->cmsg_len > ctllen) return -1;
    const int* fds = (const int*)((const uint8_t*)ctl + LNX_CMSG_ALIGN(sizeof *c));
    return fds[0];
}

/* Write a one-descriptor SCM_RIGHTS block into the client's control buffer and
 * report how many bytes it occupies (0 if it does not fit / none passed). */
static unsigned long lnx_cmsg_put_fd(void* ctl, unsigned long ctllen, int fd) {
    unsigned long need = LNX_CMSG_ALIGN(sizeof(struct lnx_cmsghdr)) + sizeof(int);
    if (fd < 0 || !ctl || ctllen < need) return 0;
    if (!lnx_w_ok((uintptr_t)ctl, need)) return 0;
    struct lnx_cmsghdr* c = (struct lnx_cmsghdr*)ctl;
    c->cmsg_len   = need;
    c->cmsg_level = LNX_SOL_SOCKET;
    c->cmsg_type  = LNX_SCM_RIGHTS;
    int* fds = (int*)((uint8_t*)ctl + LNX_CMSG_ALIGN(sizeof(struct lnx_cmsghdr)));
    fds[0] = fd;
    return need;
}


/* ---- BSD sockets (Linux x86_64 gives each call its own syscall number, unlike
 * i386's single multiplexed socketcall) --------------------------------------
 *
 * sockaddr_in is a WIRE structure — identical 16 bytes on both arches — but
 * `struct msghdr` is not: its length fields are size_t/socklen_t, so the amd64
 * layout has 64-bit iovlen/controllen and padding after msg_namelen.  Getting
 * that wrong reads msg_iov out of the padding and fails silently, so the layout
 * is spelled out with its offsets rather than copied from the i386 twin. */
#define AF_INET_LNX      2
/* Type-bits Linux ORs into socket()'s `type` argument. */
#define LSOCK_NONBLOCK  0x800
#define LSOCK_CLOEXEC   0x80000
#define LNX_EAFNOSUPPORT 97
#define LNX_EOPNOTSUPP   95

struct lnx_sockaddr_in {
    uint16_t sin_family;     /* AF_INET == 2 */
    uint16_t sin_port;       /* network byte order */
    uint32_t sin_addr;       /* network byte order */
    uint8_t  sin_zero[8];
};

struct lnx_msghdr {          /* amd64: 56 bytes */
    void*             msg_name;        /*  0: optional source/dest sockaddr   */
    uint32_t          msg_namelen;     /*  8: in room / out actual (+4 pad)   */
    uint32_t          _pad0;
    struct lnx_iovec* msg_iov;         /* 16: scatter/gather buffers          */
    uint64_t          msg_iovlen;      /* 24: iovec count (size_t!)           */
    void*             msg_control;     /* 32: ancillary data (unused here)    */
    uint64_t          msg_controllen;  /* 40 */
    int               msg_flags;       /* 48: out — we report 0               */
    uint32_t          _pad1;
};


#define LNX_S_IFREG 0100000u
#define LNX_S_IFDIR 0040000u


/* --------------------------------------------------------------------------
 * User-pointer discipline in this dispatcher (§1.1) — see the i386 twin.
 * The portable sys_* handlers gate their own ring-3 pointers; this file also
 * marshals results into the Linux layout, so it calls the ungated sys_*_k cores
 * with KERNEL structs and validates the ring-3 destinations itself.
 * ------------------------------------------------------------------------ */
static int lnx_w_ok(uintptr_t uptr, uintptr_t len) {   /* ring-3 write target */
    return uptr && vmm_user_access_ok(uptr, len, 1);
}
static int lnx_r_ok(uintptr_t uptr, uintptr_t len) {   /* ring-3 read source  */
    return uptr && vmm_user_access_ok(uptr, len, 0);
}
/* ---- sockaddr marshalling (twin of the i386 helpers) -----------------------
 * Byte-order conversion lives ONLY here: it is a Linux-ABI concern, not
 * something the M24 stack should know about. */
static int sockaddr_to_hostorder(const struct lnx_sockaddr_in* sa,
                                 uint32_t* ip_out, int* port_out) {
    if (!lnx_r_ok((uintptr_t)sa, sizeof *sa)) return -1;
    if (sa->sin_family != AF_INET_LNX) return -1;
    const uint8_t* a = (const uint8_t*)&sa->sin_addr;   /* network order bytes */
    const uint8_t* p = (const uint8_t*)&sa->sin_port;
    *ip_out   = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
                ((uint32_t)a[2] << 8)  |  (uint32_t)a[3];
    *port_out = ((int)p[0] << 8) | (int)p[1];
    return 0;
}

/* Fill a Linux sockaddr_in (network order) from a host-order (ip, port).
 * Honours the caller's addrlen: requiring the full struct would wrongly reject
 * a caller that legitimately passed a shorter buffer. */
/* `addrlen` is an in/out KERNEL word: in = how much room the client gave us,
 * out = the length we would have written.  It is deliberately NOT a client
 * pointer, because this helper has callers with both origins — recvfrom hands
 * us the client's socklen_t, recvmsg hands us msg_namelen lifted out of an
 * already-validated msghdr.  Validating it here as a ring-3 pointer therefore
 * made the recvmsg path bail out silently, leaving msg_name untouched: musl's
 * resolver then compared the (still zeroed) source address against its
 * nameserver list, decided the reply came from a stranger and dropped every
 * single DNS answer.  Same lesson as the sys_*_k cores — check where the
 * pointer's ORIGIN is known.  `sa` itself IS a client pointer and is still
 * validated below. */
static void hostorder_to_sockaddr(struct lnx_sockaddr_in* sa, uint32_t* addrlen,
                                  uint32_t ip, int port) {
    struct lnx_sockaddr_in tmp;
    tmp.sin_family = AF_INET_LNX;
    ((uint8_t*)&tmp.sin_port)[0] = (uint8_t)(port >> 8);
    ((uint8_t*)&tmp.sin_port)[1] = (uint8_t)port;
    ((uint8_t*)&tmp.sin_addr)[0] = (uint8_t)(ip >> 24);
    ((uint8_t*)&tmp.sin_addr)[1] = (uint8_t)(ip >> 16);
    ((uint8_t*)&tmp.sin_addr)[2] = (uint8_t)(ip >> 8);
    ((uint8_t*)&tmp.sin_addr)[3] = (uint8_t)ip;
    for (int i = 0; i < 8; i++) tmp.sin_zero[i] = 0;
    uint32_t room = addrlen ? *addrlen : (uint32_t)sizeof tmp;
    uint32_t cnt  = room < sizeof tmp ? room : (uint32_t)sizeof tmp;
    if (cnt && !lnx_w_ok((uintptr_t)sa, cnt)) return;
    for (uint32_t i = 0; i < cnt; i++) ((uint8_t*)sa)[i] = ((uint8_t*)&tmp)[i];
    if (addrlen) *addrlen = (uint32_t)sizeof tmp;
}

/* recvmsg(fd, msg, flags) — the resolver's receive path.  musl's __res_msend
 * receives every DNS answer here and DROPS any reply whose source address does
 * not match a nameserver it queried, so filling msg_name is not optional:
 * leaving it alone makes getaddrinfo fail silently. */
static long linux_recvmsg(int fd, struct lnx_msghdr* mh, int flags) {
    (void)flags;
    if (!lnx_w_ok((uintptr_t)mh, sizeof *mh)) return -LNX_EFAULT;
    if (!mh->msg_iov || mh->msg_iovlen == 0) return -LNX_EOPNOTSUPP;
    if (!lnx_r_ok((uintptr_t)mh->msg_iov, sizeof(struct lnx_iovec))) return -LNX_EFAULT;
    struct lnx_iovec* iov = &mh->msg_iov[0];

    /* Route by fd KIND.  A UNIX socket (the Wayland display connection) has no
     * datagram source address and is served by the ordinary read path; only an
     * AF_INET socket goes through recvfrom.  Handling only the latter is what
     * made upstream libwayland's very first read fail. */
    if (sys_fd_kind(fd) != FDK_NETSOCK) {
        /* sys_recv is the read that can also carry a DESCRIPTOR; anything it
         * hands back is marshalled into the client's SCM_RIGHTS block. */
        int passfd = -1;
        long r = sys_recv_u(fd, (uintptr_t)iov->iov_base, (size_t)iov->iov_len,
                            &passfd);
        if (r < 0) return r;
        mh->msg_namelen    = 0;      /* connection-mode: no source address */
        mh->msg_flags      = 0;
        unsigned long ctl = lnx_cmsg_put_fd(mh->msg_control,
                                            (unsigned long)mh->msg_controllen,
                                            passfd);
        mh->msg_controllen = ctl;
        return r;
    }

    uint32_t ip = 0; int port = 0;
    /* Payload lands in the CLIENT's iovec, source address in kernel locals —
     * sys_recvfrom_u is exactly that split and bounce-buffers the payload. */
    long n = sys_recvfrom_u(fd, (uintptr_t)iov->iov_base, (size_t)iov->iov_len,
                            &ip, &port);
    if (n < 0) return n;
    if (mh->msg_name) {
        uint32_t namelen = mh->msg_namelen;
        hostorder_to_sockaddr((struct lnx_sockaddr_in*)mh->msg_name, &namelen, ip, port);
        mh->msg_namelen = namelen;
    }
    mh->msg_flags = 0;
    return n;
}

/* sendmsg(fd, msg, flags) — gather the iovecs and send as one message.  Used by
 * musl's TCP-fallback DNS path.  `buf` is a KERNEL gather buffer, so this calls
 * the *_k cores: the gated sys_write/sys_sendto would (correctly) reject a
 * kernel address while task->in_user_syscall is set. */
static long linux_sendmsg(int fd, const struct lnx_msghdr* mh, int flags) {
    (void)flags;
    if (!lnx_r_ok((uintptr_t)mh, sizeof *mh)) return -LNX_EFAULT;
    if (!mh->msg_iov || mh->msg_iovlen > 1024) return -LNX_EOPNOTSUPP;
    if (!lnx_r_ok((uintptr_t)mh->msg_iov,
                  (uintptr_t)mh->msg_iovlen * sizeof(struct lnx_iovec)))
        return -LNX_EFAULT;
    uint8_t buf[1024];
    size_t total = 0;
    for (uint64_t i = 0; i < mh->msg_iovlen; i++) {
        const struct lnx_iovec* v = &mh->msg_iov[i];
        const uint8_t* p = (const uint8_t*)v->iov_base;
        if (v->iov_len && !lnx_r_ok((uintptr_t)p, v->iov_len)) return -LNX_EFAULT;
        for (uint64_t k = 0; k < v->iov_len && total < sizeof buf; k++)
            buf[total++] = p[k];
    }
    /* Same routing on the way out — but a UNIX socket may also be carrying a
     * DESCRIPTOR in its ancillary data (a Wayland client's shm pool). */
    if (sys_fd_kind(fd) != FDK_NETSOCK) {
        int passfd = lnx_cmsg_take_fd(mh->msg_control,
                                      (unsigned long)mh->msg_controllen);
        if (passfd >= 0) return sys_send_k(fd, buf, total, passfd);
        return sys_write_k(fd, buf, total);
    }

    if (mh->msg_name) {                              /* datagram to a peer */
        uint32_t ip; int port;
        if (sockaddr_to_hostorder((const struct lnx_sockaddr_in*)mh->msg_name,
                                  &ip, &port) != 0)
            return -LNX_EAFNOSUPPORT;
        return sys_sendto_k(fd, buf, total, ip, port);
    }
    return sys_write_k(fd, buf, total);              /* connected stream */
}

/* fcntl(fd, cmd, arg) — only the status-flag commands do real work. */
#define LNX_F_DUPFD          0
#define LNX_F_DUPFD_CLOEXEC 1030
#define LNX_F_GETFL    3
#define LNX_F_SETFL    4
#define LNX_O_NONBLOCK 04000



/* End a Linux process/excursion (identical flow to the native SYS_EXIT). */
static void linux_exit(struct int_frame* f, int code) {
    struct task* cur = task_current();
    if (cur && cur->user_task) {
        fd_close_all();
        task_exit_code(code);
    }
    user_excursion_exit(code);   /* #11: the task's own resume point */
    (void)f;
}

static void linux_syscall_body(struct int_frame* f);

/* §1.1 — arm the RING-3 POINTER GATE for the duration, exactly like the native
 * dispatcher does.  It never was armed here, which silently disabled the first
 * of §M46's three boundary layers for the ENTIRE musl userland — every
 * coreutil, the shell, NetSurf, the TLS stack — because the gated sys_*
 * wrappers fall through to their ungated `_k` cores when the flag is clear.
 * Everything in this file that legitimately hands a KERNEL buffer to a sys_*
 * must therefore call the `_k` core (linux_sendmsg) or the `_u` split
 * (recvmsg/recvfrom) — the same discipline the native path already follows. */
void linux_syscall_dispatch(struct int_frame* f) {
    struct task* me = task_current();
    int prev = me ? me->in_user_syscall : 0;
    if (me) me->in_user_syscall = 1;
    linux_syscall_body(f);
    if (me) me->in_user_syscall = prev;
}

static void linux_syscall_body(struct int_frame* f) {
    /* SysV/Linux x86_64 argument registers. */
    uint64_t a0 = f->rdi, a1 = f->rsi, a2 = f->rdx;
    uint64_t a3 = f->r10, a4 = f->r8,  a5 = f->r9;
    (void)a4; (void)a5;

    /* §M50 — THE ARCH SHIM.  Everything above this point is the only
     * architecture-specific part of a syscall translation: which registers
     * carry the number and the arguments.  Below it, the engine answers from a
     * table shared with every other arch and guest ABI (kernel/core/abi_*.c).
     *
     * It is consulted FIRST and may decline: an operation the vocabulary does
     * not name yet falls through to the hand-written switch below.  That is
     * what lets 1000 lines of it migrate one operation at a time, with the old
     * path still there to compare against, instead of in one unverifiable
     * jump. */
    {
        long r;
        if (abi_dispatch(&abi_map_linux_amd64, f->rax, a0, a1, a2, a3, a4, a5, &r)) {
            f->rax = (uint64_t)r;
            return;
        }
    }

    switch (f->rax) {
        case LNX_exit_group:                     /* §M90 — the whole process */
            if (task_current() && task_current()->user_task) task_exit_group((int)a0);
            linux_exit(f, (int)a0);              /* an excursion: teleport back */
            return;
        case LNX_exit:
            linux_exit(f, (int)a0);                 /* never returns */
            return;

        case LNX_write:
            f->rax = (uint64_t)sys_write((int)a0, (const void*)a1, (size_t)a2);
            return;
        case LNX_read:
            f->rax = (uint64_t)sys_read((int)a0, (void*)a1, (size_t)a2);
            return;

        case LNX_writev: {
            const struct lnx_iovec* iov = (const struct lnx_iovec*)a1;
            int cnt = (int)a2;
            long total = 0;
            for (int i = 0; i < cnt && iov; i++) {
                long w = sys_write((int)a0, iov[i].iov_base, (size_t)iov[i].iov_len);
                if (w < 0) { total = total ? total : w; break; }
                total += w;
            }
            f->rax = (uint64_t)total;
            return;
        }
        case LNX_readv: {
            const struct lnx_iovec* iov = (const struct lnx_iovec*)a1;
            int cnt = (int)a2;
            long total = 0;
            for (int i = 0; i < cnt && iov; i++) {
                long r = sys_read((int)a0, iov[i].iov_base, (size_t)iov[i].iov_len);
                if (r < 0) { total = total ? total : r; break; }
                total += r;
                if ((uint64_t)r < iov[i].iov_len) break;   /* short read → done */
            }
            f->rax = (uint64_t)total;
            return;
        }

        case LNX_close:
            f->rax = (uint64_t)sys_close((int)a0);
            return;
        case LNX_lseek:
            f->rax = (uint64_t)sys_lseek((int)a0, (long)a1, (int)a2);
            return;


        /* mmap / munmap / mprotect, pread64 / pwrite64 are ABI-engine
         * operations (§M89). */
        case LNX_brk:
            /* No program break → report 0 so musl's malloc falls back to mmap. */
            f->rax = 0;
            return;

        case LNX_arch_prctl: {
            /* musl's __init_tls sets the thread pointer via ARCH_SET_FS.  On
             * x86_64, TLS is the FS.base MSR (hal_set_tls_base), recorded on the
             * task so the scheduler restores it on every switch. */
            struct task* t = task_current();
            if ((int)a0 == ARCH_SET_FS) {
                if (t) { t->tls_base = (uintptr_t)a1; t->has_tls = 1; }
                hal_set_tls_base((uintptr_t)a1);
                f->rax = 0;
            } else if ((int)a0 == ARCH_GET_FS) {
                if (a1) {                       /* §1.1 — client out-slot */
                    if (!lnx_w_ok(a1, sizeof(uint64_t))) {
                        f->rax = (uint64_t)-LNX_EFAULT; return;
                    }
                    *(uint64_t*)a1 = t ? (uint64_t)t->tls_base : 0;
                }
                f->rax = 0;
            } else {
                f->rax = (uint64_t)-LNX_ENOSYS;      /* GS unused by musl TLS */
            }
            return;
        }

        case LNX_set_tid_address:
        case LNX_gettid:
        case LNX_getpid:
            f->rax = (uint64_t)(task_current() ? task_current()->pid : 0);
            return;

        /* set_robust_list, membarrier, memfd_create, ftruncate, getrandom,
         * sched_{get,set}affinity, madvise, mincore and sched_yield are
         * ABI-engine operations now (§M89 rung 3): they lived here and in the
         * i386 switch only, so arm64 had none of them. */
        case LNX_ioctl:
            /* ENOTTY (not ENOSYS) → musl's isatty() reports "not a terminal". */
            f->rax = (uint64_t)-LNX_ENOTTY;
            return;




        /* §M42 display bridge — a ring-3 graphical client (NetSurf's libnsfb
         * "dos" surface) drives a WM window through these.  Buffer/event
         * pointers are in the caller's address space (active now), read/written
         * directly.  See kernel/gui/dosgui.c. */
        case LNX_DOSGUI_CREATE:
            {   /* §1.1 — copy the client's window title in. */
                char title[64];
                if (copy_str_from_user(title, a2, sizeof title) < 0) title[0] = 0;
                f->rax = (uint64_t)(int64_t)dosgui_create((int)a0, (int)a1, title);
            }
            return;
        case LNX_DOSGUI_PRESENT:
            f->rax = (uint64_t)(int64_t)dosgui_present((int)a0, (const uint32_t*)a1,
                                                       (int)a2, (int)a3, (int)a4);
            return;
        case LNX_DOSGUI_POLL:
            f->rax = (uint64_t)(int64_t)dosgui_poll((int)a0, (struct dosgui_event*)a1);
            return;
        case LNX_DOSGUI_DESTROY:
            dosgui_destroy((int)a0);
            f->rax = 0;
            return;
        /* ---- Phase 3: process model (fork/execve/waitpid/pipe/dup2) ------- */
        case LNX_vfork:     /* §M73 — a fork is a correct vfork; busybox sh uses it */
        case LNX_fork: {
            /* Snapshot the full user register file; the child resumes here with
             * rax = 0 (proc_fork sets it) via enter_user_mode_regs.  musl's
             * fork() on x86_64 uses SYS_fork directly. */
            struct user_regs r;
            r.rax = 0;
            r.rbx = f->rbx; r.rcx = f->rcx; r.rdx = f->rdx;
            r.rsi = f->rsi; r.rdi = f->rdi; r.rbp = f->rbp;
            r.r8 = f->r8; r.r9 = f->r9; r.r10 = f->r10; r.r11 = f->r11;
            r.r12 = f->r12; r.r13 = f->r13; r.r14 = f->r14; r.r15 = f->r15;
            r.rip = f->rip; r.rflags = f->rflags; r.user_sp = f->rsp;
            f->rax = (uint64_t)proc_fork(&r);
            return;
        }
        case LNX_execve:
            /* execve(path=rdi, argv=rsi, envp=rdx) — §M89: envp honoured.  On
             * success does not return (iretq into the new image); on failure
             * the old image continues. */
            f->rax = (uint64_t)(long)proc_execve_env((const char*)a0, (char* const*)a1,
                                                     (char* const*)a2);
            return;
        case LNX_wait4: {
            int code = 0;
            int pid = task_wait((int)a0, &code);
            if (a1) {                            /* §1.1 — client status slot */
                if (!lnx_w_ok(a1, sizeof(int))) { f->rax = (uint64_t)-LNX_EFAULT; return; }
                *(int*)a1 = (code & 0xFF) << 8;  /* WIFEXITED: code in 8..15 */
            }
            f->rax = (uint64_t)pid;
            return;
        }
        /* pipe / pipe2 moved to the shared engine (§M59, abi_engine.c). */
        /* kill / tkill / tgkill are engine operations (§M89, lnx_signal.c). */
        case 15:                                  /* rt_sigreturn (§M89) */
            lnx_rt_sigreturn(f);
            return;
        /* futex is an ABI-engine operation (§M89, futex.c). */

        case LNX_poll:
            /* poll(fds=rdi, nfds=rsi, timeout_ms=rdx).  Linux's struct pollfd is
             * byte-identical to ours and POLLIN/POLLOUT share values, so the
             * array goes straight to sys_poll.  Without this musl's resolver
             * spun on -ENOSYS and getaddrinfo never completed. */
            f->rax = (uint64_t)sys_poll((struct pollfd*)a0, (int)a1, (int)a2);
            return;

        /* ---- BSD sockets.
         *
         * §M24 MOVED THIS OUT.  socket/bind/connect/listen/accept/accept4/
         * getsockname/getpeername/sendto/recvfrom/shutdown/set-getsockopt are
         * canonical operations now (kernel/core/abi_engine.c), reached through
         * this arch's number map — so the engine above answers them and the
         * cases that used to be here are GONE rather than left as a shadow.
         * §M56.1's lesson, applied on purpose: a superseded fallback that is
         * still reachable is a second implementation nobody is testing, and
         * the last one of those worked on one arch and was silently dead on
         * two.
         *
         * sendmsg/recvmsg stay: they are about CONTROL MESSAGES and passing
         * file descriptors (SCM_RIGHTS, which Wayland runs on), and their
         * `struct msghdr` is a row of guest-width words — a different
         * marshalling problem from a fixed 16-byte sockaddr, and one that has
         * not been solved once yet. */
        case LNX_sendmsg:
            f->rax = (uint64_t)linux_sendmsg((int)a0, (const struct lnx_msghdr*)a1,
                                             (int)a2);
            return;
        case LNX_recvmsg:
            f->rax = (uint64_t)linux_recvmsg((int)a0, (struct lnx_msghdr*)a1, (int)a2);
            return;

        /* §M40 — clone().  amd64: clone(flags, stack, ptid, ctid, tls).
         *
         * Only the THREAD shape is served (CLONE_VM|CLONE_THREAD), which is
         * what musl's pthread_create issues; a clone without CLONE_VM is a
         * fork and is routed there so both spellings work.  musl's __clone has
         * already laid the start function and its argument on the new stack and
         * expects the child to resume at the same instruction with rax = 0. */
        case LNX_clone: {
            unsigned long flags = (unsigned long)a0;
            if (!(flags & LNX_CLONE_VM)) {          /* a fork in disguise */
                struct user_regs r;
                r.rax = 0;
                r.rbx = f->rbx; r.rcx = f->rcx; r.rdx = f->rdx;
                r.rsi = f->rsi; r.rdi = f->rdi; r.rbp = f->rbp;
                r.r8  = f->r8;  r.r9  = f->r9;  r.r10 = f->r10; r.r11 = f->r11;
                r.r12 = f->r12; r.r13 = f->r13; r.r14 = f->r14; r.r15 = f->r15;
                r.rip = f->rip; r.rflags = f->rflags; r.user_sp = f->rsp;
                /* §M90 — every flag honoured or refused (proc_clone_fork);
                 * amd64: (flags, stack, ptid, ctid, tls). */
                f->rax = (uint64_t)proc_clone_fork(&r, flags, (uintptr_t)a1,
                                                   (uintptr_t)a2, (uintptr_t)a3);
                return;
            }
            if (!a1) { f->rax = (uint64_t)-LNX_EINVAL; return; }

            struct user_regs r;
            r.rax = 0;
            r.rbx = f->rbx; r.rcx = f->rcx; r.rdx = f->rdx;
            r.rsi = f->rsi; r.rdi = f->rdi; r.rbp = f->rbp;
            r.r8  = f->r8;  r.r9  = f->r9;  r.r10 = f->r10; r.r11 = f->r11;
            r.r12 = f->r12; r.r13 = f->r13; r.r14 = f->r14; r.r15 = f->r15;
            r.rip = f->rip; r.rflags = f->rflags; r.user_sp = f->rsp;

            /* ctid lives in the SHARED address space, so the pointer stays
             * valid for the child's whole life — the kernel keeps it and
             * zeroes it at exit (see task_exit_code). */
            int* ctid = (flags & LNX_CLONE_CHILD_CLEARTID) ? (int*)a3 : NULL;
            if (ctid && !lnx_w_ok((uintptr_t)ctid, sizeof(int))) {
                f->rax = (uint64_t)-LNX_EFAULT; return;
            }
            int tid = proc_clone_thread(&r, (uintptr_t)a1, (uintptr_t)a4, ctid);
            if (tid < 0) { f->rax = (uint64_t)-LNX_EINVAL; return; }
            if ((flags & LNX_CLONE_PARENT_SETTID) && a2) {
                if (lnx_w_ok(a2, sizeof(int))) *(int*)a2 = tid;
            }
            if ((flags & LNX_CLONE_CHILD_SETTID) && ctid) *ctid = tid;
            f->rax = (uint64_t)tid;
            return;
        }

        default:
            kprintf("linux-abi64: unhandled syscall %lu (returning -ENOSYS)\n",
                    (unsigned long)f->rax);
            f->rax = (uint64_t)-LNX_ENOSYS;
            return;
    }
}
