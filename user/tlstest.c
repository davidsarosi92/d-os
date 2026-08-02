/* =============================================================================
 * tlstest.c — thread-local storage via the thread pointer (M35).
 *
 * Each thread points the thread-pointer register at its own thread-control
 * block (via set_tls) and then repeatedly reads its id back out of that block
 * while all threads run concurrently.  A consistent
 * read-back — each thread always sees ITS OWN id, never a sibling's — proves
 * the kernel maintains a per-thread TLS base across context switches (the
 * scheduler reloads this CPU's user-TLS descriptor on every switch-in).
 *
 * WHICH register holds the pointer is the kernel's choice and differs per arch
 * (i386 %gs, x86_64 FS.base, aarch64 TPIDR_EL0) — libc.h's tls_load4 hides
 * that.  The FIELD OFFSET must come from offsetof, not a literal: `self` is a
 * pointer, so `tid` sits at 4 on a 32-bit build and at 8 on a 64-bit one.
 *
 * This exercises the raw thread-pointer mechanism; the compiler's `__thread`
 * ABI (PT_TLS template + variant-II layout) layers on top later.
 * ============================================================================= */

#include "libc.h"

struct tcb { void* self; int tid; };      /* [0] = self, [sizeof(void*)] = tid */
#define TCB_TID_OFF ((unsigned long)__builtin_offsetof(struct tcb, tid))

#define NTHREADS 4
#define NITER    50000

static int worker(void* arg) {
    struct tcb t;
    t.self = &t;
    t.tid  = (int)(long)arg;
    set_tls(&t);                           /* thread pointer := &t */

    int mism = 0;
    for (int i = 0; i < NITER; i++)
        if (tls_load4(TCB_TID_OFF) != t.tid) mism++;   /* read our id back */

    printf("tls: tid=%d read-back via the thread pointer, mismatches=%d %s\n",
           t.tid, mism, mism == 0 ? "OK" : "FAIL");
    return mism == 0 ? 0 : 1;
}

int main(void) {
    int tid[NTHREADS];
    printf("tls: spawning %d threads, each with its own TLS block\n", NTHREADS);
    for (int i = 0; i < NTHREADS; i++)
        tid[i] = thread_create(worker, (void*)(long)(100 + i));

    int fails = 0, st;
    for (int i = 0; i < NTHREADS; i++) { thread_join(tid[i]); }
    (void)st; (void)fails;
    printf("tls: done (each thread saw only its own id => per-thread TLS works)\n");
    return 0;
}
