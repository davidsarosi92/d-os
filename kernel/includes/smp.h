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

#endif
