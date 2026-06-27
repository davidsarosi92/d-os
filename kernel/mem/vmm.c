/* =============================================================================
 * vmm.c — enable paging and manage 4 KiB page mappings.
 *
 * The initial state after `vmm_init` is:
 *
 *   - CR4.PSE = 1  (4 MiB pages enabled)
 *   - CR3 points at `kernel_pd` (the single kernel page directory)
 *   - CR0.PG = 1
 *   - The first IDENTITY_MAP_MIB (256) megabytes of virtual address space
 *     are identity-mapped via 4 MiB PSE PDEs: virt == phys, RW, supervisor.
 *
 * Because our kernel image, stack, heap, and any physical memory the PMM
 * hands us today all live below 256 MiB, every pointer we already hold
 * keeps working the instant paging turns on — no pointer rewriting, no
 * higher-half magic, no relocation.  That simplicity is the whole reason
 * for the 256 MiB identity map; later milestones may swap this out for a
 * higher-half kernel mapping when we want to reclaim the low virtual
 * addresses for user space.
 *
 * -------------------- PDE / PTE bit layout (§4.3) -------------------------
 *   bit 0    P   — Present
 *   bit 1    RW  — 0 = read-only, 1 = read/write
 *   bit 2    US  — 0 = supervisor only, 1 = user accessible
 *   bit 3    PWT — Write-through
 *   bit 4    PCD — Cache disabled
 *   bit 5    A   — Accessed (CPU sets)
 *   bit 6    D   — Dirty (PTE only; CPU sets)
 *   bit 7    PS  — Page Size.  In a PDE: 1 = this entry maps a 4 MiB page
 *                  directly.  In a PTE: ignored.
 *   bit 8    G   — Global (sticky TLB entry across CR3 reloads)
 *   bits 9..11   Available for OS use
 *   bits 12..31  Page frame / page-table base address (4 KiB aligned)
 *
 * For 4 MiB PSE PDEs the base is aligned to 4 MiB, so bits 12..21 must be
 * zero and bits 22..31 hold the 4 MiB page base.
 *
 * -------------------- Address breakdown ----------------------------------
 *   virt[31:22]  Page directory index (10 bits → 1024 PDEs)
 *   virt[21:12]  Page table index    (10 bits → 1024 PTEs)
 *   virt[11:0]   Offset inside the 4 KiB page
 * ========================================================================= */

#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Bit helpers and macros.                                                   */
/* ------------------------------------------------------------------------- */

#define PDE_P   0x001
#define PDE_RW  0x002
#define PDE_US  0x004
#define PDE_PS  0x080                   /* 4 MiB page when set in a PDE */

#define PTE_P   0x001
#define PTE_RW  0x002
#define PTE_US  0x004

#define PAGE_MASK   0xFFFFF000u         /* keep base, drop flags (4 KiB aligned) */
#define PSE_MASK    0xFFC00000u         /* 4 MiB page base mask */

#define PD_IDX(v)  (((v) >> 22) & 0x3FF)
#define PT_IDX(v)  (((v) >> 12) & 0x3FF)

#define IDENTITY_MAP_MIB 256
#define IDENTITY_PDES    (IDENTITY_MAP_MIB / 4)   /* 4 MiB per PSE PDE */

/* ------------------------------------------------------------------------- */
/* Page directory.  Must be 4 KiB aligned — the low 12 bits of CR3 are
 * control flags, not part of the address.                                   */
/* ------------------------------------------------------------------------- */
static uint32_t kernel_pd[1024] __attribute__((aligned(4096)));

/* ------------------------------------------------------------------------- */
/* Low-level helpers — tiny inline asm wrappers to read/write CRx and
 * invalidate a single TLB entry.  Keeping them `static inline` lets the
 * compiler fold them into callers while still documenting each access.    */
/* ------------------------------------------------------------------------- */

static inline void invlpg(uint32_t virt) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

static inline void load_cr3(uint32_t pd_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

static inline uint32_t read_cr0(void) {
    uint32_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void write_cr0(uint32_t v) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(v));
}

static inline uint32_t read_cr4(void) {
    uint32_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v;
}
static inline void write_cr4(uint32_t v) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(v));
}

/* ------------------------------------------------------------------------- */
/* Init.                                                                     */
/* ------------------------------------------------------------------------- */

void vmm_init(void) {
    /* Clear every PDE first so entries we don't explicitly populate stay
     * "not present" and cause clean page faults if touched. */
    for (int i = 0; i < 1024; i++) kernel_pd[i] = 0;

    /* Identity-map the first IDENTITY_PDES × 4 MiB with PSE PDEs.  Each
     * entry's base address occupies bits [31:22]; lower bits are flags. */
    for (int i = 0; i < IDENTITY_PDES; i++) {
        uint32_t phys = (uint32_t)i << 22;
        kernel_pd[i] = phys | PDE_P | PDE_RW | PDE_PS;
    }

    /* Enable 4 MiB pages in CR4 before switching on paging.  Doing it
     * the other way round would leave our PDEs misinterpreted. */
    write_cr4(read_cr4() | (1u << 4));          /* CR4.PSE */

    /* Install the page directory. */
    load_cr3((uint32_t)(uintptr_t)&kernel_pd[0]);

    /* Flip the master switch.  The instruction right after this one is
     * fetched from EIP, now translated via kernel_pd.  Because the
     * current EIP sits in the identity-mapped 256 MiB, execution
     * continues seamlessly. */
    write_cr0(read_cr0() | 0x80000000u);        /* CR0.PG */

    kprintf("vmm: paging on, identity %d MiB (PSE), pd @ %p\n",
            IDENTITY_MAP_MIB, (void*)&kernel_pd[0]);
}

/* ------------------------------------------------------------------------- */
/* Mapping operations.                                                       */
/* ------------------------------------------------------------------------- */

int vmm_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pdi = PD_IDX(virt);
    uint32_t pti = PT_IDX(virt);
    uint32_t pde = kernel_pd[pdi];

    /* Refuse to punch a 4 KiB hole through a 4 MiB PSE entry.  A future
     * milestone could split the PSE into a regular PT on demand. */
    if ((pde & PDE_P) && (pde & PDE_PS)) return -1;

    uint32_t* pt;
    if ((pde & PDE_P) == 0) {
        /* No table here yet — carve one out of physical memory.  Today
         * the PMM only ever returns frames below 256 MiB, so we can
         * reach the new table through the identity map and zero it. */
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return -2;

        pt = (uint32_t*)(uintptr_t)pt_phys;
        for (int i = 0; i < 1024; i++) pt[i] = 0;

        /* PDE points at PT; USER bit on the PDE propagates from the
         * caller's flags so a user mapping stays user-accessible. */
        kernel_pd[pdi] = pt_phys | PDE_P | PDE_RW | (flags & PDE_US);
    } else {
        pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    }

    pt[pti] = (phys & PAGE_MASK) | PTE_P | (flags & (PTE_RW | PTE_US));
    invlpg(virt);
    return 0;
}

int vmm_map_4mib(uint32_t virt, uint32_t phys, uint32_t flags) {
    /* PSE requires the low 22 bits of both `virt` and `phys` to be zero. */
    if (virt & 0x003FFFFFu) return -1;
    if (phys & 0x003FFFFFu) return -1;

    uint32_t pdi = PD_IDX(virt);
    uint32_t pde = kernel_pd[pdi];

    /* Refuse to clobber a regular (non-PSE) page table that might be
     * backing finer-grained mappings.  Caller must pick an unused PDE. */
    if ((pde & PDE_P) && (pde & PDE_PS) == 0) return -2;

    kernel_pd[pdi] = (phys & PSE_MASK) | PDE_P | PDE_PS
                   | (flags & (PDE_RW | PDE_US));
    /* invlpg of any address in the 4 MiB range flushes the entry. */
    invlpg(virt);
    return 0;
}

void vmm_unmap(uint32_t virt) {
    uint32_t pdi = PD_IDX(virt);
    uint32_t pti = PT_IDX(virt);
    uint32_t pde = kernel_pd[pdi];

    if ((pde & PDE_P) == 0) return;             /* already unmapped */
    if (pde & PDE_PS) return;                   /* PSE region — refuse */

    uint32_t* pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    pt[pti] = 0;
    invlpg(virt);
}

uint32_t vmm_translate(uint32_t virt) {
    uint32_t pdi = PD_IDX(virt);
    uint32_t pde = kernel_pd[pdi];
    if ((pde & PDE_P) == 0) return 0;

    if (pde & PDE_PS) {
        /* 4 MiB PSE page — low 22 bits are the offset. */
        return (pde & PSE_MASK) | (virt & 0x003FFFFFu);
    }

    uint32_t* pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    uint32_t pte = pt[PT_IDX(virt)];
    if ((pte & PTE_P) == 0) return 0;
    return (pte & PAGE_MASK) | (virt & 0x00000FFFu);
}

void vmm_print_status(void) {
    uint32_t cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    kprintf("vmm: cr0=%x cr3=%x cr4=%x (paging=%s, pse=%s)\n",
            cr0, cr3, cr4,
            (cr0 & 0x80000000u) ? "on" : "off",
            (cr4 & (1u << 4))   ? "on" : "off");
}
