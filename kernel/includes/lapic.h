/* =============================================================================
 * lapic.h — Local APIC interface.
 *
 * One LAPIC per CPU; the BSP's LAPIC is the same hardware accessed
 * through the same MMIO window (each CPU sees ITS own LAPIC at that
 * address — the chipset routes the read/write to the local unit).
 *
 * M18 first cut uses it for:
 *   - IRQ delivery (after IOAPIC takes over from the 8259)
 *   - EOI acknowledgement (`lapic_eoi`)
 *   - Inter-Processor Interrupts for AP bring-up (INIT + SIPI)
 *
 * IRQ handlers reach `lapic_eoi` instead of the legacy `pic_eoi` once
 * IOAPIC routing is active.  Vector layout stays compatible with the
 * IDT we already have: IRQ N → vector 0x20+N.
 * ============================================================================= */

#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

/* Map the LAPIC's MMIO window at `phys` and enable the local unit
 * (SIVR.APIC_EN = 1).  Must be called once on the BSP before IOAPIC
 * routing is activated.  Idempotent across CPUs — APs call the
 * "enable-only" variant once they're up. */
int  lapic_init_bsp(uint32_t phys);

/* Per-CPU APIC-enable for an AP (assumes lapic_init_bsp already
 * mapped the MMIO window).  Sets SIVR.APIC_EN, masks LVT lines. */
void lapic_init_ap(void);

/* Acknowledge the current IRQ to the LAPIC.  Replaces pic_eoi for
 * vectors delivered via IOAPIC. */
void lapic_eoi(void);

/* This CPU's APIC ID — read from LAPIC ID register (offset 0x20). */
uint8_t lapic_id(void);

/* Send INIT IPI to the target APIC ID (used by AP bring-up). */
void lapic_send_init(uint8_t target_apic_id);

/* Send Startup IPI carrying a vector that encodes the trampoline
 * physical address: trampoline must be at (vector << 12).  Used twice
 * per AP per the Intel-recommended SIPI sequence. */
void lapic_send_sipi(uint8_t target_apic_id, uint8_t vector);

#endif
