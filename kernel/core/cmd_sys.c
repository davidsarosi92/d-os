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

    /* --- 4. edge-triggered is REFUSED, not silently downgraded --------- */
    int rc = sys_epoll_ctl_k(ep, EPOLL_CTL_MOD, tfd, POLLIN | EPOLLET, 0);
    if (rc >= 0) {
        console_write("epoll: FAIL (EPOLLET accepted — a program written for "
                      "it would spin)\n");
        ok = 0;
    } else {
        kprintf("epoll: EPOLLET refused with %d (correct)\n", rc);
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
    me->mm = s;
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
    me->mm = prev;
    vmm_space_destroy(s);

    kprintf("faulttest: gate   write(kernel ptr)=%ld open(unmapped)=%d stat(unmapped)=%d -> %s\n",
            w_kern, o_unmap, s_unmap,
            (w_kern < 0 && o_unmap < 0 && s_unmap < 0) ? "PASS" : "FAIL");
    kprintf("faulttest: fixup  copy_in=%d copy_out=%d str_in=%ld -> %s\n",
            cp_in, cp_out, str_in,
            (cp_in < 0 && cp_out < 0 && str_in < 0) ? "PASS" : "FAIL");
    kprintf("faulttest: bounce write(valid)=%ld write(straddle)=%ld mid-copy=%d partial=%d -> %s\n",
            w_ok, w_strad, cp_partial, partial_ok,
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

    static const unsigned req_us[] = { 500, 1000, 5000, 20000, 100000 };
    kprintf("  requested   actual    error\n");
    for (unsigned i = 0; i < sizeof req_us / sizeof req_us[0]; i++) {
        uint64_t want = (uint64_t)req_us[i] * 1000ull;
        uint64_t t0 = timer_now_ns();
        task_sleep_until_ns(t0 + want);
        uint64_t got = timer_now_ns() - t0;
        long err = (long)((int64_t)got - (int64_t)want) / 1000;
        kprintf("  %u us      %u us     %s%u us\n", req_us[i],
                (unsigned)(got / 1000ull), err < 0 ? "-" : "+",
                (unsigned)(err < 0 ? -err : err));
    }
    ktimer_stats(&pending, &fired, &late);
    kprintf("  after: %u fired, worst lateness %u us (floor = one tick)\n",
            (unsigned)fired, (unsigned)(late / 1000ull));
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
                        SHELL_G_SYS, sy_about };
SHELL_CMD(uptime)   = { "uptime",  "",          "how long it has been up",
                        SHELL_G_SYS, sy_uptime };
SHELL_CMD(clear)    = { "clear",   "",          "clear this terminal",
                        SHELL_G_SYS, sy_clear };
SHELL_CMD(echo)     = { "echo",    "<text>",    "print its arguments",
                        SHELL_G_SYS, sy_echo };
SHELL_CMD(dmesg)    = { "dmesg",   "[-l <level>]", "the kernel log",
                        SHELL_G_SYS, cmd_dmesg };
SHELL_CMD(crash)    = { "crash",   "",          "what has gone wrong, including on earlier boots",
                        SHELL_G_SYS, sy_crash };
SHELL_CMD(service)  = { "service", "[list|start|stop <name>]", "supervised services",
                        SHELL_G_SYS, cmd_service };
SHELL_CMD(cron)     = { "cron",    "[list|run <name>]", "scheduled jobs",
                        SHELL_G_SYS, cmd_cron };
SHELL_CMD(abi)      = { "abi",     "",          "the guest-ABI number spaces, side by side",
                        SHELL_G_SYS, sy_abi };
SHELL_CMD(ktime)    = { "ktime",   "",          "the monotonic clock: source and resolution",
                        SHELL_G_SYS, sy_ktime };
SHELL_CMD(ktimer)   = { "ktimer",  "",          "deadline-timer accuracy over a spread of sleeps",
                        SHELL_G_SYS, sy_ktimer };
SHELL_CMD(shutdown) = { "shutdown","",          "power off",
                        SHELL_G_SYS, sy_shutdown };
SHELL_CMD(reboot)   = { "reboot",  "",          "restart the machine",
                        SHELL_G_SYS, sy_reboot };

SHELL_CMD(ringtest)    = { "ringtest",    "", "ring 3 / EL0 excursion",
                           SHELL_G_TEST, sy_ringtest };
SHELL_CMD(archtest)    = { "archtest",    "", "the ELF architecture gate",
                           SHELL_G_TEST, sy_archtest };
SHELL_CMD(fputest)     = { "fputest",     "", "per-task FP/SIMD register file",
                           SHELL_G_TEST, sy_fputest };
SHELL_CMD(faulttest)   = { "faulttest",   "", "bad ring-3 pointers, through all three layers",
                           SHELL_G_TEST, sy_faultt };
SHELL_CMD(timerfdtest) = { "timerfdtest", "[ms]", "timerfd drift against the original start",
                           SHELL_G_TEST, cmd_timerfdtest };
SHELL_CMD(alarmtest)   = { "alarmtest",   "[ms]", "setitimer + SIGALRM delivery",
                           SHELL_G_TEST, cmd_alarmtest };
SHELL_CMD(epolltest)   = { "epolltest",   "", "readiness sets, and the scan cost measured",
                           SHELL_G_TEST, sy_epolltest };

/* §M71 — `boundarytest on|off`: the falsifier for the ring-3 boundary audit's
 * leaked-flag violation.  Hidden from `help`, like `hardlock` and `leaktest`:
 * reachable for the harness, not advertised. */
void usyscall_boundary_test(int on);
static void sy_boundarytest(const char* a) {
    usyscall_boundary_test(!(a[0] == 'o' && a[1] == 'f'));
}
SHELL_CMD(boundarytest) = { "boundarytest", "on|off", 0, SHELL_G_TEST, sy_boundarytest };
