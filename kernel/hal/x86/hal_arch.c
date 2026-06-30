/* =============================================================================
 * hal_arch.c — x86 implementation of `kernel/includes/hal_api.h`.
 *
 * Tiny by design: every function here is either an inline-asm wrapper
 * around a single instruction (sti/cli/hlt/pause) or a delegation to
 * an existing x86-internal init routine (gdt_init / idt_init / tss_init).
 *
 * Keeping it in one file makes the arch-↔-portable boundary obvious:
 * if you're reading this file you're already in `kernel/hal/x86/`.
 * Core code that touches CPU state at all goes through `hal_api.h`
 * which forwards here.
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
    /* x86 `hlt` parks until the next interrupt (or NMI/SMI). */
    __asm__ volatile ("hlt");
}

void hal_cpu_pause(void) {
    /* `pause` is a hint to the CPU that we're in a spin-loop; on older
     * processors it's a no-op (encoded as `rep nop`), on modern ones it
     * reduces speculative pipeline pressure. */
    __asm__ volatile ("pause");
}

void hal_cpu_idle(void) {
    /* Atomic `sti; hlt` pair.  Per Intel SDM Vol 2, `sti` blocks
     * interrupt recognition for ONE instruction boundary — the next
     * instruction (here: `hlt`) is guaranteed to begin before any IRQ
     * can be delivered.  That window is exactly what lets the consumer
     * "check the ring, then sleep until next IRQ" idiom be race-free
     * against an IRQ that fires between the check and the sleep. */
    __asm__ volatile ("sti; hlt" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Interrupt flag.
 *
 * We deliberately push/pop the FULL EFLAGS register rather than reading
 * the IF bit by hand — that way a debugger or future code that twiddles
 * other flags between save and restore doesn't get clobbered.
 * --------------------------------------------------------------------------- */

void hal_intr_enable(void) {
    __asm__ volatile ("sti" ::: "memory");
}

void hal_intr_disable(void) {
    __asm__ volatile ("cli" ::: "memory");
}

uint32_t hal_intr_save(void) {
    uint32_t fl;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    return fl;
}

void hal_intr_restore(uint32_t cookie) {
    /* Only restore IF — don't blindly popf the whole register, since
     * code between save and restore may have changed CF/ZF/... and we
     * shouldn't undo that.  Same pattern task.c used to inline before
     * M17. */
    if (cookie & 0x200) __asm__ volatile ("sti" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Arch bring-up.
 *
 * Order matters and is the same as the boot-prologue used to do
 * inline.  TSS first so its address can be baked into the GDT entry;
 * GDT second (which also loads TR); IDT last.
 * --------------------------------------------------------------------------- */

void hal_arch_early_init(void) {
    tss_init();
    gdt_init();
    idt_init();
}

/* ---------------------------------------------------------------------------
 * Identity-map extension (M19.5.1).
 *
 * No-op on i386 today.  vmm.c builds a 256 MiB identity map at boot via
 * 4 MiB PSE PDEs and we don't grow it: doing so would need either (a)
 * extending into kernel virtual address space we use for kmalloc /
 * vmm_map (a layout reshuffle), or (b) kmap-style on-demand temporary
 * mappings.  Neither is shipped — see PLAN §M19.5.1.  Returns the
 * existing identity cap so pmm_init caps managed memory at 256 MiB
 * on i386.
 *
 * On systems with > 256 MiB RAM, the kmap-less i386 PMM simply ignores
 * the upper frames.  Users hit by this should run x86_64 or wait for
 * kmap to land.
 * --------------------------------------------------------------------------- */
uintptr_t hal_extend_identity_map(uintptr_t end_phys) {
    (void)end_phys;
    return (uintptr_t)256u * 1024u * 1024u;     /* IDENTITY_MAP_MIB in vmm.c */
}

/* ---------------------------------------------------------------------------
 * Syscall epilogue — i386 ESP/EIP rewrite for SYS_EXIT.
 *
 * Reassigns the kernel stack pointer to the saved value and jumps to
 * the saved return address.  noreturn — the syscall path that called
 * us never sees control again.
 * --------------------------------------------------------------------------- */

void hal_syscall_exit_to_kernel(uintptr_t saved_sp, uintptr_t saved_pc) {
    __asm__ volatile (
        "mov %0, %%esp\n\t"
        "jmp *%1\n\t"
        :
        : "r"(saved_sp), "r"(saved_pc)
    );
    __builtin_unreachable();
}
