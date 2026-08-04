/* =============================================================================
 * thrdyn.c — does a musl thread that EXITS survive in a DYNAMIC binary?
 *
 * `pthreadtest` already proves threads work — but it is a STATIC musl binary.
 * NetSurf is a dynamic PIE, and adding one worker thread there killed the
 * browser immediately after the worker returned, with a fault just below a page
 * boundary in the mmap region.  The two differ in more than linkage: a dynamic
 * program's threads are created after ld.so has run, its TLS comes from the
 * dynamic TLS blocks, and its stacks are mmap'd by a musl that also owns the
 * loader's own allocations.
 *
 * So this is the same shape as pthreadtest, built as a dynamic PIE, and nothing
 * else — the minimum needed to say whether "threads" or "threads in a dynamic
 * binary" is the broken thing.
 *
 * Three phases, each printed, so a crash localises itself:
 *   1. create + join a thread that returns immediately
 *   2. the same with a thread that allocates and touches memory
 *   3. several in sequence, to catch a leak that only bites on repeat
 * ============================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static void *trivial(void *arg)
{
    (void)arg;
    return NULL;
}

static void *worker(void *arg)
{
    int n = *(int *)arg;
    /* Enough heap traffic to look like real work, and a touch of stack. */
    char *buf = malloc(64 * 1024);
    if (!buf) return NULL;
    memset(buf, n & 0xFF, 64 * 1024);
    volatile unsigned sum = 0;
    for (int i = 0; i < 64 * 1024; i += 512) sum += (unsigned char)buf[i];
    free(buf);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t th;

    printf("thrdyn: phase 1 — create + join a thread that returns at once\n");
    if (pthread_create(&th, NULL, trivial, NULL) != 0) {
        printf("thrdyn: pthread_create FAILED\n"); return 1;
    }
    pthread_join(th, NULL);
    printf("thrdyn: phase 1 OK\n");

    printf("thrdyn: phase 2 — a thread that allocates and touches memory\n");
    int n = 7;
    if (pthread_create(&th, NULL, worker, &n) != 0) {
        printf("thrdyn: pthread_create FAILED\n"); return 1;
    }
    pthread_join(th, NULL);
    printf("thrdyn: phase 2 OK\n");

    printf("thrdyn: phase 3 — five in sequence\n");
    for (int i = 0; i < 5; i++) {
        int k = i;
        if (pthread_create(&th, NULL, worker, &k) != 0) {
            printf("thrdyn: pthread_create FAILED at %d\n", i); return 1;
        }
        pthread_join(th, NULL);
        printf("thrdyn:   joined %d\n", i);
    }

    printf("thrdyn: PASS — a dynamic binary can create, run and reap threads\n");
    return 0;
}
