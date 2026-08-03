/* =============================================================================
 * pthreadtest.c — REAL musl pthreads on d-os (§M40).
 *
 * Not our in-tree thread_create: this uses the pthread API, which means musl's
 * own pthread_create → clone(CLONE_VM|CLONE_THREAD|…) and pthread_join → a
 * futex wait on the kernel-cleared child tid.  Every toolkit and Mesa itself is
 * built on these, so "threads work" has to mean THESE threads.
 * ============================================================================= */

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

#define NTHREADS 4
#define NITER    5000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *worker(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < NITER; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    printf("pthreadtest: thread %ld done\n", id);
    return (void *)(id * 10);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t t[NTHREADS];

    printf("pthreadtest: creating %d pthreads x %d locked increments\n",
           NTHREADS, NITER);
    for (long i = 0; i < NTHREADS; i++) {
        if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) {
            printf("pthreadtest: pthread_create %ld FAILED\n", i);
            return 1;
        }
    }
    int joined = 0;
    for (int i = 0; i < NTHREADS; i++) {
        void *ret = NULL;
        /* The join is the real test of CLONE_CHILD_CLEARTID: musl parks on a
         * futex at the child tid address and only the kernel can release it. */
        if (pthread_join(t[i], &ret) == 0) joined++;
        printf("pthreadtest: joined %d -> %ld\n", i, (long)ret);
    }
    printf("pthreadtest: counter=%ld (expected %d), joined=%d -> %s\n",
           counter, NTHREADS * NITER, joined,
           (counter == NTHREADS * NITER && joined == NTHREADS) ? "PASS" : "FAIL");
    return 0;
}
