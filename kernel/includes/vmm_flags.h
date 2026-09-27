/* =============================================================================
 * vmm_flags.h — the page-mapping flag vocabulary, and the walk callback (§M75).
 *
 * WHY THIS IS ITS OWN HEADER.  These constants belong to vmm.h and lived there
 * — but `kernel/hal/aarch64/vmm.c` **cannot include vmm.h**: its
 * `vmm_map_4mib` signature deliberately diverges from the declared one.  So it
 * carried HAND-COPIED MIRRORS of the flags (`VMM_SHARED_BIT`, `VMM_EXEC_BIT`,
 * `VMM_WRITABLE_BIT`), which is two definitions of one thing kept in step by
 * nobody.
 *
 * That mattered the moment §M75 asked the arch backends to translate their
 * native descriptor bits into this vocabulary: the aarch64 walker has to name
 * VMM_COW and VMM_USER, neither of which had a mirror — and the failure mode
 * of adding two more copies is a flag that means one thing on x86 and another
 * on ARM, silently, on the architecture the developer is not running.  §M70's
 * finding, and the reason the accounting policy is portable at all.
 *
 * So the flags move here, vmm.h includes this, and the one file that cannot
 * include vmm.h includes THIS instead.  Nothing is copied.
 *
 * The values match the hardware bit positions in BOTH i386 and x86_64 PTEs —
 * Intel kept the low 12 bits compatible when adding long mode — so the two x86
 * backends can store them directly.  aarch64's descriptor layout is unrelated
 * and it translates at its own boundary; see `vmm_space_walk` there.
 * ============================================================================= */

#ifndef VMM_FLAGS_H
#define VMM_FLAGS_H

#include <stdint.h>

/* Flags passed to `vmm_map`.  Present bit is implicit — unmap if you want
 * a P=0 entry. */
#define VMM_WRITABLE     0x002
#define VMM_USER         0x004
#define VMM_WRITE_THRU   0x008
#define VMM_CACHE_DIS    0x010
/* M25 — request an executable mapping.  Enforced where the arch has an
 * execute-permission bit (aarch64 UXN); on x86 today (no NX yet) pages are
 * executable regardless, so this is advisory there.  Sits in an
 * OS-available PTE bit on x86, masked out of the hardware entry. */
#define VMM_EXEC         0x200
/* M25 — a BORROWED mapping: the frame is owned by someone else (a shm object
 * shared between processes), so vmm_space_destroy must NOT free it — only
 * drop the mapping.  Stored in an OS-available PTE bit (x86 bit 10 / aarch64
 * software bit 55), invisible to the hardware walk. */
#define VMM_SHARED       0x400
/* M34 — a COPY-ON-WRITE mapping: the page is shared read-only between spaces
 * after fork; a write faults and vmm_cow_fault() gives the writer a private
 * copy.  Stored in PTE bit 11 (OS-available) on x86, software bit 56 on
 * aarch64.  A per-frame refcount tracks how many spaces still share it so the
 * frame is freed only by the last owner. */
#define VMM_COW          0x800

struct vmm_space;

/* §M75 — called once per PRESENT page in a space's PRIVATE region.  `flags`
 * carries the VMM_* bits above, translated by the backend where the hardware
 * layout differs.  Declared here rather than in vmm.h so the aarch64 backend
 * can implement `vmm_space_walk` without including a header it cannot. */
typedef void (*vmm_walk_fn)(void* ctx, uintptr_t va, uint64_t phys,
                            uint32_t flags);

void vmm_space_walk(struct vmm_space* space, vmm_walk_fn cb, void* ctx);
uint32_t vmm_frame_share_count(uint64_t phys);   /* 64-bit: i386 PAE frames */

/* ---------------------------------------------------------------------------
 * §M72 stage 3 — AN EVICTED PAGE IS RECORDED IN ITS OWN PAGE-TABLE ENTRY.
 *
 * A not-present entry has every bit but bit 0 free to software, so the slot
 * the page was written to lives there, with the page's permissions — the page
 * tables stay the ONE record of what a mapping is (a side table keyed by VA
 * would have to be kept in step across fork, exec, mprotect and teardown).
 * The same encoding on every arch:
 *     slot << 12  |  VMM_SWPE_MARK  |  VMM_SWPE_W?  |  VMM_SWPE_X?
 * and bit 0 (present / valid) clear.
 *
 *  vmm_space_mark_swapped  a PRESENT user page becomes that entry, with the
 *                          TLB invalidated everywhere (a weakening, §M51).
 *                          The frame is the CALLER's to free afterwards.
 *                          0, or -1 if the page is not present.
 *  vmm_space_walk_swapped  every such entry in the private region.
 *  swap_slot_release       called by vmm_space_destroy for each one, so a
 *                          process that exits while evicted gives its slots
 *                          back (swap.c; a weak no-op where it is absent).
 * Bringing a page back is the ordinary vmm_space_map over the entry. */
#define VMM_SWPE_MARK   0x200u
#define VMM_SWPE_W      0x100u
#define VMM_SWPE_X      0x080u
typedef void (*vmm_swapped_fn)(void* ctx, uintptr_t va, uint32_t slot, uint32_t flags);
int  vmm_space_mark_swapped(struct vmm_space* space, uintptr_t va, uint32_t slot,
                            uint32_t flags);
void vmm_space_walk_swapped(struct vmm_space* space, vmm_swapped_fn cb, void* ctx);
void swap_slot_release(uint32_t slot);

#endif
