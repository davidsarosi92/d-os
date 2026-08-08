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
#include "hal_api.h"   /* §M51 — hal_tlb_shootdown */
#include "pmm.h"
#include "printf.h"
#include "kmalloc.h"
#include "task.h"      /* task_current — COW resolves in the current space */
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
    /* Page tables are reached through the kernel direct map (§M48) — NOT as
     * bare physical addresses.  Low virtual addresses now belong to user
     * space, so a physical-as-virtual dereference would read whatever the
     * running process has mapped there.  Strip the low 12 bits (PTE flags). */
    return (uint64_t*)phys_to_virt(phys & PAGE_MASK_4K);
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

/* §M48 — install the KERNEL DIRECT MAP; stop growing the LOW identity map.
 *
 * This used to add 1 GiB pages to PDPT[1..], mapping physical N at virtual N
 * for as much RAM as existed.  That is exactly what collided with user space:
 * user programs are linked at vmm_user_base() (1 GiB) and cannot move, because
 * the small code model requires every symbol below 2 GiB.  So on any machine
 * with more than 1 GiB of RAM the identity map's 1 GiB page landed on top of
 * the user region, walk_to_pt_root refused to build a user mapping under a
 * large page, and every exec failed with ELF_ENOMEM.  The port only ever
 * looked healthy because it was always tested with -m 1024M.
 *
 * The low identity map therefore stays at the boot-time 1 GiB — below
 * vmm_user_base(), covering the kernel image and early structures, where it
 * can never be in a user program's way.  ALL of physical memory is mapped
 * instead in the canonical upper half at KERNEL_DIRECT_MAP_BASE (PML4[256]),
 * which no user mapping can reach.  One PDPT of 1 GiB pages covers 512 GiB.
 *
 * 1 GiB pages need PDPTE.PS (AMD64 APM Vol 2 §5.3.7) — present on every
 * x86_64 since K10 / Nehalem, unconditional in QEMU TCG.
 *
 * Adding mappings needs no TLB flush: nothing changed, only filled in. */

/* The direct map's PDPT.  In .bss, so it lives inside the kernel image below
 * 1 GiB and its link address IS its physical address — which is what the PML4
 * entry has to contain. */
static uint64_t dm_pdpt[512] __attribute__((aligned(4096)));

uintptr_t hal_extend_identity_map(uintptr_t end_phys) {
    const uintptr_t GIB = (uintptr_t)1 << 30;
    uintptr_t end = (end_phys + GIB - 1) & ~(GIB - 1);
    if (end > GIB * 512) end = GIB * 512;          /* one PDPT's worth */
    if (end < GIB)       end = GIB;

    for (uintptr_t addr = 0; addr < end; addr += GIB)
        dm_pdpt[(addr >> 30) & 0x1FF] =
            ((uint64_t)addr & PAGE_MASK_4K) | PTE_P | PTE_RW | PTE_PS;

    /* Hook it under PML4[256].  Kernel-only — no PTE_US anywhere on this
     * path, so ring 3 can never walk into the direct map. */
    ((uint64_t*)pml4)[256] =
        ((uint64_t)(uintptr_t)dm_pdpt & PAGE_MASK_4K) | PTE_P | PTE_RW;

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
/* §M46/security — is [va, va+len) fully mapped + USER-accessible in the ACTIVE
 * address space (process CR3, live during a syscall)?  The 64-bit twin of the
 * i386 check: walks PML4→PDPT→PD→PT via CR3, requiring P + U/S at every level
 * (and R/W too if want_write) so a syscall can safely touch a ring-3 pointer.
 * Every page table lives in the identity-mapped physical region. */
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write) {
    if (len == 0) return 1;
    if (va < vmm_user_base()) return 0;
    if (va + len < va)        return 0;                 /* overflow */
    const uint64_t AMASK = 0x000FFFFFFFFFF000ULL;       /* phys addr bits 51:12 */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    const int shifts[4] = { 39, 30, 21, 12 };
    for (uintptr_t p = va & ~0xFFFUL; p < va + len; p += 0x1000) {
        uint64_t* tbl = (uint64_t*)phys_to_virt(cr3 & AMASK);
        int mapped = 0;
        for (int level = 0; level < 4; level++) {
            uint64_t e = tbl[((unsigned)(p >> shifts[level])) & 0x1FFu];
            if (!(e & PTE_P) || !(e & PTE_US)) return 0;
            if (want_write && !(e & PTE_RW))   return 0;
            if (level == 3 || (level >= 1 && (e & PTE_PS))) { mapped = 1; break; }
            tbl = (uint64_t*)phys_to_virt(e & AMASK);
        }
        if (!mapped) return 0;
    }
    return 1;
}

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
        pmm_phys_t newphys = pmm_alloc_frame();
        if (!newphys) return 0;
        uint64_t* newtbl = (uint64_t*)phys_to_virt(newphys);
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
    int was_present = (pt[idx] & PTE_P) != 0;
    pt[idx] = ((uint64_t)phys & PAGE_MASK_4K)
            | PTE_P
            | ((uint64_t)flags & (PTE_RW | PTE_US));
    if (was_present) hal_tlb_shootdown(0, virt);   /* §M51 — a remap weakens */
    else             invlpg(virt);
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
            pmm_phys_t newphys = pmm_alloc_frame();
            if (!newphys) return -3;
            uint64_t* newtbl = (uint64_t*)phys_to_virt(newphys);
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
    hal_tlb_shootdown(0, virt);          /* §M51 — present → absent */
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

    /* §M48 — the mmap bump cursor lives with the ADDRESS SPACE, not with the
     * task.  See the i386 twin for why a per-task cursor (or a snapshot copied
     * at clone time) lets one thread map over another's memory. */
    uintptr_t mmap_cursor;
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
    /* kmalloc does not zero: an uninitialised bump cursor is handed straight
     * back to the program as an mmap address. */
    s->mmap_cursor = 0;

    pmm_phys_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) { kfree(s); return NULL; }
    s->pml4      = (uint64_t*)phys_to_virt(pml4_phys);
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
        pmm_phys_t pdpt_phys = pmm_alloc_frame();
        if (!pdpt_phys) { pmm_free_frame(pml4_phys); kfree(s); return NULL; }
        uint64_t* kpdpt = table_at((uintptr_t)k0);
        uint64_t* npdpt = (uint64_t*)phys_to_virt(pdpt_phys);
        for (int i = 0; i < 512; i++) npdpt[i] = kpdpt[i];
        /* Point PML4[0] at the private PDPT; keep P+RW, USER widened lazily
         * by walk_to_pt_root when the first user page is mapped. */
        s->pml4[0] = ((uint64_t)pdpt_phys & PAGE_MASK_4K) | PTE_P | PTE_RW;
    }
    return s;
}

/* M34/§3.1 — per-frame COW reference counts, indexed by frame number.
 *
 * §M48 — this table used to be a static array covering only the first 1 GiB of
 * physical memory, on the reasoning that "a frame outside that range is
 * untracked: cow_slot returns NULL and the resolver then always makes a private
 * copy, so untracked frames cost a little memory, never correctness".
 *
 * That reasoning covered the FAULT path but not the other two.  clone_subtree
 * still maps a writable page COW in BOTH parent and child and merely skips the
 * refcount when the slot is NULL — and free_subtree, finding no refcount,
 * frees the frame from EACH space.  A double free: the buddy then handed the
 * same frame to two owners, and the second one's page table silently
 * overwrote the first one's.  What that looked like was one of two freshly
 * spawned processes dying on a not-present code page, on machines with more
 * than 1 GiB of RAM and only after a fork had happened.
 *
 * So the table now covers every frame the PMM manages, sized at boot from the
 * same arena as page_state[].  There is no untracked case left to reason
 * about. */
static uint16_t* g_cow_ref;
static uint32_t  g_cow_nr;

static uint16_t* cow_slot(uintptr_t phys) {
    if (!g_cow_ref) {
        /* First use — after pmm_init, so the frame count is known.  Bootmem
         * memory is not zeroed; a stale nonzero refcount would pin frames
         * forever, so clear it explicitly. */
        g_cow_nr  = pmm_nr_frames;
        g_cow_ref = (uint16_t*)pmm_bootmem_alloc(g_cow_nr * (uint32_t)sizeof(uint16_t));
        if (!g_cow_ref) { g_cow_nr = 0; return NULL; }
        for (uint32_t i = 0; i < g_cow_nr; i++) g_cow_ref[i] = 0;
    }
    uintptr_t fn = phys >> 12;
    return (fn < g_cow_nr) ? &g_cow_ref[fn] : NULL;
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
            pmm_free_frame((pmm_phys_t)((uintptr_t)e & PAGE_MASK_4K));
        } else {
            /* depth 3 = PT: user pages.  Skip VMM_SHARED (borrowed) ones —
             * their owner (e.g. a shm object) frees them.  A COW-shared frame
             * is freed only by its LAST owner (refcount). */
            if (e & VMM_SHARED) continue;
            uintptr_t fphys = (uintptr_t)e & PAGE_MASK_4K;
            if (e & VMM_COW) {
                uint16_t* rc = cow_slot(fphys);
                if (rc && *rc > 1) { (*rc)--; continue; }
                if (rc) *rc = 0;
            }
            pmm_free_frame((pmm_phys_t)fphys);
        }
    }
}

void vmm_space_destroy(struct vmm_space* s) {
    if (!s) return;
    free_subtree(s->pml4, (uint64_t*)pml4, 0);
    pmm_free_frame((pmm_phys_t)s->pml4_phys);
    kfree(s);
}

/* Recursively clone one page-table level for fork() (M34, x86_64) — now with
 * real COPY-ON-WRITE (§3.1 parity with i386; it used to eager-copy every page).
 * Kernel-shared entries (== the kernel table's) and large pages are carried by
 * value; private sub-tables are cloned into fresh frames; at the leaf (depth 3):
 *   - VMM_SHARED (shm/memfd)      → shared verbatim, owner frees it;
 *   - writable, or already COW    → COW: refcount++, and BOTH parent and child
 *                                  map it read-only + VMM_COW.  (Catching the
 *                                  already-COW case matters: a second fork must
 *                                  re-share, not mistake it for read-only code —
 *                                  the same bug that was fixed on i386.)
 *   - read-only (code)            → eager private copy (cheap, rarely large).
 * `va` is the virtual base this table covers, threaded down so the PARENT's
 * newly read-only entries can be invalidated (its CR3 is live during fork).
 * `depth`: 1=PDPT 2=PD 3=PT.  Returns the child table's phys, or 0 on OOM. */
static const int shift_for_depth[4] = { 39, 30, 21, 12 };

static uintptr_t clone_subtree(uint64_t* ptbl, uint64_t* ktbl, int depth,
                               uintptr_t va) {
    pmm_phys_t phys = pmm_alloc_frame();
    if (!phys) return 0;
    uint64_t* ntbl = (uint64_t*)phys_to_virt(phys);
    for (int i = 0; i < 512; i++) {
        uint64_t e = ptbl[i];
        if (!(e & PTE_P))              { ntbl[i] = 0; continue; }
        if (ktbl && e == ktbl[i])      { ntbl[i] = e; continue; }  /* kernel-shared */
        if (depth >= 1 && (e & PTE_PS)){ ntbl[i] = e; continue; }  /* large page */

        uintptr_t entry_va = va + ((uintptr_t)i << shift_for_depth[depth]);

        if (depth < 3) {
            uint64_t* pchild = table_at((uintptr_t)e);
            uint64_t* kchild = (ktbl && (ktbl[i] & PTE_P) && !(ktbl[i] & PTE_PS))
                             ? table_at((uintptr_t)ktbl[i]) : NULL;
            uintptr_t cphys = clone_subtree(pchild, kchild, depth + 1, entry_va);
            if (!cphys) return 0;                 /* OOM — caller unwinds via destroy */
            ntbl[i] = ((uint64_t)cphys & PAGE_MASK_4K) | (e & ~PAGE_MASK_4K);
        } else {
            /* Leaf user page. */
            if (e & VMM_SHARED) { ntbl[i] = e; continue; }
            uintptr_t fphys_old = (uintptr_t)e & PAGE_MASK_4K;

            if ((e & PTE_RW) || (e & VMM_COW)) {
                /* → copy-on-write in BOTH spaces (read-only + VMM_COW). */
                uint16_t* rc = cow_slot(fphys_old);
                if (rc) *rc = (*rc == 0) ? 2 : (uint16_t)(*rc + 1);
                uint64_t cow_e = (e & ~(uint64_t)PTE_RW) | VMM_COW;
                ptbl[i] = cow_e;                  /* parent loses write access  */
                ntbl[i] = cow_e;                  /* child shares it read-only  */
                invlpg(entry_va);                 /* parent's CR3 is live NOW    */
                /* Remote CPUs are handled by ONE whole-space shootdown at the
                 * end of the clone — per page it would be thousands of IPI
                 * round trips for the same end state (§M51). */
                continue;
            }
            /* Read-only (code) → eager private copy. */
            pmm_phys_t fphys = pmm_alloc_frame();
            if (!fphys) return 0;
            const uint8_t* src = (const uint8_t*)phys_to_virt(fphys_old);
            uint8_t* dst = (uint8_t*)phys_to_virt(fphys);
            for (int b = 0; b < 4096; b++) dst[b] = src[b];
            ntbl[i] = ((uint64_t)fphys & PAGE_MASK_4K) | (e & ~PAGE_MASK_4K);
        }
    }
    return phys;
}

/* fork() address-space duplication (x86_64).  Snapshot the kernel PML4, then
 * eager-clone the parent's private PML4[0] subtree (which holds the user region
 * under PDPT[1]).  The child gets its own copy of every private user page. */
struct vmm_space* vmm_space_clone(struct vmm_space* parent) {
    if (!parent) return NULL;
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    /* fork(): the child inherits the parent's mappings, so it must inherit the
     * cursor too — restarting at the region base would re-issue addresses the
     * child already has mapped. */
    s->mmap_cursor = parent->mmap_cursor;
    pmm_phys_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) { kfree(s); return NULL; }
    s->pml4      = (uint64_t*)phys_to_virt(pml4_phys);
    s->pml4_phys = pml4_phys;

    uint64_t* kpml4 = (uint64_t*)pml4;
    for (int i = 0; i < 512; i++) s->pml4[i] = kpml4[i];   /* kernel snapshot */

    uint64_t p0 = parent->pml4[0];
    if (p0 & PTE_P) {
        uint64_t* pdpt_parent = table_at((uintptr_t)p0);
        uint64_t* pdpt_kernel = (kpml4[0] & PTE_P) ? table_at((uintptr_t)kpml4[0]) : NULL;
        uintptr_t cphys = clone_subtree(pdpt_parent, pdpt_kernel, /*depth PDPT*/1, 0);
        if (!cphys) { vmm_space_destroy(s); return NULL; }
        /* Preserve the parent's PML4[0] flags — crucially the US bit, which
         * walk_to_pt_root widened in when user pages were first mapped.  A
         * hardcoded PTE_P|PTE_RW (no US) would make every user access in the
         * child fault (#PF err=5: present, user, protection). */
        s->pml4[0] = ((uint64_t)cphys & PAGE_MASK_4K) | (p0 & ~PAGE_MASK_4K);
    }
    /* §M51 — clone_subtree took write access AWAY from the parent, entry by
     * entry, and the parent may be runnable on another core RIGHT NOW: that is
     * exactly the window between fork and the child's execve.  That core still
     * holds writable entries for the pages we just protected, so its next write
     * does NOT fault — it lands in the frame the child now shares, and the two
     * processes corrupt each other with no fault and no log.  One whole-space
     * shootdown, not one per page: the pages number in the thousands and every
     * remote CPU flushes everything anyway. */
    hal_tlb_shootdown(0, 0);
    return s;
}

/* Resolve a copy-on-write page fault in the CURRENT address space (x86_64 twin
 * of the i386 resolver).  Returns 1 if the fault was a COW page we privatised
 * (the faulting instruction is simply retried), 0 if it is a real fault the
 * caller must handle.  Walks PML4→PDPT→PD→PT of task->mm; every table is
 * reachable through the identity map. */
int vmm_cow_fault(uintptr_t fault_va) {
    struct task* t = task_current();
    if (!t || !t->mm) return 0;
    uint64_t* pml4t = t->mm->pml4;
    if (!pml4t) return 0;

    uint64_t e = pml4t[(fault_va >> 39) & 0x1FF];
    if (!(e & PTE_P)) return 0;
    uint64_t* pdpt = table_at((uintptr_t)e);
    e = pdpt[(fault_va >> 30) & 0x1FF];
    if (!(e & PTE_P) || (e & PTE_PS)) return 0;
    uint64_t* pd = table_at((uintptr_t)e);
    e = pd[(fault_va >> 21) & 0x1FF];
    if (!(e & PTE_P) || (e & PTE_PS)) return 0;
    uint64_t* pt = table_at((uintptr_t)e);
    unsigned pti = (unsigned)((fault_va >> 12) & 0x1FF);
    uint64_t pte = pt[pti];
    if (!(pte & PTE_P) || !(pte & VMM_COW)) return 0;   /* not a COW page */

    uintptr_t old = (uintptr_t)pte & PAGE_MASK_4K;
    uint16_t* rc = cow_slot(old);

    if (rc && *rc <= 1) {
        /* Last tracked sharer — grant write in place, no copy needed. */
        pt[pti] = (pte | PTE_RW) & ~(uint64_t)VMM_COW;
        *rc = 0;
    } else {
        /* Shared (or untracked → always copy, see cow_slot): private copy. */
        pmm_phys_t nf = pmm_alloc_frame();
        if (!nf) return 0;                          /* OOM → treat as real fault */
        const uint8_t* src = (const uint8_t*)phys_to_virt(old);
        uint8_t* dst = (uint8_t*)phys_to_virt(nf);
        for (int b = 0; b < 4096; b++) dst[b] = src[b];
        if (rc) (*rc)--;
        pt[pti] = ((uint64_t)nf & PAGE_MASK_4K)
                | ((((pte & ~PAGE_MASK_4K) | PTE_RW) & ~(uint64_t)VMM_COW));
    }
    /* §M51 — the entry now points at a different frame (or became writable in
     * place); a sibling thread on another core still holds the old read-only
     * one.  Without this it writes through a translation we already replaced. */
    hal_tlb_shootdown(0, fault_va & ~(uintptr_t)0xFFF);
    return 1;
}

int vmm_space_map(struct vmm_space* s, uintptr_t virt, uintptr_t phys,
                  uint32_t flags) {
    if (!s) return vmm_map(virt, phys, flags);
    uint64_t* pt = walk_to_pt_root(s->pml4, virt, /*create*/1, flags);
    if (!pt) return -1;
    /* VMM_SHARED (0x400) rides in PTE bit 10 (OS-available) so
     * free_subtree can skip borrowed frames it doesn't own. */
    /* §M51 — see the i386 twin: only a REMAP over a present entry can weaken
     * or redirect an existing translation, and only that needs every CPU told.
     * A fresh map is free — nothing has it cached. */
    int was_present = (pt[IDX_PT(virt)] & PTE_P) != 0;
    pt[IDX_PT(virt)] = ((uint64_t)phys & PAGE_MASK_4K)
                     | PTE_P | ((uint64_t)flags & (PTE_RW | PTE_US | VMM_SHARED));
    if (was_present) hal_tlb_shootdown(0, virt);
    else             invlpg(virt);
    return 0;
}

void vmm_space_unmap(struct vmm_space* s, uintptr_t virt) {
    if (!s) { vmm_unmap(virt); return; }
    uint64_t* pt = walk_to_pt_root(s->pml4, virt, /*create*/0, 0);
    if (!pt) return;
    pt[IDX_PT(virt)] = 0;
    hal_tlb_shootdown(0, virt);          /* §M51 — present → absent */
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
    hal_tlb_shootdown(0, virt);          /* §M51 — may drop PTE_RW */
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


/* §M48 — the address space's mmap bump cursor.  Policy (where the region
 * starts, how far it may grow) stays in usyscall.c; the SPACE only owns the
 * storage, so every task sharing an mm shares one cursor. */
uintptr_t vmm_space_mmap_cursor(struct vmm_space* s) {
    return s ? s->mmap_cursor : 0;
}
void vmm_space_set_mmap_cursor(struct vmm_space* s, uintptr_t v) {
    if (s) s->mmap_cursor = v;
}
