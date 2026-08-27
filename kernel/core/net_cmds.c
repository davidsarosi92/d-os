/* =============================================================================
 * net_cmds.c — the network subsystem's diagnostic + test commands (§M24).
 *
 * WHY THEY ARE NOT IN A SHELL.  This kernel has TWO shells: `shell.c`, the
 * full-featured one on the x86 virtual consoles, and `serial_shell.c`, the
 * AArch64 REPL on the PL011 (that port has no VC — its display arrives over
 * virtio-gpu, which is undrivable headless).  A network test that lives in one
 * of them can only ever be run on the arches that build it, which is exactly
 * how §M24's stack came to be shipped, and untested, on a third of its
 * targets for a year.
 *
 * So the commands live HERE, arch-independent, and both shells are three lines
 * of dispatch each.  §M52's lesson, applied before it bites: two copies of one
 * idea diverge, and the copy nobody runs diverges silently.
 * ========================================================================== */

#include "net.h"
#include "net_cmds.h"
#include "dhcp.h"
#include "printf.h"
#include "task.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>


/* Two string helpers, local because this file must not depend on either
 * shell's private ones. */
static int nc_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int nc_starts(const char* s, const char* pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

/* -------------------------------------------------------------------- */
/* §M24.9/.10 — `tcptest [clients]`: an echo SERVER and N clients, both  */
/* inside this kernel, talking over the loopback.                        */
/*                                                                       */
/* WHAT MAKES THIS A TEST AND NOT A DEMONSTRATION.  The stack this        */
/* replaces held ONE connection in one global, so the interesting         */
/* question is not "does a connection work" — that already worked — but   */
/* "do FOUR work AT THE SAME TIME".  Hence the barrier: every client      */
/* connects and then waits until all of them are connected before any     */
/* byte is exchanged.  On the old single-connection engine the second     */
/* connect would destroy the first and the barrier could never be         */
/* reached, so this reports FAIL rather than a slower PASS.  §M57's rule: */
/* run the new test against the old code first, and if it cannot fail     */
/* there, it is not evidence about the new code.                         */
/*                                                                       */
/* Loopback rather than SLIRP because a SERVER cannot be tested through   */
/* SLIRP at all without a hostfwd rule the automated runs do not have —   */
/* and because a test that needs the host's network to be up is a test    */
/* that reports the host's network.                                      */
/* -------------------------------------------------------------------- */

#define TCPTEST_PORT   9001

static volatile int g_tt_connected;    /* clients that reached ESTABLISHED   */
static volatile int g_tt_ok;           /* clients whose echo came back right */
static volatile int g_tt_done;
static volatile int g_tt_nclients;
static volatile int g_tt_srv_accepted;
static volatile int g_tt_srv_done;

/* The server: accept `n`, echo whatever each one says, close.  One task per
 * connection would be the natural shape and is deliberately NOT what this
 * does — a single task serving several live connections in turn is precisely
 * the thing a one-connection stack cannot do. */
static void tcptest_server(void) {
    struct tcp_conn* l = net_tcp_listen(IPV4(127,0,0,1), TCPTEST_PORT, 8);
    if (!l) { kprintf("tcptest: server FAIL (listen)\n"); g_tt_srv_done = 1; return; }

    int n = g_tt_nclients;
    struct tcp_conn* cs[8];
    int got = 0;
    while (got < n && got < 8) {
        struct tcp_conn* c = net_tcp_accept(l, 5000);
        if (!c) break;
        cs[got++] = c;
        __atomic_add_fetch(&g_tt_srv_accepted, 1, __ATOMIC_ACQ_REL);
    }

    /* Every connection is open at once here — that is the claim.  Serve them
     * round-robin so no client's echo depends on another's having finished. */
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < got; i++) {
            char buf[64];
            int r = net_tcp_recv(cs[i], buf, sizeof buf, 1, 0);   /* non-blocking */
            if (r > 0) net_tcp_send(cs[i], buf, (uint32_t)r, 0);
        }
        task_msleep(50);
    }
    for (int i = 0; i < got; i++) net_tcp_close(cs[i]);
    net_tcp_close(l);
    kprintf("tcptest: server accepted %d connection(s)\n", got);
    __atomic_store_n(&g_tt_srv_done, 1, __ATOMIC_RELEASE);
}

static void tcptest_client(void) {
    struct tcp_conn* c = net_tcp_connect(IPV4(127,0,0,1), TCPTEST_PORT, 4000);
    if (!c) { __atomic_add_fetch(&g_tt_done, 1, __ATOMIC_ACQ_REL); return; }

    int me = __atomic_add_fetch(&g_tt_connected, 1, __ATOMIC_ACQ_REL);

    /* THE BARRIER.  Nothing is sent until every client is connected, so a
     * stack that can only hold one connection at a time deadlocks here and
     * the test says so. */
    for (int ms = 0; ms < 4000; ms += 20) {
        if (__atomic_load_n(&g_tt_connected, __ATOMIC_ACQUIRE) >= g_tt_nclients) break;
        task_msleep(20);
    }

    char msg[16];
    msg[0] = 'p'; msg[1] = 'i'; msg[2] = 'n'; msg[3] = 'g';
    msg[4] = (char)('0' + (me % 10)); msg[5] = '\0';
    net_tcp_send(c, msg, 5, 0);

    char back[16];
    int n = net_tcp_recv(c, back, sizeof back, 0, 3000);
    if (n == 5 && back[0] == 'p' && back[4] == msg[4])
        __atomic_add_fetch(&g_tt_ok, 1, __ATOMIC_ACQ_REL);

    net_tcp_close(c);
    __atomic_add_fetch(&g_tt_done, 1, __ATOMIC_ACQ_REL);
}

void netcmd_tcptest(const char* args) {
    int n = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) n = n * 10 + (*args - '0');
    if (n <= 0) n = 4;
    if (n > 8)  n = 8;

    if (!net_route(IPV4(127,0,0,1))) {
        kprintf("tcptest: FAIL (no loopback device)\n"); return;
    }

    g_tt_connected = g_tt_ok = g_tt_done = 0;
    g_tt_srv_accepted = g_tt_srv_done = 0;
    g_tt_nclients = n;

    uint64_t t0 = timer_ticks_ms();
    if (!task_spawn_detached("tcp-srv", tcptest_server)) {
        kprintf("tcptest: FAIL (cannot spawn server)\n"); return;
    }
    task_msleep(100);                       /* let the listener bind first    */
    int spawned = 0;
    for (int i = 0; i < n; i++)
        if (task_spawn_detached("tcp-cli", tcptest_client)) spawned++;

    for (int ms = 0; ms < 20000; ms += 50) {
        if (__atomic_load_n(&g_tt_done, __ATOMIC_ACQUIRE) >= spawned &&
            __atomic_load_n(&g_tt_srv_done, __ATOMIC_ACQUIRE)) break;
        task_msleep(50);
    }
    uint32_t elapsed = (uint32_t)(timer_ticks_ms() - t0);

    kprintf("tcptest: %d/%d clients connected, %d accepted, %d echoed, %u ms\n",
            g_tt_connected, spawned, g_tt_srv_accepted, g_tt_ok, elapsed);
    if (g_tt_connected < spawned)
        kprintf("tcptest: FAIL (not every client connected — serialised?)\n");
    else if (g_tt_srv_accepted < spawned)
        kprintf("tcptest: FAIL (server did not accept them all)\n");
    else if (g_tt_ok < spawned)
        kprintf("tcptest: FAIL (an echo did not come back)\n");
    else
        kprintf("tcptest: PASS (concurrent connections, server + clients)\n");
}

/* -------------------------------------------------------------------- */
/* §M24.11 — `tcploss [permille] [kbytes]`: make the link LOSE frames and  */
/* show that the stream survives it byte for byte.                        */
/*                                                                        */
/* This test exists because of what the old stack could not do: it put    */
/* the caller's bytes on the wire and forgot them, so a dropped segment   */
/* was a hole in the stream and nothing anywhere could notice.  A         */
/* retransmit timer written without a way to lose a packet is a feature   */
/* nothing in the build can falsify, which is why the loopback learned to */
/* drop before the timer learned to resend.                               */
/*                                                                        */
/* The assertion is TWO-SIDED on purpose: every byte must arrive in order */
/* AND the retransmit counter must be non-zero.  Checking only the first  */
/* would pass on a link that never lost anything, i.e. it would prove the */
/* test harness rather than the stack.                                    */
/* -------------------------------------------------------------------- */

#define TCPLOSS_PORT 9002
static void netstat_row(const struct net_tcp_info* i, void* ctx);

static volatile int      g_tl_bytes, g_tl_bad, g_tl_srv_done, g_tl_kbytes;
static volatile int      g_tl_readdelay = 5;   /* ms the reader dawdles       */
/* Split the reader's wall clock into its parts.  "The transfer is slow" is not
 * a finding; "the reader spent 8.4 s inside recv and 0.2 s asleep" is. */
static volatile uint32_t g_tl_reads, g_tl_ms_recv, g_tl_ms_sleep;
static volatile uint32_t g_tl_retrans;

static uint8_t tcploss_byte(uint32_t i) { return (uint8_t)((i * 31u + 7u) & 0xFF); }

static void tcploss_server(void) {
    struct tcp_conn* l = net_tcp_listen(IPV4(127,0,0,1), TCPLOSS_PORT, 4);
    if (!l) { kprintf("tcploss: FAIL (listen)\n"); g_tl_srv_done = 1; return; }
    struct tcp_conn* c = net_tcp_accept(l, 8000);
    if (!c) { kprintf("tcploss: FAIL (no connection)\n"); net_tcp_close(l);
              g_tl_srv_done = 1; return; }

    uint32_t got = 0, bad = 0;
    for (;;) {
        uint8_t buf[1024];
        uint64_t ta = timer_ticks_ms();
        int r = net_tcp_recv(c, buf, sizeof buf, 0, 4000);
        g_tl_ms_recv += (uint32_t)(timer_ticks_ms() - ta);
        g_tl_reads++;
        if (r == NET_EAGAIN) {
            /* A read that timed out is the moment to look at the table — what
             * is stuck is visible NOW and gone a second later.  This exact
             * print is what turned "the transfer is slow" into "the sender is
             * in FIN_WAIT_2 while the receiver is still ESTABLISHED", which
             * named the bug in one line after two wrong theories. */
            kprintf("tcploss: stalled after %u bytes — table:\n", got);
            net_tcp_foreach(netstat_row, NULL);
        }
        if (r == 0) break;                       /* the client closed          */
        if (r < 0) break;                        /* reset, or nothing in 8 s   */
        for (int i = 0; i < r; i++)
            if (buf[i] != tcploss_byte(got + (uint32_t)i)) bad++;
        got += (uint32_t)r;
        /* Read in small bites so the receive window really does close and the
         * sender really does have to wait for it to re-open.  A test that
         * drains as fast as the peer can send never exercises a window. */
        if (g_tl_readdelay) {
            uint64_t tb = timer_ticks_ms();
            task_msleep(g_tl_readdelay);
            g_tl_ms_sleep += (uint32_t)(timer_ticks_ms() - tb);
        }
    }
    g_tl_bytes = (int)got;
    g_tl_bad   = (int)bad;
    net_tcp_close(c);
    net_tcp_close(l);
    __atomic_store_n(&g_tl_srv_done, 1, __ATOMIC_RELEASE);
}

/* §M67 — the loopback driver is a module; its test hooks may not be there.
 * A weak symbol that is absent is a call through zero, so every use is gated
 * and the refusal names the reason rather than doing nothing. */
static int lo_hooks_ready(const char* what) {
    if (loopback_set_drop && loopback_stats) return 1;
    kprintf("%s: the loopback module is not loaded"
            " (insmod /modules/loopback.ko)\n", what);
    return 0;
}

static void tcploss_retrans_row(const struct net_tcp_info* i, void* ctx) {
    (void)ctx;
    g_tl_retrans += i->retrans;
}

void netcmd_tcploss(const char* args) {
    if (!lo_hooks_ready("tcploss")) return;
    int permille = 0, kb = 0, have_pm = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) { permille = permille * 10 + (*args - '0'); have_pm = 1; }
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) kb = kb * 10 + (*args - '0');
    /* An explicit 0 means a PERFECT link — the baseline every "how slow is
     * loss" question needs.  Folding it into "unset" would make the one
     * measurement that isolates the loss impossible to ask for. */
    if (!have_pm) permille = 50;                 /* 5% of frames               */
    if (permille > 300) permille = 300;          /* past this the RTOs dominate*/
    if (kb <= 0) kb = 32;
    if (kb > 256) kb = 256;
    while (*args == ' ') args++;
    { int d = -1; if (*args >= '0' && *args <= '9') { d = 0;
        for (; *args >= '0' && *args <= '9'; args++) d = d * 10 + (*args - '0'); }
      if (d >= 0) g_tl_readdelay = d; }

    if (!net_route(IPV4(127,0,0,1))) {
        kprintf("tcploss: FAIL (no loopback device)\n"); return;
    }

    g_tl_bytes = g_tl_bad = g_tl_srv_done = 0;
    g_tl_retrans = 0;
    g_tl_reads = g_tl_ms_recv = g_tl_ms_sleep = 0;
    g_tl_kbytes = kb;

    loopback_set_drop((uint32_t)permille);
    struct net_poller_stats ps0; net_poller_stats(&ps0);
    uint32_t rtos0 = 0; net_tcp_timing_counters(NULL, &rtos0, NULL);
    uint64_t t0 = timer_ticks_ms();
    if (!task_spawn_detached("tcp-loss-srv", tcploss_server)) {
        loopback_set_drop(0);
        kprintf("tcploss: FAIL (cannot spawn server)\n"); return;
    }
    task_msleep(100);

    struct tcp_conn* c = net_tcp_connect(IPV4(127,0,0,1), TCPLOSS_PORT, 8000);
    if (!c) {
        loopback_set_drop(0);
        kprintf("tcploss: FAIL (connect)\n"); return;
    }

    uint32_t total = (uint32_t)kb * 1024u, sent = 0;
    uint32_t sends = 0, ms_send = 0, eagains = 0;
    while (sent < total) {
        uint8_t chunk[512];
        uint32_t n = total - sent; if (n > sizeof chunk) n = sizeof chunk;
        for (uint32_t i = 0; i < n; i++) chunk[i] = tcploss_byte(sent + i);
        uint64_t ts = timer_ticks_ms();
        int w = net_tcp_send(c, chunk, n, 0);
        ms_send += (uint32_t)(timer_ticks_ms() - ts);
        sends++;
        if (w > 0) { sent += (uint32_t)w; continue; }
        if (w == NET_EAGAIN) { eagains++; task_msleep(20); continue; } /* shut  */
        break;                                               /* broken          */
    }
    /* Per-connection retransmit counts, read BEFORE the close — but they are
     * only ever a HINT here, and the first ARM run said so: seven timeouts had
     * fired and this sum reported zero, because the connections they belonged
     * to had already gone.  A counter that lives on an object cannot measure a
     * period longer than the object.  The ASSERTION below therefore uses the
     * global timeout counter, which is cumulative and cannot vanish. */
    net_tcp_foreach(tcploss_retrans_row, NULL);
    net_tcp_close(c);

    for (int ms = 0; ms < 30000; ms += 50) {
        if (__atomic_load_n(&g_tl_srv_done, __ATOMIC_ACQUIRE)) break;
        task_msleep(50);
    }
    uint32_t elapsed = (uint32_t)(timer_ticks_ms() - t0);
    loopback_set_drop(0);

    uint32_t dropped = 0, qfull = 0, pm = 0;
    loopback_stats(&pm, &dropped, &qfull);
    struct net_poller_stats ps1; net_poller_stats(&ps1);
    uint32_t pers = 0, rtos = 0, zwin = 0;
    net_tcp_timing_counters(&pers, &rtos, &zwin);
    uint32_t rto_delta = rtos - rtos0;

    kprintf("tcploss: %u‰ loss — sent %u, received %u, corrupt %d, "
            "%u frames dropped (%u queue-full), %u timeouts (%u still-live "
            "retransmits), %u ms\n",
            (uint32_t)permille, sent, (uint32_t)g_tl_bytes, g_tl_bad,
            dropped, qfull, rto_delta, g_tl_retrans, elapsed);
    /* The poller's own figures alongside the transfer's.  Without them a slow
     * run is a mystery: "the link lost frames" and "the poller waited for an
     * interrupt that a loopback can never send" look identical from here, and
     * they need opposite fixes. */
    kprintf("tcploss: poller — %u pumps, %u backstops, %u frames; "
            "%u persists, %u rtos, %u zero-window ads\n",
            ps1.pumps - ps0.pumps, ps1.backstops - ps0.backstops,
            ps1.frames - ps0.frames, pers, rtos, zwin);
    kprintf("tcploss: reader — %u reads, %u ms in recv, %u ms asleep\n",
            g_tl_reads, g_tl_ms_recv, g_tl_ms_sleep);
    uint32_t dsegs = 0, wblk = 0;
    net_tcp_output_counters(&dsegs, &wblk);
    kprintf("tcploss: writer — %u sends, %u ms in send, %u eagain; "
            "%u data segments, %u window-blocked\n",
            sends, ms_send, eagains, dsegs, wblk);

    if ((uint32_t)g_tl_bytes != total || g_tl_bad)
        kprintf("tcploss: FAIL (the stream did not survive the loss)\n");
    else if (!dropped)
        kprintf("tcploss: FAIL (nothing was ever dropped — nothing was tested)\n");
    else if (!rto_delta)
        kprintf("tcploss: FAIL (frames were lost but no timeout ever fired)\n");
    else
        kprintf("tcploss: PASS (every byte arrived, in order, after loss)\n");
}

/* `lo drop <permille>` — the instrument itself, exposed so a person can leave
 * it on and watch another command survive (or not). */
void netcmd_lo(const char* args) {
    if (!lo_hooks_ready("lo")) return;
    while (*args == ' ') args++;
    if (nc_starts(args, "drop")) {
        args += 4;
        while (*args == ' ') args++;
        int pm = 0;
        for (; *args >= '0' && *args <= '9'; args++) pm = pm * 10 + (*args - '0');
        loopback_set_drop((uint32_t)pm);
    }
    uint32_t pm = 0, inj = 0, qfull = 0;
    loopback_stats(&pm, &inj, &qfull);
    kprintf("lo: drop %u‰   injected %u   queue-full %u\n", pm, inj, qfull);
}

/* `netstat` — the connection table, which until §M24.9 had exactly one row and
 * therefore nothing to print.  Ships WITH the table for the §M49 reason: a
 * subsystem that cannot be inspected is one whose bugs are argued about
 * instead of looked at. */
static void netstat_row(const struct net_tcp_info* i, void* ctx) {
    (void)ctx;
    char l[16], p[16];
    net_fmt_ip(i->local_ip, l); net_fmt_ip(i->peer_ip, p);
    if (i->listener)
        kprintf("  LISTEN  %s:%u  (backlog)\n", l, i->local_port);
    else
        kprintf("  %s  %s:%u -> %s:%u  rxq %u txq %u  seg %u/%u  retx %u  [%s]\n",
                i->state, l, i->local_port, p, i->peer_port,
                i->rx_queued, i->tx_queued, i->rx_segs, i->tx_segs,
                i->retrans, i->dev);
}

static void netstat_count(const struct net_tcp_info* i, void* ctx) {
    (void)i; (*(int*)ctx)++;
}

void netcmd_netstat(void) {
    int inuse = 0;
    net_tcp_foreach(netstat_count, &inuse);
    /* Print the CAPACITY next to the usage.  A table that is full refuses
     * connections with an RST, which looks from the outside exactly like a
     * peer that is not listening — so the one number that distinguishes them
     * belongs on the screen, not in a header file. */
    kprintf("tcp connections: %d of %d table entries in use\n",
            inuse, net_tcp_capacity());
    net_tcp_foreach(netstat_row, NULL);
    uint32_t rs = 0, rr = 0, nc = 0;
    net_tcp_counters(&rs, &rr, &nc);
    kprintf("  resets sent %u   received %u   segments to no connection %u\n",
            rs, rr, nc);
}

/* `dhcp [dev]` — ask the network for an address (§M24 stage 7). */
void netcmd_dhcp(const char* args) {
    while (*args == ' ') args++;
    if (nc_eq(args, "status")) { dhcp_status(); return; }
    struct net_device* dev = args[0] ? net_find(args) : net_primary();
    if (!dev) { kprintf("dhcp: no such device\n"); return; }
    if (dhcp_configure(dev) == 0) net_list();
}

