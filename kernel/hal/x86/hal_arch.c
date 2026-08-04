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
#include "hal.h"         /* inb/outb — CMOS NVRAM scratch (§M47) */
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

/* Enable SSE/SSE2 for ring 3 (i386).
 *
 * Until §M48 this kernel deliberately left CR4.OSFXSR clear, on the reasoning
 * that 32-bit userland was x87-only.  That held while everything in user space
 * was built by our own toolchain — but a PORTED library does not ask: Mesa's
 * i386 build uses SSE for its math and memory paths, and every one of those
 * instructions raised #UD.  The EGL client got as far as the Wayland handshake
 * and then died on an "Invalid Opcode" inside libEGL, which reads like a
 * corrupt binary rather than a disabled CPU feature.
 *
 * Per-CPU state, so it has to run on every core (see hal_arch_init_this_cpu).
 * FXSAVE/FXRSTOR already cover the XMM half automatically once OSFXSR is set —
 * fpu.c was written for exactly this day and needs no change.
 *
 *   CR0: clear EM (bit 2, "emulate FPU"), set MP (bit 1, "monitor coproc").
 *   CR4: set OSFXSR (bit 9) + OSXMMEXCPT (bit 10). */
static void enable_sse(void) {
    uint32_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2);
    cr0 |=  (1u << 1);
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9) | (1u << 10);
    __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4));
}

/* The per-CPU half of arch bring-up — everything an AP must repeat for itself.
 * Mirrors the x86_64 twin; missing it makes a task fault only once the load
 * balancer moves it onto a core that never ran this. */
void hal_arch_init_this_cpu(void) {
    enable_sse();
}

void hal_arch_early_init(void) {
    tss_init();
    gdt_init();
    idt_init();
    enable_sse();
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

/* ---------------------------------------------------------------------------
 * §M39 — hardware RNG (RDRAND).  CPUID.01H:ECX bit 30 advertises RDRAND; if
 * present we retry a few times (the instruction can transiently fail) before
 * giving up.  On a CPU/QEMU without it we return 0 and the CSPRNG falls back
 * to timing jitter.
 * --------------------------------------------------------------------------- */
static int cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1U), "c"(0U));
    return (ecx >> 30) & 1U;
}

int hal_hw_random(uint32_t* out) {
    static int checked = 0, have = 0;
    if (!checked) { have = cpu_has_rdrand(); checked = 1; }
    if (!have) return 0;
    for (int retry = 0; retry < 10; retry++) {
        uint32_t val;
        unsigned char ok;
        __asm__ volatile ("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
        if (ok) { *out = val; return 1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Architecture identity (see hal_api.h).
 * --------------------------------------------------------------------------- */
const char* hal_arch_name(void) { return "i386"; }

/* A 32-bit kernel runs 32-bit x86 binaries and nothing else.  Note there is no
 * "run a 64-bit binary" option to be permissive about — the CPU is in 32-bit
 * protected mode; a 64-bit image is not merely unsupported, it is unrunnable. */
int hal_elf_can_exec(unsigned cls, unsigned machine) {
    return cls == 1 /*ELFCLASS32*/ && machine == 3 /*EM_386*/;
}

/* ---------------------------------------------------------------------------
 * NVRAM scratch (§M47) — the battery-backed CMOS bytes behind the RTC.
 *
 * Indices 0x0E..0x7F are general-purpose RAM on an MC146818; the BIOS uses the
 * low part for its own configuration + checksum, so callers should stay in the
 * upper region.  We do not touch the BIOS checksum, which means a machine that
 * validates it may complain about "CMOS settings" — acceptable for a scratch
 * byte whose whole job is to survive events nothing else can.
 *
 * NMI note: bit 7 of port 0x70 disables NMI while set.  We deliberately keep it
 * CLEAR so the hardware watchdog's NMI (§M31 L3) is never masked by a crash
 * bookkeeping write — masking the lockup detector inside the crash reporter
 * would be a fine way to lose exactly the events we are trying to record.
 * --------------------------------------------------------------------------- */
#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT  0x71

int hal_nvram_read(unsigned idx, uint8_t* out) {
    if (!out || idx > 0x7F) return 0;
    outb(CMOS_INDEX_PORT, (uint8_t)(idx & 0x7F));
    *out = inb(CMOS_DATA_PORT);
    return 1;
}

int hal_nvram_write(unsigned idx, uint8_t val) {
    if (idx > 0x7F) return 0;
    outb(CMOS_INDEX_PORT, (uint8_t)(idx & 0x7F));
    outb(CMOS_DATA_PORT, val);
    return 1;
}
