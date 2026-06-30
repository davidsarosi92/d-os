/* =============================================================================
 * hal_api.h — arch-independent HAL interface (M17 portability cut).
 *
 * Every call in `kernel/core/`, `kernel/mem/`, `kernel/fs/`, and any
 * portable driver that needs to touch CPU state, interrupt flags, or
 * the arch-specific task setup goes through THIS header — never
 * through `<arch>/gdt.h`, `<arch>/idt.h`, or raw `__asm__`.
 *
 * Implementations live under `kernel/hal/<arch>/`.  For now only
 * `kernel/hal/x86/` ships; x86_64 and aarch64 ports plug in here by
 * providing their own implementations of the same prototypes.
 *
 * Where `hal_api.h` is silent — e.g. PIC EOI, port I/O, virtual memory
 * mapping — the caller is necessarily arch-specific too.  That's fine:
 * PC-only drivers (8042 keyboard, 8259 PIC, PIT) keep using `hal.h`'s
 * x86 helpers (inb/outb/...) directly.  Future portability milestones
 * will widen the interface (notably: `hal_map`/`hal_unmap` to replace
 * the x86-specific vmm.c when x64 paging arrives).
 *
 * Design constraints (carry-over from PLAN §P):
 *   - Functions, not macros (so the impl can be a real symbol with a
 *     prologue, and `gdb` shows it on backtraces).
 *   - No core code may include arch-specific headers (gdt.h, idt.h,
 *     tss.h, ...) directly.  If a
 *     core file needs an arch service that isn't here yet, the
 *     interface gets added rather than the include leak being papered
 *     over.
 *   - `uintptr_t` for any address-shaped value the core hands across
 *     the boundary, so x64 (8-byte) and aarch64 (8-byte) ports don't
 *     require any source change in the core caller.
 * ============================================================================= */

#ifndef HAL_API_H
#define HAL_API_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * CPU control.
 *
 * `hal_cpu_halt` parks the CPU until the next interrupt.  Combined with
 * `hal_intr_enable` it forms the idle loop pattern.
 *
 * `hal_cpu_pause` is a hint that we're in a busy-wait — the CPU may use
 * it to relax the pipeline (x86 `pause`, ARM `yield`).  No-op semantics
 * are also acceptable.
 * --------------------------------------------------------------------------- */
void hal_cpu_halt(void);
void hal_cpu_pause(void);

/* Atomically enable interrupts AND halt until the next one fires.
 * Critical for blocking-read loops: a separate enable + halt would
 * race against an interrupt posted between the two instructions, so
 * the HAL guarantees an atomic pair (on x86: the `sti; hlt` pair the
 * CPU treats as un-interruptible by design).  Returns when an IRQ
 * has been delivered. */
void hal_cpu_idle(void);

/* ---------------------------------------------------------------------------
 * Interrupt flag control.
 *
 * `hal_intr_enable` / `hal_intr_disable` directly set/clear the CPU's
 * interrupt-enable flag (x86 `sti`/`cli`, ARM `cpsie i`/`cpsid i`).
 *
 * `hal_intr_save` returns the prior flag state and disables interrupts
 * atomically.  Pass that value back into `hal_intr_restore` to undo —
 * mirroring Linux's `local_irq_save`/`restore` pattern.  The token is
 * opaque; treat it as an integer cookie.
 * --------------------------------------------------------------------------- */
void     hal_intr_enable(void);
void     hal_intr_disable(void);
uint32_t hal_intr_save(void);
void     hal_intr_restore(uint32_t cookie);

/* ---------------------------------------------------------------------------
 * Arch bring-up.
 *
 * Called once from `kernel_main` very early — installs the CPU's
 * descriptor / interrupt tables and any per-arch one-time setup
 * (x86: GDT + IDT + TSS; arm64: vector table + EL setup).  After this
 * returns, IRQs may safely be `hal_intr_enable`d, and `hal_intr_save`
 * is well-defined.
 * --------------------------------------------------------------------------- */
void hal_arch_early_init(void);

/* ---------------------------------------------------------------------------
 * Task stack setup.
 *
 * Pre-build a fresh kernel stack so that the first `context_switch`
 * into this task lands in an arch-specific trampoline that:
 *   1. enables interrupts (so the new task is preemptible), and
 *   2. calls `entry()` once.
 *
 * Returns the value the scheduler should store in `task->esp`
 * (or whatever the arch calls its saved stack pointer).
 *
 * `stack_top` points ONE PAST the top of the allocated stack region —
 * i.e. `(char*)stack_base + kstack_size`.  The HAL writes downward from
 * there.
 * --------------------------------------------------------------------------- */
uintptr_t hal_task_init_stack(void* stack_top, void (*entry)(void));

/* ---------------------------------------------------------------------------
 * Identity-map extension (M19.5.1).
 *
 * Extend the kernel's physical → virtual identity map to cover at least
 * the given physical end address.  Called from pmm_init BEFORE the
 * mmap walk so the PMM can dereference (zero, free-list-thread) any
 * frame it manages.
 *
 * Returns the new physical end address actually covered (rounded up to
 * the arch's page-table granularity).  May be LESS than the requested
 * end if the arch cannot extend further (e.g. i386 today: identity is
 * fixed at 256 MiB by boot.s + linker layout; kmap is the right answer
 * but isn't shipped yet, so the i386 impl returns the existing cap and
 * pmm_init caps managed memory there).
 *
 * Today:
 *   - x86_64: uses 1 GiB pages in PDPT[1..] up to BUDDY_MAX_FRAMES.
 *   - i386:   returns IDENTITY_MAP_MIB << 20 (no-op).
 * --------------------------------------------------------------------------- */
uintptr_t hal_extend_identity_map(uintptr_t end_phys);

/* ---------------------------------------------------------------------------
 * Syscall epilogue helper.
 *
 * SYS_EXIT bypasses the normal iret-back-to-ring-3 path: it restores a
 * saved kernel stack pointer and jumps to a saved return address.
 * Both halves of that operation are arch-specific (the saved state is
 * laid out by the arch's `enter_user_mode_wrap`); this helper performs
 * the jump.  Noreturn.
 *
 * x86_64 / aarch64 ports may rename or split this; we keep the i386
 * shape concrete because there's only one caller today.
 * --------------------------------------------------------------------------- */
void hal_syscall_exit_to_kernel(uintptr_t saved_sp, uintptr_t saved_pc)
    __attribute__((noreturn));

#endif
