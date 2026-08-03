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

#include "syscall.h"
#include "idt.h"
#include "task.h"
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
extern uint64_t saved_rsp;
extern uint64_t saved_rip;

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

static int linux_open_flags(int lf) {
    int vf;
    switch (lf & LO_ACCMODE) {
        case LO_WRONLY: vf = VFS_WRONLY; break;
        case LO_RDWR:   vf = VFS_RDWR;   break;
        default:        vf = VFS_RDONLY; break;
    }
    if (lf & LO_CREAT) vf |= VFS_CREATE;
    if (lf & LO_TRUNC) vf |= VFS_TRUNC;
    return vf;
}

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

/* Linux x86_64 `struct stat` (asm/stat.h).  Note the field order differs from
 * i386's stat64: st_nlink precedes st_mode, and every slot is 64-bit.  ld.so's
 * map_library fstats a .so to learn st_size before mmapping it. */
struct lnx_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime,  st_atime_nsec;
    uint64_t st_mtime,  st_mtime_nsec;
    uint64_t st_ctime,  st_ctime_nsec;
    int64_t  __unused[3];
};

#define LNX_S_IFREG 0100000u
#define LNX_S_IFDIR 0040000u

static void fill_stat(struct lnx_stat* s, const struct kstat* k) {
    for (unsigned i = 0; i < sizeof *s; i++) ((uint8_t*)s)[i] = 0;
    s->st_mode    = (k->type == 1 /*INODE_DIR*/ ? LNX_S_IFDIR : LNX_S_IFREG) | 0755u;
    s->st_nlink   = 1;
    s->st_size    = (int64_t)(uint32_t)k->size;
    s->st_blksize = 4096;
    s->st_blocks  = ((int64_t)(uint32_t)k->size + 511) / 512;
    s->st_dev     = 1;
    s->st_ino     = (uint64_t)(uint32_t)k->size + 1;   /* crude stable-per-file id */
}

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

static long linux_fcntl(int fd, int cmd, long arg) {
    /* F_DUPFD(_CLOEXEC) must really duplicate.  libwayland dups every descriptor
     * it sends, and an fcntl that "succeeds" with 0 is indistinguishable from a
     * dup to fd 0 — exactly how a Wayland client's shm pool silently arrived
     * carrying descriptor zero. */
    if (cmd == LNX_F_DUPFD || cmd == LNX_F_DUPFD_CLOEXEC) {
        int nfd = sys_dupfd(fd, (int)arg);
        return (nfd < 0) ? -LNX_EINVAL : nfd;
    }
    if (cmd == LNX_F_SETFL) {
        sys_socket_setnonblock(fd, (arg & LNX_O_NONBLOCK) ? 1 : 0);
        return 0;                          /* not a socket → accept and ignore */
    }
    if (cmd == LNX_F_GETFL) {
        int nb = sys_socket_getnonblock(fd);
        return (nb > 0) ? LNX_O_NONBLOCK : 0;
    }
    return 0;                              /* CLOEXEC etc: accepted, untracked */
}

static int lnx_stat_upath(uintptr_t upath, struct kstat* out) {
    char kp[256];
    if (copy_str_from_user(kp, upath, sizeof kp) < 0) return -1;
    return sys_stat_k(kp, out);
}
static int lnx_put_stat(uintptr_t ustat, const struct kstat* k) {
    if (!lnx_w_ok(ustat, sizeof(struct lnx_stat))) return -1;
    fill_stat((struct lnx_stat*)ustat, k);
    return 0;
}

/* End a Linux process/excursion (identical flow to the native SYS_EXIT). */
static void linux_exit(struct int_frame* f, int code) {
    struct task* cur = task_current();
    if (cur && cur->user_task) {
        fd_close_all();
        task_exit_code(code);
    }
    hal_syscall_exit_to_kernel((uintptr_t)saved_rsp, (uintptr_t)saved_rip);
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

    switch (f->rax) {
        case LNX_exit:
        case LNX_exit_group:
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

        case LNX_open: {
            long r = sys_open((const char*)a0, linux_open_flags((int)a1));
            f->rax = (r < 0) ? (uint64_t)-LNX_ENOENT : (uint64_t)r;
            return;
        }
        case LNX_openat: {
            if ((int)a0 != LAT_FDCWD) { f->rax = (uint64_t)-LNX_ENOSYS; return; }
            long r = sys_open((const char*)a1, linux_open_flags((int)a2));
            f->rax = (r < 0) ? (uint64_t)-LNX_ENOENT : (uint64_t)r;
            return;
        }
        case LNX_close:
            f->rax = (uint64_t)sys_close((int)a0);
            return;
        case LNX_lseek:
            f->rax = (uint64_t)sys_lseek((int)a0, (long)a1, (int)a2);
            return;

        case LNX_pread64: {
            /* pread64(fd, buf, count, offset) — positioned read that must NOT
             * disturb the fd offset (musl's ld.so dlopen path reads .so headers
             * this way).  Save/seek/read/restore around the plain fd cursor. */
            int fd = (int)a0;
            long cur = sys_lseek(fd, 0, 1 /*SEEK_CUR*/);
            sys_lseek(fd, (long)a3, 0 /*SEEK_SET*/);
            long r = sys_read(fd, (void*)a1, (size_t)a2);
            if (cur >= 0) sys_lseek(fd, cur, 0 /*SEEK_SET*/);
            f->rax = (uint64_t)r;
            return;
        }
        case LNX_pwrite64: {
            int fd = (int)a0;
            long cur = sys_lseek(fd, 0, 1);
            sys_lseek(fd, (long)a3, 0);
            long r = sys_write(fd, (const void*)a1, (size_t)a2);
            if (cur >= 0) sys_lseek(fd, cur, 0);
            f->rax = (uint64_t)r;
            return;
        }
        case LNX_unlink: {
            /* §1.1 — copy the client path in before the VFS sees it. */
            char kp[256];
            if (copy_str_from_user(kp, a0, sizeof kp) < 0) {
                f->rax = (uint64_t)-LNX_EFAULT; return;
            }
            f->rax = (uint64_t)vfs_unlink(kp);
            return;
        }

        case LNX_mmap: {
            /* x86_64 mmap(addr, len, prot, flags, fd, offset) — offset is in
             * BYTES (unlike i386's mmap2 page-offset).  §M37: file-backed +
             * MAP_FIXED so ld.so can map shared objects. */
            long r = sys_mmap_full(a0, (size_t)a1, (int)a2, (int)a3,
                                   (int)a4, (uint64_t)a5);
            f->rax = (r <= 0) ? (uint64_t)-LNX_ENOMEM : (uint64_t)r;
            return;
        }
        case LNX_mprotect:
            f->rax = (uint64_t)sys_mprotect(a0, (size_t)a1, (int)a2);
            return;
        case LNX_munmap:
            /* Bump-allocated user mmap does not reclaim yet (small leak). */
            f->rax = 0;
            return;
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

        case LNX_set_robust_list:
        case LNX_rt_sigprocmask:
        case LNX_rt_sigaction:
        case LNX_membarrier:               /* UP + no reordering we care about */
        case LNX_fcntl:
            /* Best-effort success: no per-task CLOEXEC / robust futex list / fd
             * flags tracked yet, but musl's startup + stdio paths only need
             * these to "not fail".  O_NONBLOCK is the exception — it changes
             * real behaviour (see sys_socket_setnonblock), so F_SETFL/F_GETFL
             * are honoured for sockets. */
            f->rax = (uint64_t)linux_fcntl((int)a0, (int)a1, (long)a2);
            return;

        case LNX_getuid:
        case LNX_geteuid:
        case LNX_getgid:
        case LNX_getegid:
            f->rax = 0;                              /* single-user: root (0) */
            return;

        case LNX_ioctl:
            /* ENOTTY (not ENOSYS) → musl's isatty() reports "not a terminal". */
            f->rax = (uint64_t)-LNX_ENOTTY;
            return;

        case LNX_memfd_create:
            /* The NAME is advisory (Linux only uses it for /proc); we ignore it
             * and hand back a zero-length shm object, which ftruncate sizes. */
            f->rax = (uint64_t)(long)sys_memfd(0);
            return;
        case LNX_ftruncate:
            f->rax = (uint64_t)(long)sys_memfd_resize((int)a0, (size_t)a1);
            return;

        case LNX_getrandom:
            f->rax = (uint64_t)sys_getrandom((void*)a0, (size_t)a1, (unsigned)a2);
            return;

        case LNX_clock_gettime: {
            /* x86_64 timespec { long tv_sec; long tv_nsec; } (both 64-bit). */
            struct ktimespec ts;
            sys_clock_gettime_k((int)a0, &ts);
            if (!lnx_w_ok(a1, 16)) { f->rax = (uint64_t)-LNX_EFAULT; return; }
            uint64_t* p = (uint64_t*)a1;
            p[0] = ts.sec; p[1] = ts.nsec;
            f->rax = 0;
            return;
        }
        case LNX_gettimeofday: {
            struct ktimespec ts;
            sys_clock_gettime_k(CLOCK_REALTIME, &ts);
            if (!lnx_w_ok(a0, 16)) { f->rax = (uint64_t)-LNX_EFAULT; return; }
            uint64_t* p = (uint64_t*)a0;             /* {tv_sec; tv_usec} */
            p[0] = ts.sec; p[1] = ts.nsec / 1000;
            f->rax = 0;
            return;
        }

        case LNX_fstat: {
            struct kstat k;
            if (sys_fstat_k((int)a0, &k) != 0) { f->rax = (uint64_t)-LNX_ENOENT; return; }
            if (lnx_put_stat(a1, &k) != 0) { f->rax = (uint64_t)-LNX_EFAULT; return; }
            f->rax = 0;
            return;
        }
        case LNX_stat: {
            struct kstat k;
            if (lnx_stat_upath(a0, &k) != 0) { f->rax = (uint64_t)-LNX_ENOENT; return; }
            if (lnx_put_stat(a1, &k) != 0) { f->rax = (uint64_t)-LNX_EFAULT; return; }
            f->rax = 0;
            return;
        }
        case LNX_readlink:
        case LNX_readlinkat: {
            /* readlink(path, buf, sz) / readlinkat(dirfd, path, buf, sz).  The
             * d-os VFS has no symlinks, so for an existing path return -EINVAL
             * ("not a symbolic link") and -ENOENT for a missing one.  musl's
             * realpath() reads this exactly: -EINVAL → treat the component as a
             * real file/dir and keep going; -ENOENT → fail.  This unblocks the
             * realpath()-based resource + font path resolution NetSurf does. */
            const char* path = (f->rax == LNX_readlink)
                                   ? (const char*)a0    /* readlink: path=rdi */
                                   : (const char*)a1;   /* readlinkat: path=rsi */
            struct kstat k;
            f->rax = (lnx_stat_upath((uintptr_t)path, &k) != 0) ? (uint64_t)-LNX_ENOENT
                                                                : (uint64_t)-LNX_EINVAL;
            return;
        }
        case LNX_access:
        case LNX_faccessat: {
            /* access(path, mode) / faccessat(dirfd, path, mode, flags).  We do
             * not track per-file permissions yet, so treat it as an existence
             * check: 0 if the path stats OK, -ENOENT otherwise.  NetSurf probes
             * resource/font paths with access() before opening them. */
            const char* path = (f->rax == LNX_access)
                                   ? (const char*)a0    /* access: path=rdi */
                                   : (const char*)a1;   /* faccessat: path=rsi */
            struct kstat k;
            f->rax = (lnx_stat_upath((uintptr_t)path, &k) != 0) ? (uint64_t)-LNX_ENOENT : 0;
            return;
        }
        case LNX_sched_setaffinity:
            f->rax = 0;                    /* accepted; d-os schedules its own */
            return;
        case LNX_sched_getaffinity: {
            /* getaffinity(pid, cpusetsize, mask) → BYTES written, mask filled.
             * Report the CPUs this task may actually run on; a caller that gets
             * an empty or error answer concludes there are zero CPUs. */
            size_t cap = (size_t)a1;
            if (!a2 || cap < sizeof(unsigned long)) {
                f->rax = (uint64_t)-LNX_EINVAL; return;
            }
            if (!lnx_w_ok(a2, sizeof(unsigned long))) {
                f->rax = (uint64_t)-LNX_EFAULT; return;
            }
            int n = smp_ncpus();
            if (n <= 0) n = 1;
            if (n > 64) n = 64;
            unsigned long m = (n >= 64) ? ~0UL : ((1UL << n) - 1UL);
            *(unsigned long*)a2 = m;
            f->rax = sizeof(unsigned long);
            return;
        }
        case LNX_mincore:      /* Mesa probes residency; "not resident" is fine */
        case LNX_madvise:
            /* Purely advisory (MADV_*) — safe to accept and ignore. */
            f->rax = 0;
            return;
        case LNX_nanosleep:
        case LNX_clock_nanosleep: {
            /* Yield for the requested time — NetSurf's fb event loop sleeps here
             * when idle (else it busy-spins and starves the compositor).
             * timespec {s64 tv_sec; s64 tv_nsec}: nanosleep req = rdi,
             * clock_nanosleep req = rdx (after clockid + flags). */
            uintptr_t ureq = (f->rax == LNX_nanosleep) ? a0 : a2;
            if (ureq && !lnx_r_ok(ureq, 2 * sizeof(int64_t))) {   /* §1.1 */
                f->rax = (uint64_t)-LNX_EFAULT; return;
            }
            const int64_t* req = (const int64_t*)ureq;
            long ms = req ? (long)(req[0] * 1000 + req[1] / 1000000) : 0;
            task_msleep(ms > 0 ? (uint32_t)ms : 1u);
            f->rax = 0;
            return;
        }
        case LNX_sched_yield:
            task_msleep(1);                          /* cooperative yield */
            f->rax = 0;
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
        case LNX_getdents64:
            f->rax = (uint64_t)sys_getdents64((int)a0, (void*)a1, (size_t)a2);
            return;
        case LNX_uname:
            f->rax = (uint64_t)sys_uname((struct kutsname*)a0);
            return;

        /* ---- Phase 3: process model (fork/execve/waitpid/pipe/dup2) ------- */
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
            /* execve(path=rdi, argv=rsi, envp=rdx) — envp ignored (child keeps
             * the default env).  On success does not return (iretq into the new
             * image); on failure the old image continues. */
            f->rax = (uint64_t)proc_execve((const char*)a0, (char* const*)a1);
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
        case LNX_pipe:
        case LNX_pipe2:
            /* pipe(fds=rdi) / pipe2(fds=rdi, flags=rsi) — flags (CLOEXEC/
             * NONBLOCK) not tracked yet; the fd pair is what matters. */
            f->rax = (uint64_t)sys_pipe((int*)a0);
            return;
        case LNX_dup2:
            f->rax = (uint64_t)sys_dup2((int)a0, (int)a1);
            return;
        case LNX_kill:
            /* Posts the signal (sys_kill sets sig_pending); actual delivery to
             * user handlers on x86_64 is a follow-up (needs the Linux rt_sigframe
             * / ucontext layout).  Enough for waitpid-based job control. */
            f->rax = (uint64_t)sys_kill((int)a0, (int)a1);
            return;
        case LNX_futex:
            f->rax = (uint64_t)sys_futex((int*)a0, (int)a1, (int)a2);
            return;

        case LNX_poll:
            /* poll(fds=rdi, nfds=rsi, timeout_ms=rdx).  Linux's struct pollfd is
             * byte-identical to ours and POLLIN/POLLOUT share values, so the
             * array goes straight to sys_poll.  Without this musl's resolver
             * spun on -ENOSYS and getaddrinfo never completed. */
            f->rax = (uint64_t)sys_poll((struct pollfd*)a0, (int)a1, (int)a2);
            return;

        /* ---- BSD sockets.  Unlike i386 (one multiplexed socketcall), amd64
         * Linux gives each call its own number, so these translate directly to
         * the M24 primitives.  Byte order is converted in the sockaddr helpers
         * above — the M24 stack only ever sees host order. */
        case LNX_socket: {
            int domain = (int)a0;
            int type   = (int)a1 & 0xFF;      /* strip SOCK_CLOEXEC/NONBLOCK */
            if (domain != AF_INET_LNX) { f->rax = (uint64_t)-LNX_EAFNOSUPPORT; return; }
            int fd = sys_socket(domain, type, (int)a2);
            if (fd < 0) { f->rax = (uint64_t)-LNX_EOPNOTSUPP; return; }
            /* SOCK_NONBLOCK is NOT decoration: musl's resolver drains its
             * socket with `while (recvmsg(...) >= 0)` and needs the EAGAIN that
             * only a non-blocking socket produces. */
            if ((int)a1 & LSOCK_NONBLOCK) sys_socket_setnonblock(fd, 1);
            f->rax = (uint64_t)fd;
            return;
        }
        case LNX_bind: {
            uint32_t ip; int port;
            if (sockaddr_to_hostorder((const struct lnx_sockaddr_in*)a1, &ip, &port) != 0) {
                f->rax = (uint64_t)-LNX_EAFNOSUPPORT; return;
            }
            f->rax = (uint64_t)(long)(sys_bind((int)a0, port) == 0 ? 0 : -1);
            return;
        }
        case LNX_connect: {
            uint32_t ip; int port;
            if (sockaddr_to_hostorder((const struct lnx_sockaddr_in*)a1, &ip, &port) != 0) {
                f->rax = (uint64_t)-LNX_EAFNOSUPPORT; return;
            }
            f->rax = (uint64_t)(long)(sys_connect((int)a0, ip, port) == 0 ? 0 : -1);
            return;
        }
        case LNX_sendto: {
            const struct lnx_sockaddr_in* dst = (const struct lnx_sockaddr_in*)a4;
            if (!dst) {                       /* connected stream → plain write */
                f->rax = (uint64_t)sys_write((int)a0, (const void*)a1, (size_t)a2);
                return;
            }
            uint32_t ip; int port;
            if (sockaddr_to_hostorder(dst, &ip, &port) != 0) {
                f->rax = (uint64_t)-LNX_EAFNOSUPPORT; return;
            }
            f->rax = (uint64_t)sys_sendto((int)a0, (const void*)a1, (size_t)a2, ip, port);
            return;
        }
        case LNX_recvfrom: {
            struct lnx_sockaddr_in* src = (struct lnx_sockaddr_in*)a4;
            uint32_t* uaddrlen = (uint32_t*)a5;
            uint32_t ip = 0; int port = 0;
            long n = sys_recvfrom_u((int)a0, a1, (size_t)a2, &ip, &port);
            if (n >= 0 && src) {
                /* HERE the socklen_t is the CLIENT's, so it is validated here —
                 * the helper itself takes a kernel word (see its header). */
                uint32_t room = (uint32_t)sizeof(struct lnx_sockaddr_in);
                if (uaddrlen) {
                    if (!lnx_w_ok((uintptr_t)uaddrlen, sizeof *uaddrlen)) {
                        f->rax = (uint64_t)-LNX_EFAULT; return;
                    }
                    room = *uaddrlen;
                }
                hostorder_to_sockaddr(src, &room, ip, port);
                if (uaddrlen) *uaddrlen = room;
            }
            f->rax = (uint64_t)n;
            return;
        }
        case LNX_sendmsg:
            f->rax = (uint64_t)linux_sendmsg((int)a0, (const struct lnx_msghdr*)a1,
                                             (int)a2);
            return;
        case LNX_recvmsg:
            f->rax = (uint64_t)linux_recvmsg((int)a0, (struct lnx_msghdr*)a1, (int)a2);
            return;
        case LNX_shutdown:
            f->rax = 0;                       /* close() does the teardown */
            return;
        case LNX_setsockopt:
        case LNX_getsockopt:
            /* No socket options are honoured yet; report success so musl's
             * getaddrinfo/TLS setup (SO_RCVTIMEO, TCP_NODELAY…) proceeds. */
            f->rax = 0;
            return;
        case LNX_listen:
        case LNX_getsockname:
        case LNX_getpeername:
            f->rax = (uint64_t)-LNX_EOPNOTSUPP;
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
                f->rax = (uint64_t)proc_fork(&r);
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
