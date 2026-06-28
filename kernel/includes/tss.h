/* =============================================================================
 * tss.h — Task State Segment (architecture-neutral API).
 *
 * The TSS is the CPU's window onto the kernel for ring-3 → ring-0
 * transitions.  When a user-mode interrupt or exception fires, the CPU
 * switches to the kernel stack pointed to by the TSS's stack-pointer
 * slot (esp0 on i386, rsp0 on x86_64) before pushing its frame and
 * jumping to the gate's handler.
 *
 * We don't use TSS-based hardware task switching (it's slow and
 * deprecated, and doesn't exist at all in long mode); we just need its
 * stack-switch facility.  All ring 3 transitions reuse the same TSS.
 *
 * The API takes uintptr_t for stack pointers / addresses so the same
 * signature works on both 32-bit and 64-bit kernels.  Per-arch impl
 * lives in kernel/hal/<arch>/tss.c.
 *
 * References:
 *   Intel SDM Vol 3 §7.2 (32-bit TSS) — i386 layout.
 *   Intel SDM Vol 3 §7.7  (64-bit TSS) — x86_64 layout (RSP0/1/2 + IST).
 * ============================================================================= */

#ifndef TSS_H
#define TSS_H

#include <stdint.h>

/* Initialize the static TSS (zero everything, set ss0/esp0 / rsp0).
 * Called before `gdt_init` because the GDT needs the TSS address for
 * its descriptor entry. */
void      tss_init(void);

/* Update the kernel stack pointer the CPU should switch to on the next
 * privilege-elevating event.  Called per task once we have processes.
 * Width matches the kernel pointer size (4 bytes i386, 8 bytes x86_64). */
void      tss_set_kernel_stack(uintptr_t sp);

/* Address + size accessors used by gdt.c when it builds the TSS
 * descriptor in the GDT.
 *
 * x86_64 note: the TSS descriptor in a long-mode GDT is 16 bytes
 * (vs. 8 on i386) — it spans two GDT slots and carries a full 64-bit
 * base.  gdt.c on x86_64 handles that asymmetry; this header just
 * exposes the underlying address. */
uintptr_t tss_get_addr(void);
uint32_t  tss_get_limit(void);

#endif
