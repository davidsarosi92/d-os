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
#include <stdint.h>

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
            /* Restore the kernel SP / PC saved by usermode.s and resume
             * there.  Bypasses the iretq-back-to-ring-3 path; the
             * interrupt frame on the (syscall) stack is abandoned, and
             * the next ring-3 → ring-0 transition will start fresh
             * from TSS.RSP0.  Noreturn. */
            hal_syscall_exit_to_kernel((uintptr_t)saved_rsp,
                                       (uintptr_t)saved_rip);
        }

        default:
            kprintf("syscall: unknown number %lu\n",
                    (unsigned long)f->rax);
            f->rax = (uint64_t)-1;
            return;
    }
}
