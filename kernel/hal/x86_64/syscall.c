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
#include "drvuser.h"   /* §M33 Tier 1 — ring-3 driver resources */
#include "idt.h"
#include "console.h"
#include "printf.h"
#include "hal_api.h"
#include "task.h"
#include "proc.h"
#include "usermode.h"
#include "percpu.h"   /* §M52 — this_cpu_id for the per-CPU syscall area */
#include "acpi.h"     /* ACPI_MAX_CPUS — same bound the TSS array uses */
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
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr"
                      :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* §M52 — the per-CPU SYSCALL scratch area.
 *
 * SYSCALL hands the kernel no stack: rsp is still the USER stack on entry, so
 * the entry stub has to stash it and load a kernel one before it can touch
 * memory.  It also has no free register to find them with — rcx and r11 are
 * already clobbered by the instruction itself and everything else holds a
 * syscall argument.
 *
 * The original stub kept both in globals, which was correct while ring-3 tasks
 * only ever ran on the BSP and is a disaster now that they do not: two CPUs in
 * SYSCALL at the same time overwrite each other's stashed user rsp AND run the
 * kernel on the SAME stack.  syscall_entry.s said as much in its own header
 * ("UP-correct only ... Noted, not built"), and the note outlived the premise.
 *
 * `swapgs` is the instruction that exists for exactly this: one MSR the CPU
 * swaps into GS.base atomically at the entry, giving the stub a per-CPU base
 * to address through without needing a register to compute it.  GS is free for
 * this — x86_64 musl puts thread-local storage in FS. */
struct syscall_area {
    uint64_t kernel_rsp;      /* [gs:0]  — this CPU's ring-0 stack top   */
    uint64_t user_rsp;        /* [gs:8]  — stash of the caller's rsp     */
};
static struct syscall_area g_syscall_area[ACPI_MAX_CPUS];

/* tss.c calls this from hal_set_kernel_stack, on the CPU doing the switch. */
void syscall_set_kernel_rsp(int cpu, uintptr_t top) {
    if (cpu >= 0 && cpu < ACPI_MAX_CPUS) g_syscall_area[cpu].kernel_rsp = (uint64_t)top;
}

void syscall_init_64(void) {
    /* EFER.SCE (bit 0) — arm the SYSCALL/SYSRET instructions. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);

    /* §M52 — point this CPU's KERNEL_GS_BASE at its own scratch area, and
     * leave GS_BASE (the value in force while ring 3 runs) at zero.  `swapgs`
     * in the entry stub exchanges the two, so [gs:...] inside the stub always
     * addresses THIS CPU's slot no matter which CPU took the syscall. */
    {
        int c = this_cpu_id();
        if (c < 0 || c >= ACPI_MAX_CPUS) c = 0;
        wrmsr(MSR_GS_BASE, 0);
        wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&g_syscall_area[c]);
    }

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

static void syscall_dispatch_body(struct int_frame* f);

/* §1.1 — see the i386 twin: flag the task while servicing a ring-3 trap so
 * usyscall.c gates the frame's pointer arguments as USER pointers. */
void syscall_dispatch(struct int_frame* f) {
    struct task* me = task_current();
    int prev = me ? me->in_user_syscall : 0;
    if (me) me->in_user_syscall = 1;
    syscall_dispatch_body(f);
    if (me) me->in_user_syscall = prev;
}

static void syscall_dispatch_body(struct int_frame* f) {
    switch (f->rax) {
        case SYS_PRINT:
            /* RBX = const char* user pointer.  §1.1 — validated copy-in (see
             * sys_print); never dereferenced in place. */
            f->rax = (uint64_t)sys_print((const char*)(uintptr_t)f->rbx);
            return;

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

        /* ------------------------------------------------------------------
         * M34/M35/M36 — the rest of the native ABI.
         *
         * Every case below calls the SAME portable core the i386 dispatcher
         * calls; the only arch-specific part is reading the arguments out of
         * this frame.  They were missing here for no deeper reason than that
         * this file was written before them, which is what made x86_64's
         * userland look far smaller than i386's: the programs linked fine and
         * then died on "syscall: unknown number".
         * ------------------------------------------------------------------ */

        /* M34 — fork(): snapshot the caller's ring-3 register state so the
         * child can be resumed with RAX = 0 (see hal/x86_64/fork.c). */
        case SYS_FORK: {
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

        /* M34 — waitpid(pid, int* status). */
        case SYS_WAITPID: {
            int status = 0;
            int pid = task_wait((int)f->rbx, &status);
            if (f->rcx) *(int*)(uintptr_t)f->rcx = status;
            f->rax = (uint64_t)pid;
            return;
        }

        /* M34 — execve(path, argv): on success it does not return. */
        case SYS_EXECVE:
            f->rax = (uint64_t)proc_execve((const char*)(uintptr_t)f->rbx,
                                           (char* const*)(uintptr_t)f->rcx);
            return;

        case SYS_PIPE:
            f->rax = (uint64_t)sys_pipe((int*)(uintptr_t)f->rbx);
            return;
        case SYS_DUP2:
            f->rax = (uint64_t)sys_dup2((int)f->rbx, (int)f->rcx);
            return;

        /* M34 signals. */
        case SYS_KILL:
            f->rax = (uint64_t)sys_kill((int)f->rbx, (int)f->rcx);
            return;
        case SYS_SIGACTION:
            f->rax = (uint64_t)sys_sigaction((int)f->rbx, (long)f->rcx, (long)f->rdx);
            return;
        case SYS_SIGRETURN:
            /* Restores the pre-handler context; do NOT touch f->rax afterwards
             * (signal_sigreturn set it to the interrupted syscall's result). */
            signal_sigreturn(f);
            return;

        /* M24 — network sockets (AF_INET). */
        case SYS_SOCKET:
            f->rax = (uint64_t)sys_socket((int)f->rbx, (int)f->rcx, (int)f->rdx);
            return;
        case SYS_BIND:
            f->rax = (uint64_t)sys_bind((int)f->rbx, (uint32_t)f->rcx, (int)f->rdx);
            return;
        /* §M24.10 — the server half. */
        case SYS_LISTEN:
            f->rax = (uint64_t)sys_listen((int)f->rbx, (int)f->rcx);
            return;
        case SYS_ACCEPT:
            f->rax = (uint64_t)sys_accept((int)f->rbx, (uint32_t*)(uintptr_t)f->rcx,
                                          (int*)(uintptr_t)f->rdx);
            return;
        case SYS_GETSOCKNAME:
            f->rax = (uint64_t)sys_getsockname((int)f->rbx, (uint32_t*)(uintptr_t)f->rcx,
                                               (int*)(uintptr_t)f->rdx);
            return;
        case SYS_GETPEERNAME:
            f->rax = (uint64_t)sys_getpeername((int)f->rbx, (uint32_t*)(uintptr_t)f->rcx,
                                               (int*)(uintptr_t)f->rdx);
            return;
        case SYS_CONNECT:
            f->rax = (uint64_t)sys_connect((int)f->rbx, (uint32_t)f->rcx, (int)f->rdx);
            return;
        case SYS_SENDTO:
            f->rax = (uint64_t)sys_sendto((int)f->rbx, (const void*)(uintptr_t)f->rcx,
                                          (size_t)f->rdx, (uint32_t)f->rsi, (int)f->rdi);
            return;
        case SYS_RECVFROM:
            f->rax = (uint64_t)sys_recvfrom((int)f->rbx, (void*)(uintptr_t)f->rcx,
                                            (size_t)f->rdx,
                                            (uint32_t*)(uintptr_t)f->rsi,
                                            (int*)(uintptr_t)f->rdi);
            return;

        /* M35 — threads. */
        case SYS_CLONE:
            f->rax = (uint64_t)proc_clone((uintptr_t)f->rbx, (uintptr_t)f->rcx);
            return;
        case SYS_FUTEX:
            f->rax = (uint64_t)sys_futex((int*)(uintptr_t)f->rbx, (int)f->rcx,
                                         (int)f->rdx);
            return;

        /* M35 TLS — unlike i386 (where the thread pointer lives in a per-CPU
         * GDT segment and userland must load the returned selector), x86_64
         * puts it in the FS.base MSR, which the kernel writes itself.  There is
         * therefore NO selector to hand back and no need to pin the task to a
         * CPU: the base is restored on every context switch (see tss.c).  We
         * return 0 and user/libc.c's set_tls ignores it on non-i386. */
        case SYS_SET_TLS: {
            struct task* t = task_current();
            if (!t) { f->rax = 0; return; }
            t->tls_base = (uintptr_t)f->rbx;
            t->has_tls  = 1;
            hal_set_tls_base(t->tls_base);
            f->rax = 0;
            return;
        }

        /* M36 — POSIX syscall breadth. */
        case SYS_STAT:
            f->rax = (uint64_t)sys_stat((const char*)(uintptr_t)f->rbx,
                                        (struct kstat*)(uintptr_t)f->rcx);
            return;
        case SYS_FSTAT:
            f->rax = (uint64_t)sys_fstat((int)f->rbx, (struct kstat*)(uintptr_t)f->rcx);
            return;
        case SYS_GETDENTS:
            f->rax = (uint64_t)sys_getdents((int)f->rbx, (void*)(uintptr_t)f->rcx,
                                            (size_t)f->rdx);
            return;
        case SYS_UNAME:
            f->rax = (uint64_t)sys_uname((struct kutsname*)(uintptr_t)f->rbx);
            return;
        case SYS_CLOCK_GETTIME:
            f->rax = (uint64_t)sys_clock_gettime((int)f->rbx,
                                                 (struct ktimespec*)(uintptr_t)f->rcx);
            return;
        case SYS_NANOSLEEP:
            f->rax = (uint64_t)sys_nanosleep((unsigned)f->rbx);
            return;
        case SYS_TIMERFD_CREATE:
            f->rax = (uint64_t)sys_timerfd_create();
            return;
        case SYS_TIMERFD_SETTIME:
            f->rax = (uint64_t)sys_timerfd_settime_u((int)f->rbx, (int)f->rcx,
                                                     (const uint64_t*)(uintptr_t)f->rdx);
            return;
        case SYS_TIMERFD_GETTIME:
            f->rax = (uint64_t)sys_timerfd_gettime((int)f->rbx,
                                                   (uint64_t*)(uintptr_t)f->rcx);
            return;
        case SYS_SETITIMER:
            f->rax = (uint64_t)sys_setitimer_u((const uint64_t*)(uintptr_t)f->rbx);
            return;
        case SYS_GETRANDOM:
            f->rax = (uint64_t)sys_getrandom((void*)(uintptr_t)f->rbx, (size_t)f->rcx,
                                             (unsigned)f->rdx);
            return;

        /* §M33 Tier 1 — a ring-3 driver asking for its resources.  Same
         * bodies as the i386 dispatcher; only the register names differ. */
        case SYS_DRV_PORTS:
            f->rax = (uint64_t)drvuser_sys_ports((uint16_t)f->rbx, (uint16_t)f->rcx);
            return;
        case SYS_DRV_IRQ:
            f->rax = (uint64_t)drvuser_sys_irq((int)f->rbx);
            return;
        case SYS_DRV_IRQ_WAIT:
            f->rax = (uint64_t)drvuser_sys_irq_wait((int)f->rbx, (int)f->rcx);
            return;
        case SYS_DRV_INPUT:
            f->rax = (uint64_t)drvuser_sys_input((int)f->rbx, (int)f->rcx,
                                                 (unsigned)f->rdx, (int)f->rsi);
            return;
        case SYS_DRV_LOG: {
            char msg[128];
            int n = copy_str_from_user(msg, f->rbx, sizeof msg);
            f->rax = (uint64_t)drvuser_sys_log(n < 0 ? NULL : msg);
            return;
        }

        default:
            kprintf("syscall: unknown number %lu\n",
                    (unsigned long)f->rax);
            f->rax = (uint64_t)-1;
            return;
    }
}
