/* =============================================================================
 * kmutex.h — a lock you may SLEEP holding, and sleep waiting for (2026-09-25).
 *
 * Until now the kernel had spinlocks and nothing else, so code that must hold
 * a lock across a disk request — which sleeps, or at least ought to — could not
 * be locked at all, and was not: virtio-blk, the block cache, exFAT and the VFS
 * above them all ran unlocked.  `diskstorm` measured what that meant on a
 * 4-CPU machine: 17 wrong read-backs in 80 rounds, and `fsck.exfat` finding a
 * DIFFERENT file (a user's settings) with its cluster marked free.
 *
 * Semantics, all of them load-bearing:
 *   - one owner; others BLOCK on a waitq (they do not spin) — a disk request
 *     can take milliseconds and a spinning waiter would burn a CPU for all of
 *     them, the very defect the busy-waiting driver had;
 *   - RECURSIVE for the owner (a depth count): a filesystem operation calls
 *     another through the same entry points, and a mutex that deadlocks on its
 *     own holder turns every such path into a hang;
 *   - where sleeping is not allowed (before the scheduler, the idle task,
 *     preemption disabled) a waiter SPINS instead — correct, and in practice
 *     uncontended there: those contexts are boot and interrupt-adjacent code;
 *   - never from an interrupt handler.
 *
 * Lock order where several are held: filesystem -> block cache -> driver.
 * ============================================================================= */
#ifndef KMUTEX_H
#define KMUTEX_H

#include "waitq.h"

struct task;

struct kmutex {
    struct waitq  wq;        /* its lock guards owner + depth too          */
    struct task*  owner;     /* NULL = free                                */
    int           depth;     /* recursion depth for the owner              */
    const char*   name;      /* for diagnostics                            */
};

#define KMUTEX_INIT(n) { WAITQ_INIT, 0, 0, (n) }

void kmutex_init(struct kmutex* m, const char* name);
void kmutex_lock(struct kmutex* m);
void kmutex_unlock(struct kmutex* m);
int  kmutex_held_by_me(struct kmutex* m);

#endif
