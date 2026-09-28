/* =============================================================================
 * futex.c — the fast userspace mutex primitive (M35).
 *
 * A futex is the one syscall every threading library needs: FUTEX_WAIT parks
 * the caller iff *uaddr still equals the expected value (the atomic check that
 * closes the lost-wakeup race); FUTEX_WAKE wakes parked waiters.  User code
 * does the uncontended fast path with an atomic op and only traps to the kernel
 * on contention.
 *
 * Keying: waiters are hashed (by the *physical* address of `uaddr`, so it works
 * for threads that share the mapping and, later, for cross-process shared
 * memory) into a small fixed set of Tier-A wait-queues.  Distinct addresses may
 * collide into one bucket → a FUTEX_WAKE there wakes unrelated waiters too;
 * that is harmless because every waiter re-checks its own *uaddr and re-parks
 * if nothing changed (futex semantics explicitly allow spurious wakeups).  For
 * that reason FUTEX_WAKE wakes the whole bucket.
 *
 * The BSS-zeroed wait-queue array is already valid (SPINLOCK_INIT == {0},
 * head == NULL), so no init call is needed.
 * ============================================================================= */

#include "syscall.h"
#include "waitq.h"
#include "vmm.h"
#include "task.h"
#include "ktimer.h"
#include "timer.h"
#include "futex.h"
#include "uaccess.h"
#include <stdint.h>

/* §M89 — KEYED BY (address space, virtual address), not by physical address.
 * The physical key came from vmm_translate(), which on x86_64 walks the
 * KERNEL's tables, not the calling process's — so for every user address it
 * answered "unmapped" and FUTEX_WAIT failed at once, on every call, since M35.
 * Nothing broke visibly because musl re-checks and spins: threads WORKED, and
 * a thread waiting for another burned a whole CPU doing it (a JVM's main
 * thread made 3.6 million futex calls waiting for its first child).  A
 * virtual key is also what makes a demand-zero page (§M89 rung 1) usable as a
 * futex before it has a frame.  The cost, stated: two PROCESSES sharing a
 * futex through shared memory at DIFFERENT addresses would not find each
 * other (Linux keys shared futexes by the backing page); nothing here does
 * that today. */
#define FUTEX_NBUCKETS 64
static struct waitq g_futex[FUTEX_NBUCKETS];

static struct waitq* bucket_of(uintptr_t uaddr) {
    struct task* t = task_current();
    uintptr_t k = uaddr ^ ((uintptr_t)(t ? (void*)t->mm : 0) >> 4);
    return &g_futex[((k >> 2) ^ (k >> 11)) % FUTEX_NBUCKETS];
}

static void futex_deadline_fired(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t f = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, f);
}

long futex_wait(uintptr_t uaddr, uint32_t val, uint64_t deadline_ns) {
    if (!uaddr || (uaddr & 3)) return -FUTEX_EINVAL;
    /* Bring the page in (it may be demand-zero) BEFORE the lock: the read
     * below must not have to sleep. */
    if (!vmm_user_access_ok(uaddr, 4, 0)) return -FUTEX_EFAULT;
    struct waitq* wq = bucket_of(uaddr);
    struct ktimer tm = { 0, 0, 0, 0, 0 };
    if (deadline_ns) {
        if (timer_now_ns() >= deadline_ns) return -FUTEX_ETIMEDOUT;
        ktimer_arm(&tm, deadline_ns, futex_deadline_fired, wq);
    }
    uint32_t f = waitq_lock(wq);
    uint32_t cur;
    /* A fault-safe read: under the lock nothing may sleep, and a page that
     * went away since the check becomes -EFAULT instead of a kernel fault. */
    if (uaccess_copy_in(&cur, uaddr, 4) != 0) {
        waitq_unlock(wq, f);
        if (deadline_ns) ktimer_cancel(&tm);
        return -FUTEX_EFAULT;
    }
    if (cur != val) {
        waitq_unlock(wq, f);
        if (deadline_ns) ktimer_cancel(&tm);
        return -FUTEX_EAGAIN;
    }
    waitq_block(wq);                  /* drops + re-acquires the lock */
    waitq_unlock(wq, f);
    if (deadline_ns) ktimer_cancel(&tm);
    /* Any wake returns 0 (futex semantics allow spurious ones — the caller
     * re-checks); only a deadline or a signal says otherwise. */
    if (deadline_ns && timer_now_ns() >= deadline_ns) return -FUTEX_ETIMEDOUT;
    struct task* me = task_current();
    if (me && (me->sig_pending & ~me->sig_blocked)) return -FUTEX_EINTR;
    return 0;
}

long futex_wake(uintptr_t uaddr, int count) {
    if (!uaddr) return -FUTEX_EINVAL;
    struct waitq* wq = bucket_of(uaddr);
    /* The whole bucket: a collision wakes unrelated waiters, who re-check and
     * re-park.  Reported as the count asked for (bounded by 1 when nobody can
     * be told apart), which is what callers use it for: "did anyone wake". */
    uint32_t f = waitq_lock(wq);
    int any = wq->head != 0;
    waitq_wake_all(wq);
    waitq_unlock(wq, f);
    return any ? (count > 0 ? 1 : 0) : 0;
}

/* The native M35 entry point, kept for the in-tree libc (op 0 / 1). */
long sys_futex(int* uaddr, int op, int val) {
    switch (op & FUTEX_CMD_MASK) {
    case FUTEX_WAIT: { long r = futex_wait((uintptr_t)uaddr, (uint32_t)val, 0); return r == -FUTEX_EAGAIN ? 0 : (r < 0 ? -1 : r); }
    case FUTEX_WAKE: return futex_wake((uintptr_t)uaddr, val) < 0 ? -1 : 0;
    default: return -1;
    }
}
