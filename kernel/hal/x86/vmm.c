/* =============================================================================
 * vmm.c — enable paging and manage 4 KiB page mappings (i386).
 *
 * TWO PAGE-TABLE FORMATS, CHOSEN AT BOOT (§M86 stage 2, 2026-09-26).
 *
 *   - classic 32-bit paging: a 1024-entry page directory, 4 MiB PSE pages
 *     for the identity map, 1024-entry page tables, 32-bit entries.  Physical
 *     addresses stop at 4 GiB.
 *   - PAE: a 4-entry PDPT → four 512-entry page directories → 512-entry page
 *     tables, 64-bit entries, 2 MiB pages for the identity map.  Physical
 *     addresses reach past 4 GiB (36+ bits), which is the whole point.
 *
 * PAE is used whenever the CPU has it (CPUID.1:EDX bit 6 — every x86 since the
 * Pentium Pro); the classic format stays for the CPUs that do not, and is
 * reachable on purpose with `qemu -cpu qemu32,-pae` so it keeps being tested.
 * The VIRTUAL layout is identical in both, and every function below works on
 * both through one abstraction: a flat "PDE index" (1024 of 4 MiB, or 2048 of
 * 2 MiB), with the entry width hidden in pde_get/pte_get.
 *
 *   0 .. 1020 MiB     identity map (kernel image, heap, page tables, DMA)
 *   1020 .. 1024 MiB  the kmap window (kmap.h) — highmem is reached here
 *   1 GiB .. 4 GiB    user space, plus kernel MMIO mappings (LAPIC, framebuffer)
 *                     that vmm_map puts in the kernel's tables and every new
 *                     address space copies by value
 *
 * Every page table and directory lives in low (identity-mapped) memory, so
 * the kernel always reaches them directly; only USER PAGES may be highmem.
 *
 * Paging reference: Intel SDM Vol 3 §4.3 (32-bit), §4.4 (PAE).
 * ============================================================================= */

#include "vmm.h"
#include "hal_api.h"   /* §M51 — hal_tlb_shootdown */
#include "pmm.h"
#include "cowref.h"
#include "kmap.h"
#include "printf.h"
#include "kmalloc.h"
#include "task.h"
#include "percpu.h"
#include "lock.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/* Entry bits — the low 12 are the same in both formats.                     */
/* ------------------------------------------------------------------------- */

#define E_P    0x001ull
#define E_RW   0x002ull
#define E_US   0x004ull
#define E_PS   0x080ull                        /* large page (PDE only) */
#define E_OS   (0x400ull | 0x800ull)           /* VMM_SHARED | VMM_COW  */
#define E_NX   (1ull << 63)                    /* §M86 — PAE only       */

#define PAGE_MASK32   0xFFFFF000ull
#define PAGE_MASK64   0x000FFFFFFFFFF000ull    /* bits 12..51 */
#define LARGE_MASK32  0xFFC00000ull            /* 4 MiB PSE base */
#define LARGE_MASK64  0x000FFFFFFFE00000ull    /* 2 MiB PAE base */

/* §M48 / §M86 — the identity map runs to 1020 MiB; the top 4 MiB of the
 * kernel's first gigabyte is the kmap window. */
#define IDENTITY_MAP_MIB 1020
#define KMAP_BASE        0x3FC00000u
#define KMAP_PER_CPU     8
#define KMAP_CPUS        64                     /* 512 slots: one PAE PT */

static int g_pae;                               /* chosen once, in vmm_init */
/* §M86 — NO-EXECUTE for user pages.  Needs PAE (bit 63 exists only in the
 * 64-bit entry) and a CPU that has it (EFER.NXE is set per CPU by hal_arch.c).
 *
 * APPLIED TO USER PAGES ONLY, and on purpose: the kernel's identity map holds
 * the heap, and §M67's modules execute from it, so marking kernel memory NX
 * would need the module loader to map its code separately first.  What this
 * buys is W^X for PROGRAMS — a user page is executable exactly when its
 * mapping asked for VMM_EXEC (a PF_X segment, PROT_EXEC) — which is where a
 * stack or heap overflow would otherwise become code.  M25's VMM_EXEC had been
 * "advisory on x86" since it was written; this is the day it stops being. */
static int g_nx;

/* The NX bit a USER mapping with these VMM_* flags gets. */
static inline uint64_t nx_bits(uint32_t flags) {
    return (g_nx && (flags & VMM_USER) && !(flags & VMM_EXEC)) ? E_NX : 0;
}
/* Back the other way: a present user entry's executability as VMM_EXEC, so a
 * path that rebuilds an entry (fork, a shared page) carries it over. */
static inline uint32_t exec_of(uint64_t pte) {
    return (pte & E_NX) ? 0u : (uint32_t)VMM_EXEC;
}
int vmm_nx_active(void) { return g_nx; }

/* classic format */
static uint32_t kernel_pd[1024]  __attribute__((aligned(4096)));
static uint32_t kmap_pt32[1024]  __attribute__((aligned(4096)));
/* PAE format */
static uint64_t k_pdpt[4]        __attribute__((aligned(32)));
static uint64_t k_pd[4][512]     __attribute__((aligned(4096)));
static uint64_t kmap_pt64[512]   __attribute__((aligned(4096)));

static int kmap_depth[KMAP_CPUS];
static int kmap_paging_on;

/* ------------------------------------------------------------------------- */
/* Format-neutral accessors.                                                 */
/* ------------------------------------------------------------------------- */

static inline uint32_t npde(void)     { return g_pae ? 2048u : 1024u; }
static inline uint32_t npte(void)     { return g_pae ? 512u  : 1024u; }
static inline uint32_t pde_shift(void){ return g_pae ? 21u   : 22u;   }
static inline uint64_t addr_mask(void){ return g_pae ? PAGE_MASK64 : PAGE_MASK32; }
static inline uint64_t large_mask(void){ return g_pae ? LARGE_MASK64 : LARGE_MASK32; }
static inline uint32_t pde_index(uint32_t va) { return va >> pde_shift(); }
static inline uint32_t pte_index(uint32_t va) { return (va >> 12) & (npte() - 1); }

/* A "root" is the thing CR3 names: the page directory (classic) or the PDPT
 * (PAE).  Both are reachable through the identity map. */
static inline void* kroot(void) { return g_pae ? (void*)k_pdpt : (void*)kernel_pd; }
static inline uint32_t kroot_phys(void) { return (uint32_t)(uintptr_t)kroot(); }

static inline uint64_t* pae_pd(void* root, uint32_t gi) {
    uint64_t pdpte = ((uint64_t*)root)[gi >> 9];
    return (uint64_t*)(uintptr_t)(pdpte & PAGE_MASK64);
}
static inline uint64_t pde_get(void* root, uint32_t gi) {
    if (g_pae) return pae_pd(root, gi)[gi & 511];
    return ((uint32_t*)root)[gi];
}
static inline void pde_set(void* root, uint32_t gi, uint64_t v) {
    if (g_pae) pae_pd(root, gi)[gi & 511] = v;
    else       ((uint32_t*)root)[gi] = (uint32_t)v;
}
static inline uint64_t pte_get(uint64_t pt, uint32_t j) {
    if (g_pae) return ((uint64_t*)(uintptr_t)pt)[j];
    return ((uint32_t*)(uintptr_t)pt)[j];
}
static inline void pte_set(uint64_t pt, uint32_t j, uint64_t v) {
    if (g_pae) ((uint64_t*)(uintptr_t)pt)[j] = v;
    else       ((uint32_t*)(uintptr_t)pt)[j] = (uint32_t)v;
}

/* ------------------------------------------------------------------------- */
/* Low-level helpers.                                                        */
/* ------------------------------------------------------------------------- */

static inline void invlpg(uint32_t virt) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}
static inline void load_cr3(uint32_t root_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(root_phys) : "memory");
}
static inline uint32_t read_cr3(void) {
    uint32_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
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

/* Physical address of the kernel's root table — the AP boot trampoline loads
 * it into CR3 (M18).  With PAE that is the PDPT, 32-byte aligned and in low
 * memory, which is what CR3 requires. */
uintptr_t vmm_kernel_pd_phys(void) { return (uintptr_t)kroot_phys(); }

/* The CR4 bits an AP must set before it enables paging: PSE always (the
 * classic identity map uses it; PAE ignores it), PAE when the BSP chose it —
 * an AP that walked a PDPT as a page directory would be lost at once. */
uint32_t vmm_cr4_bits(void) { return 0x10u | (g_pae ? 0x20u : 0u); }
int      vmm_pae_active(void) { return g_pae; }

/* ------------------------------------------------------------------------- */
/* Init.                                                                     */
/* ------------------------------------------------------------------------- */

extern int x86_cpu_has_pae(void);                /* hal_arch.c */

extern int x86_cpu_has_nx(void);                 /* hal_arch.c */

void vmm_init(void) {
    g_pae = x86_cpu_has_pae();
    g_nx  = g_pae && x86_cpu_has_nx();

    if (!g_pae) {
        for (int i = 0; i < 1024; i++) kernel_pd[i] = 0;
        for (int i = 0; i < IDENTITY_MAP_MIB / 4; i++)
            kernel_pd[i] = ((uint32_t)i << 22) | (uint32_t)(E_P | E_RW | E_PS);
        for (int i = 0; i < 1024; i++) kmap_pt32[i] = 0;
        kernel_pd[KMAP_BASE >> 22] = (uint32_t)(uintptr_t)&kmap_pt32[0] | (uint32_t)(E_P | E_RW);
        write_cr4(read_cr4() | 0x10u);                        /* CR4.PSE */
    } else {
        for (int d = 0; d < 4; d++)
            for (int i = 0; i < 512; i++) k_pd[d][i] = 0;
        for (int i = 0; i < IDENTITY_MAP_MIB / 2; i++)
            k_pd[0][i] = ((uint64_t)i << 21) | E_P | E_RW | E_PS;
        for (int i = 0; i < 512; i++) kmap_pt64[i] = 0;
        k_pd[0][KMAP_BASE >> 21 & 511] = (uint64_t)(uintptr_t)&kmap_pt64[0] | E_P | E_RW;
        /* A PDPTE carries ONLY the present bit (and caching bits): RW/US are
         * RESERVED there, and setting them makes the CR3 load itself #GP. */
        for (int d = 0; d < 4; d++) k_pdpt[d] = (uint64_t)(uintptr_t)&k_pd[d][0] | E_P;
        write_cr4(read_cr4() | 0x10u | 0x20u);                /* PSE + PAE */
    }

    load_cr3(kroot_phys());
    write_cr0(read_cr0() | 0x80000000u);                      /* CR0.PG */
    kmap_paging_on = 1;

    kprintf("vmm: paging on (%s), identity %d MiB, root @ %p\n",
            g_pae ? "PAE, 64-bit entries, 2 MiB pages"
                  : "classic 32-bit, 4 MiB PSE pages",
            IDENTITY_MAP_MIB, (void*)(uintptr_t)kroot_phys());
    /* Say which, because "the program crashed on a data page" and "NX is off
     * so nothing is enforced" must be distinguishable from a log. */
    kprintf("vmm: no-execute %s\n",
            g_nx ? "ON for user pages (W^X: data, heap and stack cannot run)"
                 : g_pae ? "unavailable (the CPU has no NX)"
                         : "unavailable (classic paging has no NX bit)");
}

/* ------------------------------------------------------------------------- */
/* §M46/security — is the user range [va, va+len) fully mapped AND user-
 * accessible in the CURRENTLY ACTIVE address space?  Walks the tables CR3
 * names.  `want_write` also requires the R/W bit.                          */
/* ------------------------------------------------------------------------- */
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write) {
    if (len == 0) return 1;
    if (va < vmm_user_base()) return 0;             /* reject kernel/low addrs */
    if (va + len < va)        return 0;             /* overflow */
    void* root = (void*)(uintptr_t)(read_cr3() & (g_pae ? ~0x1Fu : ~0xFFFu));
    for (uintptr_t p = va & ~0xFFFu; p < va + len; p += 0x1000) {
        uint64_t pde = pde_get(root, pde_index((uint32_t)p));
        if (!(pde & E_P) || !(pde & E_US)) return 0;      /* absent / kernel */
        if (pde & E_PS) {
            if (want_write && !(pde & E_RW)) return 0;
            continue;
        }
        uint64_t pte = pte_get(pde & addr_mask(), pte_index((uint32_t)p));
        if (!(pte & E_P) || !(pte & E_US)) return 0;
        if (want_write && !(pte & E_RW)) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Core map / unmap / protect, parameterised by the root.                    */
/* ------------------------------------------------------------------------- */

/* `notify` = should a remap of an already-present entry be broadcast to the
 * other CPUs?  Every ordinary caller says yes; vmm_space_clone says no and
 * issues ONE whole-space shootdown instead (§M51: per page it is thousands of
 * IPI round trips for the same end state). */
static int map_in_root_ex(void* root, uint32_t virt, uint64_t phys,
                          uint32_t flags, int notify) {
    uint32_t gi = pde_index(virt), j = pte_index(virt);
    uint64_t pde = pde_get(root, gi);

    /* Refuse to punch a 4 KiB hole through a large page. */
    if ((pde & E_P) && (pde & E_PS)) return -1;

    uint64_t pt;
    if (!(pde & E_P)) {
        /* Tables always come from LOW memory: the kernel walks them directly. */
        pmm_phys_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return -2;
        uint32_t* z = (uint32_t*)(uintptr_t)pt_phys;
        for (int i = 0; i < 1024; i++) z[i] = 0;
        /* USER on the directory entry propagates from the caller's flags so a
         * user mapping stays user-accessible. */
        pde_set(root, gi, (uint64_t)pt_phys | E_P | E_RW | (flags & E_US));
        pt = pt_phys;
    } else {
        pt = pde & addr_mask();
    }

    /* §M51 — a remap (overwriting a PRESENT entry) can weaken or redirect a
     * translation, so every CPU has to be told; a fresh map cannot be cached
     * anywhere, so it need not be. */
    int was_present = (pte_get(pt, j) & E_P) != 0;
    pte_set(pt, j, (phys & addr_mask()) | E_P | (flags & (E_RW | E_US | E_OS)) |
                   nx_bits(flags));
    if (was_present && notify) hal_tlb_shootdown(0, virt);
    else                       invlpg(virt);
    return 0;
}
static int map_in_root(void* root, uint32_t virt, uint64_t phys, uint32_t flags) {
    return map_in_root_ex(root, virt, phys, flags, /*notify*/1);
}

static void unmap_in_root(void* root, uint32_t virt) {
    uint64_t pde = pde_get(root, pde_index(virt));
    if (!(pde & E_P) || (pde & E_PS)) return;
    pte_set(pde & addr_mask(), pte_index(virt), 0);
    hal_tlb_shootdown(0, virt);                     /* §M51 — weakening */
}

/* What a USER entry owned, released by the rules vmm_space_destroy applies to
 * a leaf — one decision, two callers, so an unmap and a teardown cannot
 * disagree about who frees a frame.  Called AFTER the entry is cleared and the
 * TLB shot down: another CPU may still be reading the frame until then. */
static inline uint16_t* cow_slot(uint64_t phys);
static void release_user_entry(uint64_t pte) {
    if (!(pte & E_P)) {
        if (pte & VMM_SWPE_MARK) swap_slot_release((uint32_t)(pte >> 12));  /* §M72 */
        return;
    }
    if (pte & VMM_SHARED) return;                  /* borrowed — its owner frees */
    uint64_t fr = pte & addr_mask();
    if (pte & VMM_COW) {
        uint16_t* rc = cow_slot(fr);
        if (rc && !cow_ref_drop(rc)) return;       /* others still hold it */
    }
    pmm_free_frame(fr);
}

/* The mprotect primitive: change protection WITHOUT touching the frame;
 * preserves the OS-available SHARED/COW bits.  0, or -1 if not mapped. */
static int protect_in_root(void* root, uint32_t virt, uint32_t flags) {
    uint64_t pde = pde_get(root, pde_index(virt));
    if (!(pde & E_P) || (pde & E_PS)) return -1;
    uint64_t pt = pde & addr_mask();
    uint32_t j = pte_index(virt);
    uint64_t pte = pte_get(pt, j);
    if (!(pte & E_P)) return -1;
    /* A COPY-ON-WRITE page never becomes writable here (2026-09-27): its frame
     * is still mapped by the other side of a fork, so write access may only
     * come through vmm_cow_fault, which copies first.  mprotect(PROT_WRITE) on
     * one used to make the SHARED frame writable in place (`cowprotecttest`).
     * The known cost: a COW page mprotected read-only is still granted write
     * on its next write fault — there is no spare PTE bit on classic i386 to
     * remember "read-only after the copy". */
    if (pte & VMM_COW) flags &= ~(uint32_t)E_RW;
    pte_set(pt, j, (pte & addr_mask()) | E_P | (flags & (E_RW | E_US)) | (pte & E_OS) |
                   nx_bits(flags | (uint32_t)(pte & E_US)));
    hal_tlb_shootdown(0, virt);                     /* §M51 — may drop RW */
    return 0;
}

int vmm_map(uintptr_t virt, uint64_t phys, uint32_t flags) {
    return map_in_root(kroot(), (uint32_t)virt, phys, flags);
}

/* One 4 MiB large mapping (framebuffer, xHCI/AHCI MMIO).  PAE's large page is
 * 2 MiB, so there it is two of them — same contract for the caller. */
int vmm_map_4mib(uintptr_t virt32, uintptr_t phys32, uint32_t flags) {
    uint32_t virt = (uint32_t)virt32, phys = (uint32_t)phys32;
    if ((virt | phys) & 0x003FFFFFu) return -1;          /* 4 MiB aligned */
    uint32_t n = g_pae ? 2 : 1, step = g_pae ? 0x200000u : 0x400000u;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t pde = pde_get(kroot(), pde_index(virt + k * step));
        if ((pde & E_P) && !(pde & E_PS)) return -2;      /* a PT lives here */
    }
    for (uint32_t k = 0; k < n; k++) {
        pde_set(kroot(), pde_index(virt + k * step),
                ((uint64_t)(phys + k * step) & large_mask()) | E_P | E_PS |
                (flags & (E_RW | E_US)));
        invlpg(virt + k * step);
    }
    return 0;
}

void vmm_unmap(uintptr_t virt) { unmap_in_root(kroot(), (uint32_t)virt); }

uintptr_t vmm_translate(uintptr_t virt32) {
    uint32_t virt = (uint32_t)virt32;
    uint64_t pde = pde_get(kroot(), pde_index(virt));
    if (!(pde & E_P)) return 0;
    if (pde & E_PS) {
        uint32_t off = virt & ((1u << pde_shift()) - 1u);
        return (uintptr_t)((pde & large_mask()) | off);
    }
    uint64_t pte = pte_get(pde & addr_mask(), pte_index(virt));
    if (!(pte & E_P)) return 0;
    return (uintptr_t)((pte & addr_mask()) | (virt & 0xFFFu));
}

void vmm_print_status(void) {
    uint32_t cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    kprintf("vmm: cr0=%x cr3=%x cr4=%x (paging=%s, format=%s)\n",
            cr0, cr3, cr4, (cr0 & 0x80000000u) ? "on" : "off",
            (cr4 & 0x20u) ? "PAE" : ((cr4 & 0x10u) ? "classic+PSE" : "classic"));
}

/* ===========================================================================
 * Per-process address spaces (M25).
 *
 * Classic: a private page directory, a snapshot of the kernel's.  PAE: a
 * private PDPT whose entry 0 is the KERNEL's first directory (the identity map
 * and the kmap window — nothing user-private lives below 1 GiB), and whose
 * entries 1..3 are PRIVATE directories initialised from the kernel's, so kernel
 * MMIO page tables are shared by pointer and user page tables are the space's
 * own.  In both formats "is this directory entry shared with the kernel?" is
 * "is it identical to the kernel's entry at that index".
 * =========================================================================== */

struct vmm_space {
    void*     root;         /* PD (classic) or PDPT (PAE); identity-mapped   */
    uint32_t  root_phys;    /* == (uint32_t)root, what CR3 is loaded with    */
    /* §M48 — the mmap bump cursor lives with the ADDRESS SPACE, not the task:
     * one space, one cursor, whichever thread bumps it. */
    uintptr_t mmap_cursor;
};

static int pde_is_kernel_shared(struct vmm_space* s, uint32_t gi) {
    return pde_get(s->root, gi) == pde_get(kroot(), gi);
}

struct vmm_space* vmm_space_create(void) {
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->mmap_cursor = 0;

    pmm_phys_t rf = pmm_alloc_frame();            /* low memory: CR3 needs it */
    if (!rf) { kfree(s); return NULL; }
    s->root = (void*)(uintptr_t)rf;
    s->root_phys = (uint32_t)rf;

    if (!g_pae) {
        uint32_t* pd = (uint32_t*)s->root;
        for (int i = 0; i < 1024; i++) pd[i] = kernel_pd[i];
        return s;
    }
    uint64_t* pdpt = (uint64_t*)s->root;
    pdpt[0] = k_pdpt[0];                          /* shared kernel directory */
    for (int d = 1; d < 4; d++) {
        pmm_phys_t pf = pmm_alloc_frame();
        if (!pf) {
            for (int e = 1; e < d; e++) pmm_free_frame(pdpt[e] & PAGE_MASK64);
            pmm_free_frame(rf); kfree(s); return NULL;
        }
        uint64_t* pd = (uint64_t*)(uintptr_t)pf;
        for (int i = 0; i < 512; i++) pd[i] = k_pd[d][i];
        pdpt[d] = (uint64_t)pf | E_P;
    }
    return s;
}

/* M34 — per-frame COW reference counts (cowref.h: built once, updated
 * atomically), indexed by frame number, covering every frame the PMM manages
 * — which with PAE includes frames above 4 GiB. */
static uint16_t* g_cow_ref;
static uint32_t  g_cow_nr;

static inline uint16_t* cow_slot(uint64_t phys) {
    static spinlock_t cow_build_lock = SPINLOCK_INIT;
    if (!cow_table_get(&g_cow_ref, &g_cow_nr, &cow_build_lock)) return NULL;
    uint32_t fn = (uint32_t)(phys >> 12);
    return (fn >= pmm_pfn_base && fn < g_cow_nr) ? &g_cow_ref[fn] : NULL;
}

void vmm_space_destroy(struct vmm_space* s) {
    if (!s) return;
    /* Free every page table + user frame this space added on top of the
     * kernel snapshot.  Kernel-shared entries and large pages are left alone. */
    for (uint32_t gi = 0; gi < npde(); gi++) {
        uint64_t pde = pde_get(s->root, gi);
        if (!(pde & E_P) || (pde & E_PS)) continue;
        if (pde_is_kernel_shared(s, gi)) continue;
        uint64_t pt = pde & addr_mask();
        for (uint32_t j = 0; j < npte(); j++) {
            uint64_t pte = pte_get(pt, j);
            if (!(pte & E_P)) {
                /* §M72 — an evicted page: its slot goes back, nothing else. */
                if (pte & VMM_SWPE_MARK) swap_slot_release((uint32_t)(pte >> 12));
                continue;
            }
            if (pte & VMM_SHARED)    continue;     /* borrowed shm — owner frees */
            uint64_t fr = pte & addr_mask();
            if (pte & VMM_COW) {
                uint16_t* rc = cow_slot(fr);
                if (rc && !cow_ref_drop(rc)) continue;   /* others still hold it */
            }
            pmm_free_frame(fr);                   /* owned user page */
        }
        pmm_free_frame(pt);                       /* the page table itself */
    }
    if (g_pae) {
        uint64_t* pdpt = (uint64_t*)s->root;
        for (int d = 1; d < 4; d++) pmm_free_frame(pdpt[d] & PAGE_MASK64);
    }
    pmm_free_frame(s->root_phys);
    kfree(s);
}

struct vmm_space* vmm_space_clone(struct vmm_space* parent) {
    if (!parent) return NULL;
    struct vmm_space* child = vmm_space_create();     /* kernel snapshot only */
    if (!child) return NULL;
    child->mmap_cursor = parent->mmap_cursor;

    /* WRITABLE (or already COW) pages become copy-on-write in BOTH spaces,
     * ref-counted; read-only pages (code) are copied eagerly; borrowed shm
     * pages stay shared.  (Catching "already COW" matters: a second fork must
     * re-share, never mistake it for read-only code.) */
    for (uint32_t gi = 0; gi < npde(); gi++) {
        uint64_t pde = pde_get(parent->root, gi);
        if (!(pde & E_P) || (pde & E_PS)) continue;
        if (pde_is_kernel_shared(parent, gi)) continue;
        uint64_t pt = pde & addr_mask();
        for (uint32_t j = 0; j < npte(); j++) {
            uint64_t pte = pte_get(pt, j);
            if (!(pte & E_P)) continue;
            uint32_t virt  = (gi << pde_shift()) | (j << 12);
            uint64_t frame = pte & addr_mask();

            if (pte & VMM_SHARED) {
                uint32_t fl = VMM_USER | VMM_SHARED | ((pte & E_RW) ? VMM_WRITABLE : 0) |
                              exec_of(pte);
                if (map_in_root(child->root, virt, frame, fl) != 0) {
                    vmm_space_destroy(child); return NULL;
                }
            } else if ((pte & E_RW) || (pte & VMM_COW)) {
                uint16_t* rc = cow_slot(frame);
                if (rc) cow_ref_share(rc);
                uint32_t cf = VMM_USER | VMM_COW | exec_of(pte);   /* §M86 */
                map_in_root_ex(parent->root, virt, frame, cf, /*notify*/0);
                if (map_in_root(child->root, virt, frame, cf) != 0) {
                    vmm_space_destroy(child); return NULL;
                }
            } else {
                pmm_phys_t nf = pmm_alloc_frame_user();   /* §M86 — may be highmem */
                if (!nf) { vmm_space_destroy(child); return NULL; }
                kmap_copy_frame(nf, frame);
                if (map_in_root(child->root, virt, nf, VMM_USER | exec_of(pte)) != 0) {
                    pmm_free_frame(nf); vmm_space_destroy(child); return NULL;
                }
            }
        }
    }
    /* §M51 — write access was taken AWAY from the parent, which may be
     * running on another core right now: one whole-space shootdown. */
    hal_tlb_shootdown(0, 0);
    return child;
}

int vmm_cow_fault(uintptr_t fault_va) {
    struct task* t = task_current();
    if (!t || !t->mm) return 0;
    struct vmm_space* s = t->mm;

    uint32_t va = (uint32_t)fault_va;
    uint64_t pde = pde_get(s->root, pde_index(va));
    if (!(pde & E_P) || (pde & E_PS)) return 0;
    uint64_t pt = pde & addr_mask();
    uint32_t j  = pte_index(va);
    uint64_t pte = pte_get(pt, j);
    if (!(pte & E_P) || !(pte & VMM_COW)) return 0;   /* not COW → real fault */

    uint64_t old = pte & addr_mask();
    uint16_t* rc = cow_slot(old);
    if (!rc || cow_ref_sole(rc)) {
        /* Last (or untracked) sharer — writable in place. */
        pte_set(pt, j, old | E_P | E_US | E_RW | (pte & E_NX));
        if (rc) __atomic_store_n(rc, 0, __ATOMIC_RELEASE);
    } else {
        pmm_phys_t nf = pmm_alloc_frame_user();       /* §M86 — may be highmem */
        if (!nf) return 0;                            /* OOM → real fault */
        kmap_copy_frame(nf, old);
        pte_set(pt, j, (nf & addr_mask()) | E_P | E_US | E_RW | (pte & E_NX));
        /* Give up our share only after the copy (cowref.h). */
        if (cow_ref_put_copy(rc)) pmm_free_frame(old);
    }
    hal_tlb_shootdown(0, va & ~0xFFFu);             /* §M51 */
    return 1;
}

int vmm_space_map(struct vmm_space* s, uintptr_t virt, uint64_t phys, uint32_t flags) {
    if (!s) return vmm_map(virt, phys, flags);      /* NULL == kernel space */
    return map_in_root(s->root, (uint32_t)virt, phys, flags);
}

/* §M74 (2026-09-27) — a USER space's unmap RELEASES what the entry held.  It
 * used to clear the entry only, so every MAP_FIXED overlay leaked the frame it
 * covered (never freed, not even at exit) and a COW frame's share count never
 * came down.  The kernel space (s == NULL) keeps the old behaviour: its
 * mappings are device windows and identity pages nobody allocated per-map. */
void vmm_space_unmap(struct vmm_space* s, uintptr_t virt) {
    if (!s) { unmap_in_root(kroot(), (uint32_t)virt); return; }
    uint32_t v = (uint32_t)virt;
    if (pde_is_kernel_shared(s, pde_index(v))) return;   /* never a user page */
    uint64_t pde = pde_get(s->root, pde_index(v));
    if (!(pde & E_P) || (pde & E_PS)) return;
    uint64_t pt = pde & addr_mask();
    uint64_t pte = pte_get(pt, pte_index(v));
    if (!pte) return;
    /* Only a USER page (or an evicted user page) is this space's to drop: a
     * kernel entry in a shared table would vanish for every space at once. */
    if ((pte & E_P) ? !(pte & E_US) : !(pte & VMM_SWPE_MARK)) return;
    pte_set(pt, pte_index(v), 0);
    hal_tlb_shootdown(0, v);                        /* §M51 — before the free */
    release_user_entry(pte);
}

/* §M74 — the page cache holds a frame as one more COW sharer (see pcache.c). */
void vmm_frame_share(uint64_t phys) {
    uint16_t* rc = cow_slot(phys);
    if (rc) cow_ref_share(rc);
}
int vmm_frame_unshare(uint64_t phys) {
    uint16_t* rc = cow_slot(phys);
    return rc ? cow_ref_drop(rc) : 1;
}

int vmm_space_protect(struct vmm_space* s, uintptr_t virt, uint32_t flags) {
    return protect_in_root(s ? s->root : kroot(), (uint32_t)virt, flags);
}

/* §M75 — enumerate every present page in this space's PRIVATE region; the
 * traversal is vmm_space_destroy's minus the freeing, and must stay so. */
void vmm_space_walk(struct vmm_space* s, vmm_walk_fn cb, void* ctx) {
    if (!s || !cb) return;
    for (uint32_t gi = 0; gi < npde(); gi++) {
        uint64_t pde = pde_get(s->root, gi);
        if (!(pde & E_P) || (pde & E_PS)) continue;
        if (pde_is_kernel_shared(s, gi)) continue;
        uint64_t pt = pde & addr_mask();
        for (uint32_t j = 0; j < npte(); j++) {
            uint64_t pte = pte_get(pt, j);
            if (!(pte & E_P)) continue;
            cb(ctx, ((uintptr_t)gi << pde_shift()) | ((uintptr_t)j << 12),
               pte & addr_mask(), (uint32_t)(pte & 0xFFFu & ~(uint64_t)VMM_EXEC) |
                                  exec_of(pte));
        }
    }
}

/* §M74 — see vmm_flags.h.  The clear is a 32-bit atomic AND on the entry's LOW
 * word in both formats: bit 5 lives there in a PAE entry too (little-endian),
 * and a 64-bit atomic on i386 would need cmpxchg8b through libatomic. */
uint32_t vmm_space_age(struct vmm_space* s, vmm_walk_fn cb, void* ctx) {
    if (!s) return 0;
    uint32_t cleared = 0;
    for (uint32_t gi = 0; gi < npde(); gi++) {
        uint64_t pde = pde_get(s->root, gi);
        if (!(pde & E_P) || (pde & E_PS)) continue;
        if (pde_is_kernel_shared(s, gi)) continue;
        uint64_t pt = pde & addr_mask();
        for (uint32_t j = 0; j < npte(); j++) {
            uint64_t pte = pte_get(pt, j);
            if (!(pte & E_P)) continue;
            uint32_t* lo = g_pae ? (uint32_t*)(uintptr_t)pt + 2 * j
                                 : (uint32_t*)(uintptr_t)pt + j;
            uint32_t was = __atomic_fetch_and(lo, ~(uint32_t)VMM_ACCESSED, __ATOMIC_RELAXED);
            if (was & VMM_ACCESSED) cleared++;
            if (cb) cb(ctx, ((uintptr_t)gi << pde_shift()) | ((uintptr_t)j << 12),
                       pte & addr_mask(),
                       (uint32_t)((pte & 0xFFFu & ~(uint64_t)(VMM_EXEC | VMM_ACCESSED)) |
                                  exec_of(pte) | (was & VMM_ACCESSED)));
        }
    }
    return cleared;
}
void vmm_age_flush(void) { hal_tlb_shootdown(0, 0); }
uint64_t vmm_af_fault_count(void) { return 0; }   /* x86 sets A itself */   /* full, everywhere */

/* §M72 stage 3 — see vmm_flags.h. */
int vmm_space_mark_swapped(struct vmm_space* s, uintptr_t va, uint32_t slot, uint32_t flags) {
    if (!s) return -1;
    uint32_t v = (uint32_t)va, gi = pde_index(v);
    if (pde_is_kernel_shared(s, gi)) return -1;
    uint64_t pde = pde_get(s->root, gi);
    if (!(pde & E_P) || (pde & E_PS)) return -1;
    uint64_t pt = pde & addr_mask();
    uint32_t j = pte_index(v);
    if (!(pte_get(pt, j) & E_P)) return -1;
    uint64_t e = ((uint64_t)slot << 12) | VMM_SWPE_MARK |
                 ((flags & VMM_WRITABLE) ? VMM_SWPE_W : 0) |
                 ((flags & VMM_EXEC) ? VMM_SWPE_X : 0);
    /* §M74 — ATOMICALLY, because the owner may be RUNNING (pressure eviction
     * does not pause it): a plain store can race another CPU's hardware
     * setting A/D in the same entry.  The marker fits in the low word (slot <
     * 65536), so clearing the present bit is one 32-bit exchange in both
     * formats; the hardware only updates a PRESENT entry, and in PAE the high
     * word (NX, upper address bits) is zeroed after P is already clear. */
    uint32_t* lo = g_pae ? (uint32_t*)(uintptr_t)pt + 2 * j : (uint32_t*)(uintptr_t)pt + j;
    __atomic_exchange_n(lo, (uint32_t)e, __ATOMIC_ACQ_REL);
    if (g_pae) ((volatile uint32_t*)lo)[1] = 0;
    hal_tlb_shootdown(0, v);                        /* §M51 — present -> absent */
    return 0;
}

int vmm_space_swap_out(struct vmm_space* s, uintptr_t va, uint32_t slot,
                       uint64_t* old_raw, uint64_t* old_phys, uint32_t* old_flags, int flush) {
    if (!s) return -1;
    uint32_t v = (uint32_t)va, gi = pde_index(v);
    if (pde_is_kernel_shared(s, gi)) return -1;
    uint64_t pde = pde_get(s->root, gi);
    if (!(pde & E_P) || (pde & E_PS)) return -1;
    uint64_t pt = pde & addr_mask();
    uint32_t j = pte_index(v);
    uint32_t* lo = g_pae ? (uint32_t*)(uintptr_t)pt + 2 * j : (uint32_t*)(uintptr_t)pt + j;
    uint64_t cur = pte_get(pt, j);
    if (!(cur & E_P) || !(cur & E_US)) return -1;
    uint32_t mark = (slot << 12) | VMM_SWPE_MARK | ((cur & E_RW) ? VMM_SWPE_W : 0) |
                    (exec_of(cur) ? VMM_SWPE_X : 0);
    uint32_t was_lo = __atomic_exchange_n(lo, mark, __ATOMIC_ACQ_REL);
    uint64_t hi = g_pae ? ((volatile uint32_t*)lo)[1] : 0;
    if (!(was_lo & E_P)) {                      /* changed under us: put it back */
        __atomic_store_n(lo, was_lo, __ATOMIC_RELEASE);
        return -1;
    }
    if (g_pae) ((volatile uint32_t*)lo)[1] = 0;
    if (flush) hal_tlb_shootdown(0, v);         /* §M51 — before anyone copies */
    uint64_t raw = (hi << 32) | was_lo;
    *old_raw = raw;
    *old_phys = raw & addr_mask();
    *old_flags = (uint32_t)(raw & 0xFFFu & ~(uint64_t)VMM_EXEC) | exec_of(raw);
    return 0;
}
void vmm_space_swap_undo(struct vmm_space* s, uintptr_t va, uint64_t raw) {
    uint32_t v = (uint32_t)va;
    uint64_t pde = pde_get(s->root, pde_index(v));
    if (!(pde & E_P) || (pde & E_PS)) return;
    uint64_t pt = pde & addr_mask();
    uint32_t j = pte_index(v);
    uint32_t* lo = g_pae ? (uint32_t*)(uintptr_t)pt + 2 * j : (uint32_t*)(uintptr_t)pt + j;
    if (g_pae) ((volatile uint32_t*)lo)[1] = (uint32_t)(raw >> 32);   /* high first */
    __atomic_store_n(lo, (uint32_t)raw, __ATOMIC_RELEASE);          /* P last */
}

void vmm_space_walk_swapped(struct vmm_space* s, vmm_swapped_fn cb, void* ctx) {
    if (!s || !cb) return;
    for (uint32_t gi = 0; gi < npde(); gi++) {
        uint64_t pde = pde_get(s->root, gi);
        if (!(pde & E_P) || (pde & E_PS)) continue;
        if (pde_is_kernel_shared(s, gi)) continue;
        uint64_t pt = pde & addr_mask();
        for (uint32_t j = 0; j < npte(); j++) {
            uint64_t pte = pte_get(pt, j);
            if ((pte & E_P) || !(pte & VMM_SWPE_MARK)) continue;
            uint32_t fl = VMM_USER | ((pte & VMM_SWPE_W) ? VMM_WRITABLE : 0) |
                          ((pte & VMM_SWPE_X) ? VMM_EXEC : 0);
            cb(ctx, ((uintptr_t)gi << pde_shift()) | ((uintptr_t)j << 12),
               (uint32_t)(pte >> 12), fl);
        }
    }
}

int vmm_space_swapped_entry(struct vmm_space* s, uintptr_t va, uint32_t* slot, uint32_t* flags) {
    if (!s) return -1;
    uint32_t v = (uint32_t)va, gi = pde_index(v);
    if (pde_is_kernel_shared(s, gi)) return -1;
    uint64_t pde = pde_get(s->root, gi);
    if (!(pde & E_P) || (pde & E_PS)) return -1;
    uint64_t pte = pte_get(pde & addr_mask(), pte_index(v));
    if ((pte & E_P) || !(pte & VMM_SWPE_MARK)) return -1;
    *slot = (uint32_t)(pte >> 12);
    *flags = VMM_USER | ((pte & VMM_SWPE_W) ? VMM_WRITABLE : 0) | ((pte & VMM_SWPE_X) ? VMM_EXEC : 0);
    return 0;
}

/* §M75 — a QUERY: must not build the table (cow_slot would). */
uint32_t vmm_frame_share_count(uint64_t phys) {
    if (!g_cow_ref) return 0;
    uint32_t fn = (uint32_t)(phys >> 12);
    return (fn >= pmm_pfn_base && fn < g_cow_nr) ? (uint32_t)g_cow_ref[fn] : 0;
}

uintptr_t vmm_space_pd_phys(struct vmm_space* s) {
    return s ? (uintptr_t)s->root_phys : (uintptr_t)kroot_phys();
}
uintptr_t vmm_space_root_phys(struct vmm_space* s) { return vmm_space_pd_phys(s); }

void vmm_space_switch(struct vmm_space* s) {
    uint32_t target = s ? s->root_phys : kroot_phys();
    /* Reload only on a change.  With PAE a CR3 load also re-reads the four
     * PDPTEs; ours never change after a space is built, so skipping the
     * reload when CR3 already matches is still correct. */
    if (read_cr3() != target) load_cr3(target);
}

/* User region base: 1 GiB, above the identity map and the kmap window. */
uintptr_t vmm_user_base(void) { return 0x40000000u; }

/* §M48 — the space's mmap bump cursor (policy stays in usyscall.c). */
uintptr_t vmm_space_mmap_cursor(struct vmm_space* s) { return s ? s->mmap_cursor : 0; }
void vmm_space_set_mmap_cursor(struct vmm_space* s, uintptr_t v) { if (s) s->mmap_cursor = v; }

/* ---------------------------------------------------------------------------
 * §M86 — kmap_frame / kunmap_frame for i386 (contract in kmap.h).
 *
 * A directly-mapped frame is its own address.  Anything else gets this CPU's
 * next window slot: write the PTE, flush THIS CPU's entry, and keep
 * preemption off until the matching kunmap so the task cannot migrate away
 * from its slot — which is also why no cross-CPU shootdown is needed.  With
 * PAE the frame may lie above 4 GiB; the window is the only way to reach it.
 *
 * Before paging is on, a frame below 4 GiB is directly addressable and comes
 * back as-is; one above 4 GiB cannot be reached at all, which is why the PMM
 * seeds those only after vmm_init (pmm_seed_deferred).
 * ------------------------------------------------------------------------- */
void* kmap_frame(pmm_phys_t frame) {
    uint64_t f = (uint64_t)frame & ~0xFFFull;
    if (f < (uint64_t)IDENTITY_MAP_MIB * 1024u * 1024u)
        return (void*)(uintptr_t)frame;
    if (!kmap_paging_on) {
        if (f >> 32) return NULL;                 /* unreachable before paging */
        return (void*)(uintptr_t)frame;
    }
    preempt_disable();
    uint32_t fl = hal_intr_save();
    int c = this_cpu_id();
    if (c < 0 || c >= KMAP_CPUS) c = 0;
    int d = kmap_depth[c];
    if (d >= KMAP_PER_CPU) {
        hal_intr_restore(fl);
        preempt_enable();
        kprintf("!! KMAP: cpu %d has %d mappings open - a kunmap_frame is "
                "missing\n", c, d);
        return NULL;
    }
    kmap_depth[c] = d + 1;
    uint32_t idx = (uint32_t)(c * KMAP_PER_CPU + d);
    uint32_t va  = KMAP_BASE + (idx << 12);
    if (g_pae) kmap_pt64[idx] = f | E_P | E_RW;
    else       kmap_pt32[idx] = (uint32_t)f | (uint32_t)(E_P | E_RW);
    invlpg(va);
    hal_intr_restore(fl);
    return (void*)(uintptr_t)(va | ((uint32_t)frame & 0xFFFu));
}

void kunmap_frame(void* p) {
    uint32_t va = (uint32_t)(uintptr_t)p & ~0xFFFu;
    if (va < KMAP_BASE || va >= KMAP_BASE + (uint32_t)(KMAP_CPUS * KMAP_PER_CPU) * 4096u)
        return;                                   /* a direct address: nothing */
    uint32_t fl = hal_intr_save();
    int c = this_cpu_id();
    if (c < 0 || c >= KMAP_CPUS) c = 0;
    uint32_t idx = (va - KMAP_BASE) >> 12;
    if (g_pae) kmap_pt64[idx] = 0;
    else       kmap_pt32[idx] = 0;
    invlpg(va);
    if (kmap_depth[c] > 0) kmap_depth[c]--;
    hal_intr_restore(fl);
    preempt_enable();
}
