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

/* The SYSCALL-instruction entry point (syscall_entry.s) — installed into the
 * IA32_LSTAR MSR so `syscall` from ring 3 lands there. */
extern void syscall_entry_64(void);

/* ---- MSR helpers + fast-syscall (SYSCALL/SYSRET) bring-up (§M20.6.1) -------
 *
 * x86_64 musl issues `syscall` (not int 0x80), so a Linux-ABI process reaches
 * the kernel through the SYSCALL instruction.  We arm it here: enable SCE in
 * EFER, point LSTAR at our entry stub, set STAR's kernel selectors, and mask
 * the dangerous RFLAGS bits (notably IF) on entry.  We RETURN via iretq (see
 * syscall_entry.s), so STAR's user half (used only by SYSRET) is set to a sane
 * value but never actually consumed. */
#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr"
                      :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void syscall_init_64(void) {
    /* EFER.SCE (bit 0) — arm the SYSCALL/SYSRET instructions. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);

    /* STAR[47:32] = kernel CS (0x08): SYSCALL loads CS=0x08, SS=0x10.
     * STAR[63:48] = user base for SYSRET; we iretq back so it is unused, but
     * set it to 0x1B so a future SYSRET path would find sane user selectors. */
    wrmsr(MSR_STAR, ((uint64_t)0x1Bu << 48) | ((uint64_t)0x08u << 32));

    /* LSTAR = 64-bit entry RIP. */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_64);

    /* FMASK — RFLAGS bits cleared on entry.  IF(0x200) so interrupts are off
     * (matching the int-0x80 interrupt gate); plus TF/DF/NT/AC/direction, the
     * standard Linux mask (0x47700). */
    wrmsr(MSR_FMASK, 0x47700u);
}

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
