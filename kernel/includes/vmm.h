/* =============================================================================
 * vmm.h — Virtual Memory Manager public interface.
 *
 * After `vmm_init`, paging is on and the first 256 MiB of physical memory
 * is identity-mapped via 4 MiB PSE pages (base = virt, no translation).
 * That covers the kernel image, low memory, and any PMM-allocated frame
 * we currently care about, so every pointer we already hold keeps
 * working as-is.
 *
 * `vmm_map` installs a 4 KiB-granular virtual→physical mapping using
 * conventional two-level tables, allocating a new page table from the
 * PMM when the relevant PDE is absent.  It explicitly refuses to modify
 * PDEs that are currently 4 MiB PSE entries (the kernel identity map)
 * because unpacking a 4 MiB mapping into 1024 × 4 KiB entries is
 * significantly more subtle and not needed yet.
 *
 * Paging reference: Intel SDM Vol 3, Chapter 4 ("Paging").
 * ============================================================================= */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* Flags passed to `vmm_map`.  Present bit is implicit — unmap if you want
 * a P=0 entry.  These constants match the hardware bit positions so they
 * can flow straight into PTEs. */
#define VMM_WRITABLE     0x002
#define VMM_USER         0x004
#define VMM_WRITE_THRU   0x008
#define VMM_CACHE_DIS    0x010

/* Turn on paging + identity-map the kernel region.  Must be called after
 * `pmm_init` so subsequent `vmm_map` calls can allocate page tables. */
void vmm_init(void);

/* Install a mapping from virtual `virt` to physical `phys`, both 4 KiB
 * aligned.  Returns 0 on success, non-zero if the PDE is a 4 MiB PSE
 * entry or a PT allocation fails. */
int vmm_map(uint32_t virt, uint32_t phys, uint32_t flags);

/* Physical address of the kernel page directory.  Used by the AP boot
 * trampoline (M18) so each AP can load CR3 before enabling paging. */
uint32_t vmm_kernel_pd_phys(void);

/* Install a single 4 MiB PSE mapping.  `virt` and `phys` must both be
 * 4 MiB aligned.  Returns 0 on success, non-zero if a non-PSE mapping
 * already exists in that PDE slot.  Useful for mapping MMIO-ish regions
 * (like a framebuffer) cheaply without burning a page table. */
int vmm_map_4mib(uint32_t virt, uint32_t phys, uint32_t flags);

/* Remove a mapping.  Safe to call on unmapped addresses (no-op). */
void vmm_unmap(uint32_t virt);

/* Walk the tables and return the physical address `virt` maps to, with
 * the low 12 bits copied from `virt`.  Returns 0 if unmapped. */
uint32_t vmm_translate(uint32_t virt);

/* One-line diagnostics. */
void vmm_print_status(void);

#endif
