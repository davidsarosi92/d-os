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
 *   RTM_GETADDR  dump (IPv4; an AF_INET6 dump is empty — there is no IPv6)
 *   RTM_GETROUTE dump (the connected route of every configured device and
 *                the default route via its gateway — net_route's own table)
 *   RTM_GETNEIGH / GETRULE / GETQDISC dumps: empty (DONE)
 *   RTM_SETLINK  that only asks a device to be UP: acknowledged (they are)
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

struct devlist { struct net_device* d[16]; int n; };
static void dev_collect(struct net_device* dev, void* ctx) {
    struct devlist* l = (struct devlist*)ctx;
    if (l->n < 16 && !(dev->flags & NETDEV_F_SIMULATED)) l->d[l->n++] = dev;
}
static int is_lo(const struct net_device* d) { return (d->flags & NETDEV_F_LOOPBACK) != 0; }
static int prefix_of(uint32_t mask) { int n = 0; while (mask & 0x80000000u) { n++; mask <<= 1; } return n; }

static uint32_t if_flags(const struct net_device* d) {
    uint32_t f = 0;
    if (!d->admin_down) f |= 0x1 | 0x40;              /* IFF_UP | IFF_RUNNING */
    if (is_lo(d)) f |= 0x8;                           /* IFF_LOOPBACK */
    else          f |= 0x2 | 0x1000;                  /* IFF_BROADCAST | IFF_MULTICAST */
    if (d->link && !d->link((struct net_device*)d)) f &= ~0x40u;
    return f;
}

static void put_link(struct nlb* b, const struct net_device* d, int idx, uint16_t flags,
                     uint32_t seq, uint32_t pid) {
    uint32_t at = nl_begin(b, RTM_NEWLINK, flags, seq, pid);
    uint8_t* ifi = (uint8_t*)nlb_put(b, 16);
    if (ifi) {
        w16(ifi + 2, is_lo(d) ? 772 : 1);             /* ARPHRD_LOOPBACK / ETHER */
        w32(ifi + 4, (uint32_t)idx);
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
    nl_end(b, at);
}

static void put_addr(struct nlb* b, const struct net_device* d, int idx, uint32_t seq, uint32_t pid) {
    if (!d->ip) return;
    uint32_t at = nl_begin(b, RTM_NEWADDR, F_MULTI, seq, pid);
    uint8_t* ifa = (uint8_t*)nlb_put(b, 8);
    if (ifa) {
        ifa[0] = AF_INET_;
        ifa[1] = (uint8_t)prefix_of(d->netmask);
        ifa[2] = 0x80;                                /* IFA_F_PERMANENT */
        ifa[3] = is_lo(d) ? 254 : 0;                  /* RT_SCOPE_HOST / UNIVERSE */
        w32(ifa + 4, (uint32_t)idx);
    }
    nl_attr_be32(b, 1 /* IFA_ADDRESS */, d->ip);
    nl_attr_be32(b, 2 /* IFA_LOCAL */, d->ip);
    if (!is_lo(d)) nl_attr_be32(b, 4 /* IFA_BROADCAST */, d->ip | ~d->netmask);
    const char* nm = d->name ? d->name : "eth";
    uint16_t nl = 0; while (nm[nl]) nl++;
    nl_attr(b, 3 /* IFA_LABEL */, nm, (uint16_t)(nl + 1));
    nl_end(b, at);
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

/* ---- one request -------------------------------------------------------------- */

static void handle(struct nlsock* s, const uint8_t* m, uint32_t len, struct nlb* b) {
    uint16_t type = r16(m + 4), flags = r16(m + 6);
    uint32_t seq = r32(m + 8), pid = s->portid;
    int dump = (flags & F_DUMP) == F_DUMP;
    struct devlist l = { { 0 }, 0 };
    net_for_each(dev_collect, &l);
    uint8_t family = len > 16 ? m[16] : 0;

    switch (type) {
    case RTM_GETLINK:
        if (dump) {
            for (int i = 0; i < l.n; i++) put_link(b, l.d[i], i + 1, F_MULTI, seq, pid);
            put_done(b, seq, pid);
            return;
        } else {
            int want = len >= 32 ? (int)r32(m + 20) : 0;
            uint16_t al = 0;
            const uint8_t* nm = len > 32 ? find_attr(m + 32, len - 32, 3, &al) : NULL;
            for (int i = 0; i < l.n; i++) {
                int match = want ? (want == i + 1) : 0;
                if (!want && nm) {
                    const char* dn = l.d[i]->name ? l.d[i]->name : "";
                    match = 1;
                    for (uint16_t k = 0; k < al; k++) {
                        if (nm[k] == 0 && dn[k] == 0) break;
                        if (nm[k] != (uint8_t)dn[k]) { match = 0; break; }
                    }
                }
                if (match) {
                    put_link(b, l.d[i], i + 1, 0, seq, pid);
                    if (flags & F_ACK) put_error(b, 0, m, pid);
                    return;
                }
            }
            put_error(b, -19 /* ENODEV */, m, pid);
            return;
        }
    case RTM_GETADDR:
        if (family != AF_INET6_)
            for (int i = 0; i < l.n; i++) put_addr(b, l.d[i], i + 1, seq, pid);
        put_done(b, seq, pid);
        return;
    case RTM_GETROUTE:
        if (family != AF_INET6_) {
            for (int i = 0; i < l.n; i++) {
                struct net_device* d = l.d[i];
                if (!d->ip) continue;
                put_route(b, d->ip & d->netmask, prefix_of(d->netmask), 0, d->ip, i + 1,
                          is_lo(d) ? 254 : 253 /* HOST / LINK */, seq, pid);
            }
            for (int i = 0; i < l.n; i++)
                if (l.d[i]->gateway && !is_lo(l.d[i])) {
                    put_route(b, 0, 0, l.d[i]->gateway, 0, i + 1, 0, seq, pid);
                    break;
                }
        }
        put_done(b, seq, pid);
        return;
    case RTM_SETLINK: {
        /* Accepted only when it asks for nothing beyond "up": the devices are. */
        uint32_t want_flags = len >= 32 ? r32(m + 24) : 0, change = len >= 32 ? r32(m + 28) : 0;
        int only_up = (len <= 32) && ((change & ~0x1u) == 0) && (!change || (want_flags & 0x1));
        put_error(b, only_up ? 0 : -95 /* EOPNOTSUPP */, m, pid);
        return;
    }
    default:
        if (dump && (type & 3) == 2) {               /* any other GET dump: nothing to list */
            put_done(b, seq, pid);
            return;
        }
        put_error(b, -95 /* EOPNOTSUPP: no configuration changes yet */, m, pid);
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
    struct devlist l = { { 0 }, 0 };
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
            if (a && al == 4 && idx >= 1 && (int)idx <= l.n) {
                uint32_t ip = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
                if (ip == l.d[idx - 1]->ip) match++;
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

    n = nlt_ask(s, RTM_NEWLINK, F_ACK, 0, rx, NL_DGRAM);
    int err = (n >= 20 && r16(rx + 4) == NLMSG_ERROR) ? (int)r32(rx + 16) : 0;
    kprintf("nltest: NEWLINK (a configuration change) -> error %d (want -95)\n", err);
    if (err != -95) ok = 0;

    kfree(rx);
    nl_close(s);
    kprintf("nltest: %s\n", ok ? "ok" : "FAIL");
}
SHELL_CMD(nltest) = { "nltest", "", "rtnetlink: link/address/route dumps against the stack",
                      SHELL_G_TEST, cmd_nltest, SHELL_P_ANY };
