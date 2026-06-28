/* =============================================================================
 * idt.h — Interrupt Descriptor Table + IRQ dispatch public interface.
 *
 * The IDT tells the CPU which function to jump to for each of the 256
 * interrupt vectors.  Vectors 0–31 are CPU exceptions (divide-by-zero,
 * page fault, GPF, ...); vectors 32–47 are the legacy 8259 PIC IRQ lines
 * after we remap them (the default mapping collides with exceptions);
 * vectors 48+ are free for software interrupts (syscalls later).
 *
 * `idt_init` builds the table, installs all 48 hardware vector gates,
 * remaps the PIC, and leaves interrupts *still disabled* — the caller is
 * responsible for `sti` once every subsystem that expects to service
 * interrupts is ready.
 *
 * Per-arch `int_frame` layouts live in #ifdef blocks below — the CPU
 * pushes very different things on entry depending on the bitness.  The
 * field NAMES still match across archs so portable handlers
 * (pit_irq, keyboard_irq) can read int_no/err_code uniformly; the
 * register-set fields (eax/rax) are accessed only by arch-specific code
 * (syscall.c — and even there only on i386 for now since x86_64 has no
 * ring-3 path until M20.5+).
 * ============================================================================= */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

#if defined(__x86_64__)

/* Snapshot of the CPU state captured on interrupt entry — x86_64.
 *
 * On x86_64 the CPU pushes ss/rsp/rflags/cs/rip on every interrupt
 * (no more "only on privilege change" — the long-mode interrupt
 * stack frame is always 5 quadwords).  Our stub adds err_code + int_no
 * and pushes 15 GPRs (rax..r15 except rsp).  Segment registers are
 * essentially meaningless in long mode (ds/es/fs/gs.base is what
 * matters; ds/es/fs/gs limits are ignored) so we don't save them.
 *
 * Field order MUST match the push sequence in
 * kernel/hal/x86_64/isr_stubs.s. */
struct int_frame {
    /* Pushed by our stub (general-purpose registers, high to low):
     * r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;

    /* Pushed by our stub: */
    uint64_t int_no;
    uint64_t err_code;

    /* Pushed by the CPU on every interrupt in long mode: */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

#else /* i386 */

/* Snapshot of the CPU state captured on interrupt entry — i386.
 *
 * The field order must exactly match the push sequence in
 * kernel/hal/x86/isr_stubs.s — C code and asm need to agree byte-
 * for-byte. */
struct int_frame {
    /* Pushed by our stub, in reverse order from the code: */
    uint32_t gs, fs, es, ds;

    /* Pushed by `pusha`, in the order the instruction defines: */
    uint32_t edi, esi, ebp, esp_ignored, ebx, edx, ecx, eax;

    /* Pushed by our stub (uniform for all vectors): */
    uint32_t int_no;
    uint32_t err_code;

    /* Pushed by the CPU on exception / interrupt entry: */
    uint32_t eip, cs, eflags;

    /* Only present when the interrupt caused a privilege change (ring 3 →
     * ring 0).  We don't use ring 3 yet, so these are effectively unused,
     * but kept for completeness when user-mode lands. */
    uint32_t user_esp, ss;
} __attribute__((packed));

#endif /* __x86_64__ */

/* Handler signature for IRQ lines (32..47 after remap).  The handler runs
 * with interrupts disabled; the dispatcher sends the PIC EOI after it
 * returns.  Take the frame by pointer so handlers can inspect CPU state
 * if they ever want to. */
typedef void (*irq_handler_t)(struct int_frame* f);

/* Build the IDT, install gates for vectors 0..47, remap the PIC, then
 * `lidt`.  Does NOT enable interrupts (callers do `sti` when ready). */
void idt_init(void);

/* Per-CPU `lidt` (M18.5).  IDTR is a per-CPU register even though the
 * IDT data itself is shared.  APs call this from their C entry to
 * start accepting interrupts on their own LAPIC. */
void idt_load(void);

/* Register a handler for IRQ line `irq` (0..15).  Passing NULL removes a
 * previously registered handler.  Multiple registrations on the same IRQ
 * overwrite each other — there is no chaining today. */
void irq_install(int irq, irq_handler_t handler);

/* Switch the IRQ delivery path from the 8259 PIC to the IOAPIC + LAPIC
 * pair (M18).  After this call:
 *   - new `irq_install` invocations program the IOAPIC redirection
 *     table instead of unmasking a PIC line;
 *   - already-installed handlers (PIT, PS/2) are re-routed transparently;
 *   - the EOI in `isr_handler` goes to the LAPIC;
 *   - both 8259s are masked off entirely.
 *
 * Idempotent in the sense that calling it twice is harmless, but only
 * the first call does the real work; subsequent calls just re-route. */
void idt_use_apic(uint8_t bsp_apic_id);

#endif
