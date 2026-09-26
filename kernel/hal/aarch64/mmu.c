/* =============================================================================
 * mmu.c — AArch64 stage-1 translation (M21; relocated kernel since §M85).
 *
 * THE LAYOUT (4 KiB granule, 39-bit halves: T0SZ = T1SZ = 25)
 *
 *   TTBR1 (kernel, every CPU, never switched)
 *     slots 0..510   the DIRECT MAP: VA KERNEL_DIRECT_MAP_BASE + (PA -
 *                    aarch64_phys_offset), 1 GiB Normal blocks, only over RAM
 *                    the boot description names (holes stay unmapped);
 *     slot 511       the kernel IMAGE at KIMAGE_VBASE, through an L2 table of
 *                    2 MiB blocks onto wherever the image was loaded.
 *   TTBR0 (per process; this file's l1_table is the kernel-thread template)
 *     slots 0..3     the low 4 GiB identity-mapped as DEVICE memory — the
 *                    registers of every board this port knows live there;
 *                    RAM is never reached through it any more;
 *     slots 4..      user space (vmm.c).
 *
 * WHY THE KERNEL MOVED (§M85 stage 4, 2026-09-26).  It was linked at, and ran
 * identity-mapped at, 0x4008_0000 — which is RAM on `virt` and the GIC on
 * sbsa-ref, whose RAM begins at 1 TiB.  An identity map cannot even reach 1 TiB
 * with a 39-bit TTBR0.  So the image is linked high and loaded anywhere, and
 * the direct map is OFFSET by the RAM's base instead of starting at PA 0.
 *
 * THE SWITCH.  boot.S zeroes .bss and calls aarch64_mmu_early() at the image's
 * PHYSICAL address with the MMU off.  It builds the tables and turns the MMU on
 * with TTBR0 = a temporary IDENTITY map of the image (a 4-level walk,
 * T0SZ = 16, because the image's PA may be above 2^39); boot.S then jumps to
 * the image's VIRTUAL address and calls aarch64_mmu_drop_idmap(), which
 * restores the 39-bit TTBR0 template.  Secondaries repeat the middle step
 * (aarch64_mmu_enable_this_cpu) from smp_entry.S.
 *
 * EVERYTHING BEFORE THE JUMP RUNS WITH THE MMU OFF, and that constrains the
 * code: every data access is Device-nGnRnE, so an unaligned access FAULTS
 * (hence `strict-align` for this file); every global is reached PC-relatively,
 * i.e. at its physical address, which is also why the tables' own addresses
 * can be used as descriptor values here without conversion; and nothing may
 * print — the console's address is not known yet on a firmware boot.
 * ============================================================================= */

#pragma GCC target("strict-align")

#include <stdint.h>
#include "hal_api.h"
#include "efi_bootinfo.h"

/* ---- descriptor bit fields -------------------------------------------------- */
#define DESC_VALID      (1ULL << 0)
#define DESC_BLOCK      (1ULL << 0)   /* bits[1:0]=0b01: block at L1/L2         */
#define DESC_TABLE      (3ULL << 0)   /* bits[1:0]=0b11: next-level table       */
#define DESC_AF         (1ULL << 10)  /* Access Flag — unset ⇒ access faults    */
#define DESC_SH_INNER   (3ULL << 8)   /* Inner shareable (for Normal memory)    */
#define DESC_UXN        (1ULL << 54)
#define DESC_PXN        (1ULL << 53)
#define DESC_ATTR(idx)  (((uint64_t)(idx)) << 2)   /* MAIR attribute index      */

#define ATTR_DEVICE     0
#define ATTR_NORMAL     1

#define GIB             (1ULL << 30)
#define MIB2            (2ULL << 20)

uint64_t aarch64_phys_offset;         /* see hal_api.h                      */
uint64_t aarch64_kimage_voffset;
struct dos_bootinfo aarch64_bootinfo; /* copied here before anything allocates */
int      aarch64_have_bootinfo;
int      aarch64_uart_hold;           /* firmware boot: console unknown yet  */

static uint64_t l1_table[512]  __attribute__((aligned(4096)));  /* TTBR0 tmpl  */
static uint64_t l1_ttbr1[512]  __attribute__((aligned(4096)));  /* kernel half */
static uint64_t l2_kimage[512] __attribute__((aligned(4096)));  /* the image   */
static uint64_t idmap_l0[512]  __attribute__((aligned(4096)));  /* the switch  */
static uint64_t idmap_l1[512]  __attribute__((aligned(4096)));

extern char kernel_start[], kernel_end[];

static uint64_t normal_block(uint64_t pa) {
    return pa | DESC_BLOCK | DESC_AF | DESC_SH_INNER | DESC_ATTR(ATTR_NORMAL);
}

/* The CPU's physical address width, as TCR.IPS wants it (ID_AA64MMFR0_EL1
 * PARange uses the same encoding).  Was a constant 40 bits — exactly one bit
 * short of sbsa-ref's RAM at 2^40. */
static uint64_t ips_field(void) {
    uint64_t mmfr0;
    __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t pa = mmfr0 & 0xF;
    return (pa > 5 ? 5 : pa) << 32;
}

static uint64_t tcr_value(uint64_t t0sz) {
    return t0sz                 /* T0SZ                                       */
         | (1ULL << 8) | (1ULL << 10) | (3ULL << 12)   /* TTBR0 walk: WB, ISH */
         | (0ULL << 14)          /* TG0 = 4 KiB                                */
         | (25ULL << 16)         /* T1SZ = 25                                  */
         | (1ULL << 24) | (1ULL << 26) | (3ULL << 28)  /* TTBR1 walk: WB, ISH */
         | (2ULL << 30)          /* TG1 = 4 KiB — TG1's encoding is not TG0's:
                                  * 0b00 there is RESERVED, and the CPU does not
                                  * report that; the walks just go wrong      */
         | ips_field();
}

/* Program THIS CPU for the switch: MAIR, TCR (TTBR0 as a 48-bit identity
 * walk), TTBR0 = the identity map of the image, TTBR1 = the kernel half, and
 * the MMU + caches on.  Called at the image's PHYSICAL address (the BSP from
 * aarch64_mmu_early, each secondary from smp_entry.S). */
void aarch64_mmu_enable_this_cpu(void) {
    uint64_t mair = (0x00ULL << (8 * ATTR_DEVICE)) | (0xFFULL << (8 * ATTR_NORMAL));
    __asm__ volatile ("msr mair_el1, %0" :: "r"(mair));
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr_value(16)));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)idmap_l0));
    __asm__ volatile ("msr ttbr1_el1, %0" :: "r"((uint64_t)(uintptr_t)l1_ttbr1));
    __asm__ volatile ("dsb ish\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    uint64_t sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12);
    __asm__ volatile ("msr sctlr_el1, %0\n isb" :: "r"(sctlr));
}

/* Called once running at the image's VIRTUAL address: TTBR0 back to the 39-bit
 * kernel template.  The identity map of the image is no longer reachable after
 * this, which is the point — nothing may still depend on it. */
void aarch64_mmu_drop_idmap(void) {
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"(kptr_phys(l1_table)));
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr_value(25)));
    __asm__ volatile ("dsb ish\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
}

/* The MMU-off half of boot (see the header).  pa = physical address of
 * _start, va = its link address, arg = what the loader put in x0. */
void aarch64_mmu_early(uint64_t pa, uint64_t va, uint64_t arg) {
    /* The image is mapped with 2 MiB blocks from (pa - TEXT_OFFSET), so that
     * must be 2 MiB aligned — the EFI stub guarantees it, QEMU's `-kernel`
     * load at 0x4008_0000 satisfies it.  Violated, there is nothing sane to
     * do before a console exists: stop. */
    uint64_t text_offset = va & (MIB2 - 1);
    uint64_t base_pa = pa - text_offset;
    if (base_pa & (MIB2 - 1)) for (;;) __asm__ volatile ("wfe");

    aarch64_kimage_voffset = va - pa;
    aarch64_phys_offset    = base_pa & ~(GIB - 1);

    /* A firmware boot hands over a dos_bootinfo; copy it into the image while
     * it is plainly addressable, before the allocator can reuse its page. */
    if (arg && *(const volatile uint64_t*)(uintptr_t)arg == DOS_BOOTINFO_MAGIC) {
        const volatile uint8_t* src = (const volatile uint8_t*)(uintptr_t)arg;
        uint8_t* dst = (uint8_t*)&aarch64_bootinfo;
        for (uint64_t i = 0; i < sizeof aarch64_bootinfo; i++) dst[i] = src[i];
        aarch64_have_bootinfo = 1;
        aarch64_uart_hold = 1;           /* console address unknown until the
                                          * description has been read */
        /* The direct map starts at the LOWEST RAM, not at the GiB the image
         * happens to sit in: firmware loads us wherever it has room (sbsa-ref:
         * the second GiB of a bank starting at 1 TiB), and RAM below the
         * direct map's base is unreachable — the PMM put its metadata exactly
         * there on the first try and faulted at DM_BASE - 1 GiB. */
        for (uint32_t i = 0; i < aarch64_bootinfo.nmem && i < DOS_BI_MAXMEM; i++) {
            uint64_t b = aarch64_bootinfo.mem[i].base & ~(GIB - 1);
            if (b < aarch64_phys_offset) aarch64_phys_offset = b;
        }
    }

    /* TTBR0 template: the low 4 GiB, DEVICE. */
    for (uint64_t i = 0; i < 4; i++)
        l1_table[i] = (i * GIB) | DESC_BLOCK | DESC_AF | DESC_ATTR(ATTR_DEVICE) | DESC_UXN | DESC_PXN;

    /* The image: slot 511 → L2, 2 MiB blocks over [base_pa, kernel_end). */
    uint64_t span = (uint64_t)(kernel_end - kernel_start) + text_offset;
    for (uint64_t i = 0; i * MIB2 < span && i < 512; i++)
        l2_kimage[i] = normal_block(base_pa + i * MIB2);
    l1_ttbr1[511] = (uint64_t)(uintptr_t)l2_kimage | DESC_TABLE;

    /* The direct map's slot for the GiB the image was loaded into — RAM by
     * construction; the raw boot's device tree is read through it (a firmware
     * boot maps its described RAM before touching anything else). */
    uint64_t islot = ((base_pa & ~(GIB - 1)) - aarch64_phys_offset) >> 30;
    if (islot < 511) l1_ttbr1[islot] = normal_block(base_pa & ~(GIB - 1)) | DESC_UXN;

    /* The temporary identity map of the image, 4-level so any PA works. */
    idmap_l0[(base_pa >> 39) & 511] = (uint64_t)(uintptr_t)idmap_l1 | DESC_TABLE;
    idmap_l1[(base_pa >> 30) & 511] = normal_block(base_pa & ~(GIB - 1));

    aarch64_mmu_enable_this_cpu();
}

/* ---- after the switch -------------------------------------------------------- */

uint64_t* mmu_kernel_l1(void) { return l1_table; }

/* Map the 1 GiB Device block containing `va` into the kernel identity map —
 * reaching MMIO outside the low 4 GiB, e.g. `virt`'s ECAM at 0x40_1000_0000. */
void mmu_map_device_1gib(uint64_t va) {
    uint64_t idx = (va >> 30) & 0x1FF;
    l1_table[idx] = (idx << 30) | DESC_BLOCK | DESC_AF | DESC_ATTR(ATTR_DEVICE) | DESC_UXN | DESC_PXN;
    __asm__ volatile ("dsb ish\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
}

/* Direct-map every 1 GiB slot overlapping [base, base+size).  RAM below
 * aarch64_phys_offset cannot be in the direct map at all (it would need a
 * negative offset) and is skipped; slot 511 belongs to the image. */
void mmu_direct_map_range(uint64_t base, uint64_t size) {
    if (!size) return;
    uint64_t end = base + size;
    if (end <= aarch64_phys_offset) return;
    if (base < aarch64_phys_offset) base = aarch64_phys_offset;
    uint64_t first = (base - aarch64_phys_offset) >> 30;
    uint64_t last  = (end - aarch64_phys_offset + GIB - 1) >> 30;
    if (last > 511) last = 511;
    for (uint64_t i = first; i < last; i++)
        if (!(l1_ttbr1[i] & DESC_VALID))
            l1_ttbr1[i] = normal_block(aarch64_phys_offset + i * GIB) | DESC_UXN;
    __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
}

/* Without a description of the RAM: map contiguously up to end_phys. */
uint64_t mmu_direct_map_extend(uint64_t end_phys) {
    uint64_t cap = aarch64_phys_offset + 511 * GIB;
    if (end_phys > cap) end_phys = cap;
    mmu_direct_map_range(aarch64_phys_offset, end_phys - aarch64_phys_offset);
    return end_phys;
}

/* Kept for the boot banner: the old Phase-A entry point is now boot.S's job. */
void mmu_init(void) { }
