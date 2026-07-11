/* =============================================================================
 * vmm.h — Virtual Memory Manager public interface (arch-neutral API).
 *
 * After `vmm_init`, paging is on and a sizeable chunk of physical
 * memory is identity-mapped via the largest page granule the arch
 * supports cheaply:
 *
 *   i386:   first 256 MiB via 4 MiB PSE pages.
 *   x86_64: first 1 GiB via 2 MiB pages (set up in boot.s before C
 *           runs; vmm.c inherits them).
 *
 * That covers the kernel image, low memory, and any PMM-allocated
 * frame we currently care about, so every pointer we already hold
 * keeps working as-is.
 *
 * `vmm_map` installs a 4 KiB-granular virtual→physical mapping using
 * conventional N-level tables, allocating new intermediate tables
 * from the PMM when the relevant entry is absent.  Like the i386
 * impl, it refuses to refine an entry that is already a "large page"
 * (PSE on i386, 2 MiB / 1 GiB on x86_64) — splitting a large page
 * into 4 KiB ones is significantly more subtle and not needed yet.
 *
 * Addresses are `uintptr_t` (4 bytes on i386, 8 bytes on x86_64) so
 * the same prototype works on both arches without conditional
 * compilation at call sites.  i386 callers see no source change;
 * x86_64 callers can pass full 64-bit addresses.
 *
 * Paging reference: Intel SDM Vol 3 §4 (Paging), AMD64 APM Vol 2 §5.
 * ============================================================================= */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Flags passed to `vmm_map`.  Present bit is implicit — unmap if you want
 * a P=0 entry.  These constants match the hardware bit positions in BOTH
 * i386 PTEs and x86_64 PTEs — Intel kept the low 12 bits compatible when
 * adding long mode, so the same values flow straight into either page
 * table format. */
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

/* Turn on paging + identity-map the kernel region.  On i386 this builds
 * the page directory and turns on CR0.PG; on x86_64 paging is already
 * on (set up in boot.s) and vmm_init just records the PML4 it inherited
 * so subsequent vmm_map calls can walk it. */
void vmm_init(void);

/* Install a mapping from virtual `virt` to physical `phys`, both 4 KiB
 * aligned.  Returns 0 on success, non-zero if the relevant entry is
 * already a large page or a page-table allocation fails. */
int vmm_map(uintptr_t virt, uintptr_t phys, uint32_t flags);

/* Physical address of the top-level page table.  Returned as uintptr_t
 * so it works on both archs.  Used by the AP boot trampoline (M18) so
 * each AP can load the same table into CR3 before enabling paging. */
uintptr_t vmm_kernel_pd_phys(void);

/* Install a single "large page" mapping spanning 4 MiB of address space.
 *
 * Granule rationale:
 *   - i386 PSE pages are exactly 4 MiB (one PDE covers 4 MiB).
 *   - x86_64 long-mode large pages are 2 MiB at the PD level; the
 *     impl installs TWO adjacent 2 MiB entries to keep the 4 MiB
 *     contract.
 *
 * Both archs require `virt` and `phys` to be 4 MiB aligned.  Returns
 * 0 on success, non-zero if a conflicting non-large mapping already
 * exists.  Useful for cheap MMIO mappings (framebuffer, xHCI BARs)
 * without burning a page table per 4 KiB. */
int vmm_map_4mib(uintptr_t virt, uintptr_t phys, uint32_t flags);

/* Remove a mapping.  Safe to call on unmapped addresses (no-op). */
void vmm_unmap(uintptr_t virt);

/* Walk the tables and return the physical address `virt` maps to,
 * with the low 12 bits copied from `virt`.  Returns 0 if unmapped. */
uintptr_t vmm_translate(uintptr_t virt);

/* One-line diagnostics. */
void vmm_print_status(void);

/* ===========================================================================
 * Per-process address spaces (M25 stage 1).
 *
 * Until M25 every task shared the single kernel address space (one page
 * directory / PML4).  A `vmm_space` is a *separate* top-level page table
 * that keeps the kernel mapped in every space (so ring-0 code + the kernel
 * stack keep working after a CR3/TTBR switch) but owns a private *user*
 * region for a process's code / data / stack.
 *
 * Portability: opaque handle, uintptr_t physical addresses — the i386,
 * x86_64 and aarch64 backends each implement it over their own table
 * format.  `NULL` denotes "the kernel space" everywhere (a kernel thread
 * has no private user mappings), so `vmm_space_switch(NULL)` returns the
 * CPU to the shared kernel table.
 *
 * Kernel-mapping model (stage 1): a freshly created space snapshots the
 * kernel's top-level entries at creation time.  All kernel MMIO / high
 * mappings are established at boot, before any user process exists, so the
 * snapshot is complete.  (A kernel mapping *added* after a space is created
 * would not propagate into it — a known stage-1 limitation; the eventual
 * fix is shared kernel page-table pages / a PDE generation counter.  Not
 * needed while all high mappings are boot-time.)
 * =========================================================================== */

struct vmm_space;

/* Create a new user address space (kernel mapped, user region empty).
 * Returns NULL on OOM. */
struct vmm_space* vmm_space_create(void);

/* Free a space: its user page tables + the top-level table.  Must NOT be
 * the currently-loaded space on any CPU.  NULL is a no-op. */
void vmm_space_destroy(struct vmm_space* space);

/* M34 — clone a space for fork(): every private user page is EAGERLY copied
 * into a fresh space with the same VA + flags (VMM_SHARED pages are shared,
 * not copied — they are borrowed shm frames).  Returns NULL on OOM.  (Eager
 * copy first; copy-on-write is a later optimisation.)  i386 impl today. */
struct vmm_space* vmm_space_clone(struct vmm_space* parent);

/* Map / unmap a page in a *specific* space's user region.  Same flag
 * semantics as vmm_map (pass VMM_USER for a ring-3-accessible page). */
int  vmm_space_map(struct vmm_space* space, uintptr_t virt, uintptr_t phys,
                   uint32_t flags);
void vmm_space_unmap(struct vmm_space* space, uintptr_t virt);

/* Physical address of the space's top-level table (CR3 / PML4 / TTBR0). */
uintptr_t vmm_space_pd_phys(struct vmm_space* space);

/* Make `space` (NULL = kernel space) the active address space on this CPU.
 * Loads CR3 (x86) / TTBR0 (aarch64) only when it actually changes, so
 * switching between kernel threads is free. */
void vmm_space_switch(struct vmm_space* space);

/* Base virtual address of the per-process user region on this arch — the
 * first address a loader/self-test may hand to vmm_space_map for VMM_USER
 * pages.  It is chosen to sit clear of the kernel's identity map: i386 /
 * x86_64 return 1 GiB (0x40000000); aarch64 returns 4 GiB (its identity
 * map covers the low 4 GiB). */
uintptr_t vmm_user_base(void);

#endif
