/* =============================================================================
 * net6.c — IPv6, §M90 3a (stages a + b: addresses, neighbour discovery,
 * ICMPv6, routes, echo).
 *
 * WHY HERE AND NOT A FLAG ON net.c.  IPv6 is not IPv4 with longer addresses:
 * it has no ARP (neighbours are found with ICMPv6 Neighbour Solicitation sent
 * to a MULTICAST group derived from the address), no broadcast, a fixed
 * 40-byte header with no checksum of its own (every upper layer checksums a
 * PSEUDO-HEADER that carries both 16-byte addresses), and every interface has
 * a LINK-LOCAL address (fe80::/64 from its MAC) before anything is configured.
 * Those are the parts that live here; the stack lock, the device list, the
 * namespaces and the L2 send are net.c's and are reached through net.h.
 *
 * WHAT A DEVICE CARRIES (net.h): `ip6_ll` (link-local, EUI-64 from the MAC,
 * set at register) and up to NET_IP6_MAX configured addresses with prefix
 * lengths; a loopback has ::1/128.  Addresses are kept in NETWORK byte order —
 * an IPv6 address has no host order worth having.
 *
 * NEIGHBOURS.  A small cache (dev, address) → MAC, filled by every Neighbour
 * Advertisement AND by the source link-layer option of every Solicitation we
 * answer (the asker is about to talk to us).  Resolution sends NS to the
 * solicited-node group ff02::1:ffXX:XXXX (MAC 33:33:ff:XX:XX:XX) and waits on
 * the stack's condition like ARP does; it must therefore never run under the
 * stack lock, and the RX path never resolves — replies go to the MAC the
 * frame came from (§M55's rule, unchanged).
 *
 * ROUTES.  Per namespace: the CONNECTED prefixes of the namespace's devices
 * (from their configured addresses), a table (prefix, length, gateway, device)
 * and longest-prefix wins.  A LINK-LOCAL destination has no route — it is
 * only meaningful together with a link, so the caller names the device (the
 * `%eth0` of "fe80::1%eth0", or a socket's scope id).
 *
 * NOT HERE (stated, not silently absent): SLAAC and Router Advertisements
 * (Docker configures addresses statically), Duplicate Address Detection
 * (an address is used at once), extension headers other than skipping
 * Hop-by-Hop (nothing here sends them), fragmentation (the MTU is 1500 on
 * every link this kernel has; an oversized datagram is refused), MLD (the
 * devices accept every 33:33 multicast frame, so no membership report is
 * needed on links we control).
 * ============================================================================= */

#include "net.h"
#include "printf.h"
#include "kmalloc.h"
#include "timer.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

#define IP6_HLEN      40
#define NH_TCP        6
#define NH_UDP        17
#define NH_ICMP6      58
#define NH_HOPBYHOP   0

#define ICMP6_ECHO_REQ   128
#define ICMP6_ECHO_REP   129
#define ICMP6_NS         135
#define ICMP6_NA         136

/* ---- address helpers ---------------------------------------------------------- */

static const uint8_t IP6_ZERO[16];
static const uint8_t IP6_LOOP[16] = { [15] = 1 };
static const uint8_t IP6_ALLNODES[16] = { 0xff, 0x02, [15] = 1 };

static int a6_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void a6_cpy(uint8_t* d, const uint8_t* s) { for (int i = 0; i < 16; i++) d[i] = s[i]; }
static int a6_is_mcast(const uint8_t* a) { return a[0] == 0xff; }
static int a6_is_ll(const uint8_t* a) { return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; }
static int a6_is_zero(const uint8_t* a) { return a6_eq(a, IP6_ZERO); }
static int a6_prefix_eq(const uint8_t* a, const uint8_t* b, int plen) {
    int i = 0;
    for (; plen >= 8; plen -= 8, i++) if (a[i] != b[i]) return 0;
    if (plen) {
        uint8_t m = (uint8_t)(0xff << (8 - plen));
        if ((a[i] & m) != (b[i] & m)) return 0;
    }
    return 1;
}
/* ff02::1:ffXX:XXXX — the group a Neighbour Solicitation for `a` goes to. */
static void a6_solicited(const uint8_t* a, uint8_t* g) {
    a6_cpy(g, IP6_ZERO);
    g[0] = 0xff; g[1] = 0x02; g[11] = 0x01; g[12] = 0xff;
    g[13] = a[13]; g[14] = a[14]; g[15] = a[15];
}
static void mcast_mac(const uint8_t* a, uint8_t* mac) {
    mac[0] = 0x33; mac[1] = 0x33;
    mac[2] = a[12]; mac[3] = a[13]; mac[4] = a[14]; mac[5] = a[15];
}

/* An IPv6 upper-layer checksum: the pseudo-header (both addresses, the
 * length, the next header) and then the data, as big-endian 16-bit words.
 * Returned in host order; stored big-endian by the caller. */
static uint16_t csum6(const uint8_t* src, const uint8_t* dst, uint8_t nh,
                      const uint8_t* data, uint32_t len) {
    uint32_t s = 0;
    for (int i = 0; i < 16; i += 2) s += (uint32_t)(src[i] << 8 | src[i + 1]);
    for (int i = 0; i < 16; i += 2) s += (uint32_t)(dst[i] << 8 | dst[i + 1]);
    s += len >> 16; s += len & 0xffff;
    s += nh;
    for (uint32_t i = 0; i + 1 < len; i += 2) s += (uint32_t)(data[i] << 8 | data[i + 1]);
    if (len & 1) s += (uint32_t)(data[len - 1] << 8);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

/* ---- parse / format ------------------------------------------------------------ */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int net_parse_ip6(const char* s, uint8_t out[16], char* scope, int scope_cap) {
    uint16_t w[8] = {0};
    int n = 0, gap = -1;
    if (scope && scope_cap) scope[0] = 0;
    if (s[0] == ':' && s[1] == ':') { gap = 0; s += 2; }
    while (*s && *s != '%') {
        int v = 0, d = 0;
        while (hexv(*s) >= 0 && d < 5) { v = v * 16 + hexv(*s); s++; d++; }
        if (!d || d > 4 || n >= 8) return -1;
        w[n++] = (uint16_t)v;
        if (*s == ':') {
            s++;
            if (*s == ':') { if (gap >= 0) return -1; gap = n; s++; }
            else if (!*s || *s == '%') return -1;
        } else if (*s && *s != '%') return -1;
    }
    if (*s == '%' && scope) {
        int i = 0;
        for (s++; *s && i + 1 < scope_cap; s++) scope[i++] = *s;
        scope[i] = 0;
    }
    if (gap >= 0) {
        int tail = n - gap;
        if (n >= 8) return -1;
        for (int i = 7; i >= 8 - tail; i--) w[i] = w[i - (8 - n)];
        for (int i = gap; i < 8 - tail; i++) w[i] = 0;
    } else if (n != 8) return -1;
    for (int i = 0; i < 8; i++) { out[2 * i] = (uint8_t)(w[i] >> 8); out[2 * i + 1] = (uint8_t)w[i]; }
    return 0;
}

void net_fmt_ip6(const uint8_t a[16], char* out) {
    uint16_t w[8];
    for (int i = 0; i < 8; i++) w[i] = (uint16_t)(a[2 * i] << 8 | a[2 * i + 1]);
    /* the longest run of zero words (length >= 2) becomes "::" (RFC 5952) */
    int bs = -1, bl = 0;
    for (int i = 0; i < 8; ) {
        if (w[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !w[j]) j++;
        if (j - i > bl && j - i >= 2) { bs = i; bl = j - i; }
        i = j;
    }
    static const char hx[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 8; i++) {
        if (i == bs) { out[o++] = ':'; if (i == 0) out[o++] = ':'; i += bl - 1; continue; }
        int started = 0;
        for (int sh = 12; sh >= 0; sh -= 4) {
            int d = (w[i] >> sh) & 0xf;
            if (d || started || sh == 0) { out[o++] = hx[d]; started = 1; }
        }
        if (i < 7) out[o++] = ':';
    }
    out[o] = 0;
}

/* ---- per-device addresses ------------------------------------------------------- */

void net6_dev_init(struct net_device* dev) {
    if (dev->flags & NETDEV_F_LOOPBACK) {
        if (!dev->n_ip6) { a6_cpy(dev->ip6[0], IP6_LOOP); dev->ip6_plen[0] = 128; dev->n_ip6 = 1; }
        return;
    }
    /* fe80::/64 with the modified EUI-64 interface id from the MAC */
    uint8_t* a = dev->ip6_ll;
    a6_cpy(a, IP6_ZERO);
    a[0] = 0xfe; a[1] = 0x80;
    a[8]  = (uint8_t)(dev->mac[0] ^ 0x02); a[9] = dev->mac[1]; a[10] = dev->mac[2];
    a[11] = 0xff; a[12] = 0xfe;
    a[13] = dev->mac[3]; a[14] = dev->mac[4]; a[15] = dev->mac[5];
}

int net6_addr_add(struct net_device* dev, const uint8_t a[16], int plen) {
    if (!dev || plen < 0 || plen > 128 || a6_is_zero(a) || a6_is_mcast(a)) return -22;
    uint32_t f = net_lock();
    int r = 0;
    for (int i = 0; i < dev->n_ip6; i++)
        if (a6_eq(dev->ip6[i], a)) { r = -17; goto out; }            /* EEXIST */
    if (a6_is_ll(a)) { a6_cpy(dev->ip6_ll, a); goto out; }            /* replaces the derived one */
    if (dev->n_ip6 >= NET_IP6_MAX) { r = -28; goto out; }             /* ENOSPC */
    a6_cpy(dev->ip6[dev->n_ip6], a);
    dev->ip6_plen[dev->n_ip6] = (uint8_t)plen;
    dev->n_ip6++;
out:
    net_unlock(f);
    return r;
}

int net6_addr_del(struct net_device* dev, const uint8_t a[16]) {
    if (!dev) return -22;
    uint32_t f = net_lock();
    int r = -99;                                                      /* EADDRNOTAVAIL */
    for (int i = 0; i < dev->n_ip6; i++) {
        if (!a6_eq(dev->ip6[i], a)) continue;
        for (int k = i; k + 1 < dev->n_ip6; k++) {
            a6_cpy(dev->ip6[k], dev->ip6[k + 1]);
            dev->ip6_plen[k] = dev->ip6_plen[k + 1];
        }
        dev->n_ip6--;
        r = 0;
        break;
    }
    net_unlock(f);
    return r;
}

/* Is `a` one of `dev`'s own addresses (or a group it listens to)? */
static int dev_owns(struct net_device* dev, const uint8_t* a) {
    if (a6_is_mcast(a)) {
        if (a6_eq(a, IP6_ALLNODES)) return 1;
        uint8_t g[16];
        a6_solicited(dev->ip6_ll, g);
        if (a6_eq(a, g)) return 1;
        for (int i = 0; i < dev->n_ip6; i++) { a6_solicited(dev->ip6[i], g); if (a6_eq(a, g)) return 1; }
        return 0;
    }
    if (!(dev->flags & NETDEV_F_LOOPBACK) && a6_eq(a, dev->ip6_ll)) return 1;
    for (int i = 0; i < dev->n_ip6; i++) if (a6_eq(a, dev->ip6[i])) return 1;
    return 0;
}

/* The source address for a packet to `dst` out of `dev`: link-local for a
 * link-local or multicast destination, else the first configured address
 * whose prefix covers dst, else the first configured one, else link-local. */
static void pick_src(struct net_device* dev, const uint8_t* dst, uint8_t* src) {
    /* round the loopback, a packet to one of our own addresses comes FROM it */
    if (dev->flags & NETDEV_F_LOOPBACK) { a6_cpy(src, dst); return; }
    if (a6_is_ll(dst) || a6_is_mcast(dst) || !dev->n_ip6) { a6_cpy(src, dev->ip6_ll); return; }
    for (int i = 0; i < dev->n_ip6; i++)
        if (a6_prefix_eq(dev->ip6[i], dst, dev->ip6_plen[i])) { a6_cpy(src, dev->ip6[i]); return; }
    a6_cpy(src, dev->ip6[0]);
}

/* ---- neighbours ----------------------------------------------------------------- */

#define NEIGH_MAX 64
static struct { int used; struct net_device* dev; uint8_t a[16]; uint8_t mac[ETH_ALEN]; uint64_t ms; } g_neigh[NEIGH_MAX];
static uint32_t g_ns_sent, g_na_sent, g_na_rcvd, g_echo_rcvd, g_rx6, g_rx6_drop;

static void hold6_release_locked(struct net_device* dev, const uint8_t* a, const uint8_t* mac);
static void neigh_put_locked(struct net_device* dev, const uint8_t* a, const uint8_t* mac) {
    if (a6_is_zero(a) || a6_is_mcast(a)) return;
    int slot = -1;
    uint64_t oldest = ~0ull;
    for (int i = 0; i < NEIGH_MAX; i++) {
        if (g_neigh[i].used && g_neigh[i].dev == dev && a6_eq(g_neigh[i].a, a)) { slot = i; break; }
        if (!g_neigh[i].used) { if (oldest) { slot = i; oldest = 0; } continue; }
        if (g_neigh[i].ms < oldest) { oldest = g_neigh[i].ms; slot = i; }
    }
    if (slot < 0) return;
    g_neigh[slot].used = 1; g_neigh[slot].dev = dev;
    a6_cpy(g_neigh[slot].a, a);
    for (int k = 0; k < ETH_ALEN; k++) g_neigh[slot].mac[k] = mac[k];
    g_neigh[slot].ms = timer_ticks_ms();
    hold6_release_locked(dev, a, mac);             /* forwarded packets waiting for it */
}
static int neigh_get_locked(struct net_device* dev, const uint8_t* a, uint8_t* mac) {
    for (int i = 0; i < NEIGH_MAX; i++)
        if (g_neigh[i].used && g_neigh[i].dev == dev && a6_eq(g_neigh[i].a, a)) {
            for (int k = 0; k < ETH_ALEN; k++) mac[k] = g_neigh[i].mac[k];
            return 0;
        }
    return -1;
}

/* ---- routes ------------------------------------------------------------------- */

#define RT6_MAX 64
static struct { int used, ns; uint8_t pfx[16]; uint8_t plen; uint8_t gw[16]; struct net_device* dev; } g_rt6[RT6_MAX];

int net6_route_add(int ns, const uint8_t pfx[16], int plen, const uint8_t gw[16], struct net_device* dev) {
    if (plen < 0 || plen > 128 || !dev) return -22;
    uint32_t f = net_lock();
    int r = -28;
    for (int i = 0; i < RT6_MAX; i++)
        if (g_rt6[i].used && g_rt6[i].ns == ns && g_rt6[i].plen == plen && a6_prefix_eq(g_rt6[i].pfx, pfx, plen)) {
            r = -17; goto out;
        }
    for (int i = 0; i < RT6_MAX; i++) {
        if (g_rt6[i].used) continue;
        g_rt6[i].used = 1; g_rt6[i].ns = ns; g_rt6[i].plen = (uint8_t)plen;
        a6_cpy(g_rt6[i].pfx, pfx); a6_cpy(g_rt6[i].gw, gw ? gw : IP6_ZERO);
        g_rt6[i].dev = dev;
        r = 0;
        break;
    }
out:
    net_unlock(f);
    return r;
}
int net6_route_del(int ns, const uint8_t pfx[16], int plen) {
    uint32_t f = net_lock();
    int r = -3;                                                       /* ESRCH */
    for (int i = 0; i < RT6_MAX; i++)
        if (g_rt6[i].used && g_rt6[i].ns == ns && g_rt6[i].plen == plen && a6_prefix_eq(g_rt6[i].pfx, pfx, plen)) {
            g_rt6[i].used = 0; r = 0;
        }
    net_unlock(f);
    return r;
}

struct rt6_pick { int ns; const uint8_t* dst; int best; struct net_device* dev; uint8_t* nh; };
static void rt6_connected(struct net_device* d, void* ctx) {
    struct rt6_pick* p = (struct rt6_pick*)ctx;
    if (d->netns != p->ns || d->admin_down) return;
    for (int i = 0; i < d->n_ip6; i++) {
        int pl = d->ip6_plen[i];
        if (pl > p->best && a6_prefix_eq(d->ip6[i], p->dst, pl)) {
            /* our OWN address goes round the namespace's loopback (as v4) */
            p->best = pl; p->dev = d;
            if (p->nh) a6_cpy(p->nh, p->dst);
        }
    }
}
static void rt6_own(struct net_device* d, void* ctx) {
    struct rt6_pick* p = (struct rt6_pick*)ctx;
    if (d->netns == p->ns && (d->flags & NETDEV_F_LOOPBACK) && !p->dev) p->dev = d;
}
static void rt6_owner(struct net_device* d, void* ctx) {
    struct rt6_pick* p = (struct rt6_pick*)ctx;
    if (d->netns == p->ns && !(d->flags & NETDEV_F_LOOPBACK) && dev_owns(d, p->dst)) p->best = 129;
}

/* Longest prefix among the connected routes and the table.  `nexthop` gets
 * the gateway, or `dst` itself on-link.  A link-local destination has no
 * route (the caller names the link). */
struct net_device* net6_route(int ns, const uint8_t dst[16], uint8_t nexthop[16]) {
    if (a6_is_ll(dst) || a6_is_mcast(dst)) return NULL;
    struct rt6_pick p = { ns, dst, -1, NULL, nexthop };
    /* one of our own addresses: delivered by the namespace's loopback */
    net_for_each(rt6_owner, &p);
    if (p.best == 129 || a6_eq(dst, IP6_LOOP)) {
        p.dev = NULL;
        net_for_each(rt6_own, &p);
        if (p.dev && nexthop) a6_cpy(nexthop, dst);
        return p.dev;
    }
    p.best = -1;
    net_for_each(rt6_connected, &p);
    for (int i = 0; i < RT6_MAX; i++) {
        if (!g_rt6[i].used || g_rt6[i].ns != ns || g_rt6[i].plen <= p.best) continue;
        if (!a6_prefix_eq(g_rt6[i].pfx, dst, g_rt6[i].plen) || g_rt6[i].dev->admin_down) continue;
        p.best = g_rt6[i].plen; p.dev = g_rt6[i].dev;
        if (nexthop) a6_cpy(nexthop, a6_is_zero(g_rt6[i].gw) ? dst : g_rt6[i].gw);
    }
    return p.dev;
}

/* The i-th table route (for netlink's dump): 0, or -1 past the end. */
int net6_route_at(int i, struct net6_rt* out) {
    if (i < 0 || i >= RT6_MAX) return -1;
    uint32_t f = net_lock();
    out->used = g_rt6[i].used; out->ns = g_rt6[i].ns; out->plen = g_rt6[i].plen;
    a6_cpy(out->pfx, g_rt6[i].pfx); a6_cpy(out->gw, g_rt6[i].gw);
    out->dev = g_rt6[i].used ? g_rt6[i].dev : NULL;
    net_unlock(f);
    return 0;
}

static void hold6_drop_dev(struct net_device* dev);
void net6_dev_gone_locked(struct net_device* dev) {
    hold6_drop_dev(dev);
    for (int i = 0; i < NEIGH_MAX; i++) if (g_neigh[i].dev == dev) g_neigh[i].used = 0;
    for (int i = 0; i < RT6_MAX; i++) if (g_rt6[i].dev == dev) g_rt6[i].used = 0;
}

/* ---- emit --------------------------------------------------------------------- */

/* One IPv6 packet: header + payload to `via_mac`.  Stack lock held. */
int net6_disabled(int ns);
static int ip6_emit_locked(struct net_device* dev, const uint8_t* via_mac, const uint8_t* src,
                           const uint8_t* dst, uint8_t nh, uint8_t hops,
                           const uint8_t* payload, uint32_t len) {
    static uint8_t pkt[ETH_MTU];
    if (net6_disabled(dev->netns)) return -97;       /* EAFNOSUPPORT, as Linux: disabled */
    if (len + IP6_HLEN > dev->mtu || len + IP6_HLEN > sizeof pkt) return -90;   /* EMSGSIZE */
    pkt[0] = 0x60; pkt[1] = 0; pkt[2] = 0; pkt[3] = 0;
    pkt[4] = (uint8_t)(len >> 8); pkt[5] = (uint8_t)len;
    pkt[6] = nh; pkt[7] = hops;
    a6_cpy(pkt + 8, src); a6_cpy(pkt + 24, dst);
    for (uint32_t i = 0; i < len; i++) pkt[IP6_HLEN + i] = payload[i];
    return net_eth_send_locked(dev, via_mac, ETHERTYPE_IPV6, pkt, IP6_HLEN + len);
}

static void icmp6_fill_csum(uint8_t* m, uint32_t len, const uint8_t* src, const uint8_t* dst) {
    m[2] = m[3] = 0;
    uint16_t c = csum6(src, dst, NH_ICMP6, m, len);
    m[2] = (uint8_t)(c >> 8); m[3] = (uint8_t)c;
}

/* A Neighbour Solicitation for `target` out of `dev`.  Stack lock held. */
static void ns_send_locked(struct net_device* dev, const uint8_t* target) {
    uint8_t m[32] = { ICMP6_NS, 0 };
    a6_cpy(m + 8, target);
    m[24] = 1; m[25] = 1;                                /* source link-layer address */
    for (int k = 0; k < ETH_ALEN; k++) m[26 + k] = dev->mac[k];
    uint8_t g[16], mac[ETH_ALEN], src[16];
    a6_solicited(target, g);
    mcast_mac(g, mac);
    pick_src(dev, target, src);
    icmp6_fill_csum(m, sizeof m, src, g);
    ip6_emit_locked(dev, mac, src, g, NH_ICMP6, 255, m, sizeof m);
    g_ns_sent++;
}

struct n6wait { struct net_device* dev; const uint8_t* a; uint8_t* mac; };
static int n6_cond(void* x) {
    struct n6wait* w = (struct n6wait*)x;
    return neigh_get_locked(w->dev, w->a, w->mac) == 0;
}
/* The MAC for `nh` on `dev`.  Blocks: NEVER with the stack lock held. */
static int neigh_resolve(struct net_device* dev, const uint8_t* nh, uint8_t* mac) {
    if (dev->flags & NETDEV_F_LOOPBACK) { for (int k = 0; k < ETH_ALEN; k++) mac[k] = dev->mac[k]; return 0; }
    if (a6_is_mcast(nh)) { mcast_mac(nh, mac); return 0; }
    struct n6wait w = { dev, nh, mac };
    uint32_t f = net_lock();
    int hit = neigh_get_locked(dev, nh, mac) == 0;
    net_unlock(f);
    if (hit) return 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        f = net_lock();
        ns_send_locked(dev, nh);
        net_unlock(f);
        if (net_wait_cond(n6_cond, &w, 1000)) return 0;
    }
    return -1;
}

/* ---- forwarding (§M90 3a stage b) --------------------------------------------- *
 *
 * Per namespace, off by default (/proc/sys/net/ipv6/conf/all/forwarding, as
 * on Linux).  A packet for an address that is not the namespace's is routed,
 * its hop limit decremented (a packet that would leave with 0 is dropped),
 * and sent to the next hop's MAC; when that MAC is not yet known a
 * Solicitation goes out and the packet is HELD until the Advertisement (the
 * v4 path's ARP hold queue, here).  Link-local packets never cross a router.
 * No ICMPv6 error is sent for a drop (Time Exceeded, Unreachable) — said. */
#define NET6_NS_MAX 256
static uint8_t g_fwd6[NET6_NS_MAX], g_dis6[NET6_NS_MAX];
static uint32_t g_fwd6_count, g_fwd6_noroute, g_fwd6_held, g_fwd6_hoplimit;
int  net6_forwarding(int ns) { return ns >= 0 && ns < NET6_NS_MAX ? g_fwd6[ns] : 0; }
void net6_set_forwarding(int ns, int on) { if (ns >= 0 && ns < NET6_NS_MAX) g_fwd6[ns] = on ? 1 : 0; }
int  net6_disabled(int ns) { return ns >= 0 && ns < NET6_NS_MAX ? g_dis6[ns] : 0; }
void net6_set_disabled(int ns, int on) { if (ns >= 0 && ns < NET6_NS_MAX) g_dis6[ns] = on ? 1 : 0; }

#define HOLD6_SLOTS 8
static struct { int used; struct net_device* dev; uint8_t nh[16]; uint32_t len; uint64_t ms;
                uint8_t data[ETH_MTU]; } g_hold6[HOLD6_SLOTS];
static void hold6_release_locked(struct net_device* dev, const uint8_t* a, const uint8_t* mac) {
    for (int i = 0; i < HOLD6_SLOTS; i++) {
        if (!g_hold6[i].used || g_hold6[i].dev != dev || !a6_eq(g_hold6[i].nh, a)) continue;
        g_hold6[i].used = 0;
        net_eth_send_locked(dev, mac, ETHERTYPE_IPV6, g_hold6[i].data, g_hold6[i].len);
        g_fwd6_count++;
    }
}
static void hold6_drop_dev(struct net_device* dev) {
    for (int i = 0; i < HOLD6_SLOTS; i++) if (g_hold6[i].dev == dev) g_hold6[i].used = 0;
}
static void forward6_locked(struct net_device* in, const uint8_t* p, uint32_t len) {
    int ns = in->netns;
    const uint8_t* src = p + 8;
    const uint8_t* dst = p + 24;
    if (a6_is_ll(src) || a6_is_ll(dst) || a6_is_mcast(dst)) return;
    if (p[7] <= 1) { g_fwd6_hoplimit++; return; }
    uint8_t nh[16];
    struct net_device* out = net6_route(ns, dst, nh);
    if (!out || (out->flags & NETDEV_F_LOOPBACK)) { g_fwd6_noroute++; return; }
    static uint8_t buf[ETH_MTU];
    if (len > sizeof buf || len > out->mtu) return;
    for (uint32_t i = 0; i < len; i++) buf[i] = p[i];
    buf[7]--;                                         /* hop limit (not checksummed) */
    uint8_t mac[ETH_ALEN];
    if (neigh_get_locked(out, nh, mac) == 0) {
        net_eth_send_locked(out, mac, ETHERTYPE_IPV6, buf, len);
        g_fwd6_count++;
        return;
    }
    ns_send_locked(out, nh);
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < HOLD6_SLOTS; i++) {
        if (g_hold6[i].used && now - g_hold6[i].ms > 3000) g_hold6[i].used = 0;
        if (g_hold6[i].used) continue;
        g_hold6[i].used = 1; g_hold6[i].dev = out; a6_cpy(g_hold6[i].nh, nh);
        g_hold6[i].len = len; g_hold6[i].ms = now;
        for (uint32_t k = 0; k < len; k++) g_hold6[i].data[k] = buf[k];
        g_fwd6_held++;
        return;
    }
}

/* /proc/sys/net/ipv6/conf/all/{forwarding,disable_ipv6} — the WRITER's
 * namespace, as ip_forward.  Per-interface conf/<if>/ directories are not
 * offered: every setting here is per namespace (said, not pretended). */
#include "procfs.h"
static void gen_fwd6(struct procfs_writer* w) { pw_put_uint(w, (unsigned)net6_forwarding(net_cur_ns())); pw_putc(w, '\n'); }
static long write_fwd6(struct task* t, const char* buf, size_t n) {
    (void)t;
    if (!n || (buf[0] != '0' && buf[0] != '1')) return -22;
    net6_set_forwarding(net_cur_ns(), buf[0] == '1');
    return (long)n;
}
static void gen_dis6(struct procfs_writer* w) { pw_put_uint(w, (unsigned)net6_disabled(net_cur_ns())); pw_putc(w, '\n'); }
static long write_dis6(struct task* t, const char* buf, size_t n) {
    (void)t;
    if (!n || (buf[0] != '0' && buf[0] != '1')) return -22;
    net6_set_disabled(net_cur_ns(), buf[0] == '1');
    return (long)n;
}
static struct procfs_node nd_fwd6 = { .name = "sys/net/ipv6/conf/all/forwarding", .gen = gen_fwd6, .write = write_fwd6 };
static struct procfs_node nd_dis6 = { .name = "sys/net/ipv6/conf/all/disable_ipv6", .gen = gen_dis6, .write = write_dis6 };
void net6_procfs_init(void) { procfs_register(&nd_fwd6); procfs_register(&nd_dis6); }

/* ---- receive ------------------------------------------------------------------ */

static volatile uint16_t g_ping6_id, g_ping6_seq_got;
static volatile int g_ping6_wait;

/* §M90 3a — ICMPv6 sockets: every ICMPv6 message for the namespace goes to
 * each listener (usyscall.c's raw and ping sockets filter).  Unlike IPv4's
 * raw socket, an IPv6 one receives the message WITHOUT the IP header. */
#define ICMP6_LISTENERS 16
static struct { int used, ns; icmp6_recv_fn fn; void* ctx; } g_i6l[ICMP6_LISTENERS];
int net6_icmp_listen(int ns, icmp6_recv_fn fn, void* ctx) {
    uint32_t f = net_lock();
    int r = -1;
    for (int i = 0; i < ICMP6_LISTENERS; i++)
        if (!g_i6l[i].used) { g_i6l[i].used = 1; g_i6l[i].ns = ns; g_i6l[i].fn = fn; g_i6l[i].ctx = ctx; r = 0; break; }
    net_unlock(f);
    return r;
}
void net6_icmp_unlisten(void* ctx) {
    uint32_t f = net_lock();
    for (int i = 0; i < ICMP6_LISTENERS; i++) if (g_i6l[i].used && g_i6l[i].ctx == ctx) g_i6l[i].used = 0;
    net_unlock(f);
}
/* Send an ICMPv6 message built by a socket: the checksum is the kernel's (as
 * Linux does for ICMPv6 raw sockets).  Blocks for neighbour discovery. */
int net6_icmp_send(int ns, struct net_device* scope, const uint8_t dst[16], uint8_t* m, uint32_t len) {
    struct net_device* dev = NULL;
    uint8_t mac[ETH_ALEN], src[16];
    if (len < 4 || len > ETH_MTU - IP6_HLEN) return -90;
    int pr = net6_prepare(ns, scope, dst, &dev, mac, src);
    if (pr) return pr == -1 ? -101 : -113;
    uint32_t f = net_lock();
    icmp6_fill_csum(m, len, src, dst);
    int r = ip6_emit_locked(dev, mac, src, dst, NH_ICMP6, 64, m, len);
    net_unlock(f);
    return r;
}

static void icmp6_input(struct net_device* dev, const uint8_t* src_mac, const uint8_t* src,
                        const uint8_t* dst, const uint8_t* m, uint32_t len) {
    if (len < 8 || csum6(src, dst, NH_ICMP6, m, len) != 0) { g_rx6_drop++; return; }
    for (int i = 0; i < ICMP6_LISTENERS; i++)
        if (g_i6l[i].used && g_i6l[i].ns == dev->netns) g_i6l[i].fn(src, m, len, g_i6l[i].ctx);
    switch (m[0]) {
    case ICMP6_ECHO_REQ: {
        if (a6_is_mcast(dst) && !a6_eq(dst, IP6_ALLNODES)) return;
        static uint8_t r[ETH_MTU];
        if (len > sizeof r - IP6_HLEN) return;
        for (uint32_t i = 0; i < len; i++) r[i] = m[i];
        r[0] = ICMP6_ECHO_REP;
        uint8_t me[16];
        if (a6_is_mcast(dst)) pick_src(dev, src, me); else a6_cpy(me, dst);
        icmp6_fill_csum(r, len, me, src);
        ip6_emit_locked(dev, src_mac, me, src, NH_ICMP6, 64, r, len);
        break;
    }
    case ICMP6_ECHO_REP:
        if (len >= 8 && g_ping6_wait && (uint16_t)(m[4] << 8 | m[5]) == g_ping6_id) {
            g_ping6_seq_got = (uint16_t)(m[6] << 8 | m[7]);
            g_echo_rcvd++;
        }
        break;
    case ICMP6_NS: {
        if (len < 24) return;
        const uint8_t* target = m + 8;
        if (!dev_owns(dev, target) || a6_is_mcast(target)) return;
        /* the asker's own MAC, from its option or the frame */
        const uint8_t* lla = src_mac;
        for (uint32_t o = 24; o + 8 <= len && m[o + 1]; o += m[o + 1] * 8u)
            if (m[o] == 1) { lla = m + o + 2; break; }
        if (!a6_is_zero(src)) neigh_put_locked(dev, src, lla);
        uint8_t a[32] = { ICMP6_NA, 0 };
        a[4] = a6_is_zero(src) ? 0x20 : 0x60;          /* Solicited|Override (S only if unicast reply) */
        a6_cpy(a + 8, target);
        a[24] = 2; a[25] = 1;                          /* target link-layer address */
        for (int k = 0; k < ETH_ALEN; k++) a[26 + k] = dev->mac[k];
        const uint8_t* to = a6_is_zero(src) ? IP6_ALLNODES : src;
        uint8_t tomac[ETH_ALEN];
        if (a6_is_zero(src)) mcast_mac(IP6_ALLNODES, tomac);
        else for (int k = 0; k < ETH_ALEN; k++) tomac[k] = lla[k];
        icmp6_fill_csum(a, sizeof a, target, to);
        ip6_emit_locked(dev, tomac, target, to, NH_ICMP6, 255, a, sizeof a);
        g_na_sent++;
        break;
    }
    case ICMP6_NA: {
        if (len < 24) return;
        const uint8_t* target = m + 8;
        const uint8_t* lla = NULL;
        for (uint32_t o = 24; o + 8 <= len && m[o + 1]; o += m[o + 1] * 8u)
            if (m[o] == 2) { lla = m + o + 2; break; }
        neigh_put_locked(dev, target, lla ? lla : src_mac);
        g_na_rcvd++;
        break;
    }
    default:
        break;                         /* router discovery, MLD, errors: not here */
    }
}


struct own_q { int ns; const uint8_t* a; int hit; };
static void own_any(struct net_device* d, void* ctx) {
    struct own_q* q = (struct own_q*)ctx;
    if (d->netns == q->ns && dev_owns(d, q->a)) q->hit = 1;
}

void ipv6_input(struct net_device* dev, const uint8_t* src_mac, const uint8_t* p, uint32_t len) {
    if (len < IP6_HLEN || (p[0] >> 4) != 6) { g_rx6_drop++; return; }
    if (net6_disabled(dev->netns)) { g_rx6_drop++; return; }     /* disable_ipv6 = 1 */
    uint32_t plen = (uint32_t)(p[4] << 8 | p[5]);
    if (plen + IP6_HLEN > len) { g_rx6_drop++; return; }
    const uint8_t* src = p + 8;
    const uint8_t* dst = p + 24;
    /* Linux's weak host: an address of ANY device of the namespace is ours,
     * whichever device the packet came in on (round the loopback above all). */
    if (!dev_owns(dev, dst)) {
        struct own_q q = { dev->netns, dst, 0 };
        if (!a6_is_mcast(dst)) net_for_each(own_any, &q);
        if (!q.hit) {
            if (net6_forwarding(dev->netns)) forward6_locked(dev, p, plen + IP6_HLEN);
            else g_rx6_drop++;
            return;
        }
    }
    g_rx6++;
    uint8_t nh = p[6];
    const uint8_t* l4 = p + IP6_HLEN;
    if (nh == NH_HOPBYHOP) {                             /* skip it (MLD reports carry one) */
        if (plen < 8) return;
        uint32_t hl = (uint32_t)(l4[1] + 1) * 8u;
        if (hl > plen) return;
        nh = l4[0]; l4 += hl; plen -= hl;
    }
    if (nh == NH_ICMP6) icmp6_input(dev, src_mac, src, dst, l4, plen);
    else if (nh == NH_TCP || nh == NH_UDP) net_l4_input6(dev, src_mac, src, dst, nh, l4, plen);
}

/* ---- what the transport (net.c) uses ------------------------------------------ */

uint16_t net6_csum(const uint8_t* src, const uint8_t* dst, uint8_t nh, const uint8_t* data, uint32_t len) {
    return csum6(src, dst, nh, data, len);
}
int net6_emit_locked(struct net_device* dev, const uint8_t* via_mac, const uint8_t* src,
                     const uint8_t* dst, uint8_t nh, const uint8_t* payload, uint32_t len) {
    return ip6_emit_locked(dev, via_mac, src, dst, nh, 64, payload, len);
}
int  net6_resolve(struct net_device* dev, const uint8_t* nh, uint8_t* mac) { return neigh_resolve(dev, nh, mac); }
void net6_pick_src(struct net_device* dev, const uint8_t* dst, uint8_t* src) { pick_src(dev, dst, src); }
int  net6_is_linklocal(const uint8_t* a) { return a6_is_ll(a); }

/* Route + next hop + MAC + source for a v6 destination: `scope` names the
 * link for a link-local one.  Blocks (neighbour discovery): never under the
 * stack lock.  0 / -1 (no route) / -2 (neighbour unreachable). */
int net6_prepare(int ns, struct net_device* scope, const uint8_t* dst,
                 struct net_device** dev_out, uint8_t* mac, uint8_t* src) {
    uint8_t nh[16];
    struct net_device* dev = scope;
    if (a6_is_ll(dst) || a6_is_mcast(dst)) { if (!dev) return -1; a6_cpy(nh, dst); }
    else {
        dev = net6_route(ns, dst, nh);
        if (!dev) return -1;
    }
    if (neigh_resolve(dev, nh, mac) != 0) return -2;
    uint32_t f = net_lock();
    pick_src(dev, dst, src);
    net_unlock(f);
    *dev_out = dev;
    return 0;
}

/* ---- echo --------------------------------------------------------------------- */

static int ping6_cond(void* a) { return g_ping6_seq_got == *(uint16_t*)a; }

int net_ping6(int ns, struct net_device* dev, const uint8_t dst[16], int count) {
    uint8_t nh[16];
    if (!dev) dev = net6_route(ns, dst, nh);
    else a6_cpy(nh, dst);
    if (!dev) { kprintf("ping6: no route\n"); return -1; }
    uint8_t mac[ETH_ALEN];
    if (neigh_resolve(dev, nh, mac) != 0) { kprintf("ping6: neighbour unreachable\n"); return 0; }
    static uint16_t id = 0x6400;
    g_ping6_id = ++id;
    g_ping6_wait = 1;
    int got = 0;
    for (int seq = 1; seq <= count; seq++) {
        uint8_t m[24] = { ICMP6_ECHO_REQ, 0, 0, 0, (uint8_t)(id >> 8), (uint8_t)id,
                          (uint8_t)(seq >> 8), (uint8_t)seq, 'd', '-', 'o', 's' };
        uint8_t src[16];
        uint32_t f = net_lock();
        pick_src(dev, dst, src);
        icmp6_fill_csum(m, sizeof m, src, dst);
        ip6_emit_locked(dev, mac, src, dst, NH_ICMP6, 64, m, sizeof m);
        net_unlock(f);
        uint16_t want = (uint16_t)seq;
        uint64_t t0 = timer_ticks_ms();
        if (net_wait_cond(ping6_cond, &want, 1000)) {
            got++;
            char b[48]; net_fmt_ip6(dst, b);
            kprintf("ping6: reply from %s seq=%d time=%d ms\n", b, seq, (int)(timer_ticks_ms() - t0));
        }
    }
    g_ping6_wait = 0;
    return got;
}

/* ---- the shell: ping6, ip6, and a test that makes each rule show -------------- */
#include "shellcmd.h"

static struct net_device* dev_by_name(int ns, const char* n) { return n && *n ? net_find_ns(ns, n) : NULL; }

static void cmd_ping6(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { kprintf("usage: ping6 <addr>[%%dev] [count]\n"); return; }
    char a[64]; int i = 0;
    while (args[i] && args[i] != ' ' && i < 63) { a[i] = args[i]; i++; }
    a[i] = 0;
    int count = 3;
    if (args[i] == ' ') { count = 0; for (const char* c = args + i + 1; *c >= '0' && *c <= '9'; c++) count = count * 10 + (*c - '0'); }
    uint8_t d[16]; char scope[16];
    if (net_parse_ip6(a, d, scope, sizeof scope) != 0) { kprintf("ping6: bad address\n"); return; }
    struct net_device* dev = dev_by_name(net_cur_ns(), scope);
    if (scope[0] && !dev) { kprintf("ping6: no device %s\n", scope); return; }
    if (a6_is_ll(d) && !dev) { kprintf("ping6: a link-local address needs %%dev\n"); return; }
    int got = net_ping6(net_cur_ns(), dev, d, count ? count : 3);
    kprintf("ping6: %d/%d replies\n", got < 0 ? 0 : got, count ? count : 3);
}
SHELL_CMD(ping6) = { "ping6", "<addr>[%dev] [count]", "ICMPv6 echo", SHELL_G_NET, cmd_ping6, SHELL_P_ANY };

static void ip6_show(struct net_device* d, void* ctx) {
    (void)ctx;
    if (d->netns != net_cur_ns()) return;
    char b[48];
    kprintf("%s:", d->name);
    if (!(d->flags & NETDEV_F_LOOPBACK)) { net_fmt_ip6(d->ip6_ll, b); kprintf(" %s/64", b); }
    for (int i = 0; i < d->n_ip6; i++) { net_fmt_ip6(d->ip6[i], b); kprintf(" %s/%d", b, d->ip6_plen[i]); }
    kprintf("\n");
}
static void cmd_ip6(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        net_for_each(ip6_show, NULL);
        kprintf("ip6: rx %u (dropped %u), NS sent %u, NA sent %u / received %u\n",
                g_rx6, g_rx6_drop, g_ns_sent, g_na_sent, g_na_rcvd);
        kprintf("ip6: forwarding %d: forwarded %u, held for NDP %u, no route %u, hop limit %u\n",
                net6_forwarding(net_cur_ns()), g_fwd6_count, g_fwd6_held, g_fwd6_noroute, g_fwd6_hoplimit);
        return;
    }
    /* ip6 add <dev> <addr>/<plen> */
    char w[3][64]; int n = 0;
    for (const char* c = args; *c && n < 3; ) {
        while (*c == ' ') c++;
        int k = 0;
        while (*c && *c != ' ' && k < 63) w[n][k++] = *c++;
        w[n][k] = 0;
        if (k) n++;
    }
    if (n == 3 && (w[0][0] == 'a' || w[0][0] == 'd')) {
        struct net_device* d = net_find_ns(net_cur_ns(), w[1]);
        char* sl = w[2];
        while (*sl && *sl != '/') sl++;
        int plen = 64;
        if (*sl == '/') { *sl = 0; plen = 0; for (char* c = sl + 1; *c >= '0' && *c <= '9'; c++) plen = plen * 10 + (*c - '0'); }
        uint8_t a[16];
        if (!d || net_parse_ip6(w[2], a, NULL, 0) != 0) { kprintf("ip6: bad device or address\n"); return; }
        int r = w[0][0] == 'a' ? net6_addr_add(d, a, plen) : net6_addr_del(d, a);
        kprintf("ip6: %s\n", r == 0 ? "ok" : "refused");
        return;
    }
    /* ip6 route <pfx>/<plen> via <gw> <dev>   ("default" = ::/0) */
    if (n >= 1 && w[0][0] == 'r') {
        char r[5][64]; int m = 0;
        for (const char* c = args; *c && m < 5; ) {
            while (*c == ' ') c++;
            int k = 0;
            while (*c && *c != ' ' && k < 63) r[m][k++] = *c++;
            r[m][k] = 0;
            if (k) m++;
        }
        if (m == 5 && r[2][0] == 'v') {
            uint8_t pfx[16], gw[16]; int plen = 0;
            if (r[1][0] == 'd') { for (int i = 0; i < 16; i++) pfx[i] = 0; }
            else {
                char* sl = r[1];
                while (*sl && *sl != '/') sl++;
                if (*sl == '/') { *sl = 0; for (char* c = sl + 1; *c >= '0' && *c <= '9'; c++) plen = plen * 10 + (*c - '0'); }
                if (net_parse_ip6(r[1], pfx, NULL, 0) != 0) { kprintf("ip6: bad prefix\n"); return; }
            }
            struct net_device* d = net_find_ns(net_cur_ns(), r[4]);
            if (!d || net_parse_ip6(r[3], gw, NULL, 0) != 0) { kprintf("ip6: bad gateway or device\n"); return; }
            kprintf("ip6: %s\n", net6_route_add(net_cur_ns(), pfx, plen, gw, d) == 0 ? "ok" : "refused");
            return;
        }
    }
    kprintf("usage: ip6 [add|del <dev> <addr>/<plen>] [route <pfx>/<plen>|default via <gw> <dev>]\n");
}
SHELL_CMD(ip6) = { "ip6", "[add|del <dev> <addr>/<plen>] [route ... via <gw> <dev>]", "IPv6 addresses and counters",
                   SHELL_G_NET, cmd_ip6, SHELL_P_ANY };

int netvirt_veth_create(int ns, const char* name, int peer_ns, const char* peer_name,
                        struct net_device** a_out, struct net_device** b_out);

static void cmd_ip6test(const char* args) {
    (void)args;
    int ok = 1;
    #define T6(cond, what) do { int _c = (cond); kprintf("ip6: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    uint8_t a[16], b[16];
    char s[48];
    /* formatting is RFC 5952's: the longest zero run, lower case */
    T6(net_parse_ip6("2001:db8:0:0:1::1", a, NULL, 0) == 0 && (net_fmt_ip6(a, s), 1)
       && s[0] == '2' && s[8] == ':' && s[9] == ':' && s[10] == '1', "parse + compress 2001:db8::1:0:0:1");
    kprintf("ip6:   -> %s\n", s);
    T6(net_parse_ip6("::1", a, NULL, 0) == 0 && a6_eq(a, IP6_LOOP), "::1");
    T6(net_parse_ip6("1:2:3", a, NULL, 0) != 0 && net_parse_ip6("1::2::3", a, NULL, 0) != 0, "malformed refused");

    int A = net_ns_create(), B = net_ns_create(), C = net_ns_create();
    struct net_device *va = NULL, *vb = NULL;
    if (A <= 0 || B <= 0 || C <= 0 || netvirt_veth_create(A, "v6a", B, "v6b", &va, &vb) != 0) {
        kprintf("ip6: FAIL (setup)\n");
        net_ns_destroy(A); net_ns_destroy(B); net_ns_destroy(C);
        return;
    }
    va->admin_down = 0; vb->admin_down = 0;
    struct net_device* loA = net_find_ns(A, "lo");
    if (loA) loA->admin_down = 0;
    T6(a6_is_ll(va->ip6_ll) && va->ip6_ll[11] == 0xff && va->ip6_ll[12] == 0xfe, "a link-local address from the MAC");
    T6(net_ping6(A, loA, IP6_LOOP, 1) == 1, "::1 answers in its namespace");
    T6(net_ping6(A, va, vb->ip6_ll, 2) == 2, "A pings B's link-local over the veth (NDP first)");
    net_parse_ip6("fd00:6::1", a, NULL, 0);
    net_parse_ip6("fd00:6::2", b, NULL, 0);
    net6_addr_add(va, a, 64);
    net6_addr_add(vb, b, 64);
    T6(net_ping6(B, NULL, a, 2) == 2, "B pings A's global address by route");
    T6(net_ping6(A, NULL, a, 1) == 1, "A's own global address goes round its loopback");
    uint8_t far[16]; net_parse_ip6("fd00:99::1", far, NULL, 0);
    T6(net6_route(B, far, NULL) == NULL, "no route to another prefix");
    uint8_t pfx[16]; net_parse_ip6("fd00:99::", pfx, NULL, 0);
    T6(net6_route_add(B, pfx, 64, a, vb) == 0 && net6_route(B, far, b) == vb && a6_eq(b, a),
       "a table route: fd00:99::/64 via A on v6b");
    T6(net6_route(C, a, NULL) == NULL && net_ping6(C, NULL, a, 1) <= 0, "a third namespace cannot reach A");
    net_ns_destroy(A);
    T6(net6_route(B, far, NULL) == NULL, "a device's end takes its routes");
    net_ns_destroy(B); net_ns_destroy(C);
    #undef T6
    kprintf("ip6: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(ip6test) = { "ip6test", "", "IPv6: parse/format, link-local, NDP, echo, routes, namespaces",
                       SHELL_G_TEST, cmd_ip6test, SHELL_P_ANY };

/* ---- `tcp6test` — §M90 3a stage c: TCP and UDP over IPv6 --------------------- */
struct u6rec { int got; uint8_t src[16]; uint16_t sport; char data[16]; };
static void u6_cb(uint32_t src_ip, uint16_t src_port, const uint8_t* data, uint32_t len, void* ctx) {
    struct u6rec* r = (struct u6rec*)ctx;
    const uint8_t* s6 = net_rx_src6();
    if (!s6 || src_ip) return;                       /* an IPv4 datagram: not this test's */
    a6_cpy(r->src, s6);
    r->sport = src_port;
    for (uint32_t i = 0; i < len && i < sizeof r->data - 1; i++) r->data[i] = ((const char*)data)[i];
    r->got++;
}
static int u6_cond(void* a) { return ((struct u6rec*)a)->got > 0; }

static int t6_talk(struct tcp_conn* c, struct tcp_conn* s, uint32_t bytes) {
    static uint8_t tx[4096], rx[4096];
    uint32_t sent = 0, got = 0, bad = 0;
    while (got < bytes) {
        if (sent < bytes) {
            uint32_t n = bytes - sent < sizeof tx ? bytes - sent : sizeof tx;
            for (uint32_t i = 0; i < n; i++) tx[i] = (uint8_t)(sent + i);
            int w = net_tcp_send(c, tx, n, 0);
            if (w < 0) return 0;
            sent += (uint32_t)w;
        }
        int r = net_tcp_recv(s, rx, sizeof rx, 0, 2000);
        if (r <= 0) return 0;
        for (int i = 0; i < r; i++) if (rx[i] != (uint8_t)(got + (uint32_t)i)) bad++;
        got += (uint32_t)r;
    }
    return bad == 0;
}

static void cmd_tcp6test(const char* args) {
    (void)args;
    int ok = 1;
    #define T6(cond, what) do { int _c = (cond); kprintf("tcp6: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    int A = net_ns_create(), B = net_ns_create();
    struct net_device *va = NULL, *vb = NULL;
    if (A <= 0 || B <= 0 || netvirt_veth_create(A, "t6a", B, "t6b", &va, &vb) != 0) {
        kprintf("tcp6: FAIL (setup)\n"); net_ns_destroy(A); net_ns_destroy(B); return;
    }
    va->admin_down = 0; vb->admin_down = 0;
    struct net_device* loA = net_find_ns(A, "lo");
    if (loA) loA->admin_down = 0;
    uint8_t a[16], b[16];
    net_parse_ip6("fd00:7::1", a, NULL, 0);
    net_parse_ip6("fd00:7::2", b, NULL, 0);
    net6_addr_add(va, a, 64);
    net6_addr_add(vb, b, 64);

    /* a DUAL-STACK listener on [::]:9000 in A */
    struct tcp_conn* l = net_tcp_listen6_ns(A, IP6_ZERO, 9000, 4, 0);
    T6(l != NULL, "listen on [::]:9000 (dual-stack)");
    struct tcp_conn* c = net_tcp_connect6_ns(A, NULL, IP6_LOOP, 9000, 1500);
    struct tcp_conn* s = c ? net_tcp_accept(l, 1500) : NULL;
    uint8_t pa[16]; uint16_t pp = 0;
    T6(c && s && net_tcp_peer6(s, pa, &pp) == 1 && a6_eq(pa, IP6_LOOP) && t6_talk(c, s, 4000),
       "connect to [::1]:9000, accepted as IPv6, data both ways");
    if (s) net_tcp_close(s);
    if (c) net_tcp_close(c);
    c = net_tcp_connect_ns(A, IPV4(127, 0, 0, 1), 9000, 1500);
    s = c ? net_tcp_accept(l, 1500) : NULL;
    T6(c && s && net_tcp_peer6(s, NULL, NULL) == 0, "an IPv4 connection to the dual-stack listener is accepted (as IPv4)");
    if (s) net_tcp_close(s);
    if (c) net_tcp_close(c);
    T6(net_tcp_listen_ns(A, 0, 9000, 4) == NULL, "an IPv4 listener cannot share the dual-stack port");

    /* v6only: the IPv4 port stays free */
    struct tcp_conn* l6 = net_tcp_listen6_ns(A, IP6_ZERO, 9001, 4, 1);
    struct tcp_conn* l4 = net_tcp_listen_ns(A, 0, 9001, 4);
    T6(l6 && l4, "a v6-only listener and an IPv4 one share port 9001");
    c = net_tcp_connect_ns(A, IPV4(127, 0, 0, 1), 9001, 1500);
    s = c ? net_tcp_accept(l4, 1500) : NULL;
    T6(c && s && net_tcp_pending(l6) == 0, "IPv4 to 9001 reaches the IPv4 listener, not the v6-only one");
    if (s) net_tcp_close(s);
    if (c) net_tcp_close(c);
    if (l4) net_tcp_close(l4);
    if (l6) net_tcp_close(l6);

    /* across the veth, by global address, enough data to need segmentation */
    c = net_tcp_connect6_ns(B, NULL, a, 9000, 1500);
    s = c ? net_tcp_accept(l, 1500) : NULL;
    T6(c && s && net_tcp_peer6(s, pa, NULL) == 1 && a6_eq(pa, b) && t6_talk(c, s, 32768),
       "B connects to [fd00:7::1]:9000, 32 KiB intact (MSS for a 40-byte header)");
    if (s) net_tcp_close(s);
    if (c) net_tcp_close(c);
    {
        uint64_t t0 = timer_ticks_ms();
        struct tcp_conn* r = net_tcp_connect6_ns(B, NULL, a, 9555, 5000);
        uint64_t dt = timer_ticks_ms() - t0;
        T6(r == NULL && dt < 1000, "a closed v6 port answers with an RST (refused well before the 5 s timeout)");
    }
    if (l) net_tcp_close(l);

    /* forwarding through a router namespace R: C (fd00:c::2) -- R -- D (fd00:d::2) */
    {
        int R = net_ns_create(), C = net_ns_create(), D = net_ns_create();
        struct net_device *rc = NULL, *cc = NULL, *rd = NULL, *dd = NULL;
        int okset = R > 0 && C > 0 && D > 0 &&
                    netvirt_veth_create(R, "rc", C, "eth0", &rc, &cc) == 0 &&
                    netvirt_veth_create(R, "rd", D, "eth0", &rd, &dd) == 0;
        uint8_t ca[16], ra1[16], da[16], ra2[16], z[16] = {0};
        net_parse_ip6("fd00:c::2", ca, NULL, 0); net_parse_ip6("fd00:c::1", ra1, NULL, 0);
        net_parse_ip6("fd00:d::2", da, NULL, 0); net_parse_ip6("fd00:d::1", ra2, NULL, 0);
        if (okset) {
            rc->admin_down = cc->admin_down = rd->admin_down = dd->admin_down = 0;
            net6_addr_add(cc, ca, 64); net6_addr_add(rc, ra1, 64);
            net6_addr_add(dd, da, 64); net6_addr_add(rd, ra2, 64);
            net6_route_add(C, z, 0, ra1, cc);
            net6_route_add(D, z, 0, ra2, dd);
        }
        int off = okset ? net_ping6(C, NULL, da, 1) : -1;
        net6_set_forwarding(R, 1);
        int on = okset ? net_ping6(C, NULL, da, 2) : -1;
        struct tcp_conn* lD = okset ? net_tcp_listen6_ns(D, z, 9200, 2, 1) : NULL;
        struct tcp_conn* c2 = lD ? net_tcp_connect6_ns(C, NULL, da, 9200, 1500) : NULL;
        struct tcp_conn* s2 = c2 ? net_tcp_accept(lD, 1500) : NULL;
        T6(okset && off <= 0, "with forwarding off, C cannot reach D through R");
        T6(on == 2 && c2 && s2 && t6_talk(c2, s2, 8000), "with forwarding on, ping and TCP cross R");
        if (s2) net_tcp_close(s2);
        if (c2) net_tcp_close(c2);
        if (lD) net_tcp_close(lD);
        net6_set_disabled(C, 1);
        struct net_device* loC = net_find_ns(C, "lo");
        if (loC) loC->admin_down = 0;
        T6(net_ping6(C, loC, IP6_LOOP, 1) <= 0, "disable_ipv6 = 1 silences the namespace (even ::1)");
        net6_set_disabled(C, 0);
        net6_set_forwarding(R, 0);
        net_ns_destroy(C); net_ns_destroy(D); net_ns_destroy(R);
    }

    /* UDP over v6, with the v6 source visible to the receiver */
    struct u6rec rec = { 0 };
    net_udp_bind_ns(A, 9100, u6_cb, &rec);
    int sr = net_udp_send6_ns(B, NULL, a, 4321, 9100, "udp6", 4);
    net_wait_cond(u6_cond, &rec, 1000);
    T6(sr == 0 && rec.got == 1 && a6_eq(rec.src, b) && rec.sport == 4321 && rec.data[0] == 'u',
       "a UDP datagram from B arrives with its IPv6 source");
    net_udp_unbind_ns(A, 9100);

    net_ns_destroy(A); net_ns_destroy(B);
    #undef T6
    kprintf("tcp6: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(tcp6test) = { "tcp6test", "", "TCP and UDP over IPv6: dual-stack, v6only, segmentation, RST, UDP source",
                        SHELL_G_TEST, cmd_tcp6test, SHELL_P_ANY };
