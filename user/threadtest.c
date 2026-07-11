/* =============================================================================
 * threadtest.c — threads + a futex mutex (M35).
 *
 * Spawns several threads that all increment one shared counter under a
 * futex-backed mutex.  A correct total proves: threads share the address space
 * (they see the same `counter`/`mtx`), the futex mutex serialises them, and
 * thread_join() (waitpid on the tid) reaps each thread.  Runs correctly on SMP
 * (the mutex is a locked xchg + a kernel futex wait-queue).
 * ============================================================================= */

#include "libc.h"

#define NTHREADS 4
#define NITER    5000

static volatile int counter = 0;
static volatile int mtx     = 0;      /* 0=unlocked, 1=locked, 2=locked+waiters */

/* The 3-state (Drepper) futex mutex: the uncontended lock/unlock path is a
 * single atomic op with NO syscall; we only trap to the kernel on contention
 * (and unlock only wakes when a waiter is registered — state 2).  This keeps
 * futex traffic — and the cross-CPU wakeups it triggers — proportional to real
 * contention rather than to every acquire. */
static void mlock(volatile int* m) {
    int c = __sync_val_compare_and_swap(m, 0, 1);   /* try 0 → 1 */
    if (c != 0) {
        if (c != 2) c = __sync_lock_test_and_set(m, 2);  /* mark contended */
        while (c != 0) {
            futex((int*)m, FUTEX_WAIT, 2);          /* park while held+contended */
            c = __sync_lock_test_and_set(m, 2);
        }
    }
}
static void munlock(volatile int* m) {
    if (__sync_fetch_and_sub(m, 1) != 1) {          /* was 2 → had waiters */
        __sync_lock_release(m);                      /* store 0 */
        futex((int*)m, FUTEX_WAKE, 1);
    }
}

static int worker(void* arg) {
    (void)arg;
    for (int i = 0; i < NITER; i++) {
        mlock(&mtx);
        counter++;
        munlock(&mtx);
    }
    return 0;
}

int main(void) {
    int tid[NTHREADS];
    printf("threads: spawning %d threads x %d increments\n", NTHREADS, NITER);
    for (int i = 0; i < NTHREADS; i++) {
        tid[i] = thread_create(worker, 0);
        if (tid[i] < 0) printf("threads: thread_create %d failed\n", i);
    }
    for (int i = 0; i < NTHREADS; i++) thread_join(tid[i]);

    int expected = NTHREADS * NITER;
    printf("threads: counter=%d (expected %d) %s\n",
           counter, expected, counter == expected ? "PASS" : "FAIL");
    return counter == expected ? 0 : 1;
}
