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
#include "lock.h"

struct task;   /* fwd; defined in task.h */

/* Slot of per-CPU state.  Kept small — adding fields is fine, but
 * sharing a cache line across CPUs is fine too on 32 boards; we
 * pad to 64 bytes only when contention shows up. */
struct percpu {
    uint8_t      apic_id;          /* this CPU's LAPIC ID */
    int          cpu_index;        /* same as the array index */
    int          numa_node;        /* M19.5.3 — NUMA node from SRAT (0 if no SRAT) */
    int          online;           /* 1 once the CPU has finished its init */
    struct task* current;          /* M18 — per-CPU current-task pointer */
    struct task* idle;             /* per-CPU idle task; never DEAD */
    uint64_t     ticks;            /* CPU-local tick counter (diagnostics) */
    /* M18.6.2 — per-CPU preemption counter.  The global preempt_count
     * (used pre-M18.6) was incorrect on SMP: disabling on CPU A would
     * ALSO suppress preemption on CPU B, which both starves B and
     * masks legitimate races.  preempt_count must be local to "do not
     * reschedule on THIS CPU."  Accessed only from this_cpu()'s slot,
     * so no atomics needed — IRQ-off bracketing in the accessors keeps
     * the read-modify-write coherent against the local timer IRQ. */
    int          preempt_count;    /* M18.6.2 */
    /* M18.6.1 — per-CPU runqueue (intrusive doubly-linked list of
     * RUNNABLE non-idle tasks plus their head).  Each CPU picks from
     * its own queue; the load balancer steals across queues every N
     * ticks.  `rq_count` excludes idle.  All access under rq_lock
     * (per-CPU). */
    struct task* rq_head;          /* first task in this CPU's runqueue, NULL = empty */
    int          rq_count;         /* count of non-idle RUNNABLE tasks queued here */
    spinlock_t   rq_lock;          /* protects rq_head + rq_count + member rq_next/rq_prev */
    /* M18.6.1 — per-CPU deferred-reschedule flag.  Set by the local
     * timer IRQ handler and by cross-CPU preempt IPI (vector 0x41
     * handler in idt.c calls schedule_request, which now writes the
     * receiving CPU's slot).  Read+cleared by schedule_check on this
     * CPU only.  Pre-M18.6.1 this was a single global, which got
     * raced under SMP. */
    volatile int need_resched;
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
