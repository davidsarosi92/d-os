/* =============================================================================
 * syscall.c — int 0x80 dispatcher (i386).
 *
 * Moved from kernel/core/ to kernel/hal/x86/ in M20.5 Phase C — the
 * dispatcher's body reads arch-specific int_frame fields (eax, ebx)
 * so it's not portable.  x86_64 has its own copy under
 * kernel/hal/x86_64/syscall.c that reads (rax, rbx, ...).  The
 * syscall numbers themselves live in the portable
 * kernel/includes/syscall.h.
 *
 * Reached from `isr_handler` (idt.c) when a ring-3 process executes
 * `int 0x80`.  The CPU has already pushed the user's SS/ESP/EFLAGS/CS/EIP
 * onto the kernel stack, swapped to that stack via TSS.esp0, and our
 * isr_common stub has saved general regs and segments.  By the time we
 * arrive here the int_frame mirrors all of it.
 *
 * Convention:
 *   EAX = syscall number
 *   EBX = arg 0   ECX = arg 1   EDX = arg 2
 *   EAX = return value, written into f->eax so iret restores it.
 *
 * SYS_EXIT is special: instead of returning normally (which would iret
 * back into ring 3), we jump to the kernel context that
 * `enter_user_mode_wrap` saved before iret-ing into ring 3.  See
 * usermode.s for the matching half.
 * ============================================================================= */

#include "syscall.h"
#include "idt.h"
#include "console.h"
#include "printf.h"
#include "hal_api.h"
#include "task.h"
#include "proc.h"
#include "usermode.h"
#include "gdt.h"
#include "percpu.h"
#include <stdint.h>

/* Imports from usermode.s — the saved kernel context that lets SYS_EXIT
 * teleport back. */
extern uint32_t saved_esp;
extern uint32_t saved_eip;

void syscall_dispatch(struct int_frame* f) {
    switch (f->eax) {
        case SYS_PRINT: {
            /* EBX = const char* user pointer.  Walk it directly; our
             * identity map covers all of physical memory the user could
             * possibly hand us (their pages live below 256 MiB). */
            const char* s = (const char*)f->ebx;
            if (s) {
                while (*s) console_putchar(*s++);
            }
            f->eax = 0;
            return;
        }

        case SYS_EXIT: {
            /* Tier B — an INDEPENDENT user task ends here for good: close its
             * fds (it is still current) and task_exit(), so the scheduler moves
             * on and init reaps it (freeing its address space).  Never returns. */
            struct task* cur = task_current();
            if (cur && cur->user_task) {
                fd_close_all();
                task_exit_code((int)f->ebx);
            }
            /* Excursion-model self-tests: restore the kernel SP / PC saved by
             * `enter_user_mode_wrap` and resume there (teleport-back).  Bypasses
             * the iret-back-to-ring-3 path; the syscall-stack frame is abandoned,
             * fine because TSS.esp0 resets it for the next transition. */
            hal_syscall_exit_to_kernel(saved_esp, saved_eip);
        }

        case SYS_GETPID:
            f->eax = (uint32_t)(task_current() ? task_current()->pid : -1);
            return;

        /* M34 — fork(): snapshot the caller's user registers from the trapframe
         * and hand them to the portable-ish orchestrator.  The parent returns
         * here with the child's pid; the child starts on its own task. */
        case SYS_FORK: {
            struct user_regs r;
            r.eax = 0;
            r.ebx = f->ebx; r.ecx = f->ecx; r.edx = f->edx;
            r.esi = f->esi; r.edi = f->edi; r.ebp = f->ebp;
            r.eip = f->eip; r.eflags = f->eflags; r.user_sp = f->user_esp;
            f->eax = (uint32_t)proc_fork(&r);
            return;
        }

        /* M34 — waitpid(pid, int* status): block on the child-exit wait queue
         * (task_wait), then write the exit code to the user status pointer. */
        case SYS_WAITPID: {
            int status = 0;
            int pid = task_wait((int)f->ebx, &status);
            if (f->ecx) *(int*)f->ecx = status;
            f->eax = (uint32_t)pid;
            return;
        }

        /* M34 — execve(path, argv): replace this process's image.  On success
         * proc_execve does not return (it iret's into the new program); on
         * failure it returns -1 and the old image continues. */
        case SYS_EXECVE:
            f->eax = (uint32_t)proc_execve((const char*)f->ebx,
                                           (char* const*)f->ecx);
            return;

        case SYS_PIPE:
            f->eax = (uint32_t)sys_pipe((int*)f->ebx);
            return;
        case SYS_DUP2:
            f->eax = (uint32_t)sys_dup2((int)f->ebx, (int)f->ecx);
            return;

        /* M34 signals. */
        case SYS_KILL:
            f->eax = (uint32_t)sys_kill((int)f->ebx, (int)f->ecx);
            return;
        case SYS_SIGACTION:
            f->eax = (uint32_t)sys_sigaction((int)f->ebx, (long)f->ecx, (long)f->edx);
            return;
        case SYS_SIGRETURN:
            /* Restore the pre-handler context; do NOT touch f->eax afterwards
             * (signal_sigreturn set it to the interrupted syscall's result). */
            signal_sigreturn(f);
            return;

        /* M24 — network sockets (AF_INET). */
        case SYS_SOCKET:
            f->eax = (uint32_t)sys_socket((int)f->ebx, (int)f->ecx, (int)f->edx);
            return;
        case SYS_BIND:
            f->eax = (uint32_t)sys_bind((int)f->ebx, (int)f->ecx);
            return;
        case SYS_CONNECT:
            f->eax = (uint32_t)sys_connect((int)f->ebx, (uint32_t)f->ecx, (int)f->edx);
            return;

        /* M35 — threads. */
        case SYS_CLONE:
            f->eax = (uint32_t)proc_clone((uintptr_t)f->ebx, (uintptr_t)f->ecx);
            return;
        case SYS_FUTEX:
            f->eax = (uint32_t)sys_futex((int*)f->ebx, (int)f->ecx, (int)f->edx);
            return;

        /* M35 TLS — set_thread_area(base): record the caller's TLS pointer,
         * pin it to this CPU (its %gs selector is per-CPU), load this CPU's
         * user-TLS descriptor base, and return the ring-3 %gs selector. */
        case SYS_SET_TLS: {
            struct task* t = task_current();
            if (!t) { f->eax = 0; return; }
            t->tls_base = (uintptr_t)f->ebx;
            t->has_tls  = 1;
            task_set_affinity(t, 1u << this_cpu_id());
            hal_set_tls_base(t->tls_base);
            f->eax = gdt_tls_selector();
            return;
        }
        case SYS_SENDTO:
            f->eax = (uint32_t)sys_sendto((int)f->ebx, (const void*)f->ecx,
                                          f->edx, (uint32_t)f->esi, (int)f->edi);
            return;
        case SYS_RECVFROM:
            f->eax = (uint32_t)sys_recvfrom((int)f->ebx, (void*)f->ecx, f->edx,
                                            (uint32_t*)f->esi, (int*)f->edi);
            return;

        /* M25 stage 3 — fd syscalls.  EBX/ECX/EDX = arg0/arg1/arg2. */
        case SYS_WRITE:
            f->eax = (uint32_t)sys_write((int)f->ebx, (const void*)f->ecx, f->edx);
            return;
        case SYS_READ:
            f->eax = (uint32_t)sys_read((int)f->ebx, (void*)f->ecx, f->edx);
            return;
        case SYS_OPEN:
            f->eax = (uint32_t)sys_open((const char*)f->ebx, (int)f->ecx);
            return;
        case SYS_CLOSE:
            f->eax = (uint32_t)sys_close((int)f->ebx);
            return;
        case SYS_LSEEK:
            f->eax = (uint32_t)sys_lseek((int)f->ebx, (long)f->ecx, (int)f->edx);
            return;
        case SYS_MMAP:
            f->eax = (uint32_t)sys_mmap((size_t)f->ebx, (int)f->ecx);
            return;
        case SYS_MEMFD:
            f->eax = (uint32_t)sys_memfd((size_t)f->ebx);
            return;
        case SYS_SOCKETPAIR:
            f->eax = (uint32_t)sys_socketpair((int*)f->ebx);
            return;
        case SYS_SEND:
            f->eax = (uint32_t)sys_send((int)f->ebx, (const void*)f->ecx,
                                        f->edx, (int)f->esi);
            return;
        case SYS_RECV:
            f->eax = (uint32_t)sys_recv((int)f->ebx, (void*)f->ecx,
                                        f->edx, (int*)f->esi);
            return;
        case SYS_POLL:
            f->eax = (uint32_t)sys_poll((struct pollfd*)f->ebx, (int)f->ecx,
                                        (int)f->edx);
            return;

        default:
            kprintf("syscall: unknown number %u\n", f->eax);
            f->eax = (uint32_t)-1;
            return;
    }
}
