/* =============================================================================
 * crash.h — crash records and pluggable crash SINKS (§M47).
 *
 * THE REQUIREMENT THIS SERVES
 * ---------------------------
 * A faulty program must never take the machine down, and when something does
 * go wrong the system must SAY SO — recorded now, surfaced to the user later
 * (or immediately).  M46 delivered the first half for the cases where a fault
 * handler still runs.  This file delivers the second half, and does it in a way
 * that lets ANY notification mechanism be armed later — a GUI popup, a file, a
 * network report — WITHOUT touching a single fault path again.
 *
 * TWO PHASES, ON PURPOSE
 * ----------------------
 * Capture and delivery are deliberately separated, because they have opposite
 * constraints:
 *
 *   CAPTURE (crash_report) runs in the worst context in the system: inside an
 *   exception or NMI handler, IRQs off, possibly on a broken stack, possibly
 *   with the heap or a lock in an inconsistent state.  So it does the absolute
 *   minimum — copy a fixed-size record into a static ring and bump a counter.
 *   No allocation, no locks, no formatting, no I/O.
 *
 *   DELIVERY (crash_drain) runs later from an ordinary task, where a sink may
 *   allocate, block, draw a window or open a file.  A sink that hangs delays
 *   only the reporting, never the fault.
 *
 * That split is the whole design.  A popup sink is a normal piece of GUI code
 * precisely because it never runs in fault context.
 *
 * THE ONE FAILURE THIS CANNOT CAPTURE
 * -----------------------------------
 * A triple fault (or a power loss) resets the CPU with no handler running at
 * all — nothing in this file can execute.  That case is covered from the OTHER
 * side: crash_boot_begin/crash_boot_clean maintain an "unclean shutdown"
 * marker, so the NEXT boot reports that the previous one died silently.  It is
 * the only way such an event can ever reach the user, which is exactly why the
 * marker exists.
 * ============================================================================= */

#ifndef CRASH_H
#define CRASH_H

#include <stdint.h>
#include <stddef.h>

/* What went wrong.  Keep the list small and meaningful — a sink renders these
 * to the user, so each value must answer "what happened" on its own. */
enum crash_kind {
    CRASH_USER_FAULT   = 0,  /* ring-3 exception — the process was killed      */
    CRASH_KERNEL_FAULT = 1,  /* ring-0 exception — kernel.fault_policy applied */
    CRASH_HARD_LOCKUP  = 2,  /* NMI watchdog: IRQs-off spin/halt               */
    CRASH_TASK_HANG    = 3,  /* L1 watchdog: a task missed its heartbeat       */
    CRASH_DEADLOCK     = 4,  /* a spinlock spun past the sanity threshold      */
    CRASH_FORCED_KILL  = 5,  /* a wedged task was reclaimed by force           */
    CRASH_UNCLEAN_BOOT = 6,  /* the PREVIOUS boot ended without a clean exit   */
    CRASH_KIND_MAX     = 7,
};

#define CRASH_COMM_MAX  24
#define CRASH_WHAT_MAX  56

struct crash_record {
    uint32_t  seq;                    /* monotonic; 0 = empty slot            */
    uint64_t  ms;                     /* uptime in ms at capture              */
    uint8_t   kind;                   /* enum crash_kind                      */
    uint8_t   cpu;                    /* which CPU captured it                */
    int       pid;                    /* faulting task, or -1                 */
    int       code;                   /* signal / exception number            */
    uintptr_t pc;                     /* instruction pointer, or 0            */
    uintptr_t addr;                   /* faulting address, or 0               */
    char      comm[CRASH_COMM_MAX];   /* task name                            */
    char      what[CRASH_WHAT_MAX];   /* short human summary                  */
};

/* ---------------------------------------------------------------------------
 * Capture — callable from ANY context, including an exception or NMI handler.
 * Never allocates, never takes a lock, never returns an error: losing the
 * record would defeat the point, so the ring simply overwrites its oldest
 * entry.  `comm`/`what` may be NULL.
 * ------------------------------------------------------------------------- */
void crash_report(int kind, int pid, const char* comm,
                  uintptr_t pc, uintptr_t addr, int code, const char* what);

/* ---------------------------------------------------------------------------
 * Sinks — where crash records are DELIVERED.  Register one and every future
 * crash reaches it; no fault-path change is ever needed to add a destination.
 * `emit` runs from an ordinary task (see the two-phase note above), so it may
 * allocate, block, write files or draw.
 *
 * A sink is expected to be idempotent-ish and cheap to fail: if it cannot
 * deliver, it should give up quietly rather than block the drain for others.
 * ------------------------------------------------------------------------- */
struct crash_sink {
    const char* name;
    void      (*emit)(const struct crash_record* r);
};

/* Static registration, same linker-section pattern as DRIVER()/SERVICE(). */
#define CRASH_SINK(_name, _emitfn)                                        \
    static const struct crash_sink                                        \
    __attribute__((used, section("crashsinks"), aligned(4)))              \
    __crashsink_##_emitfn = { .name = (_name), .emit = (_emitfn) }

/* Deliver every not-yet-delivered record to all sinks.  Called periodically
 * from a normal task (the watchdog sweep); returns how many it delivered. */
int  crash_drain(void);

/* ---------------------------------------------------------------------------
 * Introspection — the `crash` shell command and /proc/crash read these.
 * ------------------------------------------------------------------------- */
void crash_init(void);                               /* registers /proc/crash */
int  crash_count(void);                              /* records captured ever */
const struct crash_record* crash_at(int i);          /* newest-first, or NULL */
const char* crash_kind_name(int kind);

/* ---------------------------------------------------------------------------
 * Unclean-shutdown detection — the only way a triple fault or a power loss can
 * ever be reported, since no code runs at the moment it happens.
 *
 *   crash_boot_begin()  early in boot: reports an unclean PREVIOUS boot if the
 *                       marker survived, then arms the marker for this boot.
 *   crash_boot_clean()  on an orderly shutdown/reboot: disarms it.
 * ------------------------------------------------------------------------- */
void crash_boot_begin(void);
void crash_boot_clean(void);

#endif /* CRASH_H */
