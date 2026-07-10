/* =============================================================================
 * mmu.c — AArch64 stage-1 MMU bring-up for EL1 (M21).
 *
 * The x86 ports enable paging in boot.s with a hand-built table; on ARM the
 * translation-table format + control registers are different enough that it
 * is far clearer to do it from C.  This is the AArch64 analogue of vmm.c's
 * early identity map.
 *
 * Strategy (Phase A — coarse but correct):
 *   - 4 KiB granule, 39-bit VA (T0SZ = 25) → the TTBR0 walk starts at level 1,
 *     where each entry maps a 1 GiB *block* directly.  That lets a SINGLE
 *     512-entry level-1 table identity-map the whole address space we care
 *     about with no lower-level tables at all.
 *   - index 0  (0x0000_0000..0x3FFF_FFFF): Device-nGnRnE — covers the QEMU
 *     `virt` peripheral window: the PL011 UART (0x0900_0000) and the GIC
 *     (0x0800_0000).  Device memory so MMIO is never cached/reordered.
 *   - index 1..3 (0x4000_0000..0xFFFF_FFFF): Normal write-back, inner
 *     shareable — the RAM window (`virt` RAM base is 0x4000_0000).
 *
 * MAIR_EL1 attribute slots:  Attr0 = Device-nGnRnE (0x00),
 *                            Attr1 = Normal WB, RA/WA (0xFF).
 *
 * A later M21 phase replaces this with a real page-granular vmm_map()/unmap()
 * behind the hal_map interface; this coarse map is enough to turn the MMU on
 * (required before caches, and before any SMP/atomic work).
 *
 * References: Arm ARM (DDI 0487) D8 "The AArch64 Virtual Memory System
 * Architecture" — descriptor formats, TCR_EL1/MAIR_EL1/TTBR0_EL1 fields.
 * ============================================================================= */

#include <stdint.h>

void uart_early_puts(const char* s);

/* ---- descriptor bit fields -------------------------------------------------- */
#define DESC_BLOCK      (1ULL << 0)   /* bits[1:0]=0b01: block at L1/L2         */
#define DESC_AF         (1ULL << 10)  /* Access Flag — unset ⇒ access faults    */
#define DESC_SH_INNER   (3ULL << 8)   /* Inner shareable (for Normal memory)    */
#define DESC_ATTR(idx)  (((uint64_t)(idx)) << 2)   /* MAIR attribute index      */

/* MAIR attribute indices (byte position in MAIR_EL1). */
#define ATTR_DEVICE     0
#define ATTR_NORMAL     1

/* The level-1 translation table.  512 × 8 bytes = 4 KiB, naturally aligned so
 * it can go straight into TTBR0_EL1.  Lives in .bss (zeroed by boot.S). */
static uint64_t l1_table[512] __attribute__((aligned(4096)));

/* Program THIS CPU's stage-1 translation registers from the (already-built)
 * level-1 table and enable the MMU + caches.  The translation table is shared
 * (one identity map for all CPUs), but MAIR/TCR/TTBR0/SCTLR are per-CPU system
 * registers, so every core — the BSP and each PSCI-started secondary — must
 * run this before it may touch shared cacheable memory (a lock taken with the
 * MMU off is non-cacheable and would not be coherent with the other cores). */
void mmu_enable_this_cpu(void) {
    /* MAIR_EL1: slot 0 = Device-nGnRnE (0x00), slot 1 = Normal WB WA RA (0xFF). */
    uint64_t mair = (0x00ULL << (8 * ATTR_DEVICE))
                  | (0xFFULL << (8 * ATTR_NORMAL));
    __asm__ volatile ("msr mair_el1, %0" :: "r"(mair));

    /* TTBR0_EL1 = physical base of the shared level-1 table. */
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)l1_table));

    /* TCR_EL1:
     *   T0SZ  = 25   → 39-bit VA (level-1 start, 1 GiB blocks)
     *   IRGN0 = 01   → walk memory inner write-back
     *   ORGN0 = 01   → walk memory outer write-back
     *   SH0   = 11   → walk memory inner shareable
     *   TG0   = 00   → 4 KiB granule
     *   EPD1  = 1    → disable the TTBR1 (upper-half) walks; we use TTBR0 only
     *   IPS   = 010  → 40-bit intermediate physical address (1 TiB) */
    uint64_t tcr = (25ULL)
                 | (1ULL << 8)
                 | (1ULL << 10)
                 | (3ULL << 12)
                 | (0ULL << 14)
                 | (1ULL << 23)
                 | (2ULL << 32);
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr));

    /* Ensure all the above are visible before the translation regime changes. */
    __asm__ volatile ("dsb ish\nisb");

    /* Enable the MMU + caches: SCTLR_EL1.M (bit0) | .C (bit2) | .I (bit12). */
    uint64_t sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile ("msr sctlr_el1, %0\nisb" :: "r"(sctlr));
}

/* Expose the shared kernel level-1 table so the per-process VMM (vmm.c) can
 * copy the kernel/device identity blocks (entries 0..3) into every user address
 * space — the kernel stays mapped in the low 4 GiB of every TTBR0, exactly as
 * the x86 ports keep the kernel mapped in every process's page directory.  User
 * mappings then live at VA >= 4 GiB (L1 index >= 4), which never collide with
 * these blocks. */
uint64_t* mmu_kernel_l1(void) { return l1_table; }

void mmu_init(void) {
    /* index 0 → device window (peripherals + GIC + UART). */
    l1_table[0] = (0x00000000ULL)
                | DESC_BLOCK | DESC_AF | DESC_ATTR(ATTR_DEVICE);

    /* index 1..3 → 3 GiB of Normal RAM starting at 0x4000_0000. */
    for (uint64_t i = 1; i < 4; i++) {
        l1_table[i] = (i << 30)
                    | DESC_BLOCK | DESC_AF | DESC_SH_INNER | DESC_ATTR(ATTR_NORMAL);
    }

    mmu_enable_this_cpu();
    uart_early_puts("aarch64: MMU + caches enabled (identity map)\n");
}
