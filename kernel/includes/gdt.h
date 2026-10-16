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

#include <stdint.h>

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

/* M35 TLS — the ring-3 %gs selector for the running CPU's user-TLS descriptor
 * (RPL 3).  `hal_set_tls_base` (hal_api.h) rewrites that descriptor's base. */
uint16_t gdt_tls_selector(void);

/* NEXT #14 (2026-10-16) — BIND THE CALLING TASK'S TLS TO THE CPU IT IS ON, AS
 * ONE STEP.  The slot is per CPU (GDT_TLS_BASE + cpu), so "pin to this CPU",
 * "write this CPU's slot" and "the selector for this CPU" must all name the
 * SAME cpu.  They were three preemptible calls, each reading this_cpu_id()
 * afresh: a thread migrated between them wrote ANOTHER CPU's slot and entered
 * ring 3 with that CPU's selector while pinned somewhere else — and from then
 * on its %gs base was whatever thread owned that slot, i.e. it ran as another
 * thread (musl's __pthread_self()).  Interrupts are off across the three, so
 * the CPU cannot change under them.  `t` must be the CALLING task with
 * `t->tls_base` already set.  Returns the ring-3 selector (RPL 3). */
struct task;
uint16_t x86_tls_bind(struct task* t);

#endif
