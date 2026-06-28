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

void hal_arch_early_init(void) {
    tss_init();
    gdt_init();
    idt_init();
}

/* ---------------------------------------------------------------------------
 * Syscall epilogue — stubbed for M20 phases 1-6.
 *
 * The int 0x80 path is not wired up on x86_64 yet (Phase 7 / M20.5
 * lands SYSCALL/SYSRET).  Until then any SYS_EXIT from a ring-3 task
 * would call here; we trap with a recognisable signature so it shows
 * up clearly in a serial log rather than silently misbehaving.
 *
 * Once SYSCALL lands this becomes a real swapgs + register-restore
 * path mirroring i386's ESP/EIP rewrite.
 * --------------------------------------------------------------------------- */

void hal_syscall_exit_to_kernel(uintptr_t saved_sp, uintptr_t saved_pc) {
    (void)saved_sp;
    (void)saved_pc;
    /* No ring-3 path on x86_64 yet.  Hard-stop so a misrouted SYS_EXIT
     * doesn't run off into garbage. */
    for (;;) __asm__ volatile ("cli; hlt");
}
