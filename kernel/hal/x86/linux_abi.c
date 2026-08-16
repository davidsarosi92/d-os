/* =============================================================================
 * linux_abi.c — Linux i386 syscall-ABI compatibility layer (M36 / §M41), i386.
 *
 * The MODULAR way to run an UNMODIFIED musl/Linux binary: keep musl pristine
 * (a vendored external dependency) and have d-os provide the Linux i386 system-
 * call ABI it expects.  A process marked with the Linux personality
 * (`task->linux_abi`, set at exec time) traps `int 0x80` with LINUX syscall
 * numbers + Linux struct layouts; the d-os dispatcher (hal/x86/syscall.c)
 * routes such a process here instead of the native path, and this module
 * translates each Linux call to a d-os primitive (usyscall.c etc.).
 *
 * Kept deliberately isolated: the native d-os ABI is untouched, the two
 * personalities coexist, and this file is the single place the Linux number
 * space + struct translations live — so growing it toward "musl runs" is
 * additive and does not entangle the rest of the kernel.
 *
 * Reference: Linux i386 syscall table (arch/x86/entry/syscalls/syscall_32.tbl),
 * ABi: eax = number, ebx/ecx/edx/esi/edi/ebp = args, return in eax.
 * ============================================================================= */

#include "syscall.h"
#include "idt.h"
#include "task.h"
#include "abi.h"      /* §M50 — the shared guest-ABI translation engine */
#include "printf.h"
#include "hal_api.h"
#include "gdt.h"          /* gdt_tls_selector — the ring-3 %gs selector */
#include "percpu.h"       /* this_cpu_id — TLS descriptor is per-CPU     */
#include "vfs.h"          /* VFS_* open flags — target of the translation */
#include "proc.h"         /* proc_fork / proc_execve                      */
#include "vmm.h"          /* §1.1 — vmm_user_access_ok for the iovec array */
#include "usermode.h"     /* struct user_regs (fork)                      */
#include "dosgui.h"       /* §M42 display bridge (create/present/poll)    */
#include <stdint.h>
#include <stddef.h>

/* Excursion teleport-back (shared with the native path, usermode.s). */
extern uint32_t saved_esp;
extern uint32_t saved_eip;

/* Linux i386 syscall numbers we understand (grows toward the musl-required
 * set).  Anything else returns -ENOSYS and is logged once. */
#define LNX_exit             1
#define LNX_fork             2
#define LNX_clone         120
#define LNX_futex         240
/* clone() flag bits we care about (linux/sched.h). */
#define LNX_CLONE_VM                0x00000100
#define LNX_CLONE_PARENT_SETTID     0x00100000
#define LNX_CLONE_CHILD_CLEARTID    0x00200000
#define LNX_CLONE_CHILD_SETTID      0x01000000
#define LNX_read             3
#define LNX_write            4
#define LNX_open             5
#define LNX_close            6
#define LNX_waitpid          7
#define LNX_execve          11
#define LNX_wait4          114
#define LNX_getpid          20
#define LNX_ioctl           54
#define LNX_brk             45
#define LNX_munmap          91
#define LNX_mprotect       125
#define LNX_readv          145
#define LNX_writev         146
/* §M56 — rt_sigprocmask is handled by the ABI engine now, not by the switch
 * below.  The stub that used to live here answered "success" and did nothing,
 * and because the engine only handles numbers present in the guest's map, it
 * silently shadowed the real handler on both x86 guests while arm64 — whose
 * map did name the number — got the working one.  A fallback is only a
 * fallback while nothing better exists; when something better arrives, the
 * fallback has to be removed in the same change. */
#define LNX_rt_sigprocmask 175
#define LNX_mmap2          192
#define LNX_fstat64        197
#define LNX_set_thread_area 243
#define LNX_unlink          10
#define LNX_lseek           19
#define LNX__llseek        140
#define LNX_time            13
#define LNX_gettimeofday    78
#define LNX_clock_gettime  265
#define LNX_clock_gettime64 403
#define LNX_getrandom      355
/* §M40 — a Wayland client's shm pool: memfd_create gives a zero-length object,
 * ftruncate sizes it, then the fd travels over SCM_RIGHTS. */
#define LNX_ftruncate       93
#define LNX_ftruncate64    194
#define LNX_memfd_create   356
#define LNX_exit_group     252
#define LNX_getdents64     220
#define LNX_fcntl64        221
#define LNX_fcntl           55   /* the pre-LFS variant musl still uses */
#define LNX_set_tid_address 258
#define LNX_openat         295
#define LNX_poll           168   /* musl's DNS resolver waits on the UDP socket
                                  * with poll(); i386 struct pollfd == ours.    */
#define LNX_socketcall     102   /* §M39 3b: musl i386 routes ALL BSD-socket
                                  * ops through this multiplexer (SYS_SOCKET…). */
/* Direct i386 socket syscalls (Linux 4.3+).  musl PREFERS these and only falls
 * back to socketcall on -ENOSYS, so handling them avoids a wasted failing
 * syscall (and a misleading "unhandled" log line) per socket op. */
#define LNX_socket         359
#define LNX_bind           361
#define LNX_connect        362
#define LNX_getsockopt     365
#define LNX_setsockopt     366
#define LNX_getsockname    367
#define LNX_getpeername    368
#define LNX_sendto         369
#define LNX_sendmsg        370
#define LNX_recvfrom       371
#define LNX_recvmsg        372
#define LNX_shutdown       373

#define LNX_ENOSYS  38
#define LNX_EFAULT  14
#define LNX_ENOTTY  25
#define LNX_ENOENT   2
#define LNX_EINVAL  22
#define LNX_PAGE_SIZE 4096
#define LNX_ENOMEM  12
#define LNX_EAFNOSUPPORT 97
/* §M42 — resource/font path resolution + the display bridge (see the x86_64
 * twin for the rationale).  i386 syscall numbers + custom DOSGUI numbers. */
#define LNX_access          33
#define LNX_faccessat      307
#define LNX_readlink        85
#define LNX_readlinkat     305
#define LNX_madvise        219
#define LNX_mincore        218
#define LNX_nanosleep      162
#define LNX_clock_nanosleep 267
#define LNX_sched_yield    158
#define LNX_stat64         195
#define LNX_lstat64        196
#define LNX_fstatat64      300
#define LNX_statx          383
#define LNX_newuname       122
#define LNX_rt_sigaction   174
#define LNX_DOSGUI_CREATE  0xD050
#define LNX_DOSGUI_PRESENT 0xD051
#define LNX_DOSGUI_POLL    0xD052
#define LNX_DOSGUI_DESTROY 0xD053
#define LNX_EOPNOTSUPP   95

/* Linux socketcall sub-call numbers (linux/net.h). */
/* Type-bits Linux ORs into socket()'s `type` argument. */
#define LSOCK_NONBLOCK  0x800
#define LSOCK_CLOEXEC   0x80000

#define LSOC_SOCKET      1
#define LSOC_BIND        2
#define LSOC_CONNECT     3
#define LSOC_LISTEN      4
#define LSOC_ACCEPT      5
#define LSOC_GETSOCKNAME 6
#define LSOC_GETPEERNAME 7
#define LSOC_SOCKETPAIR  8
#define LSOC_SEND        9
#define LSOC_RECV        10
#define LSOC_SENDTO      11
#define LSOC_RECVFROM    12
#define LSOC_SHUTDOWN    13
#define LSOC_SETSOCKOPT  14
#define LSOC_GETSOCKOPT  15
#define LSOC_SENDMSG     16
#define LSOC_RECVMSG     17

/* Linux `struct sockaddr_in` (netinet/in.h) — the layout musl hands to
 * connect()/sendto().  sin_addr + sin_port are in NETWORK byte order, whereas
 * the M24 socket API takes a HOST-order IPv4 int + a host-order port int, so
 * this is the single place the two representations are reconciled. */
struct lnx_sockaddr_in {
    uint16_t sin_family;     /* AF_INET == 2 */
    uint16_t sin_port;       /* network byte order */
    uint32_t sin_addr;       /* network byte order */
    uint8_t  sin_zero[8];
};

/* Linux i386 O_* open flags (asm-generic/fcntl.h).  These do NOT match d-os's
 * VFS_* bits, so LNX_open/openat must TRANSLATE, not pass raw — musl opens with
 * O_LARGEFILE|O_CLOEXEC set, which as raw VFS bits would mean create/truncate. */
#define LO_WRONLY   00000001
#define LO_RDWR     00000002
#define LO_CREAT    00000100
#define LO_TRUNC    00001000
#define LO_ACCMODE  00000003
#define LAT_FDCWD   (-100)

/* Map Linux open flags → d-os VFS_* flags (isolated here — the ONE place the
 * two flag namespaces are reconciled).  O_APPEND/O_LARGEFILE/O_CLOEXEC/… have
 * no d-os equivalent yet and are simply dropped. */
static int linux_open_flags(int lf) {
    int vf;
    switch (lf & LO_ACCMODE) {
        case LO_WRONLY: vf = VFS_WRONLY; break;
        case LO_RDWR:   vf = VFS_RDWR;   break;
        default:        vf = VFS_RDONLY; break;   /* O_RDONLY == 0 → VFS_RDONLY */
    }
    if (lf & LO_CREAT) vf |= VFS_CREATE;
    if (lf & LO_TRUNC) vf |= VFS_TRUNC;
    return vf;
}

/* Linux i386 struct iovec (for writev). */
struct lnx_iovec { void* iov_base; uint32_t iov_len; };

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


/* Linux i386 `struct msghdr` (bits/socket.h) — the argument sendmsg()/recvmsg()
 * hand us.  musl's DNS resolver (__res_msend) receives every answer via
 * recvmsg() (single-iovec, msg_name = the source sockaddr it verifies the reply
 * came from), so getaddrinfo() cannot work without this.  All fields are 32-bit
 * on i386 (pointers + size_t). */
struct lnx_msghdr {
    void*             msg_name;        /* optional source/dest sockaddr        */
    uint32_t          msg_namelen;     /* in: room; out: actual addr length    */
    struct lnx_iovec* msg_iov;         /* scatter/gather buffers               */
    uint32_t          msg_iovlen;      /* iovec count                          */
    void*             msg_control;     /* ancillary data (unused here)         */
    uint32_t          msg_controllen;
    int               msg_flags;       /* out: MSG_TRUNC/… (we report 0)       */
};

/* Linux i386 `struct stat64` (asm/stat.h) — the layout SYS_fstat64 fills and
 * musl copies from.  ld.so's map_library fstats a .so to learn its size before
 * mmapping it, so st_size (offset 44) is the field that matters; st_mode marks
 * it a regular file, st_dev/st_ino let musl dedup already-loaded objects. */
struct lnx_stat64 {
    uint64_t st_dev;        uint32_t __pad0;
    uint32_t __st_ino;      uint32_t st_mode;      uint32_t st_nlink;
    uint32_t st_uid;        uint32_t st_gid;
    uint64_t st_rdev;       uint32_t __pad3;
    int64_t  st_size;       uint32_t st_blksize;   uint64_t st_blocks;
    uint32_t st_atime;      uint32_t st_atime_nsec;
    uint32_t st_mtime;      uint32_t st_mtime_nsec;
    uint32_t st_ctime;      uint32_t st_ctime_nsec;
    uint64_t st_ino;
} __attribute__((packed));

#define LNX_S_IFREG 0100000u
#define LNX_S_IFDIR 0040000u

/* Fill a Linux stat64 from d-os's kstat (translated in one place). */
static void fill_stat64(struct lnx_stat64* s, const struct kstat* k) {
    for (unsigned i = 0; i < sizeof *s; i++) ((uint8_t*)s)[i] = 0;
    s->st_mode    = (k->type == 1 /*INODE_DIR*/ ? LNX_S_IFDIR : LNX_S_IFREG) | 0755u;
    s->st_nlink   = 1;
    s->st_size    = (int64_t)(uint32_t)k->size;
    s->st_blksize = 4096;
    s->st_blocks  = ((uint64_t)(uint32_t)k->size + 511) / 512;
    s->st_dev     = 1;
    s->__st_ino   = (uint32_t)k->size + 1;   /* crude but stable-per-file id */
    s->st_ino     = s->__st_ino;
}

/* --------------------------------------------------------------------------
 * User-pointer discipline in this dispatcher (§1.1).
 *
 * The portable sys_* handlers gate their own ring-3 pointers, but THIS file
 * also marshals results into the foreign (Linux) layout: it fills a KERNEL
 * struct and writes it out itself.  So it must (a) call the ungated sys_*_k
 * cores with those kernel structs — passing a kernel pointer to the gated
 * wrapper would be rejected as "not a user pointer" — and (b) validate every
 * ring-3 destination it writes to on its own, or a bad pointer faults the
 * kernel (→ the fault policy halts the box).  These three helpers are that
 * discipline in one place.
 * ------------------------------------------------------------------------ */
static int lnx_w_ok(uintptr_t uptr, uintptr_t len) {   /* ring-3 write target */
    return uptr && vmm_user_access_ok(uptr, len, 1);
}
static int lnx_r_ok(uintptr_t uptr, uintptr_t len) {   /* ring-3 read source  */
    return uptr && vmm_user_access_ok(uptr, len, 0);
}
/* stat() a ring-3 path: copy the string in (validated), then run the core. */
static int lnx_stat_upath(uintptr_t upath, struct kstat* out) {
    char kp[256];
    if (copy_str_from_user(kp, upath, sizeof kp) < 0) return -1;
    return sys_stat_k(kp, out);
}
/* Write a kstat out as a Linux stat64 at a validated ring-3 address. */
static int lnx_put_stat64(uintptr_t ustat, const struct kstat* k) {
    if (!lnx_w_ok(ustat, sizeof(struct lnx_stat64))) return -1;
    fill_stat64((struct lnx_stat64*)ustat, k);
    return 0;
}


/* Linux i386 `struct user_desc` (arch/x86/include/asm/ldt.h), the argument to
 * set_thread_area.  We only consume entry_number (write-back) + base_addr; the
 * segment attributes are fixed by our GDT-TLS descriptor, so limit/flags are
 * accepted and ignored. */
struct lnx_user_desc {
    uint32_t entry_number;   /* -1 on input => "allocate one, write it back" */
    uint32_t base_addr;      /* the thread's TLS pointer                     */
    uint32_t limit;
    uint32_t flags;          /* seg_32bit/contents/… bitfield word           */
};

/* Pull a host-order (ip, port) pair out of a Linux sockaddr_in.  Returns 0 on
 * success, -1 if the address is missing / not AF_INET.  ntoh conversions live
 * ONLY here (byte-order is a Linux-ABI concern, not an M24-stack concern). */
static int sockaddr_to_hostorder(const struct lnx_sockaddr_in* sa,
                                 uint32_t* ip_out, int* port_out) {
    /* §1.1 — `sa` is the client's pointer; validate before reading it. */
    if (!lnx_r_ok((uintptr_t)sa, sizeof *sa)) return -1;
    if (sa->sin_family != AF_INET) return -1;
    const uint8_t* a = (const uint8_t*)&sa->sin_addr;   /* network order bytes */
    const uint8_t* p = (const uint8_t*)&sa->sin_port;
    *ip_out   = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
                ((uint32_t)a[2] << 8)  |  (uint32_t)a[3];
    *port_out = ((int)p[0] << 8) | (int)p[1];
    return 0;
}

/* Fill a Linux sockaddr_in (network order) from a host-order (ip, port), used
 * by recvfrom to report the datagram's source.  Honours the caller's addrlen. */
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
    /* §1.1 — both `sa` and the in/out `addrlen` are client pointers.  Validate
     * addrlen first (it decides how much of `sa` we may touch), then exactly the
     * bytes we are going to write — requiring the FULL struct would wrongly
     * reject a caller that legitimately passed a shorter buffer. */
    struct lnx_sockaddr_in tmp;
    tmp.sin_family = AF_INET;
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

/* recvmsg(fd, msg, flags) — the resolver's receive path.  A UDP datagram is
 * delivered into the first iovec (musl's __res_msend uses a single iovec sized
 * >= 512); sys_recvfrom pump-polls the RX ring.  Crucially we fill msg_name with
 * the datagram's source sockaddr: musl DROPS any reply whose source doesn't
 * match a nameserver it queried, so an unfilled msg_name would fail resolution
 * silently.  MSG_TRUNC is never reported (we deliver whole datagrams). */
static long linux_recvmsg(int fd, struct lnx_msghdr* mh, int flags) {
    (void)flags;
    /* §1.1 — the msghdr itself and its iovec array are client memory. */
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
    /* Payload lands in the CLIENT's iovec, source address in our kernel locals
     * (we marshal it into the client's sockaddr below) — sys_recvfrom_u is
     * exactly that split, and bounce-buffers the payload for us. */
    long n = sys_recvfrom_u(fd, (uintptr_t)iov->iov_base, iov->iov_len, &ip, &port);
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
 * musl's TCP-fallback DNS path (2-byte length prefix + query); a connected
 * stream (no msg_name) rides sys_write, an explicit peer rides sys_sendto. */
static long linux_sendmsg(int fd, const struct lnx_msghdr* mh, int flags) {
    (void)flags;
    /* §1.1 — msghdr + iovec array + every iov_base are client pointers. */
    if (!lnx_r_ok((uintptr_t)mh, sizeof *mh)) return -LNX_EFAULT;
    if (!mh->msg_iov || mh->msg_iovlen > 1024) return -LNX_EOPNOTSUPP;
    if (!lnx_r_ok((uintptr_t)mh->msg_iov,
                  (uintptr_t)mh->msg_iovlen * sizeof(struct lnx_iovec)))
        return -LNX_EFAULT;
    uint8_t buf[1024];
    size_t total = 0;
    for (uint32_t i = 0; i < mh->msg_iovlen; i++) {
        const struct lnx_iovec* v = &mh->msg_iov[i];
        const uint8_t* p = (const uint8_t*)v->iov_base;
        if (v->iov_len && !lnx_r_ok((uintptr_t)p, v->iov_len)) return -LNX_EFAULT;
        for (uint32_t k = 0; k < v->iov_len && total < sizeof buf; k++)
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
        if (sockaddr_to_hostorder((const struct lnx_sockaddr_in*)mh->msg_name, &ip, &port) != 0)
            return -LNX_EAFNOSUPPORT;
        return sys_sendto_k(fd, buf, total, ip, port);
    }
    /* `buf` is our KERNEL gather buffer, so call the kernel-pointer cores — the
     * gated sys_write/sys_sendto would (correctly) reject a kernel address while
     * task->in_user_syscall is set. */
    return sys_write_k(fd, buf, total);              /* connected stream */
}

/* §M39 3b — Linux socketcall(call, args[]) demultiplexer.  musl on i386 funnels
 * socket()/connect()/sendto()/recv()… through syscall 102, passing the call
 * number in ebx and a pointer to an array of the call's word-sized args in ecx.
 * Each sub-call is translated to the M24 BSD-socket primitives (usyscall.c),
 * with sockaddr_in ⇄ host-order (ip,port) conversion done here.  TCP payload
 * (send/recv on a connected stream) rides sys_write/sys_read, which already
 * route FD_NETSOCK to the TCP engine. */

/* How many argument words each sub-call consumes — used to validate exactly the
 * words we are about to read out of the client's array. */
static const uint8_t socketcall_nargs[] = {
    0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3
};

/* The CORE.  `a` is a KERNEL array of already-fetched argument words; the
 * pointer arguments INSIDE it are still client pointers and are validated
 * individually below (sockaddr_to_hostorder, the sys_* gates).
 *
 * The split matters: this function has two callers with opposite pointer
 * origins — the socketcall(102) multiplexer, whose array lives in ring 3, and
 * the DIRECT socket syscalls (359+), which repack CPU registers into a kernel
 * array.  Validating the array here as a user pointer therefore rejected every
 * direct call with -EFAULT, and musl only falls back to socketcall on -ENOSYS,
 * so `socket()` simply failed and DNS died with it.  Same lesson as the
 * sys_*_k cores and linux_sendmsg (see DOCS §4.37): the user-pointer check
 * belongs where the pointer's ORIGIN is known, not in the shared core. */
static long linux_socketcall_k(int call, const uint32_t* a) {
    if (call < 0 || call >= (int)(sizeof socketcall_nargs / sizeof socketcall_nargs[0]))
        return -LNX_EINVAL;

    /* §M24 — THE DEMULTIPLEXER, and nothing else.
     *
     * i386 is the only architecture that packs the whole socket API behind one
     * syscall number, and that is a CALLING CONVENTION, not a different set of
     * operations.  So this function's entire job is to turn a sub-call number
     * into a canonical operation and hand the already-fetched argument words to
     * the same shared handler amd64 and arm64 reach directly.  What used to be
     * here — sockaddr conversion, flag stripping, the socklen dance — is in
     * kernel/core/abi_engine.c now, written once. */
    static const uint16_t op_of[] = {
        [LSOC_SOCKET]      = ABI_SOCKET,
        [LSOC_BIND]        = ABI_BIND,
        [LSOC_CONNECT]     = ABI_CONNECT,
        [LSOC_LISTEN]      = ABI_LISTEN,
        [LSOC_ACCEPT]      = ABI_ACCEPT,
        [LSOC_GETSOCKNAME] = ABI_GETSOCKNAME,
        [LSOC_GETPEERNAME] = ABI_GETPEERNAME,
        [LSOC_SEND]        = ABI_SEND,
        [LSOC_RECV]        = ABI_RECV,
        [LSOC_SENDTO]      = ABI_SENDTO,
        [LSOC_RECVFROM]    = ABI_RECVFROM,
        [LSOC_SHUTDOWN]    = ABI_SHUTDOWN,
        [LSOC_SETSOCKOPT]  = ABI_SETSOCKOPT,
        [LSOC_GETSOCKOPT]  = ABI_GETSOCKOPT,
    };
    if (call < (int)(sizeof op_of / sizeof op_of[0]) && op_of[call]) {
        struct abi_ctx c;
        for (int i = 0; i < 6; i++) c.a[i] = (unsigned long)a[i];
        c.nr  = LNX_socketcall;
        c.map = &abi_map_linux_i386;
        long out = -LNX_ENOSYS;
        if (abi_invoke((enum abi_op)op_of[call], &c, &out)) return out;
        return -LNX_ENOSYS;
    }

    switch (call) {
        /* These two stay here: they carry CONTROL MESSAGES and file
         * descriptors (SCM_RIGHTS — what Wayland runs on), whose `struct
         * msghdr` is a row of guest-width words rather than a fixed 16-byte
         * address.  That marshalling has not been solved once yet, and moving
         * it half-solved would trade a duplicated implementation for a subtly
         * wrong one. */
        case LSOC_SENDMSG:
            return linux_sendmsg((int)a[0], (const struct lnx_msghdr*)a[1], (int)a[2]);
        case LSOC_RECVMSG:
            return linux_recvmsg((int)a[0], (struct lnx_msghdr*)a[1], (int)a[2]);
        default:
            kprintf("linux-abi: unhandled socketcall %d\n", call);
            return -LNX_ENOSYS;
    }
}

/* The GATED entry: socketcall(call, args) from ring 3.  Validates the client's
 * argument array, copies it into kernel memory, then runs the core above. */
static long linux_socketcall(int call, const uint32_t* uargs) {
    if (call < 0 || call >= (int)(sizeof socketcall_nargs / sizeof socketcall_nargs[0]))
        return -LNX_EINVAL;
    uint32_t n = socketcall_nargs[call];
    if (!lnx_r_ok((uintptr_t)uargs, (uintptr_t)n * sizeof(uint32_t)))
        return -LNX_EFAULT;
    uint32_t a[8];
    for (uint32_t i = 0; i < n && i < 8; i++) a[i] = uargs[i];
    for (uint32_t i = n; i < 8; i++) a[i] = 0;
    return linux_socketcall_k(call, a);
}

/* fcntl(fd, cmd, arg).  Most commands are flag bookkeeping we do not track yet
 * and can safely report success for — but NOT the two that yield a DESCRIPTOR.
 * libwayland dups every fd it sends (wl_closure_marshal -> wl_os_dupfd_cloexec),
 * and an fcntl that "succeeds" with 0 is indistinguishable from a dup to fd 0,
 * which is exactly how a Wayland client's shm pool silently arrived carrying
 * descriptor zero.  O_NONBLOCK is honoured too, since it changes real behaviour
 * (see sys_socket_setnonblock). */
#define LNX_F_DUPFD          0
#define LNX_F_DUPFD_CLOEXEC 1030
#define LNX_F_GETFL          3
#define LNX_F_SETFL          4
#define LNX_O_NONBLOCK   04000

static long linux_fcntl(int fd, int cmd, long arg) {
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

/* End a Linux process/excursion: an independent user task exits for good;
 * an excursion teleports back to proc_exec_*'s caller (identical to the native
 * SYS_EXIT handling — the personality only changes the number, not the flow). */
static void linux_exit(struct int_frame* f, int code) {
    struct task* cur = task_current();
    if (cur && cur->user_task) {
        fd_close_all();
        task_exit_code(code);
    }
    hal_syscall_exit_to_kernel(saved_esp, saved_eip);
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
    /* §M50 — THE ARCH SHIM.  Linux/i386 passes arguments in ebx,ecx,edx,esi,
     * edi,ebp; that mapping is the ONLY architecture-specific thing about a
     * syscall translation, and it is these six lines.  Everything the engine
     * answers below is shared with x86_64 (and with any future arch) — the
     * same operations, the same handlers, a different number map.
     *
     * Note what is NOT duplicated: `read` is 3 here and 0 on amd64, and neither
     * this file nor the handler knows or cares.  That difference lives in
     * kernel/core/abi_linux.c as data. */
    {
        long r;
        if (abi_dispatch(&abi_map_linux_i386, f->eax,
                         f->ebx, f->ecx, f->edx, f->esi, f->edi, f->ebp, &r)) {
            f->eax = (uint32_t)r;
            return;
        }
    }

    switch (f->eax) {
        case LNX_exit:
        case LNX_exit_group:
            linux_exit(f, (int)f->ebx);          /* never returns */
            return;

        case LNX_write:
            f->eax = (uint32_t)sys_write((int)f->ebx, (const void*)f->ecx, f->edx);
            return;
        case LNX_read:
            f->eax = (uint32_t)sys_read((int)f->ebx, (void*)f->ecx, f->edx);
            return;

        case LNX_writev: {
            const struct lnx_iovec* iov = (const struct lnx_iovec*)f->ecx;
            int cnt = (int)f->edx;
            /* §1.1 — validate the iovec ARRAY (each iov_base is re-checked by
             * sys_write); reject an out-of-range count or a bad array pointer. */
            if (cnt < 0 || cnt > 1024 ||
                !vmm_user_access_ok((uintptr_t)iov, (uintptr_t)cnt * sizeof(*iov), 0)) {
                f->eax = (uint32_t)-LNX_EFAULT; return;
            }
            long total = 0;
            for (int i = 0; i < cnt && iov; i++) {
                long w = sys_write((int)f->ebx, iov[i].iov_base, iov[i].iov_len);
                if (w < 0) { total = (total ? total : w); break; }
                total += w;
            }
            f->eax = (uint32_t)total;
            return;
        }

        case LNX_readv: {
            /* readv(fd=ebx, iov=ecx, cnt=edx) — musl's buffered fread uses it.
             * Read each iovec; stop on error or a short read (EOF). */
            const struct lnx_iovec* iov = (const struct lnx_iovec*)f->ecx;
            int cnt = (int)f->edx;
            if (cnt < 0 || cnt > 1024 ||        /* §1.1 — validate iovec array */
                !vmm_user_access_ok((uintptr_t)iov, (uintptr_t)cnt * sizeof(*iov), 0)) {
                f->eax = (uint32_t)-LNX_EFAULT; return;
            }
            long total = 0;
            for (int i = 0; i < cnt && iov; i++) {
                long r = sys_read((int)f->ebx, iov[i].iov_base, iov[i].iov_len);
                if (r < 0) { total = (total ? total : r); break; }
                total += r;
                if ((uint32_t)r < iov[i].iov_len) break;   /* short read → done */
            }
            f->eax = (uint32_t)total;
            return;
        }

        case LNX_mprotect:
            /* §M37: real mprotect(addr=ebx, len=ecx, prot=edx).  mallocng maps a
             * PROT_NONE reservation then mprotects the used part to R/W; ld.so
             * tightens RELRO to read-only.  Must actually change PTE perms. */
            f->eax = (uint32_t)sys_mprotect(f->ebx, (size_t)f->ecx, (int)f->edx);
            return;


        case LNX_munmap:
            /* The d-os user mmap bump-allocates and does not reclaim yet, so
             * unmap is a no-op (a small leak).  Real reclaim is a follow-up. */
            f->eax = 0;
            return;

        case LNX_open: {
            /* open(path=ebx, flags=ecx, mode=edx) — translate flags, ignore mode.
             * On failure return -ENOENT (not a generic -1): musl's library search
             * loop only advances to the next candidate path on ENOENT-class
             * errors, so a generic error would abort the search. */
            long r = sys_open((const char*)f->ebx, linux_open_flags((int)f->ecx));
            f->eax = (r < 0) ? (uint32_t)-LNX_ENOENT : (uint32_t)r;
            return;
        }

        case LNX_openat: {
            /* openat(dirfd=ebx, path=ecx, flags=edx, mode=esi).  We support the
             * AT_FDCWD form (absolute paths / cwd-relative), which is what musl
             * uses for open(); a real dirfd is a follow-up. */
            int dirfd = (int)f->ebx;
            if (dirfd != LAT_FDCWD) { f->eax = (uint32_t)-LNX_ENOSYS; return; }
            long r = sys_open((const char*)f->ecx, linux_open_flags((int)f->edx));
            f->eax = (r < 0) ? (uint32_t)-LNX_ENOENT : (uint32_t)r;
            return;
        }
        case LNX_close:
            f->eax = (uint32_t)sys_close((int)f->ebx);
            return;

        case LNX_clock_gettime64: {
            /* clock_gettime64(clockid=ebx, __kernel_timespec*=ecx): s64 tv_sec@0,
             * s64 tv_nsec@8.  musl on i386 (time64) routes time()/gettimeofday
             * here — without it mbedTLS's x509 date check fatals (gmtime bad). */
            struct ktimespec ts;
            sys_clock_gettime_k((int)f->ebx, &ts);
            if (!lnx_w_ok(f->ecx, 16)) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            uint32_t* p = (uint32_t*)f->ecx;
            p[0] = ts.sec; p[1] = 0; p[2] = ts.nsec; p[3] = 0;
            f->eax = 0;
            return;
        }
        case LNX_clock_gettime: {
            /* 32-bit timespec { long tv_sec; long tv_nsec; }. */
            struct ktimespec ts;
            sys_clock_gettime_k((int)f->ebx, &ts);
            if (!lnx_w_ok(f->ecx, 8)) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            uint32_t* p = (uint32_t*)f->ecx;
            p[0] = ts.sec; p[1] = ts.nsec;
            f->eax = 0;
            return;
        }
        case LNX_gettimeofday: {
            /* struct timeval { long tv_sec; long tv_usec; } at ebx. */
            struct ktimespec ts;
            sys_clock_gettime_k(CLOCK_REALTIME, &ts);
            if (!lnx_w_ok(f->ebx, 8)) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            uint32_t* p = (uint32_t*)f->ebx;
            p[0] = ts.sec; p[1] = ts.nsec / 1000;
            f->eax = 0;
            return;
        }
        case LNX_unlink:
            /* unlink(path=ebx) — tcc removes the output file before writing.
             * §1.1 — copy the client path into the kernel before the VFS sees it. */
            {
                char kp[256];
                if (copy_str_from_user(kp, f->ebx, sizeof kp) < 0) {
                    f->eax = (uint32_t)-LNX_EFAULT; return;
                }
                f->eax = (uint32_t)vfs_unlink(kp);
            }
            return;
        case LNX_lseek:
            /* lseek(fd=ebx, offset=ecx, whence=edx) → new offset. */
            f->eax = (uint32_t)sys_lseek((int)f->ebx, (long)f->ecx, (int)f->edx);
            return;
        case LNX__llseek: {
            /* _llseek(fd=ebx, off_hi=ecx, off_lo=edx, loff_t* result=esi,
             * whence=edi) → 0 + *result.  tcc seeks in object files with this;
             * without it tcc misreads a .o as "invalid object file".  32-bit
             * files: the low word is the offset. */
            long r = sys_lseek((int)f->ebx, (long)f->edx, (int)f->edi);
            if (r < 0) { f->eax = (uint32_t)-LNX_ENOENT; return; }
            if (f->esi) {                       /* §1.1 — client result slot */
                if (!lnx_w_ok(f->esi, sizeof(long long))) {
                    f->eax = (uint32_t)-LNX_EFAULT; return;
                }
                *(long long*)f->esi = (long long)r;
            }
            f->eax = 0;
            return;
        }
        case LNX_time: {
            /* time(time_t* t) → seconds; also writes *t if non-NULL. */
            struct ktimespec ts;
            sys_clock_gettime_k(CLOCK_REALTIME, &ts);
            if (f->ebx) {
                if (!lnx_w_ok(f->ebx, 4)) { f->eax = (uint32_t)-LNX_EFAULT; return; }
                *(uint32_t*)f->ebx = ts.sec;
            }
            f->eax = ts.sec;
            return;
        }

        case LNX_memfd_create:
            /* The NAME is advisory (Linux only uses it for /proc); we ignore it
             * and hand back a zero-length shm object, which ftruncate sizes. */
            f->eax = (uint32_t)sys_memfd(0);
            return;
        case LNX_ftruncate:
        case LNX_ftruncate64:
            f->eax = (uint32_t)sys_memfd_resize((int)f->ebx, (size_t)f->ecx);
            return;

        case LNX_getrandom:
            /* getrandom(buf=ebx, len=ecx, flags=edx) → the §M39 CSPRNG.  musl's
             * arc4random / TLS seeding uses it. */
            f->eax = (uint32_t)sys_getrandom((void*)f->ebx, (size_t)f->ecx,
                                             (unsigned)f->edx);
            return;

        case LNX_fstat64: {
            /* fstat64(fd=ebx, statbuf=ecx).  ld.so's map_library fstats a .so to
             * learn its size before mmapping it.  Translate d-os kstat → Linux
             * stat64.  (statx (383) is left ENOSYS; musl falls back to this.) */
            struct kstat k;
            if (sys_fstat_k((int)f->ebx, &k) != 0) { f->eax = (uint32_t)-LNX_ENOENT; return; }
            if (lnx_put_stat64(f->ecx, &k) != 0) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            f->eax = 0;
            return;
        }

        case LNX_stat64:
        case LNX_lstat64: {
            /* stat64/lstat64(path=ebx, statbuf=ecx) — path-based; musl falls back
             * to these when statx (383) returns ENOSYS.  No symlinks, so lstat
             * == stat.  NetSurf stats its resource + font files. */
            struct kstat k;
            if (lnx_stat_upath(f->ebx, &k) != 0) { f->eax = (uint32_t)-LNX_ENOENT; return; }
            if (lnx_put_stat64(f->ecx, &k) != 0) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            f->eax = 0;
            return;
        }
        case LNX_fstatat64: {
            /* fstatat64(dirfd=ebx, path=ecx, statbuf=edx, flags=esi) — AT_FDCWD
             * only (absolute/relative-to-cwd paths, which is all NetSurf uses). */
            struct kstat k;
            if (lnx_stat_upath(f->ecx, &k) != 0) { f->eax = (uint32_t)-LNX_ENOENT; return; }
            if (lnx_put_stat64(f->edx, &k) != 0) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            f->eax = 0;
            return;
        }
        case LNX_statx:
            /* Deliberately ENOSYS: musl (incl. ld.so's library dedup) falls back
             * to stat64, which we implement correctly.  A hand-rolled statx
             * struct is error-prone (its dev/ino feed ld.so's already-loaded
             * dedup — a wrong one makes ld.so reject valid libraries), so we do
             * NOT return synthetic statx data.  The one-per-stat probe is cheap
             * and silent (no log line here). */
            f->eax = (uint32_t)-LNX_ENOSYS;
            return;
        case LNX_newuname:
            /* uname(buf=ebx) — NetSurf builds its User-Agent from it. */
            f->eax = (uint32_t)sys_uname((struct kutsname*)f->ebx);
            return;
        case LNX_rt_sigaction:
            /* Accept + ignore: NetSurf installs SIGCHLD/SIGPIPE handlers it never
             * needs headless.  Real delivery is a later item (as on x86_64). */
            f->eax = 0;
            return;

        case LNX_getdents64:
            /* readdir — musl packs the Linux dirent64 layout (sys_getdents64). */
            f->eax = (uint32_t)sys_getdents64((int)f->ebx, (void*)f->ecx, (size_t)f->edx);
            return;
        case LNX_getpid:
            f->eax = (uint32_t)(task_current() ? task_current()->pid : -1);
            return;

        case LNX_fork: {
            /* Same as the native SYS_FORK: snapshot the user frame (child gets
             * eax=0) and clone.  proc_fork copies task->linux_abi to the child,
             * so a musl shell's children are serviced here too. */
            struct user_regs r;
            r.eax = 0;
            r.ebx = f->ebx; r.ecx = f->ecx; r.edx = f->edx;
            r.esi = f->esi; r.edi = f->edi; r.ebp = f->ebp;
            r.eip = f->eip; r.eflags = f->eflags; r.user_sp = f->user_esp;
            f->eax = (uint32_t)proc_fork(&r);
            return;
        }

        case LNX_execve:
            /* execve(path=ebx, argv=ecx, envp=edx) — envp ignored for now (the
             * child keeps the default env).  Replaces the image; on failure the
             * old image continues. */
            f->eax = (uint32_t)proc_execve((const char*)f->ebx, (char* const*)f->ecx);
            return;

        case LNX_waitpid:
        case LNX_wait4: {
            /* waitpid(pid=ebx, status=ecx, options=edx[, rusage=esi]).  d-os
             * task_wait returns the raw exit code; re-encode it into the Linux
             * wait-status layout (WIFEXITED: code in bits 8..15) so musl's
             * WEXITSTATUS() reads it correctly. */
            int code = 0;
            int pid = task_wait((int)f->ebx, &code);
            if (f->ecx) {                       /* §1.1 — client status slot */
                if (!lnx_w_ok(f->ecx, sizeof(int))) { f->eax = (uint32_t)-LNX_EFAULT; return; }
                *(int*)f->ecx = (code & 0xFF) << 8;
            }
            f->eax = (uint32_t)pid;
            return;
        }

        case LNX_set_tid_address:
            /* musl records the clear-child-tid address for thread cleanup and
             * uses the return as its initial TID; hand back our pid. */
            f->eax = (uint32_t)(task_current() ? task_current()->pid : 0);
            return;

        case LNX_poll:
            /* poll(fds=ebx, nfds=ecx, timeout_ms=edx).  The Linux i386 struct
             * pollfd is byte-identical to ours and POLLIN/POLLOUT share values,
             * so hand the array straight to sys_poll.  musl's resolver polls the
             * UDP socket here; sys_poll reports a netsock ready and recvmsg then
             * pump-reads the datagram. */
            f->eax = (uint32_t)sys_poll((struct pollfd*)f->ebx, (int)f->ecx, (int)f->edx);
            return;

        case LNX_socketcall:
            /* socketcall(call=ebx, args=ecx) — see linux_socketcall(). */
            f->eax = (uint32_t)linux_socketcall((int)f->ebx, (const uint32_t*)f->ecx);
            return;

        /* Direct socket syscalls (359+) are canonical operations now and are
         * answered by the engine above, from this arch's number map.  The two
         * that are not — sendmsg/recvmsg — still repack their register
         * arguments into the socketcall array shape so both spellings reach
         * one implementation. */
        case LNX_sendmsg:
        case LNX_recvmsg: {
            uint32_t args[6] = { f->ebx, f->ecx, f->edx, f->esi, f->edi, f->ebp };
            int call = (f->eax == LNX_sendmsg) ? LSOC_SENDMSG : LSOC_RECVMSG;
            f->eax = (uint32_t)linux_socketcall_k(call, args);
            return;
        }

        case LNX_ioctl:
            /* No TTY ioctls yet.  Returning ENOTTY (not ENOSYS) makes musl's
             * isatty() correctly report "not a terminal" (→ fully-buffered
             * stdio) instead of logging an unhandled syscall. */
            f->eax = (uint32_t)-LNX_ENOTTY;
            return;

        /* §M40 — clone().  i386 argument order is (flags, stack, ptid, TLS, ctid)
         * — TLS comes BEFORE ctid here, the opposite of amd64; getting that
         * backwards hands the kernel a thread pointer where it expects a futex
         * address.  Only the THREAD shape is served (CLONE_VM); anything else
         * is a fork. */
        case LNX_clone: {
            unsigned long flags = (unsigned long)f->ebx;
            struct user_regs r;
            r.eax = 0;
            r.ebx = f->ebx; r.ecx = f->ecx; r.edx = f->edx;
            r.esi = f->esi; r.edi = f->edi; r.ebp = f->ebp;
            r.eip = f->eip; r.eflags = f->eflags; r.user_sp = f->user_esp;
            if (!(flags & LNX_CLONE_VM)) {          /* a fork in disguise */
                f->eax = (uint32_t)proc_fork(&r);
                return;
            }
            if (!f->ecx) { f->eax = (uint32_t)-LNX_EINVAL; return; }
            int* ctid = (flags & LNX_CLONE_CHILD_CLEARTID) ? (int*)f->edi : NULL;
            if (ctid && !lnx_w_ok((uintptr_t)ctid, sizeof(int))) {
                f->eax = (uint32_t)-LNX_EFAULT; return;
            }
            /* i386 TLS: musl passes a `struct user_desc*`, not a raw base — the
             * base is the field the GDT descriptor needs, so read it out.  Using
             * the descriptor address as the thread pointer would aim %gs at the
             * descriptor itself. */
            uintptr_t tls = 0;
            if (f->esi) {
                const struct lnx_user_desc* ud = (const struct lnx_user_desc*)f->esi;
                if (lnx_r_ok((uintptr_t)ud, sizeof *ud)) tls = (uintptr_t)ud->base_addr;
            }
            int tid = proc_clone_thread(&r, (uintptr_t)f->ecx, tls, ctid);
            if (tid < 0) { f->eax = (uint32_t)-LNX_EINVAL; return; }
            if ((flags & LNX_CLONE_PARENT_SETTID) && f->edx) {
                if (lnx_w_ok(f->edx, sizeof(int))) *(int*)f->edx = tid;
            }
            if ((flags & LNX_CLONE_CHILD_SETTID) && ctid) *ctid = tid;
            f->eax = (uint32_t)tid;
            return;
        }

        case LNX_futex:
            /* musl's pthread mutexes/joins are futexes; without this a thread
             * that has to WAIT spins on -ENOSYS forever. */
            f->eax = (uint32_t)sys_futex((int*)f->ebx, (int)f->ecx, (int)f->edx);
            return;

        case LNX_fcntl64:
        case LNX_fcntl:
            f->eax = (uint32_t)linux_fcntl((int)f->ebx, (int)f->ecx, (long)f->edx);
            return;

        case LNX_mmap2: {
            /* i386 mmap2(addr=ebx, len=ecx, prot=edx, flags=esi, fd=edi,
             * pgoff=ebp) — pgoff is in PAGES.  §M37: full mmap so musl's ld.so
             * can load shared objects (file-backed segments at an offset, some
             * MAP_FIXED over a reservation) — not just anonymous malloc pages. */
            uintptr_t addr = f->ebx;
            uint32_t  len  = f->ecx;
            int       prot = (int)f->edx;
            int       flags = (int)f->esi;
            int       fd   = (int)f->edi;
            uint64_t  off  = (uint64_t)f->ebp * 4096u;   /* pgoff → byte offset */
            long r = sys_mmap_full(addr, (size_t)len, prot, flags, fd, off);
            f->eax = (r <= 0) ? (uint32_t)-12 /*ENOMEM*/ : (uint32_t)r;
            return;
        }

        case LNX_brk:
            /* No program break yet → report failure so musl's malloc uses
             * mmap instead (a valid fallback).  A real brk heap is a follow-up. */
            f->eax = 0;
            return;

        case LNX_set_thread_area: {
            /* musl's THE startup blocker.  Translate Linux user_desc onto the
             * §M35 per-CPU %gs GDT-TLS mechanism (identical to the native
             * SYS_SET_TLS), then hand musl back a GDT *index* it can turn into
             * a %gs selector: Linux userland loads %gs = (entry_number<<3)|3,
             * so entry_number = our selector >> 3 round-trips exactly. */
            struct lnx_user_desc* u = (struct lnx_user_desc*)f->ebx;
            struct task* t = task_current();
            /* §1.1 — read base_addr from and write entry_number back to client
             * memory: require the whole struct be user-writable. */
            if (!t || !lnx_w_ok(f->ebx, sizeof *u)) { f->eax = (uint32_t)-LNX_EFAULT; return; }
            t->tls_base = (uintptr_t)u->base_addr;
            t->has_tls  = 1;
            task_set_affinity(t, 1u << this_cpu_id());  /* per-CPU selector */
            hal_set_tls_base(t->tls_base);
            u->entry_number = (uint32_t)(gdt_tls_selector() >> 3);
            f->eax = 0;
            return;
        }

        case LNX_readlink:
        case LNX_readlinkat: {
            /* No symlinks in the VFS → -EINVAL for an existing path (musl
             * realpath() treats it as "use as-is"), -ENOENT if missing.
             * Unblocks NetSurf's realpath()-based resource/font lookup. */
            const char* path = (f->eax == LNX_readlink)
                                   ? (const char*)f->ebx    /* readlink: path=ebx */
                                   : (const char*)f->ecx;   /* readlinkat: path=ecx */
            struct kstat k;
            f->eax = (lnx_stat_upath((uintptr_t)path, &k) != 0) ? (uint32_t)-LNX_ENOENT
                                                                : (uint32_t)-LNX_EINVAL;
            return;
        }
        case LNX_access:
        case LNX_faccessat: {
            /* Existence check (no per-file perms yet): 0 if it stats OK. */
            const char* path = (f->eax == LNX_access)
                                   ? (const char*)f->ebx    /* access: path=ebx */
                                   : (const char*)f->ecx;   /* faccessat: path=ecx */
            struct kstat k;
            f->eax = (lnx_stat_upath((uintptr_t)path, &k) != 0) ? (uint32_t)-LNX_ENOENT : 0;
            return;
        }
        case LNX_madvise:
            f->eax = 0;                              /* advisory — accept + ignore */
            return;

        case LNX_mincore: {
            /* NOT advisory, unlike its neighbour above — the RETURN VALUE is the
             * answer.  Mesa's _eglPointerIsDereferencable() asks mincore whether
             * an address is mapped, so a blanket "success" would tell it that
             * literally every address is dereferenceable (see the x86_64 twin,
             * where exactly that made libEGL dereference the address 3).
             * Linux: EINVAL if addr is unaligned, ENOMEM if the range holds
             * unmapped pages, else 0 with one byte per page (bit 0 = resident;
             * we never swap, so every mapped page reports resident). */
            uintptr_t addr = f->ebx, len = f->ecx, uvec = f->edx;
            if (addr & (LNX_PAGE_SIZE - 1)) { f->eax = (uint32_t)-LNX_EINVAL; return; }
            uintptr_t pages = (len + LNX_PAGE_SIZE - 1) / LNX_PAGE_SIZE;
            if (!pages) { f->eax = 0; return; }
            if (!lnx_w_ok(uvec, pages))   { f->eax = (uint32_t)-LNX_EFAULT; return; }
            if (!vmm_user_access_ok(addr, pages * LNX_PAGE_SIZE, 0)) {
                f->eax = (uint32_t)-LNX_ENOMEM; return;
            }
            for (uintptr_t i = 0; i < pages; i++) ((uint8_t*)uvec)[i] = 1;
            f->eax = 0;
            return;
        }

        case LNX_nanosleep:
        case LNX_clock_nanosleep: {
            /* Yield the CPU for the requested duration.  NetSurf's fb event loop
             * sleeps here when idle; without it the browser busy-spins and
             * starves the compositor (the desktop appears frozen).  timespec =
             * {long tv_sec; long tv_nsec}; nanosleep's req is arg0 (ebx),
             * clock_nanosleep's is arg2 (edx, after clockid + flags). */
            uintptr_t ureq = (f->eax == LNX_nanosleep) ? f->ebx : f->edx;
            if (ureq && !lnx_r_ok(ureq, 2 * sizeof(int32_t))) {   /* §1.1 */
                f->eax = (uint32_t)-LNX_EFAULT; return;
            }
            const int32_t* req = (const int32_t*)ureq;
            long ms = req ? ((long)req[0] * 1000 + req[1] / 1000000) : 0;
            task_msleep(ms > 0 ? (uint32_t)ms : 1u);
            f->eax = 0;
            return;
        }
        case LNX_sched_yield:
            task_msleep(1);                          /* cooperative yield */
            f->eax = 0;
            return;

        /* §M42 display bridge — a ring-3 client (NetSurf's libnsfb "dos" surface)
         * drives a WM window.  Buffer/event pointers are in the caller's address
         * space (active now).  See kernel/gui/dosgui.c. */
        case LNX_DOSGUI_CREATE: {
            /* §1.1 — the title is a client string; copy it in before the
             * compositor stores/draws it. */
            char title[64];
            if (copy_str_from_user(title, f->edx, sizeof title) < 0) title[0] = 0;
            f->eax = (uint32_t)dosgui_create((int)f->ebx, (int)f->ecx, title);
            return;
        }
        case LNX_DOSGUI_PRESENT:
            f->eax = (uint32_t)dosgui_present((int)f->ebx, (const uint32_t*)f->ecx,
                                              (int)f->edx, (int)f->esi, (int)f->edi);
            return;
        case LNX_DOSGUI_POLL:
            f->eax = (uint32_t)dosgui_poll((int)f->ebx, (struct dosgui_event*)f->ecx);
            return;
        case LNX_DOSGUI_DESTROY:
            dosgui_destroy((int)f->ebx);
            f->eax = 0;
            return;

        default:
            kprintf("linux-abi: unhandled syscall %u (returning -ENOSYS)\n", f->eax);
            f->eax = (uint32_t)-LNX_ENOSYS;
            return;
    }
}
