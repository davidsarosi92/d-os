/* =============================================================================
 * tss.h — Task State Segment (x86 32-bit).
 *
 * The TSS is the CPU's window onto the kernel for ring-3 → ring-0
 * transitions.  When a user-mode interrupt or exception fires, the CPU
 * switches to the kernel stack pointed to by `tss.esp0` (with `ss0` as
 * the kernel data selector) before pushing its frame and jumping to
 * the gate's handler.
 *
 * We don't use TSS-based hardware task switching (it's slow and
 * deprecated); we just need its stack-switch facility.  All ring 3
 * transitions reuse the same TSS.
 *
 * Reference: Intel SDM Vol 3, §7.2 "Task Management Data Structures".
 * ============================================================================= */

#ifndef TSS_H
#define TSS_H

#include <stdint.h>

/* Initialize the static TSS (zero everything, set ss0/esp0).  Called
 * before `gdt_init` because the GDT needs the TSS address for its
 * descriptor entry. */
void     tss_init(void);

/* Update the kernel stack pointer the CPU should switch to on the next
 * privilege-elevating event.  Called per task once we have processes. */
void     tss_set_kernel_stack(uint32_t esp);

/* Address + size accessors used by gdt.c when it builds the TSS
 * descriptor in the GDT. */
uint32_t tss_get_addr(void);
uint32_t tss_get_limit(void);

#endif
