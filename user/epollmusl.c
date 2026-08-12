/* =============================================================================
 * epollmusl.c — epoll through UNMODIFIED musl, to prove the ABI marshalling.
 *
 * The kernel-side `epolltest` exercises the mechanism; this exercises the
 * TRANSLATION, which is the part with a real trap in it.  A guest's
 * `struct epoll_event` is:
 *
 *      i386   : { u32 events; u64 data; }              12 bytes, data at 4
 *      amd64  : { u32 events; u64 data; } __packed__   12 bytes, data at 4
 *      arm64  : { u32 events; u64 data; }              16 bytes, data at 8
 *
 * Linux packs it on x86_64 *specifically* so that the 32- and 64-bit layouts
 * agree, and does not do so anywhere else — so the size does NOT follow the
 * word size, and any kernel that derives one from the other is wrong on some
 * architecture.  The `data` cookie is what catches a mistake: get the offset
 * wrong by four bytes and it comes back shifted, which no amount of "it
 * compiled" will tell you.
 *
 * Built and run as a stock Linux binary under the personality — nothing here
 * knows it is not on Linux.
 * ============================================================================= */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <sys/time.h>

int main(void) {
    printf("epollmusl: sizeof(struct epoll_event) = %d\n",
           (int)sizeof(struct epoll_event));

    int ep = epoll_create1(0);
    if (ep < 0) { printf("epollmusl: epoll_create1 failed\n"); return 1; }

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { printf("epollmusl: timerfd_create failed\n"); return 1; }

    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 60 * 1000 * 1000;          /* 60 ms, one-shot */
    if (timerfd_settime(tfd, 0, &its, 0) != 0) {
        printf("epollmusl: timerfd_settime failed\n");
        return 1;
    }

    /* A cookie with bits set in BOTH halves of the u64: a marshalling bug that
     * only moved the value by four bytes would still look plausible with a
     * small integer, and would be invisible with zero. */
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events   = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) != 0) {
        printf("epollmusl: epoll_ctl failed\n");
        return 1;
    }

    struct epoll_event out[4];
    memset(out, 0, sizeof out);
    int n = epoll_wait(ep, out, 4, 5000);
    printf("epollmusl: epoll_wait -> n=%d events=0x%x data=0x%08x%08x\n",
           n, n > 0 ? out[0].events : 0,
           n > 0 ? (unsigned)(out[0].data.u64 >> 32) : 0,
           n > 0 ? (unsigned)(out[0].data.u64 & 0xFFFFFFFFu) : 0);

    int ok = (n == 1 && out[0].data.u64 == 0x1122334455667788ull
                     && (out[0].events & EPOLLIN));

    /* A finite timeout on a set with nothing ready must WAIT, not return.
     * This is what was wrong before M56, and it is invisible unless timed. */
    uint64_t drain;
    (void)!read(tfd, &drain, sizeof drain);
    n = epoll_wait(ep, out, 4, 150);
    printf("epollmusl: idle wait -> n=%d (expect 0)\n", n);
    if (n != 0) ok = 0;

    /* --- sigprocmask, which was a `return 0` stub until M56.1 ------------ */
    /*
     * The check has to be observable, so: block SIGALRM, arm a 60 ms timer,
     * sleep past it, and confirm the signal is PENDING rather than delivered
     * or lost.  A stub that accepted and forgot would fail here in the most
     * informative way — the handler would have run.
     */
    sigset_t block, old_set;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &block, &old_set) != 0) {
        printf("epollmusl: sigprocmask failed\n");
        ok = 0;
    }

    struct itimerval iv;
    memset(&iv, 0, sizeof iv);
    iv.it_value.tv_usec = 60000;                  /* 60 ms, microseconds */
    setitimer(ITIMER_REAL, &iv, 0);

    struct epoll_event idle[1];
    epoll_wait(ep, idle, 1, 200);                 /* outlive the timer */

    sigset_t pend;
    sigemptyset(&pend);
    sigpending(&pend);
    int is_pending = sigismember(&pend, SIGALRM);
    printf("epollmusl: SIGALRM blocked -> pending=%d (want 1)\n", is_pending);
    if (!is_pending) {
        printf("epollmusl: FAIL (a blocked signal was lost, not deferred)\n");
        ok = 0;
    }
    /* Reaching this line at all is half the result: an unblocked SIGALRM with
     * no handler terminates the process, so a stub sigprocmask would have
     * killed us before the printf. */

    sigprocmask(SIG_SETMASK, &old_set, 0);

    close(tfd);
    close(ep);
    printf("epollmusl: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
