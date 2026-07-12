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
#include <stdint.h>
#include <stddef.h>

/* Excursion teleport-back (shared with the native path, usermode.s). */
extern uint32_t saved_esp;
extern uint32_t saved_eip;

/* Linux i386 syscall numbers we understand (grows toward the musl-required
 * set).  Anything else returns -ENOSYS and is logged once. */
#define LNX_exit             1
#define LNX_read             3
#define LNX_write            4
#define LNX_open             5
#define LNX_close            6
#define LNX_getpid          20
#define LNX_ioctl           54
#define LNX_brk             45
#define LNX_munmap          91
#define LNX_mprotect       125
#define LNX_readv          145
#define LNX_writev         146
#define LNX_mmap2          192
#define LNX_set_thread_area 243
#define LNX_exit_group     252
#define LNX_set_tid_address 258
#define LNX_openat         295

#define LNX_ENOSYS  38
#define LNX_ENOTTY  25

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
            /* We don't enforce page protections on the anonymous mappings musl
             * allocates (mallocng calls mprotect) — accept as a no-op. */
            f->eax = 0;
            return;

        case LNX_munmap:
            /* The d-os user mmap bump-allocates and does not reclaim yet, so
             * unmap is a no-op (a small leak).  Real reclaim is a follow-up. */
            f->eax = 0;
            return;

        case LNX_open:
            /* open(path=ebx, flags=ecx, mode=edx) — translate flags, ignore mode. */
            f->eax = (uint32_t)sys_open((const char*)f->ebx,
                                        linux_open_flags((int)f->ecx));
            return;

        case LNX_openat: {
            /* openat(dirfd=ebx, path=ecx, flags=edx, mode=esi).  We support the
             * AT_FDCWD form (absolute paths / cwd-relative), which is what musl
             * uses for open(); a real dirfd is a follow-up. */
            int dirfd = (int)f->ebx;
            if (dirfd != LAT_FDCWD) { f->eax = (uint32_t)-LNX_ENOSYS; return; }
            f->eax = (uint32_t)sys_open((const char*)f->ecx,
                                        linux_open_flags((int)f->edx));
            return;
        }
        case LNX_close:
            f->eax = (uint32_t)sys_close((int)f->ebx);
            return;
        case LNX_getpid:
            f->eax = (uint32_t)(task_current() ? task_current()->pid : -1);
            return;

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

        case LNX_mmap2: {
            /* i386 mmap2(addr=ebx, len=ecx, prot=edx, flags=esi, fd=edi,
             * pgoff=ebp).  We support anonymous (fd == -1) mappings — musl's
             * malloc uses MAP_ANONYMOUS|MAP_PRIVATE, fd=-1 — and the d-os mmap
             * bump-allocates a user VA.  (Earlier this misread len from edx /
             * fd from esi — prot/flags — so any malloc-driven mmap failed.) */
            uint32_t len = f->ecx;
            int fd = (int)f->edi;
            long r = sys_mmap((size_t)len, fd < 0 ? -1 : fd);
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
