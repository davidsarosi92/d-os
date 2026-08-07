/* =============================================================================
 * workqueue.c — deferred work on task context (§M49).  See workqueue.h for
 * the contract; this file is about how it is made safe.
 *
 * ONE LOCK.  The pending list and the workers' waitq share a single lock —
 * the waitq's own.  waitq.h's contract already says that lock "also
 * serialises the guarded condition", and here the condition IS the list, so
 * a second lock would only create an ordering question with no benefit.  It
 * also closes the lost-wakeup window for free: a worker tests the list and
 * parks while holding it, so a submitter's wake can never land between the
 * two.
 *
 * SUBMISSION FROM AN INTERRUPT.  `work_submit` takes that lock with
 * interrupts masked and never blocks, so a driver ISR can defer work.  Every
 * task-side path (worker loop, flush) uses the same masked acquire, so an
 * interrupt on the same CPU can never find the lock held by code it just
 * interrupted.
 * ============================================================================= */

#include "workqueue.h"
#include "waitq.h"
#include "task.h"
#include "percpu.h"
#include "printf.h"
#include <stddef.h>

/* Pending items, FIFO.  Guarded by `pending_wq.lock` (see the file comment). */
static struct waitq  pending_wq = WAITQ_INIT;
static struct work*  q_head = NULL;
static struct work*  q_tail = NULL;
static int           q_len  = 0;

/* Completion side: `inflight` counts items that are queued OR running, so a
 * flush waits for work to be FINISHED rather than merely dequeued.  Counting
 * only the queue would let flush return while a callback was still touching
 * the submitter's data — the exact bug flush exists to prevent. */
static struct waitq  done_wq = WAITQ_INIT;
static int           inflight  = 0;
static uint64_t      completed = 0;

static int           n_workers = 0;
static int           pool_up   = 0;

#define WQ_MAX_WORKERS 8

void work_init(struct work* w, work_fn fn) {
    if (!w) return;
    w->fn     = fn;
    w->next   = NULL;
    w->queued = 0;
}

int work_submit(struct work* w) {
    if (!w || !w->fn) return -1;
    if (!pool_up)     return -1;        /* no one would ever run it */

    uint32_t fl = waitq_lock(&pending_wq);
    if (w->queued) {                    /* already pending — one run is enough */
        waitq_unlock(&pending_wq, fl);
        return 0;
    }
    w->queued = 1;
    w->next   = NULL;
    if (q_tail) q_tail->next = w;
    else        q_head = w;
    q_tail = w;
    q_len++;
    inflight++;
    /* Wake exactly one worker: the item can only be run once, and waking the
     * whole pool for it would have every worker take the lock, find nothing
     * and park again — a thundering herd on every single submission. */
    waitq_wake_one(&pending_wq);
    waitq_unlock(&pending_wq, fl);
    return 0;
}

/* Pop the next item, or NULL.  Caller holds pending_wq.lock. */
static struct work* q_pop_locked(void) {
    struct work* w = q_head;
    if (!w) return NULL;
    q_head = w->next;
    if (!q_head) q_tail = NULL;
    w->next   = NULL;
    w->queued = 0;      /* cleared BEFORE the callback runs, so a callback
                         * may re-submit itself for work that arrived while
                         * it was running */
    q_len--;
    return w;
}

static void worker_main(void) {
    for (;;) {
        uint32_t fl = waitq_lock(&pending_wq);
        while (!q_head && !task_should_stop())
            waitq_block(&pending_wq);

        struct work* w = q_pop_locked();
        waitq_unlock(&pending_wq, fl);

        if (!w) {
            /* Woken with an empty queue => a kill is pending.  task_yield is
             * the documented cooperative kill point (it task_exit()s us). */
            task_yield();
            continue;
        }

        w->fn(w);

        /* Account the completion and release any flusher.  A flush waits on
         * `inflight` reaching zero, so this has to happen AFTER the callback
         * returns, not when the item was dequeued. */
        uint32_t f2 = waitq_lock(&done_wq);
        completed++;
        if (inflight > 0) inflight--;
        if (inflight == 0) waitq_wake_all(&done_wq);
        waitq_unlock(&done_wq, f2);
    }
}

void workqueue_init(void) {
    if (pool_up) return;                /* idempotent */
    waitq_init(&pending_wq);
    waitq_init(&done_wq);

    int want = smp_ncpus();
    if (want < 1)              want = 1;
    if (want > WQ_MAX_WORKERS) want = WQ_MAX_WORKERS;

    /* Mark the pool up BEFORE spawning: a worker that starts instantly on
     * another core is fine, but a submit racing the last spawn should be
     * accepted rather than rejected. */
    pool_up = 1;
    for (int i = 0; i < want; i++) {
        /* Detached: workers are system infrastructure and must not die with
         * whoever happened to bring the pool up. */
        struct task* t = task_spawn_detached("kworker", worker_main);
        if (!t) break;
        n_workers++;
    }
    if (n_workers == 0) { pool_up = 0; kprintf("workqueue: FAILED to spawn any worker\n"); return; }
    kprintf("workqueue: %d worker(s) ready\n", n_workers);
}

void work_flush(void) {
    if (!pool_up) return;
    uint32_t fl = waitq_lock(&done_wq);
    while (inflight > 0)
        waitq_block(&done_wq);
    waitq_unlock(&done_wq, fl);
}

void workqueue_stats(int* workers, int* pending, uint64_t* done) {
    if (workers) *workers = n_workers;
    if (pending) *pending = q_len;
    if (done)    *done    = completed;
}
