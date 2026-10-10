/* =============================================================================
 * netlink.c — AF_NETLINK / NETLINK_ROUTE ("rtnetlink"), §M90.
 *
 * WHY.  On Linux a program does not read the network configuration out of
 * /proc: it asks the kernel over a netlink socket.  dockerd's network
 * controller opens one on the host namespace at start-up — with
 * --bridge=none too — and panics if it cannot ("could not create netlink
 * handle on initial (host) namespace").  ip(8), Go's net.Interfaces and
 * every container runtime speak the same protocol.
 *
 * WHAT IT ANSWERS.  Requests are a stream of nlmsghdr's; each is answered
 * synchronously, its replies queued as ONE datagram on the socket, read back
 * by recv/recvfrom/read:
 *   RTM_GETLINK  dump, or one device by index / IFLA_IFNAME
 *   RTM_GETADDR  dump, IPv4 and (§M90 3a) IPv6: each device's link-local and
 *                configured addresses; NEW/DELADDR for both families
 *   RTM_GETROUTE dump (the connected route of every configured device and
 *                the default route via its gateway — net_route's own table)
 *   RTM_GETNEIGH / GETRULE / GETQDISC dumps: empty (DONE)
 *   RTM_SETLINK / RTM_NEWLINK without NLM_F_CREATE that only ask an existing
 *                device to be UP: acknowledged (they are)
 * Everything else that would CHANGE the configuration (new links, addresses,
 * routes — a bridge, a veth pair) is refused with EOPNOTSUPP and named in the
 * answer, not acknowledged: pretending a veth was made would fail later, far
 * from the cause.  That is §M90 rung 5.
 *
 * The devices and their numbers are the stack's own (net_for_each): what
 * netlink reports and what the stack routes with cannot disagree.  Interface
 * indexes are registry order from 1.
 *
 * Unsolicited messages (multicast groups: link/address change events) are
 * accepted at bind and never sent — nothing here changes the configuration
 * behind a program's back yet.
 * ============================================================================= */

#include "netlink.h"
#include "net.h"
#include "kmalloc.h"
#include "waitq.h"
#include "task.h"
#include "fd.h"
#include "printf.h"
#include "nsproxy.h"
#include "syscall.h"
#include <stddef.h>
#include <stdint.h>

#define NL_MAXQ   16                 /* queued reply datagrams per socket */
#define NL_DGRAM  16384              /* one reply datagram's capacity     */

struct nl_dgram { uint32_t len; uint8_t* data; };

struct nlsock {
    int          proto;
    uint32_t     portid;
    uint32_t     groups;
    struct waitq wq;                 /* its lock guards the queue          */
    struct nl_dgram q[NL_MAXQ];
    int          head, count;
    struct ofile* owner;
    int          netns;              /* §M90 rung 5 — the namespace it was made in */
};

/* ---- message building ------------------------------------------------------ */

struct nlb { uint8_t* p; uint32_t n, cap; int overflow; };

static void* nlb_put(struct nlb* b, uint32_t len) {
    uint32_t a = (len + 3u) & ~3u;
    if (b->n + a > b->cap) { b->overflow = 1; return NULL; }
    uint8_t* r = b->p + b->n;
    for (uint32_t i = 0; i < a; i++) r[i] = 0;
    b->n += a;
    return r;
}
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t r32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Start a message; returns its offset (the length is fixed by nl_end). */
static uint32_t nl_begin(struct nlb* b, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    uint32_t at = b->n;
    uint8_t* h = (uint8_t*)nlb_put(b, 16);
    if (!h) return at;
    w16(h + 4, type); w16(h + 6, flags); w32(h + 8, seq); w32(h + 12, pid);
    return at;
}
static void nl_end(struct nlb* b, uint32_t at) {
    if (!b->overflow) w32(b->p + at, b->n - at);
}
static void nl_attr(struct nlb* b, uint16_t type, const void* data, uint16_t len) {
    uint8_t* a = (uint8_t*)nlb_put(b, 4u + len);
    if (!a) return;
    w16(a, (uint16_t)(4 + len)); w16(a + 2, type);
    for (uint16_t i = 0; i < len; i++) a[4 + i] = ((const uint8_t*)data)[i];
}
static void nl_attr32(struct nlb* b, uint16_t type, uint32_t v) { uint8_t t[4]; w32(t, v); nl_attr(b, type, t, 4); }
static void nl_attr8(struct nlb* b, uint16_t type, uint8_t v) { nl_attr(b, type, &v, 1); }
static void nl_attr_be32(struct nlb* b, uint16_t type, uint32_t host) {
    uint8_t t[4] = { (uint8_t)(host >> 24), (uint8_t)(host >> 16), (uint8_t)(host >> 8), (uint8_t)host };
    nl_attr(b, type, t, 4);
}

/* ---- the devices --------------------------------------------------------------- */

#define NLMSG_ERROR 2
#define NLMSG_DONE  3
#define F_MULTI  0x2
#define F_ACK    0x4
#define F_DUMP   0x300
#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_SETLINK 19
#define RTM_NEWADDR 20
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26
#define AF_INET_  2
#define AF_INET6_ 10

struct devlist { struct net_device* d[64]; int n; int ns; };
static void dev_collect(struct net_device* dev, void* ctx) {
    struct devlist* l = (struct devlist*)ctx;
    if (l->n < 64 && !(dev->flags & NETDEV_F_SIMULATED) && dev->netns == l->ns) l->d[l->n++] = dev;
}
static int is_lo(const struct net_device* d) { return (d->flags & NETDEV_F_LOOPBACK) != 0; }
static int prefix_of(uint32_t mask) { int n = 0; while (mask & 0x80000000u) { n++; mask <<= 1; } return n; }
static uint32_t mask_of(int len) { return len <= 0 ? 0 : (len >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - len)) - 1)); }
static uint32_t be32_host(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static uint32_t if_flags(const struct net_device* d) {
    uint32_t f = 0;
    if (!d->admin_down) f |= 0x1 | 0x40;              /* IFF_UP | IFF_RUNNING */
    if (is_lo(d)) f |= 0x8;                           /* IFF_LOOPBACK */
    else          f |= 0x2 | 0x1000;                  /* IFF_BROADCAST | IFF_MULTICAST */
    if (d->link && !d->link((struct net_device*)d)) f &= ~0x40u;
    return f;
}

static const char* kind_name(const struct net_device* d) {
    switch (d->kind) {
    case NETDEV_K_VETH:   return "veth";
    case NETDEV_K_BRIDGE: return "bridge";
    default:              return NULL;
    }
}

static void put_link(struct nlb* b, const struct net_device* d, uint16_t flags,
                     uint32_t seq, uint32_t pid) {
    uint32_t at = nl_begin(b, RTM_NEWLINK, flags, seq, pid);
    uint8_t* ifi = (uint8_t*)nlb_put(b, 16);
    if (ifi) {
        w16(ifi + 2, is_lo(d) ? 772 : 1);             /* ARPHRD_LOOPBACK / ETHER */
        w32(ifi + 4, (uint32_t)d->ifindex);
        w32(ifi + 8, if_flags(d));
    }
    const char* nm = d->name ? d->name : "eth";
    uint16_t nl = 0; while (nm[nl]) nl++;
    char name[17]; for (uint16_t i = 0; i <= nl && i < 16; i++) name[i] = nm[i]; name[16] = 0;
    nl_attr(b, 3 /* IFLA_IFNAME */, name, (uint16_t)(nl + 1));
    nl_attr32(b, 4 /* IFLA_MTU */, d->mtu ? d->mtu : (is_lo(d) ? 65536 : 1500));
    nl_attr32(b, 13 /* IFLA_TXQLEN */, is_lo(d) ? 0 : 1000);
    nl_attr8(b, 16 /* IFLA_OPERSTATE */, d->admin_down ? 2 /* DOWN */ : (is_lo(d) ? 0 : 6 /* UP */));
    nl_attr8(b, 17 /* IFLA_LINKMODE */, 0);
    nl_attr32(b, 27 /* IFLA_GROUP */, 0);
    nl_attr32(b, 30 /* IFLA_PROMISCUITY */, 0);
    uint8_t mac[6], bc[6];
    for (int i = 0; i < 6; i++) { mac[i] = is_lo(d) ? 0 : d->mac[i]; bc[i] = is_lo(d) ? 0 : 0xFF; }
    nl_attr(b, 1 /* IFLA_ADDRESS */, mac, 6);
    nl_attr(b, 2 /* IFLA_BROADCAST */, bc, 6);
    if (d->master) nl_attr32(b, 10 /* IFLA_MASTER */, (uint32_t)d->master->ifindex);
    /* §M90 rung 5 — what kind of link it is (Docker checks that docker0 is a
     * "bridge" before it reuses it): IFLA_LINKINFO { IFLA_INFO_KIND }. */
    const char* k = kind_name(d);
    if (k) {
        uint32_t li = b->n;
        uint8_t* hdr = (uint8_t*)nlb_put(b, 4);
        uint16_t kl = 0; while (k[kl]) kl++;
        nl_attr(b, 1 /* IFLA_INFO_KIND */, k, (uint16_t)(kl + 1));
        if (hdr && !b->overflow) { w16(hdr, (uint16_t)(b->n - li)); w16(hdr + 2, 18 /* IFLA_LINKINFO */); }
    }
    nl_end(b, at);
}

static void put_addr(struct nlb* b, const struct net_device* d, uint32_t seq, uint32_t pid) {
    if (!d->ip) return;
    uint32_t at = nl_begin(b, RTM_NEWADDR, F_MULTI, seq, pid);
    uint8_t* ifa = (uint8_t*)nlb_put(b, 8);
    if (ifa) {
        ifa[0] = AF_INET_;
        ifa[1] = (uint8_t)prefix_of(d->netmask);
        ifa[2] = 0x80;                                /* IFA_F_PERMANENT */
        ifa[3] = is_lo(d) ? 254 : 0;                  /* RT_SCOPE_HOST / UNIVERSE */
        w32(ifa + 4, (uint32_t)d->ifindex);
    }
    nl_attr_be32(b, 1 /* IFA_ADDRESS */, d->ip);
    nl_attr_be32(b, 2 /* IFA_LOCAL */, d->ip);
    if (!is_lo(d)) nl_attr_be32(b, 4 /* IFA_BROADCAST */, d->ip | ~d->netmask);
    const char* nm = d->name ? d->name : "eth";
    uint16_t nl = 0; while (nm[nl]) nl++;
    nl_attr(b, 3 /* IFA_LABEL */, nm, (uint16_t)(nl + 1));
    nl_end(b, at);
}

/* §M90 3a stage e — IPv6 addresses: the link-local of every non-loopback
 * device (scope link) and each configured one (scope universe; ::1 host). */
static void put_addr6_one(struct nlb* b, const struct net_device* d, const uint8_t* a, int plen,
                          int scope, uint32_t seq, uint32_t pid) {
    uint32_t at = nl_begin(b, RTM_NEWADDR, F_MULTI, seq, pid);
    uint8_t* ifa = (uint8_t*)nlb_put(b, 8);
    if (ifa) {
        ifa[0] = AF_INET6_; ifa[1] = (uint8_t)plen;
        ifa[2] = 0x80;                                /* IFA_F_PERMANENT */
        ifa[3] = (uint8_t)scope;
        w32(ifa + 4, (uint32_t)d->ifindex);
    }
    nl_attr(b, 1 /* IFA_ADDRESS */, a, 16);
    nl_end(b, at);
}
static void put_addr6(struct nlb* b, const struct net_device* d, uint32_t seq, uint32_t pid) {
    if (d->ip6_disabled) return;                       /* disable_ipv6: no v6 addresses */
    if (!is_lo(d)) put_addr6_one(b, d, d->ip6_ll, 64, 253 /* RT_SCOPE_LINK */, seq, pid);
    for (int i = 0; i < d->n_ip6; i++)
        put_addr6_one(b, d, d->ip6[i], d->ip6_plen[i], is_lo(d) ? 254 : 0, seq, pid);
}
static void put_route6(struct nlb* b, const uint8_t* dst, int dst_len, const uint8_t* gw,
                       int oif, int scope, uint32_t seq, uint32_t pid) {
    uint32_t at = nl_begin(b, RTM_NEWROUTE, F_MULTI, seq, pid);
    uint8_t* rt = (uint8_t*)nlb_put(b, 12);
    int has_gw = 0;
    if (gw) for (int i = 0; i < 16; i++) if (gw[i]) has_gw = 1;
    if (rt) {
        rt[0] = AF_INET6_; rt[1] = (uint8_t)dst_len;
        rt[4] = 254; rt[5] = has_gw ? 3 : 2; rt[6] = (uint8_t)scope; rt[7] = 1;
    }
    nl_attr32(b, 15 /* RTA_TABLE */, 254);
    if (dst_len) nl_attr(b, 1 /* RTA_DST */, dst, 16);
    if (has_gw) nl_attr(b, 5 /* RTA_GATEWAY */, gw, 16);
    nl_attr32(b, 4 /* RTA_OIF */, (uint32_t)oif);
    nl_attr32(b, 6 /* RTA_PRIORITY */, 256);
    nl_end(b, at);
}
static void prefix6(const uint8_t* a, int plen, uint8_t* out) {
    for (int i = 0; i < 16; i++) {
        int bits = plen - 8 * i;
        out[i] = bits >= 8 ? a[i] : bits <= 0 ? 0 : (uint8_t)(a[i] & (0xff << (8 - bits)));
    }
}

static void put_route(struct nlb* b, uint32_t dst, int dst_len, uint32_t gw, uint32_t src,
                      int oif, int scope, uint32_t seq, uint32_t pid) {
    uint32_t at = nl_begin(b, RTM_NEWROUTE, F_MULTI, seq, pid);
    uint8_t* rt = (uint8_t*)nlb_put(b, 12);
    if (rt) {
        rt[0] = AF_INET_; rt[1] = (uint8_t)dst_len;
        rt[4] = 254;                                  /* RT_TABLE_MAIN */
        rt[5] = gw ? 3 /* RTPROT_BOOT */ : 2 /* RTPROT_KERNEL */;
        rt[6] = (uint8_t)scope;
        rt[7] = 1;                                    /* RTN_UNICAST */
    }
    nl_attr32(b, 15 /* RTA_TABLE */, 254);
    if (dst_len) nl_attr_be32(b, 1 /* RTA_DST */, dst);
    if (gw) nl_attr_be32(b, 5 /* RTA_GATEWAY */, gw);
    if (src) nl_attr_be32(b, 7 /* RTA_PREFSRC */, src);
    nl_attr32(b, 4 /* RTA_OIF */, (uint32_t)oif);
    nl_end(b, at);
}

static void put_done(struct nlb* b, uint32_t seq, uint32_t pid) {
    uint32_t at = nl_begin(b, NLMSG_DONE, F_MULTI, seq, pid);
    nlb_put(b, 4);
    nl_end(b, at);
}
static void put_error(struct nlb* b, int err, const uint8_t* req, uint32_t pid) {
    uint32_t at = nl_begin(b, NLMSG_ERROR, 0, r32(req + 8), pid);
    uint8_t* e = (uint8_t*)nlb_put(b, 4 + 16);
    if (e) { w32(e, (uint32_t)err); for (int i = 0; i < 16; i++) e[4 + i] = req[i]; }
    nl_end(b, at);
}

/* Find an attribute in a request's attribute area. */
static const uint8_t* find_attr(const uint8_t* a, uint32_t len, uint16_t type, uint16_t* alen) {
    while (len >= 4) {
        uint16_t l = r16(a), t = r16(a + 2);
        if (l < 4 || l > len) return NULL;
        if ((t & 0x3FFF) == type) { *alen = (uint16_t)(l - 4); return a + 4; }
        uint32_t step = (l + 3u) & ~3u;
        if (step >= len) return NULL;
        a += step; len -= step;
    }
    return NULL;
}
static int attr_str(const uint8_t* a, uint16_t al, char* out, int cap) {
    int i = 0;
    for (; i < al && i + 1 < cap && a[i]; i++) out[i] = (char)a[i];
    out[i] = 0;
    return i;
}

/* ---- §M90 rung 5 — configuration ------------------------------------------------ */

#define RTM_DELLINK  17
#define RTM_DELADDR  21
#define RTM_DELROUTE 25
#define IFF_UP_      0x1u

struct nsobj;
int procfs_ns_handle(struct file* f, int* kind, struct nsobj** obj, int* mnt, uint32_t* ino);

/* The network namespace an IFLA_NET_NS_FD / IFLA_NET_NS_PID names, or -1. */
static int ns_from_attrs(const uint8_t* attrs, uint32_t alen) {
    uint16_t l = 0;
    const uint8_t* v = find_attr(attrs, alen, 28 /* IFLA_NET_NS_FD */, &l);
    if (v && l >= 4) {
        struct file* f = sys_fd_vfs_file((int)r32(v));
        int kind; struct nsobj* o = NULL;
        if (!f || !procfs_ns_handle(f, &kind, &o, NULL, NULL) || kind != NSK_NET) return -1;
        return o ? o->net_id : 0;
    }
    v = find_attr(attrs, alen, 19 /* IFLA_NET_NS_PID */, &l);
    if (v && l >= 4) {
        struct task* me = task_current();
        int g = ns_pid_resolve(me, (int)r32(v));
        struct task* t = g > 0 ? task_find(g) : NULL;
        return t ? ns_net_id(t) : -1;
    }
    return -2;                                         /* not given */
}

/* SETLINK / NEWLINK on an existing device: the parts we can apply. */
static int link_apply(struct net_device* d, uint32_t ifl, uint32_t change,
                      const uint8_t* attrs, uint32_t alen) {
    uint16_t l = 0;
    const uint8_t* v;
    if ((v = find_attr(attrs, alen, 3 /* IFLA_IFNAME */, &l))) {
        char nm[16];
        attr_str(v, l, nm, sizeof nm);
        const char* cur = d->name ? d->name : "";
        int same = 1; for (int i = 0; nm[i] || cur[i]; i++) if (nm[i] != cur[i]) { same = 0; break; }
        if (!same && netvirt_rename(d, nm) != 0) return netvirt_is_virtual(d) ? -17 : -95;
    }
    if ((v = find_attr(attrs, alen, 4 /* IFLA_MTU */, &l)) && l >= 4) {
        uint32_t mtu = r32(v);
        if (mtu < 68 || mtu > ETH_MTU) return -22;     /* frames here are at most 1500 */
        d->mtu = mtu;
    }
    if ((v = find_attr(attrs, alen, 1 /* IFLA_ADDRESS */, &l)) && l == 6 && netvirt_is_virtual(d))
        for (int i = 0; i < 6; i++) d->mac[i] = v[i];
    if ((v = find_attr(attrs, alen, 10 /* IFLA_MASTER */, &l)) && l >= 4) {
        uint32_t mi = r32(v);
        struct net_device* br = mi ? net_by_index(d->netns, (int)mi) : NULL;
        if (mi && !br) return -19;
        if (netvirt_set_master(d, br) != 0) return -22;
    }
    int nsid = ns_from_attrs(attrs, alen);
    if (nsid == -1) return -22;
    if (nsid >= 0 && nsid != d->netns) {
        if (!netvirt_is_virtual(d)) return -95;        /* hardware stays where it is */
        if (net_find_ns(nsid, d->name)) return -17;
        if (net_dev_set_ns(d, nsid) != 0) return -22;
    }
    if (change & IFF_UP_) d->admin_down = (ifl & IFF_UP_) ? 0 : 1;
    else if (!change && (ifl & IFF_UP_)) d->admin_down = 0;     /* old-style: flags only */
    return 0;
}

/* RTM_NEWLINK with NLM_F_CREATE: a bridge or a veth pair. */
static int link_create(int ns, uint32_t ifl, const uint8_t* attrs, uint32_t alen) {
    uint16_t l = 0;
    char name[16] = "";
    const uint8_t* v = find_attr(attrs, alen, 3 /* IFLA_IFNAME */, &l);
    if (v) attr_str(v, l, name, sizeof name);
    const uint8_t* li = find_attr(attrs, alen, 18 /* IFLA_LINKINFO */, &l);
    if (!li) return -22;
    uint16_t ll = l, kl = 0;
    const uint8_t* kv = find_attr(li, ll, 1 /* IFLA_INFO_KIND */, &kl);
    char kind[16] = "";
    if (kv) attr_str(kv, kl, kind, sizeof kind);
    static uint32_t auto_n;
    if (!name[0]) {                                     /* the kernel picks one */
        const char* pre = kind[0] == 'b' ? "br" : "veth";
        int o = 0; for (; pre[o]; o++) name[o] = pre[o];
        uint32_t n = auto_n++; char d[8]; int k = 0;
        do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n);
        while (k) name[o++] = d[--k];
        name[o] = 0;
    }
    struct net_device* made = NULL;
    if (kind[0] == 'b' && kind[1] == 'r') {
        made = netvirt_bridge_create(ns, name);
        if (!made) return -17;
    } else if (kind[0] == 'v' && kind[1] == 'e' && kind[2] == 't' && kind[3] == 'h') {
        /* IFLA_INFO_DATA { VETH_INFO_PEER { ifinfomsg, attrs… } } */
        char pname[16] = "";
        int pns = ns;
        uint16_t dl = 0, pl = 0;
        const uint8_t* data = find_attr(li, ll, 2 /* IFLA_INFO_DATA */, &dl);
        const uint8_t* peer = data ? find_attr(data, dl, 1 /* VETH_INFO_PEER */, &pl) : NULL;
        if (peer && pl >= 16) {
            const uint8_t* pa = peer + 16;
            uint32_t pal = pl - 16u;
            uint16_t nl2 = 0;
            const uint8_t* pn = find_attr(pa, pal, 3, &nl2);
            if (pn) attr_str(pn, nl2, pname, sizeof pname);
            int x = ns_from_attrs(pa, pal);
            if (x == -1) return -22;
            if (x >= 0) pns = x;
        }
        if (!pname[0]) {
            const char* pre = "veth";
            int o = 0; for (; pre[o]; o++) pname[o] = pre[o];
            uint32_t n = auto_n++; char d[8]; int k = 0;
            do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n);
            while (k) pname[o++] = d[--k];
            pname[o] = 0;
        }
        struct net_device* b2 = NULL;
        int r = netvirt_veth_create(ns, name, pns, pname, &made, &b2);
        if (r == -2) return -17;
        if (r != 0) return -12;
    } else {
        kprintf("netlink: RTM_NEWLINK kind '%s' refused - only bridge and veth exist here\n", kind);
        return -95;
    }
    /* the rest of the request (MTU, master, address, up) applies to it */
    return link_apply(made, ifl, ifl & IFF_UP_, attrs, alen) == 0 ? 0 : 0;
}

static int addr_change(int ns, int add, const uint8_t* m, uint32_t len, uint16_t nlflags) {
    if (len < 24) return -22;
    if (m[16] == AF_INET6_) {                         /* §M90 3a stage e */
        struct net_device* d6 = net_by_index(ns, (int)r32(m + 20));
        if (!d6) return -19;
        uint16_t l6 = 0;
        const uint8_t* v6 = find_attr(m + 24, len - 24, 2 /* IFA_LOCAL */, &l6);
        if (!v6) v6 = find_attr(m + 24, len - 24, 1 /* IFA_ADDRESS */, &l6);
        if (!v6 || l6 < 16) return -22;
        int r6 = add ? net6_addr_add(d6, v6, m[17]) : net6_addr_del(d6, v6);
        if (r6 == -17 && add && !(nlflags & 0x200)) r6 = 0;   /* not EXCL: replacing is fine */
        return r6;
    }
    if (m[16] != AF_INET_) return -97;
    int plen = m[17];
    struct net_device* d = net_by_index(ns, (int)r32(m + 20));
    if (!d) return -19;
    uint16_t l = 0;
    const uint8_t* v = find_attr(m + 24, len - 24, 2 /* IFA_LOCAL */, &l);
    if (!v) v = find_attr(m + 24, len - 24, 1 /* IFA_ADDRESS */, &l);
    if (!v || l < 4) return -22;
    uint32_t ip = be32_host(v);
    uint32_t f = net_lock();
    int r = 0;
    if (add) {
        if (d->ip == ip && (nlflags & 0x200 /* EXCL */)) r = -17;
        else if (d->ip && d->ip != ip && !is_lo(d)) r = -95;   /* one address per device */
        else { d->ip = ip; d->netmask = mask_of(plen); d->config_src = NETCFG_STATIC; }
    } else {
        if (d->ip != ip) r = -99;                              /* EADDRNOTAVAIL */
        else { d->ip = 0; d->netmask = 0; }
    }
    net_unlock(f);
    return r;
}

static int route_change(int ns, int add, const uint8_t* m, uint32_t len) {
    if (len < 28) return -22;
    if (m[16] == AF_INET6_) {                         /* §M90 3a stage e */
        uint8_t dst6[16] = {0}, gw6[16] = {0};
        int oif6 = 0;
        uint16_t l6 = 0;
        const uint8_t* v6;
        const uint8_t* a6 = m + 28; uint32_t al6 = len - 28;
        if ((v6 = find_attr(a6, al6, 1, &l6)) && l6 >= 16) for (int i = 0; i < 16; i++) dst6[i] = v6[i];
        if ((v6 = find_attr(a6, al6, 5, &l6)) && l6 >= 16) for (int i = 0; i < 16; i++) gw6[i] = v6[i];
        if ((v6 = find_attr(a6, al6, 4, &l6)) && l6 >= 4) oif6 = (int)r32(v6);
        struct net_device* d6 = oif6 ? net_by_index(ns, oif6) : NULL;
        if (oif6 && !d6) return -19;
        int has_gw = 0;
        for (int i = 0; i < 16; i++) if (gw6[i]) has_gw = 1;
        if (!d6 && has_gw && !net6_is_linklocal(gw6)) d6 = net6_route(ns, gw6, NULL);
        if (!add) return net6_route_del(ns, dst6, m[17]) == 0 ? 0 : -3;
        if (!d6) return has_gw ? -101 : -22;
        /* a connected prefix of the device exists already */
        if (!has_gw) for (int i = 0; i < d6->n_ip6; i++) {
            uint8_t p1[16], p2[16];
            prefix6(d6->ip6[i], d6->ip6_plen[i], p1); prefix6(dst6, m[17], p2);
            int same = d6->ip6_plen[i] == m[17];
            for (int k = 0; k < 16 && same; k++) if (p1[k] != p2[k]) same = 0;
            if (same) return 0;
        }
        int r6 = net6_route_add(ns, dst6, m[17], gw6, d6);
        return r6 == 0 ? 0 : r6;
    }
    if (m[16] != AF_INET_) return -97;
    int dlen = m[17];
    uint32_t dst = 0, gw = 0, metric = 0;
    int oif = 0;
    uint16_t l = 0;
    const uint8_t* v;
    const uint8_t* a = m + 28; uint32_t al = len - 28;
    if ((v = find_attr(a, al, 1 /* RTA_DST */, &l)) && l >= 4) dst = be32_host(v);
    if ((v = find_attr(a, al, 5 /* RTA_GATEWAY */, &l)) && l >= 4) gw = be32_host(v);
    if ((v = find_attr(a, al, 4 /* RTA_OIF */, &l)) && l >= 4) oif = (int)r32(v);
    if ((v = find_attr(a, al, 6 /* RTA_PRIORITY */, &l)) && l >= 4) metric = r32(v);
    struct net_device* d = oif ? net_by_index(ns, oif) : NULL;
    if (oif && !d) return -19;
    if (!d && gw) {                                   /* the device that reaches the gateway */
        uint32_t nh = 0;
        d = net_route_ns(ns, gw, &nh);
        if (d && nh != gw) d = NULL;                  /* not on-link: Linux says ENETUNREACH */
        if (!d) return -101;
    }
    if (!add) return net_route_del(ns, dst, mask_of(dlen), d) == 0 ? 0 : -3;   /* ESRCH */
    if (!d) return -22;
    /* The connected route of a device's own subnet exists already. */
    if (!gw && d->ip && mask_of(dlen) == d->netmask && (dst & d->netmask) == (d->ip & d->netmask)) return 0;
    int r = net_route_add(ns, dst, mask_of(dlen), gw, d, metric);
    return r == 0 ? 0 : (r == -2 ? -17 : -28);
}

/* ---- one request -------------------------------------------------------------- */

static void handle(struct nlsock* s, const uint8_t* m, uint32_t len, struct nlb* b) {
    uint16_t type = r16(m + 4), flags = r16(m + 6);
    uint32_t seq = r32(m + 8), pid = s->portid;
    int dump = (flags & F_DUMP) == F_DUMP;
    struct devlist l;
    l.n = 0; l.ns = s->netns;
    net_for_each(dev_collect, &l);
    uint8_t family = len > 16 ? m[16] : 0;

    switch (type) {
    case RTM_GETLINK:
        if (dump) {
            for (int i = 0; i < l.n; i++) put_link(b, l.d[i], F_MULTI, seq, pid);
            put_done(b, seq, pid);
            return;
        } else {
            int want = len >= 32 ? (int)r32(m + 20) : 0;
            uint16_t al = 0;
            const uint8_t* nm = len > 32 ? find_attr(m + 32, len - 32, 3, &al) : NULL;
            for (int i = 0; i < l.n; i++) {
                int match = want ? (want == l.d[i]->ifindex) : 0;
                if (!want && nm) {
                    const char* dn = l.d[i]->name ? l.d[i]->name : "";
                    match = 1;
                    for (uint16_t k = 0; k < al; k++) {
                        if (nm[k] == 0 && dn[k] == 0) break;
                        if (nm[k] != (uint8_t)dn[k]) { match = 0; break; }
                    }
                }
                if (match) {
                    put_link(b, l.d[i], 0, seq, pid);
                    if (flags & F_ACK) put_error(b, 0, m, pid);
                    return;
                }
            }
            put_error(b, -19 /* ENODEV */, m, pid);
            return;
        }
    case RTM_GETADDR:
        if (family != AF_INET6_)
            for (int i = 0; i < l.n; i++) put_addr(b, l.d[i], seq, pid);
        if (family != AF_INET_)                          /* §M90 3a stage e */
            for (int i = 0; i < l.n; i++) put_addr6(b, l.d[i], seq, pid);
        put_done(b, seq, pid);
        return;
    case RTM_GETROUTE:
        /* §M90 rung 5 — WITHOUT NLM_F_DUMP this is a route LOOKUP ("ip route
         * get", netlink.RouteGet): the one route that carries RTA_DST, as the
         * kernel would send the packet.  Answering it with the whole table
         * made Docker take the FIRST entry's device — the loopback's — for a
         * container's default gateway ("default via 172.17.0.1 dev lo"). */
        if (!dump && family == AF_INET6_) {             /* §M90 3a stage e — a v6 lookup */
            uint8_t dst6[16] = {0}, nh6[16];
            uint16_t al = 0;
            const uint8_t* v = len > 28 ? find_attr(m + 28, len - 28, 1, &al) : NULL;
            if (v && al >= 16) for (int i = 0; i < 16; i++) dst6[i] = v[i];
            struct net_device* d = net6_route(s->netns, dst6, nh6);
            if (!d) { put_error(b, -101, m, pid); return; }
            uint32_t at = nl_begin(b, RTM_NEWROUTE, 0, seq, pid);
            uint8_t* rt = (uint8_t*)nlb_put(b, 12);
            if (rt) { rt[0] = AF_INET6_; rt[1] = 128; rt[4] = 254; rt[5] = 2; rt[7] = is_lo(d) ? 2 : 1; }
            nl_attr32(b, 15, 254);
            nl_attr(b, 1, dst6, 16);
            nl_attr32(b, 4, (uint32_t)d->ifindex);
            int same = 1;
            for (int i = 0; i < 16; i++) if (nh6[i] != dst6[i]) same = 0;
            if (!same) nl_attr(b, 5, nh6, 16);
            nl_end(b, at);
            if (flags & F_ACK) put_error(b, 0, m, pid);
            return;
        }
        if (!dump) {
            uint32_t dst = 0;
            uint16_t al = 0;
            const uint8_t* v = len > 28 ? find_attr(m + 28, len - 28, 1 /* RTA_DST */, &al) : NULL;
            if (v && al >= 4) dst = be32_host(v);
            uint32_t nh = 0;
            struct net_device* d = net_route_ns(s->netns, dst, &nh);
            if (!d) { put_error(b, -101 /* ENETUNREACH */, m, pid); return; }
            uint32_t at = nl_begin(b, RTM_NEWROUTE, 0, seq, pid);
            uint8_t* rt = (uint8_t*)nlb_put(b, 12);
            if (rt) {
                rt[0] = AF_INET_; rt[1] = 32;
                rt[4] = 254;
                rt[5] = 2;
                rt[6] = 0;
                rt[7] = (d->flags & NETDEV_F_LOOPBACK) ? 2 /* RTN_LOCAL */ : 1 /* UNICAST */;
            }
            nl_attr32(b, 15 /* RTA_TABLE */, 254);
            nl_attr_be32(b, 1 /* RTA_DST */, dst);
            nl_attr32(b, 4 /* RTA_OIF */, (uint32_t)d->ifindex);
            if (nh != dst) nl_attr_be32(b, 5 /* RTA_GATEWAY */, nh);
            if (d->ip) nl_attr_be32(b, 7 /* RTA_PREFSRC */, d->ip);
            nl_end(b, at);
            if (flags & F_ACK) put_error(b, 0, m, pid);
            return;
        }
        if (family != AF_INET6_) {
            for (int i = 0; i < l.n; i++) {
                struct net_device* d = l.d[i];
                if (!d->ip || d->admin_down) continue;
                put_route(b, d->ip & d->netmask, prefix_of(d->netmask), 0, d->ip, d->ifindex,
                          is_lo(d) ? 254 : 253 /* HOST / LINK */, seq, pid);
            }
            struct net_rt r;
            int have_default = 0;
            for (int i = 0; net_route_at(i, &r) == 0; i++) {
                if (!r.used || r.ns != s->netns || !r.dev) continue;
                put_route(b, r.dst, prefix_of(r.mask), r.gw, 0, r.dev->ifindex, r.gw ? 0 : 253, seq, pid);
                if (!r.mask) have_default = 1;
            }
            for (int i = 0; i < l.n && !have_default; i++)
                if (l.d[i]->gateway && !is_lo(l.d[i])) {
                    put_route(b, 0, 0, l.d[i]->gateway, 0, l.d[i]->ifindex, 0, seq, pid);
                    break;
                }
        }
        if (family != AF_INET_) {                        /* §M90 3a stage e */
            static const uint8_t ll[16] = { 0xfe, 0x80 };
            for (int i = 0; i < l.n; i++) {
                struct net_device* d = l.d[i];
                if (d->admin_down) continue;
                if (!is_lo(d)) put_route6(b, ll, 64, NULL, d->ifindex, 0, seq, pid);
                for (int k = 0; k < d->n_ip6; k++) {
                    uint8_t p[16];
                    prefix6(d->ip6[k], d->ip6_plen[k], p);
                    put_route6(b, p, d->ip6_plen[k], NULL, d->ifindex, 0, seq, pid);
                }
            }
            struct net6_rt r6;
            for (int i = 0; net6_route_at(i, &r6) == 0; i++)
                if (r6.used && r6.ns == s->netns && r6.dev)
                    put_route6(b, r6.pfx, r6.plen, r6.gw, r6.dev->ifindex, 0, seq, pid);
        }
        put_done(b, seq, pid);
        return;
    case RTM_NEWLINK:
    case RTM_SETLINK: {
        if (len < 32) { put_error(b, -22, m, pid); return; }
        uint32_t idx = r32(m + 20);
        uint32_t ifl = r32(m + 24), change = r32(m + 28);
        const uint8_t* attrs = m + 32; uint32_t alen = len - 32;
        struct net_device* d = idx ? net_by_index(s->netns, (int)idx) : NULL;
        if (!d && !idx) {                             /* by name */
            uint16_t al = 0; char nm[16];
            const uint8_t* v = find_attr(attrs, alen, 3, &al);
            if (v) { attr_str(v, al, nm, sizeof nm); d = net_find_ns(s->netns, nm); }
        }
        int err;
        if (d) {
            err = (type == RTM_NEWLINK && (flags & 0x200 /* EXCL */) && (flags & 0x400)) ? -17
                : link_apply(d, ifl, change, attrs, alen);
        } else if (type == RTM_NEWLINK && (flags & 0x400 /* CREATE */)) {
            err = link_create(s->netns, ifl, attrs, alen);
        } else {
            err = -19;                                 /* ENODEV */
        }
        put_error(b, err, m, pid);
        return;
    }
    case RTM_DELLINK: {
        uint32_t idx = len >= 32 ? r32(m + 20) : 0;
        struct net_device* d = idx ? net_by_index(s->netns, (int)idx) : NULL;
        if (!d && len > 32) {
            uint16_t al = 0; char nm[16];
            const uint8_t* v = find_attr(m + 32, len - 32, 3, &al);
            if (v) { attr_str(v, al, nm, sizeof nm); d = net_find_ns(s->netns, nm); }
        }
        put_error(b, !d ? -19 : (netvirt_destroy(d) == 0 ? 0 : -95), m, pid);
        return;
    }
    case RTM_NEWADDR: case RTM_DELADDR:
        put_error(b, addr_change(s->netns, type == RTM_NEWADDR, m, len, flags), m, pid);
        return;
    case RTM_NEWROUTE: case RTM_DELROUTE:
        put_error(b, route_change(s->netns, type == RTM_NEWROUTE, m, len), m, pid);
        return;
    default:
        if (dump && (type & 3) == 2) {               /* any other GET dump: nothing to list */
            put_done(b, seq, pid);
            return;
        }
        kprintf("netlink: message type %u (flags %x) refused - not handled here\n", type, flags);
        put_error(b, -95, m, pid);
        return;
    }
}

/* ---- the socket ---------------------------------------------------------------- */

struct nlsock* nl_create(int proto) {
    if (proto != 0 /* NETLINK_ROUTE */) return NULL;
    struct nlsock* s = (struct nlsock*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    s->proto = proto;
    waitq_init(&s->wq);
    struct task* t = task_current();
    s->portid = t ? (uint32_t)task_tgid(t) : 1;
    s->netns  = net_cur_ns();       /* Linux: a socket belongs to its creator's namespace */
    return s;
}

void nl_set_owner(struct nlsock* s, struct ofile* o) { if (s) s->owner = o; }

void nl_close(struct nlsock* s) {
    if (!s) return;
    for (int i = 0; i < s->count; i++) kfree(s->q[(s->head + i) % NL_MAXQ].data);
    kfree(s);
}

int nl_bind(struct nlsock* s, uint32_t portid, uint32_t groups) {
    if (!s) return -9;
    if (portid) s->portid = portid;
    s->groups = groups;
    return 0;
}
uint32_t nl_portid(struct nlsock* s) { return s ? s->portid : 0; }

int nl_can_read(struct nlsock* s) { return s && s->count > 0; }

long nl_send(struct nlsock* s, const void* buf, size_t n) {
    if (!s) return -9;
    const uint8_t* m = (const uint8_t*)buf;
    struct nlb b = { (uint8_t*)kmalloc(NL_DGRAM), 0, NL_DGRAM, 0 };
    if (!b.p) return -12;
    size_t off = 0;
    while (off + 16 <= n) {
        uint32_t len = r32(m + off);
        if (len < 16 || off + len > n) break;
        handle(s, m + off, len, &b);
        off += (len + 3u) & ~3u;
    }
    if (b.overflow || b.n == 0) {
        kfree(b.p);
        return b.overflow ? -105 /* ENOBUFS */ : (long)n;
    }
    uint32_t f = waitq_lock(&s->wq);
    if (s->count == NL_MAXQ) { waitq_unlock(&s->wq, f); kfree(b.p); return -105; }
    s->q[(s->head + s->count) % NL_MAXQ].data = b.p;
    s->q[(s->head + s->count) % NL_MAXQ].len  = b.n;
    s->count++;
    waitq_wake_all(&s->wq);
    waitq_unlock(&s->wq, f);
    fd_readiness_changed(s->owner);
    return (long)n;
}

/* One datagram into `buf` (kernel memory).  `peek` leaves it queued; the
 * return is its FULL length when `trunc`, else what was copied — recvmsg's
 * MSG_PEEK|MSG_TRUNC, which is how a reader sizes its buffer first. */
long nl_recv(struct nlsock* s, void* buf, size_t n, int block, int peek, int trunc) {
    if (!s) return -9;
    uint32_t f = waitq_lock(&s->wq);
    while (s->count == 0) {
        if (!block) { waitq_unlock(&s->wq, f); return -11; }
        if (task_should_stop()) { waitq_unlock(&s->wq, f); return -4; }
        waitq_block(&s->wq);
    }
    struct nl_dgram d = s->q[s->head];
    size_t take = d.len < n ? d.len : n;
    for (size_t i = 0; i < take; i++) ((uint8_t*)buf)[i] = d.data[i];
    if (!peek) {
        s->head = (s->head + 1) % NL_MAXQ;
        s->count--;
        kfree(d.data);
    }
    waitq_unlock(&s->wq, f);
    return trunc ? (long)d.len : (long)take;
}

/* ---- `nltest` — every answer above checked against the stack's own view ---- */
#include "shellcmd.h"
#include "printf.h"
static int nlt_ask(struct nlsock* s, uint16_t type, uint16_t flags, uint8_t family,
                   uint8_t* rx, size_t cap) {
    uint8_t req[32];
    for (int i = 0; i < 32; i++) req[i] = 0;
    w32(req, 32); w16(req + 4, type); w16(req + 6, (uint16_t)(1 | flags)); w32(req + 8, 77);
    req[16] = family;
    if (nl_send(s, req, 32) != 32) return -1;
    return (int)nl_recv(s, rx, cap, 0, 0, 0);
}
static void cmd_nltest(const char* args) {
    (void)args;
    struct nlsock* s = nl_create(0);
    uint8_t* rx = (uint8_t*)kmalloc(NL_DGRAM);
    if (!s || !rx) { kprintf("nltest: FAIL (no socket)\n"); return; }
    struct devlist l;
    l.n = 0; l.ns = net_cur_ns();
    net_for_each(dev_collect, &l);
    int ok = 1;

    int n = nlt_ask(s, RTM_GETLINK, F_DUMP, 0, rx, NL_DGRAM);
    int links = 0, done = 0, named = 0;
    for (int off = 0; n > 0 && off + 16 <= n; ) {
        uint32_t len = r32(rx + off); uint16_t t = r16(rx + off + 4);
        if (len < 16) break;
        if (t == RTM_NEWLINK) {
            links++;
            uint16_t al; const uint8_t* nm = find_attr(rx + off + 32, len - 32, 3, &al);
            if (nm && al > 1) named++;
        }
        if (t == NLMSG_DONE) done = 1;
        off += (len + 3) & ~3u;
    }
    kprintf("nltest: GETLINK dump: %d link(s), %d named, DONE %s (stack has %d)\n",
            links, named, done ? "yes" : "NO", l.n);
    if (links != l.n || named != l.n || !done) ok = 0;

    n = nlt_ask(s, RTM_GETADDR, F_DUMP, AF_INET_, rx, NL_DGRAM);
    int addrs = 0, want = 0, match = 0;
    for (int i = 0; i < l.n; i++) if (l.d[i]->ip) want++;
    for (int off = 0; n > 0 && off + 16 <= n; ) {
        uint32_t len = r32(rx + off);
        if (len < 16) break;
        if (r16(rx + off + 4) == RTM_NEWADDR) {
            addrs++;
            uint32_t idx = r32(rx + off + 20);
            uint16_t al; const uint8_t* a = find_attr(rx + off + 24, len - 24, 2, &al);
            struct net_device* d = net_by_index(l.ns, (int)idx);
            if (a && al == 4 && d) {
                uint32_t ip = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
                if (ip == d->ip) match++;
            }
        }
        off += (len + 3) & ~3u;
    }
    kprintf("nltest: GETADDR dump: %d address(es), %d matching the stack (want %d)\n", addrs, match, want);
    if (addrs != want || match != want) ok = 0;

    n = nlt_ask(s, RTM_GETROUTE, F_DUMP, AF_INET_, rx, NL_DGRAM);
    int routes = 0, deflt = 0;
    for (int off = 0; n > 0 && off + 16 <= n; ) {
        uint32_t len = r32(rx + off);
        if (len < 16) break;
        if (r16(rx + off + 4) == RTM_NEWROUTE) { routes++; if (rx[off + 17] == 0) deflt++; }
        off += (len + 3) & ~3u;
    }
    int want_def = 0;
    for (int i = 0; i < l.n; i++) if (l.d[i]->gateway && !is_lo(l.d[i])) want_def = 1;
    kprintf("nltest: GETROUTE dump: %d route(s), default %d (want %d)\n", routes, deflt, want_def);
    if (deflt != want_def || routes < want) ok = 0;

    n = nlt_ask(s, RTM_NEWLINK, F_ACK | 0x400 /* CREATE */, 0, rx, NL_DGRAM);
    int err = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : 0;
    kprintf("nltest: NEWLINK|CREATE with no link kind -> error %d (want -22)\n", err);
    if (err != -22) ok = 0;

    /* §M90 rung 5 — a bridge made, addressed, read back and removed over
     * netlink: what Docker does to create docker0. */
    {
        uint8_t req[128];
        for (int i = 0; i < 128; i++) req[i] = 0;
        uint32_t o2 = 32;
        /* IFLA_IFNAME "nlbr0" */
        w16(req + o2, 4 + 6); w16(req + o2 + 2, 3); for (int i = 0; i < 6; i++) req[o2 + 4 + i] = (uint8_t)"nlbr0"[i]; o2 += 12;
        /* IFLA_LINKINFO { IFLA_INFO_KIND "bridge" } */
        w16(req + o2, 4 + 12); w16(req + o2 + 2, 18);
        w16(req + o2 + 4, 4 + 7); w16(req + o2 + 6, 1); for (int i = 0; i < 7; i++) req[o2 + 8 + i] = (uint8_t)"bridge"[i];
        o2 += 16;
        w32(req, o2); w16(req + 4, RTM_NEWLINK); w16(req + 6, 1 | F_ACK | 0x400 | 0x200); w32(req + 8, 90);
        w32(req + 24, 1); w32(req + 28, 1);                   /* IFF_UP */
        n = (nl_send(s, req, o2) == (long)o2) ? (int)nl_recv(s, rx, NL_DGRAM, 0, 0, 0) : -1;
        int mk = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
        struct net_device* br = net_find_ns(l.ns, "nlbr0");
        int again = -999;
        if (nl_send(s, req, o2) == (long)o2 && nl_recv(s, rx, NL_DGRAM, 0, 0, 0) >= 20) again = (int)r32(rx + 16);
        /* NEWADDR 10.77.0.1/24 */
        for (int i = 0; i < 128; i++) req[i] = 0;
        req[16] = AF_INET_; req[17] = 24; w32(req + 20, br ? (uint32_t)br->ifindex : 0);
        w16(req + 24, 8); w16(req + 26, 2); req[28] = 10; req[29] = 77; req[30] = 0; req[31] = 1;
        w32(req, 32); w16(req + 4, RTM_NEWADDR); w16(req + 6, 1 | F_ACK | 0x400); w32(req + 8, 91);
        n = (nl_send(s, req, 32) == 32) ? (int)nl_recv(s, rx, NL_DGRAM, 0, 0, 0) : -1;
        int ad = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
        int up = br && !br->admin_down && br->ip == IPV4(10, 77, 0, 1) && br->netmask == IPV4(255, 255, 255, 0);
        /* DELLINK by index */
        for (int i = 0; i < 32; i++) req[i] = 0;
        w32(req, 32); w16(req + 4, RTM_DELLINK); w16(req + 6, 1 | F_ACK); w32(req + 8, 92);
        w32(req + 20, br ? (uint32_t)br->ifindex : 0);
        n = (nl_send(s, req, 32) == 32) ? (int)nl_recv(s, rx, NL_DGRAM, 0, 0, 0) : -1;
        int del = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
        int gone = net_find_ns(l.ns, "nlbr0") == NULL;
        kprintf("nltest: bridge create %d, again %d (want -17), address %d, up+addressed %s, delete %d, gone %s\n",
                mk, again, ad, up ? "yes" : "NO", del, gone ? "yes" : "NO");
        if (mk != 0 || again != -17 || ad != 0 || !up || del != 0 || !gone) ok = 0;
    }

    /* §M90 3a stage e — IPv6 over rtnetlink: an address added with NEWADDR
     * appears in an AF_INET6 dump; a default route added with NEWROUTE is
     * what a v6 RouteGet answers with. */
    {
        int ln = net_ns_create();                         /* a scratch namespace ... */
        struct net_device* d = ln > 0 ? net_find_ns(ln, "lo") : NULL;   /* ... and its loopback */
        struct nlsock* s6 = NULL;
        int ad = -999, found = 0, rt = -999, gwok = 0;
        if (d) {
            s6 = nl_create(0);
            if (s6) s6->netns = ln;                       /* a socket made in that namespace */
        }
        uint8_t* q = (uint8_t*)kmalloc(128);
        if (s6 && q && d) {
            d->admin_down = 0;
            for (int i = 0; i < 128; i++) q[i] = 0;
            q[16] = AF_INET6_; q[17] = 64; w32(q + 20, (uint32_t)d->ifindex);
            w16(q + 24, 20); w16(q + 26, 2 /* IFA_LOCAL */);
            q[28] = 0xfd; q[29] = 0x00; q[30] = 0x00; q[31] = 0x42; q[43] = 0x01;   /* fd00:42::1 */
            w32(q, 44); w16(q + 4, RTM_NEWADDR); w16(q + 6, 1 | F_ACK | 0x400); w32(q + 8, 96);
            n = (nl_send(s6, q, 44) == 44) ? (int)nl_recv(s6, rx, NL_DGRAM, 0, 0, 0) : -1;
            ad = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
            n = nlt_ask(s6, 22 /* RTM_GETADDR */, F_DUMP, AF_INET6_, rx, NL_DGRAM);
            for (int off = 0; n > 0 && off + 16 <= n; ) {
                uint32_t len2 = r32(rx + off);
                if (len2 < 16) break;
                if (r16(rx + off + 4) == RTM_NEWADDR && rx[off + 16] == AF_INET6_) {
                    uint16_t al; const uint8_t* a = find_attr(rx + off + 24, len2 - 24, 1, &al);
                    if (a && al == 16 && a[0] == 0xfd && a[3] == 0x42 && a[15] == 1 && rx[off + 17] == 64) found = 1;
                }
                off += (len2 + 3) & ~3u;
            }
            /* NEWROUTE ::/0 via fd00:42::fe dev d */
            for (int i = 0; i < 128; i++) q[i] = 0;
            q[16] = AF_INET6_; q[17] = 0; q[20] = 254; q[21] = 3; q[23] = 1;
            w16(q + 28, 20); w16(q + 30, 5 /* RTA_GATEWAY */);
            q[32] = 0xfd; q[35] = 0x42; q[47] = 0xfe;
            w16(q + 48, 8); w16(q + 50, 4 /* RTA_OIF */); w32(q + 52, (uint32_t)d->ifindex);
            w32(q, 56); w16(q + 4, RTM_NEWROUTE); w16(q + 6, 1 | F_ACK | 0x400); w32(q + 8, 97);
            n = (nl_send(s6, q, 56) == 56) ? (int)nl_recv(s6, rx, NL_DGRAM, 0, 0, 0) : -1;
            rt = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
            /* RouteGet 2001:db8::1 */
            for (int i = 0; i < 128; i++) q[i] = 0;
            w32(q, 48); w16(q + 4, 26); w16(q + 6, 1); w32(q + 8, 98);
            q[16] = AF_INET6_; q[17] = 128;
            w16(q + 28, 20); w16(q + 30, 1); q[32] = 0x20; q[33] = 0x01; q[34] = 0x0d; q[35] = 0xb8; q[47] = 1;
            n = (nl_send(s6, q, 48) == 48) ? (int)nl_recv(s6, rx, NL_DGRAM, 0, 0, 0) : -1;
            if (n > 16 && r16(rx + 4) == RTM_NEWROUTE) {
                uint16_t al; const uint8_t* g = find_attr(rx + 28, r32(rx) - 28, 5, &al);
                gwok = g && al == 16 && g[0] == 0xfd && g[15] == 0xfe;
            }
        }
        if (q) kfree(q);
        if (s6) nl_close(s6);
        if (ln > 0) net_ns_destroy(ln);
        kprintf("nltest: v6 NEWADDR %d, in the AF_INET6 dump %s, NEWROUTE default %d, RouteGet via the gateway %s\n",
                ad, found ? "yes" : "NO", rt, gwok ? "yes" : "NO");
        if (ad != 0 || !found || rt != 0 || !gwok) ok = 0;
    }

    /* §M90 rung 5 — RouteGet: a non-dump GETROUTE answers ONE route. */
    {
        int oif_lo = -1, oif_far = -1; uint32_t gw_far = 0; int nmsg = 0;
        for (int pass = 0; pass < 2; pass++) {
            uint8_t req[40];
            for (int i = 0; i < 40; i++) req[i] = 0;
            w32(req, 36); w16(req + 4, 26 /* RTM_GETROUTE */); w16(req + 6, 1); w32(req + 8, 95);
            req[16] = AF_INET_; req[17] = 32;
            w16(req + 28, 8); w16(req + 30, 1);
            if (pass == 0) { req[32] = 127; req[33] = 0; req[34] = 0; req[35] = 1; }
            else           { req[32] = 8;   req[33] = 8; req[34] = 8; req[35] = 8; }
            n = (nl_send(s, req, 36) == 36) ? (int)nl_recv(s, rx, NL_DGRAM, 0, 0, 0) : -1;
            for (int off = 0; n > 0 && off + 16 <= n; ) {
                uint32_t ln = r32(rx + off);
                if (ln < 16) break;
                if (r16(rx + off + 4) == RTM_NEWROUTE) {
                    nmsg++;
                    uint16_t al; const uint8_t* a = find_attr(rx + off + 28, ln - 28, 4, &al);
                    int oif = a ? (int)r32(a) : -1;
                    if (pass == 0) oif_lo = oif;
                    else {
                        oif_far = oif;
                        const uint8_t* g = find_attr(rx + off + 28, ln - 28, 5, &al);
                        if (g) gw_far = be32_host(g);
                    }
                }
                off += (ln + 3) & ~3u;
            }
        }
        struct net_device* lo = NULL;
        for (int i = 0; i < l.n; i++) if (is_lo(l.d[i])) lo = l.d[i];
        struct net_device* pr = net_primary_ns(l.ns);
        int good = nmsg == 2 && lo && oif_lo == lo->ifindex && pr && oif_far == pr->ifindex &&
                   gw_far == pr->gateway;
        kprintf("nltest: RouteGet 127.0.0.1 -> oif %d, 8.8.8.8 -> oif %d via %x (%d answers) %s\n",
                oif_lo, oif_far, gw_far, nmsg, good ? "ok" : "WRONG");
        if (!good) ok = 0;
    }

    /* LinkSetUp's shape: RTM_NEWLINK, index 1, Change = Flags = IFF_UP. */
    {
        uint8_t req[32];
        for (int i = 0; i < 32; i++) req[i] = 0;
        w32(req, 32); w16(req + 4, RTM_NEWLINK); w16(req + 6, 1 | F_ACK); w32(req + 8, 78);
        w32(req + 20, 1); w32(req + 24, 1); w32(req + 28, 1);
        n = (nl_send(s, req, 32) == 32) ? (int)nl_recv(s, rx, NL_DGRAM, 0, 0, 0) : -1;
        int up = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
        w32(req + 20, 999);
        n = (nl_send(s, req, 32) == 32) ? (int)nl_recv(s, rx, NL_DGRAM, 0, 0, 0) : -1;
        int nodev = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : -999;
        kprintf("nltest: NEWLINK up on index 1 -> %d (want 0), on index 999 -> %d (want -19)\n",
                up, nodev);
        if (up != 0 || nodev != -19) ok = 0;
    }

    kfree(rx);
    nl_close(s);
    kprintf("nltest: %s\n", ok ? "ok" : "FAIL");
}
SHELL_CMD(nltest) = { "nltest", "", "rtnetlink: link/address/route dumps against the stack",
                      SHELL_G_TEST, cmd_nltest, SHELL_P_ANY };
