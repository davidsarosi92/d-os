/* =============================================================================
 * vmm.c — AArch64 per-process virtual memory (M21 Phase L — M25 prerequisite).
 *
 * mmu.c brings up the coarse identity map that turns the MMU on (1 GiB blocks,
 * EL1-only).  This file adds the piece userspace needs and that the userland
 * milestone (M25) will build per-process address spaces on: page-granular,
 * EL0-accessible mappings in their own TTBR0 translation table.
 *
 * Address-space model (mirrors the x86 ports' "kernel mapped in every process"):
 *   - Every process gets its own level-1 table.  Its first four entries are a
 *     COPY of the kernel's identity blocks (mmu_kernel_l1()) — device window +
 *     3 GiB of RAM — so the kernel + peripherals are reachable at EL1 in every
 *     space (needed for the syscall path, which runs at EL1 with the process's
 *     TTBR0 still loaded).  Those blocks are EL1-only, so EL0 cannot touch them.
 *   - User pages live at VA >= 4 GiB (L1 index >= 4), which never collides with
 *     the kernel blocks.  They are mapped 4 KiB-granular through freshly
 *     allocated L2/L3 tables, with AP=01 (EL0+EL1 RW) + PXN (the kernel never
 *     executes user memory) + UXN cleared only for executable pages.
 *   - RAM is identity-mapped, so a physical frame's address doubles as the
 *     kernel VA the page-table walker and this code use to read/write it.
 *
 * Switching TTBR0 to a process's table (aarch64_vmm_switch) is the primitive
 * M25's context_switch will call per task; today the ring-3/EL0 self-test
 * (syscall.c) is the only caller.
 *
 * References: Arm ARM (DDI 0487) D8 — VMSAv8-64 descriptor formats.
 * ============================================================================= */

#include "pmm.h"
#include "hal_api.h"   /* phys_to_virt / kptr_phys — the TTBR1 direct map */
#include "cowref.h"
#include "kmalloc.h"
#include "printf.h"
#include "task.h"   /* §A1 — vmm_cow_fault needs the current task's space */
#include "vma.h"    /* §M89 — reservations live beside the tables */
#include "vmm.h"    /* §M90 — vmm_space_map, VMM_* flags (the trampoline) */
#include <stdint.h>
#include <stddef.h>

/* Portable VMM entry points implemented lower in this file / used by
 * vmm_user_access_ok before their definition. */
uintptr_t vmm_user_base(void);
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write);

uint64_t* mmu_kernel_l1(void);          /* mmu.c — shared kernel L1 table */

/* §M90 (2026-10-01) — the user region is ALL of TTBR0 from 64 KiB up (the
 * lowest pages stay unmapped so a NULL dereference faults, Linux's
 * mmap_min_addr).  It began at 4 GiB while the kernel's Device identity map
 * occupied L1 slots 0..3 in every process. */
#define USER_L1_FIRST 0
#define USER_VA_MIN   0x10000ULL
#define AARCH64_SIGTRAMP_VA 0x7FFFFFF000ULL     /* §M90 — the top page of TTBR0 */
void vmm_map_sigtramp(struct vmm_space* s);
uintptr_t vmm_user_min(void) { return USER_VA_MIN; }

/* ---- descriptor bit fields (stage-1, 4 KiB granule) ------------------------ */
#define PTE_VALID     (1ULL << 0)
#define PTE_TABLE     (1ULL << 1)       /* at L1/L2: points to a next-level table */
#define PTE_PAGE      (1ULL << 1)       /* at L3: a page (bit1 must be 1)          */
#define PTE_ATTR(i)   (((uint64_t)(i)) << 2)   /* MAIR attribute index            */
#define PTE_AP_EL0    (1ULL << 6)       /* AP[1]=1 → EL0 access (RW with AP[2]=0)  */
#define PTE_SH_INNER  (3ULL << 8)       /* inner shareable                         */
#define PTE_AF        (1ULL << 10)      /* Access Flag                             */
#define PTE_PXN       (1ULL << 53)      /* Privileged eXecute Never                */
#define PTE_UXN       (1ULL << 54)      /* Unprivileged eXecute Never              */

#define ATTR_NORMAL   1                 /* MAIR slot 1 = Normal WB (see mmu.c)     */
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000ULL     /* output address bits [47:12]     */

struct vmm_space {
    uint64_t* l1;                       /* level-1 table = TTBR0 root */
    void*     vma;          /* §M89 — the reservation set (vma.c owns it; it
                             * replaced §M48's per-space mmap bump cursor) */
};

/* Allocate a zeroed 4 KiB translation table, reached through the TTBR1
 * direct map (§M86 stage 3, 2026-09-26).  RAM used to be identity-mapped in
 * TTBR0, so a frame's physical address WAS its kernel pointer — true only for
 * RAM below 4 GiB, which is all the Phase-A identity blocks covered.  Every
 * table is therefore addressed as `phys_to_virt(phys)` and every descriptor or
 * TTBR value is built from `kptr_phys(pointer)`: the two directions are now
 * different numbers, and a cast in either direction is a bug that only shows
 * on a machine with RAM above the line. */
static uint64_t* alloc_table(void) {
    pmm_phys_t pa = pmm_alloc_frame();
    if (pa == PMM_ALLOC_FAIL) return NULL;
    uint64_t* t = (uint64_t*)phys_to_virt(pa);
    for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

/* Descend into tbl[idx], allocating a next-level table if absent.
 *
 * **THE BARRIER IS NOT OPTIONAL, AND IT IS ARM-SPECIFIC.**  `alloc_table`
 * zeroes the new frame and then this publishes a descriptor pointing at it —
 * two stores that AArch64's memory model is free to make visible to another
 * observer IN THE OTHER ORDER.  Whoever sees the descriptor first then reads
 * whatever the frame held when the allocator handed it over, and interprets
 * that as translation-table entries: a table pointer built out of stale bytes,
 * followed by a read of an address that may not be memory at all.
 *
 * The observers are real, and there are two of them.  A sibling core sharing
 * this `mm` (a cloned thread) has its own hardware walker; and since §M75 a
 * task manager WALKS A LIVE SPACE IN SOFTWARE while its owner keeps mapping.
 *
 * **THIS BARRIER FIXES A CODE FACT AND NOT A KNOWN FAULT, AND THE DIFFERENCE
 * IS RECORDED DELIBERATELY.**  It was added while chasing an intermittent EL1
 * data abort on this arch, on the theory that a half-published table was being
 * walked.  **That theory is FALSIFIED:** the abort reproduced twice in three
 * runs afterwards, its `FAR_EL1` is the SAME fixed address every time
 * (0x80000000 — not the varying garbage a corrupt table pointer would give),
 * and its `ELR_EL1` lands inside `exc_dispatch`, i.e. in the exception
 * REPORTING path rather than in any table walk.  That fault is somebody
 * else's and is written down as open.
 *
 * The barrier stays because the ordering hazard is real when read on its own
 * terms — but claiming it as the fix would be the §M52 shape: a note that was
 * true about the author's intention and false about the machine.
 *
 * `dsb ishst` — store barrier, inner shareable: cheaper than a full `dsb ish`
 * and exactly the guarantee needed (the zeroing stores land before the store
 * that publishes them). */
static uint64_t* next_table(uint64_t* tbl, uint64_t idx) {
    if (!(tbl[idx] & PTE_VALID)) {
        uint64_t* nt = alloc_table();
        if (!nt) return NULL;
        __asm__ volatile ("dsb ishst" ::: "memory");
        tbl[idx] = kptr_phys(nt) | PTE_VALID | PTE_TABLE;
    }
    return (uint64_t*)phys_to_virt(tbl[idx] & PTE_ADDR_MASK);
}

/* §M46/security — is [va, va+len) fully mapped + EL0-accessible in the ACTIVE
 * TTBR0 space (loaded during a syscall)?  The ARM twin of the x86 checks: walks
 * L1(>>30)→L2(>>21)→L3(>>12), requiring PTE_VALID + PTE_AP_EL0 (AP[1], EL0 can
 * access) at the leaf; want_write also requires AP[2]==0 (bit 7 clear = writable).
 * A leaf can be an L2 2 MiB block or an L3 page.  Tables are identity-reachable. */
static int access_walk(uintptr_t va, uintptr_t len, int want_write) {
    if (len == 0) return 1;
    if (va < USER_VA_MIN)     return 0;
    if (va + len < va)        return 0;
    /* THE TOP OF THE TTBR0 RANGE (§M86 stage 3, 2026-09-26).  The walk below
     * indexes with `(p >> 30) & 0x1FF`, i.e. it looks only at bits 38..30 —
     * so a kernel direct-map address (0xFFFFFF80_xxxxxxxx, TTBR1) folds onto
     * an L1 slot of the USER table and would pass whenever the user happens
     * to have something mapped at the same low bits.  Before the direct map
     * moved up there was nothing above 2^39 to fold; now there is the whole
     * of RAM.  Anything the TTBR0 walk cannot translate is not user memory. */
    if (va + len > (1ULL << 39)) return 0;
    uint64_t ttbr0;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    uint64_t* l1 = (uint64_t*)phys_to_virt(ttbr0 & PTE_ADDR_MASK);
    for (uintptr_t p = va & ~0xFFFUL; p < va + len; p += 0x1000) {
        uint64_t e1 = l1[(p >> 30) & 0x1FF];
        if (!(e1 & PTE_VALID) || !(e1 & PTE_TABLE)) return 0;
        uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
        uint64_t e2 = l2[(p >> 21) & 0x1FF];
        if (!(e2 & PTE_VALID)) return 0;
        uint64_t leaf;
        if (!(e2 & PTE_TABLE)) {                       /* 2 MiB block at L2 */
            leaf = e2;
        } else {
            uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
            leaf = l3[(p >> 12) & 0x1FF];
            if (!(leaf & PTE_VALID)) return 0;
        }
        if (!(leaf & PTE_AP_EL0)) return 0;            /* not EL0-accessible */
        if (want_write && (leaf & (1ULL << 7))) return 0;   /* AP[2]=1 → read-only */
    }
    return 1;
}

/* §M89 — see the x86_64 twin: a demand page not mapped YET is still the
 * program's, so it is brought in before the range is refused. */
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write) {
    if (access_walk(va, len, want_write)) return 1;
    if (va < USER_VA_MIN || va + len < va || va + len > (1ULL << 39)) return 0;
    return vma_prefault(va, len, want_write) && access_walk(va, len, want_write);
}

/* Create a fresh address space: private L1 table with the kernel's identity
 * blocks copied in.  Returns NULL on OOM. */
/* §M90 (2026-10-01) — THE SIGNAL-RETURN TRAMPOLINE, Linux's vDSO
 * __kernel_rt_sigreturn.  On arm64 a program is not obliged to supply
 * sa_restorer: Go never does, and Linux returns its handlers through a
 * trampoline in the vDSO.  Without one, the first SIGURG Go uses to preempt a
 * goroutine killed the process ("no usable stack or restorer").  ONE frame,
 * shared read-only by every space at a fixed address at the top of TTBR0:
 *     mov x8, #139      (rt_sigreturn)
 *     svc #0                                                                 */
static uint64_t g_sigtramp_pa;
void vmm_map_sigtramp(struct vmm_space* s) {
    if (!g_sigtramp_pa) {
        uint64_t pa = pmm_alloc_frame();
        if (!pa) return;
        uint32_t* code = (uint32_t*)phys_to_virt(pa);
        for (int i = 0; i < 1024; i++) code[i] = 0xd4200000u;     /* brk #0 */
        code[0] = 0xd2801168u;                                     /* mov x8, #139 */
        code[1] = 0xd4000001u;                                     /* svc #0       */
        __asm__ volatile ("dc cvau, %0\ndsb ish\nic ivau, %0\ndsb ish\nisb" :: "r"(code) : "memory");
        g_sigtramp_pa = pa;
    }
    vmm_space_map(s, AARCH64_SIGTRAMP_VA, g_sigtramp_pa, VMM_USER | VMM_EXEC | VMM_SHARED);
}

struct vmm_space* aarch64_vmm_create(void) {
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof *s);
    if (!s) return NULL;
    s->vma = NULL;                  /* kmalloc does not zero */
    s->l1 = alloc_table();
    if (!s->l1) { kfree(s); return NULL; }
    vmm_map_sigtramp(s);            /* §M90 — the rt_sigreturn trampoline */
    /* §M90 — NOTHING of the kernel's is copied in any more.  The low 4 GiB used
     * to be the kernel's Device identity map, present in every process — on
     * exactly the addresses a Linux program is linked at (0x200000).  Device
     * memory lives in TTBR1 now (mmu.c, hal_mmio_map), so the whole TTBR0
     * range from USER_VA_MIN up belongs to the program. */
    return s;
}

/* Map [va, va+size) → [pa, pa+size) as EL0-accessible pages (4 KiB granular).
 * `exec` non-zero clears UXN so EL0 may execute (code); otherwise UXN is set
 * (data/stack).  va must be >= 4 GiB so it never lands on a kernel block.
 * Returns 0 on success, -1 on OOM. */
int aarch64_vmm_map_user(struct vmm_space* s, uint64_t va, uint64_t pa,
                         uint64_t size, int exec) {
    for (uint64_t off = 0; off < size; off += 4096) {
        uint64_t v = va + off, p = pa + off;
        uint64_t* l2 = next_table(s->l1, (v >> 30) & 0x1FF);
        if (!l2) return -1;
        uint64_t* l3 = next_table(l2, (v >> 21) & 0x1FF);
        if (!l3) return -1;
        l3[(v >> 12) & 0x1FF] =
            (p & PTE_ADDR_MASK) | PTE_VALID | PTE_PAGE | PTE_ATTR(ATTR_NORMAL)
            | PTE_AP_EL0 | PTE_SH_INNER | PTE_AF | PTE_PXN
            | (exec ? 0 : PTE_UXN);
    }
    __asm__ volatile ("dsb ish\nisb" ::: "memory");
    return 0;
}

/* Make `s` the active low-half (TTBR0) address space on THIS CPU. */
void aarch64_vmm_switch(struct vmm_space* s) {
    __asm__ volatile (
        "msr ttbr0_el1, %0\n"
        "dsb ish\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(kptr_phys(s->l1)) : "memory");
}

/* Restore the shared kernel identity map as the active TTBR0 (used after a
 * user program returns, and by any kernel-only task). */
void aarch64_vmm_kernel_switch(void) {
    __asm__ volatile (
        "msr ttbr0_el1, %0\n"
        "dsb ish\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(kptr_phys(mmu_kernel_l1())) : "memory");
}

/* The `vmm` shell command's status dump.  The x86 vmm.c prints page-directory
 * details; aarch64's translation is set up in mmu.c (coarse identity) + this
 * file (per-process EL0 spaces), so report that shape.  Keeps shell.c portable
 * (it just calls vmm_print_status). */
void vmm_print_status(void) {
    uint64_t tcr;
    __asm__ volatile ("mrs %0, tcr_el1" : "=r"(tcr));
    kprintf("aarch64 MMU: 4 KiB granule, 39-bit VA; RAM = TTBR1 direct map @ %p (%s), "
            "devices = TTBR1 windows (hal_mmio_map); per-process EL0 spaces from "
            "64 KiB\n", (void*)KERNEL_DIRECT_MAP_BASE,
            (tcr & (1ULL << 23)) ? "WALKS DISABLED" : "on");
}

/* (vmm_map_4mib is gone on this arch, §M90: every driver reaches its registers
 * through hal_mmio_map, which knows device memory lives in the kernel half.) */

/* ===========================================================================
 * Portable per-process address-space API (M25 stage 1).
 *
 * aarch64 already had per-process EL0 spaces (aarch64_vmm_create / _map_user
 * / _switch); this exposes them under the arch-neutral vmm.h names so core
 * code (task.c scheduler, shell.c self-test, the coming ELF loader) is
 * identical across arches.  A space is a private TTBR0 L1 table sharing the
 * kernel's low-4-GiB identity blocks (l1[0..3]) and owning the user region
 * at VA >= 4 GiB (l1[4..]).
 * =========================================================================== */

/* The flag vocabulary comes from the header BOTH sides share.  It used to be
 * three hand-copied `#define`s here, because this file cannot include vmm.h —
 * its `vmm_map_4mib` signature intentionally diverges from the declared one.
 * §M75 needed two more of them (VMM_COW, VMM_USER) for `vmm_space_walk`'s
 * translation, and a fourth and fifth copy is how a bit comes to mean one
 * thing on x86 and another here, silently, on the arch nobody is running. */
#include "vmm_flags.h"
/* Descriptor software-use bit (IGNORED by the hardware walk) marking a
 * BORROWED page — vmm_space_destroy leaves those frames for their owner. */
#define PTE_SW_SHARED   (1ULL << 55)
/* §A1 — a second software bit: this page is COPY-ON-WRITE.  Both the parent
 * and the child of a fork() carry it, both mapped read-only; the first writer
 * takes a permission fault and vmm_cow_fault privatises the page.  Bits 55-58
 * are reserved for software use in a stage-1 descriptor, so the hardware walk
 * ignores it. */
#define PTE_SW_COW      (1ULL << 56)

/* AP[2]: 0 = read/write, 1 = read-only.  Defined here (rather than beside
 * vmm_space_protect further down) because the COW paths above it need it. */
#define PTE_AP_RO_BIT   (1ULL << 7)

/* Walk the KERNEL table (mmu_kernel_l1) for `va`; return phys or 0.  Handles
 * 1 GiB / 2 MiB block descriptors and 4 KiB pages.  Used by the isolation
 * self-test to confirm a user VA is NOT visible in the kernel space. */
uintptr_t vmm_translate(uintptr_t va) {
    uint64_t* l1 = mmu_kernel_l1();
    uint64_t e1 = l1[(va >> 30) & 0x1FF];
    if (!(e1 & PTE_VALID)) return 0;
    if (!(e1 & PTE_TABLE))                          /* 1 GiB block */
        return (uintptr_t)((e1 & PTE_ADDR_MASK & ~0x3FFFFFFFULL) | (va & 0x3FFFFFFF));
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!(e2 & PTE_VALID)) return 0;
    if (!(e2 & PTE_TABLE))                          /* 2 MiB block */
        return (uintptr_t)((e2 & PTE_ADDR_MASK & ~0x1FFFFFULL) | (va & 0x1FFFFF));
    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
    uint64_t e3 = l3[(va >> 12) & 0x1FF];
    if (!(e3 & PTE_VALID)) return 0;
    return (uintptr_t)((e3 & PTE_ADDR_MASK) | (va & 0xFFF));
}

uintptr_t vmm_user_base(void) { return 0x100000000ULL; }   /* 4 GiB */

struct vmm_space* vmm_space_create(void) { return aarch64_vmm_create(); }

/* ---------------------------------------------------------------------------
 * Copy-on-write fork (§A1, the AArch64 twin of the two x86 vmm.c files).
 *
 * A frame shared by a fork can be owned by several address spaces at once, so
 * "free it when this space dies" stops being true and a reference count has to
 * decide.  The table is sized from `pmm_nr_frames`, i.e. from the RAM the
 * firmware actually reported — the §M48 lesson, learned the expensive way on
 * x86_64: a FIXED window meant any frame above it was untracked, and an
 * untracked shared frame is a DOUBLE FREE the moment both spaces exit.
 * Allocated from the boot arena because it must not itself be a buddy
 * allocation that COW might later have to reason about.
 * ------------------------------------------------------------------------- */

void vmm_space_destroy(struct vmm_space* s);   /* defined below; clone unwinds with it */

static uint16_t* g_cow_ref = NULL;
static uint32_t  g_cow_nr  = 0;

/* Refcount slot for a physical frame, or NULL if untracked (allocation failed,
 * or the frame sits past what the PMM knows about).  An untracked frame is
 * always COPIED rather than shared-in-place — the conservative direction: it
 * wastes a page, where guessing the other way loses data. */
static uint16_t* cow_slot(uintptr_t phys) {
    static spinlock_t cow_build_lock = SPINLOCK_INIT;
    if (!cow_table_get(&g_cow_ref, &g_cow_nr, &cow_build_lock)) return NULL;
    uintptr_t fn = phys >> 12;
    return (fn >= pmm_pfn_base && fn < g_cow_nr) ? &g_cow_ref[fn] : NULL;
}

/* Drop a reference to a COW frame; free it when the last holder lets go.
 * Called from the teardown path for every page carrying PTE_SW_COW. */
static void cow_release(uintptr_t phys) {
    uint16_t* rc = cow_slot(phys);
    if (rc && !cow_ref_drop(rc)) return;      /* someone else still holds it */
    /* Last holder (rc 0 or 1), or an untracked frame.  `*rc > 0` here instead of
     * `> 1` would decrement the final reference and return WITHOUT freeing —
     * a silent leak of every page a fork ever shared.  Matches the x86_64 twin. */
    pmm_free_frame((pmm_phys_t)phys);
}

/* Mark one leaf read-only + COW in place, and account the extra reference.
 * Returns the descriptor to store in the CHILD (identical to the parent's). */
static uint64_t cow_share_leaf(uint64_t* parent_slot) {
    uint64_t pte = *parent_slot;
    uintptr_t phys = (uintptr_t)(pte & PTE_ADDR_MASK);

    /* A borrowed page (memfd / shared mapping) must stay shared and WRITABLE —
     * that is the whole point of it.  Privatising it on first write would give
     * the child a copy nobody else can see. */
    if (pte & PTE_SW_SHARED) return pte;

    uint16_t* rc = cow_slot(phys);
    if (rc) cow_ref_share(rc);   /* 0 (one untracked holder) -> 2, n -> n+1 */
    /* Read-only in BOTH spaces: the parent must fault on its own next write
     * too, or it would silently edit the child's memory. */
    uint64_t shared = (pte | PTE_AP_RO_BIT | PTE_SW_COW);
    *parent_slot = shared;
    return shared;
}

struct vmm_space* vmm_space_clone(struct vmm_space* parent) {
    if (!parent) return NULL;
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof *s);
    if (!s) return NULL;
    /* The child inherits the parent's reservations as well as its mappings
     * (vma_clone at the end) — otherwise its allocator would re-issue ranges
     * it already has mapped. */
    s->vma = NULL;
    s->l1 = alloc_table();
    if (!s->l1) { kfree(s); return NULL; }

    for (int i1 = USER_L1_FIRST; i1 < 512; i1++) {      /* §M90: all of it is user */
        uint64_t e1 = parent->l1[i1];
        if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) continue;
        uint64_t* pl2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
        uint64_t* cl2 = next_table(s->l1, (uint64_t)i1);
        if (!cl2) { vmm_space_destroy(s); return NULL; }

        for (int i2 = 0; i2 < 512; i2++) {
            uint64_t e2 = pl2[i2];
            if (!(e2 & PTE_VALID)) continue;
            if (!(e2 & PTE_TABLE)) {
                /* A 2 MiB block leaf.  Nothing maps user memory this way today
                 * (aarch64_vmm_map_user is 4 KiB granular); copying the
                 * descriptor would silently share 2 MiB writably, so refuse
                 * loudly instead of guessing. */
                kprintf("vmm: fork: unexpected 2MiB user block at l1[%d] l2[%d]\n", i1, i2);
                vmm_space_destroy(s);
                return NULL;
            }
            uint64_t* pl3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
            uint64_t* cl3 = next_table(cl2, (uint64_t)i2);
            if (!cl3) { vmm_space_destroy(s); return NULL; }
            for (int i3 = 0; i3 < 512; i3++)
                if (pl3[i3] & PTE_VALID)
                    cl3[i3] = cow_share_leaf(&pl3[i3]);
        }
    }
    /* The parent's own entries just became read-only — its TLB still holds the
     * writable versions, so without this its next write would NOT fault and it
     * would scribble on the child's pages.
     *
     * `vmalle1IS` (inner-shareable), NOT `vmalle1`: the plain form invalidates
     * only THIS CPU.  A sibling thread of the parent running on another core
     * would keep its stale writable entry and write straight through the COW —
     * the failure would be silent data corruption between parent and child,
     * visible only under -smp. */
    __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
    vma_clone(parent, s);           /* §M89 — the child inherits the reservations */
    return s;
}

/* Resolve a write fault on a COW page in the CURRENT address space.  Returns 1
 * if it was ours to handle (retry the instruction), 0 if it is a real fault. */
int vmm_cow_fault(uintptr_t fault_va) {
    struct task* t = task_current();
    if (!t || !t->mm) return 0;
    uint64_t* l1 = t->mm->l1;
    if (!l1) return 0;

    uint64_t e1 = l1[(fault_va >> 30) & 0x1FF];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return 0;
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(fault_va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return 0;
    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
    unsigned i3 = (unsigned)((fault_va >> 12) & 0x1FF);
    uint64_t pte = l3[i3];
    if (!(pte & PTE_VALID) || !(pte & PTE_SW_COW)) return 0;   /* not a COW page */

    uintptr_t old = (uintptr_t)(pte & PTE_ADDR_MASK);
    uint16_t* rc  = cow_slot(old);

    if (rc && cow_ref_sole(rc)) {
        /* Last tracked sharer — grant write in place, no copy needed. */
        l3[i3] = (pte & ~PTE_AP_RO_BIT) & ~PTE_SW_COW;
        __atomic_store_n(rc, 0, __ATOMIC_RELEASE);
    } else {
        pmm_phys_t nf = pmm_alloc_frame_user();         /* §M72 — user memory */
        if (nf == PMM_ALLOC_FAIL) return 0;             /* OOM → a real fault */
        const uint8_t* src = (const uint8_t*)phys_to_virt(old);
        uint8_t* dst = (uint8_t*)phys_to_virt(nf);
        for (int b = 0; b < 4096; b++) dst[b] = src[b];
        l3[i3] = (((uint64_t)nf & PTE_ADDR_MASK) | (pte & ~PTE_ADDR_MASK))
                 & ~PTE_AP_RO_BIT & ~PTE_SW_COW;
        /* Give up our share only after the copy (cowref.h). */
        if (rc && cow_ref_put_copy(rc)) pmm_free_frame((pmm_phys_t)old);
    }
    /* Inner-shareable: another core may share this mm (threads) and still hold
     * the read-only entry we just replaced.  See vmm_space_clone. */
    __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
    return 1;
}

/* Free the user-region tables (l1[4..]) + their frames; kernel-shared
 * blocks (l1[0..3]) are left alone. */
static void free_l2_subtree(uint64_t* l2) {
    for (int i = 0; i < 512; i++) {
        uint64_t e = l2[i];
        if ((e & PTE_VALID) && (e & PTE_TABLE)) {
            uint64_t* l3 = (uint64_t*)phys_to_virt(e & PTE_ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                uint64_t pte = l3[j];
                if (!(pte & PTE_VALID) && (pte & VMM_SWPE_MARK)) {   /* §M72 */
                    swap_slot_release((uint32_t)(pte >> 12));
                    continue;
                }
                if (!(pte & PTE_VALID) || (pte & PTE_SW_SHARED)) continue;
                uintptr_t pa = (uintptr_t)(pte & PTE_ADDR_MASK);
                /* §A1 — a COW frame may still belong to the other side of a
                 * fork.  Freeing it outright here is the double free §M48
                 * found on x86_64. */
                if (pte & PTE_SW_COW) cow_release(pa);
                else                  pmm_free_frame((pmm_phys_t)pa);
            }
            /* NOT (uint32_t): a physical address is 64-bit wide on this arch,
             * and truncating it frees a DIFFERENT frame. */
            pmm_free_frame((pmm_phys_t)(e & PTE_ADDR_MASK));            /* L3 table */
        }
    }
}
void vmm_space_destroy(struct vmm_space* s) {
    if (!s) return;
    vma_destroy(s);                 /* §M89 — before the tables: it may unref files */
    for (int i = USER_L1_FIRST; i < 512; i++) {                 /* user region = VA >= 4 GiB */
        uint64_t e = s->l1[i];
        if ((e & PTE_VALID) && (e & PTE_TABLE)) {
            uint64_t* l2 = (uint64_t*)phys_to_virt(e & PTE_ADDR_MASK);
            free_l2_subtree(l2);
            pmm_free_frame((pmm_phys_t)(e & PTE_ADDR_MASK));           /* L2 table */
        }
    }
    pmm_free_frame((pmm_phys_t)kptr_phys(s->l1));                      /* L1 table */
    kfree(s);
}

int vmm_space_map(struct vmm_space* s, uintptr_t va, uintptr_t pa, uint32_t flags) {
    if (!s) return -1;                              /* kernel space not user-mappable */
    int rc = aarch64_vmm_map_user(s, va, pa, 4096, (flags & VMM_EXEC) ? 1 : 0);
    /* THE WRITE BIT AND THE COW MARK ARE HONOURED (2026-09-27).  This mapped
     * every user page WRITABLE whatever it was asked, and dropped VMM_COW — so
     * on ARM a read-only mapping was never read-only, and a COW mapping made
     * through this call (§M74's page cache: one frame shared by every program
     * that maps the file) would have let a program write straight into
     * everybody's copy.  Fork's clone writes its entries directly, which is
     * why nothing had noticed. */
    if (rc == 0 && (flags & (VMM_SHARED | VMM_COW) || !(flags & VMM_WRITABLE))) {
        uint64_t e1 = s->l1[(va >> 30) & 0x1FF];
        uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
        uint64_t e2 = l2[(va >> 21) & 0x1FF];
        uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
        uint64_t e = l3[(va >> 12) & 0x1FF];
        if (flags & VMM_SHARED) e |= PTE_SW_SHARED;           /* borrowed frame */
        if (flags & VMM_COW)    e |= PTE_SW_COW;
        if (!(flags & VMM_WRITABLE) || (flags & VMM_COW)) e |= PTE_AP_RO_BIT;
        l3[(va >> 12) & 0x1FF] = e;
        __asm__ volatile ("dsb ishst" ::: "memory");
    }
    return rc;
}

void vmm_space_unmap(struct vmm_space* s, uintptr_t va) {
    if (!s) return;
    uint64_t e1 = s->l1[(va >> 30) & 0x1FF];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return;
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return;
    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
    uint64_t e3 = l3[(va >> 12) & 0x1FF];
    if (!e3) return;
    if ((e3 & PTE_VALID) ? !(e3 & PTE_AP_EL0) : !(e3 & VMM_SWPE_MARK)) return;  /* user pages only */
    l3[(va >> 12) & 0x1FF] = 0;
    /* INNER-SHAREABLE (2026-09-27, found writing §M72's eviction): the local
     * `tlbi vmalle1` left a sibling core — another thread of the same process
     * — holding the translation after munmap.  §M51 moved the COW paths to
     * `vmalle1is` and this one and mprotect were missed. */
    __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
    /* §M74 — release what the entry held, as teardown does (see the i386
     * twin: a MAP_FIXED overlay used to leak the frame it covered). */
    if (!(e3 & PTE_VALID)) {
        if (e3 & VMM_SWPE_MARK) swap_slot_release((uint32_t)(e3 >> 12));
        return;
    }
    if (e3 & PTE_SW_SHARED) return;
    uint64_t pa = e3 & PTE_ADDR_MASK;
    if (e3 & PTE_SW_COW) cow_release(pa);
    else                 pmm_free_frame((pmm_phys_t)pa);
}

void vmm_frame_share(uint64_t phys) {
    uint16_t* rc = cow_slot((uintptr_t)phys);
    if (rc) cow_ref_share(rc);
}
int vmm_frame_unshare(uint64_t phys) {
    uint16_t* rc = cow_slot((uintptr_t)phys);
    return rc ? cow_ref_drop(rc) : 1;
}

/* Change the permissions of an already-mapped user page (the arch half of
 * mprotect; §M37 needs it so ld.so can flip a relocated segment back to
 * read-only).  AArch64 encodes write permission in AP[2] (bit 7): 0 = RW,
 * 1 = read-only; execute permission is the UXN bit.  Returns 0 on success, -1
 * if the page is not mapped.  (This was simply missing on aarch64, so the
 * portable core did not link here — the same class of gap as the emergency
 * serial sink.) */
#define PTE_AP_RO   (1ULL << 7)

int vmm_space_protect(struct vmm_space* s, uintptr_t va, uint32_t flags) {
    uint64_t* l1 = s ? s->l1 : (uint64_t*)(uintptr_t)mmu_kernel_l1();
    if (!l1) return -1;
    uint64_t e1 = l1[(va >> 30) & 0x1FF];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return -1;
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return -1;
    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
    unsigned i = (unsigned)((va >> 12) & 0x1FF);
    uint64_t e3 = l3[i];
    if (!(e3 & PTE_VALID)) return -1;

    /* COW pages stay read-only whatever mprotect asks (see the i386 twin):
     * write access comes only through vmm_cow_fault, which copies first. */
    if ((flags & VMM_WRITABLE) && !(e3 & PTE_SW_COW)) e3 &= ~PTE_AP_RO;  /* writable  */
    else                                              e3 |=  PTE_AP_RO;  /* read-only */
    if (flags & VMM_EXEC) e3 &= ~PTE_UXN;        /* EL0-executable */
    else                      e3 |=  PTE_UXN;
    /* §M89 — VMM_USER is honoured: without it the page is EL1-only, which is
     * how PROT_NONE keeps a page (and its contents) while making every EL0
     * access fault.  It used to be ignored, so PROT_NONE read as readable. */
    if (flags & VMM_USER) e3 |=  PTE_AP_EL0;
    else                  e3 &= ~PTE_AP_EL0;
    l3[i] = e3;
    __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");   /* see unmap */
    return 0;
}

/* §M72 stage 3 — see vmm_flags.h.  An INVALID descriptor (bit 0 clear) has
 * all other bits free to software, so the same encoding as x86 fits. */
static uint64_t* user_l3_of(struct vmm_space* s, uintptr_t va) {
    uint64_t e1 = s->l1[(va >> 30) & 0x1FF];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return NULL;
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return NULL;
    return (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
}

int vmm_space_swap_out(struct vmm_space* s, uintptr_t va, uint32_t slot,
                       uint64_t* old_raw, uint64_t* old_phys, uint32_t* old_flags, int flush) {
    if (!s || va < (4ull << 30) || (va >> 39)) return -1;
    uint64_t* l3 = user_l3_of(s, va);
    if (!l3) return -1;
    unsigned i = (unsigned)((va >> 12) & 0x1FF);
    uint64_t cur = l3[i];
    if (!(cur & PTE_VALID) || !(cur & PTE_AP_EL0)) return -1;
    uint64_t mark = ((uint64_t)slot << 12) | VMM_SWPE_MARK |
                    ((cur & PTE_AP_RO_BIT) ? 0 : VMM_SWPE_W) | ((cur & PTE_UXN) ? 0 : VMM_SWPE_X);
    uint64_t was = __atomic_exchange_n(&l3[i], mark, __ATOMIC_ACQ_REL);
    if (!(was & PTE_VALID)) { __atomic_store_n(&l3[i], was, __ATOMIC_RELEASE); return -1; }
    if (flush) __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
    uint32_t fl = VMM_USER;
    if (was & PTE_SW_SHARED)   fl |= VMM_SHARED;
    if (was & PTE_SW_COW)      fl |= VMM_COW;
    if (!(was & PTE_AP_RO_BIT))fl |= VMM_WRITABLE;
    if (!(was & PTE_UXN))      fl |= VMM_EXEC;
    *old_raw = was;
    *old_phys = was & PTE_ADDR_MASK;
    *old_flags = fl;
    return 0;
}
void vmm_space_swap_undo(struct vmm_space* s, uintptr_t va, uint64_t raw) {
    uint64_t* l3 = user_l3_of(s, va);
    if (!l3) return;
    __atomic_store_n(&l3[(va >> 12) & 0x1FF], raw, __ATOMIC_RELEASE);
    __asm__ volatile ("dsb ishst\nisb" ::: "memory");
}

int vmm_space_swapped_entry(struct vmm_space* s, uintptr_t va, uint32_t* slot, uint32_t* flags) {
    if (!s || va < USER_VA_MIN || (va >> 39)) return -1;
    uint64_t* l3 = user_l3_of(s, va);
    if (!l3) return -1;
    uint64_t e = l3[(va >> 12) & 0x1FF];
    if ((e & PTE_VALID) || !(e & VMM_SWPE_MARK)) return -1;
    *slot = (uint32_t)(e >> 12);
    *flags = VMM_USER | ((e & VMM_SWPE_W) ? VMM_WRITABLE : 0) | ((e & VMM_SWPE_X) ? VMM_EXEC : 0);
    return 0;
}
int vmm_space_mark_swapped(struct vmm_space* s, uintptr_t va, uint32_t slot, uint32_t flags) {
    if (!s || va < (4ull << 30)) return -1;         /* user region only */
    uint64_t* l3 = user_l3_of(s, va);
    if (!l3) return -1;
    unsigned i = (unsigned)((va >> 12) & 0x1FF);
    if (!(l3[i] & PTE_VALID)) return -1;
    /* §M74 — atomic: the owner may be running, and vmm_af_fault ORs into
     * live entries from another CPU (see the x86 twins). */
    __atomic_exchange_n(&l3[i], ((uint64_t)slot << 12) | VMM_SWPE_MARK |
                        ((flags & VMM_WRITABLE) ? VMM_SWPE_W : 0) |
                        ((flags & VMM_EXEC) ? VMM_SWPE_X : 0), __ATOMIC_ACQ_REL);
    __asm__ volatile ("dsb ish\ntlbi vmalle1is\ndsb ish\nisb" ::: "memory");
    return 0;
}
void vmm_space_walk_swapped(struct vmm_space* s, vmm_swapped_fn cb, void* ctx) {
    if (!s || !cb) return;
    for (uint64_t i = USER_L1_FIRST; i < 512; i++) {
        uint64_t e1 = s->l1[i];
        if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) continue;
        uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
        for (uint64_t j = 0; j < 512; j++) {
            uint64_t e2 = l2[j];
            if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) continue;
            uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
            for (uint64_t k = 0; k < 512; k++) {
                uint64_t e3 = l3[k];
                if ((e3 & PTE_VALID) || !(e3 & VMM_SWPE_MARK)) continue;
                uint32_t fl = VMM_USER | ((e3 & VMM_SWPE_W) ? VMM_WRITABLE : 0) |
                              ((e3 & VMM_SWPE_X) ? VMM_EXEC : 0);
                cb(ctx, (uintptr_t)((i << 30) | (j << 21) | (k << 12)),
                   (uint32_t)(e3 >> 12), fl);
            }
        }
    }
}

/* §M75 — enumerate every present page in this space's PRIVATE region.
 *
 * **THIS IS THE BACKEND THE PORTABLE-FLAGS RULE EXISTS FOR.**  AArch64 keeps
 * "borrowed" in bit 55 and "COW" in bit 56, where x86 uses 0x400 and 0x800; a
 * walker that handed the raw descriptor to the shared policy in
 * vmm_account.c would test bits that mean nothing here, report every page as
 * private, and be wrong ONLY on this architecture — which is exactly the
 * failure mode that made the accounting portable in the first place.  So the
 * descriptor is translated into the VMM_* vocabulary before the callback.
 *
 * The traversal mirrors `vmm_space_destroy` / `free_l2_subtree`: the user
 * region is l1[4..] (VA >= 4 GiB; l1[0..3] are the shared kernel identity
 * blocks), and only TABLE entries are descended — the same restriction
 * teardown applies, because `aarch64_vmm_map_user` only ever creates 4 KiB L3
 * pages down here. */
void vmm_space_walk(struct vmm_space* s, vmm_walk_fn cb, void* ctx) {
    if (!s || !cb) return;

    for (uint64_t i = USER_L1_FIRST; i < 512; i++) {                /* user region only   */
        uint64_t e1 = s->l1[i];
        if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) continue;
        uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);

        for (uint64_t j = 0; j < 512; j++) {
            uint64_t e2 = l2[j];
            if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) continue;
            uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);

            for (uint64_t k = 0; k < 512; k++) {
                uint64_t e3 = l3[k];
                if (!(e3 & PTE_VALID)) continue;

                uint32_t flags = 0;
                if (e3 & PTE_SW_SHARED)   flags |= VMM_SHARED;
                if (e3 & PTE_SW_COW)      flags |= VMM_COW;
                if (e3 & PTE_AP_EL0)      flags |= VMM_USER;
                if (!(e3 & PTE_AP_RO_BIT))flags |= VMM_WRITABLE;
                if (!(e3 & PTE_UXN))      flags |= VMM_EXEC;
                if (e3 & PTE_AF)          flags |= VMM_ACCESSED;   /* §M74 */

                cb(ctx, (uintptr_t)((i << 30) | (j << 21) | (k << 12)),
                        (uintptr_t)(e3 & PTE_ADDR_MASK), flags);
            }
        }
    }
}

/* §M74 — see vmm_flags.h.  ARM's Access Flag is the accessed bit, and with no
 * hardware management (FEAT_HAFDBS not used here) a CLEARED flag makes the
 * next access take an access-flag fault, which vmm_af_fault answers by setting
 * it again — so "was it touched since the sweep" is exactly "did it fault". */
uint32_t vmm_space_age(struct vmm_space* s, vmm_walk_fn cb, void* ctx) {
    if (!s) return 0;
    uint32_t cleared = 0;
    for (uint64_t i = USER_L1_FIRST; i < 512; i++) {
        uint64_t e1 = s->l1[i];
        if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) continue;
        uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
        for (uint64_t j = 0; j < 512; j++) {
            uint64_t e2 = l2[j];
            if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) continue;
            uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
            for (uint64_t k = 0; k < 512; k++) {
                if (!(l3[k] & PTE_VALID)) continue;
                uint64_t e3 = __atomic_fetch_and(&l3[k], ~PTE_AF, __ATOMIC_RELAXED);
                if (e3 & PTE_AF) cleared++;
                if (!cb) continue;
                uint32_t flags = 0;
                if (e3 & PTE_SW_SHARED)   flags |= VMM_SHARED;
                if (e3 & PTE_SW_COW)      flags |= VMM_COW;
                if (e3 & PTE_AP_EL0)      flags |= VMM_USER;
                if (!(e3 & PTE_AP_RO_BIT))flags |= VMM_WRITABLE;
                if (!(e3 & PTE_UXN))      flags |= VMM_EXEC;
                if (e3 & PTE_AF)          flags |= VMM_ACCESSED;
                cb(ctx, (uintptr_t)((i << 30) | (j << 21) | (k << 12)),
                        (uintptr_t)(e3 & PTE_ADDR_MASK), flags);
            }
        }
    }
    return cleared;
}
void vmm_age_flush(void) {
    __asm__ volatile ("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
}

/* §M74 — an access-flag fault: the sweep cleared AF on a valid page and this
 * is its next access.  Set it again and retry.  Returns 1 if handled (the
 * address is a valid page in the CURRENT space's user region), 0 otherwise —
 * in which case the fault is whatever else it is.  Setting AF strengthens the
 * entry, so no TLB maintenance is needed beyond making the store visible. */
static volatile uint64_t g_af_faults;
uint64_t vmm_af_fault_count(void) { return g_af_faults; }
int vmm_af_fault(uintptr_t va) {
    struct task* t = task_current();
    if (!t || !t->mm) return 0;
    struct vmm_space* s = t->mm;
    uint64_t i = (va >> 30) & 511, j = (va >> 21) & 511, k = (va >> 12) & 511;
    if (va < USER_VA_MIN || (va >> 39)) return 0;
    uint64_t e1 = s->l1[i];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return 0;
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[j];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return 0;
    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
    if (!(l3[k] & PTE_VALID)) return 0;
    __atomic_fetch_or(&l3[k], PTE_AF, __ATOMIC_RELAXED);
    __asm__ volatile ("dsb ishst\n isb" ::: "memory");
    __atomic_add_fetch(&g_af_faults, 1, __ATOMIC_RELAXED);
    return 1;
}

/* §M75 — see vmm.h: a QUERY, so it must not build the table.  `cow_slot`
 * creates it on first use out of bootmem; calling that from a task manager
 * would make reading the memory column allocate memory. */
uint32_t vmm_frame_share_count(uintptr_t phys) {
    if (!g_cow_ref) return 0;
    uintptr_t fn = phys >> 12;
    return (fn >= pmm_pfn_base && fn < g_cow_nr) ? (uint32_t)g_cow_ref[fn] : 0;
}

uintptr_t vmm_space_pd_phys(struct vmm_space* s) {
    return (uintptr_t)kptr_phys(s ? s->l1 : mmu_kernel_l1());
}

void vmm_space_switch(struct vmm_space* s) {
    uint64_t target = kptr_phys(s ? s->l1 : mmu_kernel_l1());
    uint64_t cur;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(cur));
    /* Skip the (expensive) TTBR0 reload + full TLBI when the space is
     * already active — kernel-thread → kernel-thread switches cost nothing. */
    if ((cur & PTE_ADDR_MASK) == (target & PTE_ADDR_MASK)) return;
    __asm__ volatile (
        "msr ttbr0_el1, %0\n"
        "dsb ish\ntlbi vmalle1\ndsb ish\nisb\n"
        :: "r"(target) : "memory");
}


/* §M86 — UXN has been set on every non-VMM_EXEC user page since §M25 (above);
 * the architecture has no mode without it. */
int vmm_nx_active(void) { return 1; }

/* ---------------------------------------------------------------------------
 * §M89 — the primitives vma.c needs (contract in vma.h).
 * ------------------------------------------------------------------------- */
void* vmm_space_vma(struct vmm_space* s) { return s ? s->vma : NULL; }
void  vmm_space_set_vma(struct vmm_space* s, void* v) { if (s) s->vma = v; }

/* TTBR0 covers 39 bits (T0SZ = 25, mmu.c). */
uintptr_t vmm_user_limit(void) { return (uintptr_t)1 << 39; }

int vmm_space_probe(struct vmm_space* s, uintptr_t va) {
    if (!s || va >= ((uintptr_t)1 << 39) || va < USER_VA_MIN) return 0;
    uint64_t e1 = s->l1[(va >> 30) & 0x1FF];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return 0;
    uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return 0;
    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
    uint64_t e3 = l3[(va >> 12) & 0x1FF];
    if (e3 & PTE_VALID) return 1;
    if (e3 & VMM_SWPE_MARK) return 2;
    return 0;
}

/* L1 slots 0-3 (the low 4 GiB) are the kernel's device identity map, shared
 * by value into every space; everything above is private to the space. */
int vmm_space_range_state(struct vmm_space* s, uintptr_t a, uintptr_t b) {
    if (!s) return VMA_RS_KERNEL;
    if (b > ((uintptr_t)1 << 39) || a < USER_VA_MIN) return VMA_RS_KERNEL;   /* §M90 */
    int st = 0;
    while (a < b) {
        uint64_t e1 = s->l1[(a >> 30) & 0x1FF];
        uintptr_t next = (a & ~(((uintptr_t)1 << 30) - 1)) + ((uintptr_t)1 << 30);
        if (e1 & PTE_VALID) {
            if (!(e1 & PTE_TABLE)) return st | VMA_RS_KERNEL;
            uint64_t* l2 = (uint64_t*)phys_to_virt(e1 & PTE_ADDR_MASK);
            while (a < b && a < next) {
                uint64_t e2 = l2[(a >> 21) & 0x1FF];
                uintptr_t n2 = (a & ~(((uintptr_t)1 << 21) - 1)) + ((uintptr_t)1 << 21);
                if (e2 & PTE_VALID) {
                    if (!(e2 & PTE_TABLE)) return st | VMA_RS_KERNEL;
                    uint64_t* l3 = (uint64_t*)phys_to_virt(e2 & PTE_ADDR_MASK);
                    for (uintptr_t p = a; p < b && p < n2; p += 0x1000) {
                        uint64_t e3 = l3[(p >> 12) & 0x1FF];
                        if ((e3 & PTE_VALID) || (e3 & VMM_SWPE_MARK)) return st | VMA_RS_USER;
                    }
                }
                a = n2;
            }
            continue;
        }
        a = next;
    }
    return st;
}
