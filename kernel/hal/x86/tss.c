/* =============================================================================
 * tss.c — per-CPU Task State Segments (i386).
 *
 * The 386 TSS is a 104-byte structure with one field that matters today:
 * `esp0`, the kernel ESP the CPU swaps in when a ring-3 → ring-0
 * transition happens.
 *
 * SMP (M35 follow-up): each CPU needs its OWN TSS.  On i386 the CPU loads
 * `esp0` from whatever TSS its task register (TR) points at, and TR is
 * per-CPU state — so a single shared TSS would let CPU A's scheduler set an
 * `esp0` that CPU B then uses for a ring-3 → ring-0 trap, landing on the
 * wrong kernel stack.  (This is exactly why ring-3 user tasks used to hang on
 * an AP: the APs never even LTR'd a TSS, and there was only one to share.)
 * We therefore keep an array of TSSes — one per logical CPU — each with its
 * own dedicated syscall stack; every CPU LTR's its own descriptor (gdt.c
 * builds one TSS descriptor per CPU) and `hal_set_kernel_stack` writes
 * `tss[this_cpu_id()].esp0`.
 *
 * Reference: Intel SDM Vol 3, §7.2 (32-bit TSS).
 * ============================================================================= */

#include "tss.h"
#include "gdt.h"
#include "hal_api.h"
#include "acpi.h"          /* ACPI_MAX_CPUS */
#include "percpu.h"        /* this_cpu_id  */
#include <stdint.h>

/* Full 32-bit TSS layout per the SDM.  `__attribute__((packed))` is
 * critical; the CPU reads each field at its exact offset. */
struct tss32 {
    uint32_t prev_link;     /* 0x00 */
    uint32_t esp0;          /* 0x04 — kernel stack pointer */
    uint32_t ss0;           /* 0x08 — kernel stack segment */
    uint32_t esp1;          /* 0x0C */
    uint32_t ss1;           /* 0x10 */
    uint32_t esp2;          /* 0x14 */
    uint32_t ss2;           /* 0x18 */
    uint32_t cr3;           /* 0x1C */
    uint32_t eip;           /* 0x20 */
    uint32_t eflags;        /* 0x24 */
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp;
    uint32_t esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;           /* 0x60 */
    uint16_t trap;          /* 0x64 */
    uint16_t iomap_base;    /* 0x66 */
} __attribute__((packed));

/* One TSS + one dedicated ring-3→ring-0 syscall stack per logical CPU.  The
 * arrays live in .bss (zero-filled, no image cost).  Sized to the affinity
 * cap so any logical CPU index is valid. */
#define TSS_MAX_CPUS ACPI_MAX_CPUS
#define KSTACK_SIZE  4096

static struct tss32 tss[TSS_MAX_CPUS];
static uint8_t syscall_stack[TSS_MAX_CPUS][KSTACK_SIZE] __attribute__((aligned(16)));

static inline uint32_t fallback_esp0(int cpu) {
    return (uint32_t)(uintptr_t)(syscall_stack[cpu] + KSTACK_SIZE);
}

void tss_init(void) {
    /* Initialise EVERY CPU's TSS up front (the BSP calls this once, before
     * gdt_init, which needs the addresses for its per-CPU descriptors). */
    for (int c = 0; c < TSS_MAX_CPUS; c++) {
        uint8_t* b = (uint8_t*)&tss[c];
        for (uint32_t i = 0; i < sizeof(struct tss32); i++) b[i] = 0;
        tss[c].ss0  = GDT_KERNEL_DS;
        tss[c].esp0 = fallback_esp0(c);
        /* iomap_base = sizeof(TSS) → "no I/O bitmap": ring-3 IN/OUT #GP. */
        tss[c].iomap_base = sizeof(struct tss32);
    }
}

void tss_set_kernel_stack(uintptr_t esp) {
    tss[this_cpu_id()].esp0 = (uint32_t)esp;
}

/* Tier B — per-task ring-3→ring-0 stack, per CPU.  `top != 0` selects the
 * running user task's own kernel-stack top; `top == 0` restores this CPU's
 * dedicated fixed syscall stack (kernel threads + the excursion-model
 * self-tests, which saved a resume context on their own kstack). */
void hal_set_kernel_stack(uintptr_t top) {
    int c = this_cpu_id();
    tss[c].esp0 = top ? (uint32_t)top : fallback_esp0(c);
}

/* gdt.c builds one TSS descriptor per CPU from these. */
uintptr_t tss_get_addr(void)          { return (uintptr_t)&tss[0]; }   /* legacy */
uintptr_t tss_get_addr_cpu(int cpu)   { return (uintptr_t)&tss[cpu]; }
uint32_t  tss_get_limit(void)         { return sizeof(struct tss32) - 1; }
int       tss_max_cpus(void)          { return TSS_MAX_CPUS; }
