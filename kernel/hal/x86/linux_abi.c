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
#include "printf.h"
#include "hal_api.h"
#include "gdt.h"          /* gdt_tls_selector — the ring-3 %gs selector */
#include "percpu.h"       /* this_cpu_id — TLS descriptor is per-CPU     */
#include "vfs.h"          /* VFS_* open flags — target of the translation */
#include "proc.h"         /* proc_fork / proc_execve                      */
#include "usermode.h"     /* struct user_regs (fork)                      */
#include <stdint.h>
#include <stddef.h>

/* Excursion teleport-back (shared with the native path, usermode.s). */
extern uint32_t saved_esp;
extern uint32_t saved_eip;

/* Linux i386 syscall numbers we understand (grows toward the musl-required
 * set).  Anything else returns -ENOSYS and is logged once. */
#define LNX_exit             1
#define LNX_fork             2
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
#define LNX_rt_sigprocmask 175
#define LNX_mmap2          192
#define LNX_fstat64        197
#define LNX_set_thread_area 243
#define LNX_getrandom      355
#define LNX_exit_group     252
#define LNX_getdents64     220
#define LNX_fcntl64        221
#define LNX_set_tid_address 258
#define LNX_openat         295

#define LNX_ENOSYS  38
#define LNX_ENOTTY  25
#define LNX_ENOENT   2

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

void linux_syscall_dispatch(struct int_frame* f) {
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

        case LNX_rt_sigprocmask:
            /* musl brackets fork() (and other paths) with signal-mask changes.
             * We don't implement a per-task blocked-signal mask yet; report
             * success so those paths proceed (signals themselves are best-effort
             * here anyway). */
            f->eax = 0;
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
            if (sys_fstat((int)f->ebx, &k) != 0) { f->eax = (uint32_t)-LNX_ENOENT; return; }
            fill_stat64((struct lnx_stat64*)f->ecx, &k);
            f->eax = 0;
            return;
        }

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
            if (f->ecx) *(int*)f->ecx = (code & 0xFF) << 8;
            f->eax = (uint32_t)pid;
            return;
        }

        case LNX_set_tid_address:
            /* musl records the clear-child-tid address for thread cleanup and
             * uses the return as its initial TID; hand back our pid. */
            f->eax = (uint32_t)(task_current() ? task_current()->pid : 0);
            return;

        case LNX_ioctl:
            /* No TTY ioctls yet.  Returning ENOTTY (not ENOSYS) makes musl's
             * isatty() correctly report "not a terminal" (→ fully-buffered
             * stdio) instead of logging an unhandled syscall. */
            f->eax = (uint32_t)-LNX_ENOTTY;
            return;

        case LNX_fcntl64:
            /* musl's opendir() sets FD_CLOEXEC via fcntl.  We don't track the
             * CLOEXEC/status flags yet; report success (0) so descriptor setup
             * proceeds.  F_DUPFD would need real work, but musl's dir/stdio
             * paths only use the flag-setting/getting commands. */
            f->eax = 0;
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
            if (!u || !t) { f->eax = (uint32_t)-14 /*EFAULT*/; return; }
            t->tls_base = (uintptr_t)u->base_addr;
            t->has_tls  = 1;
            task_set_affinity(t, 1u << this_cpu_id());  /* per-CPU selector */
            hal_set_tls_base(t->tls_base);
            u->entry_number = (uint32_t)(gdt_tls_selector() >> 3);
            f->eax = 0;
            return;
        }

        default:
            kprintf("linux-abi: unhandled syscall %u (returning -ENOSYS)\n", f->eax);
            f->eax = (uint32_t)-LNX_ENOSYS;
            return;
    }
}
