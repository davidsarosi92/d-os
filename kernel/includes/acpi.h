/* acpi.h — ACPI table discovery and soft-off interface.
 *
 * `acpi_init` is a one-shot at boot that walks RSDP → RSDT → FADT → DSDT,
 * caches the PM1 control register addresses, and parses the `_S5_` object
 * from AML.  `acpi_shutdown` then writes the appropriate values to those
 * registers to trigger ACPI sleep state 5 (soft power-off).
 *
 * When ACPI is unavailable (e.g. `qemu -no-acpi`, or a BIOS without ACPI),
 * `acpi_init` returns -1 and leaves the module inactive.  In that state
 * `acpi_shutdown` silently does nothing, and callers (specifically
 * `hal_shutdown`) can fall back to other mechanisms. */

#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

/* Discover and cache ACPI tables.  Returns 0 on success, -1 on any failure. */
int acpi_init(void);

/* Trigger ACPI S5.  No effect if acpi_init failed or the DSDT had no
 * parseable _S5_ object.  Caller must have a fallback plan. */
void acpi_shutdown(void);

#endif
