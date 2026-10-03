/* =============================================================================
 * cmd_sys.c — machine-level shell commands (§M70).
 *
 * Split out of shell.c: the commands whose subject is the MACHINE — what it
 * is, how long it has been up, what has gone wrong on it, what time it thinks
 * it is, and how to turn it off.
 *
 * `clear` is here and is the one command in this file that addresses the
 * TERMINAL rather than the machine, which is why it is also the one that asks
 * for `shell_current_vc()` and reports honestly when there is none (the ARM
 * serial REPL has no VC at all).
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "klog.h"
#include "timer.h"
#include "ktimer.h"
#include "crash.h"
#include "service.h"
#include "cron.h"
#include "abi.h"
#include "hal.h"
#include "version.h"
#include "vc.h"
#include "config.h"
#include "task.h"
#include "uaccess.h"
#include "epoll.h"
#include "fd.h"
#include "hal_api.h"
#include "vfs.h"
#include "kmalloc.h"
#include "syscall.h"
#include "elf.h"
#include "proc.h"
#include "vmm.h"
#include "usermode.h"
#include "driver.h"
#include "flock.h"
#include "fifo.h"
#include "nsproxy.h"
int procfs_ns_handle(struct file* f, int* kind, struct nsobj** obj, int* mnt, uint32_t* ino);
#include <stdint.h>
#include <stddef.h>

struct dmesg_ctx { int max_level; };

/* --- file-scope state these commands own (moved with them out of shell.c) --- */
/* ---------------------------------------------------------------------------
 * `fputest` — prove the FP/SIMD register file is PER-TASK.
 *
 * Two kernel tasks each stamp a different pattern into a live FP register, then
 * spend a few thousand yields checking it is still theirs.  Without per-task
 * FXSAVE/FXRSTOR the two tasks share one physical register file, so each sees
 * the other's value within a yield or two and the mismatch counters explode —
 * i.e. this test FAILS on the code that existed before the fix, which is the
 * only kind of regression test worth having.
 *
 * The patterns are exactly-representable doubles on purpose: i386 holds the
 * value in an 80-bit x87 register and converts back on read, so an arbitrary
 * bit pattern (an SNaN, say) would not survive the round trip and the test
 * would report a corruption that never happened.
 * --------------------------------------------------------------------------- */
#define FPUTEST_ROUNDS 3000

static volatile uint64_t g_fpu_mismatch[2];

static volatile int      g_fpu_done[2];

/* -------------------------------------------------------------------- */
/* §M53 stage 3 — `timerfdtest`: prove that a deadline can be WAITED FOR  */
/* alongside I/O, and that a periodic one does not drift.                */
/*                                                                       */
/* Two things are measured rather than asserted.  (1) The wait goes       */
/* through poll(2), not through a sleep — that is the whole point of the  */
/* descriptor, and a timerfd that woke only its own reader would pass a   */
/* read-based test and still be useless to an event loop.  (2) The error  */
/* reported is the error against the ORIGINAL start, not against the      */
/* previous tick: a timer that re-arms from "now" looks perfect tick to   */
/* tick and drifts without bound over a minute, and only the cumulative   */
/* number shows it.                                                      */
/* -------------------------------------------------------------------- */
static void cmd_timerfdtest(const char* args) {
    unsigned period_ms = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) period_ms = period_ms * 10 + (unsigned)(*args - '0');
    if (period_ms == 0) period_ms = 50;

    int fd = sys_timerfd_create();
    if (fd < 0) { console_write("timerfd: create failed\n"); return; }

    uint64_t period_ns = (uint64_t)period_ms * 1000000ull;
    uint64_t t0 = timer_now_ns();
    if (sys_timerfd_settime(fd, 0 /* relative */, period_ns, period_ns) != 0) {
        console_write("timerfd: settime failed\n");
        sys_close(fd);
        return;
    }
    kprintf("timerfd: fd %d, period %u ms — waiting via poll(2)\n", fd, period_ms);

    for (int i = 1; i <= 5; i++) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = sys_poll_k(&pf, 1, -1);         /* block until readable */
        uint64_t now = timer_now_ns();
        uint64_t buf = 0;
        long got = sys_read_k(fd, &buf, sizeof buf);

        /* Error against the START, not against the previous tick — see above. */
        uint64_t want = t0 + (uint64_t)i * period_ns;
        long long err_us = (long long)((now - want) / 1000ull);
        if (now < want) err_us = -(long long)((want - now) / 1000ull);

        kprintf("  tick %d: poll=%d revents=%x read=%d expirations=%u  drift %s%u us\n",
                i, r, (unsigned)pf.revents, (int)got, (unsigned)buf,
                err_us < 0 ? "-" : "+",
                (unsigned)(err_us < 0 ? -err_us : err_us));
    }

    uint64_t rem = 0, iv = 0;
    sys_timerfd_gettime_k(fd, &rem, &iv);
    kprintf("  gettime: %u us to go, interval %u us\n",
            (unsigned)(rem / 1000), (unsigned)(iv / 1000));
    sys_close(fd);
    console_write("timerfd: ok\n");
}

static void cmd_epolltest(void) {
    /* --- 1. a finite timeout on an empty set must actually wait --------- */
    int ep = sys_epoll_create();
    if (ep < 0) { console_write("epoll: create failed\n"); return; }

    uint64_t t0 = timer_now_ns();
    uint64_t evbuf[2 * 8];
    int n = sys_epoll_wait_k(ep, evbuf, 8, 200);
    uint64_t waited_us = (timer_now_ns() - t0) / 1000ull;
    kprintf("epoll: empty set, 200 ms timeout -> n=%d after %u us\n",
            n, (unsigned)waited_us);
    int ok = (n == 0 && waited_us >= 150000ull);
    if (!ok) console_write("epoll: FAIL (a finite timeout returned early)\n");

    /* --- 2. a timerfd in the set wakes the wait ------------------------- */
    int tfd = sys_timerfd_create();
    if (tfd < 0) { console_write("epoll: timerfd failed\n"); sys_close(ep); return; }
    sys_timerfd_settime(tfd, 0, 80000000ull, 0);         /* one-shot, 80 ms */

    uint64_t ev[2] = { POLLIN, 0xABCDEF01ull };          /* events, cookie */
    if (sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, tfd, (uint32_t)ev[0], ev[1]) != 0)
        console_write("epoll: FAIL (ctl ADD)\n");

    t0 = timer_now_ns();
    n = sys_epoll_wait_k(ep, evbuf, 8, 5000);
    waited_us = (timer_now_ns() - t0) / 1000ull;
    kprintf("epoll: timerfd -> n=%d events=%x data=%x%x after %u us\n",
            n, (unsigned)evbuf[0],
            (unsigned)(evbuf[1] >> 32), (unsigned)evbuf[1],
            (unsigned)waited_us);
    if (n != 1 || evbuf[1] != 0xABCDEF01ull) {
        console_write("epoll: FAIL (timerfd not reported, or cookie mangled)\n");
        ok = 0;
    }
    /* The cookie is the whole ergonomics of epoll: the kernel hands back the
     * caller's own pointer-sized token, so a loop does not have to search its
     * own tables for which connection an fd belongs to. */

    uint64_t drained = 0;
    sys_read_k(tfd, &drained, sizeof drained);

    /* --- 3. a pipe in the SAME set ------------------------------------- */
    int pfds[2];
    if (sys_pipe(pfds) == 0) {
        if (sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, pfds[0], POLLIN, 0x2222ull) != 0)
            console_write("epoll: FAIL (ctl ADD pipe)\n");

        /* Nothing written yet: the set must NOT report the pipe. */
        n = sys_epoll_wait_k(ep, evbuf, 8, 50);
        if (n != 0) { kprintf("epoll: FAIL (spurious ready, n=%d)\n", n); ok = 0; }

        sys_write_k(pfds[1], "x", 1);
        n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
        kprintf("epoll: pipe -> n=%d data=%x\n", n, (unsigned)evbuf[1]);
        if (n != 1 || evbuf[1] != 0x2222ull) {
            console_write("epoll: FAIL (pipe not reported)\n"); ok = 0;
        }
        /* DEL before close, always.  A set watches descriptor NUMBERS, so a
         * closed-and-reused fd silently inherits the old registration — the
         * first version of this test skipped the DEL, the next pipe got the
         * same fd number, its ADD failed with -EEXIST, and the stale entry's
         * narrower event mask made the kernel look like it was dropping
         * POLLRDHUP.  The hazard is real; it just belonged to the test. */
        sys_epoll_ctl_k(ep, EPOLL_CTL_DEL, pfds[0], 0, 0);
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 3b. hangup: closing the writer must be VISIBLE without reading -- */
    if (sys_pipe(pfds) == 0) {
        if (sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, pfds[0],
                            POLLIN | EPOLLRDHUP, 0x3333ull) != 0) {
            console_write("epoll: FAIL (ctl ADD for the hangup case)\n"); ok = 0;
        }
        sys_write_k(pfds[1], "z", 1);
        sys_close(pfds[1]);                      /* writer gone, one byte left */

        n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
        kprintf("epoll: after writer close -> n=%d events=%x\n",
                n, n > 0 ? (unsigned)evbuf[0] : 0);
        /* The byte is still there, so POLLIN AND RDHUP — but NOT yet HUP:
         * a reader must be able to drain the tail before it shuts down. */
        if (n != 1 || !(evbuf[0] & POLLIN) || !(evbuf[0] & POLLRDHUP)
                   || (evbuf[0] & POLLHUP)) {
            console_write("epoll: FAIL (hangup reported wrong with data left)\n");
            ok = 0;
        }
        char c = 0;
        sys_read_k(pfds[0], &c, 1);              /* drain it */
        n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
        kprintf("epoll: after drain      -> n=%d events=%x\n",
                n, n > 0 ? (unsigned)evbuf[0] : 0);
        if (n != 1 || !(evbuf[0] & POLLHUP)) {
            console_write("epoll: FAIL (POLLHUP not reported once drained)\n");
            ok = 0;
        }
        sys_epoll_ctl_k(ep, EPOLL_CTL_DEL, pfds[0], 0, 0);
        sys_close(pfds[0]);
    }

    /* --- 3c. O_NONBLOCK on a PIPE, which used to be silently ignored ---- */
    if (sys_pipe(pfds) == 0) {
        sys_socket_setnonblock(pfds[0], 1);
        char c = 0;
        long r = sys_read_k(pfds[0], &c, 1);     /* empty, writer alive */
        kprintf("epoll: nonblocking empty pipe read -> %d (want EAGAIN=%d)\n",
                (int)r, -SOCK_EAGAIN);
        /* Must be EAGAIN and NOT 0: zero means end of file, and a drain loop
         * told "EOF" by a live-but-empty pipe stops for good. */
        if (r != -SOCK_EAGAIN) {
            console_write("epoll: FAIL (O_NONBLOCK ignored on a pipe)\n");
            ok = 0;
        }
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 4. edge-triggered (§M90): reported once per EDGE, not per level --
     * A byte arrives -> reported.  Nothing happens -> NOT reported again,
     * although the byte is still unread (that is the difference from level).
     * A second byte arrives -> reported again although the fd never stopped
     * being readable (Linux's ET wakes on each arrival, and Go relies on it).
     * The middle step is what fails if ET is served as level; the last is
     * what fails if an edge is lost. */
    if (sys_pipe(pfds) == 0) {
        int ep2 = sys_epoll_create();
        int et_ok = ep2 >= 0
            && sys_epoll_ctl_k(ep2, EPOLL_CTL_ADD, pfds[0], POLLIN | EPOLLET, 0x5555ull) == 0;
        char c = 'e';
        sys_write_k(pfds[1], &c, 1);
        int n1 = et_ok ? sys_epoll_wait_k(ep2, evbuf, 8, 1000) : -1;
        int n2 = et_ok ? sys_epoll_wait_k(ep2, evbuf, 8, 0)    : -1;
        sys_write_k(pfds[1], &c, 1);
        int n3 = et_ok ? sys_epoll_wait_k(ep2, evbuf, 8, 1000) : -1;
        kprintf("epoll: EPOLLET first=%d again=%d after-new-byte=%d (want 1 0 1)\n",
                n1, n2, n3);
        if (n1 != 1 || n2 != 0 || n3 != 1) {
            console_write("epoll: FAIL (edge-triggered semantics)\n");
            ok = 0;
        }
        if (ep2 >= 0) sys_close(ep2);
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 4b. the readiness memo must never hide a transition ------------ */
    /*
     * §M56.2 caches each item's readiness against the description's generation
     * counter, so a scan re-evaluates only the descriptors that moved.  The
     * failure mode of a missing generation bump is not slowness — it is an
     * event that never arrives, and it would show up long after the change
     * that caused it.  So: drive a pipe through many ready/not-ready
     * transitions and insist that every single one is seen.
     */
    if (sys_pipe(pfds) == 0) {
        sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, pfds[0], POLLIN, 0x4444ull);
        int seen_ready = 0, seen_idle = 0;
        for (int round = 0; round < 20; round++) {
            char c = 'a';
            sys_write_k(pfds[1], &c, 1);
            n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
            if (n == 1 && (evbuf[0] & POLLIN) && evbuf[1] == 0x4444ull) seen_ready++;

            sys_read_k(pfds[0], &c, 1);          /* drain → not readable again */
            n = sys_epoll_wait_k(ep, evbuf, 8, 0);
            if (n == 0) seen_idle++;
        }
        kprintf("epoll: memo check — %d/20 ready, %d/20 idle transitions seen\n",
                seen_ready, seen_idle);
        if (seen_ready != 20 || seen_idle != 20) {
            console_write("epoll: FAIL (the readiness cache hid a transition)\n");
            ok = 0;
        }
        sys_epoll_ctl_k(ep, EPOLL_CTL_DEL, pfds[0], 0, 0);
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 5. what does the SCAN actually cost? --------------------------- */
    /*
     * epoll's reputation is O(ready); ours scans the registered set.  Rather
     * than argue about whether that matters here, measure it: fill the set
     * with timerfds that will never fire and time a non-blocking wait.  The
     * number is what decides whether per-fd wakeups are worth the cross-object
     * lifetime coupling they require — and it is printed rather than asserted
     * so a future reader can re-decide with their own workload.
     */
    int bench = sys_epoll_create();
    if (bench >= 0) {
        int fds[48]; int nf = 0;
        for (int i = 0; i < 48; i++) {
            int t = sys_timerfd_create();
            if (t < 0) break;
            fds[nf++] = t;
            sys_epoll_ctl_k(bench, EPOLL_CTL_ADD, t, POLLIN, (uint64_t)i);
        }
        /* One warm-up wait so the memo is populated, then measure the steady
         * state: this is what a real loop pays on every iteration where most
         * of its descriptors have not moved. */
        sys_epoll_wait_k(bench, evbuf, 8, 0);
        uint64_t b0 = timer_now_ns();
        for (int i = 0; i < 100; i++) sys_epoll_wait_k(bench, evbuf, 8, 0);
        uint64_t per_ns = (timer_now_ns() - b0) / 100ull;
        kprintf("epoll: scan of %d registered fds costs %u ns per wait\n",
                nf, (unsigned)per_ns);
        for (int i = 0; i < nf; i++) sys_close(fds[i]);
        sys_close(bench);
    }

    sys_close(tfd);
    sys_close(ep);
    console_write(ok ? "epoll: ok\n" : "epoll: FAILED\n");
}

/* §M53 stage 3 — `alarmtest`: the OTHER delivery.  A program with nothing to
 * poll wants to be interrupted, not woken, so this arms an interval timer and
 * watches SIGALRM land in the caller's pending set. */
static void cmd_alarmtest(const char* args) {
    unsigned ms = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) ms = ms * 10 + (unsigned)(*args - '0');
    if (ms == 0) ms = 100;

    struct task* me = task_current();
    if (!me) return;
    me->sig_pending &= ~(1u << SIGALRM);

    uint64_t t0 = timer_now_ns();
    if (sys_setitimer_ns((uint64_t)ms * 1000000ull, 0) != 0) {
        console_write("alarm: setitimer failed\n");
        return;
    }
    kprintf("alarm: armed for %u ms — waiting for SIGALRM to be posted\n", ms);

    /* A kernel task never takes a return-to-user trip, so it observes the
     * PENDING BIT rather than a handler call.  Same signal, same posting
     * path — only the delivery point differs, and that difference is what
     * this test deliberately does not pretend to cover. */
    for (int i = 0; i < 400; i++) {
        if (me->sig_pending & (1u << SIGALRM)) break;
        task_msleep(5);
    }
    uint64_t dt = timer_now_ns() - t0;
    if (me->sig_pending & (1u << SIGALRM)) {
        me->sig_pending &= ~(1u << SIGALRM);
        kprintf("alarm: SIGALRM posted after %u us (asked for %u us)\n",
                (unsigned)(dt / 1000), ms * 1000);
        console_write("alarm: ok\n");
    } else {
        console_write("alarm: FAIL — SIGALRM never arrived\n");
    }
    sys_setitimer_ns(0, 0);
}

/* §1.1 — `faulttest`: prove that a BAD user pointer can no longer take the box
 * down.  Two layers are checked:
 *   (a) the GATE — a syscall handed a kernel/unmapped address must return an
 *       error (the address never gets dereferenced at all);
 *   (b) the FAULT FIXUP — the uaccess primitive is called DIRECTLY on unmapped
 *       memory, bypassing the pre-check, so the copy really does fault in ring 0.
 *       The exception table must catch it and return -1.  Before this existed,
 *       (b) was a kernel #PF → halt policy → the whole machine froze.
 * Borrows a private address space so the "in a user syscall" gate is realistic. */
static void cmd_faulttest(void) {
    struct task* me = task_current();
    if (!me) return;
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("faulttest: no space\n"); return; }
    struct vmm_space* prev = me->mm;
    int prev_gate = me->in_user_syscall;
    task_swap_mm(me, s);
    vmm_space_switch(s);
    me->in_user_syscall = 1;                 /* pretend we came from ring 3 */

    /* (a) gate: a kernel address and an unmapped user address as syscall args. */
    long w_kern   = sys_write(1, (const void*)(uintptr_t)0x00100000u, 8);   /* kernel text */
    int  o_unmap  = sys_open((const char*)(uintptr_t)(vmm_user_base() + 0x123000u), 0);
    struct kstat st;
    int  s_unmap  = sys_stat((const char*)(uintptr_t)(vmm_user_base() + 0x123000u), &st);

    me->in_user_syscall = prev_gate;

    /* (b) fixup: unmapped, checked by nothing — only the exception table can
     *     save us here.  If the table were missing this line would panic. */
    char buf[8];
    int  cp_in    = uaccess_copy_in(buf, (uintptr_t)(vmm_user_base() + 0x456000u), sizeof buf);
    int  cp_out   = uaccess_copy_out((uintptr_t)(vmm_user_base() + 0x456000u), buf, sizeof buf);
    long str_in   = uaccess_str_in(buf, (uintptr_t)(vmm_user_base() + 0x456000u), sizeof buf);

    /* (c) BOUNCE (§1.1 layer 3).  Map exactly ONE user page, so the page right
     *     after it is guaranteed unmapped.  Two things get proven here:
     *       - a valid ring-3 payload still round-trips correctly through the
     *         kernel staging chunk (`sys_write` of "bnce" prints it), and
     *       - the TOCTOU the pre-check CANNOT catch: a copy whose range goes bad
     *         PART-WAY THROUGH.  Straddling the page boundary reproduces exactly
     *         that — the first bytes copy, then the next page faults.  We call
     *         uaccess_copy_in directly (no pre-check in the way) so the only
     *         thing standing between us and a ring-0 #PF is the exception table.
     *         Before bounce buffers, this shape reached the VFS/socket layers as
     *         a raw pointer, where no fixup entry covers the dereference. */
    long upage = sys_mmap(4096, -1);
    long w_ok = -1, w_strad = 0, cp_partial = 0; int partial_ok = 0;
    if (upage > 0) {
        char* up = (char*)(uintptr_t)upage;
        up[0] = 'b'; up[1] = 'n'; up[2] = 'c'; up[3] = 'e'; up[4] = ' ';
        for (int i = 4088; i < 4096; i++) up[i] = (char)0xA5;   /* tail pattern */

        me->in_user_syscall = 1;
        w_ok    = sys_write(1, (const void*)(uintptr_t)upage, 5);        /* valid  */
        w_strad = sys_write(1, (const void*)(uintptr_t)(upage + 4088), 32); /* runs off */
        me->in_user_syscall = prev_gate;

        /* Mid-copy fault: 8 good bytes then unmapped memory. */
        uint8_t k[16];
        for (int i = 0; i < 16; i++) k[i] = 0;
        cp_partial = uaccess_copy_in(k, (uintptr_t)upage + 4088, 16);
        partial_ok = (k[0] == 0xA5 && k[7] == 0xA5);   /* the good half DID land */
    }

    vmm_space_switch(prev);
    task_swap_mm(me, prev);
    vmm_space_destroy(s);

    kprintf("faulttest: gate   write(kernel ptr)=%ld open(unmapped)=%d stat(unmapped)=%d -> %s\n",
            w_kern, o_unmap, s_unmap,
            (w_kern < 0 && o_unmap < 0 && s_unmap < 0) ? "PASS" : "FAIL");
    kprintf("faulttest: fixup  copy_in=%d copy_out=%d str_in=%ld -> %s\n",
            cp_in, cp_out, str_in,
            (cp_in < 0 && cp_out < 0 && str_in < 0) ? "PASS" : "FAIL");
    kprintf("faulttest: bounce write(valid)=%ld write(straddle)=%ld mid-copy=%d partial=%d -> %s\n",
            w_ok, w_strad, (int)cp_partial, partial_ok,
            (w_ok == 5 && w_strad < 0 && cp_partial < 0 && partial_ok) ? "PASS" : "FAIL");
    console_write("faulttest: the box is still running — that IS the test.\n");
}

/* ---------------------------------------------------------------------------
 * `archtest` — prove the ELF loader refuses a FOREIGN-architecture image.
 *
 * Before this check existed, a wrong-arch binary loaded "successfully": the
 * segments mapped and the CPU was handed an entry point full of foreign
 * instruction encodings, so the failure surfaced later as an unrelated-looking
 * fault (and on a 32-bit kernel, a 64-bit image had its p_vaddr fields silently
 * truncated, mapping segments at the wrong addresses entirely).
 *
 * The test synthesises bare ELF headers — no address space needed, because both
 * outcomes are decided in the header:
 *   - a foreign (class, machine) pair must be rejected with ELF_EBADARCH;
 *   - the NATIVE pair must get PAST the arch gate, which we observe as the next
 *     error along (ELF_ENOLOAD — the synthetic header has no PT_LOAD).  That
 *     second half is the important one: it proves the gate is discriminating
 *     rather than just refusing everything.
 * --------------------------------------------------------------------------- */
static void put16le(uint8_t* p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void make_ehdr(uint8_t* b, unsigned cls, unsigned machine) {
    for (int i = 0; i < 64; i++) b[i] = 0;
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = (uint8_t)cls;            /* EI_CLASS  */
    b[5] = 1;                       /* EI_DATA = little-endian */
    b[6] = 1;                       /* EI_VERSION */
    put16le(b + 16, 2);             /* e_type = ET_EXEC */
    put16le(b + 18, machine);       /* e_machine */
    /* e_phnum stays 0 → a native header falls through to ELF_ENOLOAD. */
}

/* ---------------------------------------------------------------------------
 * `crash` — what has gone wrong on this machine (§M47).
 *
 * Every fault, lockup, hang, forced kill and unclean shutdown lands in the
 * crash ring; this prints it newest-first.  The point is that a user who saw
 * something misbehave can find out WHAT afterwards, instead of the event
 * existing only as a line that scrolled past on a serial console they were not
 * watching.
 * --------------------------------------------------------------------------- */
static void cmd_crash(void) {
    int n = crash_count();
    if (n == 0) {
        console_write("crash: no crashes recorded on this boot\n");
        return;
    }
    kprintf("%d crash record(s), newest first:\n", n);
    kprintf("  UPTIME    KIND          CPU  PID  NAME              DETAIL\n");
    for (int i = 0; i < n; i++) {
        const struct crash_record* r = crash_at(i);
        if (!r) break;
        kprintf("  %us  %s  %u  %d  %s  pc=%p addr=%p code=%d %s\n",
                (unsigned)(r->ms / 1000), crash_kind_name(r->kind),
                (unsigned)r->cpu, r->pid, r->comm,
                (void*)r->pc, (void*)r->addr, r->code, r->what);
    }
}

static void cmd_archtest(void) {
    uint8_t hdr[64];
    /* Two foreign shapes: the "other" word size and a plainly alien machine. */
    int is64 = (sizeof(void*) == 8);
    make_ehdr(hdr, is64 ? 1u : 2u, is64 ? 3u : 62u);      /* other class+machine */
    int foreign_other = elf_load_ex(NULL, hdr, sizeof hdr, 0, NULL);

    make_ehdr(hdr, is64 ? 2u : 1u, 183u);                 /* EM_AARCH64 */
    int foreign_arm = elf_load_ex(NULL, hdr, sizeof hdr, 0, NULL);

    /* Native class+machine — must pass the gate and fail for the NEXT reason. */
    make_ehdr(hdr, is64 ? 2u : 1u, is64 ? 62u : 3u);
    int native = elf_load_ex(NULL, hdr, sizeof hdr, 0, NULL);

    kprintf("archtest: arch=%s foreign=%d/%d (want %d) native=%d (want %d) -> %s\n",
            hal_arch_name(), foreign_other, foreign_arm, ELF_EBADARCH,
            native, ELF_ENOLOAD,
            (foreign_other == ELF_EBADARCH && foreign_arm == ELF_EBADARCH &&
             native == ELF_ENOLOAD) ? "PASS" : "FAIL");
}

static void fputest_worker(void) {
    int slot = (int)(uintptr_t)task_start_arg();
    uint64_t pattern = slot ? 0x4004000000000000ull    /* 2.5 */
                            : 0x3FF8000000000000ull;   /* 1.5 */
    uint64_t bad = 0;
    hal_fpu_test_stamp(pattern);
    for (int i = 0; i < FPUTEST_ROUNDS; i++) {
        task_yield();
        if (hal_fpu_test_read() != pattern) bad++;
    }
    g_fpu_mismatch[slot] = bad;
    g_fpu_done[slot] = 1;
}

static void cmd_fputest(void) {
    if (!hal_fpu_present()) {
        console_write("fputest: SKIP — this arch has no reachable FP unit "
                      "(see kernel/hal/aarch64/fpu.c)\n");
        return;
    }
    g_fpu_mismatch[0] = g_fpu_mismatch[1] = 0;
    g_fpu_done[0] = g_fpu_done[1] = 0;

    task_spawn_arg("fpu-a", fputest_worker, (void*)(uintptr_t)0);
    task_spawn_arg("fpu-b", fputest_worker, (void*)(uintptr_t)1);

    /* Wait for both, bounded so a wedged worker cannot hang the shell. */
    for (int spins = 0; spins < 200000 && !(g_fpu_done[0] && g_fpu_done[1]); spins++)
        task_yield();

    if (!(g_fpu_done[0] && g_fpu_done[1])) {
        console_write("fputest: workers did not finish\n");
        return;
    }
    kprintf("fputest: %d rounds x 2 tasks — mismatches a=%u b=%u -> %s\n",
            FPUTEST_ROUNDS, (unsigned)g_fpu_mismatch[0], (unsigned)g_fpu_mismatch[1],
            (g_fpu_mismatch[0] == 0 && g_fpu_mismatch[1] == 0) ? "PASS" : "FAIL");
}

/* §M53 — `ktime`: what clock is actually backing timer_now_ns, and does it
 * resolve better than the tick?  The measurement matters more than the name:
 * a source can be present and mis-calibrated, and the only way to see that is
 * to compare a measured interval against the tick that was used to calibrate
 * it.  Printing "hires counter" without proving it advances would be exactly
 * the kind of claim this project keeps learning not to trust. */
/* §M53 — `ktimer`: does the timer service actually meet its deadlines?
 *
 * The list is easy to get right and easy to believe in; the number that
 * matters is LATENESS, because the deadline is kept in nanoseconds while the
 * moment we notice it is bounded by the tick.  Measuring a spread of sleeps
 * against the clock is what turns "we have timers" into a figure someone can
 * decide on — specifically, whether replacing the periodic tick with a
 * one-shot hardware deadline is worth doing. */
static void cmd_ktimer(void) {
    uint32_t pending; uint64_t fired, late;
    ktimer_stats(&pending, &fired, &late);
    kprintf("ktimer: %u pending, %u fired, worst lateness %u us\n",
            pending, (unsigned)fired, (unsigned)(late / 1000ull));

    /* The since-boot worst figure includes boot, the busiest time the machine
     * has; the spread below is measured from a clean slate. */
    ktimer_stats_reset();
    unsigned worst_us = 0;
    static const unsigned req_us[] = { 500, 1000, 5000, 20000, 100000 };
    kprintf("  requested   actual    error\n");
    for (unsigned i = 0; i < sizeof req_us / sizeof req_us[0]; i++) {
        uint64_t want = (uint64_t)req_us[i] * 1000ull;
        uint64_t t0 = timer_now_ns();
        task_sleep_until_ns(t0 + want);
        uint64_t got = timer_now_ns() - t0;
        long err = (long)((int64_t)got - (int64_t)want) / 1000;
        if (err > 0 && (unsigned)err > worst_us) worst_us = (unsigned)err;
        kprintf("  %u us      %u us     %s%u us\n", req_us[i],
                (unsigned)(got / 1000ull), err < 0 ? "-" : "+",
                (unsigned)(err < 0 ? -err : err));
    }
    ktimer_stats(&pending, &fired, &late);
    kprintf("  after: %u fired, expiry lateness %u us, worst sleep error +%u us"
            " (floor: the tick, or the emulator's timer dispatch)\n",
            (unsigned)fired, (unsigned)(late / 1000ull), worst_us);
}

static void cmd_ktime(void) {
    kprintf("clock source : %s", timer_source_name());
    if (timer_source_hz())
        kprintf(" (%u kHz)", (unsigned)(timer_source_hz() / 1000u));
    kprintf("\n resolution  : %u ns\n", (unsigned)timer_res_ns());

    /* Back-to-back reads: with the tick these are IDENTICAL most of the time,
     * which is the whole problem in one number. */
    uint64_t a = timer_now_ns(), b = timer_now_ns(), c = timer_now_ns();
    kprintf(" back-to-back: %u ns, %u ns apart\n",
            (unsigned)(b - a), (unsigned)(c - b));

    /* Measure one tick-based sleep with the new clock.  Agreement to within a
     * tick is the calibration check; a wildly different number means the
     * counter frequency is wrong, not that the sleep is. */
    uint64_t t0 = timer_now_ns();
    task_msleep(100);
    uint64_t t1 = timer_now_ns();
    kprintf(" 100 ms sleep: measured %u us (%u ms) by the ns clock\n",
            (unsigned)((t1 - t0) / 1000ull), (unsigned)((t1 - t0) / 1000000ull));
    kprintf(" uptime      : %u ms\n", (unsigned)(t1 / 1000000ull));
}

/* --------------------------------------------------------------------
 * `abi` — show the guest-ABI translation tables (§M50).
 *
 * The point of the engine is that a platform's syscall numbering is DATA,
 * so the data should be readable.  Printing the three Linux number spaces
 * side by side is also the clearest statement of what the engine does:
 * one column per platform, one row per meaning.
 * -------------------------------------------------------------------- */
static void cmd_abi(void) {
    int have = 0, total = 0;
    abi_stats(&have, &total);
    kprintf("abi: %d/%d canonical operations have handlers\n", have, total);

    const struct abi_map* maps[] = {
        &abi_map_linux_i386, &abi_map_linux_amd64, &abi_map_linux_arm64,
    };
    const unsigned nmaps = sizeof(maps) / sizeof(maps[0]);

    /* kprintf is a minimal formatter with no width specifiers, so columns are
     * padded by hand rather than by "%-14s". */
    console_write("MEANING     ");
    for (unsigned m = 0; m < nmaps; m++) { kprintf("%s   ", maps[m]->name); }
    console_write("\n");

    /* One row per MEANING, one column per platform: the same operation under
     * three different numbers is the whole point. */
    for (uint32_t i = 0; i < maps[0]->n_ents; i++) {
        uint16_t op = maps[0]->ents[i].op;
        kprintf("op %u", (unsigned)op);
        console_write(op < 10 ? "         " : "        ");
        for (unsigned m = 0; m < nmaps; m++) {
            int found = -1;
            for (uint32_t j = 0; j < maps[m]->n_ents; j++)
                if (maps[m]->ents[j].op == op) { found = (int)maps[m]->ents[j].nr; break; }
            if (found >= 0) kprintf("%d", found); else console_write("-");
            console_write("            ");
        }
        console_write("\n");
    }
    console_write("abi: one meaning per row, one platform per column — the "
                  "difference between platforms is the table, not the code\n");
}

/* The ring-3/EL0 self-test is arch-specific (see usermode.h / the per-arch
 * arch_ringtest implementations); shell.c just invokes it. */
static void cmd_ringtest(void) { arch_ringtest(); }

/* `service [list | start|stop|restart|status <name>]` — supervisor control. */
static void cmd_service(const char* args) {
    if (!args || !*args || cmd_starts_with(args, "list")) { service_list(); return; }

    char sub[16]; int i = 0;
    while (args[i] && args[i] != ' ' && i < 15) { sub[i] = args[i]; i++; }
    sub[i] = 0;
    const char* name = args + i;
    while (*name == ' ') name++;

    if (cmd_streq(sub, "status")) {
        if (*name) service_status(name);
        else console_write("service: usage: service status <name>\n");
        return;
    }
    if (cmd_streq(sub, "start")) {
        int r = service_start(name);
        kprintf("service start %s: %s\n", name,
                r == 0 ? "ok" : (r == -2 ? "already running" : "no such service"));
        return;
    }
    if (cmd_streq(sub, "stop")) {
        int r = service_stop(name);
        kprintf("service stop %s: %s\n", name,
                r == 0 ? "ok" : (r == -2 ? "not running" : "no such service"));
        return;
    }
    if (cmd_streq(sub, "restart")) {
        int r = service_restart(name);
        kprintf("service restart %s: %s\n", name, r == 0 ? "ok" : "no such service");
        return;
    }
    console_write("service: usage: service [list | start|stop|restart|status <name>]\n");
}

/* `crontab -l` / `cron [list|status|reload]` — M30 cron control. */
static void cmd_cron(const char* args) {
    if (!args || !*args || cmd_starts_with(args, "list") || cmd_starts_with(args, "status"))
        { cron_list(); return; }
    if (cmd_starts_with(args, "reload")) { cron_reload(); console_write("cron: reloaded\n"); return; }
    console_write("cron: usage: cron [list|status|reload]\n");
}

static void cmd_uptime(void) {
    /* Format ms as h:mm:ss.mmm.  No %02u in our tiny printf, so we
     * hand-roll the leading zeros. */
    uint64_t total_ms = timer_ticks_ms();
    uint32_t ms  = (uint32_t)(total_ms % 1000);
    uint32_t sec = (uint32_t)((total_ms / 1000) % 60);
    uint32_t min = (uint32_t)((total_ms / 60000) % 60);
    uint32_t hr  = (uint32_t)(total_ms / 3600000);
    kprintf("uptime: %u:%s%u:%s%u.%s%s%u\n",
            hr,
            min < 10 ? "0" : "", min,
            sec < 10 ? "0" : "", sec,
            ms  < 100 ? "0" : "", ms < 10 ? "0" : "", ms);
}

static void cmd_about(void) {
    console_write("d-os — toy x86 kernel. multiboot1, polled PS/2, VGA text mode.\n");
}

/* M28 — dmesg: dump the klog ring, oldest → newest.  We render straight
 * to the console (NOT via kprintf) on purpose: kprintf tees into klog, so
 * printing the log with it would append every rendered line back into the
 * ring and evict the very boot messages we came to read. */
static void dmesg_put_uint(unsigned v) {
    char b[12];
    int n = 0;
    if (v == 0) { console_putchar('0'); return; }
    while (v) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) console_putchar(b[n]);
}

static void dmesg_line(const struct klog_record* r, void* ctx) {
    struct dmesg_ctx* d = (struct dmesg_ctx*)ctx;
    if ((int)r->level > d->max_level) return;          /* severity filter */
    unsigned sec = (unsigned)(r->t_ms / 1000);
    unsigned ms  = (unsigned)(r->t_ms % 1000);
    console_putchar('[');
    dmesg_put_uint(sec); console_putchar('.');
    if (ms < 100) console_putchar('0');
    if (ms <  10) console_putchar('0');
    dmesg_put_uint(ms);
    console_write("] ");
    console_write(klog_level_name(r->level)); console_putchar(' ');
    console_write(r->tag); console_write(": ");
    console_write(r->msg); console_putchar('\n');
}

/* Accept a level as a name (emerg..debug) or a digit (0..7). -1 = bad. */
static int dmesg_parse_level(const char* s) {
    static const char* const names[KLOG_NLEVELS] = {
        "emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"
    };
    if (s[0] >= '0' && s[0] <= '7' && s[1] == '\0') return s[0] - '0';
    for (int i = 0; i < KLOG_NLEVELS; i++)
        if (cmd_streq(s, names[i])) return i;
    return -1;
}

static void cmd_dmesg(const char* args) {
    struct dmesg_ctx d = { .max_level = KLOG_DEBUG };  /* show everything */
    if (args && *args) {
        if (cmd_starts_with(args, "-l ")) {
            int lv = dmesg_parse_level(args + 3);
            if (lv < 0) {
                kprintf("dmesg: unknown level '%s' (emerg..debug or 0..7)\n",
                        args + 3);
                return;
            }
            d.max_level = lv;                          /* show <= this severity */
        } else {
            kprintf("usage: dmesg [-l <level>]   level: emerg..debug or 0..7\n");
            return;
        }
    }
    klog_for_each(dmesg_line, &d);
}

/* --- registrations --------------------------------------------------------- */

static void sy_about   (const char* a) { (void)a; cmd_about();    }
static void sy_uptime  (const char* a) { (void)a; cmd_uptime();   }
static void sy_crash   (const char* a) { (void)a; cmd_crash();    }
static void sy_abi     (const char* a) { (void)a; cmd_abi();      }
static void sy_ktime   (const char* a) { (void)a; cmd_ktime();    }
static void sy_ktimer  (const char* a) { (void)a; cmd_ktimer();   }
static void sy_archtest(const char* a) { (void)a; cmd_archtest(); }
static void sy_fputest (const char* a) { (void)a; cmd_fputest();  }
static void sy_faultt  (const char* a) { (void)a; cmd_faulttest();}
static void sy_ringtest(const char* a) { (void)a; cmd_ringtest(); }
static void sy_epolltest(const char* a){ (void)a; cmd_epolltest();}

/* `clear` wipes OUR VC, not the global sinks: after vc_init the framebuffer
 * sink is inactive, so console_clear() has no visible effect in a pane anyway.
 * vc_clear also reaches GUI windows through the emit hook.
 *
 * With NO VC — the aarch64 serial REPL — the honest answer is not an error but
 * the ANSI sequence, because that shell really is talking to a terminal
 * emulator on the other end of a UART.  Reporting "no terminal" there would be
 * a command that refuses to do something it can plainly do. */
static void sy_clear(const char* a) {
    (void)a;
    struct vc* v = shell_current_vc();
    if (v) { vc_clear(v); return; }
    kprintf("\033[2J\033[H");
}

static void sy_echo(const char* a) {
    console_write(a);
    console_putchar('\n');
}

static void sy_shutdown(const char* a) {
    (void)a;
    console_write("shutting down...\n");
    /* The NVRAM marker is disarmed INSIDE system_power_off() — one route, so
     * the Start menu and this command cannot disagree about it. */
    system_power_off();                          /* normally never returns */
}

static void sy_reboot(const char* a) {
    (void)a;
    console_write("rebooting...\n");
    system_reboot();                             /* normally never returns */
}

SHELL_CMD(about)    = { "about",   "",          "what this machine is running",
                        SHELL_G_SYS, sy_about, SHELL_P_ANY };
SHELL_CMD(uptime)   = { "uptime",  "",          "how long it has been up",
                        SHELL_G_SYS, sy_uptime, SHELL_P_ANY };
SHELL_CMD(clear)    = { "clear",   "",          "clear this terminal",
                        SHELL_G_SYS, sy_clear, SHELL_P_ANY };
SHELL_CMD(echo)     = { "echo",    "<text>",    "print its arguments",
                        SHELL_G_SYS, sy_echo, SHELL_P_ANY };
SHELL_CMD(dmesg)    = { "dmesg",   "[-l <level>]", "the kernel log",
                        SHELL_G_SYS, cmd_dmesg, SHELL_P_ANY };
SHELL_CMD(crash)    = { "crash",   "",          "what has gone wrong, including on earlier boots",
                        SHELL_G_SYS, sy_crash, SHELL_P_ADMIN };
SHELL_CMD(service)  = { "service", "[list|start|stop <name>]", "supervised services",
                        SHELL_G_SYS, cmd_service, SHELL_P_ADMIN };
SHELL_CMD(cron)     = { "cron",    "[list|run <name>]", "scheduled jobs",
                        SHELL_G_SYS, cmd_cron, SHELL_P_ADMIN };
SHELL_CMD(abi)      = { "abi",     "",          "the guest-ABI number spaces, side by side",
                        SHELL_G_SYS, sy_abi, SHELL_P_ANY };
SHELL_CMD(ktime)    = { "ktime",   "",          "the monotonic clock: source and resolution",
                        SHELL_G_SYS, sy_ktime, SHELL_P_ANY };
SHELL_CMD(ktimer)   = { "ktimer",  "",          "deadline-timer accuracy over a spread of sleeps",
                        SHELL_G_SYS, sy_ktimer, SHELL_P_ANY };
SHELL_CMD(shutdown) = { "shutdown","",          "power off",
                        SHELL_G_SYS, sy_shutdown, SHELL_P_ADMIN };
SHELL_CMD(reboot)   = { "reboot",  "",          "restart the machine",
                        SHELL_G_SYS, sy_reboot, SHELL_P_ADMIN };

SHELL_CMD(ringtest)    = { "ringtest",    "", "ring 3 / EL0 excursion",
                           SHELL_G_TEST, sy_ringtest, SHELL_P_ADMIN };
SHELL_CMD(archtest)    = { "archtest",    "", "the ELF architecture gate",
                           SHELL_G_TEST, sy_archtest, SHELL_P_ADMIN };
SHELL_CMD(fputest)     = { "fputest",     "", "per-task FP/SIMD register file",
                           SHELL_G_TEST, sy_fputest, SHELL_P_ANY };
SHELL_CMD(faulttest)   = { "faulttest",   "", "bad ring-3 pointers, through all three layers",
                           SHELL_G_TEST, sy_faultt, SHELL_P_ADMIN };
SHELL_CMD(timerfdtest) = { "timerfdtest", "[ms]", "timerfd drift against the original start",
                           SHELL_G_TEST, cmd_timerfdtest, SHELL_P_ANY };
SHELL_CMD(alarmtest)   = { "alarmtest",   "[ms]", "setitimer + SIGALRM delivery",
                           SHELL_G_TEST, cmd_alarmtest, SHELL_P_ANY };
/* §M90 — `flocktest`: flock's rules, each one able to fail.  Two opens of
 * one file are two OWNERS: EX held by the first must refuse the second, both
 * as EX and as SH (LOCK_NB, so a broken lock shows as a wrong number instead
 * of a hang); after LOCK_UN the second gets it; and closing the last
 * reference of a holder must release, or a crashed holder locks the file
 * forever. */
static void sy_flocktest(const char* args) {
    (void)args;
    const char* path = "/tmp-flocktest";
    struct file* f1 = vfs_open(path, VFS_RDWR | VFS_CREATE);
    struct file* f2 = f1 ? vfs_open(path, VFS_RDWR) : NULL;
    struct ofile* a = f1 ? ofile_from_file(f1) : NULL;
    struct ofile* b = f2 ? ofile_from_file(f2) : NULL;
    if (!a || !b) { console_write("flock: FAIL (could not open the file twice)\n"); return; }
    int r1 = flock_op(a, 2);               /* EX             -> 0   */
    int r2 = flock_op(b, 2 | 4);           /* EX|NB, other    -> -11 */
    int r3 = flock_op(b, 1 | 4);           /* SH|NB vs EX     -> -11 */
    int r4 = flock_op(a, 8);               /* UN              -> 0   */
    int r5 = flock_op(b, 1 | 4);           /* SH|NB           -> 0   */
    int r6 = flock_op(a, 1 | 4);           /* SH|NB, shared   -> 0   */
    int r7 = flock_op(a, 2 | 4);           /* EX|NB vs b's SH -> -11 */
    ofile_unref(b);                        /* last close of b releases */
    int r8 = flock_op(a, 2 | 4);           /* EX|NB           -> 0   */
    ofile_unref(a);
    vfs_unlink(path);
    kprintf("flock: %d %d %d %d %d %d %d %d (want 0 -11 -11 0 0 0 -11 0)\n",
            r1, r2, r3, r4, r5, r6, r7, r8);
    int ok = r1 == 0 && r2 == -11 && r3 == -11 && r4 == 0 && r5 == 0 &&
             r6 == 0 && r7 == -11 && r8 == 0;
    console_write(ok ? "flock: ok\n" : "flock: FAIL\n");
}
/* §M90 — `unlinkopentest`: POSIX says an unlinked file lives while it is
 * open.  Open, write, unlink, then read back THROUGH THE OPEN FILE and close —
 * the old VFS freed the inode at unlink, so the read and the close touched
 * freed memory.  Also checks the name is really gone at once. */
static void sy_unlinkopentest(const char* args) {
    (void)args;
    const char* path = "/tmp-unlinkopen";
    struct file* f = vfs_open(path, VFS_RDWR | VFS_CREATE | VFS_TRUNC);
    if (!f) { console_write("unlinkopen: FAIL (create)\n"); return; }
    vfs_write(f, "still-here", 10);
    int ur = vfs_unlink(path);
    struct vfs_stat st;
    int gone = vfs_stat(path, &st) != 0;
    char buf[16] = { 0 };
    f->pos = 0;
    long n = vfs_read(f, buf, 10);
    vfs_close(f);
    int again = vfs_stat(path, &st) != 0;
    kprintf("unlinkopen: unlink=%d name-gone=%d read=%d '%s' after-close-gone=%d\n",
            ur, gone, (int)n, buf, again);
    int ok = ur == 0 && gone && n == 10 && buf[0] == 's' && buf[9] == 'e' && again;
    console_write(ok ? "unlinkopen: ok\n" : "unlinkopen: FAIL\n");
}
/* §M90 — `fifotest`: named pipes (fifo.c), every rule a program depends on,
 * checked on the real open path (vfs_open + fifo_attach, as sys_open_ex_k
 * does): mkfifo twice (EEXIST), stat says S_IFIFO, a non-blocking writer with
 * no reader (ENXIO), a non-blocking reader that sees NO hang-up before any
 * writer existed, data through, EAGAIN on an empty pipe with a live writer,
 * POLLHUP + EOF after the writer leaves, the pipe discarded with the last end,
 * and a BLOCKING reader that waits for a writer arriving 100 ms later on
 * another task — containerd's exact pattern for a container's stdout. */
static const char* const FT_PATH = "/fifotest.p";
static volatile int g_ft_writer_opened;
static int ft_open(unsigned role, int nonblock, struct ofile** out) {
    struct file* f = vfs_open(FT_PATH, (role & FIFO_W) ? ((role & FIFO_R) ? VFS_RDWR : VFS_WRONLY)
                                                     : VFS_RDONLY);
    struct ofile* o = f ? ofile_from_file(f) : NULL;
    if (!o) { if (f) vfs_close(f); return -2; }
    o->nonblock = nonblock;
    o->kind = FD_FIFO;
    int e = fifo_attach(o, role, nonblock);
    if (e < 0) { o->kind = FD_VFS; ofile_unref(o); return e; }
    *out = o;
    return 0;
}
static void ft_late_writer(void) {
    task_msleep(100);
    g_ft_writer_opened = 1;
    struct ofile* w = NULL;
    if (ft_open(FIFO_W, 0, &w) == 0) {
        fifo_write(w, "late", 4, 1);
        ofile_unref(w);
    }
}
static void sy_fifotest(const char* args) {
    (void)args;
    vfs_unlink(FT_PATH);
    int mk = vfs_mkfifo(FT_PATH), mk2 = vfs_mkfifo(FT_PATH);
    struct kstat_full k;
    int isfifo = sys_stat_full_k(FT_PATH, &k) == 0 && (k.mode & KS_IFMT) == KS_IFIFO;
    struct ofile *r = NULL, *w = NULL;
    struct ofile* tmp = NULL;
    int enxio = ft_open(FIFO_W, 1, &tmp);
    int ro = ft_open(FIFO_R, 1, &r);
    uint32_t ev0 = r ? fifo_readiness(r) : 0;            /* want: no POLLHUP */
    int wo = ft_open(FIFO_W, 1, &w);
    long wr = w ? fifo_write(w, "hello", 5, 0) : -99;
    uint32_t ev1 = r ? fifo_readiness(r) : 0;            /* want: POLLIN */
    char buf[8] = { 0 };
    long rd = r ? fifo_read(r, buf, sizeof buf - 1, 0) : -99;
    long again = r ? fifo_read(r, buf + 6, 1, 0) : -99;  /* empty, writer alive: EAGAIN */
    if (w) ofile_unref(w);
    uint32_t ev2 = r ? fifo_readiness(r) : 0;            /* want: POLLHUP */
    long eof = r ? fifo_read(r, buf + 6, 1, 0) : -99;
    if (r) ofile_unref(r);
    int enxio2 = ft_open(FIFO_W, 1, &tmp);               /* pipe gone with its last end */

    g_ft_writer_opened = 0;
    struct ofile* br = NULL;
    task_spawn_arg("fifotest-w", ft_late_writer, NULL);
    int bo = ft_open(FIFO_R, 0, &br);                    /* BLOCKS until the writer */
    int waited = g_ft_writer_opened;
    char lb[8] = { 0 };
    long lr = br ? fifo_read(br, lb, 4, 1) : -99;
    long leof = br ? fifo_read(br, lb + 5, 1, 1) : -99;  /* writer has closed: EOF */
    if (br) ofile_unref(br);
    int un = vfs_unlink(FT_PATH);

    kprintf("fifotest: mkfifo=%d again=%d S_IFIFO=%d nb-writer=%d reader=%d hup-before=%d "
            "writer=%d write=%d in=%d read=%d '%s' empty=%d hup-after=%d eof=%d gone=%d\n",
            mk, mk2, isfifo, enxio, ro, (ev0 & POLLHUP) ? 1 : 0, wo, (int)wr,
            (ev1 & POLLIN) ? 1 : 0, (int)rd, buf, (int)again, (ev2 & POLLHUP) ? 1 : 0,
            (int)eof, enxio2);
    kprintf("fifotest: blocking reader=%d waited-for-writer=%d read=%d '%s' eof=%d unlink=%d\n",
            bo, waited, (int)lr, lb, (int)leof, un);
    int ok = mk == 0 && mk2 == -2 && isfifo && enxio == -6 && ro == 0 && !(ev0 & POLLHUP) &&
             wo == 0 && wr == 5 && (ev1 & POLLIN) && rd == 5 && buf[0] == 'h' && buf[4] == 'o' &&
             again == -11 && (ev2 & POLLHUP) && eof == 0 && enxio2 == -6 &&
             bo == 0 && waited && lr == 4 && lb[0] == 'l' && lb[3] == 'e' && leof == 0 && un == 0;
    console_write(ok ? "fifotest: ok\n" : "fifotest: FAIL\n");
}
/* §M90 — `subreapertest`: prctl(PR_SET_CHILD_SUBREAPER) as containerd's shim
 * uses it.  A helper ("sr") marks itself a subreaper and starts "mid"; mid
 * starts "leaf" (detached, as a setsid/setpgid program is) and exits at once.
 * The leaf must then belong to sr — not to init — and sr must collect its
 * exit status with an ordinary wait.  The control run does the same without
 * the subreaper mark: the leaf must go to init.  Kernel tasks drive it so the
 * test exercises exactly the adoption code in task.c. */
static volatile int g_srt_leaf, g_srt_mark;
static void srt_leaf(void) { task_msleep(150); task_exit_code(7); }
static void srt_mid(void) {
    struct task* l = task_spawn("srt-leaf", srt_leaf);
    if (l) { l->survives_parent = 1; task_set_reap_owned(l, 1); g_srt_leaf = l->pid; }
}
static volatile int g_srt_ppid, g_srt_code, g_srt_done;
static void srt_sr(void) {
    struct task* me = task_current();
    me->child_subreaper = g_srt_mark;
    g_srt_leaf = 0;
    struct task* m = task_spawn("srt-mid", srt_mid);
    int mpid = m ? m->pid : -1;
    int code = -1;
    if (mpid > 0) task_wait(mpid, &code);             /* mid has gone */
    for (int i = 0; i < 50 && !g_srt_leaf; i++) task_msleep(2);
    struct task* l = g_srt_leaf ? task_find(g_srt_leaf) : NULL;
    g_srt_ppid = l ? l->ppid : -1;
    g_srt_code = -1;
    if (g_srt_mark && l) task_wait(g_srt_leaf, (int*)&g_srt_code);
    else if (l) { task_set_reap_owned(l, 0); }        /* control: init collects it */
    g_srt_done = 1;
}
static int srt_run(int mark, int* sr_pid) {
    g_srt_mark = mark; g_srt_done = 0; g_srt_ppid = -1;
    struct task* sr = task_spawn("srt-sr", srt_sr);
    if (!sr) return -1;
    *sr_pid = sr->pid;
    for (int i = 0; i < 300 && !g_srt_done; i++) task_msleep(5);
    return g_srt_done ? 0 : -1;
}
static void sy_subreapertest(const char* args) {
    (void)args;
    int sr1 = 0, sr2 = 0;
    int r1 = srt_run(1, &sr1);
    int p1 = g_srt_ppid, c1 = g_srt_code;
    int r2 = srt_run(0, &sr2);
    int p2 = g_srt_ppid;
    int init = task_reaper_pid();
    kprintf("subreaper: with mark: done=%d leaf ppid %d (subreaper %d) code %d (want 7); "
            "without: done=%d leaf ppid %d (init %d)\n", r1 == 0, p1, sr1, c1, r2 == 0, p2, init);
    int ok = r1 == 0 && p1 == sr1 && c1 == 7 && r2 == 0 && p2 == init;
    console_write(ok ? "subreaper: ok\n" : "subreaper: FAIL\n");
}
/* §M90 — `oomtest`: /proc/<pid>/oom_score_adj as containerd's shim uses it —
 * written for the caller, read back through /proc/self and /proc/<own pid>,
 * inherited by a child, read for that child through /proc/<ITS pid> (a
 * FOREIGN pid: procfs generates for the process the path named), a value out
 * of range refused, and a per-process file NOT offered under a foreign pid
 * (/proc/<pid>/mountinfo) answering "no such file" rather than the caller's. */
static void bt_path(char* out, const char* a, int n1, const char* b, int n2, const char* c);
static long ot_read(const char* path) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -99999;
    char b[16] = { 0 };
    long n = vfs_read(f, b, sizeof b - 1);
    vfs_close(f);
    if (n <= 0) return -99998;
    long v = 0; int i = 0, neg = 0;
    if (b[0] == '-') { neg = 1; i = 1; }
    for (; b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
    return neg ? -v : v;
}
static long ot_write(const char* path, const char* v) {
    struct file* f = vfs_open(path, VFS_WRONLY);
    if (!f) return -99999;
    size_t len = 0; while (v[len]) len++;
    long n = vfs_write(f, v, len);
    vfs_close(f);
    return n;
}
static void ot_child(void) { task_msleep(300); }
static void sy_oomtest(const char* args) {
    (void)args;
    struct task* me = task_current();
    int saved = me->oom_score_adj;
    char p[48], q[48], r[48];
    bt_path(p, "/proc/", task_tgid(me), "/oom_score_adj", -1, "");
    long w1 = ot_write("/proc/self/oom_score_adj", "-250\n");
    long a = ot_read("/proc/self/oom_score_adj"), b = ot_read(p);
    struct task* c = task_spawn("oomtest-child", ot_child);
    long cv = -1, bad = 0, foreign_mi = 0, nul = 0;
    if (c) {
        bt_path(q, "/proc/", c->pid, "/oom_score_adj", -1, "");
        bt_path(r, "/proc/", c->pid, "/mountinfo", -1, "");
        cv = ot_read(q);
        struct file* f = vfs_open(r, VFS_RDONLY);
        foreign_mi = f ? 1 : 0;
        if (f) vfs_close(f);
    }
    /* written as runc's nsexec does: with the string's terminating NUL */
    struct file* nf = vfs_open("/proc/self/oom_score_adj", VFS_WRONLY);
    nul = nf ? vfs_write(nf, "-250\0", 5) : -1;
    if (nf) vfs_close(nf);
    bad = ot_write("/proc/self/oom_score_adj", "5000");
    long after = ot_read("/proc/self/oom_score_adj");
    me->oom_score_adj = saved;
    kprintf("oomtest: write=%d self=%d own-pid=%d child(foreign pid)=%d with-NUL=%d out-of-range=%d "
            "still=%d foreign-mountinfo-opened=%d\n",
            (int)w1, (int)a, (int)b, (int)cv, (int)nul, (int)bad, (int)after, (int)foreign_mi);
    int ok = w1 == 5 && a == -250 && b == -250 && cv == -250 && nul == 5 && bad < 0 &&
             after == -250 && !foreign_mi;
    console_write(ok ? "oomtest: ok\n" : "oomtest: FAIL\n");
}
/* §M90 — `peercredtest`: SO_PEERCRED as containerd's ttrpc reads it.  Both
 * ends of a socketpair name the caller; through a NAMED listener the client
 * sees the listener's owner and the accepted end sees the connecting process;
 * an endpoint never connected has no peer (-1).  All in one task, so every
 * recorded pid must equal the caller's process id — a zero (the old "no
 * credentials") or a stale value fails it. */
static void sy_peercredtest(const char* args) {
    (void)args;
    struct task* me = task_current();
    int mypid = task_tgid(me);
    struct usock *a = NULL, *b = NULL;
    int pa = -9, ua = -9, ga = -9, pb = -9, ub = -9, gb = -9;
    int rp = usock_pair(&a, &b);
    int ra = a ? usock_peercred(a, &pa, &ua, &ga) : -9;
    int rb = b ? usock_peercred(b, &pb, &ub, &gb) : -9;
    if (a) usock_close(a);
    if (b) usock_close(b);
    struct usock* l = usock_new();
    struct usock* cl = usock_new();
    struct usock* lone = usock_new();
    int pc = -9, uc = -9, gc = -9, ps = -9, us = -9, gs = -9, dummy;
    int rl = -9, rc = -9, rs = -9, rn = 0;
    struct usock* srv = NULL;
    if (l && cl && lone && usock_bind(l, "@peercredtest") == 0 && usock_listen(l, 2) == 0 &&
        usock_connect(cl, "@peercredtest") == 0 && usock_accept(l, 0, &srv) == 0 && srv) {
        rl = 0;
        rc = usock_peercred(cl, &pc, &uc, &gc);
        rs = usock_peercred(srv, &ps, &us, &gs);
    }
    rn = lone ? usock_peercred(lone, &dummy, &dummy, &dummy) : 0;
    if (srv) usock_close(srv);
    if (cl) usock_close(cl);
    if (l) usock_close(l);
    if (lone) usock_close(lone);
    kprintf("peercred: me %d; pair %d/%d pids %d %d; listener %d client->%d server->%d; "
            "unconnected %d\n", mypid, ra, rb, pa, pb, rl, pc, ps, rn);
    int ok = rp == 0 && ra == 0 && rb == 0 && pa == mypid && pb == mypid &&
             rl == 0 && rc == 0 && rs == 0 && pc == mypid && ps == mypid &&
             ua == uc && ga == gs && rn == -1;
    console_write(ok ? "peercred: ok\n" : "peercred: FAIL\n");
}
/* §M90 — `nstest`: namespaces as objects (nsproxy.c).  A helper task
 * unshares UTS, IPC, cgroup and time; its UTS number changes and its own
 * hostname does not touch the host's; /proc/self/ns/uts reads the new number;
 * the TIME namespace applies to its CHILD only (time_for_children); and
 * setns() through the SHELL's handle (/proc/<shell pid>/ns/uts, a foreign
 * pid) takes it back to the host's namespace and name. */
static volatile int g_nst_done, g_nst_ok;
static volatile int g_nst_shell;
static char g_nst_line[192];
static void nst_child(void) { task_msleep(200); }
static void nst_helper(void) {
    struct task* me = task_current();
    uint32_t uts0 = ns_ino_of(me, NSK_UTS), tm0 = ns_ino_of(me, NSK_TIME);
    int u = ns_unshare(me, NS_CLONE_NEWUTS | NS_CLONE_NEWIPC | NS_CLONE_NEWCGROUP | NS_CLONE_NEWTIME);
    uint32_t uts1 = ns_ino_of(me, NSK_UTS);
    int hs = ns_set_hostname(me, "box", 3);
    struct task* sh = task_find(g_nst_shell);
    int mine = ns_hostname(me)[0] == 'b', host_kept = sh && ns_hostname(sh)[0] == 'd';
    int time_self_same = ns_ino_of(me, NSK_TIME) == tm0;
    int time_child_new = ns_child_ino_of(me, NSK_TIME) != tm0;
    struct task* ch = task_spawn("nstest-child", nst_child);
    int child_in = ch && ns_ino_of(ch, NSK_TIME) == ns_child_ino_of(me, NSK_TIME);
    /* /proc/self/ns/uts names the new namespace */
    char b[32] = { 0 };
    struct file* f = vfs_open("/proc/self/ns/uts", VFS_RDONLY);
    if (f) { vfs_read(f, b, sizeof b - 1); vfs_close(f); }
    unsigned long shown = 0;
    for (int i = 0; b[i]; i++) if (b[i] >= '0' && b[i] <= '9') shown = shown * 10 + (unsigned long)(b[i] - '0');
    /* setns back through the shell's handle */
    char hp[48];
    bt_path(hp, "/proc/", g_nst_shell, "/ns/uts", -1, "");
    struct file* hf = vfs_open(hp, VFS_RDONLY);
    int kind = -1, mnt = 0; struct nsobj* o = (struct nsobj*)1; uint32_t hino = 0;
    int is_h = hf && procfs_ns_handle(hf, &kind, &o, &mnt, &hino);
    if (is_h) ns_enter(me, kind, o);
    if (hf) vfs_close(hf);
    int back = ns_ino_of(me, NSK_UTS) == uts0 && ns_hostname(me)[0] == 'd';
    kprintf("nstest: unshare=%d uts %u -> %u (handle shows %u) sethostname=%d own=%d host-kept=%d "
            "time: self-same=%d children-new=%d child-in=%d; setns handle=%d kind=%d back=%d\n",
            u, uts0, uts1, (unsigned)shown, hs, mine, host_kept, time_self_same, time_child_new,
            child_in, is_h, kind, back);
    g_nst_line[0] = 1;
    g_nst_ok = u == 0 && uts1 != uts0 && shown == uts1 && hs == 0 && mine && host_kept &&
               time_self_same && time_child_new && child_in && is_h && kind == NSK_UTS && back;
    g_nst_done = 1;
}
static void sy_nstest(const char* args) {
    (void)args;
    g_nst_done = 0; g_nst_ok = 0; g_nst_line[0] = 0;
    g_nst_shell = task_tgid(task_current());
    task_spawn("nstest", nst_helper);
    for (int i = 0; i < 300 && !g_nst_done; i++) task_msleep(5);
    if (!g_nst_line[0]) kprintf("nstest: (helper did not finish)\n");
    console_write(g_nst_done && g_nst_ok ? "nstest: ok\n" : "nstest: FAIL\n");
}
/* §M90 — `seqpackettest`: SOCK_SEQPACKET keeps message boundaries.  Two sends
 * arrive as two receives (a stream would hand back "abcde" in one), a message
 * read into a short buffer loses its tail instead of leaking it into the next
 * read, and an empty ring then has nothing (not the dropped tail). */
static void sy_seqpackettest(const char* args) {
    (void)args;
    struct usock *a = NULL, *b = NULL;
    if (usock_pair(&a, &b) != 0) { console_write("seqpacket: FAIL (pair)\n"); return; }
    usock_set_seqpacket(a, b);
    char r[16];
    long s1 = usock_send(a, "ab", 2, NULL), s2 = usock_send(a, "cde", 3, NULL);
    long r1 = usock_recv(b, r, sizeof r, 0, NULL);
    int m1 = r1 == 2 && r[0] == 'a' && r[1] == 'b';
    long r2 = usock_recv(b, r, sizeof r, 0, NULL);
    int m2 = r2 == 3 && r[0] == 'c' && r[2] == 'e';
    long s3 = usock_send(a, "hello", 5, NULL);
    long r3 = usock_recv(b, r, 2, 0, NULL);                 /* truncated to "he" */
    int m3 = r3 == 2 && r[0] == 'h' && r[1] == 'e';
    long r4 = usock_recv(b, r, sizeof r, 0, NULL);          /* the tail is gone */
    usock_close(a); usock_close(b);
    kprintf("seqpacket: send %d %d -> recv %d %d; 'hello' into 2 -> %d, then %d (want 2 3 2 0)\n",
            (int)s1, (int)s2, (int)r1, (int)r2, (int)r3, (int)r4);
    int ok = s1 == 2 && s2 == 3 && m1 && m2 && s3 == 5 && m3 && r4 == 0;
    console_write(ok ? "seqpacket: ok\n" : "seqpacket: FAIL\n");
}
/* §M90 — `memfdtest`: a memfd behaves as a FILE.  Write 10 000 bytes (three
 * pages, so a frame boundary is crossed twice), seek back and read them
 * whole, fstat reports the size, a shrink moves the end and zeroes what it
 * cut off, and the seals are ENFORCED: after GROW|SHRINK|WRITE a write and a
 * truncate are refused, and after SEAL no further seal is accepted.  The
 * whole runc chain depends on this: it copies itself into one and runs it. */
static void sy_memfdtest(const char* args) {
    (void)args;
    int fd = sys_memfd(0);
    struct ofile* o = fd >= 0 ? fd_lookup(fd) : NULL;
    if (!o || !o->shm) { console_write("memfd: FAIL (create)\n"); return; }
    static uint8_t buf[10000], back[10000];
    for (int i = 0; i < 10000; i++) buf[i] = (uint8_t)(i * 7 + 3);
    long w = sys_write_k(fd, buf, sizeof buf);
    long sk = sys_lseek(fd, 0, SEEK_SET);
    long r = sys_read_k(fd, back, sizeof back);
    int same = r == 10000;
    for (int i = 0; same && i < 10000; i++) if (back[i] != buf[i]) same = 0;
    struct kstat_full k;
    int st = sys_fstat_full_k(fd, &k) == 0 && k.size == 10000 && (k.mode & KS_IFMT) == KS_IFREG;
    int sh = shm_truncate(o->shm, 5000);
    uint8_t z = 0xFF;
    int t2 = shm_truncate(o->shm, 6000);
    shm_read(o->shm, 5500, &z, 1);                      /* past the old cut: zero */
    o->shm->seals |= SHM_SEAL_GROW | SHM_SEAL_SHRINK | SHM_SEAL_WRITE;
    long wsealed = shm_write(o->shm, 0, "x", 1);
    int tsealed = shm_truncate(o->shm, 100);
    o->shm->seals |= SHM_SEAL_SEAL;
    sys_close(fd);
    kprintf("memfd: write=%d seek=%d read=%d same=%d fstat=%d shrink=%d regrow=%d zero=%d "
            "sealed-write=%d sealed-truncate=%d\n", (int)w, (int)sk, (int)r, same, st, sh, t2,
            z == 0, (int)wsealed, tsealed);
    int ok = w == 10000 && sk == 0 && same && st && sh == 0 && t2 == 0 && z == 0 &&
             wsealed == -1 && tsealed == -1;
    console_write(ok ? "memfd: ok\n" : "memfd: FAIL\n");
}
/* §M90 — `pidnstest`: pid namespaces (nsproxy.c).  H unshares NEWPID and stays
 * where it was; its first child C1 is number 1 inside, sees H as 0 (outside)
 * while H sees C1 by its global pid; C1's child C2 is 2 inside and resolves
 * from inside; C2's detached child C3 is adopted by C1 — the namespace's init,
 * not the machine's — when C2 dies; and C1's death takes C3 down with it. */
static volatile int g_pn_c1, g_pn_c2, g_pn_c3, g_pn_c1_self, g_pn_c1_seesh, g_pn_c2_in,
                    g_pn_res2, g_pn_c3_ppid, g_pn_c1_go, g_pn_c2_go;
static void pn_c3(void) { for (int i = 0; i < 400; i++) task_msleep(5); }
static void pn_c2(void) {
    struct task* me = task_current();
    g_pn_c2_in = ns_vnr(me, me);
    struct task* c3 = task_spawn("pidns-c3", pn_c3);
    if (c3) { c3->survives_parent = 1; g_pn_c3 = c3->pid; }
    while (!g_pn_c2_go) task_msleep(2);              /* then exit: C3 is orphaned */
}
static void pn_c1(void) {
    struct task* me = task_current();
    g_pn_c1_self = ns_vnr(me, me);
    struct task* h = task_find(me->ppid);
    g_pn_c1_seesh = h ? ns_vnr(me, h) : -1;
    struct task* c2 = task_spawn("pidns-c2", pn_c2);
    if (c2) g_pn_c2 = c2->pid;
    task_msleep(30);
    g_pn_res2 = ns_pid_resolve(me, 2);
    g_pn_c2_go = 1;
    task_msleep(60);                                 /* C2 is gone: who has C3? */
    struct task* c3 = g_pn_c3 ? task_find(g_pn_c3) : NULL;
    g_pn_c3_ppid = c3 ? c3->ppid : -1;
    while (!g_pn_c1_go) task_msleep(2);              /* then the init dies */
}
static volatile int g_pn_lvl_h, g_pn_hsees, g_pn_done;
static void pn_h(void) {
    struct task* me = task_current();
    int e = ns_unshare(me, NS_CLONE_NEWPID);
    g_pn_lvl_h = e ? -1 : ns_pid_level(me);
    struct task* c1 = task_spawn("pidns-c1", pn_c1);
    if (c1) { g_pn_c1 = c1->pid; g_pn_hsees = ns_vnr(me, c1); }
    g_pn_done = 1;
    for (int i = 0; i < 300; i++) task_msleep(5);
}
static void sy_pidnstest(const char* args) {
    (void)args;
    g_pn_c1 = g_pn_c2 = g_pn_c3 = 0; g_pn_c1_go = g_pn_c2_go = 0; g_pn_done = 0;
    g_pn_c3_ppid = g_pn_res2 = g_pn_c1_self = g_pn_c1_seesh = g_pn_c2_in = -9;
    task_spawn("pidns-h", pn_h);
    for (int i = 0; i < 100 && !(g_pn_done && g_pn_c3_ppid != -9); i++) task_msleep(5);
    int c3_alive_before = g_pn_c3 && task_find(g_pn_c3) && task_find(g_pn_c3)->state != TASK_DEAD;
    g_pn_c1_go = 1;                                  /* the init exits */
    task_msleep(150);
    struct task* c3 = g_pn_c3 ? task_find(g_pn_c3) : NULL;
    int c3_gone = !c3 || c3->state == TASK_DEAD || c3->kill_pending;
    kprintf("pidns: H level %d; C1 self %d (H sees %d, global %d); C1 sees H as %d; C2 inside %d, "
            "resolve(2)=%d (C2 %d); C3 adopted by %d (C1 %d); C3 alive %d, gone after init %d\n",
            g_pn_lvl_h, g_pn_c1_self, g_pn_hsees, g_pn_c1, g_pn_c1_seesh, g_pn_c2_in,
            g_pn_res2, g_pn_c2, g_pn_c3_ppid, g_pn_c1, c3_alive_before, c3_gone);
    int ok = g_pn_lvl_h == 0 && g_pn_c1_self == 1 && g_pn_hsees == g_pn_c1 && g_pn_c1_seesh == 0 &&
             g_pn_c2_in == 2 && g_pn_res2 == g_pn_c2 && g_pn_c3_ppid == g_pn_c1 &&
             c3_alive_before && c3_gone;
    console_write(ok ? "pidns: ok\n" : "pidns: FAIL\n");
}
/* §M90 — `fdlinktest`: what /proc/<pid>/fd/N names, per descriptor kind,
 * worded as Linux words it (runc identifies a container's stdio pipes this
 * way).  Both ends of one pipe must name the SAME object. */
static int fl_starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static void sy_fdlinktest(const char* args) {
    (void)args;
    struct task* me = task_current();
    int p[2] = { -1, -1 }, sp[2] = { -1, -1 };
    sys_pipe_k(p);
    sys_socketpair_k(sp);
    int ef = sys_eventfd_create(0, 0), mf = sys_memfd(0);
    int ff = sys_open_k("/fdlinktest.f", VFS_RDWR | VFS_CREATE);
    char a[64], b[64], c[64], d[64], e[64], f[64], g[64];
    sys_fd_link_of(me, p[0], a, sizeof a); sys_fd_link_of(me, p[1], b, sizeof b);
    sys_fd_link_of(me, sp[0], c, sizeof c); sys_fd_link_of(me, ef, d, sizeof d);
    sys_fd_link_of(me, mf, e, sizeof e);   sys_fd_link_of(me, ff, f, sizeof f);
    int con = me->fds[2] ? 1 : (sys_fd_link_of(me, 2, g, sizeof g) == 0 && fl_starts(g, "/dev/console"));
    int same = 1;
    for (int i = 0; a[i] || b[i]; i++) if (a[i] != b[i]) { same = 0; break; }
    kprintf("fdlink: pipe '%s' '%s' socket '%s' eventfd '%s' memfd '%s' file '%s'\n", a, b, c, d, e, f);
    int ok = fl_starts(a, "pipe:[") && same && fl_starts(c, "socket:[") &&
             fl_starts(d, "anon_inode:[eventfd]") && fl_starts(e, "/memfd:") &&
             fl_starts(f, "/fdlinktest.f") && con;
    sys_close(p[0]); sys_close(p[1]); sys_close(sp[0]); sys_close(sp[1]);
    sys_close(ef); sys_close(mf); sys_close(ff);
    vfs_unlink("/fdlinktest.f");
    console_write(ok ? "fdlink: ok\n" : "fdlink: FAIL\n");
}
/* §M90 — `renametest`: rename across directories (docker writes a layer's
 * metadata in tmp/ and renames it into the store), a directory over an EMPTY
 * directory, and the two refusals that keep the tree a tree: a directory into
 * its own subtree, and over a non-empty directory. */
static void sy_renametest(const char* args) {
    (void)args;
    struct vfs_stat st;
    vfs_mkdir("/rt");
    vfs_mkdir("/rt/a");
    vfs_mkdir("/rt/b");
    vfs_mkdir("/rt/b/full");
    vfs_create("/rt/b/full/x");
    vfs_create("/rt/a/f");
    int slash = vfs_stat("/rt/b/", &st) == 0 && vfs_stat("/rt/b//", &st) == 0;  /* "dir/" = "dir" */
    int r1 = vfs_rename_replace("/rt/a", "/rt/b/moved");              /* cross-dir dir */
    int ok1 = r1 == 0 && vfs_stat("/rt/b/moved/f", &st) == 0 && vfs_stat("/rt/a", &st) != 0;
    int r2 = vfs_rename_replace("/rt/b", "/rt/b/moved/inside");       /* into itself */
    vfs_mkdir("/rt/empty");
    int r3 = vfs_rename_replace("/rt/b/moved", "/rt/empty");          /* over empty dir */
    int ok3 = r3 == 0 && vfs_stat("/rt/empty/f", &st) == 0 && vfs_stat("/rt/b/moved", &st) != 0;
    int r4 = vfs_rename_replace("/rt/empty", "/rt/b/full");           /* over non-empty */
    int r5 = vfs_rename_replace("/rt/empty/f", "/rt/f2");             /* cross-dir file */
    int ok5 = r5 == 0 && vfs_stat("/rt/f2", &st) == 0;
    kprintf("renametest: crossdir=%d into-self=%d over-empty=%d over-full=%d file=%d slash=%d\n",
            r1, r2, r3, r4, r5, slash);
    vfs_unlink_recursive("/rt");
    int ok = ok1 && r2 == -1 && ok3 && r4 == -4 && ok5 && slash;
    console_write(ok ? "renametest: ok\n" : "renametest: FAIL\n");
}
SHELL_CMD(renametest) = { "renametest", "", "rename across directories, and its refusals",
                          SHELL_G_TEST, sy_renametest, SHELL_P_ANY };
/* "<a><n1><b>[<n2>]<c>" — this kernel has no snprintf. */
static void bt_num(char** o, int v) {
    char d[12]; int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *(*o)++ = d[--n];
}
static void bt_path(char* out, const char* a, int n1, const char* b, int n2, const char* c) {
    char* o = out;
    while (*a) *o++ = *a++;
    bt_num(&o, n1);
    while (*b) *o++ = *b++;
    if (n2 >= 0) bt_num(&o, n2);
    while (*c) *o++ = *c++;
    *o = 0;
}
/* §M90 — `bindtest`: bind mounts and mount namespaces.  A directory bind and
 * a FILE bind (a namespace handle, dockerd's use), a bind target refuses
 * unlink, and visibility runs one way: a namespace created after a bind sees
 * it, a bind made inside that namespace is invisible to its parent. */
static void sy_bindtest(const char* args) {
    (void)args;
    struct vfs_stat st;
    struct task* me = task_current();
    int home = me->mntns;
    vfs_mkdir("/bt"); vfs_mkdir("/bt/src"); vfs_mkdir("/bt/tgt"); vfs_mkdir("/bt/tgt2");
    vfs_create("/bt/src/f");
    vfs_create("/bt/file");
    int b1 = vfs_bind("/bt/src", "/bt/tgt");
    int see1 = vfs_stat("/bt/tgt/f", &st) == 0;
    int b2 = vfs_bind("/proc/self/ns/net", "/bt/file");
    char buf[32] = { 0 };
    struct file* f = vfs_open("/bt/file", VFS_RDONLY);
    if (f) { vfs_read(f, buf, sizeof buf - 1); vfs_close(f); }
    int fileok = buf[0] == 'n' && buf[1] == 'e' && buf[2] == 't' && buf[3] == ':';
    int busy = vfs_unlink("/bt/tgt");
    int ns = vfs_mntns_new(home);
    me->mntns = ns;
    int child_sees = vfs_stat("/bt/tgt/f", &st) == 0;          /* made before: visible */
    int b3 = vfs_bind("/bt/src", "/bt/tgt2");
    int child_own = vfs_stat("/bt/tgt2/f", &st) == 0;
    me->mntns = home;
    int parent_blind = vfs_stat("/bt/tgt2/f", &st) != 0;       /* made inside: invisible */
    int u_wrong = vfs_unbind("/bt/tgt2");                     /* not ours to remove */
    me->mntns = ns;  int u2 = vfs_unbind("/bt/tgt2");  me->mntns = home;
    int u1 = vfs_unbind("/bt/tgt"), u3 = vfs_unbind("/bt/file");
    int gone = vfs_stat("/bt/tgt/f", &st) != 0;
    kprintf("bindtest: dir=%d see=%d file=%d '%s' busy=%d ns=%d child-sees=%d child-bind=%d "
            "child-own=%d parent-blind=%d unbind-foreign=%d unbind=%d/%d/%d gone=%d\n",
            b1, see1, b2, fileok ? "net:" : "?", busy, ns, child_sees, b3, child_own,
            parent_blind, u_wrong, u1, u2, u3, gone);
    /* the /proc spellings Go uses for its own threads, and one it must not get */
    char pa[64], pb[64], pc[64];
    bt_path(pa, "/proc/", task_tgid(me), "/task/", me->pid, "/ns/net");
    bt_path(pb, "/proc/", task_tgid(me), "/status", -1, "");
    bt_path(pc, "/proc/", task_tgid(me) + 7777, "/status", -1, "");
    int al = vfs_stat(pa, &st) == 0 && vfs_stat(pb, &st) == 0 &&
             vfs_stat("/proc/thread-self/ns/net", &st) == 0 && vfs_stat(pc, &st) != 0;
    kprintf("bindtest: proc aliases %s\n", al ? "ok" : "WRONG");
    vfs_unlink_recursive("/bt");
    int ok = al && b1 == 0 && see1 && b2 == 0 && fileok && busy == -6 && ns > 0 && child_sees &&
             b3 == 0 && child_own && parent_blind && u_wrong == -1 && u1 == 0 && u2 == 0 &&
             u3 == 0 && gone;
    console_write(ok ? "bindtest: ok\n" : "bindtest: FAIL\n");
}
SHELL_CMD(bindtest) = { "bindtest", "", "bind mounts and mount-namespace visibility",
                        SHELL_G_TEST, sy_bindtest, SHELL_P_ANY };
SHELL_CMD(subreapertest) = { "subreapertest", "", "orphans go to the nearest child subreaper",
                              SHELL_G_TEST, sy_subreapertest, SHELL_P_ANY };
SHELL_CMD(oomtest) = { "oomtest", "", "/proc/<pid>/oom_score_adj: write, inherit, foreign read",
                        SHELL_G_TEST, sy_oomtest, SHELL_P_ANY };
SHELL_CMD(peercredtest) = { "peercredtest", "", "SO_PEERCRED on unix sockets",
                             SHELL_G_TEST, sy_peercredtest, SHELL_P_ANY };
SHELL_CMD(nstest) = { "nstest", "", "UTS/IPC/cgroup/time namespaces, handles, setns",
                       SHELL_G_TEST, sy_nstest, SHELL_P_ANY };
SHELL_CMD(seqpackettest) = { "seqpackettest", "", "SOCK_SEQPACKET keeps message boundaries",
                              SHELL_G_TEST, sy_seqpackettest, SHELL_P_ANY };
SHELL_CMD(memfdtest) = { "memfdtest", "", "a memfd as a file: read/write/seek/truncate/seals",
                          SHELL_G_TEST, sy_memfdtest, SHELL_P_ANY };
SHELL_CMD(pidnstest) = { "pidnstest", "", "pid namespaces: numbers, visibility, init, orphans",
                          SHELL_G_TEST, sy_pidnstest, SHELL_P_ANY };
SHELL_CMD(fdlinktest) = { "fdlinktest", "", "/proc/<pid>/fd/N names per descriptor kind",
                           SHELL_G_TEST, sy_fdlinktest, SHELL_P_ANY };
SHELL_CMD(fifotest) = { "fifotest", "", "named pipes: open rules, EOF/HUP, a blocking reader",
                         SHELL_G_TEST, sy_fifotest, SHELL_P_ANY };
SHELL_CMD(unlinkopentest) = { "unlinkopentest", "", "an unlinked file stays readable while open",
                              SHELL_G_TEST, sy_unlinkopentest, SHELL_P_ANY };
SHELL_CMD(flocktest)   = { "flocktest",   "", "flock(2): owners, sharing, release on close",
                           SHELL_G_TEST, sy_flocktest, SHELL_P_ANY };
SHELL_CMD(epolltest)   = { "epolltest",   "", "readiness sets, and the scan cost measured",
                           SHELL_G_TEST, sy_epolltest, SHELL_P_ANY };

/* §M71 — `boundarytest on|off`: the falsifier for the ring-3 boundary audit's
 * leaked-flag violation.  Hidden from `help`, like `hardlock` and `leaktest`:
 * reachable for the harness, not advertised. */
void usyscall_boundary_test(int on);
static void sy_boundarytest(const char* a) {
    usyscall_boundary_test(!(a[0] == 'o' && a[1] == 'f'));
}
SHELL_CMD(boundarytest) = { "boundarytest", "on|off", 0, SHELL_G_TEST, sy_boundarytest, SHELL_P_ADMIN };
