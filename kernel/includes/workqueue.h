/* =============================================================================
 * workqueue.h — deferred work executed on task context (§M49).
 *
 * WHAT THIS IS FOR.  Code that runs where it cannot do much — an interrupt
 * handler, a fault path, a spinlock-held region — but needs something done
 * that CAN block, allocate or draw.  It submits a `struct work` and returns;
 * a pool of ordinary kernel tasks runs the callback later, in a context with
 * no such restrictions.
 *
 * WHY A POOL AND NOT A SINGLE THREAD.  Because §M49 gave the scheduler a real
 * load balancer and a demand metric: the workers are plain tasks, so several
 * items submitted at once genuinely run on several cores, and the balancer
 * spreads them without the workqueue knowing anything about CPUs.  That is
 * the whole point of building this on top of the scheduler rather than beside
 * it.
 *
 * WHAT IT COSTS WHEN IDLE: nothing.  Workers block on a waitq, so they hold
 * no runqueue slot and register no demand — the property §M49 had to fix in
 * `task_msleep` and `vc_getchar` before it was true of anything.
 *
 * OWNERSHIP.  A `struct work` belongs to the submitter and must stay alive
 * until its callback has run.  Static or embedded-in-a-longer-lived-struct is
 * the intended shape; a stack-allocated work item is a use-after-free waiting
 * to happen unless the submitter `work_flush()`es before returning.
 *
 * RE-SUBMISSION.  Submitting an item that is already queued is a no-op that
 * reports success — the classic "one pending run is enough" semantic (a
 * driver signalling "there is data to drain" does not want N queued copies).
 * An item that is currently RUNNING can be re-queued: the callback may
 * legitimately ask to be run again for work that arrived while it ran.
 *
 * WHAT IS NOT HERE, DELIBERATELY.  No submission from NMI context.
 * `work_submit` takes a spinlock, and an NMI that interrupts a CPU already
 * holding it would deadlock.  §M47's crash capture is exactly such a caller
 * and keeps its own lock-free ring for that reason.  A future NMI-safe path
 * would need Linux's irq_work shape (per-CPU list + self-IPI), which is a
 * different mechanism, not a flag on this one.
 * ============================================================================= */

#ifndef WORKQUEUE_H
#define WORKQUEUE_H

#include <stdint.h>

struct work;

/* Callback signature.  Receives its own item so a caller can embed `struct
 * work` in a larger struct and recover it by offset (container-of style). */
typedef void (*work_fn)(struct work* w);

struct work {
    work_fn      fn;
    struct work* next;      /* queue link — workqueue-internal */
    int          queued;    /* 1 while waiting to run */
};

/* Prepare an item.  Safe to call again on an item that is not queued. */
void work_init(struct work* w, work_fn fn);

/* Bring the pool up: one worker task per CPU, parented to init so they
 * outlive whatever started them.  Idempotent.  Call after task_start_init. */
void workqueue_init(void);

/* Queue `w` for execution.  Returns 0 if queued (or already queued), -1 if
 * the item is malformed or the pool is not up yet.
 *
 * Safe from interrupt context (NOT from NMI — see the header comment).
 * Never blocks. */
int work_submit(struct work* w);

/* Block until everything submitted before this call has finished running.
 * Must NOT be called from a worker (it would wait on itself) nor from
 * interrupt context. */
void work_flush(void);

/* Diagnostics for `sched` / `wqtest`: how many workers exist, how many items
 * are waiting, and how many have completed since boot. */
void workqueue_stats(int* workers, int* pending, uint64_t* completed);

#endif
