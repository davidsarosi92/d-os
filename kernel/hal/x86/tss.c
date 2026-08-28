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

/* Ports 0..0x3FF, one bit each.  See the bitmap's own comment for why this is
 * a deliberate ceiling and not a shortcut. */
#define IOMAP_PORTS 0x400
#define IOMAP_BYTES (IOMAP_PORTS / 8)

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

    /* ---- §M33 Tier 1: the I/O permission bitmap -----------------------------
     * A ring-3 IN/OUT is allowed only if its port's bit here is CLEAR and the
     * byte is inside the TSS limit.  Both halves matter, and the second is the
     * one doing the security work: **a port beyond the bitmap is denied by the
     * limit**, so covering only 0..0x3FF means everything above it is refused
     * BY CONSTRUCTION rather than by a check somebody has to remember.
     *
     * 0x400 ports is the legacy ISA range — PS/2, serial, the PIC, the PIT.  A
     * PCI I/O BAR (AC97's lives around 0xC000) is therefore NOT grantable to
     * ring 3, and that lines up with reality rather than limiting it: every
     * driver with a high I/O BAR here is a DMA driver, and a DMA driver cannot
     * be isolated at all until there is an IOMMU (§M33 stage 5).
     *
     * The trailing 0xFF byte is required by the SDM: the CPU may read one byte
     * past the port's own, and that read must deny. */
    uint8_t  iomap[IOMAP_BYTES];
    uint8_t  iomap_tail;
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
        /* No bitmap by default: iomap_base points past the segment limit, so
         * every ring-3 IN/OUT #GPs.  A task with no port grant gets exactly
         * this, which is what keeps the default unchanged from before §M33. */
        tss[c].iomap_base = sizeof(struct tss32);
        for (uint32_t i = 0; i < IOMAP_BYTES; i++) tss[c].iomap[i] = 0xFF;
        tss[c].iomap_tail = 0xFF;
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

/* ----------------------------------------------------------------------
 * §M33 Tier 1 — install a task's port grant.
 *
 * `bm` is IOMAP_BYTES of bitmap (0 = allowed) or NULL for "no ports", which is
 * every task that is not a user-mode driver.
 *
 * THE COPY IS SKIPPED WHEN NOTHING CHANGED, and that is not premature: this
 * runs on every context switch, and 128 bytes per switch on a box that has no
 * user-mode driver at all would be pure cost for a feature nobody enabled.
 * The cached owner is per-CPU because the TSS is.
 * ---------------------------------------------------------------------- */
static const void* iomap_loaded[TSS_MAX_CPUS];

void hal_set_io_bitmap(const void* bm) {
    int c = this_cpu_id();
    if (iomap_loaded[c] == bm) return;
    iomap_loaded[c] = bm;

    if (!bm) {
        tss[c].iomap_base = sizeof(struct tss32);   /* past the limit = deny */
        return;
    }
    const uint8_t* src = (const uint8_t*)bm;
    for (uint32_t i = 0; i < IOMAP_BYTES; i++) tss[c].iomap[i] = src[i];
    tss[c].iomap_tail = 0xFF;
    tss[c].iomap_base = (uint16_t)__builtin_offsetof(struct tss32, iomap);
}

/* ----------------------------------------------------------------------
 * §M33 Tier 2 — FORGET A BITMAP THAT IS ABOUT TO BE FREED.
 *
 * The cache above compares POINTERS, which is correct exactly as long as a
 * given address means the same bitmap forever.  Restarting a ring-3 driver
 * breaks that: the old bitmap is freed and a fresh one allocated, and the slab
 * hands back the SAME ADDRESS almost every time — so the cache would say
 * "already loaded" and skip the copy, leaving the TSS holding the DEAD
 * driver's permissions.
 *
 * Today's two grants happen to be identical, so the stale copy would be right
 * by accident and nothing would look wrong.  That is precisely why it is worth
 * closing now: the first driver whose restart asks for a different window would
 * inherit the previous one's ports, and the symptom — a driver reading a port
 * it was never granted, with no error anywhere — is about as far from its cause
 * as a bug in this tree can get.
 *
 * Called before the free.  Plain stores, no lock: the entry is only ever a
 * "skip the copy" hint, so the worst a racing CPU can do is reload a bitmap it
 * already had.
 * ---------------------------------------------------------------------- */
void hal_io_bitmap_forget(const void* bm) {
    for (int c = 0; c < TSS_MAX_CPUS; c++)
        if (iomap_loaded[c] == bm) iomap_loaded[c] = NULL;
}

uint32_t hal_io_bitmap_bytes(void) { return IOMAP_BYTES; }
int       tss_max_cpus(void)          { return TSS_MAX_CPUS; }
