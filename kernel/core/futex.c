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
#include <stdint.h>

#define FUTEX_NBUCKETS 32
static struct waitq g_futex[FUTEX_NBUCKETS];

static int futex_bucket(uintptr_t phys) {
    /* Mix the (page-aligned-ish) physical address into a bucket index. */
    return (int)(((phys >> 2) ^ (phys >> 10)) % FUTEX_NBUCKETS);
}

long sys_futex(int* uaddr, int op, int val) {
    if (!uaddr) return -1;
    uintptr_t phys = vmm_translate((uintptr_t)uaddr);
    if (!phys) return -1;                       /* unmapped → EFAULT-ish */
    struct waitq* wq = &g_futex[futex_bucket(phys)];

    switch (op) {
        case FUTEX_WAIT: {
            /* Park iff *uaddr == val, re-checked under the queue lock so a
             * concurrent FUTEX_WAKE cannot slip between the test and the park
             * (the Tier-A lost-wakeup-free contract). */
            uint32_t f = waitq_lock(wq);
            if (*uaddr == val) waitq_block(wq);  /* drops + re-acquires the lock */
            waitq_unlock(wq, f);
            return 0;
        }
        case FUTEX_WAKE: {
            /* Wake the whole bucket (see the collision note above); waiters
             * whose condition is unchanged simply re-park. */
            uint32_t f = waitq_lock(wq);
            waitq_wake_all(wq);
            waitq_unlock(wq, f);
            return 0;
        }
        default:
            return -1;
    }
}
