/* =============================================================================
 * waitq.h — wait-queue: race-free block / wake for kernel tasks (Tier A.1).
 *
 * Until now `TASK_SLEEPING` was an inert enum value: nothing ever put a task
 * into it, and there was no way for a task to give up the CPU *until an event*
 * (as opposed to task_yield's "round-robin, come back next tick" or task_exit's
 * "never come back").  Every blocking-looking path in the kernel actually
 * POLLED — the init reaper spins on hlt+yield, a would-be blocking read would
 * have to busy-wait.  A wait-queue is the missing primitive that makes
 * `TASK_SLEEPING` real: a task parks itself on a queue, is fully off every
 * runqueue (consuming zero CPU), and a producer wakes it when the awaited
 * condition becomes true.
 *
 * ---------------------------------------------------------------------------
 * The lost-wakeup problem, and how this API closes it
 * ---------------------------------------------------------------------------
 *
 * The classic race:
 *
 *      consumer                         producer
 *      --------                         --------
 *      if (!ready)                       ready = 1;
 *          // <-- producer runs here     wake();   // no one parked yet -> no-op
 *          block();                      // consumer sleeps forever
 *
 * The fix is the condition-variable discipline: the SAME lock guards both the
 * condition and the waitq, and each side holds it across check-then-block /
 * mutate-then-wake.  This waitq's own `lock` IS that lock — take it with
 * waitq_lock, and by convention mutate/check the guarded condition under it:
 *
 *      // consumer                       // producer
 *      f = waitq_lock(&wq);              f = waitq_lock(&wq);
 *      while (!ready)                    ready = 1;
 *          waitq_block(&wq);             waitq_wake_all(&wq);
 *      ... consume ...                   waitq_unlock(&wq, f);
 *      waitq_unlock(&wq, f);
 *
 * waitq_block atomically (w.r.t. a waker) parks the caller and DROPS the lock,
 * then re-acquires it before returning — exactly pthread_cond_wait's contract.
 * Because the producer can only take the lock after the consumer has both
 * registered on the queue AND marked itself SLEEPING, no wakeup is ever lost.
 *
 * ---------------------------------------------------------------------------
 * SMP
 * ---------------------------------------------------------------------------
 *
 * A woken task is re-enqueued via the normal scheduler path (affinity-aware
 * CPU pick); if it lands on another core a reschedule IPI is sent, so a task
 * blocked on CPU A can be woken by a producer on CPU B and resume promptly on
 * whichever core the load balancer prefers.  Interrupts are held off across
 * the whole park sequence, so a same-CPU timer IRQ cannot wedge the tiny
 * unlock->context-switch window.
 *
 * Implemented in task.c (it needs the scheduler internals: this_cpu, the
 * per-CPU runqueue remove path, and schedule()).
 * ============================================================================= */

#ifndef WAITQ_H
#define WAITQ_H

#include <stdint.h>
#include "lock.h"

struct task;   /* intrusive queue via task->wq_next — see task.h */

struct waitq {
    spinlock_t   lock;   /* guards the queue AND, by convention, the condition */
    struct task* head;   /* singly-linked list of parked tasks, NULL = empty   */
};

/* Static initialiser for a file-scope / embedded waitq. */
#define WAITQ_INIT { SPINLOCK_INIT, 0 }

/* Runtime initialiser (for heap-allocated waitqs). */
void waitq_init(struct waitq* wq);

/* Acquire / release the waitq lock.  This lock also serialises the guarded
 * condition (see header): check/mutate the condition only while holding it.
 * waitq_lock returns saved IRQ flags; pass them verbatim to waitq_unlock. */
uint32_t waitq_lock(struct waitq* wq);
void     waitq_unlock(struct waitq* wq, uint32_t flags);

/* Block the current task on `wq`.  MUST be called with the waitq locked (via
 * waitq_lock).  Atomically parks the caller and drops the lock; re-acquires
 * the lock before returning, with interrupts still masked to the locked
 * state.  ALWAYS loop on the condition:
 *
 *      uint32_t f = waitq_lock(&wq);
 *      while (!condition)
 *          waitq_block(&wq);
 *      waitq_unlock(&wq, f);
 *
 * A spurious return (woken but condition still false) is harmless — the loop
 * re-blocks.  Never call from an IRQ handler or the idle task. */
void waitq_block(struct waitq* wq);

/* Wake the first / all parked task(s).  MUST be called with the waitq locked.
 * Woken tasks become RUNNABLE and are re-enqueued (cross-CPU IPI if they land
 * on another core).  Waking an empty queue is a no-op. */
void waitq_wake_one(struct waitq* wq);
void waitq_wake_all(struct waitq* wq);

#endif
