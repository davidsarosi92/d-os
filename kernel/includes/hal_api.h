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

/* The per-CPU half of arch bring-up: state that lives in a control register
 * or MSR and therefore has to be programmed again on every core. */
void hal_arch_init_this_cpu(void);

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
 * Physical <-> kernel-virtual window (§M48).
 *
 * The kernel constantly needs to touch a page it only has the PHYSICAL address
 * of — zeroing a fresh frame, threading a free-list link through it, reading a
 * page table.  Historically it just dereferenced the physical address, which
 * works only because low memory is identity-mapped.
 *
 * That identity map is also what broke 64-bit userland.  User programs are
 * linked at `vmm_user_base()` (1 GiB) — they cannot move, because they are
 * compiled with the small code model, which requires every symbol below 2 GiB.
 * So as soon as a machine had more than 1 GiB of RAM, the identity map's
 * 1 GiB page landed exactly on top of the user region and the page-table
 * walker refused to build user mappings under it.  Every x86_64 exec failed
 * with ELF_ENOMEM; the port only ever appeared to work because it was always
 * tested with -m 1024M.
 *
 * The fix is to stop competing for low addresses: on x86_64 all of physical
 * memory is mapped ONCE at KERNEL_DIRECT_MAP_BASE in the (kernel-only) upper
 * half, and physical dereferences go through `phys_to_virt`.  Low addresses
 * then belong entirely to user space, whatever the RAM size.
 *
 * On i386 and aarch64 the direct map base is 0, so this is the identity
 * mapping those ports already have and the calls compile away to nothing.
 * --------------------------------------------------------------------------- */
#if defined(__x86_64__)
/* Canonical upper half, PML4 slot 256.  Physical memory is capped well below
 * the 512 GiB a single PDPT of 1 GiB pages covers. */
#  define KERNEL_DIRECT_MAP_BASE  0xFFFF800000000000UL
#else
#  define KERNEL_DIRECT_MAP_BASE  0UL
#endif

/* Kernel-virtual pointer for a physical address.  Valid once the direct map is
 * installed (hal_extend_identity_map, called from pmm_init). */
static inline void* phys_to_virt(uint64_t phys) {
    return (void*)(uintptr_t)(phys + KERNEL_DIRECT_MAP_BASE);
}

/* Inverse — ONLY for pointers that came from phys_to_virt.  A kernel image or
 * stack address is not in the direct map and must not be passed here. */
static inline uint64_t virt_to_phys(const void* v) {
    return (uint64_t)(uintptr_t)v - KERNEL_DIRECT_MAP_BASE;
}

/* Physical address of ANY kernel pointer — direct-map or kernel-image/low.
 * Use this where an address is handed to hardware (a DMA descriptor), since
 * the buffer may equally be a kmalloc'd direct-map page or a static array in
 * the image.  virt_to_phys is the cheaper choice when the origin is known. */
static inline uint64_t kptr_phys(const void* v) {
    uintptr_t a = (uintptr_t)v;
    /* The test has to be a PREPROCESSOR one, not a runtime one: where the base
     * is 0 (every 32-bit arch), `a >= 0` is always true and gcc says so — in
     * every translation unit that includes this header, which on a clean build
     * is a hundred copies of one warning.  A real warning cannot be seen in
     * that, and §M57 had already paid for the lesson that a noisy build is a
     * build nobody reads. */
#if KERNEL_DIRECT_MAP_BASE
    if (a >= (uintptr_t)KERNEL_DIRECT_MAP_BASE)
        return (uint64_t)(a - (uintptr_t)KERNEL_DIRECT_MAP_BASE);
#endif
    return (uint64_t)a;
}

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

/* ---------------------------------------------------------------------------
 * TLB shootdown (§M51).
 *
 * Invalidate `va` — or, with va == 0, the whole address space — in the space
 * whose page-table root is `root_phys`, on EVERY CPU, not just this one.
 *
 * This exists because the two architectures disagree about whose job the
 * broadcast is.  AArch64 has `tlbi ...is`: the instruction reaches every core
 * in the inner-shareable domain, so its implementation is empty here and the
 * invalidation stays where the page-table edit is.  x86 has no such
 * instruction — `invlpg` and a CR3 reload are strictly local — so the
 * broadcast has to be built out of an inter-processor interrupt
 * (hal/x86/tlb.c).
 *
 * WHEN TO CALL IT.  After any edit that makes an existing translation WEAKER
 * or invalid — write→read-only, present→absent, or a remap to a different
 * frame.  Making a translation stronger (absent→present) needs nothing: a CPU
 * with no cached entry will walk the table and see the new one.
 *
 * `root_phys` rather than a `struct vmm_space*` so the HAL needs to know
 * nothing about either arch's page-table types.  Pass 0 to mean "whatever this
 * CPU currently has loaded".
 * ------------------------------------------------------------------------- */
void hal_tlb_shootdown(uintptr_t root_phys, uintptr_t va);

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

/* ---------------------------------------------------------------------------
 * Small non-volatile scratch (§M47).
 *
 * A handful of bytes that survive a RESET — including the resets nothing can be
 * logged through: a triple fault, a hardware reset, a power loss.  That is the
 * entire point: an "unclean shutdown" marker written here is the only way such
 * an event can ever be reported to the user, because by definition no code runs
 * at the moment it happens (see crash.h).
 *
 * x86 uses the battery-backed CMOS NVRAM behind the RTC.  Return 0 from both if
 * the platform has none — the caller then loses this one capability and says
 * so, rather than silently believing every boot was clean.
 * `idx` is a small byte index; the caller owns its own slot allocation.
 * ------------------------------------------------------------------------- */
int hal_nvram_read(unsigned idx, uint8_t* out);   /* 1 = read, 0 = unavailable */
int hal_nvram_write(unsigned idx, uint8_t val);   /* 1 = written               */

/* ----------------------------------------------------------------------
 * §M33 Tier 1 — per-task I/O port grant.
 *
 * Install `bm` (hal_io_bitmap_bytes() bytes, a 0 bit = permitted) as the
 * running task's ring-3 port permission, or NULL for "no ports at all", which
 * is every task that is not a user-mode driver.  Called from the context
 * switch.
 *
 * ON AARCH64 THIS IS EMPTY AND THAT IS THE ANSWER, not a gap: the architecture
 * has no port I/O, so there is no permission to grant.  A driver that needs
 * ports there is a driver for hardware that machine does not have, and
 * drv_ports_request already says DRV_ENOSYS.
 * ---------------------------------------------------------------------- */
void     hal_set_io_bitmap(const void* bm);
uint32_t hal_io_bitmap_bytes(void);

/* Drop any cached reference to `bm`, which the caller is about to free.  The
 * install above skips the copy when the pointer has not changed, and a freed
 * bitmap's address is reused by the very next allocation — so a driver restart
 * without this leaves the CPU holding the dead driver's permissions. */
void     hal_io_bitmap_forget(const void* bm);

#endif
