/* =============================================================================
 * percpu.h — per-CPU storage interface (M18).
 *
 * Each CPU has its own slot of mutable state: currently running task,
 * idle-task pointer, per-CPU stats.  The slot is indexed by CPU
 * number (0..smp_ncpus()-1), NOT by LAPIC ID — those can be sparse
 * (e.g. 0, 1, 4, 5 if the firmware skipped IDs).
 *
 * `this_cpu_id` returns the logical index of the running CPU.  Its
 * implementation reads LAPIC ID (cheap — one MMIO load) and translates
 * via a small mapping table populated during SMP discovery.  Until
 * LAPIC is up, it returns 0 (BSP).
 *
 * On UP (single-CPU build), every call returns the BSP's slot.  The
 * shape doesn't change: code written against this API runs unmodified
 * once AP boot lands.
 * ============================================================================= */

#ifndef PERCPU_H
#define PERCPU_H

#include <stdint.h>

struct task;   /* fwd; defined in task.h */

/* Slot of per-CPU state.  Kept small — adding fields is fine, but
 * sharing a cache line across CPUs is fine too on 32 boards; we
 * pad to 64 bytes only when contention shows up. */
struct percpu {
    uint8_t      apic_id;          /* this CPU's LAPIC ID */
    int          cpu_index;        /* same as the array index */
    int          online;           /* 1 once the CPU has finished its init */
    struct task* current;          /* M18 — per-CPU current-task pointer */
    struct task* idle;             /* per-CPU idle task; never DEAD */
    uint64_t     ticks;            /* CPU-local tick counter (diagnostics) */
};

/* Bring up the per-CPU table on the BSP.  Records the BSP's APIC ID
 * (so this_cpu_id works before any AP boots) and pre-sizes the table
 * to ACPI's CPU count.  Must run after acpi_init + lapic_init_bsp. */
void percpu_init_bsp(void);

/* APs call this from their C entry to register themselves in the
 * table.  Reads its own LAPIC ID, maps it to the right slot, and
 * marks itself online. */
void percpu_init_ap(void);

/* Logical CPU index of the calling CPU (0..smp_ncpus()-1).  Reads
 * LAPIC ID and consults the mapping table; returns 0 before percpu
 * is initialized. */
int  this_cpu_id(void);

/* The per-CPU slot for the calling CPU. */
struct percpu* this_cpu(void);

/* Lookup by logical index — used by diag (`lscpu`) and AP launcher. */
struct percpu* percpu_at(int cpu_index);

/* Number of CPUs the kernel knows about (recorded from MADT at BSP
 * init time; not "number online" — see `online` field for that). */
int  smp_ncpus(void);

#endif
