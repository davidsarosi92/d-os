/* =============================================================================
 * vma.h — what a process's address space is SUPPOSED to hold (§M89 rung 1).
 *
 * The page tables say what is mapped NOW.  Until §M89 that was also the only
 * record of what a program had ASKED for, which forced three things:
 *
 *   - an anonymous mmap allocated and zeroed every frame up front (there was
 *     nowhere to write down "zero pages go here, when touched");
 *   - addresses came from a bump cursor that never went back down (there was
 *     nowhere to write down which ranges were free again);
 *   - PROT_NONE mapped a present, readable page (there was nowhere to write
 *     down "this range is reserved but must fault").
 *
 * A program like HotSpot RESERVES gigabytes of address space with PROT_NONE
 * and commits pieces of it later; its thread-stack guard pages must fault.
 * None of that can be expressed without this layer.
 *
 * A `vma` is a range [start, end) with a protection and a BACKING:
 *   VMA_ANON   demand-zero: a frame appears on the first touch;
 *   VMA_FILE   a private file mapping, filled on the first touch from the
 *              page cache (pcache.c), copy-on-write;
 *   VMA_EAGER  the pages were mapped by whoever created the range (shm,
 *              MAP_SHARED files, driver windows) — the vma only RESERVES the
 *              addresses and records the protection.
 *
 * ONE SET PER ADDRESS SPACE, not per task: threads share an mm and must share
 * its reservations (§M48's cursor lesson, one layer up).  The set is guarded by
 * a SLEEPING lock, because filling a file page can read the disk.
 *
 * The arch backends know nothing about vmas beyond two hooks: a clone copies
 * the set (vma_clone) and a destroy frees it (vma_destroy).  What a range
 * MEANS is decided here, once, for all three architectures.
 * ============================================================================= */
#ifndef DOS_VMA_H
#define DOS_VMA_H

#include <stdint.h>
#include <stddef.h>

struct vmm_space;
struct ofile;

/* Linux mmap flag and prot values — the same on all three guests. */
#define VMA_PROT_READ   0x1
#define VMA_PROT_WRITE  0x2
#define VMA_PROT_EXEC   0x4
#define VMA_MAP_SHARED          0x01
#define VMA_MAP_PRIVATE         0x02
#define VMA_MAP_FIXED           0x10
#define VMA_MAP_ANONYMOUS       0x20
#define VMA_MAP_NORESERVE       0x4000
#define VMA_MAP_FIXED_NOREPLACE 0x100000

/* Map for the CURRENT task: the whole mmap(2) contract.  Returns the address
 * or a negative errno (-ENOMEM, -EINVAL, -EEXIST, -EBADF, -EACCES). */
long vma_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, uint64_t off);
long vma_munmap(uintptr_t addr, size_t len);
long vma_mprotect(uintptr_t addr, size_t len, int prot);

/* Reserve `npages` of address space in `mm` for a caller that maps the pages
 * itself (a driver window, the native sys_mmap).  Returns the address or 0.
 * The range is recorded as VMA_EAGER so nothing else is handed it. */
uintptr_t vma_reserve_eager(struct vmm_space* mm, size_t npages, int prot);

/* A fault at `va` in the current task's space: if a vma says a page belongs
 * there and the access is allowed, provide it and return 1 (retry the
 * access).  0 = not ours (a real fault).  Never sleeps when !can_sleep — it
 * declines instead, and the uaccess fixup turns that into -EFAULT. */
int  vma_fault(uintptr_t va, int is_write, int can_sleep);

/* Called by vmm_user_access_ok before it refuses a range: bring in any
 * demand pages the range covers, so a syscall writing into a freshly
 * mmap'd buffer does not see "not mapped".  Returns 1 if every page of
 * [va, va+len) is now either present or not ours to fill. */
int  vma_prefault(uintptr_t va, uintptr_t len, int is_write);
/* §M90 — print what covers `va` (diagnostics; task context). */
void vma_explain(uintptr_t va);

/* madvise(MADV_DONTNEED) on a private range: its pages go, and the next touch
 * brings a fresh zero page (anonymous) or the file's page back (file).  What a
 * garbage collector uses to give memory back without giving up the range. */
long vma_madvise_dontneed(uintptr_t addr, size_t len);

/* mincore: one byte per page, bit 0 = resident NOW.  A reserved page that was
 * never touched is not resident — answering by prefaulting would change the
 * answer by asking the question.  -ENOMEM if part of the range is not the
 * program's at all. */
long vma_mincore(uintptr_t addr, size_t len, uint8_t* vec_kernel);

/* Arch hooks: copy the parent's set into a fork child, free a space's set. */
void vma_clone(struct vmm_space* parent, struct vmm_space* child);
void vma_destroy(struct vmm_space* mm);

/* Diagnostics: print the current task's set (`maps` command, /proc later). */
void vma_dump(struct vmm_space* mm);

/* ---- per-arch primitives this layer needs (implemented in each vmm.c) ---- */

/* Storage for the set on the space (one pointer, owned by vma.c). */
void* vmm_space_vma(struct vmm_space* s);
void  vmm_space_set_vma(struct vmm_space* s, void* v);

/* 0 = nothing at `va`; 1 = a present page; 2 = a non-present entry that
 * still MEANS something (an evicted page's swap slot). */
int   vmm_space_probe(struct vmm_space* s, uintptr_t va);

/* What [start, end) holds: 0 = nothing at all (it may be handed to a
 * program); VMA_RS_USER = at least one user entry; VMA_RS_KERNEL = a page
 * table shared with the kernel covers part of it (never the program's, not
 * even under MAP_FIXED).  This is what keeps the allocator off the ELF image,
 * the stack and the kernel's MMIO windows without a hard-coded layout. */
#define VMA_RS_USER    1
#define VMA_RS_KERNEL  2
int   vmm_space_range_state(struct vmm_space* s, uintptr_t start, uintptr_t end);

/* One past the highest user address this arch's page tables can hold. */
uintptr_t vmm_user_limit(void);

#endif
