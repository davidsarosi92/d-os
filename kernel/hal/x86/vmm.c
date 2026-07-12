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
#include "kmalloc.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

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

/* Phys address of the page directory — needed by the AP boot trampoline
 * (M18) so each AP can load CR3 before enabling paging.  The kernel is
 * identity-mapped, so virt = phys for the array itself.  Returned as
 * uintptr_t for arch-API symmetry with the x86_64 vmm.c; the SMP code
 * truncates to uint32_t when patching the AP info area (i386 PD is in
 * the low 4 GiB by construction). */
uintptr_t vmm_kernel_pd_phys(void) { return (uintptr_t)&kernel_pd[0]; }

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

/* Core 4 KiB map, parameterised by the target page directory.  Both the
 * kernel PD (vmm_map) and a per-process space's PD (vmm_space_map, M25)
 * share this exact walk.  `pd` points at a 1024-entry PDE array reachable
 * through the identity map (every PD we allocate lives below 256 MiB). */
static int map_in_pd(uint32_t* pd, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pdi = PD_IDX(virt);
    uint32_t pti = PT_IDX(virt);
    uint32_t pde = pd[pdi];

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
        pd[pdi] = pt_phys | PDE_P | PDE_RW | (flags & PDE_US);
    } else {
        pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    }

    /* VMM_SHARED (0x400) / VMM_COW (0x800) ride along in PTE OS-available bits
     * 10/11 so vmm_space_destroy + the COW fault path can classify the frame. */
    pt[pti] = (phys & PAGE_MASK) | PTE_P | (flags & (PTE_RW | PTE_US | VMM_SHARED | VMM_COW));
    invlpg(virt);
    return 0;
}

/* Core unmap, parameterised by the target page directory. */
static void unmap_in_pd(uint32_t* pd, uint32_t virt) {
    uint32_t pdi = PD_IDX(virt);
    uint32_t pti = PT_IDX(virt);
    uint32_t pde = pd[pdi];

    if ((pde & PDE_P) == 0) return;             /* already unmapped */
    if (pde & PDE_PS) return;                   /* PSE region — refuse */

    uint32_t* pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    pt[pti] = 0;
    invlpg(virt);
}

int vmm_map(uintptr_t virt32, uintptr_t phys32, uint32_t flags) {
    /* On i386 uintptr_t == uint32_t so the casts are no-ops; making them
     * explicit keeps the arch-portable interface obvious. */
    return map_in_pd(kernel_pd, (uint32_t)virt32, (uint32_t)phys32, flags);
}

int vmm_map_4mib(uintptr_t virt32, uintptr_t phys32, uint32_t flags) {
    uint32_t virt = (uint32_t)virt32;
    uint32_t phys = (uint32_t)phys32;
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

void vmm_unmap(uintptr_t virt32) {
    unmap_in_pd(kernel_pd, (uint32_t)virt32);
}

uintptr_t vmm_translate(uintptr_t virt32) {
    uint32_t virt = (uint32_t)virt32;
    uint32_t pdi = PD_IDX(virt);
    uint32_t pde = kernel_pd[pdi];
    if ((pde & PDE_P) == 0) return 0;

    if (pde & PDE_PS) {
        /* 4 MiB PSE page — low 22 bits are the offset. */
        return (uintptr_t)((pde & PSE_MASK) | (virt & 0x003FFFFFu));
    }

    uint32_t* pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    uint32_t pte = pt[PT_IDX(virt)];
    if ((pte & PTE_P) == 0) return 0;
    return (uintptr_t)((pte & PAGE_MASK) | (virt & 0x00000FFFu));
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

/* ===========================================================================
 * Per-process address spaces (M25 stage 1).
 *
 * On i386 a `vmm_space` is a private 1024-entry page directory.  It is
 * created by *snapshotting* the kernel PD (so the identity map + every
 * boot-time kernel high-mapping stays reachable after a CR3 switch — the
 * kernel code and stack keep resolving) and then receives the process's
 * own user-region PTs on top.  Because the kernel region below 256 MiB is
 * PSE leaves (no shared PT pages) and the high mappings are static and
 * boot-time, the snapshot copy is sufficient — see the vmm.h note on the
 * stage-1 kernel-mapping limitation.
 * =========================================================================== */

struct vmm_space {
    uint32_t* pd;           /* 4 KiB page directory (identity: virt == phys) */
    uint32_t  pd_phys;      /* == (uint32_t)pd, cached for CR3 loads */
};

static inline uint32_t read_cr3(void) {
    uint32_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}

/* Is PDE index `i` part of the shared kernel region?  A user space only
 * ever adds mappings whose PDE differs from the kernel snapshot; anything
 * still identical to kernel_pd[i] is shared kernel and must not be freed. */
static int pde_is_kernel_shared(struct vmm_space* s, uint32_t i) {
    return s->pd[i] == kernel_pd[i];
}

struct vmm_space* vmm_space_create(void) {
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof(*s));
    if (!s) return NULL;

    uint32_t pd_phys = pmm_alloc_frame();       /* one 4 KiB frame = 1024 PDEs */
    if (!pd_phys) { kfree(s); return NULL; }

    s->pd      = (uint32_t*)(uintptr_t)pd_phys; /* reachable via identity map */
    s->pd_phys = pd_phys;

    /* Snapshot the kernel directory: identity map + all boot-time high
     * mappings.  The private user region (high PDEs) is 0 in kernel_pd,
     * so it starts empty here too. */
    for (int i = 0; i < 1024; i++) s->pd[i] = kernel_pd[i];
    return s;
}

/* M34 — per-frame COW reference counts, indexed by frame number (phys >> 12).
 * Covers the i386 low-memory region the PMM allocates user pages from (256 MiB
 * → 64 Ki frames).  A count of 0 means "not COW-shared" (a normally-owned page
 * freed by its single owner); a page made COW by fork starts at 2. */
#define COW_MAX_FRAMES  (256u * 1024u * 1024u / 4096u)   /* 65536 */
static uint16_t g_cow_ref[COW_MAX_FRAMES];

static inline uint16_t* cow_slot(uint32_t phys) {
    uint32_t fn = phys >> 12;
    return (fn < COW_MAX_FRAMES) ? &g_cow_ref[fn] : NULL;
}

void vmm_space_destroy(struct vmm_space* s) {
    if (!s) return;

    /* Free every page table + user frame this space added on top of the
     * kernel snapshot.  Kernel-shared PDEs (identical to kernel_pd) and
     * PSE leaves are left alone. */
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t pde = s->pd[i];
        if ((pde & PDE_P) == 0) continue;
        if (pde & PDE_PS)            continue;  /* PSE leaf — never ours */
        if (pde_is_kernel_shared(s, i)) continue;  /* shared kernel PT */

        uint32_t* pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
        for (int j = 0; j < 1024; j++) {
            uint32_t pte = pt[j];
            if (!(pte & PTE_P))   continue;
            if (pte & VMM_SHARED) continue;     /* borrowed shm — owner frees   */
            if (pte & VMM_COW) {
                /* COW-shared: free the frame only when the last owner leaves. */
                uint16_t* rc = cow_slot(pte & PAGE_MASK);
                if (rc && *rc > 1) { (*rc)--; continue; }
                if (rc) *rc = 0;
            }
            pmm_free_frame(pte & PAGE_MASK);     /* owned user page */
        }
        pmm_free_frame(pde & PAGE_MASK);        /* the page table itself */
    }
    pmm_free_frame(s->pd_phys);                 /* the directory */
    kfree(s);
}

struct vmm_space* vmm_space_clone(struct vmm_space* parent) {
    if (!parent) return NULL;
    struct vmm_space* child = vmm_space_create();     /* kernel snapshot only */
    if (!child) return NULL;

    /* Walk the parent's private user mappings.  WRITABLE pages become
     * copy-on-write (shared read-only in BOTH spaces, ref-counted); read-only
     * pages (code) are eagerly copied; borrowed shm pages stay shared. */
    for (uint32_t i = 0; i < 1024; i++) {
        uint32_t pde = parent->pd[i];
        if ((pde & PDE_P) == 0)          continue;
        if (pde & PDE_PS)                continue;   /* PSE leaf — kernel      */
        if (pde_is_kernel_shared(parent, i)) continue;

        uint32_t* pt = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
        for (uint32_t j = 0; j < 1024; j++) {
            uint32_t pte = pt[j];
            if ((pte & PTE_P) == 0) continue;
            uint32_t virt  = (i << 22) | (j << 12);
            uint32_t frame = pte & PAGE_MASK;

            if (pte & VMM_SHARED) {
                /* Borrowed shm frame — share it verbatim (don't own/copy). */
                uint32_t fl = VMM_USER | VMM_SHARED | (pte & PTE_RW ? VMM_WRITABLE : 0);
                if (map_in_pd(child->pd, virt, frame, fl) != 0) {
                    vmm_space_destroy(child); return NULL;
                }
            } else if ((pte & PTE_RW) || (pte & VMM_COW)) {
                /* Writable → COW.  This MUST also catch a page that is already
                 * COW from a PRIOR fork (RW=0 but VMM_COW set): such a page is
                 * logically writable-shared, not read-only code, so it must be
                 * re-shared COW (ref bumped), never eager-copied as RO — else a
                 * second fork whose parent hasn't yet resolved the page would
                 * hand the child a read-only copy that faults hard on write.
                 * Share the frame read-only in both spaces. */
                uint16_t* rc = cow_slot(frame);
                if (rc) { *rc = (*rc == 0) ? 2 : (uint16_t)(*rc + 1); }
                map_in_pd(parent->pd, virt, frame, VMM_USER | VMM_COW);  /* parent RO+COW */
                if (map_in_pd(child->pd, virt, frame, VMM_USER | VMM_COW) != 0) {
                    vmm_space_destroy(child); return NULL;
                }
            } else {
                /* Read-only (code) → eager private copy (cheap, rarely large). */
                uint32_t nf = pmm_alloc_frame();
                if (!nf) { vmm_space_destroy(child); return NULL; }
                const uint32_t* src = (const uint32_t*)(uintptr_t)frame;
                uint32_t* dst = (uint32_t*)(uintptr_t)nf;
                for (int k = 0; k < 1024; k++) dst[k] = src[k];
                if (map_in_pd(child->pd, virt, nf, VMM_USER) != 0) {
                    pmm_free_frame(nf); vmm_space_destroy(child); return NULL;
                }
            }
        }
    }
    return child;
}

int vmm_cow_fault(uintptr_t fault_va) {
    struct task* t = task_current();
    if (!t || !t->mm) return 0;
    struct vmm_space* s = t->mm;

    uint32_t va  = (uint32_t)fault_va;
    uint32_t pdi = PD_IDX(va), pti = PT_IDX(va);
    uint32_t pde = s->pd[pdi];
    if (!(pde & PDE_P) || (pde & PDE_PS)) return 0;

    uint32_t* pt  = (uint32_t*)(uintptr_t)(pde & PAGE_MASK);
    uint32_t  pte = pt[pti];
    if (!(pte & PTE_P) || !(pte & VMM_COW)) return 0;   /* not a COW page → real fault */

    uint32_t old = pte & PAGE_MASK;
    uint16_t* rc = cow_slot(old);

    if (!rc || *rc <= 1) {
        /* Last (or untracked) sharer — just make it writable in place. */
        pt[pti] = old | PTE_P | PTE_US | PTE_RW;
        if (rc) *rc = 0;
    } else {
        (*rc)--;                                        /* one fewer sharer   */
        uint32_t nf = pmm_alloc_frame();
        if (!nf) { (*rc)++; return 0; }                 /* OOM → real fault    */
        const uint32_t* src = (const uint32_t*)(uintptr_t)old;
        uint32_t* dst = (uint32_t*)(uintptr_t)nf;
        for (int k = 0; k < 1024; k++) dst[k] = src[k];
        pt[pti] = (nf & PAGE_MASK) | PTE_P | PTE_US | PTE_RW;
    }
    invlpg(va & PAGE_MASK);
    return 1;
}

int vmm_space_map(struct vmm_space* s, uintptr_t virt, uintptr_t phys,
                  uint32_t flags) {
    if (!s) return vmm_map(virt, phys, flags); /* NULL == kernel space */
    return map_in_pd(s->pd, (uint32_t)virt, (uint32_t)phys, flags);
}

void vmm_space_unmap(struct vmm_space* s, uintptr_t virt) {
    if (!s) { vmm_unmap(virt); return; }
    unmap_in_pd(s->pd, (uint32_t)virt);
}

uintptr_t vmm_space_pd_phys(struct vmm_space* s) {
    return s ? (uintptr_t)s->pd_phys : (uintptr_t)&kernel_pd[0];
}

void vmm_space_switch(struct vmm_space* s) {
    uint32_t target = s ? s->pd_phys : (uint32_t)(uintptr_t)&kernel_pd[0];
    /* Reload CR3 only on an actual change — switching between kernel
     * threads (all target == kernel_pd) costs nothing and avoids a
     * needless TLB flush. */
    if (read_cr3() != target) load_cr3(target);
}

/* User region base: 1 GiB, comfortably above the 256 MiB identity map. */
uintptr_t vmm_user_base(void) { return 0x40000000u; }
