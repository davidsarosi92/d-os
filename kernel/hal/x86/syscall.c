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
            /* Restore the kernel SP / PC saved by the arch's
             * `enter_user_mode_wrap` and resume there.  Bypasses the
             * normal iret-back-to-ring-3 path; the interrupt frame on
             * this (syscall) stack is simply abandoned, which is fine
             * because the arch's ring-3 → ring-0 transition (e.g. TSS.
             * esp0 on x86) resets it for the next transition.  Noreturn. */
            hal_syscall_exit_to_kernel(saved_esp, saved_eip);
        }

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
