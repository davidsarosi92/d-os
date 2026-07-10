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
    uart_early_puts("\n  system halted.\n");

    for (;;) __asm__ volatile ("wfe");
}

void aarch64_exception_handler(uint64_t type, struct trapframe* tf) {
    switch (type) {
        case EXC_IRQ:
            aarch64_irq_dispatch();
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
                aarch64_syscall(tf);
                return;                       /* → RESTORE_TRAPFRAME → eret to EL0 */
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
