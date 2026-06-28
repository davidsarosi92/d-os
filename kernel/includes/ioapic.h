/* =============================================================================
 * ioapic.h — I/O APIC driver interface.
 *
 * The IOAPIC is the chipset-side half of APIC IRQ delivery: it accepts
 * the platform's IRQ lines (ISA IRQ 0..15, plus PCI / MSI sources via
 * higher GSIs) and forwards them as messages to one or more LAPICs.
 *
 * In M18 we use one IOAPIC and route every legacy IRQ to the BSP.
 * Multi-CPU IRQ distribution is a follow-up.
 *
 * After `ioapic_init` and per-IRQ `ioapic_route` calls, the 8259 PIC
 * MUST be masked off (otherwise both old and new interrupt paths fire
 * the same IDT vector and the handler runs twice).  See ioapic.c for
 * the mechanical disable sequence.
 * ============================================================================= */

#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

/* Map the IOAPIC MMIO at `phys` and record the GSI base.  Caches the
 * "max redirection entries" count from the version register so route
 * calls can bounds-check. */
int  ioapic_init(uint32_t phys, uint32_t gsi_base);

/* Route a legacy ISA IRQ to (vector, dest_apic_id).  Honors the ACPI
 * Interrupt Source Override table — if the firmware remapped IRQ N to
 * a different GSI (very common: IRQ0 → GSI 2), we follow.  Edge-
 * triggered + active-high by default; ISO flags override. */
int  ioapic_route_isa(int isa_irq, uint8_t vector, uint8_t dest_apic_id);

/* Mask / unmask a redirection entry directly by GSI. */
void ioapic_mask_gsi(uint32_t gsi, int masked);

#endif
