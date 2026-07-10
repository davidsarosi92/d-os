/* =============================================================================
 * syscall.c — int 0x80 dispatcher (x86_64).
 *
 * Mirror of kernel/hal/x86/syscall.c with the i386 int_frame field
 * names (eax, ebx, eip) replaced by their x86_64 counterparts
 * (rax, rbx).  Field offsets are different but field NAMES match
 * the layout in idt.h's `#if defined(__x86_64__)` section.
 *
 * Reached from `isr_handler` (idt.c) when a ring-3 process executes
 * `int 0x80`.  The CPU has already pushed the user's SS/RSP/RFLAGS/CS/RIP
 * onto the kernel stack (TSS.rsp0 = syscall_stack top), and our
 * isr_common stub has saved 15 GPRs + int_no/err_code.  By the time
 * we arrive here the int_frame mirrors all of it.
 *
 * Convention (kept identical to i386 for portability of test programs
 * like cmd_ringtest's hand-coded bytes):
 *   RAX = syscall number     (32-bit MOV zero-extends into RAX)
 *   RBX = arg 0
 *   RCX = arg 1
 *   RDX = arg 2
 *   RAX = return value, written back into f->rax so iretq restores it.
 *
 * SYS_EXIT is the teleport: instead of returning normally (which would
 * iretq back into ring 3), we restore the kernel context that
 * `enter_user_mode_wrap` saved before iretq-ing into ring 3.  See
 * kernel/hal/x86_64/usermode.s for the matching half.
 *
 * SYSCALL/SYSRET instruction path is NOT wired up here yet (would
 * need GDT slot reorganization for the SYSRET selector-arithmetic
 * convention).  Ring 3 reaches this dispatcher via `int 0x80` only
 * for now — same path that i386 has used since M6.
 * ============================================================================= */

#include "syscall.h"
#include "idt.h"
#include "console.h"
#include "printf.h"
#include "hal_api.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

/* Imports from kernel/hal/x86_64/usermode.s — the saved kernel context
 * that lets SYS_EXIT teleport back. */
extern uint64_t saved_rsp;
extern uint64_t saved_rip;

void syscall_dispatch(struct int_frame* f) {
    switch (f->rax) {
        case SYS_PRINT: {
            /* RBX = const char* user pointer.  Walk it directly; the
             * boot-built identity map covers the first 1 GiB of phys
             * memory, which spans every page a user process could
             * reference today (their pages live below 256 MiB). */
            const char* s = (const char*)(uintptr_t)f->rbx;
            if (s) {
                while (*s) console_putchar(*s++);
            }
            f->rax = 0;
            return;
        }

        case SYS_EXIT: {
            /* Tier B — an independent user task ends for good: close fds (still
             * current) + task_exit(); init reaps it (frees its address space). */
            struct task* cur = task_current();
            if (cur && cur->user_task) {
                fd_close_all();
                task_exit_code((int)f->rbx);
            }
            /* Excursion-model self-tests: teleport back to enter_user_mode_wrap's
             * saved kernel context. */
            hal_syscall_exit_to_kernel((uintptr_t)saved_rsp,
                                       (uintptr_t)saved_rip);
        }

        case SYS_GETPID:
            f->rax = (uint64_t)(task_current() ? task_current()->pid : -1);
            return;

        /* M25 stage 3 — fd syscalls.  RBX/RCX/RDX = arg0/arg1/arg2. */
        case SYS_WRITE:
            f->rax = (uint64_t)sys_write((int)f->rbx, (const void*)(uintptr_t)f->rcx,
                                         (size_t)f->rdx);
            return;
        case SYS_READ:
            f->rax = (uint64_t)sys_read((int)f->rbx, (void*)(uintptr_t)f->rcx,
                                        (size_t)f->rdx);
            return;
        case SYS_OPEN:
            f->rax = (uint64_t)sys_open((const char*)(uintptr_t)f->rbx, (int)f->rcx);
            return;
        case SYS_CLOSE:
            f->rax = (uint64_t)sys_close((int)f->rbx);
            return;
        case SYS_LSEEK:
            f->rax = (uint64_t)sys_lseek((int)f->rbx, (long)f->rcx, (int)f->rdx);
            return;
        case SYS_MMAP:
            f->rax = (uint64_t)sys_mmap((size_t)f->rbx, (int)f->rcx);
            return;
        case SYS_MEMFD:
            f->rax = (uint64_t)sys_memfd((size_t)f->rbx);
            return;
        case SYS_SOCKETPAIR:
            f->rax = (uint64_t)sys_socketpair((int*)(uintptr_t)f->rbx);
            return;
        case SYS_SEND:
            f->rax = (uint64_t)sys_send((int)f->rbx, (const void*)(uintptr_t)f->rcx,
                                        (size_t)f->rdx, (int)f->rsi);
            return;
        case SYS_RECV:
            f->rax = (uint64_t)sys_recv((int)f->rbx, (void*)(uintptr_t)f->rcx,
                                        (size_t)f->rdx, (int*)(uintptr_t)f->rsi);
            return;
        case SYS_POLL:
            f->rax = (uint64_t)sys_poll((struct pollfd*)(uintptr_t)f->rbx,
                                        (int)f->rcx, (int)f->rdx);
            return;

        default:
            kprintf("syscall: unknown number %lu\n",
                    (unsigned long)f->rax);
            f->rax = (uint64_t)-1;
            return;
    }
}
