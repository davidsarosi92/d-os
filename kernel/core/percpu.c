/* =============================================================================
 * percpu.c — per-CPU storage (M18).
 *
 * State layout:
 *
 *   percpu_table[0..ncpus-1]   — one struct percpu per CPU
 *   apic_to_index[apic_id]     — sparse-to-dense LAPIC ID → slot map
 *
 * Why a separate map: ACPI MADT lists APIC IDs in the firmware's
 * enumeration order (e.g. 0, 1, 2, 3 on QEMU but can be 0, 2, 4, 6
 * on other boards).  We want a dense logical 0..ncpus-1 index for
 * indexing arrays without bounds-check pain.  256-entry lookup is
 * O(1), one cacheline.
 *
 * BSP init records its own APIC ID at slot 0 (BSP is conventionally
 * first in MADT enumeration); APs call `percpu_init_ap` and look up
 * the slot whose apic_id matches theirs.
 * ============================================================================= */

#include "percpu.h"
#include "acpi.h"
#include "lapic.h"
#include "printf.h"
#include <stddef.h>

static struct percpu percpu_table[ACPI_MAX_CPUS];
static int           ncpus = 1;                 /* BSP-only until percpu_init_bsp runs */
static uint8_t       apic_to_index[256];         /* 0xFF = unmapped */
static int           ready  = 0;                 /* set when lookup table is valid */

void percpu_init_bsp(void) {
    /* Initialize the table from ACPI MADT.  If ACPI didn't surface a
     * MADT (very old firmware or `-no-acpi`), fall back to single-CPU
     * UP semantics: one slot, BSP, lapic_id() reads-back-zero on a
     * not-yet-mapped LAPIC. */
    int n = acpi_ncpus();
    if (n <= 0) n = 1;
    if (n > ACPI_MAX_CPUS) n = ACPI_MAX_CPUS;
    ncpus = n;

    for (int i = 0; i < 256; i++) apic_to_index[i] = 0xFF;

    for (int i = 0; i < ncpus; i++) {
        uint8_t aid = acpi_cpu_apic_id(i);
        percpu_table[i].apic_id   = aid;
        percpu_table[i].cpu_index = i;
        /* M19.5.3 — record the SRAT-derived NUMA node (0 if no SRAT). */
        percpu_table[i].numa_node = acpi_cpu_node(i);
        /* DO NOT zero `current` / `online` / `ticks` here.  task_init
         * runs BEFORE percpu_init_bsp in the boot order and has
         * already stamped slot 0's current with pid 0 (via the
         * implicit zero-LAPIC-ID code path).  Wiping it would leave
         * the scheduler with prev=NULL and no task would ever run.
         * BSS zero-init already covers the slots APs will populate. */
        if (aid != 0xFF) apic_to_index[aid] = (uint8_t)i;
    }

    /* BSP is whatever LAPIC ID we are right now. */
    uint8_t bsp_aid = lapic_id();
    if (apic_to_index[bsp_aid] == 0xFF) {
        /* BSP not in MADT (shouldn't happen but be defensive) — pin
         * it at slot 0. */
        apic_to_index[bsp_aid]   = 0;
        percpu_table[0].apic_id  = bsp_aid;
    }
    int bsp_idx = apic_to_index[bsp_aid];
    percpu_table[bsp_idx].online = 1;

    ready = 1;
    kprintf("percpu: %d CPUs known, BSP at slot %d (apic_id=%u)\n",
            ncpus, bsp_idx, bsp_aid);
}

void percpu_init_ap(void) {
    if (!ready) return;
    uint8_t aid = lapic_id();
    int idx = apic_to_index[aid];
    if (idx == 0xFF || idx >= ncpus) return;     /* unknown CPU */
    percpu_table[idx].online = 1;
}

int this_cpu_id(void) {
    if (!ready) return 0;
    uint8_t aid = lapic_id();
    uint8_t idx = apic_to_index[aid];
    return (idx == 0xFF) ? 0 : idx;
}

struct percpu* this_cpu(void) {
    return &percpu_table[this_cpu_id()];
}

struct percpu* percpu_at(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= ncpus) return NULL;
    return &percpu_table[cpu_index];
}

int smp_ncpus(void) {
    return ncpus;
}
