/* gdt.h — Global Descriptor Table public interface (x86).
 *
 * The GDT tells the CPU how to interpret segment selectors.  We only need
 * a few flat (base=0, limit=4 GiB) entries for now: null, kernel code,
 * kernel data.  Later milestones add user-mode code/data and a TSS for
 * privilege transitions.
 *
 * After `gdt_init` runs, the running CS is KERNEL_CS and all data segment
 * registers are KERNEL_DS.  The selectors below are the values you'd put
 * in a segment register or a far-jump target. */

#ifndef GDT_H
#define GDT_H

/* Segment selectors.  Low two bits are the Requested Privilege Level.
 * The index into the GDT is (selector >> 3). */
#define GDT_KERNEL_CS   0x08        /* entry 1 — kernel code, RPL 0 */
#define GDT_KERNEL_DS   0x10        /* entry 2 — kernel data, RPL 0 */
#define GDT_USER_CS     (0x18 | 3)  /* entry 3 — user code,   RPL 3 = 0x1B */
#define GDT_USER_DS     (0x20 | 3)  /* entry 4 — user data,   RPL 3 = 0x23 */
#define GDT_TSS         0x28        /* entry 5 — TSS descriptor */

/* Build the table in memory and load it via `lgdt`, then reload every
 * segment register so the new descriptors take effect.  Idempotent — safe
 * to call more than once, though today we only call it from kernel_main. */
void gdt_init(void);

/* Get the GDTR struct (6 bytes: 2-byte limit + 4-byte base).  Used by
 * the AP boot trampoline (M18) so the AP can lgdt the same table. */
void* gdt_get_ptr_struct(void);

/* Load the running CPU's task register with its own per-CPU TSS descriptor
 * (SMP).  The BSP calls this from gdt_init; each AP calls it from ap_main
 * once percpu is up.  Without it, a ring-3 → ring-0 trap on that CPU has no
 * valid kernel stack. */
void gdt_load_cpu_tss(void);

#endif
