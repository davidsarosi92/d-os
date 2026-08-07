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
#include "kmalloc.h"
#include "printf.h"
#include "task.h"   /* §A1 — vmm_cow_fault needs the current task's space */
#include <stdint.h>
#include <stddef.h>

/* Portable VMM entry points implemented lower in this file / used by
 * vmm_user_access_ok before their definition. */
uintptr_t vmm_user_base(void);
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write);

uint64_t* mmu_kernel_l1(void);          /* mmu.c — shared kernel L1 table */

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

    /* §M48 — the mmap bump cursor lives with the ADDRESS SPACE, not with the
     * task.  See the i386 twin for why a per-task cursor (or a snapshot copied
     * at clone time) lets one thread map over another's memory. */
    uintptr_t mmap_cursor;
};

/* Allocate a zeroed 4 KiB translation table.  RAM is identity-mapped, so the
 * physical frame address is directly usable as the kernel pointer. */
static uint64_t* alloc_table(void) {
    pmm_phys_t pa = pmm_alloc_frame();
    if (pa == PMM_ALLOC_FAIL) return NULL;
    uint64_t* t = (uint64_t*)(uintptr_t)pa;
    for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

/* Descend into tbl[idx], allocating a next-level table if absent. */
static uint64_t* next_table(uint64_t* tbl, uint64_t idx) {
    if (!(tbl[idx] & PTE_VALID)) {
        uint64_t* nt = alloc_table();
        if (!nt) return NULL;
        tbl[idx] = ((uint64_t)(uintptr_t)nt) | PTE_VALID | PTE_TABLE;
    }
    return (uint64_t*)(uintptr_t)(tbl[idx] & PTE_ADDR_MASK);
}

/* §M46/security — is [va, va+len) fully mapped + EL0-accessible in the ACTIVE
 * TTBR0 space (loaded during a syscall)?  The ARM twin of the x86 checks: walks
 * L1(>>30)→L2(>>21)→L3(>>12), requiring PTE_VALID + PTE_AP_EL0 (AP[1], EL0 can
 * access) at the leaf; want_write also requires AP[2]==0 (bit 7 clear = writable).
 * A leaf can be an L2 2 MiB block or an L3 page.  Tables are identity-reachable. */
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write) {
    if (len == 0) return 1;
    if (va < vmm_user_base()) return 0;
    if (va + len < va)        return 0;
    uint64_t ttbr0;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    uint64_t* l1 = (uint64_t*)(uintptr_t)(ttbr0 & PTE_ADDR_MASK);
    for (uintptr_t p = va & ~0xFFFUL; p < va + len; p += 0x1000) {
        uint64_t e1 = l1[(p >> 30) & 0x1FF];
        if (!(e1 & PTE_VALID) || !(e1 & PTE_TABLE)) return 0;
        uint64_t* l2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
        uint64_t e2 = l2[(p >> 21) & 0x1FF];
        if (!(e2 & PTE_VALID)) return 0;
        uint64_t leaf;
        if (!(e2 & PTE_TABLE)) {                       /* 2 MiB block at L2 */
            leaf = e2;
        } else {
            uint64_t* l3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
            leaf = l3[(p >> 12) & 0x1FF];
            if (!(leaf & PTE_VALID)) return 0;
        }
        if (!(leaf & PTE_AP_EL0)) return 0;            /* not EL0-accessible */
        if (want_write && (leaf & (1ULL << 7))) return 0;   /* AP[2]=1 → read-only */
    }
    return 1;
}

/* Create a fresh address space: private L1 table with the kernel's identity
 * blocks copied in.  Returns NULL on OOM. */
struct vmm_space* aarch64_vmm_create(void) {
    struct vmm_space* s = (struct vmm_space*)kmalloc(sizeof *s);
    if (!s) return NULL;
    /* kmalloc does not zero: an uninitialised bump cursor is handed straight
     * back to the program as an mmap address. */
    s->mmap_cursor = 0;
    s->l1 = alloc_table();
    if (!s->l1) { kfree(s); return NULL; }
    uint64_t* kl1 = mmu_kernel_l1();
    for (int i = 0; i < 4; i++) s->l1[i] = kl1[i];   /* share kernel low-4 GiB */
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
        :: "r"((uint64_t)(uintptr_t)s->l1) : "memory");
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
        :: "r"((uint64_t)(uintptr_t)mmu_kernel_l1()) : "memory");
}

/* The `vmm` shell command's status dump.  The x86 vmm.c prints page-directory
 * details; aarch64's translation is set up in mmu.c (coarse identity) + this
 * file (per-process EL0 spaces), so report that shape.  Keeps shell.c portable
 * (it just calls vmm_print_status). */
void vmm_print_status(void) {
    kprintf("aarch64 MMU: 4 KiB granule, 39-bit VA; kernel = TTBR0 identity "
            "(1 GiB blocks); per-process EL0 spaces via vmm.c (VA >= 4 GiB)\n");
}

/* x86 drivers (xhci.c) call vmm_map_4mib to identity-map an MMIO BAR window.
 * On aarch64 the PCIe 32-bit MMIO window (where pci.c assigns BARs, 0x1000_0000)
 * is already covered by the low-1-GiB Device block in mmu.c's identity map, so
 * this is a no-op that reports success.  Kept so xhci.c links unchanged. */
int vmm_map_4mib(uint32_t va, uint32_t pa, int flags) {
    (void)va; (void)pa; (void)flags;
    return 0;
}

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

/* Mirrors of vmm.h's flag bits (this file can't include vmm.h — its
 * vmm_map_4mib signature intentionally diverges from the header). */
#define VMM_EXEC_BIT      0x200u
#define VMM_SHARED_BIT    0x400u
#define VMM_WRITABLE_BIT  0x002u
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
    uint64_t* l2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!(e2 & PTE_VALID)) return 0;
    if (!(e2 & PTE_TABLE))                          /* 2 MiB block */
        return (uintptr_t)((e2 & PTE_ADDR_MASK & ~0x1FFFFFULL) | (va & 0x1FFFFF));
    uint64_t* l3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
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
    if (!g_cow_ref) {
        g_cow_nr  = pmm_nr_frames;
        g_cow_ref = (uint16_t*)pmm_bootmem_alloc(g_cow_nr * (uint32_t)sizeof(uint16_t));
        if (!g_cow_ref) { g_cow_nr = 0; return NULL; }
        for (uint32_t i = 0; i < g_cow_nr; i++) g_cow_ref[i] = 0;
    }
    uintptr_t fn = phys >> 12;
    return (fn < g_cow_nr) ? &g_cow_ref[fn] : NULL;
}

/* Drop a reference to a COW frame; free it when the last holder lets go.
 * Called from the teardown path for every page carrying PTE_SW_COW. */
static void cow_release(uintptr_t phys) {
    uint16_t* rc = cow_slot(phys);
    if (rc && *rc > 0) { (*rc)--; return; }   /* someone else still holds it */
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
    if (rc) {
        /* First time this frame is shared, it has one holder already. */
        if (*rc == 0) *rc = 1;
        (*rc)++;
    }
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
    /* The child inherits the parent's mappings, so it inherits the mmap
     * cursor too — restarting at the region base would re-issue addresses the
     * child already has mapped (§M48). */
    s->mmap_cursor = parent->mmap_cursor;
    s->l1 = alloc_table();
    if (!s->l1) { kfree(s); return NULL; }

    uint64_t* kl1 = mmu_kernel_l1();
    for (int i = 0; i < 4; i++) s->l1[i] = kl1[i];      /* kernel low 4 GiB */

    for (int i1 = 4; i1 < 512; i1++) {                  /* user region only */
        uint64_t e1 = parent->l1[i1];
        if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) continue;
        uint64_t* pl2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
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
            uint64_t* pl3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
            uint64_t* cl3 = next_table(cl2, (uint64_t)i2);
            if (!cl3) { vmm_space_destroy(s); return NULL; }
            for (int i3 = 0; i3 < 512; i3++)
                if (pl3[i3] & PTE_VALID)
                    cl3[i3] = cow_share_leaf(&pl3[i3]);
        }
    }
    /* The parent's own entries just became read-only — its TLB still holds the
     * writable versions, so without this its next write would NOT fault and it
     * would scribble on the child's pages. */
    __asm__ volatile ("dsb ish\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
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
    uint64_t* l2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(fault_va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return 0;
    uint64_t* l3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
    unsigned i3 = (unsigned)((fault_va >> 12) & 0x1FF);
    uint64_t pte = l3[i3];
    if (!(pte & PTE_VALID) || !(pte & PTE_SW_COW)) return 0;   /* not a COW page */

    uintptr_t old = (uintptr_t)(pte & PTE_ADDR_MASK);
    uint16_t* rc  = cow_slot(old);

    if (rc && *rc <= 1) {
        /* Last tracked sharer — grant write in place, no copy needed. */
        l3[i3] = (pte & ~PTE_AP_RO_BIT) & ~PTE_SW_COW;
        *rc = 0;
    } else {
        pmm_phys_t nf = pmm_alloc_frame();
        if (nf == PMM_ALLOC_FAIL) return 0;             /* OOM → a real fault */
        const uint8_t* src = (const uint8_t*)(uintptr_t)old;
        uint8_t* dst = (uint8_t*)(uintptr_t)nf;
        for (int b = 0; b < 4096; b++) dst[b] = src[b];
        if (rc && *rc > 0) (*rc)--;
        l3[i3] = (((uint64_t)nf & PTE_ADDR_MASK) | (pte & ~PTE_ADDR_MASK))
                 & ~PTE_AP_RO_BIT & ~PTE_SW_COW;
    }
    __asm__ volatile ("dsb ish\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
    return 1;
}

/* Free the user-region tables (l1[4..]) + their frames; kernel-shared
 * blocks (l1[0..3]) are left alone. */
static void free_l2_subtree(uint64_t* l2) {
    for (int i = 0; i < 512; i++) {
        uint64_t e = l2[i];
        if ((e & PTE_VALID) && (e & PTE_TABLE)) {
            uint64_t* l3 = (uint64_t*)(uintptr_t)(e & PTE_ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                uint64_t pte = l3[j];
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
    for (int i = 4; i < 512; i++) {                 /* user region = VA >= 4 GiB */
        uint64_t e = s->l1[i];
        if ((e & PTE_VALID) && (e & PTE_TABLE)) {
            uint64_t* l2 = (uint64_t*)(uintptr_t)(e & PTE_ADDR_MASK);
            free_l2_subtree(l2);
            pmm_free_frame((pmm_phys_t)(e & PTE_ADDR_MASK));           /* L2 table */
        }
    }
    pmm_free_frame((pmm_phys_t)(uintptr_t)s->l1);                      /* L1 table */
    kfree(s);
}

int vmm_space_map(struct vmm_space* s, uintptr_t va, uintptr_t pa, uint32_t flags) {
    if (!s) return -1;                              /* kernel space not user-mappable */
    int rc = aarch64_vmm_map_user(s, va, pa, 4096, (flags & VMM_EXEC_BIT) ? 1 : 0);
    if (rc == 0 && (flags & VMM_SHARED_BIT)) {      /* tag borrowed frame in L3 */
        uint64_t e1 = s->l1[(va >> 30) & 0x1FF];
        uint64_t* l2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
        uint64_t e2 = l2[(va >> 21) & 0x1FF];
        uint64_t* l3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
        l3[(va >> 12) & 0x1FF] |= PTE_SW_SHARED;
    }
    return rc;
}

void vmm_space_unmap(struct vmm_space* s, uintptr_t va) {
    if (!s) return;
    uint64_t e1 = s->l1[(va >> 30) & 0x1FF];
    if (!((e1 & PTE_VALID) && (e1 & PTE_TABLE))) return;
    uint64_t* l2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return;
    uint64_t* l3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
    l3[(va >> 12) & 0x1FF] = 0;
    __asm__ volatile ("dsb ish\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
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
    uint64_t* l2 = (uint64_t*)(uintptr_t)(e1 & PTE_ADDR_MASK);
    uint64_t e2 = l2[(va >> 21) & 0x1FF];
    if (!((e2 & PTE_VALID) && (e2 & PTE_TABLE))) return -1;
    uint64_t* l3 = (uint64_t*)(uintptr_t)(e2 & PTE_ADDR_MASK);
    unsigned i = (unsigned)((va >> 12) & 0x1FF);
    uint64_t e3 = l3[i];
    if (!(e3 & PTE_VALID)) return -1;

    if (flags & VMM_WRITABLE_BIT) e3 &= ~PTE_AP_RO;  /* writable  */
    else                      e3 |=  PTE_AP_RO;      /* read-only */
    if (flags & VMM_EXEC_BIT) e3 &= ~PTE_UXN;        /* EL0-executable */
    else                      e3 |=  PTE_UXN;
    l3[i] = e3;
    __asm__ volatile ("dsb ish\ntlbi vmalle1\ndsb ish\nisb" ::: "memory");
    return 0;
}

uintptr_t vmm_space_pd_phys(struct vmm_space* s) {
    return (uintptr_t)(s ? s->l1 : mmu_kernel_l1());
}

void vmm_space_switch(struct vmm_space* s) {
    uint64_t target = (uint64_t)(uintptr_t)(s ? s->l1 : mmu_kernel_l1());
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


/* §M48 — the address space's mmap bump cursor.  Policy (where the region
 * starts, how far it may grow) stays in usyscall.c; the SPACE only owns the
 * storage, so every task sharing an mm shares one cursor. */
uintptr_t vmm_space_mmap_cursor(struct vmm_space* s) {
    return s ? s->mmap_cursor : 0;
}
void vmm_space_set_mmap_cursor(struct vmm_space* s, uintptr_t v) {
    if (s) s->mmap_cursor = v;
}
