/* =============================================================================
 * smp.h — symmetric multiprocessing bring-up (M18).
 *
 * `smp_boot_aps` walks the ACPI MADT's CPU list and brings every AP
 * online via the Intel-mandated INIT + SIPI sequence.  Each AP, once
 * running, marks itself online in the percpu table and enters its
 * idle loop.
 *
 * Pre-requisites (all called from kernel_main in boot order):
 *   - acpi_init           — for the CPU list
 *   - lapic_init_bsp      — so we can send IPIs
 *   - ioapic_init         — not strictly required but the order in
 *                            kernel_main has it before this
 *   - idt_use_apic        — APs share the kernel IDT; APIC mode active
 *   - percpu_init_bsp     — so percpu_at(i) returns valid slots
 * ============================================================================= */

#ifndef SMP_H
#define SMP_H

/* Bring up every application processor listed in MADT.  Logs each AP
 * as it comes online.  Returns the number of APs successfully started
 * (NOT counting the BSP). */
int smp_boot_aps(void);

/* Stash the BSP-calibrated LAPIC timer count.  Every AP reads this
 * value to program its own LAPIC timer in periodic mode.  Must be
 * set before smp_boot_aps; passing 0 disables the AP timer (APs
 * boot online but never preempt-tick). */
#include <stdint.h>
void smp_set_lapic_timer_count(uint32_t count);

/* M18.6.4 — cross-CPU "please reschedule" request.
 *
 * Sends LAPIC IPI vector 0x41 to the target CPU's LAPIC.  The IDT
 * handler for 0x41 calls schedule_check() on receipt, so the target
 * picks any newly-runnable task without waiting up to ~10 ms for its
 * own LAPIC timer to tick.
 *
 * No-ops if `cpu_index` is THIS CPU (we'd just delay our own
 * scheduling — schedule_request() is the right local primitive) or
 * out of range.  Safe to call from IRQ context; the LAPIC ICR write
 * doesn't block on the receiver.
 *
 * Use cases:
 *   - task_set_runnable on a task whose home CPU isn't us — wake the
 *     target so it sees the work right away.
 *   - load_balance result: kick a CPU that just received a stolen
 *     task. */
void smp_send_reschedule(int cpu_index);

#endif
