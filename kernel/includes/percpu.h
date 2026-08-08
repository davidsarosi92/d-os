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
    /* §M49 — summed `demand` of the tasks queued here, i.e. how much CPU
     * this runqueue's contents actually WANT (100 = one core's worth).
     * This, not rq_count, is what the balancer and the spawn-time
     * placement compare: four hogs and four sleepers are the same
     * rq_count and nothing like the same load.  Maintained
     * incrementally under rq_lock on insert/remove, and recomputed from
     * scratch by the owning CPU at each balance tick (which is also when
     * the per-task demand figures are refreshed).  Read locklessly by
     * peers — a stale read costs one suboptimal steal decision. */
    int          rq_load;
    spinlock_t   rq_lock;          /* protects rq_head + rq_count + member rq_next/rq_prev */
    /* M18.6.1 — per-CPU deferred-reschedule flag.  Set by the local
     * timer IRQ handler and by cross-CPU preempt IPI (vector 0x41
     * handler in idt.c calls schedule_request, which now writes the
     * receiving CPU's slot).  Read+cleared by schedule_check on this
     * CPU only.  Pre-M18.6.1 this was a single global, which got
     * raced under SMP. */
    volatile int need_resched;
    /* --- Scheduler instrumentation (§M49).  Written by this CPU only, at
     * the context-switch boundary, so no lock and no atomics; read
     * locklessly by `sched` / /proc/sched, where a torn 64-bit read on a
     * 32-bit build costs a single wrong sample and nothing else.
     *
     * These exist because the load balancer could not be evaluated without
     * them.  `rq_count` says how many tasks are QUEUED, which is not the
     * same question as how much work a CPU is DOING (one hog saturates a
     * CPU with rq_count 1) — and a balancer is only as good as the metric
     * you judge it by.  `migrations` is the cost side of the ledger: a
     * balancer that keeps a queue even by shuttling the same task back and
     * forth every window is worse than no balancer at all, and without a
     * counter that failure mode is invisible. */
    uint64_t     last_balance_ms;  /* §M49 — when this CPU last ran a balance pass */
    uint64_t     busy_ms;          /* ms spent running a NON-idle task */
    uint64_t     switches;         /* context switches performed here */
    uint64_t     migrations;       /* tasks pulled onto this CPU by the balancer */

    /* --- TLB shootdown (§M51).  A ticket pair, not a message: a sender
     * bumps `tlb_req` on every CPU it needs to flush and waits for that
     * CPU's `tlb_ack` to catch up.  The request carries no address on
     * purpose — the remote action is always "flush everything", which is
     * unconditionally correct no matter how many senders overlap and
     * removes any need for a lock or a shared request slot.  Paying a
     * full flush instead of an `invlpg` costs performance on a path that
     * is already an IPI round trip.
     *
     * Monotonic counters, touched with atomics; `tlb_ack` is only ever
     * advanced AFTER the flush, so `ack >= my_ticket` means "the entries
     * I care about are gone from that CPU". */
    volatile uint32_t tlb_req;     /* bumped by whoever wants this CPU flushed */
    volatile uint32_t tlb_ack;     /* set to tlb_req by this CPU, after flushing */
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
