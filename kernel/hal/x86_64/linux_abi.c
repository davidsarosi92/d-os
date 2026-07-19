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
#include "printf.h"
#include "hal_api.h"
#include "vfs.h"
#include "proc.h"          /* proc_fork / proc_execve / proc_clone */
#include "usermode.h"      /* struct user_regs (fork snapshot)     */
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
#define LNX_socket           41
#define LNX_connect          42
#define LNX_sendto           44
#define LNX_recvfrom         45
#define LNX_clone            56
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
#define LNX_membarrier      324

#define LNX_ENOSYS  38
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

void linux_syscall_dispatch(struct int_frame* f) {
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
        case LNX_unlink:
            f->rax = (uint64_t)vfs_unlink((const char*)a0);
            return;

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
                if (a1) *(uint64_t*)a1 = t ? (uint64_t)t->tls_base : 0;
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
            /* Best-effort success: no per-task signal mask / robust futex list /
             * fd flags tracked yet, but musl's startup + stdio paths only need
             * these to "not fail". */
            f->rax = 0;
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

        case LNX_getrandom:
            f->rax = (uint64_t)sys_getrandom((void*)a0, (size_t)a1, (unsigned)a2);
            return;

        case LNX_clock_gettime: {
            /* x86_64 timespec { long tv_sec; long tv_nsec; } (both 64-bit). */
            struct ktimespec ts;
            sys_clock_gettime((int)a0, &ts);
            uint64_t* p = (uint64_t*)a1;
            if (p) { p[0] = ts.sec; p[1] = ts.nsec; }
            f->rax = 0;
            return;
        }
        case LNX_gettimeofday: {
            struct ktimespec ts;
            sys_clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t* p = (uint64_t*)a0;             /* {tv_sec; tv_usec} */
            if (p) { p[0] = ts.sec; p[1] = ts.nsec / 1000; }
            f->rax = 0;
            return;
        }

        case LNX_fstat: {
            struct kstat k;
            if (sys_fstat((int)a0, &k) != 0) { f->rax = (uint64_t)-LNX_ENOENT; return; }
            fill_stat((struct lnx_stat*)a1, &k);
            f->rax = 0;
            return;
        }
        case LNX_stat: {
            struct kstat k;
            if (sys_stat((const char*)a0, &k) != 0) { f->rax = (uint64_t)-LNX_ENOENT; return; }
            fill_stat((struct lnx_stat*)a1, &k);
            f->rax = 0;
            return;
        }
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
            if (a1) *(int*)a1 = (code & 0xFF) << 8;   /* WIFEXITED: code in 8..15 */
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

        /* ---- Still deferred (not needed by sh/coreutils): threads (clone with
         * CLONE_VM), and the BSD socket calls (a separate net-syscall port). */
        case LNX_clone:
        case LNX_socket:
        case LNX_connect:
        case LNX_sendto:
        case LNX_recvfrom:
            kprintf("linux-abi64: syscall %lu not yet ported (x86_64)\n",
                    (unsigned long)f->rax);
            f->rax = (uint64_t)-LNX_ENOSYS;
            return;

        default:
            kprintf("linux-abi64: unhandled syscall %lu (returning -ENOSYS)\n",
                    (unsigned long)f->rax);
            f->rax = (uint64_t)-LNX_ENOSYS;
            return;
    }
}
