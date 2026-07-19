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
#include "kmalloc.h"
#include <stdint.h>
#include <stddef.h>

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

/* M19.5.1 — extend the identity map past the boot-time 1 GiB cap.
 *
 * Strategy: 1 GiB pages in PDPT[1..].  Long mode allows PS=1 at the
 * PDPT level to mean "1 GiB page" (AMD64 APM Vol 2 §5.3.7).  Every
 * x86_64 CPU since K10 / Nehalem supports it; QEMU TCG supports it
 * unconditionally.  If a CPU ever didn't, we'd get #GP on the first
 * access — fall-back is to drop down to 2 MiB pages (more PD frames,
 * not implemented today).
 *
 * Each PDPT entry covers 1 GiB.  We add entries for [1 GiB, end_phys),
 * leaving PDPT[0] (which boot.s set up via 2 MiB pages) untouched.
 * No TLB flush needed — we're adding mappings, not changing existing
 * ones.  Returns the new physical extent actually covered (rounded up
 * to 1 GiB granularity, capped at 512 GiB — PDPT capacity).
 *
 * Caller (pmm_init) is expected to clamp this to BUDDY_MAX_FRAMES.
 * We don't do that capping here so the HAL stays single-purpose. */
uintptr_t hal_extend_identity_map(uintptr_t end_phys) {
    /* Round up to 1 GiB boundary. */
    const uintptr_t GIB = (uintptr_t)1 << 30;
    uintptr_t end = (end_phys + GIB - 1) & ~(GIB - 1);
    /* boot.s gave us PDPT[0] = first 1 GiB.  Nothing to do if request
     * fits in there. */
    if (end <= GIB) return GIB;
    /* PDPT cap: 512 entries × 1 GiB = 512 GiB.  We won't hit this
     * any time soon. */
    if (end > GIB * 512) end = GIB * 512;

    uint64_t* pdpt_tbl = (uint64_t*)pdpt;
    for (uintptr_t addr = GIB; addr < end; addr += GIB) {
        unsigned idx = (unsigned)((addr >> 30) & 0x1FF);
        if (pdpt_tbl[idx] & PTE_P) continue;        /* already mapped */
        pdpt_tbl[idx] = ((uint64_t)addr & PAGE_MASK_4K)
                      | PTE_P | PTE_RW | PTE_PS;
    }
    return end;
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
static uint64_t* walk_to_pt_root(uint64_t* root, uintptr_t virt, int create,
                                 uint32_t parent_flags) {
    uint64_t* tbl = root;
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

/* Kernel-space walker — the original signature, rooted at the boot PML4. */
static uint64_t* walk_to_pt(uintptr_t virt, int create, uint32_t parent_flags) {
    return walk_to_pt_root((uint64_t*)pml4, virt, create, parent_flags);
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

/* ===========================================================================
 * Per-process address spaces (M25 stage 1).
 *
 * The x86_64 twist vs i386: the *entire* kernel address space lives under
 * PML4[0] (everything the kernel touches is below 512 GiB), so a bare PML4
 * copy would share the whole low-512-GiB subtree — including the user
 * region — and user mappings would pollute the kernel.  To keep the user
 * region PRIVATE while the kernel stays mapped, a new space gets its own
 * copy of PML4 AND its own copy of the PDPT under PML4[0]; that private
 * PDPT shares the kernel's PD subtrees (identity, framebuffer, …) BY
 * POINTER but owns the currently-empty slots the user region falls into
 * (user VA 0x40000000 = PDPT[1], empty on our ≤1 GiB-RAM configs).  User
 * page tables created under it are therefore invisible to the kernel and
 * to other spaces.  Same boot-time-mappings-only caveat as vmm.h notes.
 * =========================================================================== */

struct vmm_space {
    uint64_t* pml4;         /* process PML4 (identity: virt == phys) */
    uintptr_t pml4_phys;
};

static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void write_cr3(uint64_t v) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory");
}

struct vmm_space* vmm_space_create(void) {
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof(*s));
    if (!s) return NULL;

    uint32_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) { kfree(s); return NULL; }
    s->pml4      = (uint64_t*)(uintptr_t)pml4_phys;
    s->pml4_phys = pml4_phys;

    /* Snapshot the kernel PML4 (mostly just [0]). */
    uint64_t* kpml4 = (uint64_t*)pml4;
    for (int i = 0; i < 512; i++) s->pml4[i] = kpml4[i];

    /* Give PML4[0] a PRIVATE PDPT so the user region under it doesn't write
     * into the shared kernel PDPT.  Copy the kernel PDPT entries (sharing
     * the kernel PD subtrees by pointer); the empty user slot stays empty
     * and becomes private on first map. */
    uint64_t k0 = kpml4[0];
    if (k0 & PTE_P) {
        uint32_t pdpt_phys = pmm_alloc_frame();
        if (!pdpt_phys) { pmm_free_frame(pml4_phys); kfree(s); return NULL; }
        uint64_t* kpdpt = table_at((uintptr_t)k0);
        uint64_t* npdpt = (uint64_t*)(uintptr_t)pdpt_phys;
        for (int i = 0; i < 512; i++) npdpt[i] = kpdpt[i];
        /* Point PML4[0] at the private PDPT; keep P+RW, USER widened lazily
         * by walk_to_pt_root when the first user page is mapped. */
        s->pml4[0] = ((uint64_t)pdpt_phys & PAGE_MASK_4K) | PTE_P | PTE_RW;
    }
    return s;
}

/* Free the tables a space added on top of the kernel snapshot, one level
 * at a time: a table entry still equal to the kernel's is shared and left
 * alone; anything else that this space introduced is freed (deepest
 * first).  `depth` 0=PML4 1=PDPT 2=PD; PTs (depth 3) hold user frames. */
static void free_subtree(uint64_t* tbl, uint64_t* ktbl, int depth) {
    for (int i = 0; i < 512; i++) {
        uint64_t e = tbl[i];
        if (!(e & PTE_P)) continue;
        if (ktbl && e == ktbl[i]) continue;          /* shared with kernel */
        if (depth >= 1 && (e & PTE_PS)) continue;    /* large page, not ours to free */

        uint64_t* child  = table_at((uintptr_t)e);
        uint64_t* kchild = (ktbl && (ktbl[i] & PTE_P) && !(ktbl[i] & PTE_PS))
                         ? table_at((uintptr_t)ktbl[i]) : NULL;
        if (depth < 3) {
            free_subtree(child, kchild, depth + 1);
            pmm_free_frame((uint32_t)((uintptr_t)e & PAGE_MASK_4K));
        } else {
            /* depth 3 = PT: user pages.  Skip VMM_SHARED (borrowed) ones —
             * their owner (e.g. a shm object) frees them. */
            if (!(e & VMM_SHARED))
                pmm_free_frame((uint32_t)((uintptr_t)e & PAGE_MASK_4K));
        }
    }
}

void vmm_space_destroy(struct vmm_space* s) {
    if (!s) return;
    free_subtree(s->pml4, (uint64_t*)pml4, 0);
    pmm_free_frame((uint32_t)s->pml4_phys);
    kfree(s);
}

int vmm_space_map(struct vmm_space* s, uintptr_t virt, uintptr_t phys,
                  uint32_t flags) {
    if (!s) return vmm_map(virt, phys, flags);
    uint64_t* pt = walk_to_pt_root(s->pml4, virt, /*create*/1, flags);
    if (!pt) return -1;
    /* VMM_SHARED (0x400) rides in PTE bit 10 (OS-available) so
     * free_subtree can skip borrowed frames it doesn't own. */
    pt[IDX_PT(virt)] = ((uint64_t)phys & PAGE_MASK_4K)
                     | PTE_P | ((uint64_t)flags & (PTE_RW | PTE_US | VMM_SHARED));
    invlpg(virt);
    return 0;
}

void vmm_space_unmap(struct vmm_space* s, uintptr_t virt) {
    if (!s) { vmm_unmap(virt); return; }
    uint64_t* pt = walk_to_pt_root(s->pml4, virt, /*create*/0, 0);
    if (!pt) return;
    pt[IDX_PT(virt)] = 0;
    invlpg(virt);
}

/* Change the protection of an already-mapped page WITHOUT touching its frame
 * (the mprotect primitive — §M37: musl's mallocng maps a PROT_NONE reservation
 * then mprotects the used part to R/W, and ld.so tightens RELRO to read-only).
 * Preserves the OS-available SHARED bit.  Returns 0, or -1 if unmapped. */
int vmm_space_protect(struct vmm_space* s, uintptr_t virt, uint32_t flags) {
    uint64_t* pt = s ? walk_to_pt_root(s->pml4, virt, /*create*/0, 0)
                     : walk_to_pt(virt, /*create*/0, 0);
    if (!pt) return -1;
    uint64_t pte = pt[IDX_PT(virt)];
    if ((pte & PTE_P) == 0) return -1;             /* not present */
    pt[IDX_PT(virt)] = (pte & PAGE_MASK_4K) | PTE_P
                     | ((uint64_t)flags & (PTE_RW | PTE_US))
                     | (pte & VMM_SHARED);
    invlpg(virt);
    return 0;
}

uintptr_t vmm_space_pd_phys(struct vmm_space* s) {
    return s ? s->pml4_phys : (uintptr_t)pml4;
}

void vmm_space_switch(struct vmm_space* s) {
    uint64_t target = s ? (uint64_t)s->pml4_phys : (uint64_t)(uintptr_t)pml4;
    if (read_cr3() != target) write_cr3(target);
}

/* User region base: 1 GiB (PDPT[1]), above the boot-time 1 GiB identity
 * map.  Valid while RAM ≤ 1 GiB (our configs); a larger identity map would
 * push this higher — revisit with the ELF loader if we run big-RAM guests. */
uintptr_t vmm_user_base(void) { return 0x40000000u; }
