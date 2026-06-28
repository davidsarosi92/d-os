/* acpi.h — ACPI table discovery, soft-off, MADT topology.
 *
 * `acpi_init` is a one-shot at boot that walks RSDP → RSDT → {FADT,
 * MADT}, caches the PM1 control register addresses, parses `_S5_`
 * from AML, and on the same pass enumerates the LAPIC + IOAPIC
 * topology from the MADT (M18).
 *
 * When ACPI is unavailable (e.g. `qemu -no-acpi`), `acpi_init`
 * returns -1 and leaves the module inactive.  In that state
 * `acpi_shutdown` silently does nothing and the SMP discovery
 * getters return zero CPU count — callers must have a fallback. */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* Cap on CPUs we record from MADT.  More can exist on real hardware;
 * we cap at a reasonable value to keep the table small. */
#define ACPI_MAX_CPUS 32

/* Discover and cache ACPI tables.  Returns 0 on success, -1 on any
 * failure (FADT-level — partial MADT failure leaves the SMP getters
 * at zero but does not fail the whole init). */
int acpi_init(void);

/* Trigger ACPI S5.  No effect if acpi_init failed or the DSDT had
 * no parseable _S5_ object. */
void acpi_shutdown(void);

/* ---------------------------------------------------------------------------
 * MADT-derived topology getters (M18).  Valid after `acpi_init` returns 0.
 *
 * Zero/NULL is returned in the absence of a MADT — callers should treat
 * that as "single-CPU, legacy 8259 PIC routing only" and fall back to
 * the pre-SMP IRQ path.
 * --------------------------------------------------------------------------- */

/* Physical address of the Local APIC MMIO window.  Typical value
 * 0xFEE00000.  Returns 0 if no MADT was found. */
uint32_t acpi_lapic_phys(void);

/* Number of enabled CPUs in the MADT.  Returns 0 if no MADT was found
 * (treat as 1 — the BSP). */
int acpi_ncpus(void);

/* APIC ID of the i-th enabled CPU in MADT enumeration order (i.e. as
 * listed by the firmware; the BSP is conventionally first but not
 * guaranteed).  Out-of-range i returns 0xFF. */
uint8_t acpi_cpu_apic_id(int i);

/* Physical address of the first IOAPIC.  Typical value 0xFEC00000.
 * Returns 0 if no IOAPIC was reported. */
uint32_t acpi_ioapic_phys(void);

/* The IOAPIC's Global System Interrupt base (most boards: 0). */
uint32_t acpi_ioapic_gsi_base(void);

/* For an ISA IRQ (0..15), look up any Interrupt Source Override the
 * MADT specifies.  Returns 1 if an override exists (fills *out_gsi
 * and *out_flags), 0 otherwise (legacy mapping = IRQ N → GSI N
 * applies). */
int acpi_irq_override(int isa_irq, uint32_t* out_gsi, uint16_t* out_flags);

#endif
