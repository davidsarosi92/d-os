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

/* ---------------------------------------------------------------------------
 * Per-task privilege-transition stack (Tier B — concurrent user processes).
 *
 * When a ring-3 / EL0 task takes a syscall or interrupt, the CPU switches to a
 * kernel stack: TSS.esp0/rsp0 on x86, SP_EL1 on aarch64.  With several user
 * tasks preemptible at once, that stack must be PER-TASK, so the scheduler sets
 * it on every switch-in via this hook:
 *
 *   - `top != 0`  → this is an independent user task; use its own kernel-stack
 *                   top (one past the end) for the next ring-3→ring-0 entry.
 *   - `top == 0`  → a kernel thread (or the excursion-model self-tests, which
 *                   rely on a dedicated fixed syscall stack); restore the arch
 *                   default.
 *
 * x86 (i386/x86_64): writes TSS.esp0/rsp0.  aarch64: a no-op — SP_EL1 is the
 * ordinary EL1 stack pointer, which context_switch already saves/restores per
 * task, so it tracks automatically. */
void hal_set_kernel_stack(uintptr_t top);

/* M35 TLS — set the base of the running CPU's user thread-local-storage
 * segment (the segment a ring-3 `__thread` access reads through: %gs on i386).
 * The scheduler calls this on switch-in to a thread that set a TLS pointer.
 * i386 rewrites this CPU's user-TLS GDT descriptor's base; other arches that
 * lack a segmented TLS model provide a stub (they'll use a register-based
 * thread pointer when their libc port lands). */
void hal_set_tls_base(uintptr_t base);

/* §M39 — optional hardware RNG seed source.  Write one random 32-bit word to
 * *out and return 1, or return 0 if no hardware RNG is available.  x86 uses
 * RDRAND (CPUID-gated); arches without one leave the weak no-op in random.c
 * (the CSPRNG then seeds from timing jitter only). */
int hal_hw_random(uint32_t* out);

/* ---------------------------------------------------------------------------
 * Per-task FPU / SIMD state (2026-08-01).
 *
 * The floating-point and vector register file is NOT part of the integer
 * context `context_switch` swaps, so without this the register file is simply
 * whatever the previous task left behind.  Two ring-3 programs doing FP work
 * then silently corrupt each other's arithmetic — and on SMP a task migrated to
 * another core resumes on that core's register file, which is worse because it
 * is timing-dependent and unreproducible.
 *
 * The scheduler calls save(prev) + restore(next) around every context switch.
 * The state blob is an opaque, arch-sized byte array carried inside struct task
 * (see HAL_FPU_STATE_SIZE); the HAL aligns inside the blob, so the core does not
 * need to know an arch's alignment rule — it just hands over the array.
 *
 * hal_fpu_init_state MUST be called once per task before its first restore: a
 * zero-filled blob is NOT a valid FPU image on x86 (it would restore an all-zero
 * MXCSR, i.e. every SIMD exception UNMASKED, and #XF on the first FP operation).
 *
 * Sizing: 512 B is the x86 FXSAVE area, 528 B covers AArch64's 32×16-byte
 * vector registers plus FPCR/FPSR.  The extra slack lets the HAL align the
 * start of the area within the blob without a separate allocation.
 * ------------------------------------------------------------------------- */
#define HAL_FPU_STATE_SIZE   576

void hal_fpu_init_state(void* blob);   /* make `blob` a valid initial image  */
void hal_fpu_save(void* blob);         /* current CPU state → blob            */
void hal_fpu_restore(void* blob);      /* blob → current CPU state            */

/* Self-test support for `fputest`.  stamp() puts a 64-bit pattern into a LIVE
 * FP/SIMD register and read() reads that same register back, so a test can
 * prove the register file really is per-task by holding a value across many
 * yields while another task holds a different one.  Keeping the register poke
 * behind the HAL is what lets the test itself live in core code (no __asm__
 * outside kernel/hal/).  hal_fpu_present() reports 0 on an arch whose FP unit
 * is unreachable (aarch64 today), so the test SKIPs instead of lying.
 * Pass a bit pattern that is a well-behaved double — i386 holds it in an x87
 * register, where a NaN payload would not survive the round trip. */
int      hal_fpu_present(void);
void     hal_fpu_test_stamp(uint64_t v);
uint64_t hal_fpu_test_read(void);

/* ---------------------------------------------------------------------------
 * Architecture identity (2026-08-01).
 *
 * The short name of the architecture this kernel was built for: "i386",
 * "x86_64", "aarch64".  It is the SAME string a package uses to declare which
 * build of itself a payload is (pkg_recipe.arch) and the one `uname` reports,
 * so there is exactly one spelling of an arch in the system.
 * ------------------------------------------------------------------------- */
const char* hal_arch_name(void);

/* Can this kernel EXECUTE an ELF with this word size and machine type?
 * `cls` is EI_CLASS (1 = 32-bit, 2 = 64-bit), `machine` is e_machine
 * (3 = EM_386, 62 = EM_X86_64, 183 = EM_AARCH64).
 *
 * This is a real question, not a formality: nothing about loading a foreign
 * ELF fails on its own.  The loader will happily map a 32-bit image on a
 * 64-bit kernel and jump into it, and the CPU then executes 32-bit encodings
 * in 64-bit mode — undefined behaviour that surfaces as an unrelated-looking
 * fault far from the cause.  Asking the arch up front turns that into one
 * clear error.
 *
 * A 64-bit kernel COULD gain the ability to run 32-bit binaries, but it needs
 * more than a permissive check here (a compat syscall entry, 32-bit user
 * segments, a 32-bit ABI personality), so today each arch answers only for its
 * own native pair.  When that work lands, this function is the one place that
 * changes. */
int hal_elf_can_exec(unsigned cls, unsigned machine);

#endif
