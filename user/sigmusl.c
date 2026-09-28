/* =============================================================================
 * sigmusl.c — Linux signals as a JVM uses them (§M89 rung 2).
 *
 * An UNMODIFIED musl binary under the Linux personality.  Each claim fails on
 * the pre-§M89 kernel, where rt_sigaction was accepted and ignored and every
 * fault killed the process:
 *
 *   1. SIGINFO — raise(SIGUSR1) runs an SA_SIGINFO handler with si_signo and
 *      si_code == SI_TKILL (musl's raise is tkill) and the sender's pid.
 *   2. FAULT + FIX — a write to a PROT_NONE page runs the SIGSEGV handler with
 *      si_addr == that address; the handler mprotects the page writable and
 *      returns, and the write is RE-EXECUTED and lands.
 *   3. REWRITE PC — an illegal instruction (ud2 / udf) runs the SIGILL handler,
 *      which advances the saved program counter in the ucontext past it; the
 *      program continues after it.  This is HotSpot's implicit-exception shape.
 *   4. ALTSTACK — with SA_ONSTACK, the handler runs on the sigaltstack.
 *   5. SA_MASK — a signal in sa_mask, raised inside the handler, is held until
 *      the handler returns, then delivered.
 *   6. PTHREAD_KILL — a signal to another THREAD runs the handler on it.
 * ============================================================================= */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

static int g_fail;
static void verdict(int ok, const char* what) {
    printf("sigmusl: %s - %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

/* 1 */
static volatile int s1_sig, s1_code, s1_pid;
static void h_usr1(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    s1_sig = sig; s1_code = si->si_code; s1_pid = si->si_pid;
}

/* 2 */
static unsigned char* g_page;
static volatile void* s2_addr;
static volatile int s2_hits;
static void h_segv(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)uc;
    s2_addr = si->si_addr;
    s2_hits++;
    mprotect(g_page, 4096, PROT_READ | PROT_WRITE);
}

/* 3 */
static volatile int s3_hits;
static void h_ill(int sig, siginfo_t* si, void* ucv) {
    (void)sig; (void)si;
    ucontext_t* uc = (ucontext_t*)ucv;
    s3_hits++;
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] += 2;              /* ud2 is 2 bytes */
#elif defined(__i386__)
    uc->uc_mcontext.gregs[REG_EIP] += 2;
#elif defined(__aarch64__)
    uc->uc_mcontext.pc += 4;                          /* udf #0 is 4 bytes */
#endif
}
static void do_illegal(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile ("ud2");
#elif defined(__aarch64__)
    __asm__ volatile (".inst 0x00000000");            /* udf #0 */
#endif
}

/* 4 */
static char g_alt[16384] __attribute__((aligned(16)));
static volatile uintptr_t s4_local;
static void h_usr2(int sig) { (void)sig; volatile int here = 0; s4_local = (uintptr_t)&here; }

/* 5 */
static volatile int s5_order[4], s5_n;
static void h_alrm_outer(int sig) {
    (void)sig;
    s5_order[s5_n++] = 1;
    raise(SIGHUP);                                    /* in sa_mask: must wait */
    s5_order[s5_n++] = 2;
}
static void h_hup(int sig) { (void)sig; s5_order[s5_n++] = 3; }

/* 6 */
static volatile pthread_t s6_target_self;
static volatile int s6_ran_on_target, s6_ready, s6_done;
static pthread_t s6_tid;
static void h_term_thread(int sig) { (void)sig; s6_ran_on_target = pthread_equal(pthread_self(), s6_tid); s6_done = 1; }
static void* s6_thread(void* a) {
    (void)a;
    s6_ready = 1;
    for (long i = 0; i < 400000000L && !s6_done; i++) { if ((i & 0xFFFF) == 0) sched_yield(); }
    return NULL;
}

int main(void) {
    struct sigaction sa;

    /* 1 */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    verdict(s1_sig == SIGUSR1 && s1_code == SI_TKILL && s1_pid == getpid(),
            "an SA_SIGINFO handler gets signo, si_code SI_TKILL and the sender's pid");
    if (s1_sig != SIGUSR1) printf("sigmusl: sig=%d code=%d pid=%d (self %d)\n", s1_sig, s1_code, s1_pid, getpid());

    /* 2 */
    g_page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    g_page[123] = 0x77;                               /* faults once, then lands */
    /* A COMPILER BARRIER, load-bearing: the store is not volatile, so without
     * this GCC may sink it below the volatile reads in the check — the first
     * run of this test "failed" exactly that way, the fault happening after
     * the verdict had been printed. */
    __asm__ volatile ("" ::: "memory");
    verdict(s2_hits == 1 && s2_addr == (void*)(g_page + 123) && g_page[123] == 0x77,
            "a SIGSEGV handler sees si_addr, fixes the page, and the write is re-executed");
    if (!(s2_hits == 1 && s2_addr == (void*)(g_page + 123) && g_page[123] == 0x77))
        printf("sigmusl: hits=%d si_addr=%p expected=%p value=%x\n", s2_hits, (void*)s2_addr,
               (void*)(g_page + 123), g_page[123]);

    /* 3 */
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = h_ill;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    do_illegal();
    verdict(s3_hits == 1, "a handler that advances the saved pc resumes after the faulting instruction");

    /* 4 */
    stack_t ss = { .ss_sp = g_alt, .ss_size = sizeof g_alt, .ss_flags = 0 };
    int alt_ok = sigaltstack(&ss, NULL) == 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_usr2;
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR2);
    verdict(alt_ok && s4_local >= (uintptr_t)g_alt && s4_local < (uintptr_t)g_alt + sizeof g_alt,
            "an SA_ONSTACK handler runs on the sigaltstack");

    /* 5 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_hup;
    sigaction(SIGHUP, &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_alrm_outer;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGHUP);
    sigaction(SIGALRM, &sa, NULL);
    raise(SIGALRM);
    verdict(s5_n == 3 && s5_order[0] == 1 && s5_order[1] == 2 && s5_order[2] == 3,
            "a signal in sa_mask waits until the handler returns");

    /* 6 */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h_term_thread;
    sigaction(SIGTERM, &sa, NULL);
    if (pthread_create(&s6_tid, NULL, s6_thread, NULL) != 0) {
        verdict(0, "pthread_kill runs the handler on the target thread (no threads: pthread_create failed)");
    } else {
        while (!s6_ready) sched_yield();
        pthread_kill(s6_tid, SIGTERM);
        pthread_join(s6_tid, NULL);
        verdict(s6_done && s6_ran_on_target, "pthread_kill runs the handler on the target thread");
    }

    printf("sigmusl: %s (%d failed)\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
