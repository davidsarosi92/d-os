/* =============================================================================
 * dhcp.c — a DHCP client (§M24 stage 7).
 *
 * WHY IT MATTERS MORE THAN IT LOOKS.  Every address in this kernel was a
 * CONSTANT until now: 10.0.2.15/24 via 10.0.2.2, with 10.0.2.3 as the
 * nameserver, compiled into the virtio-net driver because that is what QEMU's
 * SLIRP hands out.  Those constants are correct on exactly one emulator
 * configuration, and the failure mode on any other network is not an error
 * message — it is a machine that transmits into the void.  §M48's lesson,
 * one layer up: a constant chosen for the machine of the day, with a comment
 * explaining why it was safe.
 *
 * WHAT IT IS.  The four-message exchange, and nothing beyond it:
 *
 *     DISCOVER  →  (broadcast, from 0.0.0.0, "is anyone a server?")
 *               ←  OFFER      (here is an address you may have)
 *     REQUEST   →  (broadcast, "I accept THAT address from THAT server")
 *               ←  ACK        (it is yours, for this long)
 *
 * The REQUEST is broadcast rather than unicast on purpose: it is how the
 * servers whose offers we did NOT take find out they can release them.
 *
 * The address is applied to the device, and the lease's T1 (half its life) arms
 * a renewal.  Renewing from a ktimer callback is impossible — that runs in
 * interrupt context and DHCP has to send and wait — so the timer only submits
 * to §M49's work queue and a worker does the talking.  That indirection is the
 * whole reason the work queue exists.
 *
 * NOT DONE, and written down rather than left to be discovered: no DECLINE
 * (we do not ARP-probe the offered address first), no REBIND at T2 (a renewal
 * that fails simply retries), no INFORM, no client identifier beyond the MAC,
 * and no persistence of the lease across a reboot.  Each is a real part of the
 * protocol; none of them changes whether an address is obtained on a network
 * that hands them out.
 * ============================================================================= */

#include "net.h"
#include "printf.h"
#include "config.h"
#include "task.h"
#include "service.h"
#include "ktimer.h"
#include "timer.h"
#include "workqueue.h"
#include <stdint.h>
#include <stddef.h>

#define DHCP_SERVER_PORT   67
#define DHCP_CLIENT_PORT   68

#define BOOTREQUEST        1
#define BOOTREPLY          2
#define HTYPE_ETHER        1
#define DHCP_MAGIC         0x63825363u

#define DHCPDISCOVER       1
#define DHCPOFFER          2
#define DHCPREQUEST        3
#define DHCPACK            5
#define DHCPNAK            6

/* Options we read or write.  The numbers are the protocol's; the names are
 * here so the parser reads as the RFC does. */
#define OPT_SUBNET         1
#define OPT_ROUTER         3
#define OPT_DNS            6
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_MSG_TYPE      53
#define OPT_SERVER_ID     54
#define OPT_PARAM_LIST    55
#define OPT_END          255

/* The BOOTP frame.  The option area is deliberately modest: a DHCP packet must
 * be at least 300 bytes total (some servers drop shorter ones) and ours comes
 * to 364, which clears that without approaching the MTU. */
struct dhcp_pkt {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[128];
} __attribute__((packed));

/* Client state.  One lease at a time — this host has one address, and a
 * second concurrent negotiation would be two clients arguing over it. */
static struct {
    volatile int      active;
    volatile int      got_offer, got_ack, got_nak;
    uint32_t          xid;
    uint32_t          offered_ip, server_id;
    uint32_t          mask, router, dns, lease_s;
    struct net_device* dev;
    uint64_t          expires_ns;
    int               bound;
} g_dhcp;

static struct ktimer g_renew_timer;
static volatile int  g_renew_armed;

/* ----------------------- option helpers ----------------------------------- */

static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t opt_find(const uint8_t* opts, uint32_t len, uint8_t want,
                         const uint8_t** val, uint8_t* vlen) {
    uint32_t i = 0;
    while (i < len) {
        uint8_t code = opts[i];
        if (code == 0) { i++; continue; }                 /* pad              */
        if (code == OPT_END) break;
        if (i + 1 >= len) break;
        uint8_t l = opts[i + 1];
        if (i + 2 + l > len) break;
        if (code == want) { if (val) *val = &opts[i + 2]; if (vlen) *vlen = l; return 1; }
        i += 2 + l;
    }
    return 0;
}

static uint32_t opt_put(uint8_t* o, uint32_t at, uint8_t code,
                        const void* val, uint8_t len) {
    o[at++] = code;
    o[at++] = len;
    const uint8_t* v = (const uint8_t*)val;
    for (uint8_t i = 0; i < len; i++) o[at++] = v[i];
    return at;
}

/* ----------------------- receive ------------------------------------------ */

/* Runs on the poller task WITH THE STACK LOCK HELD (§M55): it may only record
 * what arrived.  Everything that follows from it — sending the REQUEST,
 * reconfiguring the device — happens in the task that is waiting. */
static void dhcp_recv(uint32_t src_ip, uint16_t src_port,
                      const uint8_t* data, uint32_t len, void* ctx) {
    (void)ctx; (void)src_ip; (void)src_port;
    if (!g_dhcp.active || len < sizeof(struct dhcp_pkt) - sizeof(((struct dhcp_pkt*)0)->options))
        return;
    const struct dhcp_pkt* p = (const struct dhcp_pkt*)data;
    if (p->op != BOOTREPLY) return;
    /* The transaction id is what tells our conversation from somebody else's
     * on the same broadcast domain. */
    if (rd32((const uint8_t*)&p->xid) != g_dhcp.xid) return;
    if (rd32((const uint8_t*)&p->magic) != DHCP_MAGIC) return;

    uint32_t optlen = len - (uint32_t)(sizeof(struct dhcp_pkt) - sizeof p->options);
    if (optlen > sizeof p->options) optlen = sizeof p->options;

    const uint8_t* v; uint8_t vl;
    if (!opt_find(p->options, optlen, OPT_MSG_TYPE, &v, &vl) || vl < 1) return;
    uint8_t type = v[0];

    if (type == DHCPOFFER) {
        g_dhcp.offered_ip = rd32((const uint8_t*)&p->yiaddr);
        if (opt_find(p->options, optlen, OPT_SERVER_ID, &v, &vl) && vl == 4)
            g_dhcp.server_id = rd32(v);
        g_dhcp.got_offer = 1;
    } else if (type == DHCPACK) {
        g_dhcp.offered_ip = rd32((const uint8_t*)&p->yiaddr);
        if (opt_find(p->options, optlen, OPT_SUBNET, &v, &vl) && vl == 4) g_dhcp.mask   = rd32(v);
        if (opt_find(p->options, optlen, OPT_ROUTER, &v, &vl) && vl >= 4) g_dhcp.router = rd32(v);
        if (opt_find(p->options, optlen, OPT_DNS,    &v, &vl) && vl >= 4) g_dhcp.dns    = rd32(v);
        if (opt_find(p->options, optlen, OPT_LEASE_TIME, &v, &vl) && vl == 4)
            g_dhcp.lease_s = rd32(v);
        g_dhcp.got_ack = 1;
    } else if (type == DHCPNAK) {
        g_dhcp.got_nak = 1;
    }
}

/* ----------------------- send --------------------------------------------- */

static void dhcp_send(struct net_device* dev, uint8_t type, uint32_t req_ip,
                      uint32_t server_id) {
    struct dhcp_pkt p;
    uint8_t* raw = (uint8_t*)&p;
    for (uint32_t i = 0; i < sizeof p; i++) raw[i] = 0;

    p.op    = BOOTREQUEST;
    p.htype = HTYPE_ETHER;
    p.hlen  = ETH_ALEN;
    p.xid   = htonl(g_dhcp.xid);
    /* Ask for a BROADCAST reply.  A client with no address configured cannot
     * be relied upon to accept a unicast one — the server would have to ARP
     * for an address we do not answer to yet. */
    p.flags = htons(0x8000);
    p.magic = htonl(DHCP_MAGIC);
    for (int i = 0; i < ETH_ALEN; i++) p.chaddr[i] = dev->mac[i];

    uint32_t at = 0;
    at = opt_put(p.options, at, OPT_MSG_TYPE, &type, 1);
    if (req_ip) {
        uint32_t be = htonl(req_ip);
        at = opt_put(p.options, at, OPT_REQUESTED_IP, &be, 4);
    }
    if (server_id) {
        uint32_t be = htonl(server_id);
        at = opt_put(p.options, at, OPT_SERVER_ID, &be, 4);
    }
    static const uint8_t params[] = { OPT_SUBNET, OPT_ROUTER, OPT_DNS, OPT_LEASE_TIME };
    at = opt_put(p.options, at, OPT_PARAM_LIST, params, sizeof params);
    p.options[at++] = OPT_END;

    net_udp_broadcast(dev, 0, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &p, sizeof p);
}

/* ----------------------- the exchange ------------------------------------- */

static int cond_offer(void* a) { (void)a; return g_dhcp.got_offer || g_dhcp.got_nak; }
static int cond_ack  (void* a) { (void)a; return g_dhcp.got_ack   || g_dhcp.got_nak; }

#define DHCP_WAIT_MS   2000
#define DHCP_TRIES        3

static struct work g_renew_work;
static void dhcp_renew_work(struct work* w);

static void dhcp_apply(struct net_device* dev) {
    dev->ip = g_dhcp.offered_ip;
    if (g_dhcp.mask)   dev->netmask = g_dhcp.mask;
    if (g_dhcp.router) dev->gateway = g_dhcp.router;
    if (g_dhcp.dns)    net_set_dns(g_dhcp.dns);
    g_dhcp.bound      = 1;
    g_dhcp.dev        = dev;
    g_dhcp.expires_ns = timer_now_ns() + (uint64_t)g_dhcp.lease_s * 1000000000ull;

    char a[16], m[16], r[16], d[16];
    net_fmt_ip(dev->ip, a); net_fmt_ip(dev->netmask, m);
    net_fmt_ip(dev->gateway, r); net_fmt_ip(net_get_dns(), d);
    kprintf("dhcp: %s bound %s mask %s gw %s dns %s lease %us\n",
            dev->name, a, m, r, d, g_dhcp.lease_s);
}

/* Renewal is armed at T1 = half the lease, which is what the protocol says and
 * is also the only value that leaves room to retry before the address stops
 * being ours. */
static void dhcp_renew_fired(struct ktimer* t) {
    (void)t;
    g_renew_armed = 0;
    /* Interrupt context: hand the actual conversation to a worker.  A DHCP
     * exchange sends and then WAITS, and neither is legal here. */
    work_init(&g_renew_work, dhcp_renew_work);
    work_submit(&g_renew_work);
}

static void dhcp_arm_renew(void) {
    if (!g_dhcp.lease_s || g_renew_armed) return;
    uint64_t t1 = timer_now_ns() + (uint64_t)(g_dhcp.lease_s / 2) * 1000000000ull;
    g_renew_armed = 1;
    ktimer_arm(&g_renew_timer, t1, dhcp_renew_fired, NULL);
}

int dhcp_configure(struct net_device* dev) {
    if (!dev) dev = net_primary();
    if (!dev) { kprintf("dhcp: no network device\n"); return -1; }
    if (dev->flags & NETDEV_F_LOOPBACK) { kprintf("dhcp: not on a loopback\n"); return -1; }

    /* A transaction id that differs between attempts and between boots.  The
     * clock is the source: a fixed one would make two machines on the same
     * segment indistinguishable to the server. */
    g_dhcp.xid = (uint32_t)(timer_now_ns() >> 3) ^ 0x646F5300u;
    g_dhcp.active = 1;
    g_dhcp.got_offer = g_dhcp.got_ack = g_dhcp.got_nak = 0;
    g_dhcp.offered_ip = g_dhcp.server_id = 0;
    g_dhcp.mask = g_dhcp.router = g_dhcp.dns = 0;
    g_dhcp.lease_s = 0;

    if (net_udp_bind(DHCP_CLIENT_PORT, dhcp_recv, NULL) != 0) {
        kprintf("dhcp: cannot bind port %d\n", DHCP_CLIENT_PORT);
        g_dhcp.active = 0;
        return -1;
    }

    int rc = -1;
    for (int try = 0; try < DHCP_TRIES && rc != 0; try++) {
        g_dhcp.got_offer = g_dhcp.got_ack = g_dhcp.got_nak = 0;
        dhcp_send(dev, DHCPDISCOVER, 0, 0);
        if (!net_wait_cond(cond_offer, NULL, DHCP_WAIT_MS)) continue;
        if (g_dhcp.got_nak) continue;

        dhcp_send(dev, DHCPREQUEST, g_dhcp.offered_ip, g_dhcp.server_id);
        if (!net_wait_cond(cond_ack, NULL, DHCP_WAIT_MS)) continue;
        if (g_dhcp.got_nak) continue;

        dhcp_apply(dev);
        rc = 0;
    }

    net_udp_unbind(DHCP_CLIENT_PORT);
    g_dhcp.active = 0;
    if (rc != 0) kprintf("dhcp: no answer on %s (3 attempts)\n", dev->name);
    else dhcp_arm_renew();
    return rc;
}

static void dhcp_renew_work(struct work* w) {
    (void)w;
    if (!g_dhcp.bound || !g_dhcp.dev) return;
    kprintf("dhcp: renewing lease on %s\n", g_dhcp.dev->name);
    dhcp_configure(g_dhcp.dev);
}

void dhcp_status(void) {
    if (!g_dhcp.bound) { kprintf("dhcp: no lease\n"); return; }
    uint64_t now = timer_now_ns();
    uint32_t left = (g_dhcp.expires_ns > now)
                  ? (uint32_t)((g_dhcp.expires_ns - now) / 1000000000ull) : 0;
    char a[16], s[16];
    net_fmt_ip(g_dhcp.dev ? g_dhcp.dev->ip : 0, a);
    net_fmt_ip(g_dhcp.server_id, s);
    kprintf("dhcp: %s has %s from server %s, %us of %us left\n",
            g_dhcp.dev ? g_dhcp.dev->name : "?", a, s, left, g_dhcp.lease_s);
}

/* ----------------------- the boot-time service ---------------------------- */

/* OFF by default, and the reason is worth stating: QEMU's SLIRP hands out
 * exactly the address the driver already hard-codes, so running DHCP at every
 * boot would add seconds of latency to every test on this project's usual
 * target in exchange for arriving at the same four numbers.  On a real network
 * it is the only thing that works, which is why it is one config key away
 * rather than one code change away. */
static void dhcp_service_entry(void) {
    if (config_get_long("net.dhcp", 0) == 0) { task_exit(); return; }
    /* Give the NIC driver a moment to finish registering before asking it to
     * carry a broadcast. */
    task_msleep(200);
    dhcp_configure(NULL);
    task_exit();
}
SERVICE("dhcp", dhcp_service_entry, 1, SVC_RESTART_NO);
