/* =============================================================================
 * vmm.c — 4-level paging behind the vmm.h API (x86_64).
 *
 * boot.s already built a minimal page hierarchy before C started:
 *
 *   pml4[0]          → pdpt   (P + RW)
 *   pdpt[0]          → pd_low (P + RW)
 *   pd_low[0..511]   = i*2MiB | P+RW+PS    (identity 0..1 GiB via 2 MiB pages)
 *
 * That covers the kernel image, the PMM range, and any MMIO under 1 GiB
 * with zero further work — every pointer C code already holds keeps
 * resolving to the same physical address.
 *
 * vmm_init's job is to record those table addresses so `vmm_map` /
 * `vmm_unmap` can walk and modify them.  We DON'T copy the boot tables
 * — `pml4` lives in .bss, addressable for the lifetime of the kernel.
 *
 * Mapping operations: walk PML4[idx0] → PDPT[idx1] → PD[idx2] → PT[idx3],
 * allocating intermediate tables on demand from `pmm_alloc_frame`.  We
 * refuse to refine a slot that's already a large-page entry (PS=1) for
 * the same reason i386's vmm.c refuses to split a 4 MiB PSE PDE.
 *
 * Index breakdown (canonical 48-bit virtual addressing):
 *   virt[47:39]  PML4 idx (9 bits, 512 entries, 512 GiB each)
 *   virt[38:30]  PDPT idx (9 bits, 512 entries, 1 GiB each)
 *   virt[29:21]  PD   idx (9 bits, 512 entries, 2 MiB each)
 *   virt[20:12]  PT   idx (9 bits, 512 entries, 4 KiB each)
 *   virt[11:0]   offset inside 4 KiB page
 *
 * Reference: AMD64 APM Vol 2 §5.3 (4-Level Paging Translation).
 * ============================================================================= */

#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include <stdint.h>

/* Page-table entry bit positions.  Intel kept the low 12 bits compatible
 * with i386 PDEs, so VMM_WRITABLE/USER/WRITE_THRU/CACHE_DIS flow through
 * directly.  PS (bit 7) is the "large page" marker; on the PD level that
 * means a 2 MiB page, on the PDPT level a 1 GiB page. */
#define PTE_P     0x001
#define PTE_RW    0x002
#define PTE_US    0x004
#define PTE_PS    0x080
#define PTE_NX    (1ull << 63)           /* not used yet — Phase 7+ */

#define PAGE_MASK_4K  0x000FFFFFFFFFF000ull   /* 4 KiB-aligned phys addr */
#define PAGE_MASK_2M  0x000FFFFFFFE00000ull   /* 2 MiB-aligned phys addr */

#define IDX_PML4(v)   (((v) >> 39) & 0x1FFu)
#define IDX_PDPT(v)   (((v) >> 30) & 0x1FFu)
#define IDX_PD(v)     (((v) >> 21) & 0x1FFu)
#define IDX_PT(v)     (((v) >> 12) & 0x1FFu)

/* boot.s exports these three frames as symbols.  Each is exactly one
 * 4 KiB page in .bss, aligned by the linker. */
extern uint8_t pml4[];
extern uint8_t pdpt[];
extern uint8_t pd_low[];

static inline uint64_t* table_at(uintptr_t phys) {
    /* We're identity-mapped over the PMM range, so phys-as-virt is
     * directly addressable.  Strip the low 12 bits (PTE flags). */
    return (uint64_t*)(uintptr_t)(phys & PAGE_MASK_4K);
}

static inline void invlpg(uintptr_t virt) {
    __asm__ volatile ("invlpg (%0)" : : "r"((void*)virt) : "memory");
}

/* -----------------------------------------------------------------------------
 * Init.
 *
 * Paging is already on (boot.s left CR0.PG = 1 and CR3 = pml4).  We
 * just announce ourselves and verify the inheritance.
 * ----------------------------------------------------------------------------- */

void vmm_init(void) {
    kprintf("vmm: 4-level paging active, PML4 @ %p, "
            "identity 1 GiB via 2 MiB pages (inherited from boot)\n",
            (void*)pml4);
}

uintptr_t vmm_kernel_pd_phys(void) {
    /* Top-level page table — PML4 on x86_64.  AP trampoline writes
     * this into CR3 before enabling paging.  Identity-mapped, so the
     * pointer is also the physical address. */
    return (uintptr_t)pml4;
}

/* -----------------------------------------------------------------------------
 * Page-table walker.
 *
 * Returns a pointer to the PT (level 3) that contains the entry for
 * `virt`, allocating intermediate tables as needed.  Returns NULL on
 * allocation failure or if a parent slot is already a large-page
 * entry that we'd have to split.
 *
 * `parent_flags` is OR'd into newly-created intermediate entries —
 * mainly so the USER bit propagates up the chain (without USER on
 * every parent, ring-3 code can't reach the PT).
 * ----------------------------------------------------------------------------- */
static uint64_t* walk_to_pt(uintptr_t virt, int create, uint32_t parent_flags) {
    uint64_t* tbl = (uint64_t*)pml4;
    int shifts[3] = { 39, 30, 21 };          /* PML4, PDPT, PD */

    for (int level = 0; level < 3; level++) {
        unsigned idx = ((unsigned)(virt >> shifts[level])) & 0x1FFu;
        uint64_t  e   = tbl[idx];

        if (e & PTE_P) {
            /* Refuse to split a large page (PS=1 valid only at PD or
             * PDPT levels; PML4 entries never set PS).  Returning NULL
             * mirrors i386's behaviour for the analogous 4 MiB PSE
             * case. */
            if (level >= 1 && (e & PTE_PS)) return 0;

            /* Widen the existing entry's permissions if the caller's
             * mapping is broader.  This matters in particular for
             * user mappings: the boot-time PML4[0]/PDPT[0]/PD[i]
             * entries are built with US=0 by boot.s (kernel-only); a
             * later user mapping under the same PML4 subtree would
             * still #PF in ring 3 because EVERY level of the long-
             * mode walk checks US.  OR'ing the bit in is safe (we're
             * only relaxing permissions, never tightening) — and the
             * pages below the bit are still controlled by their PT
             * entry's own US bit.
             *
             * For a kernel-only mapping (parent_flags & PTE_US = 0)
             * this is a no-op. */
            uint64_t widen = (uint64_t)parent_flags & PTE_US;
            if (widen && !(e & PTE_US)) {
                tbl[idx] = e | widen;
                /* No invlpg here — the lower-level walk hasn't been
                 * cached yet, and changing US in a higher level
                 * doesn't invalidate any TLB entry that mattered. */
            }
            tbl = table_at((uintptr_t)e);
            continue;
        }

        if (!create) return 0;

        /* Allocate a fresh frame for the next-level table.  Identity-
         * map dependency: pmm_alloc_frame returns a phys addr within
         * the first 1 GiB (our identity range), so we can zero it via
         * a direct pointer. */
        uint32_t newphys = pmm_alloc_frame();
        if (!newphys) return 0;
        uint64_t* newtbl = (uint64_t*)(uintptr_t)newphys;
        for (int i = 0; i < 512; i++) newtbl[i] = 0;

        /* Parent entry: P + RW always; carry USER from caller flags so
         * user mappings stay reachable. */
        tbl[idx] = ((uint64_t)newphys & PAGE_MASK_4K)
                 | PTE_P | PTE_RW
                 | ((uint64_t)parent_flags & PTE_US);
        tbl = newtbl;
    }

    return tbl;       /* PT */
}

/* -----------------------------------------------------------------------------
 * Mapping operations.
 * ----------------------------------------------------------------------------- */

int vmm_map(uintptr_t virt, uintptr_t phys, uint32_t flags) {
    uint64_t* pt = walk_to_pt(virt, /*create*/1, flags);
    if (!pt) return -1;

    unsigned idx = IDX_PT(virt);
    pt[idx] = ((uint64_t)phys & PAGE_MASK_4K)
            | PTE_P
            | ((uint64_t)flags & (PTE_RW | PTE_US));
    invlpg(virt);
    return 0;
}

/* Install a 4 MiB region using TWO adjacent 2 MiB large pages.
 *
 * Why: callers like fb_terminal.c map a framebuffer with `for (...;
 * a += 0x400000)` — i.e., 4 MiB strides.  On i386 each call sets up
 * one PSE PDE (which is literally 4 MiB).  On x86_64 the closest
 * equivalent is a 2 MiB PD entry; we install two of them per call so
 * the source-level contract holds.
 *
 * Requires 4 MiB alignment on both `virt` and `phys`.  Refuses to
 * clobber an existing non-large entry. */
int vmm_map_4mib(uintptr_t virt, uintptr_t phys, uint32_t flags) {
    if (virt & 0x003FFFFFu) return -1;
    if (phys & 0x003FFFFFu) return -1;

    for (int i = 0; i < 2; i++) {
        uintptr_t v = virt + (uintptr_t)i * 0x200000ull;
        uintptr_t p = phys + (uintptr_t)i * 0x200000ull;

        /* Walk PML4 → PDPT → PD, creating intermediates as needed.
         * We can't reuse walk_to_pt because we stop at PD (one level
         * earlier) and write a large-page entry rather than chaining
         * to a PT. */
        uint64_t* tbl = (uint64_t*)pml4;
        int shifts[2] = { 39, 30 };                 /* PML4, PDPT */
        for (int level = 0; level < 2; level++) {
            unsigned idx = ((unsigned)(v >> shifts[level])) & 0x1FFu;
            uint64_t  e   = tbl[idx];
            if (e & PTE_P) {
                if (level == 1 && (e & PTE_PS)) return -2; /* PDPT large = 1 GiB conflict */
                tbl = table_at((uintptr_t)e);
                continue;
            }
            uint32_t newphys = pmm_alloc_frame();
            if (!newphys) return -3;
            uint64_t* newtbl = (uint64_t*)(uintptr_t)newphys;
            for (int j = 0; j < 512; j++) newtbl[j] = 0;
            tbl[idx] = ((uint64_t)newphys & PAGE_MASK_4K)
                     | PTE_P | PTE_RW
                     | ((uint64_t)flags & PTE_US);
            tbl = newtbl;
        }

        /* tbl is now PD.  Install the 2 MiB large page. */
        unsigned idx = IDX_PD(v);
        uint64_t pd_e = tbl[idx];
        if ((pd_e & PTE_P) && !(pd_e & PTE_PS)) return -4; /* already a PT */
        tbl[idx] = ((uint64_t)p & PAGE_MASK_2M)
                 | PTE_P | PTE_PS
                 | ((uint64_t)flags & (PTE_RW | PTE_US));
        invlpg(v);
    }
    return 0;
}

void vmm_unmap(uintptr_t virt) {
    uint64_t* pt = walk_to_pt(virt, /*create*/0, 0);
    if (!pt) return;            /* already unmapped or behind a large page */
    pt[IDX_PT(virt)] = 0;
    invlpg(virt);
}

uintptr_t vmm_translate(uintptr_t virt) {
    uint64_t* tbl = (uint64_t*)pml4;
    int shifts[3] = { 39, 30, 21 };
    uintptr_t page_size_masks[3] = { 0, 0x3FFFFFFFull, 0x1FFFFFull };  /* PDPT 1 GiB, PD 2 MiB */

    for (int level = 0; level < 3; level++) {
        unsigned idx = ((unsigned)(virt >> shifts[level])) & 0x1FFu;
        uint64_t  e   = tbl[idx];
        if (!(e & PTE_P)) return 0;

        if (level >= 1 && (e & PTE_PS)) {
            uintptr_t mask = page_size_masks[level];
            return (uintptr_t)((e & ~mask & PAGE_MASK_4K) | (virt & mask));
        }
        tbl = table_at((uintptr_t)e);
    }

    uint64_t e = tbl[IDX_PT(virt)];
    if (!(e & PTE_P)) return 0;
    return (uintptr_t)((e & PAGE_MASK_4K) | (virt & 0xFFFu));
}

void vmm_print_status(void) {
    uint64_t cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    /* rdmsr returns split eax:edx; recombine into a 64-bit value. */
    uint32_t efer_lo, efer_hi;
    __asm__ volatile ("rdmsr"
                      : "=a"(efer_lo), "=d"(efer_hi)
                      : "c"(0xC0000080u));
    uint64_t efer = ((uint64_t)efer_hi << 32) | efer_lo;

    kprintf("vmm: cr0=%lx cr3=%lx cr4=%lx efer=%lx (long mode=%s, PAE=%s)\n",
            (unsigned long)cr0, (unsigned long)cr3, (unsigned long)cr4,
            (unsigned long)efer,
            (efer & (1u << 10)) ? "on" : "off",      /* LMA */
            (cr4 & (1u << 5))   ? "on" : "off");     /* PAE */
}
