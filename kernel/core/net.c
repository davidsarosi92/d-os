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

#include "task.h"      /* §M90 rung 5 — the caller's network namespace */
#include "nsproxy.h"
#include "config.h"
#include "settings.h"
#include "net.h"
#include "packet.h"
#include "hal.h"
#include "hal_api.h"
#include "printf.h"
#include "waitq.h"
#include "ktimer.h"
#include "timer.h"
#include "task.h"
#include "procfs.h"
#include "fd.h"        /* fd_readiness_signal — wake poll/epoll waiters */
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
#define NET_RT_MAX 64
static struct net_rt g_rt[NET_RT_MAX];

/* ---- §M90 rung 5 — network namespaces -----------------------------------------
 *
 * A namespace is a NUMBER here (0 = the machine's).  What belongs to it says so
 * itself — net_device.netns, tcp_conn.netns, udp_binding.ns, net_rt.ns — and
 * every lookup matches within one, so there is no per-namespace copy of the
 * stack to keep in step: one table, filtered.  The slot table only says which
 * numbers are alive. */
#define NET_NS_MAX 256
static uint8_t g_ns_used[NET_NS_MAX] = { 1 };     /* 0 is always alive */
static int g_next_ifindex = 1;

int net_cur_ns(void) {
    struct task* t = task_current();
    return t ? ns_net_id(t) : 0;
}
int net_ns_alive(int ns) { return ns >= 0 && ns < NET_NS_MAX && g_ns_used[ns]; }

static void arp_drop_dev_locked(struct net_device* dev);

int net_ns_create(void) {
    int id = 0;
    uint32_t f = net_lock();
    for (int i = 1; i < NET_NS_MAX; i++) if (!g_ns_used[i]) { g_ns_used[i] = 1; id = i; break; }
    net_unlock(f);
    if (!id) { kprintf("net: %d network namespaces in use - refused\n", NET_NS_MAX - 1); return 0; }
    /* Every namespace has its own loopback, born DOWN as on Linux (the
     * runtime raises it). */
    if (!netvirt_lo_create(id)) {
        f = net_lock(); g_ns_used[id] = 0; net_unlock(f);
        return 0;
    }
    return id;
}

void net_ns_destroy(int ns) {
    if (ns <= 0 || ns >= NET_NS_MAX) return;
    /* Its virtual devices are destroyed (a veth's peer with it), a hardware
     * device goes back to the machine's namespace — Linux's rules. */
    netvirt_ns_gone(ns);
    uint32_t f = net_lock();
    for (struct net_device* n = g_head; n; n = n->next)
        if (n->netns == ns) { n->netns = 0; n->ip = n->netmask = n->gateway = 0; n->master = NULL; }
    for (int i = 0; i < NET_RT_MAX; i++) if (g_rt[i].used && g_rt[i].ns == ns) g_rt[i].used = 0;
    g_ns_used[ns] = 0;
    net_unlock(f);
}

int net_register(struct net_device* dev) {
    dev->next = NULL;
    net6_dev_init(dev);                          /* §M90 3a — link-local / ::1 */
    /* Linked INSIDE the stack lock: the pump walks the list under it. */
    uint32_t rf = net_lock();
    if (!dev->ifindex) dev->ifindex = g_next_ifindex++;
    if (!g_head) { g_head = dev; }
    else {
        struct net_device* n = g_head;
        while (n->next) n = n->next;
        n->next = dev;
    }
    net_unlock(rf);
    net6_procfs_dev(dev);                        /* §M90 3a — conf/<name>/ */
    char ipb[16], macb[18];
    net_fmt_ip(dev->ip, ipb); net_fmt_mac(dev->mac, macb);
    kprintf("net: registered %s mac=%s ip=%s\n", dev->name, macb, ipb);
    return 0;
}

/* §M67 — withdraw a device.  Written because a LOADABLE network driver has to
 * be removable: the descriptor it registered lives in the module's own memory,
 * which `rmmod` frees, so a registry still pointing at it is a use-after-free
 * on the next frame.
 *
 * WHAT THIS DOES NOT DO, AND WHY IT IS STILL ENOUGH HERE.  It does not wait for
 * a user the way `audio_unregister` does — the network stack has no refcount on
 * a device, and inventing half of one would be worse than none.  It is safe for
 * the loopback device because the RX path runs under the stack lock and this
 * unlink runs from a task with that lock available, so a frame is either fully
 * delivered before the unlink or never starts.  A DMA-capable NIC would need
 * the audio treatment, and the driver that tries should be made to add it
 * rather than to copy this comment. */
static void tcp_dev_gone_locked(struct net_device* dev);   /* below, with the table */
int net_unregister(struct net_device* dev) {
    if (!dev) return -1;
    uint32_t lf = net_lock();
    net_routes_drop_dev(dev);
    arp_drop_dev_locked(dev);                 /* a pointer that must not outlive it */
    /* §M90 — every TCP connection on it ends HERE, as a reset.  A connection
     * holds `dev` for its whole life; one still closing when its device went
     * (FIN_WAIT, LAST_ACK, an unacknowledged FIN after `docker rm` or a
     * namespace's end) used to retransmit 200 ms later through the freed
     * device — a jump through whatever the allocator had put there (x86_64:
     * `invalid opcode at rip 3` in the idle task, after `netnstest`).  Linux
     * loses the route and the connection then times out; here the device
     * pointer would dangle, so the end is immediate. */
    tcp_dev_gone_locked(dev);
    net6_dev_gone_locked(dev);                   /* neighbours, routes, held packets */
    /* Off the device list INSIDE the lock: the pump walks g_head under it,
     * and an unlink racing that walk hands it a device about to be freed. */
    struct net_device** pp = &g_head;
    while (*pp && *pp != dev) pp = &(*pp)->next;
    int found = *pp != NULL;
    if (found) { *pp = dev->next; dev->next = NULL; }
    net_unlock(lf);
    if (!found) return -1;                       /* not registered */
    kprintf("net: unregistered %s\n", dev->name);
    return 0;
}

struct net_device* net_find(const char* name) { return net_find_ns(net_cur_ns(), name); }
struct net_device* net_find_ns(int ns, const char* name) {
    for (struct net_device* n = g_head; n; n = n->next) {
        if (n->netns != ns) continue;
        const char* a = n->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return n;
    }
    return NULL;
}

/* The default-route device.  See net.h for why loopback is excluded rather
 * than merely registered last: registration order is a property of the DRIVER
 * table, and making a routing decision depend on it means one added driver can
 * silently redirect every packet in the system. */
struct net_device* net_by_index(int ns, int ifindex) {
    for (struct net_device* n = g_head; n; n = n->next)
        if (n->ifindex == ifindex && (ns < 0 || n->netns == ns)) return n;
    return NULL;
}

struct net_device* net_primary(void) { return net_primary_ns(net_cur_ns()); }
struct net_device* net_primary_ns(int ns) {
    /* §M87: a SIMULATED adapter never carries traffic, so it is never the
     * route; and an adapter the user DISABLED is passed over while another
     * one is usable — but still returned when it is the only one, so that a
     * send fails with "the network is down" at the transmit gate instead of
     * with "no network device", which would be a lie about the hardware. */
    struct net_device* fallback = NULL;
    for (struct net_device* n = g_head; n; n = n->next) {
        if (n->netns != ns) continue;
        if (n->flags & (NETDEV_F_LOOPBACK | NETDEV_F_SIMULATED)) continue;
        if (n->master) continue;                 /* a bridge port carries no IP */
        if (!n->admin_down) return n;
        if (!fallback) fallback = n;
    }
    return fallback;
}

static struct net_device* net_loopback(int ns) {
    for (struct net_device* n = g_head; n; n = n->next)
        if ((n->flags & NETDEV_F_LOOPBACK) && n->netns == ns) return n;
    return NULL;
}

int net_is_local_ip(uint32_t ip) { return net_is_local_ip_ns(net_cur_ns(), ip); }
int net_is_local_ip_ns(int ns, uint32_t ip) {
    for (struct net_device* n = g_head; n; n = n->next)
        if (n->netns == ns && n->ip && n->ip == ip) return 1;
    if ((ip >> 24) == 127) return net_loopback(ns) != NULL;   /* all of 127/8 */
    return 0;
}

static int mask_len(uint32_t m) { int n = 0; while (m & 0x80000000u) { n++; m <<= 1; } return n; }

struct net_device* net_route(uint32_t dst_ip) { return net_route_ns(net_cur_ns(), dst_ip, NULL); }

/* §M90 rung 5 — the route for `dst_ip` in namespace `ns`, longest prefix
 * first: the CONNECTED routes (a device's own subnet), then the table, then —
 * for a device that has one — its gateway as the default.  *nexthop = the
 * address to resolve on the returned device (the gateway, or `dst_ip`). */
struct net_device* net_route_ns(int ns, uint32_t dst_ip, uint32_t* nexthop) {
    /* 127.0.0.0/8 belongs to the loopback by definition, and nothing else may
     * claim it — a NIC whose subnet was misconfigured to overlap must not
     * silently start carrying localhost traffic onto the wire. */
    if ((dst_ip >> 24) == 127) { if (nexthop) *nexthop = dst_ip; return net_loopback(ns); }
    /* One of the namespace's OWN addresses: Linux's "local" table — the
     * packet never leaves the host, it goes round the loopback. */
    for (struct net_device* n = g_head; n; n = n->next)
        if (n->netns == ns && n->ip == dst_ip && !(n->flags & NETDEV_F_LOOPBACK)) {
            struct net_device* lo = net_loopback(ns);
            if (lo) { if (nexthop) *nexthop = dst_ip; return lo; }
        }

    struct net_device* best = NULL;
    int best_len = -1;
    uint32_t best_gw = 0, best_metric = 0xFFFFFFFFu;
    for (struct net_device* n = g_head; n; n = n->next) {
        if (n->netns != ns || n->master) continue;
        if (n->flags & (NETDEV_F_LOOPBACK | NETDEV_F_SIMULATED)) continue;
        if (n->admin_down || !n->netmask || !n->ip) continue;
        if ((dst_ip & n->netmask) != (n->ip & n->netmask)) continue;
        int l = mask_len(n->netmask);
        if (l > best_len) { best = n; best_len = l; best_gw = 0; best_metric = 0; }
    }
    for (int i = 0; i < NET_RT_MAX; i++) {
        struct net_rt* r = &g_rt[i];
        if (!r->used || r->ns != ns || !r->dev || r->dev->admin_down) continue;
        if ((dst_ip & r->mask) != r->dst) continue;
        int l = mask_len(r->mask);
        if (l > best_len || (l == best_len && r->metric < best_metric)) {
            best = r->dev; best_len = l; best_gw = r->gw; best_metric = r->metric;
        }
    }
    if (best_len < 0) {
        /* No route matched: a device's own gateway is the default (DHCP's, or
         * the driver's built-in numbers) — the machine's primary first. */
        struct net_device* p = net_primary_ns(ns);
        if (p && p->gateway && !p->admin_down) { best = p; best_gw = p->gateway; }
        else {
            for (struct net_device* n = g_head; n && !best; n = n->next)
                if (n->netns == ns && n->gateway && !n->admin_down && !n->master) { best = n; best_gw = n->gateway; }
        }
        if (!best && ns == 0) best = p;          /* the old answer: the primary */
    }
    if (nexthop) *nexthop = best_gw ? best_gw : dst_ip;
    return best;
}

int net_route_add(int ns, uint32_t dst, uint32_t mask, uint32_t gw, struct net_device* dev, uint32_t metric) {
    if (!dev) return -1;
    dst &= mask;
    uint32_t f = net_lock();
    int r = -1;
    for (int i = 0; i < NET_RT_MAX; i++)
        if (g_rt[i].used && g_rt[i].ns == ns && g_rt[i].dst == dst && g_rt[i].mask == mask &&
            g_rt[i].dev == dev && g_rt[i].metric == metric) { r = -2; goto out; }
    for (int i = 0; i < NET_RT_MAX; i++)
        if (!g_rt[i].used) {
            g_rt[i].used = 1; g_rt[i].ns = ns; g_rt[i].dst = dst; g_rt[i].mask = mask;
            g_rt[i].gw = gw; g_rt[i].dev = dev; g_rt[i].metric = metric;
            r = 0; break;
        }
out:
    net_unlock(f);
    return r;
}
int net_route_del(int ns, uint32_t dst, uint32_t mask, struct net_device* dev) {
    uint32_t f = net_lock();
    int r = -1;
    for (int i = 0; i < NET_RT_MAX; i++)
        if (g_rt[i].used && g_rt[i].ns == ns && g_rt[i].dst == (dst & mask) && g_rt[i].mask == mask &&
            (!dev || g_rt[i].dev == dev)) { g_rt[i].used = 0; r = 0; break; }
    net_unlock(f);
    return r;
}
int net_route_at(int i, struct net_rt* out) {
    if (i < 0 || i >= NET_RT_MAX) return -1;
    *out = g_rt[i];
    return 0;
}
void net_routes_drop_dev(struct net_device* dev) {
    for (int i = 0; i < NET_RT_MAX; i++) if (g_rt[i].used && g_rt[i].dev == dev) g_rt[i].used = 0;
}

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
        /* §M90 rung 5 — which namespace, which bridge, up or down: every
         * namespace is listed (the machine's view of all of them). */
        kprintf("     netns %d  ifindex %d  %s%s%s%s\n", n->netns, n->ifindex,
                n->admin_down ? "DOWN" : "UP",
                n->kind == NETDEV_K_VETH ? "  veth" : n->kind == NETDEV_K_BRIDGE ? "  bridge" :
                n->kind == NETDEV_K_LOOPBACK ? "  loopback" : "",
                n->master ? "  master " : "", n->master ? n->master->name : "");
        kprintf("     RX %u pkts / %u bytes   TX %u pkts / %u bytes   drop %u\n",
                n->rx_packets, n->rx_bytes, n->tx_packets, n->tx_bytes, n->rx_dropped);
    }
    {
        uint32_t fw, na, nr, no, ni, nf;
        net_fwd_stats(&fw, &na, &nr, &no, &ni, &nf);
        kprintf("forward: ip_forward(ns 0) %d  forwarded %u  no-arp %u  no-route %u  masq out %u in %u\n",
                net_ip_forward(0), fw, na, nr, no, ni);
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
/* §M90 rung 5 — a VIRTUAL device queued a frame (netvirt.c): wake the poller
 * if it waits for an interrupt, without claiming the NIC has one. */
void net_kick(void) {
    uint32_t f = waitq_lock(&g_nicwq);
    g_irq_seq++;
    waitq_wake_all(&g_nicwq);
    waitq_unlock(&g_nicwq, f);
}

void net_rx_irq(struct net_device* dev) {
    (void)dev;
    uint32_t f = waitq_lock(&g_nicwq);
    g_irq_seq++;
    g_irq_count++;
    g_nic_irq_seen = 1;
    waitq_wake_all(&g_nicwq);
    waitq_unlock(&g_nicwq, f);
}

/* §M24 — the poller's "somebody cares" counter, exposed.
 *
 * netd runs only while somebody is waiting for the network (§M55), and until
 * now the only way to be that somebody was to call net_wait_cond().  A task
 * blocked in poll()/epoll_wait() on a socket is waiting just as much and had
 * no way to say so — so the poller stayed parked, no frames were ever
 * received, and the wait ran to its timeout with the answer sitting in the
 * NIC.  That is what broke musl's resolver (and with it every page load)
 * the moment §M56 turned a finite poll timeout into a real wait. */
void net_waiter_enter(void);
void net_waiter_leave(void);

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
    /* AFTER the unlock, never inside it.  A poll waiter holds the readiness
     * queue's lock while its scan takes the stack lock, so signalling from
     * under the stack lock would be the opposite order and the two would
     * deadlock the first time they raced. */
    if (got) fd_readiness_signal();
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

void net_waiter_enter(void) { net_wait_begin(); }
void net_waiter_leave(void) { net_wait_end();   }

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
            /* schedule(), NOT task_yield(): task_yield EXITS a task with a
             * pending kill, from right here — past the ktimer_cancel below,
             * leaving `t` armed on a stack about to be freed (see
             * ktimer_cancel_range).  The loop's own task_should_stop() check
             * is the exit, and it runs the cancel. */
            schedule();
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
        /* Wake poll()/epoll_wait() too — they wait on a different queue, and a
         * socket that became readable is exactly what they are there for.
         * Outside the lock: see net_pump_once. */
        if (got) fd_readiness_signal();

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
    /* §M87 — the admin gate.  ONE place every emit path passes through, so a
     * disabled adapter cannot be reached by a path somebody forgot. */
    if (dev->admin_down) return -1;
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
/* `src_ip` is passed EXPLICITLY rather than taken from dev->ip.  Two callers
 * need it to be something else: a connection carries the address it was
 * established on (which, once the host has more than one device, is not always
 * the sending device's primary address), and DHCP must send from 0.0.0.0
 * before it has an address at all.  A sentinel meaning "use the device's"
 * would have to be a value no real address can take, and 0.0.0.0 — the one
 * obvious candidate — is precisely the value DHCP needs to send. */
static int ipv4_emit_locked(struct net_device* dev, const uint8_t* via_mac,
                            uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                            const void* payload, uint32_t len);

/* ----------------------- ARP ---------------------------------------------- */

/* A tiny fixed-size cache — plenty for a host + a gateway.  LRU-free: on a
 * full cache we overwrite slot 0 (good enough for the slice). */
/* §M90 rung 5 — an entry belongs to the DEVICE it was learned on: two
 * namespaces (or two bridges) may each have a neighbour at 172.17.0.2. */
#define ARP_CACHE_SIZE 32
struct arp_entry { uint32_t ip; uint8_t mac[ETH_ALEN]; int valid; struct net_device* dev; };
static struct arp_entry g_arp[ARP_CACHE_SIZE];
static int g_arp_next;

static void arp_cache_put(struct net_device* dev, uint32_t ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip && g_arp[i].dev == dev) { mac_copy(g_arp[i].mac, mac); return; }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp[i].valid) { g_arp[i].ip = ip; g_arp[i].dev = dev; mac_copy(g_arp[i].mac, mac); g_arp[i].valid = 1; return; }
    }
    int v = g_arp_next++ % ARP_CACHE_SIZE;          /* full: replace round-robin */
    g_arp[v].ip = ip; g_arp[v].dev = dev; mac_copy(g_arp[v].mac, mac); g_arp[v].valid = 1;
}
static int arp_cache_get(struct net_device* dev, uint32_t ip, uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (g_arp[i].valid && g_arp[i].ip == ip && g_arp[i].dev == dev) { mac_copy(mac, g_arp[i].mac); return 0; }
    return -1;
}
static void arp_hold_drop_dev(struct net_device* dev);
/* A device moved or destroyed: what it learned is no longer true. */
static void arp_drop_dev_locked(struct net_device* dev) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) if (g_arp[i].dev == dev) g_arp[i].valid = 0;
    arp_hold_drop_dev(dev);                        /* held packets for it too */
}

/* §M90 rung 5 — packets FORWARDED to a next hop whose MAC is being asked for:
 * held until the ARP reply (arp_input releases them) or until they are 3 s
 * old.  A few slots, as Linux's per-neighbour queue is a few packets. */
#define ARP_HOLD_SLOTS 8
#define ARP_HOLD_MS    3000
static struct { int used; struct net_device* dev; uint32_t nh; uint32_t len; uint64_t ms;
                uint8_t data[ETH_MTU]; } g_arp_hold[ARP_HOLD_SLOTS];
static uint32_t g_arp_held, g_arp_released;

static int arp_hold_locked(struct net_device* dev, uint32_t nh, const uint8_t* pkt, uint32_t len) {
    if (len > ETH_MTU) return -1;
    uint64_t now = timer_ticks_ms();
    int slot = -1;
    for (int i = 0; i < ARP_HOLD_SLOTS; i++) {
        if (g_arp_hold[i].used && now - g_arp_hold[i].ms > ARP_HOLD_MS) g_arp_hold[i].used = 0;
        if (!g_arp_hold[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return -1;                       /* full: dropped (counted) */
    g_arp_hold[slot].used = 1; g_arp_hold[slot].dev = dev; g_arp_hold[slot].nh = nh;
    g_arp_hold[slot].len = len; g_arp_hold[slot].ms = now;
    for (uint32_t i = 0; i < len; i++) g_arp_hold[slot].data[i] = pkt[i];
    g_arp_held++;
    return 0;
}
static void arp_hold_drop_dev(struct net_device* dev) {
    for (int i = 0; i < ARP_HOLD_SLOTS; i++) if (g_arp_hold[i].dev == dev) g_arp_hold[i].used = 0;
}
/* `ip` on `dev` just became known: send what waited for it. */
static void arp_release_locked(struct net_device* dev, uint32_t ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_HOLD_SLOTS; i++) {
        if (!g_arp_hold[i].used || g_arp_hold[i].dev != dev || g_arp_hold[i].nh != ip) continue;
        g_arp_hold[i].used = 0;
        eth_send_locked(dev, mac, ETHERTYPE_IPV4, g_arp_hold[i].data, g_arp_hold[i].len);
        g_arp_released++;
    }
}

int net_dev_set_ns(struct net_device* dev, int ns) {
    if (!dev || !net_ns_alive(ns)) return -1;
    uint32_t f = net_lock();
    dev->netns = ns;
    dev->ip = dev->netmask = dev->gateway = 0;     /* addresses do not travel */
    dev->master = NULL;                           /* nor bridge membership   */
    net_routes_drop_dev(dev);
    arp_drop_dev_locked(dev);
    net_unlock(f);
    return 0;
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
    arp_cache_put(dev, spa, a->sha);
    arp_release_locked(dev, spa, a->sha);          /* §M90 — packets waiting for it */

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

struct arp_wait { uint32_t ip; uint8_t* mac; struct net_device* dev; };
static int arp_cond(void* a) {                  /* evaluated under the lock */
    struct arp_wait* w = (struct arp_wait*)a;
    return arp_cache_get(w->dev, w->ip, w->mac) == 0;
}

/* MUST NOT be called with the stack lock held — it waits for a reply. */
int net_arp_resolve(struct net_device* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    struct arp_wait w = { ip, mac_out, dev };

    uint32_t f = net_lock();
    int hit = arp_cache_get(dev, ip, mac_out) == 0;
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
    /* A loopback has no wire and therefore no neighbours: there is nobody to
     * answer an ARP request, so asking would spend the full retry budget and
     * then fail.  Any MAC will do — the device hands the frame straight back —
     * so we use its own, which is what a frame "from us to us" should carry. */
    if (dev->flags & NETDEV_F_LOOPBACK) { mac_copy(mac, dev->mac); return 0; }

    /* §M90 rung 5 — the next hop from the ROUTE TABLE of the device's
     * namespace (a container's default route is a table entry, not a field). */
    uint32_t nexthop = dst_ip;
    if (!(dev->netmask && (dst_ip & dev->netmask) == (dev->ip & dev->netmask))) {
        uint32_t nh = 0;
        struct net_device* via = net_route_ns(dev->netns, dst_ip, &nh);
        nexthop = (via == dev && nh) ? nh : (dev->gateway ? dev->gateway : dst_ip);
    }
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
        ipv4_emit_locked(dev, src_mac, dev->ip, src_ip, IP_PROTO_ICMP, reply, len);
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
#define UDP_BINDINGS 64
struct udp_binding { uint16_t port; udp_recv_fn fn; void* ctx; int used; int ns; };
static struct udp_binding g_udp[UDP_BINDINGS];

/* The binding table is read from the RX path (under the lock) and written from
 * task context, so bind/unbind take the lock too. */
int net_udp_bind(uint16_t port, udp_recv_fn fn, void* ctx) { return net_udp_bind_ns(net_cur_ns(), port, fn, ctx); }
void net_udp_unbind(uint16_t port) { net_udp_unbind_ns(net_cur_ns(), port); }
int net_udp_bind_ns(int ns, uint16_t port, udp_recv_fn fn, void* ctx) {
    if (!fn) { net_udp_unbind_ns(ns, port); return 0; }
    int rc = -1;
    uint32_t f = net_lock();
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == port && g_udp[i].ns == ns) {
            g_udp[i].fn = fn; g_udp[i].ctx = ctx; rc = 0; goto out;
        }
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (!g_udp[i].used) {
            g_udp[i].used = 1; g_udp[i].port = port; g_udp[i].ns = ns;
            g_udp[i].fn = fn; g_udp[i].ctx = ctx; rc = 0; goto out;
        }
out:
    net_unlock(f);
    return rc;
}
void net_udp_unbind_ns(int ns, uint16_t port) {
    uint32_t f = net_lock();
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == port && g_udp[i].ns == ns) g_udp[i].used = 0;
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
    int rc = ipv4_emit_locked(dev, mac, dev->ip, dst_ip, IP_PROTO_UDP, buf, total);
    net_unlock(f);
    return rc;
}

/* Send a UDP datagram to the all-ones broadcast address, from `src_ip` (which
 * for DHCP is 0.0.0.0 — the whole point being that we do not have an address
 * yet).  Separate from net_udp_send because BOTH of that function's normal
 * steps are impossible here: there is no source address to put in the header,
 * and there is nobody to ARP for a destination that is by definition everyone. */
int net_udp_broadcast(struct net_device* dev, uint32_t src_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const void* payload, uint32_t len) {
    static uint8_t buf[ETH_MTU];
    uint32_t total = sizeof(struct udp_hdr) + len;
    if (!dev || total > sizeof(buf)) return -1;

    uint32_t f = net_lock();
    struct udp_hdr* uh = (struct udp_hdr*)buf;
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->len      = htons((uint16_t)total);
    uh->checksum = 0;
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) buf[sizeof(struct udp_hdr) + i] = p[i];
    int rc = ipv4_emit_locked(dev, BCAST_MAC, src_ip, 0xFFFFFFFFu,
                              IP_PROTO_UDP, buf, total);
    net_unlock(f);
    return rc;
}

static void udp_input(struct net_device* dev, uint32_t src_ip,
                      const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr* uh = (const struct udp_hdr*)p;
    uint16_t dport = ntohs(uh->dst_port);
    uint16_t sport = ntohs(uh->src_port);
    uint16_t ulen  = ntohs(uh->len);
    if (ulen < sizeof(struct udp_hdr) || ulen > len) return;
    const uint8_t* data = p + sizeof(struct udp_hdr);
    uint32_t dlen = ulen - sizeof(struct udp_hdr);

    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == dport && g_udp[i].ns == dev->netns) {
            g_udp[i].fn(src_ip, sport, data, dlen, g_udp[i].ctx);
            return;
        }
}

/* ----------------------- §M90 — ICMP sockets ------------------------------- */

/* A copy of every ICMP packet for a local address goes to each listener of
 * the receiving device's namespace: SOCK_RAW/IPPROTO_ICMP and the ping socket
 * (SOCK_DGRAM/IPPROTO_ICMP) of usyscall.c filter what they keep.  The kernel's
 * own echo replies and its `ping` are unaffected. */
#define ICMP_LISTENERS 32
static struct { int used, ns; icmp_recv_fn fn; void* ctx; } g_icmpl[ICMP_LISTENERS];

int net_icmp_listen(int ns, icmp_recv_fn fn, void* ctx) {
    uint32_t f = net_lock();
    int r = -1;
    for (int i = 0; i < ICMP_LISTENERS; i++)
        if (!g_icmpl[i].used) { g_icmpl[i].used = 1; g_icmpl[i].ns = ns; g_icmpl[i].fn = fn; g_icmpl[i].ctx = ctx; r = 0; break; }
    net_unlock(f);
    return r;
}
void net_icmp_unlisten(void* ctx) {
    uint32_t f = net_lock();
    for (int i = 0; i < ICMP_LISTENERS; i++) if (g_icmpl[i].used && g_icmpl[i].ctx == ctx) g_icmpl[i].used = 0;
    net_unlock(f);
}
static void icmp_deliver_locked(struct net_device* dev, uint32_t src, const uint8_t* ip, uint32_t total) {
    for (int i = 0; i < ICMP_LISTENERS; i++)
        if (g_icmpl[i].used && g_icmpl[i].ns == dev->netns) g_icmpl[i].fn(src, ip, total, g_icmpl[i].ctx);
}

/* ----------------------- DNS stub resolver -------------------------------- */

/* The resolver's server address.  A DEFAULT rather than a constant since
 * §M24.12: DHCP learns the real one (option 6) and installs it here, and a
 * hard-coded 10.0.2.3 is only correct on the emulator it was written for. */
#define DNS_SERVER_DEFAULT  IPV4(10, 0, 2, 3)  /* QEMU SLIRP DNS proxy       */
static uint32_t g_dns_server = DNS_SERVER_DEFAULT;

void net_set_dns(uint32_t ip)  { if (ip) g_dns_server = ip; }
uint32_t net_get_dns(void)     { return g_dns_server; }
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

    if (net_udp_send(dev, g_dns_server, DNS_LOCAL_PORT, DNS_PORT, q, off) != 0) {
        net_udp_unbind(DNS_LOCAL_PORT);
        return -1;
    }

    int rc = -1;
    if (net_wait_cond(dns_cond, NULL, DNS_TIMEOUT_MS)) rc = g_dns.ok ? 0 : -2;
    net_udp_unbind(DNS_LOCAL_PORT);
    if (rc == 0 && out_ip) *out_ip = g_dns.ip;
    return rc;
}

/* =============================================================================
 * TCP (§M24, second half) — a CONNECTION TABLE, a server role, a send buffer
 * and retransmission.
 *
 * WHAT WAS HERE BEFORE, AND WHY IT HAD TO GO.  The first slice kept exactly one
 * connection in a file-scope `g_tcp`, one receive buffer that only ever grew,
 * and no send buffer at all: `net_tcp_send` put the caller's bytes on the wire
 * and forgot them.  Every one of those is fine for "fetch one page and print
 * it", which is what it was written for, and none survives contact with an
 * event loop:
 *
 *   - ONE CONNECTION.  §M56 gave this kernel poll and epoll — machinery whose
 *     entire purpose is watching several descriptors at once — over a transport
 *     that could hold one conversation at a time.  A second connect() silently
 *     destroyed the first.
 *   - NO SERVER ROLE.  Nothing could arrive from outside, so the accept path
 *     that every network program is written around did not exist and could not
 *     be tested even in principle.
 *   - A RECEIVE BUFFER THAT ONLY GREW.  Sixteen kilobytes of response and the
 *     connection stalled for good, because nothing was ever reclaimed while the
 *     advertised window stayed a constant that had stopped being true.
 *   - NO SEND BUFFER.  Bytes that left were unrecoverable, so one dropped
 *     segment ended the conversation instead of costing a round trip.
 *
 * THE SHAPE NOW.  A bounded table of `struct tcp_conn`, demultiplexed by the
 * four-tuple (local ip/port, peer ip/port) with a LISTEN entry as the fallback
 * match; per-connection receive ring, send buffer and sequence state; a
 * retransmission sweep on one timer.
 *
 * The table is STATIC, not heap-allocated, and that is deliberate: a connection
 * is created on the RX path, which runs under the stack lock with interrupts
 * off, and an allocator call there would nest the heap's lock inside the
 * network lock on the one path that must never fail.  Sixteen connections is a
 * limit this kernel can state honestly; a heap that might not answer is not.
 *
 * OWNERSHIP — the rule that keeps this safe without a refcount.  A connection
 * is released by exactly one route: whoever owns it calls net_tcp_close.
 * Connections a LISTEN created for peers nobody has accepted yet are owned by
 * that listener and go down with it.  THE RX PATH NEVER FREES A CONNECTION; it
 * may only change one's state.  That is what makes a pointer held by a socket
 * safe to dereference for as long as the socket exists — §M54's defect class
 * ruled out by construction instead of by a lifetime rule someone must
 * remember.
 * ============================================================================= */

/* Sequence arithmetic is MODULAR: 0xFFFFFFFF + 1 == 0 is an ordinary event in a
 * long-lived connection, so comparisons must be made on the SIGNED difference.
 * Writing `a < b` on the raw values works for hours and then mis-orders a
 * stream exactly once per 4 GiB — a bug that gets blamed on the network. */
static inline int seq_lt (uint32_t a, uint32_t b) { return (int32_t)(a - b) <  0; }
static inline int seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline int seq_gt (uint32_t a, uint32_t b) { return (int32_t)(a - b) >  0; }

enum {
    TCP_ST_CLOSED = 0,
    TCP_ST_LISTEN,
    TCP_ST_SYN_SENT,
    TCP_ST_SYN_RCVD,
    TCP_ST_ESTABLISHED,
    TCP_ST_FIN_WAIT_1,      /* our FIN is out, not acknowledged yet        */
    TCP_ST_FIN_WAIT_2,      /* our FIN is acked; theirs has not arrived    */
    TCP_ST_CLOSE_WAIT,      /* they are done sending; we still may send    */
    TCP_ST_LAST_ACK,        /* we answered their FIN with ours             */
    TCP_ST_TIME_WAIT,       /* both done; absorbing stray retransmissions  */
};

static const char* const TCP_STATE_NAMES[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED",
    "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "LAST_ACK", "TIME_WAIT",
};

/* How many connections may exist at once.
 *
 * The number is larger than it looks, for a reason that is easy to get wrong:
 * OVER THE LOOPBACK EVERY CONNECTION OCCUPIES TWO ENTRIES, because both
 * endpoints live in this table.  Eight concurrent local connections plus their
 * listener is seventeen — which is how the first `tcptest 8` run came back
 * 7/8 with one RST sent, and it was the stack telling the truth about a limit
 * rather than a bug. */
#define TCP_MAX_CONNS   32
#define TCP_RXBUF       8192
#define TCP_TXBUF       8192
/* The accept queue is a RING, so one slot is always the empty marker and the
 * largest honourable backlog is one less.  Sized to the connection table
 * rather than to a round number: a listener that could queue more peers than
 * the stack can hold connections would accept SYNs it must then refuse. */
#define TCP_ACCEPTQ     TCP_MAX_CONNS
#define TCP_MSS_CAP     1460          /* ETH_MTU - IPv4(20) - TCP(20)        */

struct tcp_conn {
    uint8_t  used;
    uint8_t  listener;
    uint8_t  state;
    uint8_t  fin_pending;       /* close requested; FIN goes once tx drains   */
    uint8_t  fin_sent;          /* our FIN occupies a sequence number         */
    uint32_t fin_seq;           /* WHICH sequence number.  Recorded rather
                                 * than derived from snd_nxt, because a
                                 * retransmission rolls snd_nxt BACK, and a
                                 * "have they acknowledged my FIN?" test made
                                 * of a rolled-back snd_nxt answers yes to an
                                 * acknowledgement of the data alone. */
    uint8_t  orphan;            /* owner is gone; the sweeper reclaims it     */
    volatile uint8_t peer_fin;  /* their FIN arrived and was acknowledged     */
    volatile uint8_t reset;     /* RST — an ERROR, not an orderly end (§M56.2)*/

    struct net_device* dev;
    int      netns;             /* §M90 rung 5 — the namespace it lives in    */
    uint32_t local_ip, peer_ip;
    uint16_t local_port, peer_port;
    uint8_t  peer_mac[ETH_ALEN];

    /* Send side.  tx[] is LINEAR, not a ring: tx[i] is the byte at sequence
     * snd_una + i, and an acknowledgement compacts it.  A ring would save the
     * compaction memmove and cost a wrap case in every one of emit, queue and
     * retransmit — three places to get an off-by-one wrong on a path where the
     * symptom is a corrupted byte stream a megabyte later. */
    uint32_t snd_una, snd_nxt, snd_wnd;
    uint8_t  tx[TCP_TXBUF];
    uint32_t tx_len;

    /* Receive side.  A RING, with rx_head/rx_tail as monotonic byte counters,
     * so the free space they imply IS the window we advertise — an
     * advertisement that follows the reader rather than a constant that stops
     * being true the moment the reader falls behind. */
    uint32_t rcv_nxt;
    uint8_t  rx[TCP_RXBUF];
    uint32_t rx_head, rx_tail;

    /* Retransmission + lingering. */
    uint64_t rto_deadline_ns;         /* 0 = nothing outstanding             */
    uint32_t rto_ms;
    uint32_t retries;
    uint64_t linger_deadline_ns;      /* TIME_WAIT / orphan reclaim          */

    /* Listener state. */
    struct tcp_conn* aq[TCP_ACCEPTQ];
    uint8_t  aq_head, aq_tail, backlog;
    struct tcp_conn* parent;

    /* Per-connection counters — `netstat` prints them, and the loss test
     * asserts on `retrans`, which is the only way to tell a recovery from a
     * link that never lost anything. */
    uint32_t tx_segs, rx_segs, retrans;

    /* §M90 3a — IPv6.  `fam6` = the connection runs over IPv6 and its ends
     * are l6/p6 (local_ip/peer_ip stay 0).  On a LISTENER, fam6 means it was
     * opened on an IPv6 socket, and `v6only` = 0 makes it DUAL-STACK: it also
     * accepts IPv4 (the child is then an ordinary IPv4 connection; the socket
     * layer shows its addresses as ::ffff:a.b.c.d). */
    uint8_t  fam6, v6only;
    uint8_t  l6[16], p6[16];
};

static struct tcp_conn g_conns[TCP_MAX_CONNS];

/* §M90 — a device is going: the connections on it end as resets (see
 * net_unregister).  Stack lock held. */
static void tcp_dev_gone_locked(struct net_device* dev) {
    int gone = 0;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &g_conns[i];
        if (!c->used || c->dev != dev) continue;
        c->dev = NULL;
        c->reset = 1;
        c->peer_fin = 1;
        if (!c->listener) c->state = TCP_ST_CLOSED;
        c->rto_deadline_ns = 0;
        gone++;
    }
    if (gone) waitq_wake_all(&g_netwq);         /* their readers see the reset */
}
static uint32_t g_tcp_ephem = 0;
static uint32_t g_tcp_isn   = 0x2000;
static uint32_t g_tcp_rst_sent, g_tcp_rst_rcvd, g_tcp_dropped;
/* Why these three exist: a transfer that is slow for a bad reason and one that
 * is slow for a good reason look identical from the outside, and the only way
 * to tell them apart is to count the events that carry a 200 ms price tag. */
static uint32_t g_tcp_persists;     /* zero-window probes sent               */
static uint32_t g_tcp_wndblock;     /* output runs that sent NOTHING because
                                     * the peer's window had no room         */
static uint32_t g_tcp_datasegs;     /* data segments actually emitted        */
static uint32_t g_tcp_rtos;         /* retransmit timeouts that fired        */
static uint32_t g_tcp_zerowin;      /* times WE advertised a shut window     */

/* ----------------------- the table (always under the stack lock) ---------- */

static struct tcp_conn* tcp_alloc_locked(void) {
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &g_conns[i];
        if (c->used) continue;
        /* Reset every field EXCEPT the two 8 KiB buffers: they are defined by
         * their counters, so bytes below the head are unreachable, and zeroing
         * 16 KiB here would put a memset of that size on the RX path with
         * interrupts off, once per arriving SYN. */
        c->used = 1;
        c->listener = c->state = c->fin_pending = c->fin_sent = c->orphan = 0;
        c->peer_fin = c->reset = 0;
        c->dev = NULL;
        c->netns = 0;
        c->local_ip = c->peer_ip = 0;
        c->local_port = c->peer_port = 0;
        for (int k = 0; k < ETH_ALEN; k++) c->peer_mac[k] = 0;
        c->snd_una = c->snd_nxt = c->snd_wnd = c->tx_len = 0;
        c->fin_seq = 0;
        c->rcv_nxt = c->rx_head = c->rx_tail = 0;
        c->rto_deadline_ns = 0; c->rto_ms = 0; c->retries = 0;
        c->linger_deadline_ns = 0;
        c->aq_head = c->aq_tail = c->backlog = 0;
        c->parent = NULL;
        c->tx_segs = c->rx_segs = c->retrans = 0;
        c->fam6 = c->v6only = 0;
        for (int k = 0; k < 16; k++) c->l6[k] = c->p6[k] = 0;
        return c;
    }
    return NULL;
}

static void tcp_free_locked(struct tcp_conn* c) {
    if (!c) return;
    c->state = TCP_ST_CLOSED;
    c->used  = 0;
}

/* §M90 3a — the IPv6 addresses of the segment being handled, set by
 * net_l4_input6 for the length of ONE delivery (the RX path holds the stack
 * lock throughout, so nothing else can see them); NULL = an IPv4 segment.
 * This is what lets tcp_input/udp_input serve both families unchanged. */
static const uint8_t* g_in6_src;
static const uint8_t* g_in6_dst;
const uint8_t* net_rx_src6(void) { return g_in6_src; }
const uint8_t* net_rx_dst6(void) { return g_in6_dst; }
static int a6z(const uint8_t* a) { for (int i = 0; i < 16; i++) if (a[i]) return 0; return 1; }
static int a6e(const uint8_t* a, const uint8_t* b) { for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0; return 1; }
static void a6c(uint8_t* d, const uint8_t* s_) { for (int i = 0; i < 16; i++) d[i] = s_[i]; }

/* Which families a connection-table entry holds its port in: bit 0 = IPv4,
 * bit 1 = IPv6.  A dual-stack listener holds both — so an IPv4 listener and
 * a v6-only one may share a port (Docker's proxy opens 0.0.0.0:P and [::]:P),
 * while two listeners in one family may not. */
static int tcp_fam_mask(const struct tcp_conn* c) {
    if (!c->fam6) return 1;
    return c->listener && !c->v6only ? 3 : 2;
}
static int tcp_port_clash_locked(int ns, uint16_t port, int mask) {
    for (int i = 0; i < TCP_MAX_CONNS; i++)
        if (g_conns[i].used && g_conns[i].netns == ns && g_conns[i].local_port == port &&
            (tcp_fam_mask(&g_conns[i]) & mask)) return 1;
    return 0;
}
static int tcp_port_taken_locked(int ns, uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNS; i++)
        if (g_conns[i].used && g_conns[i].netns == ns && g_conns[i].local_port == port) return 1;
    return 0;
}

static uint16_t tcp_ephem_port_locked(int ns) {
    for (int tries = 0; tries < 0x4000; tries++) {
        uint16_t p = (uint16_t)(0xC000 + (g_tcp_ephem++ & 0x3FFF));
        if (!tcp_port_taken_locked(ns, p)) return p;
    }
    return 0;
}

/* Which connection does this segment belong to?  Exact four-tuple first, a
 * LISTEN on the same local port only as a fallback — the ORDER is load-bearing:
 * a listener and the connections it accepted share a local port, so matching
 * the listener first would route every segment of every accepted connection
 * into the accept queue. */
static struct tcp_conn* tcp_lookup_locked(int ns, uint32_t local_ip, uint16_t local_port,
                                          uint32_t peer_ip, uint16_t peer_port) {
    const uint8_t* s6 = g_in6_src;                /* §M90 3a — an IPv6 segment? */
    const uint8_t* d6 = g_in6_dst;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &g_conns[i];
        if (!c->used || c->listener || c->netns != ns) continue;
        if (c->local_port != local_port || c->peer_port != peer_port) continue;
        if (s6) {
            if (c->fam6 && a6e(c->p6, s6) && (a6e(c->l6, d6) || a6z(c->l6))) return c;
        } else if (!c->fam6 && c->peer_ip == peer_ip &&
                   (c->local_ip == local_ip || c->local_ip == 0)) return c;
    }
    /* listeners: an exact family match first, then a dual-stack v6 one for v4 */
    struct tcp_conn* dual = NULL;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &g_conns[i];
        if (!c->used || !c->listener || c->state != TCP_ST_LISTEN || c->netns != ns) continue;
        if (c->local_port != local_port) continue;
        if (s6) {
            if (c->fam6 && (a6z(c->l6) || a6e(c->l6, d6))) return c;
        } else if (!c->fam6) {
            if (c->local_ip == 0 || c->local_ip == local_ip) return c;   /* 0.0.0.0 */
        } else if (!c->v6only && a6z(c->l6)) dual = c;
    }
    return dual;
}

/* ----------------------- output ------------------------------------------- */

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t* seg, uint32_t len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFF; sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF; sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += ((uint16_t)seg[i] << 8) | seg[i+1];
    if (len & 1) sum += (uint16_t)seg[len-1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* Free space in the receive ring — the number we advertise, and the only thing
 * that stops a fast peer from overrunning a reader that has stopped reading. */
static uint32_t tcp_rx_free(const struct tcp_conn* c) {
    uint32_t used = c->rx_head - c->rx_tail;
    return (used >= TCP_RXBUF) ? 0 : (TCP_RXBUF - used);
}
static uint32_t tcp_rx_avail(const struct tcp_conn* c) { return c->rx_head - c->rx_tail; }

/* Emit one segment.  `seq` is an explicit argument because a RETRANSMISSION
 * carries an old sequence number with a current acknowledgement and a current
 * window: taking all three from the connection would make every retransmit look
 * like new data. */
static int tcp_emit_locked(struct tcp_conn* c, uint32_t seq, uint8_t flags,
                           const uint8_t* data, uint32_t len) {
    static uint8_t seg[ETH_MTU];
    uint32_t total = sizeof(struct tcp_hdr) + len;
    if (!c->dev || total > sizeof(seg)) return -1;

    struct tcp_hdr* th = (struct tcp_hdr*)seg;
    th->src_port = htons(c->local_port);
    th->dst_port = htons(c->peer_port);
    th->seq      = htonl(seq);
    th->ack      = htonl(c->rcv_nxt);
    th->data_off = 5 << 4;
    th->flags    = flags;
    uint32_t win = tcp_rx_free(c);
    if (!win) g_tcp_zerowin++;
    th->window   = htons(win > 65535 ? 65535 : (uint16_t)win);
    th->checksum = 0;
    th->urgent   = 0;
    for (uint32_t i = 0; i < len; i++) seg[sizeof(struct tcp_hdr) + i] = data[i];
    c->tx_segs++;
    if (c->fam6) {                                /* §M90 3a */
        uint16_t cs = net6_csum(c->l6, c->p6, IP_PROTO_TCP, seg, total);
        seg[16] = (uint8_t)(cs >> 8); seg[17] = (uint8_t)cs;
        return net6_emit_locked(c->dev, c->peer_mac, c->l6, c->p6, IP_PROTO_TCP, seg, total);
    }
    th->checksum = tcp_checksum(c->local_ip, c->peer_ip, seg, total);
    return ipv4_emit_locked(c->dev, c->peer_mac, c->local_ip, c->peer_ip,
                            IP_PROTO_TCP, seg, total);
}

/* A bare RST for a segment belonging to no connection.
 *
 * Not politeness: without it, connecting to a port nobody listens on is
 * indistinguishable from connecting to a host that is merely slow, and the
 * caller waits out the whole connect timeout instead of failing immediately.
 * It is also what gives poll/epoll the POLLERR §M56.2 built.  Sent straight
 * back to the MAC the segment arrived from, so it needs no lookup and cannot
 * block. */
static void tcp_send_rst_locked(struct net_device* dev, const uint8_t* via_mac,
                                uint32_t local_ip, uint32_t peer_ip,
                                const struct tcp_hdr* th, uint32_t seglen) {
    if (th->flags & TCP_RST) return;            /* never answer an RST with one */
    uint8_t seg[sizeof(struct tcp_hdr)];
    struct tcp_hdr* r = (struct tcp_hdr*)seg;
    r->src_port = th->dst_port;
    r->dst_port = th->src_port;
    r->data_off = 5 << 4;
    r->window   = 0;
    r->checksum = 0;
    r->urgent   = 0;
    if (th->flags & TCP_ACK) {
        r->seq   = th->ack;                     /* the seq they expect from us  */
        r->ack   = 0;
        r->flags = TCP_RST;
    } else {
        uint32_t hlen = (uint32_t)(th->data_off >> 4) * 4;
        uint32_t dlen = seglen > hlen ? seglen - hlen : 0;
        if (th->flags & TCP_SYN) dlen++;
        r->seq   = 0;
        r->ack   = htonl(ntohl(th->seq) + dlen);
        r->flags = TCP_RST | TCP_ACK;
    }
    g_tcp_rst_sent++;
    if (g_in6_src) {                              /* §M90 3a — answering a v6 segment */
        uint16_t cs = net6_csum(g_in6_dst, g_in6_src, IP_PROTO_TCP, seg, sizeof(seg));
        seg[16] = (uint8_t)(cs >> 8); seg[17] = (uint8_t)cs;
        net6_emit_locked(dev, via_mac, g_in6_dst, g_in6_src, IP_PROTO_TCP, seg, sizeof(seg));
        return;
    }
    r->checksum = tcp_checksum(local_ip, peer_ip, seg, sizeof(seg));
    ipv4_emit_locked(dev, via_mac, local_ip, peer_ip, IP_PROTO_TCP, seg, sizeof(seg));
}

/* Retransmission timing.  A FIXED timeout with exponential backoff rather than
 * an RTT estimator: on the two links this kernel has — a loopback and QEMU's
 * SLIRP — the round trip is microseconds, so a Jacobson estimator would be
 * measuring the emulator's scheduling noise and calling it a network.  The
 * constant is a floor, not a tuning: what matters is that a lost segment is
 * RECOVERED, and recovering it 200 ms late is a latency cost, while never
 * recovering it is data loss. */
#define TCP_RTO_INITIAL_MS   200u
#define TCP_RTO_MAX_MS      3000u
#define TCP_RETRIES_MAX        8u     /* ~8 doublings, then the peer is gone   */
#define TCP_TIME_WAIT_MS    1000u     /* short: this is not an internet router */
/* How long a connection whose owner has closed may keep working.
 *
 * This is NOT the same as TIME_WAIT and the difference cost an afternoon.  A
 * close() with bytes still queued must keep sending them — and send the FIN
 * AFTER them — or the peer's last read waits out its whole timeout for an end
 * of stream that was thrown away.  The symptom is spectacularly misleading: a
 * transfer that delivers every byte correctly and takes 8 seconds instead of
 * 0.4, which reads as "the network is slow" and is actually "the sender gave
 * up on the goodbye". */
#define TCP_ORPHAN_MS      10000u

static void tcp_arm_rto_locked(struct tcp_conn* c) {
    if (!c->rto_ms) c->rto_ms = TCP_RTO_INITIAL_MS;
    c->rto_deadline_ns = timer_now_ns() + (uint64_t)c->rto_ms * 1000000ull;
}

/* Send whatever the peer's window and our queue allow.  Called after anything
 * that can change either: data queued by the sender, an ACK that opened the
 * window, a state change. */
static void tcp_output_locked(struct tcp_conn* c) {
    if (!c->used || !c->dev) return;
    if (c->state != TCP_ST_ESTABLISHED && c->state != TCP_ST_CLOSE_WAIT &&
        c->state != TCP_ST_FIN_WAIT_1  && c->state != TCP_ST_LAST_ACK) return;

    uint32_t mss = c->dev->mtu - sizeof(struct ipv4_hdr) - sizeof(struct tcp_hdr);
    if (c->fam6) mss -= 20;                        /* §M90 3a — the v6 header is 40 */
    if (mss > TCP_MSS_CAP) mss = TCP_MSS_CAP;

    for (;;) {
        uint32_t inflight = c->snd_nxt - c->snd_una;
        if (inflight >= c->tx_len) break;                    /* nothing unsent */
        /* The peer's window bounds what may be OUTSTANDING, not what may be
         * queued — that difference is the entire point of a send buffer. */
        if (c->snd_wnd <= inflight) { g_tcp_wndblock++; break; }  /* no room   */
        uint32_t n = c->tx_len - inflight;
        uint32_t room = c->snd_wnd - inflight;
        if (n > room) n = room;
        if (n > mss)  n = mss;
        if (tcp_emit_locked(c, c->snd_nxt, TCP_PSH | TCP_ACK, &c->tx[inflight], n) != 0)
            break;
        c->snd_nxt += n;
        g_tcp_datasegs++;
    }

    /* Our FIN goes out only once every queued byte has been sent: a FIN that
     * overtakes data is a stream truncated by its own close. */
    if (c->fin_pending && !c->fin_sent && (c->snd_nxt - c->snd_una) >= c->tx_len) {
        if (tcp_emit_locked(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0) == 0) {
            c->fin_sent = 1;
            c->fin_seq  = c->snd_nxt;
            c->snd_nxt += 1;                                 /* FIN takes a seq */
        }
    }
    if (c->snd_nxt != c->snd_una && !c->rto_deadline_ns) tcp_arm_rto_locked(c);
}

/* ----------------------- input -------------------------------------------- */

/* Copy a segment's payload into the receive ring, as much as fits.  Whatever
 * does not fit is simply not acknowledged, so the peer retransmits it — which
 * is exactly what a window is for and why partial acceptance is correct rather
 * than a shortcut. */
static uint32_t tcp_rx_store_locked(struct tcp_conn* c, const uint8_t* d, uint32_t len) {
    uint32_t room = tcp_rx_free(c);
    if (len > room) len = room;
    for (uint32_t i = 0; i < len; i++)
        c->rx[(c->rx_head + i) % TCP_RXBUF] = d[i];
    c->rx_head += len;
    return len;
}

/* A listener's child has reached ESTABLISHED: hand it to whoever is in accept.
 * A full queue DROPS the connection (RST) rather than growing without bound —
 * an unbounded accept queue turns a slow server into an out-of-memory event. */
static void tcp_accept_enqueue_locked(struct tcp_conn* l, struct tcp_conn* c) {
    uint8_t nx    = (uint8_t)((l->aq_head + 1) % TCP_ACCEPTQ);
    uint8_t depth = (uint8_t)((l->aq_head + TCP_ACCEPTQ - l->aq_tail) % TCP_ACCEPTQ);
    if (nx == l->aq_tail || depth >= l->backlog) {   /* backlog full           */
        tcp_emit_locked(c, c->snd_nxt, TCP_RST, NULL, 0);
        tcp_free_locked(c);
        return;
    }
    l->aq[l->aq_head] = c;
    l->aq_head = nx;
}

static void tcp_input(struct net_device* dev, const uint8_t* src_mac,
                      uint32_t src_ip, uint32_t dst_ip,
                      const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr* th = (const struct tcp_hdr*)p;

    uint16_t sport = ntohs(th->src_port), dport = ntohs(th->dst_port);
    uint32_t hlen  = (uint32_t)(th->data_off >> 4) * 4;
    if (hlen < sizeof(struct tcp_hdr) || hlen > len) return;
    const uint8_t* data = p + hlen;
    uint32_t dlen = len - hlen;
    uint32_t seq  = ntohl(th->seq);
    uint32_t ack  = ntohl(th->ack);
    uint8_t  flags = th->flags;

    struct tcp_conn* c = tcp_lookup_locked(dev->netns, dst_ip, dport, src_ip, sport);
    if (!c) {
        /* Nobody is listening.  Answering with an RST is what turns a connect
         * to a closed port into an immediate failure instead of a timeout. */
        g_tcp_dropped++;
        tcp_send_rst_locked(dev, src_mac, dst_ip, src_ip, th, len);
        return;
    }
    c->rx_segs++;

    if (flags & TCP_RST) {
        g_tcp_rst_rcvd++;
        if (c->listener) return;               /* a listener survives an RST   */
        c->reset   = 1;
        c->peer_fin = 1;                        /* every wait must end          */
        c->state   = TCP_ST_CLOSED;
        c->rto_deadline_ns = 0;
        return;
    }

    /* ---- LISTEN: a new connection request ------------------------------- */
    if (c->listener) {
        if (!(flags & TCP_SYN)) {
            tcp_send_rst_locked(dev, src_mac, dst_ip, src_ip, th, len);
            return;
        }
        struct tcp_conn* n = tcp_alloc_locked();
        if (!n) {                               /* table full — refuse loudly  */
            tcp_send_rst_locked(dev, src_mac, dst_ip, src_ip, th, len);
            return;
        }
        n->dev        = dev;
        n->netns      = c->netns;
        n->local_ip   = dst_ip;
        n->local_port = dport;
        n->peer_ip    = src_ip;
        n->peer_port  = sport;
        if (g_in6_src) { n->fam6 = 1; a6c(n->l6, g_in6_dst); a6c(n->p6, g_in6_src); }
        mac_copy(n->peer_mac, src_mac);         /* the correct next hop, free  */
        n->parent     = c;
        n->rcv_nxt    = seq + 1;                /* their SYN consumes a seq    */
        n->snd_una    = n->snd_nxt = (g_tcp_isn += 0x10000);
        n->snd_wnd    = ntohs(th->window);
        n->state      = TCP_ST_SYN_RCVD;
        tcp_emit_locked(n, n->snd_nxt, TCP_SYN | TCP_ACK, NULL, 0);
        n->snd_nxt += 1;                        /* our SYN consumes one too    */
        tcp_arm_rto_locked(n);
        return;
    }

    /* ---- SYN_SENT: the handshake's second segment ------------------------ */
    if (c->state == TCP_ST_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            if (ack != c->snd_nxt) {            /* not for our SYN             */
                tcp_send_rst_locked(dev, src_mac, dst_ip, src_ip, th, len);
                return;
            }
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->snd_wnd = ntohs(th->window);
            c->state   = TCP_ST_ESTABLISHED;
            c->rto_deadline_ns = 0; c->retries = 0; c->rto_ms = 0;
            tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
            tcp_output_locked(c);               /* data queued before connect  */
        }
        return;
    }

    /* ---- everything else needs a plausible acknowledgement --------------- */
    if (flags & TCP_ACK) {
        /* An acknowledgement PAST snd_nxt is not nonsense here: a zero-window
         * probe sends a byte without advancing snd_nxt, so a receiver that had
         * room by the time it arrived legitimately acknowledges one more than
         * we think we sent.  Pulling snd_nxt up to meet it is the honest
         * repair; discarding the ACK would strand the connection on the one
         * path that exists to unstick it. */
        if (seq_gt(ack, c->snd_nxt) && seq_leq(ack, c->snd_nxt + 1)) c->snd_nxt = ack;

        if (seq_gt(ack, c->snd_una) && seq_leq(ack, c->snd_nxt)) {
            /* Bytes acknowledged, clamped to what tx[] actually holds: a SYN
             * and a FIN each occupy a sequence number without occupying a byte
             * of the buffer, so the raw difference over-counts by one at each
             * end of the connection's life. */
            uint32_t acked = ack - c->snd_una;
            if (acked > c->tx_len) acked = c->tx_len;
            for (uint32_t i = 0; i + acked < c->tx_len; i++)
                c->tx[i] = c->tx[i + acked];
            c->tx_len -= acked;
            c->snd_una = ack;
            c->retries = 0;
            c->rto_ms  = 0;
            c->rto_deadline_ns = 0;
            if (c->snd_una != c->snd_nxt) tcp_arm_rto_locked(c);
            tcp_output_locked(c);        /* the window may have just opened   */
        }
        c->snd_wnd = ntohs(th->window);
        /* Not only inside the "new data acknowledged" branch above: a pure
         * WINDOW UPDATE carries the same acknowledgement number and a bigger
         * window, and it is the only thing that can restart a sender the
         * receiver had stopped.  Reacting to it just here was the difference
         * between a 32 KiB transfer and a connection that stopped at 10 KiB
         * (the first `tcploss` run said so before anything else did). */
        tcp_output_locked(c);

        if (c->state == TCP_ST_SYN_RCVD && seq_leq(c->snd_nxt, ack)) {
            c->state = TCP_ST_ESTABLISHED;
            c->rto_deadline_ns = 0;
            if (c->parent) tcp_accept_enqueue_locked(c->parent, c);
        }
        /* "Have they acknowledged my FIN?" is a question about the FIN's own
         * sequence number, and nothing else.  Asking it of snd_nxt was wrong
         * in exactly one situation and wrong badly: after a retransmission
         * rolled snd_nxt back to snd_una, an ACK covering only the DATA
         * satisfied the test, the sender moved to FIN_WAIT_2 believing the
         * conversation was over, and never sent the FIN again — while the
         * peer sat in ESTABLISHED waiting for an end of stream that had been
         * declared delivered.  Every byte arrived; the reader still blocked
         * until its timeout, which reads as a slow network. */
        if (c->state == TCP_ST_FIN_WAIT_1 && c->fin_sent && seq_gt(ack, c->fin_seq))
            c->state = TCP_ST_FIN_WAIT_2;
        if (c->state == TCP_ST_LAST_ACK && c->fin_sent && seq_gt(ack, c->fin_seq)) {
            c->state = TCP_ST_CLOSED;
            c->rto_deadline_ns = 0;
            return;
        }
    }

    /* ---- data ------------------------------------------------------------ */
    if (dlen) {
        if (seq == c->rcv_nxt) {
            uint32_t took = tcp_rx_store_locked(c, data, dlen);
            c->rcv_nxt += took;
            tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
        } else {
            /* Out of order or already seen.  We keep no reassembly queue — a
             * duplicate ACK naming what we DO have is both the correct answer
             * and the signal that makes the peer resend it.  Holding
             * out-of-order segments would buy throughput on a lossy link and
             * cost a second buffer with its own overlap arithmetic; the
             * decision is recorded here rather than left to be inferred. */
            tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
        }
    }

    /* ---- their FIN ------------------------------------------------------- */
    if (flags & TCP_FIN) {
        /* The FIN occupies the sequence right after the segment's data, so it
         * is only ours to take once that data has been accepted. */
        if (seq + dlen == c->rcv_nxt) {
            c->rcv_nxt += 1;
            c->peer_fin = 1;
            tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
            switch (c->state) {
                case TCP_ST_ESTABLISHED: c->state = TCP_ST_CLOSE_WAIT; break;
                case TCP_ST_FIN_WAIT_1:  c->state = TCP_ST_TIME_WAIT;
                                         c->linger_deadline_ns = timer_now_ns() +
                                             (uint64_t)TCP_TIME_WAIT_MS * 1000000ull;
                                         break;
                case TCP_ST_FIN_WAIT_2:  c->state = TCP_ST_TIME_WAIT;
                                         c->linger_deadline_ns = timer_now_ns() +
                                             (uint64_t)TCP_TIME_WAIT_MS * 1000000ull;
                                         break;
                default: break;
            }
        } else {
            tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
        }
    }
}

/* ----------------------- the sweeper (retransmit + reclaim) --------------- */

/* One timer for every connection, rather than a timer per connection.
 *
 * A per-connection ktimer would be the obvious design and it is the one §M53
 * warned about: an embedded timer must be cancelled at exactly the right point
 * in teardown, and getting that wrong fires a callback into freed memory.  With
 * a single sweep the connection table is walked under the lock that already
 * protects it, and a connection that went away is simply not there. */
#define TCP_SWEEP_MS  50u

static struct ktimer g_tcp_sweeper;
static volatile int  g_tcp_sweeper_on;

static void tcp_sweep_locked(void) {
    uint64_t now = timer_now_ns();
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_conn* c = &g_conns[i];
        if (!c->used || c->listener) continue;

        /* Retransmit: everything from snd_una, one MSS worth, and let the
         * ACK-driven path take it from there. */
        if (c->rto_deadline_ns && now >= c->rto_deadline_ns) {
            if (++c->retries > TCP_RETRIES_MAX) {
                c->reset = 1;                    /* the peer is unreachable    */
                c->peer_fin = 1;
                c->state = TCP_ST_CLOSED;
                c->rto_deadline_ns = 0;
                continue;
            }
            c->retrans++;
            g_tcp_rtos++;
            uint32_t mss = c->dev ? c->dev->mtu - sizeof(struct ipv4_hdr)
                                                - sizeof(struct tcp_hdr) : TCP_MSS_CAP;
            if (mss > TCP_MSS_CAP) mss = TCP_MSS_CAP;
            if (c->fam6) mss -= 20;                    /* §M90 3a */

            if (c->state == TCP_ST_SYN_SENT) {
                tcp_emit_locked(c, c->snd_una, TCP_SYN, NULL, 0);
            } else if (c->state == TCP_ST_SYN_RCVD) {
                tcp_emit_locked(c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0);
            } else if (c->tx_len) {
                uint32_t n = c->tx_len < mss ? c->tx_len : mss;
                /* With the window shut this retransmission IS the persist
                 * probe, and one byte is all the receiver can be asked to
                 * take: sending a full segment it must discard turns every
                 * timeout into wasted bandwidth on the link that is already
                 * struggling. */
                if (c->snd_wnd == 0) n = 1;
                tcp_emit_locked(c, c->snd_una, TCP_PSH | TCP_ACK, &c->tx[0], n);
                /* Everything past what we just re-sent is unsent again: this is
                 * the "go back to snd_una" of Go-Back-N.  Without it the gap
                 * stays a gap and the receiver drops everything after it. */
                if (c->snd_nxt - c->snd_una > n) c->snd_nxt = c->snd_una + n;
                /* ...and the FIN goes back with it.  It sits immediately after
                 * the queued data, so rewinding the data un-sends it too; a
                 * FIN left marked sent is a FIN never sent again. */
                c->fin_sent = 0;
            } else if (c->fin_sent) {
                tcp_emit_locked(c, c->fin_seq, TCP_FIN | TCP_ACK, NULL, 0);
            }
            /* Exponential backoff — a link that is dropping does not improve
             * by being asked more often. */
            c->rto_ms = c->rto_ms ? c->rto_ms * 2 : TCP_RTO_INITIAL_MS;
            if (c->rto_ms > TCP_RTO_MAX_MS) c->rto_ms = TCP_RTO_MAX_MS;
            c->rto_deadline_ns = now + (uint64_t)c->rto_ms * 1000000ull;
        }

        /* Zero-window persist.  When the peer advertises no room, nothing is
         * outstanding, so the retransmit timer above is not armed — and the
         * window update that would restart us is an ACK the peer may never
         * send, or may send and lose.  Then both sides wait forever, each
         * correctly.  One byte, periodically, is what breaks that. */
        if (c->tx_len && c->snd_nxt == c->snd_una && c->snd_wnd == 0 &&
            !c->rto_deadline_ns &&
            (c->state == TCP_ST_ESTABLISHED || c->state == TCP_ST_CLOSE_WAIT)) {
            /* A window probe asks one question — "how much room have you now?"
             * — and the answer rides on the ACK it provokes.  snd_nxt is NOT
             * advanced, and that is the whole subtlety:
             *
             *   - Advance it, and when the receiver (which by definition has
             *     no room) DROPS the probe byte, the sender's next segment
             *     starts one byte past what the receiver expects.  That hole
             *     is repaired only by the retransmit timer, so EVERY window
             *     reopen costs a full RTO.  Measured: a 32 KiB transfer to a
             *     slow reader took 8.9 s instead of 0.4 s, with a zero-loss
             *     link — the timer doing exactly its job, for a hole the
             *     sender had dug itself.
             *   - Leave it, and if the receiver happens to have room by then
             *     it ACCEPTS the byte and acknowledges past snd_nxt — which
             *     the ACK path handles explicitly (see there).
             *
             * The probe is re-sent by this same branch while the window stays
             * shut, so it needs no timer of its own. */
            (void)0;
        }

        /* Reclaim, in the one order that does not throw data away:
         *
         *   1. a TIME_WAIT that has served its purpose,
         *   2. an orphan that has actually FINISHED (state CLOSED),
         *   3. an orphan that has run out of patience.
         *
         * Rule 3 is a hard cap, not the normal exit.  An orphan with queued
         * bytes is still WORKING — its ACKs, retransmissions and FIN all flow
         * from this same sweep — and reclaiming it early is indistinguishable
         * from the peer vanishing. */
        if (c->state == TCP_ST_TIME_WAIT && c->linger_deadline_ns &&
            now >= c->linger_deadline_ns) {
            if (c->orphan) { tcp_free_locked(c); continue; }
            c->state = TCP_ST_CLOSED;
        }
        if (c->orphan) {
            if (c->state == TCP_ST_CLOSED) { tcp_free_locked(c); continue; }
            if (c->linger_deadline_ns && now >= c->linger_deadline_ns) {
                /* Out of time with work outstanding: say so rather than
                 * disappearing quietly, because "the peer stopped
                 * acknowledging" is a fact about the network and a silent
                 * reclaim is a fact about nothing. */
                if (c->tx_len)
                    kprintf("tcp: abandoning %u unsent byte(s) to %u.%u.%u.%u:%u\n",
                            c->tx_len, (c->peer_ip>>24)&0xFF, (c->peer_ip>>16)&0xFF,
                            (c->peer_ip>>8)&0xFF, c->peer_ip&0xFF, c->peer_port);
                tcp_free_locked(c);
                continue;
            }
        }
    }
}

static void tcp_sweep_fired(struct ktimer* t) {
    (void)t;
    uint32_t f = net_lock();
    tcp_sweep_locked();
    /* Re-arm from inside the callback: ktimer is one-shot, and a sweep that
     * stops when a connection is quiet is a sweep that never resumes. */
    if (g_tcp_sweeper_on) {
        ktimer_arm(&g_tcp_sweeper, timer_now_ns() + (uint64_t)TCP_SWEEP_MS * 1000000ull,
                   tcp_sweep_fired, NULL);
    }
    waitq_wake_all(&g_netwq);       /* a retransmit may have unblocked a waiter */
    net_unlock(f);
}

/* Started on the first connection and never stopped.  A box that never opens a
 * socket never arms it — same rule as netd (§M55): an idle machine must not pay
 * for a subsystem it is not using. */
static void tcp_sweeper_ensure(void) {
    if (__atomic_exchange_n(&g_tcp_sweeper_on, 1, __ATOMIC_ACQ_REL)) return;
    ktimer_arm(&g_tcp_sweeper, timer_now_ns() + (uint64_t)TCP_SWEEP_MS * 1000000ull,
               tcp_sweep_fired, NULL);
}

/* ----------------------- waits -------------------------------------------- */

#define TCP_CONNECT_MS   5000
#define TCP_RECV_MS     10000
#define TCP_SEND_MS      5000
#define TCP_FINWAIT_MS   1000

struct tcp_wait { struct tcp_conn* c; };

static int tcp_estab_cond(void* a) {
    struct tcp_conn* c = ((struct tcp_wait*)a)->c;
    return c->state == TCP_ST_ESTABLISHED || c->state == TCP_ST_CLOSED || c->reset;
}
static int tcp_readable_cond(void* a) {
    struct tcp_conn* c = ((struct tcp_wait*)a)->c;
    return tcp_rx_avail(c) > 0 || c->peer_fin || c->reset;
}
static int tcp_writable_cond(void* a) {
    struct tcp_conn* c = ((struct tcp_wait*)a)->c;
    return c->tx_len < TCP_TXBUF || c->reset || c->state == TCP_ST_CLOSED;
}
static int tcp_drained_cond(void* a) {
    struct tcp_conn* c = ((struct tcp_wait*)a)->c;
    return c->tx_len == 0 || c->reset || c->state == TCP_ST_CLOSED;
}
static int tcp_accept_cond(void* a) {
    struct tcp_conn* l = ((struct tcp_wait*)a)->c;
    return l->aq_head != l->aq_tail || !l->used;
}

/* ----------------------- public API --------------------------------------- */

struct tcp_conn* net_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    return net_tcp_connect_ns(net_cur_ns(), ip, port, timeout_ms);
}
struct tcp_conn* net_tcp_connect_ns(int ns, uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    struct net_device* dev = net_route_ns(ns, ip, NULL);
    if (!dev) return NULL;

    /* Resolve the next hop FIRST and unlocked — it is the one step that can
     * block, and doing it here is what lets every later segment, including the
     * ACKs generated on the RX path, be a pure send (§M55). */
    uint8_t mac[ETH_ALEN];
    if (route_mac(dev, ip, mac) != 0) return NULL;

    tcp_sweeper_ensure();

    uint32_t f = net_lock();
    struct tcp_conn* c = tcp_alloc_locked();
    if (!c) { net_unlock(f); return NULL; }
    c->dev        = dev;
    c->netns      = ns;
    /* To one of our OWN addresses: the connection is that address's (a
     * packet from us to 10.0.2.15 is delivered by the loopback, but it must
     * carry 10.0.2.15 on both ends to find its listener). */
    c->local_ip   = (dev->flags & NETDEV_F_LOOPBACK) && (ip >> 24) != 127 ? ip : dev->ip;
    c->local_port = tcp_ephem_port_locked(ns);
    c->peer_ip    = ip;
    c->peer_port  = port;
    mac_copy(c->peer_mac, mac);
    c->snd_una = c->snd_nxt = (g_tcp_isn += 0x10000);
    c->snd_wnd = 1460;                    /* until they tell us otherwise      */
    c->state   = TCP_ST_SYN_SENT;
    tcp_emit_locked(c, c->snd_nxt, TCP_SYN, NULL, 0);
    c->snd_nxt += 1;
    tcp_arm_rto_locked(c);
    net_unlock(f);

    struct tcp_wait w = { c };
    net_wait_cond(tcp_estab_cond, &w, timeout_ms ? timeout_ms : TCP_CONNECT_MS);

    f = net_lock();
    int ok = (c->state == TCP_ST_ESTABLISHED);
    if (!ok) { tcp_free_locked(c); c = NULL; }
    net_unlock(f);
    return c;
}

struct tcp_conn* net_tcp_listen(uint32_t local_ip, uint16_t port, int backlog) {
    return net_tcp_listen_ns(net_cur_ns(), local_ip, port, backlog);
}
struct tcp_conn* net_tcp_listen_ns(int ns, uint32_t local_ip, uint16_t port, int backlog) {
    tcp_sweeper_ensure();
    uint32_t f = net_lock();
    /* §M90 — port 0 (or a listen with no bind) asks for an EPHEMERAL port, as
     * on Linux: containerd's CRI streaming server listens on 127.0.0.1:0 and
     * died "listen: invalid argument" — unseen until the personality's
     * version let that plugin load.  The caller reads the port back. */
    if (!port) port = tcp_ephem_port_locked(ns);
    if (!port || tcp_port_clash_locked(ns, port, 1)) { net_unlock(f); return NULL; }
    struct tcp_conn* l = tcp_alloc_locked();
    if (!l) { net_unlock(f); return NULL; }
    l->netns      = ns;
    l->listener   = 1;
    l->state      = TCP_ST_LISTEN;
    l->local_ip   = local_ip;              /* 0 = every local address          */
    l->local_port = port;
    l->backlog    = (uint8_t)(backlog <= 0 || backlog > TCP_ACCEPTQ - 1
                              ? TCP_ACCEPTQ - 1 : backlog);
    /* A listener has no device of its own: the child takes the device the SYN
     * arrived on, which is how one listener serves every interface. */
    net_unlock(f);
    return l;
}

struct tcp_conn* net_tcp_accept(struct tcp_conn* l, uint32_t timeout_ms) {
    if (!l || !l->listener) return NULL;
    struct tcp_wait w = { l };
    if (l->aq_head == l->aq_tail && timeout_ms)
        net_wait_cond(tcp_accept_cond, &w, timeout_ms);

    uint32_t f = net_lock();
    struct tcp_conn* c = NULL;
    if (l->used && l->aq_head != l->aq_tail) {
        c = l->aq[l->aq_tail];
        l->aq_tail = (uint8_t)((l->aq_tail + 1) % TCP_ACCEPTQ);
        if (c) { c->parent = NULL; }        /* it belongs to its acceptor now  */
    }
    net_unlock(f);
    return c;
}

int net_tcp_send(struct tcp_conn* c, const void* buf, uint32_t len, int nonblock) {
    if (!c || !c->used) return -1;
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t f = net_lock();
    if (c->reset || c->state == TCP_ST_CLOSED) { net_unlock(f); return -1; }
    if (c->state == TCP_ST_FIN_WAIT_1 || c->state == TCP_ST_FIN_WAIT_2 ||
        c->state == TCP_ST_LAST_ACK   || c->fin_pending) { net_unlock(f); return -1; }
    uint32_t room = TCP_TXBUF - c->tx_len;
    net_unlock(f);

    if (!room) {
        /* A blocking send waits for the peer to acknowledge something; a
         * non-blocking one says EAGAIN, which is the answer an event loop
         * needs in order to go back to its poll rather than spin. */
        if (nonblock) return NET_EAGAIN;
        struct tcp_wait w = { c };
        if (!net_wait_cond(tcp_writable_cond, &w, TCP_SEND_MS)) return NET_EAGAIN;
    }

    f = net_lock();
    if (c->reset || c->state == TCP_ST_CLOSED) { net_unlock(f); return -1; }
    room = TCP_TXBUF - c->tx_len;
    uint32_t n = len < room ? len : room;
    for (uint32_t i = 0; i < n; i++) c->tx[c->tx_len + i] = p[i];
    c->tx_len += n;
    tcp_output_locked(c);
    net_unlock(f);
    return (int)n;
}

/* Returns >0 bytes, 0 for a clean end of stream, NET_EAGAIN when nothing
 * arrived in time, -1 on a broken connection.
 *
 * THE TIMEOUT IS NOT AN END OF FILE.  The previous version returned 0 both when
 * the peer had closed and when ten seconds passed with nothing on the wire, so
 * a slow server was indistinguishable from a finished one and every drain loop
 * stopped early on a busy day. */
int net_tcp_recv(struct tcp_conn* c, void* buf, uint32_t len, int nonblock,
                 uint32_t timeout_ms) {
    if (!c || !c->used) return -1;
    struct tcp_wait w = { c };

    uint32_t f = net_lock();
    uint32_t avail = tcp_rx_avail(c);
    net_unlock(f);

    if (!avail) {
        if (nonblock) {
            uint32_t g = net_lock();
            int fin = c->peer_fin, rst = c->reset;
            net_unlock(g);
            if (rst) return -1;
            if (fin) return 0;
            return NET_EAGAIN;
        }
        net_wait_cond(tcp_readable_cond, &w, timeout_ms ? timeout_ms : TCP_RECV_MS);
    }

    f = net_lock();
    avail = tcp_rx_avail(c);
    uint32_t n = avail < len ? avail : len;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < n; i++) out[i] = c->rx[(c->rx_tail + i) % TCP_RXBUF];
    c->rx_tail += n;
    int fin = c->peer_fin, rst = c->reset;
    /* Draining the ring re-opens the window.  Telling the peer costs one
     * segment and is what un-stalls a sender that filled it — a window update
     * nobody sends is a connection that stops for good. */
    if (n && (c->state == TCP_ST_ESTABLISHED || c->state == TCP_ST_CLOSE_WAIT))
        tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
    net_unlock(f);

    if (n) return (int)n;
    if (rst) return -1;
    if (fin) return 0;                       /* a real, drained end of stream  */
    return NET_EAGAIN;                       /* nothing arrived in time        */
}

/* Readiness for poll/epoll, sampled in ONE locked read — a caller that read
 * these separately could see "no data" and "no FIN" from two different instants
 * and conclude the connection was merely quiet (§M56). */
void net_tcp_state(struct tcp_conn* c, int* readable, int* writable,
                   int* peer_fin, int* reset, int* connected) {
    if (!c || !c->used) {
        if (readable)  *readable  = 0;
        if (writable)  *writable  = 0;
        if (peer_fin)  *peer_fin  = 1;
        if (reset)     *reset     = 1;
        if (connected) *connected = 0;
        return;
    }
    uint32_t f = net_lock();
    if (readable)  *readable  = tcp_rx_avail(c) > 0;
    if (writable)  *writable  = (c->state == TCP_ST_ESTABLISHED ||
                                 c->state == TCP_ST_CLOSE_WAIT) && c->tx_len < TCP_TXBUF;
    if (peer_fin)  *peer_fin  = c->peer_fin;
    if (reset)     *reset     = c->reset;
    if (connected) *connected = (c->state == TCP_ST_ESTABLISHED ||
                                 c->state == TCP_ST_CLOSE_WAIT);
    net_unlock(f);
}

int net_tcp_pending(struct tcp_conn* l) {
    if (!l || !l->listener || !l->used) return 0;
    uint32_t f = net_lock();
    int n = (int)((l->aq_head + TCP_ACCEPTQ - l->aq_tail) % TCP_ACCEPTQ);
    net_unlock(f);
    return n;
}

void net_tcp_peer(struct tcp_conn* c, uint32_t* ip, uint16_t* port) {
    if (!c || !c->used) { if (ip) *ip = 0; if (port) *port = 0; return; }
    uint32_t f = net_lock();
    if (ip)   *ip   = c->peer_ip;
    if (port) *port = c->peer_port;
    net_unlock(f);
}

/* ---- §M90 3a stage c — the IPv6 side of the transport --------------------- */

void net_l4_input6(struct net_device* dev, const uint8_t* src_mac, const uint8_t* src,
                   const uint8_t* dst, uint8_t nh, const uint8_t* l4, uint32_t len) {
    /* IPv6 has no header checksum, so the upper layer's is mandatory — a UDP
     * checksum of 0 is invalid over v6 (RFC 8200 §8.1). */
    if (net6_csum(src, dst, nh, l4, len) != 0) { g_tcp_dropped++; return; }
    g_in6_src = src; g_in6_dst = dst;
    if (nh == IP_PROTO_TCP) tcp_input(dev, src_mac, 0, 0, l4, len);
    else if (nh == IP_PROTO_UDP) udp_input(dev, 0, l4, len);
    g_in6_src = g_in6_dst = NULL;
}

struct tcp_conn* net_tcp_connect6_ns(int ns, struct net_device* scope, const uint8_t dst[16],
                                     uint16_t port, uint32_t timeout_ms) {
    struct net_device* dev = NULL;
    uint8_t mac[ETH_ALEN], src[16];
    if (net6_prepare(ns, scope, dst, &dev, mac, src) != 0) return NULL;   /* blocks: unlocked */
    tcp_sweeper_ensure();
    uint32_t f = net_lock();
    struct tcp_conn* c = tcp_alloc_locked();
    if (!c) { net_unlock(f); return NULL; }
    c->dev = dev; c->netns = ns; c->fam6 = 1;
    a6c(c->l6, src); a6c(c->p6, dst);
    c->local_port = tcp_ephem_port_locked(ns);
    c->peer_port  = port;
    mac_copy(c->peer_mac, mac);
    c->snd_una = c->snd_nxt = (g_tcp_isn += 0x10000);
    c->snd_wnd = 1440;
    c->state   = TCP_ST_SYN_SENT;
    tcp_emit_locked(c, c->snd_nxt, TCP_SYN, NULL, 0);
    c->snd_nxt += 1;
    tcp_arm_rto_locked(c);
    net_unlock(f);
    struct tcp_wait w = { c };
    net_wait_cond(tcp_estab_cond, &w, timeout_ms ? timeout_ms : TCP_CONNECT_MS);
    f = net_lock();
    int ok = (c->state == TCP_ST_ESTABLISHED);
    if (!ok) { tcp_free_locked(c); c = NULL; }
    net_unlock(f);
    return c;
}

struct tcp_conn* net_tcp_listen6_ns(int ns, const uint8_t local[16], uint16_t port,
                                    int backlog, int v6only) {
    tcp_sweeper_ensure();
    uint32_t f = net_lock();
    if (!port) port = tcp_ephem_port_locked(ns);
    int mask = v6only || (local && !a6z(local)) ? 2 : 3;
    if (!port || tcp_port_clash_locked(ns, port, mask)) { net_unlock(f); return NULL; }
    struct tcp_conn* l = tcp_alloc_locked();
    if (!l) { net_unlock(f); return NULL; }
    l->netns = ns; l->listener = 1; l->state = TCP_ST_LISTEN;
    l->fam6 = 1; l->v6only = (uint8_t)(mask == 2);
    if (local) a6c(l->l6, local);
    l->local_port = port;
    l->backlog = (uint8_t)(backlog <= 0 || backlog > TCP_ACCEPTQ - 1 ? TCP_ACCEPTQ - 1 : backlog);
    net_unlock(f);
    return l;
}

int net_tcp_peer6(struct tcp_conn* c, uint8_t a6[16], uint16_t* port) {
    if (!c || !c->used) return -1;
    uint32_t f = net_lock();
    int is6 = c->fam6;
    if (is6 && a6) a6c(a6, c->p6);
    if (port) *port = c->peer_port;
    net_unlock(f);
    return is6;
}
int net_tcp_local6(struct tcp_conn* c, uint8_t a6[16], uint16_t* port) {
    if (!c || !c->used) return -1;
    uint32_t f = net_lock();
    int is6 = c->fam6;
    if (is6 && a6) a6c(a6, c->l6);
    if (port) *port = c->local_port;
    net_unlock(f);
    return is6;
}

int net_udp_send6_ns(int ns, struct net_device* scope, const uint8_t dst[16],
                     uint16_t sport, uint16_t dport, const void* data, uint32_t len) {
    static uint8_t buf[ETH_MTU];
    uint32_t total = sizeof(struct udp_hdr) + len;
    if (total + 40 > ETH_MTU) return -90;                         /* EMSGSIZE */
    struct net_device* dev = NULL;
    uint8_t mac[ETH_ALEN], src[16];
    int pr = net6_prepare(ns, scope, dst, &dev, mac, src);
    if (pr) return pr == -1 ? -101 : -113;                        /* ENETUNREACH / EHOSTUNREACH */
    uint32_t f = net_lock();
    struct udp_hdr* uh = (struct udp_hdr*)buf;
    uh->src_port = htons(sport);
    uh->dst_port = htons(dport);
    uh->len      = htons((uint16_t)total);
    uh->checksum = 0;
    const uint8_t* p = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) buf[sizeof(struct udp_hdr) + i] = p[i];
    uint16_t cs = net6_csum(src, dst, IP_PROTO_UDP, buf, total);
    if (!cs) cs = 0xffff;                                        /* 0 means "none": not allowed */
    buf[6] = (uint8_t)(cs >> 8); buf[7] = (uint8_t)cs;
    int rc = net6_emit_locked(dev, mac, src, dst, IP_PROTO_UDP, buf, total);
    net_unlock(f);
    return rc;
}

void net_tcp_local(struct tcp_conn* c, uint32_t* ip, uint16_t* port) {
    if (!c || !c->used) { if (ip) *ip = 0; if (port) *port = 0; return; }
    uint32_t f = net_lock();
    if (ip)   *ip   = c->local_ip;
    if (port) *port = c->local_port;
    net_unlock(f);
}

/* Half-close: send our FIN and keep the connection.  This is what
 * shutdown(fd, SHUT_WR) means and it is not the same as close: the peer sees
 * end-of-stream and may keep answering, which is exactly how "send the
 * request, then tell them you are done, then read the reply" works. */
void net_tcp_shutdown(struct tcp_conn* c) {
    if (!c || !c->used || c->listener) return;
    uint32_t f = net_lock();
    if (c->state == TCP_ST_ESTABLISHED || c->state == TCP_ST_CLOSE_WAIT) {
        c->fin_pending = 1;
        c->state = (c->state == TCP_ST_CLOSE_WAIT) ? TCP_ST_LAST_ACK
                                                   : TCP_ST_FIN_WAIT_1;
        tcp_output_locked(c);
    }
    net_unlock(f);
}

void net_tcp_close(struct tcp_conn* c) {
    if (!c || !c->used) return;

    uint32_t f = net_lock();
    if (c->listener) {
        /* Everything the listener accepted but nobody took is its
         * responsibility: refuse those peers rather than leaving them
         * connected to a socket that no longer exists. */
        while (c->aq_head != c->aq_tail) {
            struct tcp_conn* p = c->aq[c->aq_tail];
            c->aq_tail = (uint8_t)((c->aq_tail + 1) % TCP_ACCEPTQ);
            if (p && p->used) {
                tcp_emit_locked(p, p->snd_nxt, TCP_RST, NULL, 0);
                tcp_free_locked(p);
            }
        }
        tcp_free_locked(c);
        net_unlock(f);
        return;
    }

    int need_fin = (c->state == TCP_ST_ESTABLISHED || c->state == TCP_ST_CLOSE_WAIT);
    if (need_fin) {
        c->fin_pending = 1;
        c->state = (c->state == TCP_ST_CLOSE_WAIT) ? TCP_ST_LAST_ACK
                                                   : TCP_ST_FIN_WAIT_1;
        tcp_output_locked(c);
    }
    net_unlock(f);

    if (need_fin) {
        /* Give the peer a moment to acknowledge — but do not BLOCK a close on
         * a peer that has gone away.  Whatever is unfinished after this is the
         * sweeper's problem, which is why the connection is marked an orphan
         * rather than freed here. */
        struct tcp_wait w = { c };
        net_wait_cond(tcp_drained_cond, &w, TCP_FINWAIT_MS);
    }

    f = net_lock();
    if (c->state == TCP_ST_CLOSED || c->reset) {
        tcp_free_locked(c);
    } else {
        c->orphan = 1;
        c->linger_deadline_ns = timer_now_ns() +
                                (uint64_t)TCP_ORPHAN_MS * 1000000ull;
    }
    net_unlock(f);
}

/* Diagnostics for `netstat` and /proc/net/tcp. */
void net_tcp_foreach(net_tcp_iter_fn fn, void* ctx) {
    if (!fn) return;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        uint32_t f = net_lock();
        struct tcp_conn* c = &g_conns[i];
        struct net_tcp_info info;
        int used = c->used;
        if (used) {
            info.state      = TCP_STATE_NAMES[c->state < 10 ? c->state : 0];
            info.local_ip   = c->local_ip;   info.local_port = c->local_port;
            info.peer_ip    = c->peer_ip;    info.peer_port  = c->peer_port;
            info.rx_queued  = tcp_rx_avail(c);
            info.tx_queued  = c->tx_len;
            info.tx_segs    = c->tx_segs;
            info.rx_segs    = c->rx_segs;
            info.retrans    = c->retrans;
            info.listener   = c->listener;
            info.dev        = c->dev ? c->dev->name : "-";
        }
        net_unlock(f);
        if (used) fn(&info, ctx);
    }
}

int net_tcp_capacity(void) { return TCP_MAX_CONNS; }

void net_tcp_counters(uint32_t* rst_sent, uint32_t* rst_rcvd, uint32_t* noconn) {
    if (rst_sent) *rst_sent = g_tcp_rst_sent;
    if (rst_rcvd) *rst_rcvd = g_tcp_rst_rcvd;
    if (noconn)   *noconn   = g_tcp_dropped;
}

void net_tcp_timing_counters(uint32_t* persists, uint32_t* rtos, uint32_t* zerowin) {
    if (persists) *persists = g_tcp_persists;
    if (rtos)     *rtos     = g_tcp_rtos;
    if (zerowin)  *zerowin  = g_tcp_zerowin;
}

void net_tcp_output_counters(uint32_t* datasegs, uint32_t* wndblocked) {
    if (datasegs)   *datasegs   = g_tcp_datasegs;
    if (wndblocked) *wndblocked = g_tcp_wndblock;
}

/* ----------------------- HTTP helper (the `wget` engine) ------------------ */

/* Its own accumulation buffer, no longer the connection's receive ring.  The
 * two were the same object in the first slice, which is why a response larger
 * than the ring could not be received at all: the reader never drained it,
 * so the window it advertised went to zero and stayed there. */
#define HTTP_BODY_CAP 65536
static uint8_t  g_http_body[HTTP_BODY_CAP];
static uint32_t g_http_body_len;

int net_http_get(struct net_device* dev, uint32_t ip, uint16_t port,
                 const char* host, const char* path) {
    (void)dev;                                   /* the route decides now      */
    struct tcp_conn* c = net_tcp_connect(ip, port, TCP_CONNECT_MS);
    if (!c) {
        kprintf("http: connect to %u.%u.%u.%u:%u failed\n",
                (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF, port);
        return -1;
    }

    char req[512]; int n = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int pi = 0; pi < 5; pi++)
        for (const char* q = parts[pi]; *q && n < (int)sizeof(req) - 1; q++) req[n++] = *q;
    net_tcp_send(c, req, (uint32_t)n, 0);

    g_http_body_len = 0;
    for (;;) {
        uint8_t chunk[1024];
        int r = net_tcp_recv(c, chunk, sizeof chunk, 0, 2000);
        if (r == 0) break;                       /* peer closed — the response */
        if (r < 0) break;                        /* error or nothing in 2 s    */
        for (int i = 0; i < r && g_http_body_len < HTTP_BODY_CAP; i++)
            g_http_body[g_http_body_len++] = chunk[i];
    }
    net_tcp_close(c);
    return (int)g_http_body_len;
}

const uint8_t* net_http_body(uint32_t* len_out) {
    if (len_out) *len_out = g_http_body_len;
    return g_http_body;
}


/* ----------------------- IPv4 --------------------------------------------- */

static int ipv4_emit_locked(struct net_device* dev, const uint8_t* via_mac,
                            uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
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
    ih->src       = htonl(src_ip);
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
    int rc = ipv4_emit_locked(dev, mac, dev->ip, dst_ip, proto, payload, len);
    net_unlock(f);
    return rc;
}

/* ---- §M90 rung 5d — FORWARDING and MASQUERADE ---------------------------------
 *
 * A packet that arrives for an address that is not the namespace's own is
 * FORWARDED when the namespace forwards (/proc/sys/net/ipv4/ip_forward, per
 * namespace, off by default as on Linux — Docker turns it on): routed in the
 * same namespace, TTL decremented, out of the route's device.  The next hop's
 * MAC must already be known: the RX path cannot wait, so a miss sends an ARP
 * request and HOLDS the packet (ARP_HOLD_SLOTS, ARP_HOLD_MS) until the reply
 * arrives, as Linux queues it — dropping it made every first SYN through the
 * host cost a full retransmit timeout.
 *
 * MASQUERADE.  Docker's default network relies on an iptables rule —
 * MASQUERADE for its subnet leaving through anything but docker0 — and this
 * kernel has neither netfilter nor an iptables binary (dockerd runs with
 * --iptables=false).  So the equivalent is a KERNEL POLICY, named and
 * switchable: `net.masquerade` (default 1) rewrites the source of a forwarded
 * TCP, UDP or ICMP-echo packet that LEAVES THE MACHINE (a hardware device) from
 * outside that device's subnet to the device's own address, with a port (or
 * echo id) of its own; the reply is matched and rewritten back before local
 * delivery.  Entries idle out (TCP 300 s, UDP 60 s, ICMP 30 s).  Other
 * protocols leave unmodified.  Without it a container could reach the host
 * and its siblings but nothing beyond. */

static uint8_t  g_fwd_on[NET_NS_MAX];
static uint32_t g_fwd_count, g_fwd_noarp, g_fwd_noroute, g_nat_out, g_nat_in, g_nat_full;
static uint8_t  g_fwdbuf[ETH_MTU];

int  net_ip_forward(int ns) { return ns >= 0 && ns < NET_NS_MAX ? g_fwd_on[ns] : 0; }
void net_set_ip_forward(int ns, int on) { if (ns >= 0 && ns < NET_NS_MAX) g_fwd_on[ns] = on ? 1 : 0; }

#define NAT_MAX 256
#define NAT_PORT_LO 20000u
#define NAT_PORT_N  10000u
struct nat_ent {
    uint8_t  used, proto;
    int      ns;
    uint32_t in_ip;   uint16_t in_port;      /* the inside end (a container)     */
    uint32_t peer_ip; uint16_t peer_port;    /* the outside end                  */
    uint32_t out_ip;  uint16_t ext_port;     /* what the outside sees            */
    uint64_t last_ms;
};
static struct nat_ent g_nat[NAT_MAX];
static uint16_t g_nat_next;

static uint32_t nat_idle_ms(uint8_t proto) {
    return proto == IP_PROTO_TCP ? 300000u : proto == IP_PROTO_UDP ? 60000u : 30000u;
}

/* The checksum of a TCP/UDP segment with its pseudo-header. */
static uint16_t l4_checksum(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* seg, uint32_t len) {
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += proto;
    sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += ((uint16_t)seg[i] << 8) | seg[i + 1];
    if (len & 1) sum += (uint16_t)seg[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

/* Where the "port" of a packet lives: TCP/UDP source/destination port, the
 * ICMP echo identifier (for both directions).  NULL = not translatable. */
static uint8_t* nat_port_ptr(uint8_t* l4, uint32_t l4len, uint8_t proto, int dst_side) {
    if (proto == IP_PROTO_TCP || proto == IP_PROTO_UDP) {
        if (l4len < 8) return NULL;
        return l4 + (dst_side ? 2 : 0);
    }
    if (proto == IP_PROTO_ICMP) {
        if (l4len < 8 || (l4[0] != 8 && l4[0] != 0)) return NULL;    /* echo request / reply */
        return l4 + 4;
    }
    return NULL;
}

/* Recompute the transport checksum after a rewrite (UDP 0 = none, kept). */
static void l4_fix(uint8_t* ip, uint32_t ihl, uint32_t total) {
    uint8_t proto = ip[9];
    uint8_t* l4 = ip + ihl;
    uint32_t l4len = total - ihl;
    uint32_t src = (uint32_t)ip[12] << 24 | (uint32_t)ip[13] << 16 | (uint32_t)ip[14] << 8 | ip[15];
    uint32_t dst = (uint32_t)ip[16] << 24 | (uint32_t)ip[17] << 16 | (uint32_t)ip[18] << 8 | ip[19];
    if (proto == IP_PROTO_TCP && l4len >= 20) {
        l4[16] = l4[17] = 0;
        uint16_t c = l4_checksum(src, dst, IP_PROTO_TCP, l4, l4len);
        l4[16] = (uint8_t)c; l4[17] = (uint8_t)(c >> 8);
    } else if (proto == IP_PROTO_UDP && l4len >= 8 && (l4[6] | l4[7])) {
        l4[6] = l4[7] = 0;
        uint16_t c = l4_checksum(src, dst, IP_PROTO_UDP, l4, l4len);
        if (!c) c = 0xFFFF;
        l4[6] = (uint8_t)c; l4[7] = (uint8_t)(c >> 8);
    } else if (proto == IP_PROTO_ICMP && l4len >= 8) {
        l4[2] = l4[3] = 0;
        uint16_t c = net_checksum(l4, l4len);
        l4[2] = (uint8_t)c; l4[3] = (uint8_t)(c >> 8);
    }
}
static void ip_fix(uint8_t* ip, uint32_t ihl) {
    ip[10] = ip[11] = 0;
    uint16_t c = net_checksum(ip, ihl);
    ip[10] = (uint8_t)c; ip[11] = (uint8_t)(c >> 8);
}
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int masq_wanted(struct net_device* out, uint32_t src) {
    if (!config_get_long("net.masquerade", 1)) return 0;
    if (out->kind != NETDEV_K_HW || (out->flags & (NETDEV_F_LOOPBACK | NETDEV_F_SIMULATED))) return 0;
    if (!out->ip) return 0;
    return !out->netmask || (src & out->netmask) != (out->ip & out->netmask);
}

/* Outbound: rewrite the inside source to the device's address and a port of
 * ours.  0 = rewritten, -1 = no entry could be made (dropped). */
static int nat_out_locked(int ns, struct net_device* out, uint8_t* ip, uint32_t ihl, uint32_t total) {
    uint8_t proto = ip[9];
    uint8_t* l4 = ip + ihl;
    uint8_t* sp = nat_port_ptr(l4, total - ihl, proto, 0);
    if (!sp) return 0;                                 /* not translatable: as is */
    uint8_t* dp = (proto == IP_PROTO_ICMP) ? sp : nat_port_ptr(l4, total - ihl, proto, 1);
    uint32_t src = get_be32(ip + 12), dst = get_be32(ip + 16);
    uint16_t sport = (uint16_t)(sp[0] << 8 | sp[1]);
    uint16_t dport = proto == IP_PROTO_ICMP ? 0 : (uint16_t)(dp[0] << 8 | dp[1]);
    uint64_t now = timer_ticks_ms();
    struct nat_ent* e = NULL;
    for (int i = 0; i < NAT_MAX; i++) {
        struct nat_ent* x = &g_nat[i];
        if (x->used && x->ns == ns && x->proto == proto && x->in_ip == src && x->in_port == sport &&
            x->peer_ip == dst && x->peer_port == dport && x->out_ip == out->ip) { e = x; break; }
    }
    if (!e) {
        struct nat_ent* victim = NULL;
        for (int i = 0; i < NAT_MAX && !victim; i++)
            if (!g_nat[i].used || now - g_nat[i].last_ms > nat_idle_ms(g_nat[i].proto)) victim = &g_nat[i];
        if (!victim) { g_nat_full++; return -1; }
        /* a port no live entry of this protocol and address uses */
        uint16_t port = 0;
        for (uint32_t tries = 0; tries < NAT_PORT_N && !port; tries++) {
            uint16_t c = (uint16_t)(NAT_PORT_LO + (g_nat_next++ % NAT_PORT_N));
            int taken = 0;
            for (int i = 0; i < NAT_MAX && !taken; i++)
                if (g_nat[i].used && &g_nat[i] != victim && g_nat[i].proto == proto &&
                    g_nat[i].out_ip == out->ip && g_nat[i].ext_port == c) taken = 1;
            if (!taken) port = c;
        }
        if (!port) { g_nat_full++; return -1; }
        e = victim;
        e->used = 1; e->ns = ns; e->proto = proto;
        e->in_ip = src; e->in_port = sport; e->peer_ip = dst; e->peer_port = dport;
        e->out_ip = out->ip; e->ext_port = port;
    }
    e->last_ms = now;
    put_be32(ip + 12, out->ip);
    sp[0] = (uint8_t)(e->ext_port >> 8); sp[1] = (uint8_t)e->ext_port;
    l4_fix(ip, ihl, total);
    g_nat_out++;
    return 0;
}

/* Inbound on `dev`: a reply to a masqueraded flow?  Rewritten back to the
 * inside address and port in `ip` (a copy), 1; else 0. */
static int nat_in_locked(struct net_device* dev, uint8_t* ip, uint32_t ihl, uint32_t total) {
    uint8_t proto = ip[9];
    uint8_t* l4 = ip + ihl;
    uint8_t* dp = nat_port_ptr(l4, total - ihl, proto, 1);
    if (!dp) return 0;
    if (proto == IP_PROTO_ICMP && l4[0] != 0) return 0;          /* only an echo REPLY */
    uint8_t* sp = proto == IP_PROTO_ICMP ? dp : nat_port_ptr(l4, total - ihl, proto, 0);
    uint32_t src = get_be32(ip + 12), dst = get_be32(ip + 16);
    uint16_t dport = (uint16_t)(dp[0] << 8 | dp[1]);
    uint16_t sport = proto == IP_PROTO_ICMP ? 0 : (uint16_t)(sp[0] << 8 | sp[1]);
    for (int i = 0; i < NAT_MAX; i++) {
        struct nat_ent* e = &g_nat[i];
        if (!e->used || e->ns != dev->netns || e->proto != proto || e->out_ip != dst ||
            e->ext_port != dport || e->peer_ip != src || e->peer_port != sport) continue;
        e->last_ms = timer_ticks_ms();
        put_be32(ip + 16, e->in_ip);
        dp[0] = (uint8_t)(e->in_port >> 8); dp[1] = (uint8_t)e->in_port;
        l4_fix(ip, ihl, total);
        g_nat_in++;
        return 1;
    }
    return 0;
}

/* Send the IPv4 packet in g_fwdbuf out of the route for its destination. */
static void fwd_emit_locked(int ns, struct net_device* in, uint32_t ihl, uint32_t total, int nat) {
    uint8_t* ip = g_fwdbuf;
    uint32_t dst = get_be32(ip + 16), src = get_be32(ip + 12);
    uint32_t nh = 0;
    struct net_device* out = net_route_ns(ns, dst, &nh);
    if (!out || out == in || out->admin_down || (out->flags & NETDEV_F_LOOPBACK)) { g_fwd_noroute++; return; }
    if (nat && masq_wanted(out, src) && nat_out_locked(ns, out, ip, ihl, total) != 0) return;
    if (ip[8] <= 1) return;                            /* TTL expired (no ICMP sent) */
    ip[8]--;
    ip_fix(ip, ihl);
    uint8_t mac[ETH_ALEN];
    if (arp_cache_get(out, nh, mac) != 0) {
        /* Not known yet: ask, and HOLD the packet until the answer (Linux's
         * neighbour queue) — dropping it cost every new flow its first
         * packet, i.e. a TCP SYN retransmit (a second) per connection. */
        arp_send_locked(out, ARP_OP_REQUEST, BCAST_MAC, nh);
        if (arp_hold_locked(out, nh, ip, total) != 0) g_fwd_noarp++;
        return;
    }
    eth_send_locked(out, mac, ETHERTYPE_IPV4, ip, total);
    g_fwd_count++;
}

static void net_forward_locked(struct net_device* dev, const uint8_t* src_mac,
                               const uint8_t* p, uint32_t len) {
    (void)src_mac;
    int ns = dev->netns;
    if (!net_ip_forward(ns)) return;
    uint32_t ihl = (p[0] & 0x0F) * 4u;
    uint32_t total = (uint32_t)p[2] << 8 | p[3];
    if (total > len || total > sizeof g_fwdbuf || ihl < 20 || ihl > total) return;
    for (uint32_t i = 0; i < total; i++) g_fwdbuf[i] = p[i];
    fwd_emit_locked(ns, dev, ihl, total, 1);
}

/* A reply to a masqueraded flow, arriving at the machine's address: rewritten
 * and forwarded inside.  1 = it was one (consumed). */
static int net_unmasq_locked(struct net_device* dev, const uint8_t* p, uint32_t len) {
    if (!dev->ip || dev->kind != NETDEV_K_HW) return 0;
    uint32_t ihl = (p[0] & 0x0F) * 4u;
    uint32_t total = (uint32_t)p[2] << 8 | p[3];
    if (total > len || total > sizeof g_fwdbuf || ihl < 20 || ihl > total) return 0;
    if (get_be32(p + 16) != dev->ip) return 0;
    uint8_t proto = p[9];
    if (proto != IP_PROTO_TCP && proto != IP_PROTO_UDP && proto != IP_PROTO_ICMP) return 0;
    for (uint32_t i = 0; i < total; i++) g_fwdbuf[i] = p[i];
    if (!nat_in_locked(dev, g_fwdbuf, ihl, total)) return 0;
    fwd_emit_locked(dev->netns, dev, ihl, total, 0);
    return 1;
}

uint32_t net_arp_released(void) { return g_arp_released; }
void net_fwd_stats(uint32_t* fwd, uint32_t* noarp, uint32_t* noroute, uint32_t* nat_out,
                   uint32_t* nat_in, uint32_t* nat_full) {
    if (fwd) *fwd = g_fwd_count;
    if (noarp) *noarp = g_fwd_noarp;
    if (noroute) *noroute = g_fwd_noroute;
    if (nat_out) *nat_out = g_nat_out;
    if (nat_in) *nat_in = g_nat_in;
    if (nat_full) *nat_full = g_nat_full;
}

/* §M90 3a — the L2 send, for net6.c. */
int net_eth_send_locked(struct net_device* dev, const uint8_t* dst_mac,
                        uint16_t ethertype, const void* payload, uint32_t len) {
    return eth_send_locked(dev, dst_mac, ethertype, payload, len);
}

static void ipv4_input(struct net_device* dev, const uint8_t* src_mac,
                       const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct ipv4_hdr)) return;
    const struct ipv4_hdr* ih = (const struct ipv4_hdr*)p;
    if ((ih->ver_ihl >> 4) != 4) return;
    uint32_t ihl = (ih->ver_ihl & 0x0F) * 4;
    if (ihl < sizeof(struct ipv4_hdr) || ihl > len) return;

    uint32_t dst = ntohl(ih->dst);
    /* An UNCONFIGURED device (dev->ip == 0) accepts anything addressed to it,
     * because it does not yet know what it is addressed as.  That is not
     * laxity, it is the only way DHCP can work: the reply that TELLS us our
     * address is itself addressed to that address. */
    /* §M90 rung 5d — a reply to a masqueraded flow is the container's, not ours. */
    if (net_unmasq_locked(dev, p, len)) return;
    /* §M90 rung 5 — any address of the device's NAMESPACE is ours (Linux's
     * weak host model: the host's eth0 address is reachable from a container
     * through the bridge).  Anything else is somebody else's: forwarded, when
     * this namespace forwards (net_forward_locked), else dropped. */
    if (dev->ip && dst != dev->ip && dst != 0xFFFFFFFFu && !net_is_local_ip_ns(dev->netns, dst)) {
        net_forward_locked(dev, src_mac, p, len);
        return;
    }

    uint32_t src = ntohl(ih->src);
    uint16_t total = ntohs(ih->total_len);
    if (total > len) return;
    const uint8_t* l4 = p + ihl;
    uint32_t l4len = total - ihl;

    switch (ih->proto) {
        case IP_PROTO_ICMP:
            icmp_input(dev, src_mac, src, l4, l4len);
            icmp_deliver_locked(dev, src, p, total);       /* §M90 — ICMP sockets */
            break;
        case IP_PROTO_UDP:  udp_input (dev, src, l4, l4len); break;
        case IP_PROTO_TCP:  tcp_input (dev, src_mac, src, dst, l4, l4len); break;
        default: break;
    }
}

/* ----------------------- Ethernet demux (RX entry) ------------------------ */

/* Called by the driver's poll (and, once the NIC interrupt lands, from an ISR)
 * with the stack lock HELD — see the file header.  Nothing below here may
 * block; the source MAC is threaded down so replies never need to look one up. */
void net_rx(struct net_device* dev, const uint8_t* frame, uint32_t len) {
    if (len < ETH_HLEN) { dev->rx_dropped++; return; }
    if (dev->admin_down) { dev->rx_dropped++; return; }     /* §M87 */
    const struct eth_hdr* eh = (const struct eth_hdr*)frame;

    /* §M90 rung 5 — a BRIDGE PORT does not deliver to the local stack: the
     * frame is the bridge's to switch (and to deliver to itself if it is
     * addressed to the bridge). */
    if (dev->master && dev->master->port_rx) {
        dev->rx_packets++; dev->rx_bytes += len;
        g_rx_frames++;
        dev->master->port_rx(dev->master, dev, frame, len);
        return;
    }

    /* Accept frames addressed to us or broadcast — and IPv6 multicast
     * (33:33:…): neighbour discovery is multicast, so a device that drops it
     * never learns a neighbour.  Not filtered per group: an interface here is
     * a member of every group it might be asked about. */
    if (!mac_eq(eh->dst, dev->mac) && !mac_eq(eh->dst, BCAST_MAC)
        && !(eh->dst[0] == 0x33 && eh->dst[1] == 0x33)) return;

    dev->rx_packets++; dev->rx_bytes += len;
    g_rx_frames++;                            /* what net_pump_locked counts */
    const uint8_t* payload = frame + ETH_HLEN;
    uint32_t plen = len - ETH_HLEN;
    packet_tap_locked(dev, frame, len);       /* §M90 — AF_PACKET sockets */

    switch (ntohs(eh->ethertype)) {
        case ETHERTYPE_ARP:  arp_input (dev, payload, plen); break;
        case ETHERTYPE_IPV4: ipv4_input(dev, eh->src, payload, plen); break;
        case ETHERTYPE_IPV6: ipv6_input(dev, eh->src, payload, plen); break;   /* §M90 3a */
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

/* =============================================================================
 * /proc/net (§M24 stage 8)
 *
 * The same information `lsnic`, `netstat` and `arp` print, as FILES — because
 * a shell command can only be read by a person at a console, and a file can be
 * read by a program, a test script, and a person over a serial line.  The
 * shapes deliberately echo Linux's (/proc/net/dev, /proc/net/arp,
 * /proc/net/route, /proc/net/tcp) without pretending to be byte-compatible
 * with them: matching the columns exactly would invite tools to parse them,
 * and this kernel does not promise what those tools would then rely on.
 * ========================================================================== */

static void pw_ip(struct procfs_writer* w, uint32_t ip) {
    char b[16]; net_fmt_ip(ip, b); pw_puts(w, b);
}

static void gen_net_dev(struct procfs_writer* w) {
    pw_puts(w, "iface  rx-packets rx-bytes tx-packets tx-bytes drops  address\n");
    int ns = net_cur_ns();                    /* §M90 — the reader's namespace */
    for (struct net_device* n = g_head; n; n = n->next) {
        if (n->netns != ns) continue;
        pw_puts(w, n->name); pw_puts(w, "  ");
        pw_put_uint(w, n->rx_packets); pw_putc(w, ' ');
        pw_put_uint(w, n->rx_bytes);   pw_putc(w, ' ');
        pw_put_uint(w, n->tx_packets); pw_putc(w, ' ');
        pw_put_uint(w, n->tx_bytes);   pw_putc(w, ' ');
        pw_put_uint(w, n->rx_dropped); pw_putc(w, ' ');
        pw_ip(w, n->ip); pw_putc(w, '/'); pw_ip(w, n->netmask);
        if (n->flags & NETDEV_F_LOOPBACK) pw_puts(w, " loopback");
        pw_putc(w, '\n');
    }
}

static void gen_net_arp(struct procfs_writer* w) {
    pw_puts(w, "address          hw-address\n");
    uint32_t f = net_lock();
    struct arp_entry snap[ARP_CACHE_SIZE];
    for (int i = 0; i < ARP_CACHE_SIZE; i++) snap[i] = g_arp[i];
    net_unlock(f);
    int ns = net_cur_ns();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!snap[i].valid || !snap[i].dev || snap[i].dev->netns != ns) continue;
        char mb[18]; net_fmt_mac(snap[i].mac, mb);
        pw_ip(w, snap[i].ip); pw_puts(w, "  "); pw_puts(w, mb); pw_putc(w, '\n');
    }
}

static void gen_net_route(struct procfs_writer* w) {
    pw_puts(w, "destination      gateway          iface\n");
    int ns = net_cur_ns();
    for (struct net_device* n = g_head; n; n = n->next) {
        if (n->netns != ns || !n->ip) continue;
        pw_ip(w, n->ip & n->netmask); pw_puts(w, "  ");
        pw_puts(w, "-                ");
        pw_puts(w, n->name); pw_putc(w, '\n');
    }
    struct net_rt r;
    for (int i = 0; net_route_at(i, &r) == 0; i++) {
        if (!r.used || r.ns != ns || !r.dev) continue;
        pw_ip(w, r.dst); pw_putc(w, '/'); pw_put_uint(w, (uint32_t)mask_len(r.mask)); pw_puts(w, "  ");
        if (r.gw) pw_ip(w, r.gw); else pw_puts(w, "-");
        pw_puts(w, "  "); pw_puts(w, r.dev->name); pw_putc(w, '\n');
    }
    struct net_device* d = net_primary_ns(ns);
    if (d && d->gateway) {
        pw_puts(w, "0.0.0.0          ");
        pw_ip(w, d->gateway); pw_puts(w, "  "); pw_puts(w, d->name); pw_putc(w, '\n');
    }
}

static void proc_tcp_row(const struct net_tcp_info* i, void* ctx) {
    struct procfs_writer* w = (struct procfs_writer*)ctx;
    pw_puts(w, i->state); pw_putc(w, ' ');
    pw_ip(w, i->local_ip); pw_putc(w, ':'); pw_put_uint(w, i->local_port);
    pw_puts(w, " -> ");
    pw_ip(w, i->peer_ip);  pw_putc(w, ':'); pw_put_uint(w, i->peer_port);
    pw_puts(w, "  rxq "); pw_put_uint(w, i->rx_queued);
    pw_puts(w, " txq ");  pw_put_uint(w, i->tx_queued);
    pw_puts(w, " retx "); pw_put_uint(w, i->retrans);
    pw_puts(w, "  ");     pw_puts(w, i->dev);
    pw_putc(w, '\n');
}

static void gen_net_tcp(struct procfs_writer* w) {
    pw_puts(w, "state local -> peer  queues  device\n");
    net_tcp_foreach(proc_tcp_row, w);
    uint32_t rs = 0, rr = 0, nc = 0;
    net_tcp_counters(&rs, &rr, &nc);
    pw_puts(w, "resets-sent "); pw_put_uint(w, rs);
    pw_puts(w, " resets-received "); pw_put_uint(w, rr);
    pw_puts(w, " no-connection "); pw_put_uint(w, nc);
    pw_putc(w, '\n');
}

static void gen_net_stat(struct procfs_writer* w) {
    struct net_poller_stats st;
    net_poller_stats(&st);
    pw_puts(w, "poller "); pw_puts(w, st.running ? "running" : "stopped");
    pw_puts(w, " waiters "); pw_put_uint(w, (unsigned)st.waiters);
    pw_puts(w, " pumps ");   pw_put_uint(w, st.pumps);
    pw_puts(w, " inline ");  pw_put_uint(w, st.inline_pumps);
    pw_puts(w, " frames ");  pw_put_uint(w, st.frames);
    pw_putc(w, '\n');
    pw_puts(w, "irqs ");      pw_put_uint(w, st.irqs);
    pw_puts(w, " backstops "); pw_put_uint(w, st.backstops);
    pw_puts(w, " missed ");    pw_put_uint(w, st.missed_irqs);
    pw_putc(w, '\n');
    uint32_t pers = 0, rtos = 0, zwin = 0, dsegs = 0, wblk = 0;
    net_tcp_timing_counters(&pers, &rtos, &zwin);
    net_tcp_output_counters(&dsegs, &wblk);
    pw_puts(w, "tcp data-segments "); pw_put_uint(w, dsegs);
    pw_puts(w, " window-blocked ");   pw_put_uint(w, wblk);
    pw_puts(w, " rtos ");             pw_put_uint(w, rtos);
    pw_puts(w, " persists ");         pw_put_uint(w, pers);
    pw_puts(w, " zero-window ");      pw_put_uint(w, zwin);
    pw_putc(w, '\n');
}

/* §M90 rung 5d — forwarding and masquerade counters (the reader's view of
 * the whole stack: the counters are not per namespace). */
static void gen_net_fwd(struct procfs_writer* w) {
    uint32_t fw, na, nr, no, ni, nf;
    net_fwd_stats(&fw, &na, &nr, &no, &ni, &nf);
    pw_puts(w, "forwarded "); pw_put_uint(w, fw);
    pw_puts(w, " no-arp ");   pw_put_uint(w, na);
    pw_puts(w, " no-route "); pw_put_uint(w, nr);
    pw_puts(w, " masq-out "); pw_put_uint(w, no);
    pw_puts(w, " masq-in ");  pw_put_uint(w, ni);
    pw_puts(w, " masq-full "); pw_put_uint(w, nf);
    pw_puts(w, " arp-held "); pw_put_uint(w, g_arp_held);
    pw_puts(w, " arp-released "); pw_put_uint(w, g_arp_released);
    pw_putc(w, '\n');
}

/* /proc/sys/net/ipv4/ip_forward — "0" or "1", the WRITER'S namespace (as on
 * Linux, each namespace has its own). */
static void gen_ip_forward(struct procfs_writer* w) {
    pw_put_uint(w, (unsigned)net_ip_forward(net_cur_ns())); pw_putc(w, '\n');
}
static long write_ip_forward(struct task* target, const char* buf, size_t n) {
    (void)target;
    if (!n || (buf[0] != '0' && buf[0] != '1')) return -22;
    net_set_ip_forward(net_cur_ns(), buf[0] == '1');
    return (long)n;
}

static struct procfs_node nd_net_dev   = { .name = "net/dev",   .gen = gen_net_dev   };
static struct procfs_node nd_net_arp   = { .name = "net/arp",   .gen = gen_net_arp   };
static struct procfs_node nd_net_route = { .name = "net/route", .gen = gen_net_route };
static struct procfs_node nd_net_tcp   = { .name = "net/tcp",   .gen = gen_net_tcp   };
static struct procfs_node nd_net_stat  = { .name = "net/stat",  .gen = gen_net_stat  };
static struct procfs_node nd_net_fwd   = { .name = "net/forward", .gen = gen_net_fwd };
static struct procfs_node nd_ip_forward = { .name = "sys/net/ipv4/ip_forward", .gen = gen_ip_forward,
                                            .write = write_ip_forward };

void net_procfs_init(void) {
    procfs_register(&nd_net_dev);
    procfs_register(&nd_net_arp);
    procfs_register(&nd_net_route);
    procfs_register(&nd_net_tcp);
    procfs_register(&nd_net_stat);
    procfs_register(&nd_net_fwd);
    procfs_register(&nd_ip_forward);
    net6_procfs_init();                         /* §M90 3a — conf/all/{forwarding,disable_ipv6} */
}

CONFIG_KEY(ck_net_masq) = {
    .key = "net.masquerade", .group = "Network", .type = CFG_BOOL, .def = "1",
    .help = "rewrite forwarded traffic leaving the machine to the machine's address (what Docker's "
            "iptables MASQUERADE rule does on Linux)",
};
