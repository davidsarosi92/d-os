/* =============================================================================
 * hal_arch.c — AArch64 implementation of the arch-independent HAL API
 * (hal_api.h) for the M21 port (Phase C).
 *
 * These are the ARM equivalents of the x86 hal_arch.c: CPU control,
 * interrupt-flag save/restore, one-time arch bring-up, and the identity-map
 * extension hook the PMM calls.  Everything is small inline system-register
 * work; no MMIO here (the GIC/timer/UART live in their own files).
 *
 * Interrupt masking uses PSTATE.DAIF bit I (IRQ).  `msr daifset, #2` masks
 * IRQs, `msr daifclr, #2` unmasks them — the ARM analogue of x86 cli/sti.
 * Reading DAIF back via `mrs x, daif` returns the bits at [9:6] (D,A,I,F),
 * so the IRQ-enable state is bit 7 (I): 0 = enabled, 1 = masked.
 * ============================================================================= */

#include "hal_api.h"
#include <stdint.h>

/* Provided by exceptions.c / mmu.c. */
void exceptions_init(void);
void mmu_init(void);

/* ---- CPU control ----------------------------------------------------------- */

/* Park the CPU until an interrupt (WFI wakes even on a masked IRQ, but we
 * only halt in the idle loop with IRQs enabled). */
void hal_cpu_halt(void) {
    __asm__ volatile ("wfi");
}

/* Power off / reboot via PSCI (same HVC conduit smp.c uses for CPU_ON).  The
 * x86 ports poke ACPI / the keyboard controller; on QEMU `virt` firmware
 * exposes PSCI SYSTEM_OFF (0x84000008) and SYSTEM_RESET (0x84000009). */
void hal_shutdown(void) {
    register uint64_t x0 __asm__("x0") = 0x84000008;   /* PSCI_SYSTEM_OFF */
    __asm__ volatile ("hvc #0" : "+r"(x0) :: "memory");
    for (;;) __asm__ volatile ("wfi");
}
void hal_reboot(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009;   /* PSCI_SYSTEM_RESET */
    __asm__ volatile ("hvc #0" : "+r"(x0) :: "memory");
    for (;;) __asm__ volatile ("wfi");
}

/* Busy-wait relax hint — ARM `yield` (the x86 `pause` analogue). */
void hal_cpu_pause(void) {
    __asm__ volatile ("yield");
}

/* Atomically enable IRQs and wait for one.  The unmask + WFI pair is the
 * idle/block primitive; an IRQ posted between them still wakes WFI, so
 * there is no lost-wakeup window. */
void hal_cpu_idle(void) {
    __asm__ volatile ("msr daifclr, #2\n\t wfi" ::: "memory");
}

/* ---- interrupt flag control ------------------------------------------------ */

void hal_intr_enable(void) {
    __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

void hal_intr_disable(void) {
    __asm__ volatile ("msr daifset, #2" ::: "memory");
}

/* Disable IRQs and return the PRIOR enable state as an opaque cookie
 * (1 = IRQs were enabled, 0 = already masked). */
uint32_t hal_intr_save(void) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif\n\t msr daifset, #2"
                      : "=r"(daif) :: "memory");
    return (daif & (1u << 7)) ? 0u : 1u;
}

/* Undo hal_intr_save: re-enable IRQs only if they were enabled before. */
void hal_intr_restore(uint32_t cookie) {
    if (cookie) __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

/* ---- arch bring-up --------------------------------------------------------- */

/* One-time early init — installs the EL1 exception vectors and turns the
 * MMU + caches on.  The ARM analogue of x86 gdt/idt/tss init.  After this,
 * IRQs may safely be unmasked and hal_intr_save is well-defined. */
void hal_arch_early_init(void) {
    exceptions_init();
    mmu_init();
}

/* ---- identity-map extension (M19.5.1 hook) --------------------------------- */

/* The Phase-A MMU already identity-maps the whole 0..4 GiB window with 1 GiB
 * blocks (device below 0x4000_0000, Normal RAM above), so any physical frame
 * the PMM manages is already mapped.  Just report the requested end back
 * (capped at the 4 GiB we cover). */
uintptr_t hal_extend_identity_map(uintptr_t end_phys) {
    const uintptr_t covered = 0x100000000ULL;   /* 4 GiB */
    return end_phys < covered ? end_phys : covered;
}

/* ---- syscall epilogue (EL0 userspace) -------------------------------------- */

/* SYS_EXIT's fast return to the kernel (M21 Phase L).  x86 passes the saved
 * kernel SP/PC as arguments; on AArch64 aarch64_enter_user stashes them in
 * usermode.S globals, so this HAL entry just delegates to the teleport helper
 * (the args are accepted for a uniform HAL signature but unused here).  Kept so
 * the portable syscall shape is identical across the three arches. */
void aarch64_user_exit(void);
void hal_syscall_exit_to_kernel(uintptr_t saved_sp, uintptr_t saved_pc) {
    (void)saved_sp; (void)saved_pc;
    aarch64_user_exit();                 /* does not return */
    __builtin_unreachable();
}

/* Tier B — per-task ring-3/EL0 → kernel stack.  On aarch64 the exception-entry
 * stack is SP_EL1, the ordinary EL1 stack pointer that context_switch already
 * saves/restores per task, so it tracks the current task automatically — no TSS
 * equivalent to set.  This hook is therefore a no-op here (it exists for x86,
 * which needs TSS.esp0/rsp0 written on every switch-in). */
void hal_set_kernel_stack(uintptr_t top) {
    (void)top;
}
