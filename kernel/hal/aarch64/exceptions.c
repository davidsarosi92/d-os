/* =============================================================================
 * exceptions.c — AArch64 EL1 exception dispatcher (M21).
 *
 * The assembler half (vectors.S) saves a trapframe and calls
 * aarch64_exception_handler(type, *tf).  This file:
 *   - installs the vector table into VBAR_EL1 (exceptions_init),
 *   - decodes and prints synchronous faults / SErrors (ESR/FAR/ELR) then
 *     halts — the ARM analogue of the x86 panic-dump in isr.c,
 *   - routes IRQs to a weak dispatch hook so the GIC + timer phases can plug
 *     in without this file having to know about them yet.
 *
 * ESR_EL1 (Exception Syndrome Register) top 6 bits (EC) classify the fault;
 * we print the raw value and the common ECs so a bring-up fault is legible on
 * the serial log.  This is diagnostics-grade, not a full decoder.
 * ============================================================================= */

#include <stdint.h>
#include "uaccess.h"      /* §1.1 — fault-fixup table for EL0 memory copies */
#include "task.h"
#include "hal_api.h"
#include "crash.h"      /* §M47 — record every fault */

/* Matches the trapframe laid down by exceptions.S (17 register-pairs). */
struct trapframe {
    uint64_t x[31];      /* x0..x30                         */
    uint64_t _pad;       /* x30's stp partner (xzr slot)    */
    uint64_t elr;        /* ELR_EL1  — return/faulting PC   */
    uint64_t spsr;       /* SPSR_EL1 — saved PSTATE         */
};

/* Exception type codes — keep in sync with the .equ values in exceptions.S. */
enum {
    EXC_SYNC   = 0,
    EXC_IRQ    = 1,
    EXC_FIQ    = 2,
    EXC_SERROR = 3,
};

/* Early UART primitives (uart.c) — used before the console stack exists. */
void uart_early_puts(const char* s);
void uart_early_puthex(uint64_t v);

/* Weak IRQ hook.  The GIC/timer phase (M21 Phase B) provides the strong
 * definition; until then a stray IRQ is simply acknowledged-by-ignoring,
 * which is safe because nothing has enabled an interrupt source yet. */
void aarch64_irq_dispatch(void) __attribute__((weak));
void aarch64_irq_dispatch(void) { }

/* SVC syscall dispatcher (syscall.c).  Weak so the early Phase-A/B builds
 * (before the userspace slice is linked) still resolve; the strong definition
 * decodes x8 and services SYS_PRINT/SYS_EXIT. */
void aarch64_syscall(struct trapframe* tf) __attribute__((weak));
void aarch64_syscall(struct trapframe* tf) { (void)tf; }

/* ESR_EL1.EC value for "SVC instruction executed in AArch64 state". */
#define EC_SVC64  0x15

static void dump_and_halt(const char* what, struct trapframe* tf) {
    uint64_t esr, far;
    __asm__ volatile ("mrs %0, esr_el1"  : "=r"(esr));
    __asm__ volatile ("mrs %0, far_el1"  : "=r"(far));

    uart_early_puts("\n*** AArch64 exception: ");
    uart_early_puts(what);
    uart_early_puts(" ***\n  ESR_EL1 = "); uart_early_puthex(esr);
    uart_early_puts("  (EC=");             uart_early_puthex(esr >> 26);
    uart_early_puts(")\n  FAR_EL1 = ");    uart_early_puthex(far);
    uart_early_puts("\n  ELR_EL1 = ");     uart_early_puthex(tf->elr);
    uart_early_puts("\n  SPSR    = ");     uart_early_puthex(tf->spsr);
    uart_early_puts("\n  x0..x3  = ");
    uart_early_puthex(tf->x[0]); uart_early_puts(" ");
    uart_early_puthex(tf->x[1]); uart_early_puts(" ");
    uart_early_puthex(tf->x[2]); uart_early_puts(" ");
    uart_early_puthex(tf->x[3]);
    /* §M47 — record it BEFORE applying the policy: halt and reboot both never
     * return, so a record written afterwards would never exist. */
    {
        struct task* ct = task_current();
        crash_report(CRASH_KERNEL_FAULT, ct ? ct->pid : -1,
                     ct ? ct->name : "kernel",
                     (uintptr_t)tf->elr, (uintptr_t)far, 11, what);
    }
    /* §3.1 — ring-0 (EL1) fault policy parity with x86: kernel.fault_policy=reboot
     * restarts the machine (PSCI) instead of halting forever.  Default = halt. */
    {
        extern const char* config_get(const char* key, const char* def);
        extern void hal_reboot(void);
        const char* pol = config_get("kernel.fault_policy", "halt");
        if (pol && (pol[0] == 'r' || pol[0] == 'R')) {
            uart_early_puts("\n  kernel.fault_policy=reboot — restarting.\n");
            hal_reboot();
        } else if (pol && (pol[0] == 'k' || pol[0] == 'K')) {
            /* §3.1 — parity with the x86 ring0_fault_policy "kill" option:
             * terminate just the faulting kernel thread and keep the machine
             * running (an M29-supervised service is then restarted).  Same
             * caveat as x86: best-effort — a kthread may have held a lock. */
            struct task* t = task_current();
            if (t && !t->is_idle && t->pid != 0) {
                uart_early_puts("\n  kernel.fault_policy=kill — terminating the "
                                "faulting kernel task (best-effort).\n");
                task_exit_code(139);          /* 128 + SIGSEGV; noreturn */
            }
            uart_early_puts("\n  kill policy: idle/pid0 context, not killable — halting.\n");
        }
    }
    uart_early_puts("\n  system halted.\n");

    for (;;) __asm__ volatile ("wfe");
}

void aarch64_exception_handler(uint64_t type, struct trapframe* tf) {
    switch (type) {
        case EXC_IRQ:
            aarch64_irq_dispatch();
            /* §M46 — force-kill safe point (parity with the x86 IRQ path): a
             * task interrupted in EL0 holds no kernel locks, so a pending forced
             * kill (a wedged ring-3 package that never yields) is torn down
             * right here.  SPSR_EL1.M[3:0]==0 means "came from EL0". */
            task_force_kill_point((tf->spsr & 0xF) == 0);
            return;                       /* return → RESTORE_TRAPFRAME → eret */
        case EXC_FIQ:
            /* We route everything through IRQ; a real FIQ is unexpected. */
            dump_and_halt("FIQ", tf);
            break;
        case EXC_SERROR:
            dump_and_halt("SError (async abort)", tf);
            break;
        case EXC_SYNC: {
            uint64_t esr;
            __asm__ volatile ("mrs %0, esr_el1" : "=r"(esr));
            if ((esr >> 26) == EC_SVC64) {   /* EL0/EL1 `svc` → syscall path */
                /* Run the syscall with interrupts ENABLED — identical treatment
                 * to the x86 `int 0x80` / SYSCALL branches, and for the same
                 * reason: taking an exception masks DAIF, so a syscall would
                 * otherwise run non-preemptibly from start to finish.  A
                 * `spin_lock` inside one could then never be resolved on a
                 * uniprocessor (the lock holder cannot be scheduled while we
                 * spin with IRQs masked) — a guaranteed hard lockup.  See the
                 * x86 twins and DOCS §8 (2026-08-02) for the failure this
                 * actually caused.  `eret` restores PSTATE from SPSR_EL1, so
                 * unmasking here does not disturb the return to EL0. */
                hal_intr_enable();
                aarch64_syscall(tf);
                return;                       /* → RESTORE_TRAPFRAME → eret to EL0 */
            }
            /* §1.1 — user-access exception table (parity with x86): an EL1
             * abort inside a uaccess primitive (an EL0 pointer that went bad
             * DURING the copy) resumes at its fixup, so the copy returns -EFAULT
             * instead of halting the machine.  Checked before any policy. */
            if ((tf->spsr & 0xF) != 0) {           /* came from EL1 (kernel) */
                uintptr_t pc = (uintptr_t)tf->elr;
                if (uaccess_fixup_lookup(&pc)) { tf->elr = (uint64_t)pc; return; }
            }
            /* §M46 — a fault taken FROM EL0 (user mode) must NEVER halt the box:
             * terminate the offending process and let the scheduler continue,
             * exactly like the x86 ring-3 fault-kill.  SPSR_EL1.M[3:0]==0b0000
             * (EL0t) is the "came from EL0" marker; a user thread holds no kernel
             * locks at a fault, so task_exit_code here is safe (reschedules; the
             * reaper frees its address space).  An EL1 (kernel) fault falls
             * through to dump_and_halt, which applies kernel.fault_policy
             * (halt / reboot / kill) — same knob as x86. */
            if ((tf->spsr & 0xF) == 0) {
                extern void task_exit_code(int) __attribute__((noreturn));
                uint64_t far; __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
                uart_early_puts("\nfault: EL0 user exception elr=");
                uart_early_puthex(tf->elr);
                uart_early_puts(" far="); uart_early_puthex(far);
                uart_early_puts(" — killing process\n");
                {   /* §M47 — record it for the reporting sinks. */
                    struct task* ct = task_current();
                    crash_report(CRASH_USER_FAULT, ct ? ct->pid : -1,
                                 ct ? ct->name : "?", (uintptr_t)tf->elr,
                                 (uintptr_t)far, 11, "EL0 synchronous exception");
                }
                task_exit_code(139);          /* 128 + SIGSEGV */
            }
            dump_and_halt("synchronous", tf);
            break;
        }
        default:
            dump_and_halt("synchronous", tf);
            break;
    }
}

/* Point VBAR_EL1 at the vector table and synchronise.  After this, any
 * fault or IRQ is delivered through the handlers above. */
void exceptions_init(void) {
    extern char vector_table[];
    __asm__ volatile (
        "msr vbar_el1, %0\n"
        "isb\n"
        :: "r"(vector_table) : "memory");
    uart_early_puts("aarch64: exception vectors installed (VBAR_EL1)\n");
}
