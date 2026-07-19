/* =============================================================================
 * hal_arch.c — x86_64 implementation of `kernel/includes/hal_api.h`.
 *
 * Mostly a line-for-line port of the i386 version; the differences:
 *   - `hal_intr_save`/`hal_intr_restore` use pushfq/popq instead of
 *     pushf/pop, and a uint64_t cookie instead of uint32_t-with-
 *     truncation.  We keep the API signature as uint32_t cookie (per
 *     hal_api.h) and check IF (bit 9) directly; the rest of rflags
 *     doesn't matter for restore.
 *   - `hal_syscall_exit_to_kernel` is a stub for now — there's no
 *     int 0x80 path in M20 phases 1-6.  Phase 7 (or M20.5) will land
 *     SYSCALL/SYSRET and a real implementation.
 *
 * The CPU control primitives (hlt, pause, sti, cli) are encoded
 * identically in 32-bit and 64-bit mode — no REX prefix needed —
 * so the inline asm is byte-for-byte the same.
 * ============================================================================= */

#include "hal_api.h"
#include "gdt.h"
#include "idt.h"
#include "tss.h"
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * CPU control.
 * --------------------------------------------------------------------------- */

void hal_cpu_halt(void) {
    __asm__ volatile ("hlt");
}

void hal_cpu_pause(void) {
    __asm__ volatile ("pause");
}

void hal_cpu_idle(void) {
    /* Same atomic sti+hlt pair as i386.  The one-instruction window
     * between `sti` and `hlt` is guaranteed by the CPU per SDM Vol 2;
     * see i386 hal_arch.c for the long explanation. */
    __asm__ volatile ("sti; hlt" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Interrupt flag.
 *
 * pushfq pushes the full 64-bit RFLAGS register; we keep only the IF
 * bit (bit 9, mask 0x200) because that's all hal_intr_restore needs.
 * Truncating to uint32_t is safe — bit 9 lives in the low half.
 * --------------------------------------------------------------------------- */

void hal_intr_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

void hal_intr_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

uint32_t hal_intr_save(void) {
    uint64_t fl;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    return (uint32_t)fl;
}

void hal_intr_restore(uint32_t cookie) {
    if (cookie & 0x200) __asm__ volatile ("sti" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Arch bring-up.  Same TSS-first / GDT-second / IDT-last order as
 * i386, for the same reason: GDT needs the TSS address baked in.
 * --------------------------------------------------------------------------- */

/* Arm the SYSCALL/SYSRET fast-syscall path (kernel/hal/x86_64/syscall.c) so
 * `syscall` from ring 3 (x86_64 musl / Linux-ABI) reaches the kernel. */
extern void syscall_init_64(void);

/* Enable SSE/SSE2.  On x86_64 SSE2 is BASELINE — gcc (and therefore every musl/
 * Linux binary the cross-toolchain produces) emits SSE for floating point AND
 * for bulk memory ops, so ring-3 code faults with #UD unless the OS turns the
 * unit on.  The kernel itself is built -mno-sse and never touches XMM, so we do
 * NOT yet FXSAVE/FXRSTOR the XMM file across context switches — safe as long as
 * at most one SSE-using user task runs at a time (today's excursion model).
 * Concurrent SSE user tasks need per-task FXSAVE state — a follow-up.
 *   CR0: clear EM (bit 2, "emulate FPU"), set MP (bit 1, "monitor coproc").
 *   CR4: set OSFXSR (bit 9) + OSXMMEXCPT (bit 10). */
static void enable_sse(void) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ull << 2);
    cr0 |=  (1ull << 1);
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10);
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
}

void hal_arch_early_init(void) {
    tss_init();
    gdt_init();
    idt_init();
    enable_sse();
    syscall_init_64();
}

/* ---------------------------------------------------------------------------
 * Syscall epilogue — SYS_EXIT teleport from a ring-3 program back to
 * the kernel context saved by `enter_user_mode_wrap` (usermode.s).
 *
 * Same shape as i386's `hal_syscall_exit_to_kernel`: load RSP with the
 * stashed kernel value, then jump to the stashed return label.  This
 * abandons the interrupt frame on the syscall stack (TSS.RSP0 →
 * syscall_stack), which is fine because the next ring-3 → ring-0
 * transition will reset RSP from TSS.RSP0 again.
 *
 * `noreturn` — the caller never sees control come back.
 * --------------------------------------------------------------------------- */

__attribute__((noreturn))
void hal_syscall_exit_to_kernel(uintptr_t saved_sp, uintptr_t saved_pc) {
    __asm__ volatile (
        "movq %0, %%rsp\n\t"
        "jmpq *%1\n\t"
        :
        : "r"(saved_sp), "r"(saved_pc)
        : "memory"
    );
    __builtin_unreachable();
}
