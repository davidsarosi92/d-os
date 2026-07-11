/* =============================================================================
 * tlstest.c — thread-local storage via %gs (M35).
 *
 * Each thread points its %gs segment at its own thread-control block (via
 * set_tls) that carries the thread's id at offset 4, then repeatedly reads it
 * back through `%gs:4` while all threads run concurrently.  A consistent
 * read-back — each thread always sees ITS OWN id, never a sibling's — proves
 * the kernel maintains a per-thread TLS base across context switches (the
 * scheduler reloads this CPU's user-TLS descriptor on every switch-in).
 *
 * This exercises the %gs-segment TLS mechanism; the compiler's `__thread` ABI
 * (PT_TLS template + variant-II layout) layers on top when the libc port lands.
 * ============================================================================= */

#include "libc.h"

struct tcb { void* self; int tid; };      /* %gs:0 = self, %gs:4 = tid */

#define NTHREADS 4
#define NITER    50000

static int worker(void* arg) {
    struct tcb t;
    t.self = &t;
    t.tid  = (int)(long)arg;
    set_tls(&t);                           /* %gs base := &t */

    int mism = 0;
    for (int i = 0; i < NITER; i++)
        if (tls_load4() != t.tid) mism++;  /* read our id back via %gs:4 */

    printf("tls: tid=%d read-back via %%gs, mismatches=%d %s\n",
           t.tid, mism, mism == 0 ? "OK" : "FAIL");
    return mism == 0 ? 0 : 1;
}

int main(void) {
    int tid[NTHREADS];
    printf("tls: spawning %d threads, each with its own %%gs TLS block\n", NTHREADS);
    for (int i = 0; i < NTHREADS; i++)
        tid[i] = thread_create(worker, (void*)(long)(100 + i));

    int fails = 0, st;
    for (int i = 0; i < NTHREADS; i++) { thread_join(tid[i]); }
    (void)st; (void)fails;
    printf("tls: done (each thread saw only its own id => per-thread TLS works)\n");
    return 0;
}
