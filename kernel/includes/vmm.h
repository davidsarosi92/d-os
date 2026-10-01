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

/* The flag vocabulary + the §M75 walk callback.  They live in their own header
 * because ONE file in the tree cannot include this one (aarch64's vmm.c —
 * see vmm_flags.h for why), and hand-copied mirrors of a flag set are how a
 * bit comes to mean two different things on two architectures. */
#include "vmm_flags.h"

/* Turn on paging + identity-map the kernel region.  On i386 this builds
 * the page directory and turns on CR0.PG; on x86_64 paging is already
 * on (set up in boot.s) and vmm_init just records the PML4 it inherited
 * so subsequent vmm_map calls can walk it. */
void vmm_init(void);

/* Install a mapping from virtual `virt` to physical `phys`, both 4 KiB
 * aligned.  Returns 0 on success, non-zero if the relevant entry is
 * already a large page or a page-table allocation fails. */
/* `phys` is 64-bit on every arch (§M86): with i386 PAE a frame may lie above
 * 4 GiB, and a uintptr_t would silently truncate it to a different page. */
int vmm_map(uintptr_t virt, uint64_t phys, uint32_t flags);

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

/* §M46/security — is [va, va+len) fully mapped + USER-accessible in the ACTIVE
 * address space (the process CR3, live during a syscall)?  A syscall must gate
 * every ring-3 pointer through this before the kernel touches it, so a bad
 * pointer returns an error instead of faulting the kernel (freeze) or reaching
 * kernel memory.  want_write also requires the page be writable.  Arch-specific
 * (walks the active page tables).  See copy_from_user / copy_to_user. */
int vmm_user_access_ok(uintptr_t va, uintptr_t len, int want_write);

/* Copy `len` bytes to/from a ring-3 pointer, validating it first with
 * vmm_user_access_ok.  Return 0 on success, -1 if the user range is not safe
 * (unmapped / kernel / wrong permission) — the caller returns -EFAULT instead
 * of faulting the kernel.  (TOCTOU note: a concurrent thread could unmap between
 * the check and the copy; a fault-fixup path is the full hardening — see
 * usyscall.c.) */
int copy_from_user(void* dst, uintptr_t user_src, uintptr_t len);
int copy_to_user(uintptr_t user_dst, const void* src, uintptr_t len);

/* Copy a NUL-terminated string from ring 3 into `dst` (≤ max, always
 * NUL-terminated), validating page by page.  Returns the length, or -1 on a bad
 * pointer.  An arch dispatcher uses it to turn a user path argument into a
 * kernel string before calling a sys_*_k core (see syscall.h). */
int copy_str_from_user(char* dst, uintptr_t user_src, uintptr_t max);

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
/* §M86 — is no-execute enforced for user pages on this machine?  1 when a
 * user mapping without VMM_EXEC really cannot be executed (x86 PAE / long mode
 * with a CPU that has NX; aarch64 always, through UXN), 0 when VMM_EXEC is
 * only advisory (i386 classic paging, or a CPU without NX). */
int vmm_nx_active(void);

struct vmm_space* vmm_space_create(void);

/* Free a space: its user page tables + the top-level table.  Must NOT be
 * the currently-loaded space on any CPU.  NULL is a no-op. */
void vmm_space_destroy(struct vmm_space* space);

/* M34 — clone a space for fork(): every private user page is EAGERLY copied
 * into a fresh space with the same VA + flags (VMM_SHARED pages are shared,
 * not copied — they are borrowed shm frames).  Returns NULL on OOM.  (Eager
 * copy first; copy-on-write is a later optimisation.)  i386 impl today. */
struct vmm_space* vmm_space_clone(struct vmm_space* parent);

/* M34 — copy-on-write page-fault handler.  Called from the #PF path (idt.c)
 * with the faulting virtual address.  If it names a COW page in the current
 * task's space, resolve it (give the writer a private, writable copy — or make
 * the page writable in place if it is the last sharer) and return 1 (retry the
 * instruction); return 0 if it is not a COW fault (→ a real fault). */
int vmm_cow_fault(uintptr_t fault_va);

/* Map / unmap a page in a *specific* space's user region.  Same flag
 * semantics as vmm_map (pass VMM_USER for a ring-3-accessible page). */
int  vmm_space_map(struct vmm_space* space, uintptr_t virt, uint64_t phys,
                   uint32_t flags);
void vmm_space_unmap(struct vmm_space* space, uintptr_t virt);

/* §M37 — change protection of an already-mapped page (the mprotect primitive),
 * keeping its frame.  Same flag semantics as vmm_space_map.  Returns 0, or -1
 * if the page is not mapped. */
int  vmm_space_protect(struct vmm_space* space, uintptr_t virt, uint32_t flags);

/* Physical address of the space's top-level table (CR3 / PML4 / TTBR0). */
uintptr_t vmm_space_pd_phys(struct vmm_space* space);

/* §M48's mmap bump cursor is gone (§M89): addresses come from the
 * reservation set in vma.h, which also gives them back on munmap. */

/* Make `space` (NULL = kernel space) the active address space on this CPU.
 * Loads CR3 (x86) / TTBR0 (aarch64) only when it actually changes, so
 * switching between kernel threads is free. */
void vmm_space_switch(struct vmm_space* space);
/* The hardware root (CR3/TTBR0 value) of a space, for diagnostics. */
uintptr_t vmm_space_root_phys(struct vmm_space* space);

/* Base virtual address of the per-process user region on this arch — the
 * first address a loader/self-test may hand to vmm_space_map for VMM_USER
 * pages.  It is chosen to sit clear of the kernel's identity map: i386 /
 * x86_64 return 1 GiB (0x40000000); aarch64 returns 4 GiB (its identity
 * map covers the low 4 GiB). */
uintptr_t vmm_user_base(void);
/* §M90 — the LOWEST address a program may own (a fixed-address Linux binary,
 * MAP_FIXED, a user pointer passed to a syscall).  vmm_user_base() stays where
 * this system's own programs and PIE images are placed; on aarch64 the region
 * below it is the program's too (64 KiB up), on x86 not yet. */
uintptr_t vmm_user_min(void);

/* ===========================================================================
 * §M75 — what does a space actually hold?
 *
 * A task manager has to answer "how much memory is this process using", and
 * nothing here could: no `vmm.c` counted a space's resident pages.
 *
 * THE OBVIOUS FIX IS THE TRAP.  A counter bumped inside vmm_space_map/unmap
 * would be THREE counters, because **`vmm.c` exists three times** (x86,
 * x86_64, aarch64) — kept in step by hand, and the one that drifts is the one
 * on the arch nobody happens to be running that week.  That is §M70's whole
 * finding and §4.63's `setconf` shape.
 *
 * AND A COUNTER CANNOT ANSWER THIS QUESTION CORRECTLY ANYWAY.  Whether a COW
 * frame is still shared changes when ANOTHER process forks or exits, so the
 * owner's counter goes stale without the owner having done anything.  The
 * number is a property of the moment, so it is MEASURED at the moment.
 *
 * So the split is: **each arch supplies the WALK; the POLICY lives in one
 * portable place** (`kernel/mem/vmm_account.c`).  Page tables really are
 * arch-specific, and each backend's traversal mirrors its own
 * `vmm_space_destroy`, which already had to know precisely the same rules —
 * which entries are kernel-shared, which are large pages, which are borrowed.
 * What a page MEANS is then decided exactly once.
 *
 * THE WALKER REPORTS PORTABLE FLAGS, NOT RAW PTE BITS.  aarch64 keeps
 * "borrowed" in bit 55 and "COW" in bit 56 while x86 uses 0x400 / 0x800; hand
 * a raw entry to a shared policy and it silently tests the wrong bits on one
 * architecture — which is three policies again, wearing one function's name.
 * Each backend translates into VMM_SHARED / VMM_COW / VMM_WRITABLE / VMM_USER
 * before it calls back.
 *
 * LOCKING, AND WHAT IT DOES **NOT** BUY.  The caller must hold whatever keeps
 * the tasks alive — `task_for_each`'s master lock is what the accounting pass
 * uses, and it is what stops a space being destroyed mid-walk.
 *
 * **IT DOES NOT STOP THE OWNER FROM MAPPING.**  `mmap` runs on the owning
 * task's own CPU and takes no scheduler lock, so on an SMP box the walk reads
 * a page table that is being written.  Two consequences, and they are
 * different sizes:
 *
 *   - the FIGURE is a sample of a moving target.  A page mapped during the
 *     walk is counted or not depending on where the walk had got to.  That is
 *     acceptable and is why this is called a measurement rather than a total.
 *
 *   - the WALK must not be led off a cliff by a half-built table.  That is an
 *     ordering question, and on aarch64 it was a real one: `next_table`
 *     published a descriptor without a barrier after zeroing the frame behind
 *     it, so another observer could read the pointer and then read whatever
 *     the allocator had last left in that frame.  Fixed at the source (see the
 *     note there); nothing here defends against it, because a reader that
 *     tolerates a corrupt table is a reader that hides the corruption.
 * =========================================================================== */

/* `vmm_space_walk` and `vmm_frame_share_count` are declared in vmm_flags.h,
 * beside the flag vocabulary they report in — the aarch64 backend implements
 * both and cannot include THIS header.  Two things to know about them:
 *
 *   - the walk reports a space's PRIVATE region only; the kernel's own
 *     mappings, which every space shares, are never visited;
 *   - the share count DELIBERATELY DOES NOT CREATE the refcount table.  It is
 *     a query, and a query that allocates fails exactly when memory is short —
 *     which is precisely when somebody is reading a task manager.
 *
 * Portable accounting over them (kernel/mem/vmm_account.c).
 *
 * `private` = frames this space alone owns, i.e. **what would be handed back
 * if the process exited now**.  That is the number a task manager wants, and
 * it is the only one whose column can be SUMMED: reporting a frame in every
 * space that maps it makes the total exceed the machine's memory, which is the
 * kind of number that destroys trust in the whole window.
 *
 * `shared` = frames it maps but does not solely own (borrowed shm/memfd, and
 * COW pages another space still holds).  Reported separately rather than
 * dropped, so "this process is small" and "this process shares everything it
 * touches" stay distinguishable.
 *
 * Either pointer may be NULL.  A NULL space (a kernel thread) yields zeroes —
 * it has no private user region, which is a real answer and not a failure. */
void vmm_space_resident(struct vmm_space* space,
                        uint64_t* out_private, uint64_t* out_shared);

#endif
