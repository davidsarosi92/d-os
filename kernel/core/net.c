/* =============================================================================
 * net.c — the portable network stack (§M24.1): device registry + Ethernet
 * demux + ARP + IPv4 + ICMP echo (ping).
 *
 * Layering (all arch-independent — no port I/O, no asm; the NIC driver is the
 * only arch/bus-specific piece and lives under kernel/drivers/net/):
 *
 *     net_ping / cmd_ping ─┐
 *                          ▼
 *     ICMP  ── net_ipv4_send ──► ARP resolve ──► eth_send ──► dev->transmit
 *       ▲                                                          (driver)
 *       │
 *     net_rx (driver → stack) ─► Ethernet demux ─► ARP in / IPv4 in ─► ICMP in
 *
 * ---------------------------------------------------------------------------
 * CONCURRENCY (§M55 — this replaces the original "one task context" model)
 * ---------------------------------------------------------------------------
 *
 * The first slice drove RX by *polling from the calling task*: whoever wanted a
 * reply called dev->poll() in a bounded spin loop until it arrived.  That model
 * has three defects, and they compound:
 *
 *   1. THE WAITER BURNS A CPU.  A task waiting for a DNS answer spun with
 *      hal_cpu_pause() and never left the runqueue, so waiting for the network
 *      cost exactly as much CPU as computing flat out.  §M49 removed the last
 *      such polls from the console and the reaper for this reason; the network
 *      stack was the one that got away.
 *
 *   2. N WAITERS MEANT N POLLERS.  dev->poll() is not reentrant — it advances
 *      the driver's last_used_idx and recycles RX buffers — so two tasks each
 *      "waiting for their own packet" were two tasks mutating one ring.  It
 *      survived only because nothing ever waited on two sockets at once.
 *
 *   3. A SPIN COUNT IS NOT A TIMEOUT.  `20000000u` iterations means a different
 *      amount of time on every machine, every arch and every host CPU under
 *      emulation.  That is not a theoretical complaint: it is exactly how musl's
 *      resolver came to hang "for minutes" on emulated i386 (§M39) — the bound
 *      was doing its job, it just could not say how long its job took.
 *
 * The model now is: ONE poller task (`netd`) is the only caller of dev->poll,
 * and everyone else BLOCKS on a wait queue until the state they care about
 * changes or a REAL deadline (§M53's nanosecond clock) passes.
 *
 * netd runs exactly while somebody is waiting for a packet, and is fully
 * blocked otherwise — a poller with nobody waiting for it is pure waste, and an
 * idle box must not pay for a network stack it is not using.
 *
 * `g_netwq`'s lock is THE stack lock: by waitq's own contract (waitq.h) the
 * queue's lock also serialises the condition, so the ARP cache, the ping/DNS
 * reply flags, the TCP connection state and the per-socket RX rings are all
 * mutated and checked under it.  net_rx() and everything below it therefore run
 * with the lock HELD.
 *
 * THE CONSEQUENCE THAT SHAPES THE REST OF THE FILE: the RX path may never
 * block, so it may never resolve an ARP entry.  Instead every reply generated
 * while handling a frame is sent back to the MAC that frame CAME FROM — which
 * is the correct next hop by construction, on-link or via a router, and is
 * cheaper besides (a TCP ACK has no business doing an address lookup).  That is
 * what the `via_mac` argument on the emit path is for, and why the send helpers
 * come in `_locked` (assembly + transmit only) and unlocked (resolve, then
 * emit) flavours.  A `_locked` function called from task context must be
 * wrapped in net_lock/net_unlock; an unlocked one must NOT be called with the
 * lock held.
 *
 * Interrupts are off while the stack lock is held, so dev->transmit's wait for
 * the virtqueue completion now runs with them masked.  Under QEMU that
 * completes in microseconds; the driver's own bounded spin is what keeps a
 * wedged device from turning into a hard lockup.
 *
 * Still a follow-up: the NIC interrupt, which turns netd's yield loop into a
 * block.  Doing it BEFORE this change would have put two pollers on one
 * virtqueue — the ISR and every spinning waiter — which is why it comes second.
 *
 * All multi-byte on-wire fields are big-endian; we convert at the boundary
 * with htons/htonl (net.h).  The stack's own state is host byte order.
 * ============================================================================= */

#include "net.h"
#include "hal.h"
#include "hal_api.h"
#include "printf.h"
#include "waitq.h"
#include "ktimer.h"
#include "timer.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

/* ----------------------- On-the-wire structs ------------------------------ */

struct eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;                        /* network order              */
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype;                            /* 1 = Ethernet               */
    uint16_t ptype;                            /* 0x0800 = IPv4              */
    uint8_t  hlen;                             /* 6                          */
    uint8_t  plen;                             /* 4                          */
    uint16_t oper;                             /* 1 request, 2 reply         */
    uint8_t  sha[ETH_ALEN];                    /* sender hardware addr       */
    uint8_t  spa[4];                           /* sender protocol addr       */
    uint8_t  tha[ETH_ALEN];                    /* target hardware addr       */
    uint8_t  tpa[4];                           /* target protocol addr       */
} __attribute__((packed));

struct ipv4_hdr {
    uint8_t  ver_ihl;                          /* 0x45 (v4, 5 dwords = 20 B) */
    uint8_t  tos;
    uint16_t total_len;                        /* header + payload, net order*/
    uint16_t id;
    uint16_t frag;                             /* flags + fragment offset    */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;                              /* network order              */
    uint32_t dst;                              /* network order              */
} __attribute__((packed));

struct icmp_hdr {
    uint8_t  type;                             /* 8 = echo request, 0 = reply*/
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct udp_hdr {
    uint16_t src_port;                         /* network order              */
    uint16_t dst_port;
    uint16_t len;                              /* header + payload           */
    uint16_t checksum;                         /* 0 = omitted (RFC 768)      */
} __attribute__((packed));

struct tcp_hdr {
    uint16_t src_port;                         /* network order              */
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;                         /* high nibble = hdr len/4    */
    uint8_t  flags;                            /* FIN/SYN/RST/PSH/ACK/URG    */
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2
#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* ----------------------- Small utilities ---------------------------------- */

static void mac_copy(uint8_t* d, const uint8_t* s) {
    for (int i = 0; i < ETH_ALEN; i++) d[i] = s[i];
}
static int mac_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < ETH_ALEN; i++) if (a[i] != b[i]) return 0;
    return 1;
}
/* host-order u32 → 4 network-order octets and back. */
static void ip_to_octets(uint32_t ip, uint8_t o[4]) {
    o[0] = (ip >> 24) & 0xFF; o[1] = (ip >> 16) & 0xFF;
    o[2] = (ip >>  8) & 0xFF; o[3] =  ip        & 0xFF;
}
static uint32_t octets_to_ip(const uint8_t o[4]) {
    return ((uint32_t)o[0] << 24) | ((uint32_t)o[1] << 16) |
           ((uint32_t)o[2] << 8)  |  (uint32_t)o[3];
}

static const uint8_t BCAST_MAC[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

uint16_t net_checksum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += ((uint16_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (uint16_t)p[0] << 8;       /* odd trailing byte, hi half */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    /* We accumulated in host order (hi<<8|lo); the fold is order-agnostic,
     * but to store the result on the wire we must byte-swap back. */
    return htons((uint16_t)~sum);
}

/* ----------------------- Device registry ---------------------------------- */

static struct net_device* g_head = NULL;

int net_register(struct net_device* dev) {
    dev->next = NULL;
    if (!g_head) { g_head = dev; }
    else {
        struct net_device* n = g_head;
        while (n->next) n = n->next;
        n->next = dev;
    }
    char ipb[16], macb[18];
    net_fmt_ip(dev->ip, ipb); net_fmt_mac(dev->mac, macb);
    kprintf("net: registered %s mac=%s ip=%s\n", dev->name, macb, ipb);
    return 0;
}

struct net_device* net_find(const char* name) {
    for (struct net_device* n = g_head; n; n = n->next) {
        const char* a = n->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return n;
    }
    return NULL;
}

struct net_device* net_primary(void) { return g_head; }

void net_for_each(net_iter_fn fn, void* ctx) {
    for (struct net_device* n = g_head; n; n = n->next) fn(n, ctx);
}

void net_list(void) {
    if (!g_head) { kprintf("no network devices\n"); return; }
    for (struct net_device* n = g_head; n; n = n->next) {
        char ip[16], mask[16], gw[16], mac[18];
        net_fmt_ip(n->ip, ip); net_fmt_ip(n->netmask, mask); net_fmt_ip(n->gateway, gw);
        net_fmt_mac(n->mac, mac);
        kprintf("%s  mac %s  ip %s/%s  gw %s\n", n->name, mac, ip, mask, gw);
        kprintf("     RX %u pkts / %u bytes   TX %u pkts / %u bytes   drop %u\n",
                n->rx_packets, n->rx_bytes, n->tx_packets, n->tx_bytes, n->rx_dropped);
    }
    /* The poller's own state.  `waiters 0` + `netd idle` on a quiet box is the
     * whole point of §M55: nothing is polling anything. */
    struct net_poller_stats st;
    net_poller_stats(&st);
    kprintf("netd: %s   waiters %d   pumps %u (%u inline)   frames %u\n",
            st.running ? (st.waiters ? "polling" : "idle") : "not started",
            st.waiters, st.pumps, st.inline_pumps, st.frames);
    /* backstops on a quiet wire are the design, not a defect — only
     * `missed` is a fault (see g_missed_irqs). */
    kprintf("      rx-irq: %s   interrupts %u   backstops %u (idle)   missed %u\n",
            st.irq_live ? "live" : "none seen (polled)",
            st.irqs, st.backstops, st.missed_irqs);
}

/* =============================================================================
 * The stack lock, the poller task, and waiting for a packet (§M55)
 *
 * See the file header for why this exists.  Three objects:
 *
 *   g_netwq   the stack lock AND the queue every waiter parks on.  Held across
 *             net_rx and everything it calls, and across every frame assembly.
 *   g_netdwq  where netd itself parks when nobody is waiting.  Its lock guards
 *             g_waiters, which is the whole condition netd blocks on.
 *   g_waiters how many tasks are currently waiting for the network.  0 means
 *             netd sleeps; the transition 0→1 is what wakes it.
 *
 * The two queues are never held nested, in either order.
 * ============================================================================= */

static struct waitq g_netwq  = WAITQ_INIT;   /* stack lock + waiter parking   */
static struct waitq g_netdwq = WAITQ_INIT;   /* netd's own idle parking spot  */
static struct waitq g_nicwq  = WAITQ_INIT;   /* where netd waits for the NIC  */
static volatile int g_waiters      = 0;      /* guarded by g_netdwq's lock    */
static volatile int g_netd_running = 0;
static volatile int g_netd_spawned = 0;

/* Observability — a change like this is only worth making if it can be
 * measured afterwards (`lsnic` prints these). */
static volatile uint32_t g_pump_count;       /* dev->poll rounds             */
static volatile uint32_t g_rx_frames;        /* frames handed to net_rx      */
/* Pumps NOT driven by netd — the fallback in net_wait_cond and the
 * non-blocking single pump.  Kept apart on purpose: a rising count here means
 * the poller is not doing its job, and that is not something to discover by
 * inferring it from the total. */
static volatile uint32_t g_inline_pumps;

/* The POLLED fallback's pacing, used only while no NIC interrupt has ever
 * arrived (see net_rx_irq).  Yielding keeps latency at a scheduler slice while
 * a conversation is in flight; the sleep is what stops a waiter with a multi-
 * second deadline from pinning a core when nothing is coming. */
#define NETD_SPIN_ROUNDS   2048
#define NETD_IDLE_SLEEP_NS 1000000ull        /* 1 ms                          */

/* The interrupt path's backstop.  netd blocks until the NIC says a frame
 * arrived — but never INDEFINITELY: a missed or misrouted interrupt must
 * degrade this stack to polling, not to a hang.  Ten milliseconds is slow
 * enough to cost nothing when interrupts work and fast enough that a box whose
 * interrupt is broken still passes its own tests, loudly (`lsnic` reports how
 * many times the backstop had to fire, which is how you tell "the interrupt is
 * wired" from "the interrupt is delivering"). */
#define NETD_IRQ_BACKSTOP_NS 10000000ull     /* 10 ms                         */

/* Interrupt bookkeeping.  g_irq_seq is the thing netd compares against: a
 * COUNTER rather than a flag, because a flag cannot distinguish "no interrupt
 * since I looked" from "one arrived and was already consumed". */
static volatile uint32_t g_irq_seq;
static volatile uint32_t g_irq_count;        /* NIC interrupts taken          */
static volatile uint32_t g_backstop_count;   /* times we timed out instead    */
static volatile uint32_t g_missed_irqs;      /* backstops that FOUND frames   */
static volatile int      g_nic_irq_seen;     /* set by the FIRST real one     */

/* Why g_missed_irqs exists, and why the raw backstop count is not enough.
 *
 * Most backstops are not defects: when nothing is coming, the timer is the
 * ONLY thing that can wake the poller, so a wait on a quiet wire back-stops
 * once every NETD_IRQ_BACKSTOP_NS by design.  `netstorm`, which waits three
 * seconds for replies that never arrive, therefore reports ~300 of them and is
 * working perfectly.
 *
 * The one that matters is a backstop whose very next pump FINDS FRAMES: the
 * data was already there and nothing told us.  That is a missed interrupt, and
 * it is the only figure here that should ever be read as a fault. */

/* Called by a NIC driver's ISR.  Interrupt context: this may take a lock (all
 * holders of it mask interrupts) and wake tasks, and it may do NOTHING else.
 * In particular it must not drain the RX ring — draining calls net_rx, which
 * can generate a TCP ACK, which spins waiting on the TX virtqueue.  That is
 * §M49's xHCI lesson, and it applies here word for word.
 *
 * The stack learns that interrupts WORK by receiving one, rather than by being
 * told: a driver that wires an interrupt which never fires leaves netd in the
 * polled mode instead of blocking forever on a promise. */
void net_rx_irq(struct net_device* dev) {
    (void)dev;
    uint32_t f = waitq_lock(&g_nicwq);
    g_irq_seq++;
    g_irq_count++;
    g_nic_irq_seen = 1;
    waitq_wake_all(&g_nicwq);
    waitq_unlock(&g_nicwq, f);
}

uint32_t net_lock(void)              { return waitq_lock(&g_netwq); }
void     net_unlock(uint32_t flags)  { waitq_unlock(&g_netwq, flags); }

/* Pump every device once.  Caller holds the stack lock.  Returns the number of
 * frames that reached net_rx during this round. */
static uint32_t net_pump_locked(void) {
    uint32_t before = g_rx_frames;
    g_pump_count++;
    for (struct net_device* n = g_head; n; n = n->next)
        if (n->poll) n->poll(n);
    return g_rx_frames - before;
}

/* One pump for a caller that will NOT wait (a non-blocking recv).  This exists
 * so that "give the device one chance" stays a stack operation instead of a
 * bare dev->poll() in another file — that bare call was the second poller. */
void net_pump_once(void) {
    uint32_t f = net_lock();
    g_inline_pumps++;
    uint32_t got = net_pump_locked();
    if (got) waitq_wake_all(&g_netwq);
    net_unlock(f);
}

/* The deadline half of a wait.  Runs in interrupt context (ktimer's contract),
 * so it does the one safe thing: take the queue lock and wake.  Taking the lock
 * is not overhead here — it is what closes the window against a task that has
 * checked its deadline but not yet parked. */
static void net_deadline_fired(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t f = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, f);
}

static void netd_main(void);

/* Start the poller on first use.  A box with no NIC, or one whose network is
 * never touched, never pays for the task at all. */
static void netd_ensure(void) {
    if (__atomic_load_n(&g_netd_spawned, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&g_netd_spawned, 1, __ATOMIC_ACQ_REL)) return;
    struct task* t = task_spawn_detached("netd", netd_main);
    if (!t) {
        /* Spawning failed — fall back to pumping from the waiter, which is the
         * old model.  Slower and CPU-hungry, but the network keeps working;
         * silently having no poller at all would hang every wait instead. */
        __atomic_store_n(&g_netd_spawned, 0, __ATOMIC_RELEASE);
        kprintf("netd: could not start poller, falling back to in-line polling\n");
    }
}

static void net_wait_begin(void) {
    netd_ensure();
    uint32_t f = waitq_lock(&g_netdwq);
    g_waiters++;
    waitq_wake_all(&g_netdwq);
    waitq_unlock(&g_netdwq, f);
}

static void net_wait_end(void) {
    uint32_t f = waitq_lock(&g_netdwq);
    if (g_waiters > 0) g_waiters--;
    waitq_unlock(&g_netdwq, f);
}

/* Wait until `cond` reports true or `timeout_ms` elapses.  `cond` is evaluated
 * WITH THE STACK LOCK HELD — it may read stack state freely and must not take
 * the lock itself or block.  Returns 1 if the condition became true. */
int net_wait_cond(int (*cond)(void*), void* arg, uint32_t timeout_ms) {
    if (!cond) return 0;

    /* Fast path: already true.  Worth having because most sends find the ARP
     * entry cached, and starting a timer + waking netd for that would cost more
     * than the lookup it is protecting. */
    uint32_t f0 = net_lock();
    int done = cond(arg);
    net_unlock(f0);
    if (done) return 1;

    uint64_t deadline = timer_now_ns() + (uint64_t)timeout_ms * 1000000ull;
    struct ktimer t = { 0, 0, 0, 0, 0 };
    ktimer_arm(&t, deadline, net_deadline_fired, &g_netwq);

    net_wait_begin();

    int ok = 0;
    uint32_t f = net_lock();
    for (;;) {
        if (cond(arg))                  { ok = 1; break; }
        if (timer_now_ns() >= deadline) break;
        if (task_should_stop())         break;   /* a kill must not wait out a
                                                  * multi-second timeout       */
        if (__atomic_load_n(&g_netd_running, __ATOMIC_ACQUIRE)) {
            waitq_block(&g_netwq);
        } else {
            /* No poller yet (see netd_ensure) — pump here so the wait can still
             * make progress.  DROP THE LOCK AND YIELD each round: staying in
             * this loop with the lock held and interrupts off would starve the
             * very task we are waiting for the scheduler to start, which turns
             * a brief fallback into a permanent one. */
            g_inline_pumps++;
            if (net_pump_locked()) waitq_wake_all(&g_netwq);
            net_unlock(f);
            task_yield();
            f = net_lock();
        }
    }
    net_unlock(f);

    net_wait_end();
    /* Cancel unconditionally: on the normal path it has already fired
     * (harmless), on the timeout path it has not — and its callback would
     * otherwise reference a queue this frame is about to leave. */
    ktimer_cancel(&t);
    return ok;
}

/* The backstop half of netd's interrupt wait.  Same shape as the deadline
 * timer above, and for the same reason: the wake must take the queue lock, or
 * it can slip past a task that has decided to park but has not yet done so. */
static void nic_backstop_fired(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t f = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, f);
}

/* Block until the NIC reports a frame, or the backstop fires.
 *
 * `seq_before` is the interrupt count sampled BEFORE the pump that came back
 * empty.  Re-comparing it here under the queue lock is what makes an interrupt
 * that landed DURING that pump impossible to miss: the ISR increments the
 * counter under this same lock, so either it got there first (and the counter
 * differs, so we do not park) or it arrives after we are parked (and its wake
 * finds us).  Testing a bare "is there work" flag instead would lose exactly
 * the interrupts that arrive in that window. */
static int netd_wait_for_irq(uint32_t seq_before) {
    struct ktimer t = { 0, 0, 0, 0, 0 };
    ktimer_arm(&t, timer_now_ns() + NETD_IRQ_BACKSTOP_NS, nic_backstop_fired, &g_nicwq);

    uint32_t f = waitq_lock(&g_nicwq);
    if (g_irq_seq == seq_before && !task_should_stop()) waitq_block(&g_nicwq);
    int woken_by_irq = (g_irq_seq != seq_before);
    waitq_unlock(&g_nicwq, f);

    if (!woken_by_irq) g_backstop_count++;
    ktimer_cancel(&t);
    return !woken_by_irq;                        /* "this was a backstop" */
}

static void netd_main(void) {
    __atomic_store_n(&g_netd_running, 1, __ATOMIC_RELEASE);
    uint32_t quiet = 0;
    int after_backstop = 0;                      /* see g_missed_irqs */

    for (;;) {
        /* Park until somebody wants packets.  This is the whole reason an idle
         * box costs nothing: with no waiters netd is off every runqueue. */
        uint32_t f = waitq_lock(&g_netdwq);
        int parked = 0;
        while (!g_waiters && !task_should_stop()) { parked = 1; waitq_block(&g_netdwq); }
        waitq_unlock(&g_netdwq, f);
        if (task_should_stop()) break;
        /* A fresh waiter deserves the fast path.  Without this reset the
         * counter left over from the last conversation's tail would put the
         * very first pump of the next one straight into the sleep. */
        if (parked) quiet = 0;

        /* Sample the interrupt counter BEFORE pumping — see netd_wait_for_irq
         * for why the order matters. */
        uint32_t seq = __atomic_load_n(&g_irq_seq, __ATOMIC_ACQUIRE);

        uint32_t lf  = net_lock();
        uint32_t got = net_pump_locked();
        if (got) waitq_wake_all(&g_netwq);
        net_unlock(lf);

        /* Frames found by a pump that ran because the TIMER fired mean the
         * interrupt for them never arrived.  This is the only reading here
         * that is a fault. */
        if (got && after_backstop) g_missed_irqs++;
        after_backstop = 0;

        if (got) {
            /* Frames are flowing: come straight back for the next batch rather
             * than waiting to be told about work that is already here. */
            quiet = 0;
            task_yield();
        } else if (__atomic_load_n(&g_nic_irq_seen, __ATOMIC_ACQUIRE)) {
            after_backstop = netd_wait_for_irq(seq);   /* the real thing      */
        } else if (++quiet < NETD_SPIN_ROUNDS) {
            task_yield();                        /* polled fallback           */
        } else {
            quiet = NETD_SPIN_ROUNDS;            /* don't wrap the counter    */
            task_sleep_until_ns(timer_now_ns() + NETD_IDLE_SLEEP_NS);
        }
    }
    __atomic_store_n(&g_netd_running, 0, __ATOMIC_RELEASE);
}

void net_poller_stats(struct net_poller_stats* out) {
    if (!out) return;
    out->pumps        = g_pump_count;
    out->inline_pumps = g_inline_pumps;
    out->frames       = g_rx_frames;
    out->irqs         = g_irq_count;
    out->backstops    = g_backstop_count;
    out->missed_irqs  = g_missed_irqs;
    out->waiters      = g_waiters;
    out->running      = g_netd_running;
    out->irq_live     = g_nic_irq_seen;
}

/* ----------------------- L2 transmit -------------------------------------- */

/* Assembly buffer for outgoing frames.  Safe as a static because every emit
 * path holds the stack lock — that is what replaced the old "single task, so no
 * reentrancy" argument, which stopped being true the moment netd could send an
 * ACK while a task sent a request. */
static uint8_t g_txframe[ETH_FRAME_MAX];

static int eth_send_locked(struct net_device* dev, const uint8_t* dst_mac,
                           uint16_t ethertype, const void* payload, uint32_t len) {
    if (len > ETH_MTU) return -1;
    struct eth_hdr* eh = (struct eth_hdr*)g_txframe;
    mac_copy(eh->dst, dst_mac);
    mac_copy(eh->src, dev->mac);
    eh->ethertype = htons(ethertype);
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) g_txframe[ETH_HLEN + i] = p[i];
    return dev->transmit(dev, g_txframe, ETH_HLEN + len);
}

/* Assemble an IPv4 packet and hand it to `via_mac`.  Caller holds the stack
 * lock and has ALREADY decided the next hop — which is what lets the RX path
 * generate replies without ever performing a lookup.  Defined with the rest of
 * IPv4 at the bottom of the file. */
static int ipv4_emit_locked(struct net_device* dev, const uint8_t* via_mac,
                            uint32_t dst_ip, uint8_t proto,
                            const void* payload, uint32_t len);

/* ----------------------- ARP ---------------------------------------------- */

/* A tiny fixed-size cache — plenty for a host + a gateway.  LRU-free: on a
 * full cache we overwrite slot 0 (good enough for the slice). */
#define ARP_CACHE_SIZE 8
struct arp_entry { uint32_t ip; uint8_t mac[ETH_ALEN]; int valid; };
static struct arp_entry g_arp[ARP_CACHE_SIZE];

static void arp_cache_put(uint32_t ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) { mac_copy(g_arp[i].mac, mac); return; }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp[i].valid) { g_arp[i].ip = ip; mac_copy(g_arp[i].mac, mac); g_arp[i].valid = 1; return; }
    }
    g_arp[0].ip = ip; mac_copy(g_arp[0].mac, mac); g_arp[0].valid = 1;
}
static int arp_cache_get(uint32_t ip, uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip) { mac_copy(mac, g_arp[i].mac); return 0; }
    return -1;
}

static void arp_send_locked(struct net_device* dev, uint16_t oper,
                            const uint8_t* target_mac, uint32_t target_ip) {
    struct arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETHERTYPE_IPV4);
    a.hlen  = ETH_ALEN;
    a.plen  = 4;
    a.oper  = htons(oper);
    mac_copy(a.sha, dev->mac);
    ip_to_octets(dev->ip, a.spa);
    mac_copy(a.tha, target_mac);
    ip_to_octets(target_ip, a.tpa);
    const uint8_t* dst = (oper == ARP_OP_REQUEST) ? BCAST_MAC : target_mac;
    eth_send_locked(dev, dst, ETHERTYPE_ARP, &a, sizeof(a));
}

static void arp_input(struct net_device* dev, const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt* a = (const struct arp_pkt*)p;
    if (ntohs(a->ptype) != ETHERTYPE_IPV4 || a->plen != 4) return;

    uint32_t spa = octets_to_ip(a->spa);
    uint32_t tpa = octets_to_ip(a->tpa);

    /* Learn the sender either way. */
    arp_cache_put(spa, a->sha);

    if (ntohs(a->oper) == ARP_OP_REQUEST && tpa == dev->ip) {
        /* Somebody wants our MAC → reply to the sender.  We already hold the
         * stack lock (we are inside net_rx) and the target MAC is right there
         * in the request, so this needs no lookup and cannot block. */
        arp_send_locked(dev, ARP_OP_REPLY, a->sha, spa);
    }
}

/* Timeouts, in milliseconds and therefore meaning the same thing on every
 * machine — see defect 3 in the file header. */
#define ARP_TIMEOUT_MS      1000
#define ARP_ATTEMPTS        3

struct arp_wait { uint32_t ip; uint8_t* mac; };
static int arp_cond(void* a) {                  /* evaluated under the lock */
    struct arp_wait* w = (struct arp_wait*)a;
    return arp_cache_get(w->ip, w->mac) == 0;
}

/* MUST NOT be called with the stack lock held — it waits for a reply. */
int net_arp_resolve(struct net_device* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    struct arp_wait w = { ip, mac_out };

    uint32_t f = net_lock();
    int hit = arp_cache_get(ip, mac_out) == 0;
    net_unlock(f);
    if (hit) return 0;

    for (int attempt = 0; attempt < ARP_ATTEMPTS; attempt++) {
        uint32_t rf = net_lock();
        arp_send_locked(dev, ARP_OP_REQUEST, BCAST_MAC, ip);
        net_unlock(rf);
        if (net_wait_cond(arp_cond, &w, ARP_TIMEOUT_MS)) return 0;
    }
    return -1;
}

/* Pick the next hop for `dst_ip` and resolve its MAC: the destination itself if
 * it is on our subnet, otherwise the gateway.  Blocks, so like net_arp_resolve
 * it must be called WITHOUT the stack lock. */
static int route_mac(struct net_device* dev, uint32_t dst_ip, uint8_t mac[ETH_ALEN]) {
    uint32_t nexthop = ((dst_ip & dev->netmask) == (dev->ip & dev->netmask))
                     ? dst_ip : dev->gateway;
    return net_arp_resolve(dev, nexthop, mac);
}

/* ----------------------- ICMP --------------------------------------------- */

/* Reply-tracking state for an in-flight ping (single outstanding at a time). */
static volatile int      g_ping_active  = 0;
static volatile uint16_t g_ping_id      = 0;
static volatile uint16_t g_ping_got_seq = 0;
static volatile int      g_ping_replied = 0;

static void icmp_input(struct net_device* dev, const uint8_t* src_mac,
                       uint32_t src_ip, const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr* ic = (const struct icmp_hdr*)p;

    if (ic->type == ICMP_ECHO_REQUEST) {
        /* Someone is pinging us → echo it straight back (swap type, keep
         * id/seq/payload, recompute checksum).  Straight back means literally
         * that: to the MAC the request arrived from, so this reply costs no
         * lookup and — the part that matters here — cannot block. */
        static uint8_t reply[ETH_MTU];
        if (len > sizeof(reply)) return;
        for (uint32_t i = 0; i < len; i++) reply[i] = p[i];
        struct icmp_hdr* r = (struct icmp_hdr*)reply;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = net_checksum(reply, len);
        ipv4_emit_locked(dev, src_mac, src_ip, IP_PROTO_ICMP, reply, len);
    } else if (ic->type == ICMP_ECHO_REPLY) {
        if (g_ping_active && ntohs(ic->id) == g_ping_id) {
            g_ping_got_seq = ntohs(ic->seq);
            g_ping_replied = 1;
        }
    }
}

#define PING_TIMEOUT_MS 1000

/* The awaited condition: the reply we are actually waiting for.  Matching the
 * sequence number matters — a late reply to the PREVIOUS request would
 * otherwise satisfy this one and report a round trip that never happened. */
static int ping_cond(void* a) {
    uint16_t seq = *(uint16_t*)a;
    return g_ping_replied && g_ping_got_seq == seq;
}

int net_ping(struct net_device* dev, uint32_t ip, int count) {
    char ipb[16]; net_fmt_ip(ip, ipb);
    kprintf("PING %s (%d packets):\n", ipb, count);

    g_ping_id = 0x1234;
    int received = 0;

    for (int seq = 1; seq <= count; seq++) {
        /* Build an echo request with an 8-byte "abcdefgh" payload. */
        uint8_t msg[sizeof(struct icmp_hdr) + 8];
        struct icmp_hdr* ic = (struct icmp_hdr*)msg;
        ic->type = ICMP_ECHO_REQUEST;
        ic->code = 0;
        ic->checksum = 0;
        ic->id  = htons(g_ping_id);
        ic->seq = htons((uint16_t)seq);
        for (int i = 0; i < 8; i++) msg[sizeof(struct icmp_hdr) + i] = 'a' + i;
        ic->checksum = net_checksum(msg, sizeof(msg));

        uint32_t f = net_lock();
        g_ping_replied = 0;
        g_ping_active  = 1;
        net_unlock(f);

        if (net_ipv4_send(dev, ip, IP_PROTO_ICMP, msg, sizeof(msg)) != 0) {
            kprintf("  seq=%d: send failed (ARP?)\n", seq);
            f = net_lock(); g_ping_active = 0; net_unlock(f);
            continue;
        }

        uint16_t want = (uint16_t)seq;
        int got = net_wait_cond(ping_cond, &want, PING_TIMEOUT_MS);

        f = net_lock(); g_ping_active = 0; net_unlock(f);

        if (got) { kprintf("  reply from %s: seq=%d\n", ipb, seq); received++; }
        else     { kprintf("  seq=%d: timeout\n", seq); }
    }

    kprintf("--- %s ping statistics: %d/%d replies ---\n", ipb, received, count);
    return received;
}

/* ----------------------- UDP ---------------------------------------------- */

/* One binding per port is enough for the slice (a stub resolver + a nc-style
 * test).  Grows into the socket layer (§M24 stage 6) later. */
#define UDP_BINDINGS 8
struct udp_binding { uint16_t port; udp_recv_fn fn; void* ctx; int used; };
static struct udp_binding g_udp[UDP_BINDINGS];

/* The binding table is read from the RX path (under the lock) and written from
 * task context, so bind/unbind take the lock too. */
int net_udp_bind(uint16_t port, udp_recv_fn fn, void* ctx) {
    if (!fn) { net_udp_unbind(port); return 0; }
    int rc = -1;
    uint32_t f = net_lock();
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == port) {
            g_udp[i].fn = fn; g_udp[i].ctx = ctx; rc = 0; goto out;
        }
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (!g_udp[i].used) {
            g_udp[i].used = 1; g_udp[i].port = port;
            g_udp[i].fn = fn; g_udp[i].ctx = ctx; rc = 0; goto out;
        }
out:
    net_unlock(f);
    return rc;
}
void net_udp_unbind(uint16_t port) {
    uint32_t f = net_lock();
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == port) g_udp[i].used = 0;
    net_unlock(f);
}

int net_udp_send(struct net_device* dev, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 const void* payload, uint32_t len) {
    static uint8_t buf[ETH_MTU];
    uint32_t total = sizeof(struct udp_hdr) + len;
    if (total > sizeof(buf)) return -1;

    /* Resolve first, unlocked — this is the one part that may block. */
    uint8_t mac[ETH_ALEN];
    if (route_mac(dev, dst_ip, mac) != 0) return -2;

    uint32_t f = net_lock();
    struct udp_hdr* uh = (struct udp_hdr*)buf;
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->len      = htons((uint16_t)total);
    uh->checksum = 0;                          /* omitted (legal for IPv4)   */
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) buf[sizeof(struct udp_hdr) + i] = p[i];
    int rc = ipv4_emit_locked(dev, mac, dst_ip, IP_PROTO_UDP, buf, total);
    net_unlock(f);
    return rc;
}

static void udp_input(struct net_device* dev, uint32_t src_ip,
                      const uint8_t* p, uint32_t len) {
    (void)dev;
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr* uh = (const struct udp_hdr*)p;
    uint16_t dport = ntohs(uh->dst_port);
    uint16_t sport = ntohs(uh->src_port);
    uint16_t ulen  = ntohs(uh->len);
    if (ulen < sizeof(struct udp_hdr) || ulen > len) return;
    const uint8_t* data = p + sizeof(struct udp_hdr);
    uint32_t dlen = ulen - sizeof(struct udp_hdr);

    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == dport) {
            g_udp[i].fn(src_ip, sport, data, dlen, g_udp[i].ctx);
            return;
        }
}

/* ----------------------- DNS stub resolver -------------------------------- */

#define DNS_SERVER  IPV4(10, 0, 2, 3)          /* QEMU SLIRP DNS proxy       */
#define DNS_PORT    53
#define DNS_LOCAL_PORT 0xC353

struct dns_result { volatile int done; volatile int ok; volatile uint32_t ip; uint16_t id; };
static struct dns_result g_dns;

/* Encode "www.example.com" as DNS labels: 3www7example3com0.  Returns the
 * number of bytes written. */
static int dns_encode_name(const char* host, uint8_t* out) {
    int op = 0, lp = 0;
    int label_start = op;
    out[op++] = 0;                             /* placeholder for 1st length  */
    for (const char* c = host; ; c++) {
        if (*c == '.' || *c == '\0') {
            out[label_start] = (uint8_t)lp;
            lp = 0;
            label_start = op;
            if (*c == '\0') break;
            out[op++] = 0;                     /* placeholder for next length */
        } else {
            out[op++] = (uint8_t)*c;
            lp++;
        }
    }
    out[op++] = 0;                             /* root label                  */
    return op;
}

/* Skip a DNS name at `p` (handles a compression pointer or a label sequence).
 * Returns the number of bytes consumed *in this record* (a pointer is 2). */
static uint32_t dns_skip_name(const uint8_t* base, uint32_t off, uint32_t total) {
    uint32_t p = off;
    while (p < total) {
        uint8_t b = base[p];
        if ((b & 0xC0) == 0xC0) { p += 2; return p - off; }   /* pointer      */
        if (b == 0)             { p += 1; return p - off; }   /* root         */
        p += 1 + b;                                           /* label        */
    }
    return p - off;
}

static void dns_recv(uint32_t src_ip, uint16_t src_port,
                     const uint8_t* data, uint32_t len, void* ctx) {
    (void)src_ip; (void)src_port; (void)ctx;
    if (len < 12) return;
    uint16_t id = ((uint16_t)data[0] << 8) | data[1];
    if (id != g_dns.id) return;
    uint16_t ancount = ((uint16_t)data[6] << 8) | data[7];

    uint32_t off = 12;
    /* Skip the single question: name + qtype(2) + qclass(2). */
    off += dns_skip_name(data, off, len);
    off += 4;

    for (uint16_t i = 0; i < ancount && off + 10 <= len; i++) {
        off += dns_skip_name(data, off, len);
        if (off + 10 > len) break;
        uint16_t type   = ((uint16_t)data[off] << 8) | data[off+1];
        uint16_t rdlen  = ((uint16_t)data[off+8] << 8) | data[off+9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {   /* A record        */
            g_dns.ip = ((uint32_t)data[off] << 24) | ((uint32_t)data[off+1] << 16) |
                       ((uint32_t)data[off+2] << 8) | data[off+3];
            g_dns.ok = 1; g_dns.done = 1;
            return;
        }
        off += rdlen;
    }
    g_dns.done = 1;                            /* answered, but no A record    */
}

#define DNS_TIMEOUT_MS 3000

static int dns_cond(void* a) { (void)a; return g_dns.done; }

int net_dns_query(struct net_device* dev, const char* hostname, uint32_t* out_ip) {
    /* Build the DNS query packet. */
    uint8_t q[512];
    g_dns.id = 0xD05;
    q[0] = g_dns.id >> 8; q[1] = g_dns.id & 0xFF;
    q[2] = 0x01; q[3] = 0x00;                  /* flags: recursion desired    */
    q[4] = 0x00; q[5] = 0x01;                  /* qdcount = 1                 */
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    int off = 12;
    off += dns_encode_name(hostname, q + off);
    q[off++] = 0x00; q[off++] = 0x01;          /* qtype  = A                  */
    q[off++] = 0x00; q[off++] = 0x01;          /* qclass = IN                 */

    uint32_t f = net_lock();
    g_dns.done = 0; g_dns.ok = 0; g_dns.ip = 0;
    net_unlock(f);
    net_udp_bind(DNS_LOCAL_PORT, dns_recv, NULL);

    if (net_udp_send(dev, DNS_SERVER, DNS_LOCAL_PORT, DNS_PORT, q, off) != 0) {
        net_udp_unbind(DNS_LOCAL_PORT);
        return -1;
    }

    int rc = -1;
    if (net_wait_cond(dns_cond, NULL, DNS_TIMEOUT_MS)) rc = g_dns.ok ? 0 : -2;
    net_udp_unbind(DNS_LOCAL_PORT);
    if (rc == 0 && out_ip) *out_ip = g_dns.ip;
    return rc;
}

/* ----------------------- TCP (client-only, single connection) ------------- */

enum { TCP_ST_CLOSED = 0, TCP_ST_SYN_SENT, TCP_ST_ESTABLISHED, TCP_ST_CLOSING };

static struct {
    int      state;
    uint32_t peer_ip;
    /* The next hop's MAC, resolved ONCE at connect time.  A connection has a
     * fixed route for its lifetime, so caching it here is not just an
     * optimisation: it is what lets tcp_input ACK from the RX path, where
     * resolving an address would mean blocking with the stack lock held. */
    uint8_t  peer_mac[ETH_ALEN];
    uint16_t peer_port, local_port;
    uint32_t snd_nxt;                          /* next seq we will send       */
    uint32_t rcv_nxt;                          /* next seq we expect          */
    volatile int established;
    volatile int peer_fin;
} g_tcp;

/* Response accumulation buffer (bounded — a first-slice wget, not a stream). */
#define TCP_RX_CAP 16384
static uint8_t         g_tcp_rx[TCP_RX_CAP];
static volatile uint32_t g_tcp_rxlen;
static uint32_t          g_tcp_rxconsumed;   /* net_tcp_recv read cursor */

/* TCP checksum: pseudo-header (src, dst, proto, tcp-len) + segment.  Mandatory
 * (unlike UDP), so we always compute it.  `src_ip`/`dst_ip` are host order. */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t* seg, uint32_t len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF; sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF; sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_TCP;                        /* 0x0006 word                 */
    sum += len;                                 /* TCP length                  */
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += ((uint16_t)seg[i] << 8) | seg[i+1];
    if (len & 1) sum += (uint16_t)seg[len-1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* Send a segment with the given flags + optional payload.  SYN and FIN each
 * consume one sequence number — the caller bumps snd_nxt accordingly. */
static int tcp_send_seg_locked(struct net_device* dev, uint8_t flags,
                               const void* data, uint32_t len) {
    static uint8_t seg[ETH_MTU];
    uint32_t total = sizeof(struct tcp_hdr) + len;
    if (total > sizeof(seg)) return -1;
    struct tcp_hdr* th = (struct tcp_hdr*)seg;
    th->src_port = htons(g_tcp.local_port);
    th->dst_port = htons(g_tcp.peer_port);
    th->seq      = htonl(g_tcp.snd_nxt);
    th->ack      = htonl(g_tcp.rcv_nxt);
    th->data_off = 5 << 4;                      /* 20-byte header, no options  */
    th->flags    = flags;
    th->window   = htons(64240);
    th->checksum = 0;
    th->urgent   = 0;
    const uint8_t* p = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) seg[sizeof(struct tcp_hdr) + i] = p[i];
    th->checksum = tcp_checksum(dev->ip, g_tcp.peer_ip, seg, total);
    return ipv4_emit_locked(dev, g_tcp.peer_mac, g_tcp.peer_ip,
                            IP_PROTO_TCP, seg, total);
}

static void tcp_input(struct net_device* dev, uint32_t src_ip,
                      const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr* th = (const struct tcp_hdr*)p;

    /* Only handle segments for our single active connection. */
    if (g_tcp.state == TCP_ST_CLOSED) return;
    if (src_ip != g_tcp.peer_ip) return;
    if (ntohs(th->dst_port) != g_tcp.local_port) return;
    if (ntohs(th->src_port) != g_tcp.peer_port) return;

    uint32_t their_seq = ntohl(th->seq);
    uint8_t  flags = th->flags;
    uint32_t hlen  = (th->data_off >> 4) * 4;
    if (hlen < sizeof(struct tcp_hdr) || hlen > len) return;
    const uint8_t* data = p + hlen;
    uint32_t dlen = len - hlen;

    if (flags & TCP_RST) { g_tcp.state = TCP_ST_CLOSING; g_tcp.peer_fin = 1; return; }

    if (g_tcp.state == TCP_ST_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            g_tcp.rcv_nxt = their_seq + 1;     /* their SYN consumes a seq    */
            g_tcp.state = TCP_ST_ESTABLISHED;
            g_tcp.established = 1;
            tcp_send_seg_locked(dev, TCP_ACK, NULL, 0);   /* finish the handshake     */
        }
        return;
    }

    /* ESTABLISHED / CLOSING — accept in-order data, ACK it. */
    if (dlen > 0 && their_seq == g_tcp.rcv_nxt) {
        for (uint32_t i = 0; i < dlen && g_tcp_rxlen < TCP_RX_CAP; i++)
            g_tcp_rx[g_tcp_rxlen++] = data[i];
        g_tcp.rcv_nxt += dlen;
        tcp_send_seg_locked(dev, TCP_ACK, NULL, 0);
    } else if (dlen > 0) {
        /* Duplicate / out-of-order — re-ACK what we have. */
        tcp_send_seg_locked(dev, TCP_ACK, NULL, 0);
    }

    if (flags & TCP_FIN) {
        /* FIN occupies the seq right after its data. */
        if (their_seq + dlen == g_tcp.rcv_nxt) {
            g_tcp.rcv_nxt += 1;
            tcp_send_seg_locked(dev, TCP_ACK, NULL, 0);
            g_tcp.peer_fin = 1;
            g_tcp.state = TCP_ST_CLOSING;
        }
    }
}

/* Timeouts for the connection's phases — real milliseconds (see the file
 * header on why the old spin budgets were not timeouts). */
#define TCP_CONNECT_MS   5000
#define TCP_RECV_MS     10000
#define TCP_QUIET_MS     2000       /* "no new data for this long" = done      */
#define TCP_FINWAIT_MS   1000

static int tcp_established_cond(void* a) { (void)a; return g_tcp.established; }
static int tcp_fin_cond(void* a)         { (void)a; return g_tcp.peer_fin;    }
static int tcp_readable_cond(void* a) {
    (void)a;
    return g_tcp_rxconsumed < g_tcp_rxlen || g_tcp.peer_fin;
}

/* Open the connection: resolve the route, publish fresh state, send the SYN and
 * wait for the SYN-ACK.  Resolving happens FIRST and unlocked, because it is
 * the one step that can block; from here on every segment this connection sends
 * — including the ACKs generated on the RX path — is a pure send. */
static int tcp_open(struct net_device* dev, uint32_t ip, uint16_t port) {
    uint8_t mac[ETH_ALEN];
    if (route_mac(dev, ip, mac) != 0) return -1;

    uint32_t f = net_lock();
    g_tcp.state       = TCP_ST_SYN_SENT;
    g_tcp.peer_ip     = ip;
    mac_copy(g_tcp.peer_mac, mac);
    g_tcp.peer_port   = port;
    g_tcp.local_port  = 0xE000 + (g_tcp.local_port & 0x0FFF) + 1;  /* vary port */
    g_tcp.snd_nxt     = 0x2000;                /* our ISN                      */
    g_tcp.rcv_nxt     = 0;
    g_tcp.established = 0;
    g_tcp.peer_fin    = 0;
    g_tcp_rxlen       = 0;
    g_tcp_rxconsumed  = 0;
    tcp_send_seg_locked(dev, TCP_SYN, NULL, 0);
    g_tcp.snd_nxt += 1;                         /* SYN consumes a seq          */
    net_unlock(f);

    if (!net_wait_cond(tcp_established_cond, NULL, TCP_CONNECT_MS)) {
        f = net_lock(); g_tcp.state = TCP_ST_CLOSED; net_unlock(f);
        return -1;
    }
    return 0;
}

/* Send our FIN and give the peer a moment to answer.  Shared by the HTTP
 * helper and the socket API so both close the same way. */
static void tcp_shutdown(struct net_device* dev) {
    uint32_t f = net_lock();
    tcp_send_seg_locked(dev, TCP_FIN | TCP_ACK, NULL, 0);
    g_tcp.snd_nxt += 1;
    net_unlock(f);
    net_wait_cond(tcp_fin_cond, NULL, TCP_FINWAIT_MS);
    f = net_lock(); g_tcp.state = TCP_ST_CLOSED; net_unlock(f);
}

int net_http_get(struct net_device* dev, uint32_t ip, uint16_t port,
                 const char* host, const char* path) {
    if (tcp_open(dev, ip, port) != 0) {
        kprintf("http: connect to %u.%u.%u.%u:%u timed out\n",
                (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF, port);
        return -1;
    }

    /* Build + send the request. */
    char req[512]; int n = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int pi = 0; pi < 5; pi++)
        for (const char* c = parts[pi]; *c && n < (int)sizeof(req) - 1; c++) req[n++] = *c;
    uint32_t f = net_lock();
    tcp_send_seg_locked(dev, TCP_PSH | TCP_ACK, req, (uint32_t)n);
    g_tcp.snd_nxt += (uint32_t)n;
    net_unlock(f);

    /* Drain until the peer FINs or the response goes quiet.  "Quiet" is now a
     * duration rather than a spin count, so each round waits TCP_QUIET_MS and
     * only gives up if nothing at all arrived in that window. */
    uint32_t last_len = 0;
    for (;;) {
        if (net_wait_cond(tcp_fin_cond, NULL, TCP_QUIET_MS)) break;
        uint32_t now_len = g_tcp_rxlen;
        if (now_len == last_len) break;
        last_len = now_len;
    }

    tcp_shutdown(dev);
    return (int)g_tcp_rxlen;
}

/* ---- general TCP client API (M24 socket API) -----------------------------
 * A minimal connect/send/recv/close over the same single-connection g_tcp
 * engine (one TCP socket at a time).  Backs SOCK_STREAM sockets in
 * usyscall.c.  Data is delivered in-order; recv blocks until data arrives or
 * the peer closes. */

int net_tcp_connect(struct net_device* dev, uint32_t ip, uint16_t port) {
    return tcp_open(dev, ip, port);
}

int net_tcp_send(struct net_device* dev, const void* buf, uint32_t len) {
    uint32_t f = net_lock();
    if (g_tcp.state != TCP_ST_ESTABLISHED && g_tcp.state != TCP_ST_CLOSING) {
        net_unlock(f);
        return -1;
    }
    /* One segment (callers send small requests; segmentation is a follow-up). */
    if (len > ETH_MTU - 40) len = ETH_MTU - 40;
    tcp_send_seg_locked(dev, TCP_PSH | TCP_ACK, buf, len);
    g_tcp.snd_nxt += len;
    net_unlock(f);
    return (int)len;
}

int net_tcp_recv(struct net_device* dev, void* buf, uint32_t len) {
    (void)dev;
    net_wait_cond(tcp_readable_cond, NULL, TCP_RECV_MS);

    /* Drain under the lock: the RX path appends to this very buffer, so the
     * "how much is there" and the copy have to be one indivisible step. */
    uint32_t f = net_lock();
    uint32_t avail = g_tcp_rxlen - g_tcp_rxconsumed;
    uint32_t cnt   = avail < len ? avail : len;
    uint8_t* out   = (uint8_t*)buf;
    for (uint32_t i = 0; i < cnt; i++) out[i] = g_tcp_rx[g_tcp_rxconsumed + i];
    g_tcp_rxconsumed += cnt;
    net_unlock(f);

    return (int)cnt;                             /* 0 = EOF (peer FIN, drained) */
}

void net_tcp_close(struct net_device* dev) {
    if (g_tcp.state == TCP_ST_CLOSED) return;
    tcp_shutdown(dev);
}

/* Expose the accumulated response so the shell `wget` command can print it. */
const uint8_t* net_http_body(uint32_t* len_out) {
    if (len_out) *len_out = g_tcp_rxlen;
    return g_tcp_rx;
}

/* ----------------------- IPv4 --------------------------------------------- */

static int ipv4_emit_locked(struct net_device* dev, const uint8_t* via_mac,
                            uint32_t dst_ip, uint8_t proto,
                            const void* payload, uint32_t len) {
    /* Assemble [IPv4 header | payload] in a scratch buffer.  Static, and safe
     * because the caller holds the stack lock. */
    static uint8_t pkt[ETH_MTU];
    uint32_t total = sizeof(struct ipv4_hdr) + len;
    if (total > sizeof(pkt)) return -1;

    struct ipv4_hdr* ih = (struct ipv4_hdr*)pkt;
    ih->ver_ihl   = 0x45;
    ih->tos       = 0;
    ih->total_len = htons((uint16_t)total);
    ih->id        = htons(0);
    ih->frag      = htons(0x4000);             /* Don't Fragment             */
    ih->ttl       = 64;
    ih->proto     = proto;
    ih->checksum  = 0;
    ih->src       = htonl(dev->ip);
    ih->dst       = htonl(dst_ip);
    ih->checksum  = net_checksum(ih, sizeof(struct ipv4_hdr));

    const uint8_t* pl = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) pkt[sizeof(struct ipv4_hdr) + i] = pl[i];

    return eth_send_locked(dev, via_mac, ETHERTYPE_IPV4, pkt, total);
}

/* Task-context send: pick and resolve the next hop, then emit.  MUST NOT be
 * called with the stack lock held (route_mac blocks) — the RX path uses
 * ipv4_emit_locked with the source MAC instead. */
int net_ipv4_send(struct net_device* dev, uint32_t dst_ip, uint8_t proto,
                  const void* payload, uint32_t len) {
    uint8_t mac[ETH_ALEN];
    if (route_mac(dev, dst_ip, mac) != 0) return -2;

    uint32_t f = net_lock();
    int rc = ipv4_emit_locked(dev, mac, dst_ip, proto, payload, len);
    net_unlock(f);
    return rc;
}

static void ipv4_input(struct net_device* dev, const uint8_t* src_mac,
                       const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct ipv4_hdr)) return;
    const struct ipv4_hdr* ih = (const struct ipv4_hdr*)p;
    if ((ih->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (ih->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(struct ipv4_hdr) || ihl > len) return;

    uint32_t dst = ntohl(ih->dst);
    if (dst != dev->ip && dst != 0xFFFFFFFFu) return;   /* not for us         */

    uint32_t src = ntohl(ih->src);
    uint16_t total = ntohs(ih->total_len);
    if (total > len) return;
    const uint8_t* l4 = p + ihl;
    uint32_t l4len = total - ihl;

    switch (ih->proto) {
        case IP_PROTO_ICMP: icmp_input(dev, src_mac, src, l4, l4len); break;
        case IP_PROTO_UDP:  udp_input (dev, src, l4, l4len); break;
        case IP_PROTO_TCP:  tcp_input (dev, src, l4, l4len); break;
        default: break;
    }
}

/* ----------------------- Ethernet demux (RX entry) ------------------------ */

/* Called by the driver's poll (and, once the NIC interrupt lands, from an ISR)
 * with the stack lock HELD — see the file header.  Nothing below here may
 * block; the source MAC is threaded down so replies never need to look one up. */
void net_rx(struct net_device* dev, const uint8_t* frame, uint32_t len) {
    if (len < ETH_HLEN) { dev->rx_dropped++; return; }
    const struct eth_hdr* eh = (const struct eth_hdr*)frame;

    /* Accept frames addressed to us or broadcast. */
    if (!mac_eq(eh->dst, dev->mac) && !mac_eq(eh->dst, BCAST_MAC)) return;

    dev->rx_packets++; dev->rx_bytes += len;
    g_rx_frames++;                            /* what net_pump_locked counts */
    const uint8_t* payload = frame + ETH_HLEN;
    uint32_t plen = len - ETH_HLEN;

    switch (ntohs(eh->ethertype)) {
        case ETHERTYPE_ARP:  arp_input (dev, payload, plen); break;
        case ETHERTYPE_IPV4: ipv4_input(dev, eh->src, payload, plen); break;
        default: break;
    }
}

/* ----------------------- Address parse / format --------------------------- */

int net_parse_ip(const char* s, uint32_t* out) {
    uint32_t o[4] = {0,0,0,0};
    int idx = 0, digits = 0, val = 0;
    for (; ; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            if (val > 255) return -1;
            digits++;
        } else if (c == '.' || c == '\0') {
            if (!digits || idx > 3) return -1;
            o[idx++] = (uint32_t)val;
            val = 0; digits = 0;
            if (c == '\0') break;
        } else {
            return -1;
        }
    }
    if (idx != 4) return -1;
    *out = (o[0] << 24) | (o[1] << 16) | (o[2] << 8) | o[3];
    return 0;
}

void net_fmt_mac(const uint8_t* mac, char* buf) {
    static const char hex[] = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < ETH_ALEN; i++) {
        buf[pos++] = hex[(mac[i] >> 4) & 0xF];
        buf[pos++] = hex[mac[i] & 0xF];
        if (i != ETH_ALEN - 1) buf[pos++] = ':';
    }
    buf[pos] = '\0';
}

void net_fmt_ip(uint32_t ip, char* buf) {
    /* Minimal itoa without depending on libc. */
    unsigned oct[4] = { (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                        (ip >> 8) & 0xFF, ip & 0xFF };
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = oct[i];
        char tmp[3]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
        while (t) buf[pos++] = tmp[--t];
        if (i != 3) buf[pos++] = '.';
    }
    buf[pos] = '\0';
}
