/* =============================================================================
 * nft_hook.c — running nf_tables at the IP hooks: the VM, conntrack, NAT.
 * §M90 3b.
 *
 * THE HOOKS.  The stack calls nft_ip_hook() at the five points Linux has —
 * PREROUTING (every packet a device receives, before the stack decides
 * whether it is ours), INPUT (ours), FORWARD (routed on), OUTPUT (made here),
 * POSTROUTING (leaving) — with the packet's network header in a WRITABLE
 * buffer.  Every base chain of the namespace hooked there, in an ip/ip6/inet
 * table of the packet's family, runs in PRIORITY order; DROP ends it, ACCEPT
 * goes on to the next chain, as on Linux.
 *
 * CONNTRACK.  One entry per connection, two tuples: the ORIGINAL (what the
 * first packet carried) and the REPLY (what a reply will carry — the inverse
 * of the original, with whatever NAT was applied).  A packet matching either
 * gets its state (NEW / ESTABLISHED) and direction for `ct` expressions, and
 * the translation recorded in the entry is applied to it before the chains
 * run (DNAT in PREROUTING / OUTPUT, the reverse of SNAT for replies) or after
 * (SNAT in POSTROUTING, the reverse of DNAT for replies).  NAT-type chains run
 * only for a connection's FIRST packet (Linux's rule: NAT is decided once per
 * connection).  TCP, UDP and ICMP echo are tracked by ports/id; anything else
 * by addresses.  Timeouts: TCP 1 h idle (10 s after both FINs or an RST), UDP
 * 30 s (2 min once answered), ICMP 30 s.
 *
 * NOT HERE (said): ICMP errors are not RELATED to the connection they quote
 * (they are tracked as new flows), no conntrack helpers (FTP, SIP), no TCP
 * window tracking (Linux's "invalid" for out-of-window segments), packet
 * mangling, ingress/egress hooks, the bridge family.
 * ============================================================================= */

#include "nftables.h"
#include "nft_vm.h"
#include "net.h"
#include "kmalloc.h"
#include "printf.h"
#include "timer.h"
#include "task.h"
#include "cred.h"
#include <stddef.h>
#include <stdint.h>

#define CT_MAX 512
static struct nft_conn g_ct[CT_MAX];          /* under nft_lock */
static uint32_t g_ct_new, g_ct_full, g_nat_bind, g_dropped, g_evals;
static uint32_t g_log_lines;

/* ---- small helpers ---------------------------------------------------------- */

static int s_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void mcpy(void* d, const void* s, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)d)[i] = ((const uint8_t*)s)[i]; }
static int mcmp(const void* a, const void* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = ((const uint8_t*)a)[i], y = ((const uint8_t*)b)[i];
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static int alen(int fam) { return fam == 10 ? 16 : 4; }

/* ---- the packet's fields ------------------------------------------------------ */

static uint8_t* p_src(struct nft_pkt* p) { return p->fam == 10 ? p->nh + 8 : p->nh + 12; }
static uint8_t* p_dst(struct nft_pkt* p) { return p->fam == 10 ? p->nh + 24 : p->nh + 16; }
static int has_ports(uint8_t proto) { return proto == 6 || proto == 17; }
static int is_echo(struct nft_pkt* p) {
    if (p->thoff + 8 > p->len) return 0;
    uint8_t t = p->nh[p->thoff];
    return (p->fam == 2 && p->l4proto == 1 && (t == 8 || t == 0)) ||
           (p->fam == 10 && p->l4proto == 58 && (t == 128 || t == 129));
}
static uint16_t p_sport(struct nft_pkt* p) {
    if (has_ports(p->l4proto) && p->thoff + 4 <= p->len) return rd16(p->nh + p->thoff);
    if (is_echo(p)) return rd16(p->nh + p->thoff + 4);          /* the echo id, both ways */
    return 0;
}
static uint16_t p_dport(struct nft_pkt* p) {
    if (has_ports(p->l4proto) && p->thoff + 4 <= p->len) return rd16(p->nh + p->thoff + 2);
    if (is_echo(p)) return rd16(p->nh + p->thoff + 4);
    return 0;
}

/* Checksum adjustment for a 16-bit field change (RFC 1624), applied to the
 * one's-complement sum at `csum`. */
static void csum_adj16(uint8_t* csum, uint16_t old, uint16_t new_) {
    uint32_t s = (uint16_t)~rd16(csum);
    s += (uint16_t)~old;
    s += new_;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    wr16(csum, (uint16_t)~s);
}
static uint8_t* l4_csum(struct nft_pkt* p) {
    if (p->l4proto == 6 && p->thoff + 18 <= p->len) return p->nh + p->thoff + 16;
    if (p->l4proto == 17 && p->thoff + 8 <= p->len) {
        uint8_t* c = p->nh + p->thoff + 6;
        if (p->fam == 2 && c[0] == 0 && c[1] == 0) return NULL;      /* v4 UDP without a sum */
        return c;
    }
    if (p->l4proto == 58 && p->thoff + 4 <= p->len) return p->nh + p->thoff + 2;   /* ICMPv6: pseudo-header */
    return NULL;
}
/* Rewrite an address field (src or dst) and every checksum that covers it. */
static void set_addr(struct nft_pkt* p, uint8_t* field, const uint8_t* a) {
    uint8_t* l4 = l4_csum(p);
    int n = alen(p->fam);
    for (int i = 0; i < n; i += 2) {
        uint16_t o = rd16(field + i), nw = rd16(a + i);
        if (o == nw) continue;
        if (p->fam == 2) csum_adj16(p->nh + 10, o, nw);           /* the IPv4 header sum */
        if (l4) csum_adj16(l4, o, nw);                              /* the pseudo-header */
        wr16(field + i, nw);
    }
}
static void set_port(struct nft_pkt* p, int dst, uint16_t v) {
    if (has_ports(p->l4proto)) {
        uint8_t* f = p->nh + p->thoff + (dst ? 2 : 0);
        uint16_t o = rd16(f);
        if (o == v) return;
        uint8_t* l4 = l4_csum(p);
        if (l4) csum_adj16(l4, o, v);
        wr16(f, v);
    } else if (is_echo(p)) {                                        /* the echo id */
        uint8_t* f = p->nh + p->thoff + 4;
        uint16_t o = rd16(f);
        if (o == v) return;
        csum_adj16(p->nh + p->thoff + 2, o, v);
        wr16(f, v);
    }
}

/* ---- conntrack ---------------------------------------------------------------- */

static uint64_t ct_timeout_ms(const struct nft_conn* c) {
    if (c->proto == 6) return c->fin_seen >= 2 ? 10000 : 3600000;
    if (c->proto == 17) return (c->status & IPS_SEEN_REPLY) ? 120000 : 30000;
    return 30000;
}
static int tuple_is(const struct nft_tuple* t, struct nft_pkt* p, int fam) {
    int n = alen(fam);
    return mcmp(t->src, p_src(p), (uint32_t)n) == 0 && mcmp(t->dst, p_dst(p), (uint32_t)n) == 0 &&
           t->sport == p_sport(p) && t->dport == p_dport(p);
}
static struct nft_conn* ct_find(struct nft_pkt* p, int* dir) {
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < CT_MAX; i++) {
        struct nft_conn* c = &g_ct[i];
        if (!c->used) continue;
        if (now - c->last_ms > ct_timeout_ms(c)) { c->used = 0; continue; }
        if (c->ns != p->ns || c->fam != p->fam || c->proto != p->l4proto) continue;
        if (tuple_is(&c->orig, p, p->fam))  { *dir = 0; return c; }
        if (tuple_is(&c->reply, p, p->fam)) { *dir = 1; return c; }
    }
    return NULL;
}
static struct nft_conn* ct_new(struct nft_pkt* p) {
    int slot = -1;
    uint64_t oldest = ~0ull, now = timer_ticks_ms();
    for (int i = 0; i < CT_MAX; i++) {
        if (!g_ct[i].used || now - g_ct[i].last_ms > ct_timeout_ms(&g_ct[i])) { slot = i; break; }
        if (!(g_ct[i].status & IPS_ASSURED) && g_ct[i].last_ms < oldest) { oldest = g_ct[i].last_ms; slot = i; }
    }
    if (slot < 0) { g_ct_full++; return NULL; }
    struct nft_conn* c = &g_ct[slot];
    for (unsigned k = 0; k < sizeof *c; k++) ((uint8_t*)c)[k] = 0;
    c->used = 1; c->ns = p->ns; c->fam = p->fam; c->proto = p->l4proto;
    int n = alen(p->fam);
    mcpy(c->orig.src, p_src(p), (uint32_t)n); mcpy(c->orig.dst, p_dst(p), (uint32_t)n);
    c->orig.sport = p_sport(p); c->orig.dport = p_dport(p);
    /* the reply is the inverse — until NAT changes it */
    mcpy(c->reply.src, c->orig.dst, (uint32_t)n); mcpy(c->reply.dst, c->orig.src, (uint32_t)n);
    c->reply.sport = c->orig.dport; c->reply.dport = c->orig.sport;
    if (is_echo(p)) { c->reply.sport = c->orig.sport; c->reply.dport = c->orig.dport; }
    c->last_ms = now;
    g_ct_new++;
    return c;
}
/* Look the packet up (or start tracking it) and set its state. */
static void ct_classify(struct nft_pkt* p) {
    int dir = 0;
    struct nft_conn* c = ct_find(p, &dir);
    if (!c) {
        c = ct_new(p);
        p->ct = c; p->dir = 0;
        p->ctstate = c ? CTS_NEW : CTS_INVALID;
        return;
    }
    p->ct = c; p->dir = dir;
    c->last_ms = timer_ticks_ms();
    if (dir == 1 && !(c->status & IPS_SEEN_REPLY)) c->status |= IPS_SEEN_REPLY | IPS_ASSURED;
    p->ctstate = (c->status & IPS_SEEN_REPLY) ? CTS_ESTABLISHED : CTS_NEW;
    if (p->l4proto == 6 && p->thoff + 14 <= p->len) {
        uint8_t fl = p->nh[p->thoff + 13];
        if (fl & 0x04) c->fin_seen = 2;                         /* RST */
        else if (fl & 0x01) c->fin_seen++;                      /* FIN */
    }
    p->mark = c->mark;
}

/* Apply the recorded translation for this packet's direction at this hook.
 * `pre` = PREROUTING/OUTPUT (destination side), else POSTROUTING (source). */
static void ct_apply(struct nft_pkt* p, int pre) {
    struct nft_conn* c = p->ct;
    if (!c) return;
    int n = alen(p->fam);
    if (pre) {
        if (p->dir == 0 && (c->status & IPS_DST_NAT)) {         /* DNAT: dst := the reply's source */
            set_addr(p, p_dst(p), c->reply.src);
            set_port(p, 1, c->reply.sport);
        } else if (p->dir == 1 && (c->status & IPS_SRC_NAT)) {  /* un-SNAT: dst := original source */
            set_addr(p, p_dst(p), c->orig.src);
            set_port(p, 1, c->orig.sport);
        }
    } else {
        if (p->dir == 0 && (c->status & IPS_SRC_NAT)) {         /* SNAT: src := the reply's dest */
            set_addr(p, p_src(p), c->reply.dst);
            set_port(p, 0, c->reply.dport);
        } else if (p->dir == 1 && (c->status & IPS_DST_NAT)) {  /* un-DNAT: src := original dest */
            set_addr(p, p_src(p), c->orig.dst);
            set_port(p, 0, c->orig.dport);
        }
    }
    (void)n;
}

/* A reply tuple nobody else uses (SNAT port choice). */
static int reply_taken(const struct nft_conn* self, const struct nft_tuple* r, int fam, uint8_t proto, int ns) {
    for (int i = 0; i < CT_MAX; i++) {
        const struct nft_conn* c = &g_ct[i];
        if (!c->used || c == self || c->ns != ns || c->fam != fam || c->proto != proto) continue;
        int n = alen(fam);
        if (!mcmp(c->reply.src, r->src, (uint32_t)n) && !mcmp(c->reply.dst, r->dst, (uint32_t)n) &&
            c->reply.sport == r->sport && c->reply.dport == r->dport) return 1;
    }
    return 0;
}

/* Record the NAT the ruleset asked for in the connection, then apply it. */
static void ct_bind_nat(struct nft_pkt* p) {
    struct nft_conn* c = p->ct;
    if (!c || !p->nat || p->dir != 0) return;
    int n = alen(p->fam);
    if (p->nat == 2) {                                          /* DNAT: the reply comes FROM there */
        if (p->nat_has_addr) mcpy(c->reply.src, p->nat_addr, (uint32_t)n);
        if (p->nat_has_port) c->reply.sport = p->nat_pmin;
        c->status |= IPS_DST_NAT;
    } else {                                                    /* SNAT / MASQ: the reply goes TO there */
        if (p->nat_has_addr) mcpy(c->reply.dst, p->nat_addr, (uint32_t)n);
        uint16_t lo = p->nat_has_port ? p->nat_pmin : 0, hi = p->nat_has_port ? p->nat_pmax : 0;
        if (lo && hi < lo) hi = lo;
        if (lo && (c->reply.dport < lo || c->reply.dport > hi)) c->reply.dport = lo;
        /* keep the port if it is free, else walk the range (or 20000-29999) */
        if (has_ports(c->proto) || is_echo(p)) {
            uint16_t a = lo ? lo : 20000, b = lo ? hi : 29999;
            uint32_t tries = 0;
            while (reply_taken(c, &c->reply, c->fam, c->proto, c->ns) && tries++ < 10000) {
                uint16_t v = (uint16_t)(c->reply.dport + 1);
                if (v < a || v > b) v = a;
                c->reply.dport = v;
                if (is_echo(p)) c->reply.sport = v;
            }
        }
        c->status |= IPS_SRC_NAT;
    }
    g_nat_bind++;
    ct_apply(p, p->nat == 2);
}

/* ---- the VM --------------------------------------------------------------------- */

static struct net_device* dev_of(struct nft_pkt* p, int out) { return out ? p->out : p->in; }

static void meta_load(struct nft_pkt* p, uint32_t key, uint32_t* reg, int words) {
    uint8_t* r = (uint8_t*)reg;
    for (int i = 0; i < words * 4; i++) r[i] = 0;
    struct net_device* d;
    switch (key) {
    case NFT_META_LEN:      reg[0] = p->len; break;
    case NFT_META_PROTOCOL: r[0] = p->fam == 10 ? 0x86 : 0x08; r[1] = p->fam == 10 ? 0xdd : 0x00; break;
    case NFT_META_NFPROTO:  r[0] = (uint8_t)p->fam; break;
    case NFT_META_L4PROTO:  r[0] = p->l4proto; break;
    case NFT_META_MARK:     reg[0] = p->mark; break;
    case NFT_META_PKTTYPE:  r[0] = p->pkttype; break;
    case NFT_META_IIF: case NFT_META_OIF:
        d = dev_of(p, key == NFT_META_OIF); reg[0] = d ? (uint32_t)d->ifindex : 0; break;
    case NFT_META_IIFNAME: case NFT_META_OIFNAME:
        d = dev_of(p, key == NFT_META_OIFNAME);
        if (d && d->name) for (int i = 0; i < 15 && d->name[i]; i++) r[i] = (uint8_t)d->name[i];
        break;
    case NFT_META_IIFTYPE: case NFT_META_OIFTYPE: {
        d = dev_of(p, key == NFT_META_OIFTYPE);
        uint16_t t = d ? ((d->flags & NETDEV_F_LOOPBACK) ? 772 : 1) : 0;
        r[0] = (uint8_t)t; r[1] = (uint8_t)(t >> 8);
        break;
    }
    case NFT_META_IIFKIND: case NFT_META_OIFKIND: {
        d = dev_of(p, key == NFT_META_OIFKIND);
        const char* k = !d ? "" : d->kind == NETDEV_K_VETH ? "veth" : d->kind == NETDEV_K_BRIDGE ? "bridge" : "";
        for (int i = 0; k[i]; i++) r[i] = (uint8_t)k[i];
        break;
    }
    case NFT_META_SKUID: case NFT_META_SKGID: {
        struct task* t = p->hook == 3 /* OUTPUT */ ? task_current() : NULL;
        int v = t ? (key == NFT_META_SKUID ? cred_uid(&t->cred) : cred_gid(&t->cred)) : 0;
        reg[0] = (uint32_t)(v < 0 ? 0 : v);
        break;
    }
    case NFT_META_CPU:      reg[0] = 0; break;
    case NFT_META_PRANDOM:  reg[0] = (uint32_t)(timer_ticks_ms() * 2654435761u); break;
    default: break;
    }
}

static void ct_load(struct nft_pkt* p, const struct nft_op* op, uint32_t* reg) {
    uint8_t* r = (uint8_t*)reg;
    for (int i = 0; i < 16; i++) r[i] = 0;
    struct nft_conn* c = p->ct;
    int dir = op->sub ? op->sub - 1 : p->dir;
    const struct nft_tuple* t = c ? (dir ? &c->reply : &c->orig) : NULL;
    switch (op->x) {
    case NFT_CT_STATE:     reg[0] = p->ctstate; break;
    case NFT_CT_DIRECTION: r[0] = (uint8_t)p->dir; break;
    case NFT_CT_STATUS:    reg[0] = c ? (c->status | IPS_CONFIRMED) : 0; break;
    case NFT_CT_MARK:      reg[0] = c ? c->mark : 0; break;
    case NFT_CT_L3PROTOCOL: r[0] = (uint8_t)p->fam; break;
    case NFT_CT_PROTOCOL:  r[0] = p->l4proto; break;
    case NFT_CT_SRC: case NFT_CT_SRC_IP: case NFT_CT_SRC_IP6:
        if (t) mcpy(r, t->src, (uint32_t)alen(p->fam));
        break;
    case NFT_CT_DST: case NFT_CT_DST_IP: case NFT_CT_DST_IP6:
        if (t) mcpy(r, t->dst, (uint32_t)alen(p->fam));
        break;
    case NFT_CT_PROTO_SRC: if (t) wr16(r, t->sport); break;
    case NFT_CT_PROTO_DST: if (t) wr16(r, t->dport); break;
    default: break;
    }
}

static int is_local_addr(struct nft_pkt* p, const uint8_t* a);

static void fib_eval(struct nft_pkt* p, const struct nft_op* op, uint32_t* reg) {
    uint8_t* r = (uint8_t*)reg;
    for (int i = 0; i < 16; i++) r[i] = 0;
    const uint8_t* a = (op->y & NFTA_FIB_F_SADDR) ? p_src(p) : p_dst(p);
    if (op->sub == NFT_FIB_RESULT_ADDRTYPE) {
        uint32_t t = 1;                                         /* RTN_UNICAST */
        if (p->fam == 2 && a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255) t = 3;   /* BROADCAST */
        else if ((p->fam == 2 && (a[0] & 0xf0) == 0xe0) || (p->fam == 10 && a[0] == 0xff)) t = 5;  /* MULTICAST */
        else if (is_local_addr(p, a)) t = 2;                     /* RTN_LOCAL */
        reg[0] = t;
        return;
    }
    struct net_device* d = NULL;
    if (p->fam == 2) {
        uint32_t ip = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
        d = net_route_ns(p->ns, ip, NULL);
    } else d = net6_route(p->ns, a, NULL);
    if (op->sub == NFT_FIB_RESULT_OIF) reg[0] = d ? (uint32_t)d->ifindex : 0;
    else if (d && d->name) for (int i = 0; i < 15 && d->name[i]; i++) r[i] = (uint8_t)d->name[i];
}

struct net_local_q { int ns; const uint8_t* a; int hit; };
static void local6_cb(struct net_device* d, void* ctx) {
    struct net_local_q* q = (struct net_local_q*)ctx;
    if (d->netns != q->ns) return;
    if (!(d->flags & NETDEV_F_LOOPBACK) && !mcmp(d->ip6_ll, q->a, 16)) q->hit = 1;
    for (int i = 0; i < d->n_ip6; i++) if (!mcmp(d->ip6[i], q->a, 16)) q->hit = 1;
}
static int is_local_addr(struct nft_pkt* p, const uint8_t* a) {
    if (p->fam == 2) {
        uint32_t ip = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
        return (ip >> 24) == 127 || net_is_local_ip_ns(p->ns, ip);
    }
    struct net_local_q q = { p->ns, a, 0 };
    static const uint8_t lo6[16] = { [15] = 1 };
    if (!mcmp(a, lo6, 16)) return 1;
    net_for_each(local6_cb, &q);
    return q.hit;
}

/* Set lookup: exact, interval (start .. before the next END), catch-all. */
static struct nft_elem* set_match(struct nft_set* s, const uint8_t* key) {
    uint32_t kl = s->klen;
    struct nft_elem* catchall = NULL;
    if (s->flags & 0x4 /* INTERVAL */) {
        struct nft_elem* best = NULL;
        for (struct nft_elem* e = s->elems; e; e = e->next) {
            if (e->flags & 2) { catchall = e; continue; }
            if (e->klen != kl || mcmp(e->key, key, kl) > 0) continue;   /* starts after the key */
            if (e->kelen) {                                             /* a [key, key_end] range */
                if (mcmp(key, e->key_end, kl) <= 0 && !(e->flags & 1)) return e;
                continue;
            }
            if (!best || mcmp(e->key, best->key, kl) > 0 ||
                (mcmp(e->key, best->key, kl) == 0 && (e->flags & 1))) best = e;
        }
        if (best && !(best->flags & 1)) return best;
        return catchall;
    }
    for (struct nft_elem* e = s->elems; e; e = e->next) {
        if (e->flags & 2) { catchall = e; continue; }
        if (e->klen == kl && !mcmp(e->key, key, kl)) return e;
    }
    return catchall;
}

static struct nft_table* g_cur_table;     /* the table whose chain is running (under nft_lock) */

/* Run one rule; returns the verdict code it left (NFT_CONTINUE = none). */
static int run_rule(struct nft_rule* r, struct nft_pkt* p, uint32_t* reg, char* jchain) {
    reg[0] = (uint32_t)NFT_CONTINUE;
    for (int i = 0; i < r->nprog; i++) {
        struct nft_op* op = &r->prog[i];
        uint8_t* R = (uint8_t*)reg;
        switch (op->kind) {
        case OP_IMM:
            if (op->is_verdict) {
                reg[0] = (uint32_t)op->verdict;
                for (int k = 0; k < NFT_NAME_MAX; k++) jchain[k] = op->name[k];
            } else if (op->dreg > 0) mcpy(R + op->dreg * 4, op->data, op->len);
            break;
        case OP_CMP: {
            int c = mcmp(R + op->sreg * 4, op->data, op->len);
            int ok = op->sub == NFT_CMP_EQ ? c == 0 : op->sub == NFT_CMP_NEQ ? c != 0 :
                     op->sub == NFT_CMP_LT ? c < 0 : op->sub == NFT_CMP_LTE ? c <= 0 :
                     op->sub == NFT_CMP_GT ? c > 0 : c >= 0;
            if (!ok) return NFT_BREAK;
            break;
        }
        case OP_RANGE: {
            uint8_t* v = R + op->sreg * 4;
            int in = mcmp(v, op->data, op->len) >= 0 && mcmp(v, op->data2, op->len) <= 0;
            if (op->sub == 0 ? !in : in) return NFT_BREAK;
            break;
        }
        case OP_BITWISE: {
            uint8_t tmp[NFT_KEY_MAX];
            uint8_t* s = R + op->sreg * 4;
            if (op->sub == NFT_BITWISE_BOOL)
                for (int k = 0; k < op->len; k++) tmp[k] = (uint8_t)((s[k] & op->data[k]) ^ op->data2[k]);
            else {                                       /* shifts over host-order u32 words */
                for (int k = 0; k + 3 < op->len; k += 4) {
                    uint32_t w = (uint32_t)s[k] | (uint32_t)s[k+1] << 8 | (uint32_t)s[k+2] << 16 | (uint32_t)s[k+3] << 24;
                    w = op->sub == NFT_BITWISE_LSHIFT ? w << op->x : w >> op->x;
                    tmp[k] = (uint8_t)w; tmp[k+1] = (uint8_t)(w >> 8); tmp[k+2] = (uint8_t)(w >> 16); tmp[k+3] = (uint8_t)(w >> 24);
                }
            }
            mcpy(R + op->dreg * 4, tmp, op->len);
            break;
        }
        case OP_BYTEORDER: {
            uint8_t tmp[NFT_KEY_MAX];
            uint8_t* s = R + op->sreg * 4;
            for (int k = 0; k + (int)op->x <= op->len; k += (int)op->x)
                for (int b = 0; b < (int)op->x; b++) tmp[k + b] = s[k + (int)op->x - 1 - b];
            mcpy(R + op->dreg * 4, tmp, op->len);
            break;
        }
        case OP_PAYLOAD: {
            uint32_t base = op->sub == NFT_PAYLOAD_NETWORK_HEADER ? 0 :
                            op->sub == NFT_PAYLOAD_TRANSPORT_HEADER ? p->thoff : 0xffffffffu;
            if (base == 0xffffffffu || base + op->x + op->len > p->len) return NFT_BREAK;
            uint8_t* d = R + op->dreg * 4;
            for (int k = 0; k < ((op->len + 3) & ~3); k++) d[k] = 0;
            mcpy(d, p->nh + base + op->x, op->len);
            break;
        }
        case OP_META:   meta_load(p, op->x, reg + op->dreg, op->x == NFT_META_IIFNAME || op->x == NFT_META_OIFNAME ||
                                  op->x == NFT_META_IIFKIND || op->x == NFT_META_OIFKIND ? 4 : 1); break;
        case OP_META_SET: p->mark = reg[op->sreg]; break;
        case OP_COUNTER: op->packets++; op->bytes += p->len; break;
        case OP_CT:     ct_load(p, op, reg + op->dreg); break;
        case OP_CT_SET: if (p->ct) p->ct->mark = reg[op->sreg]; break;
        case OP_LOOKUP: {
            struct nft_set* s = nft_set_find(g_cur_table, op->name, 0);
            struct nft_elem* e = s ? set_match(s, R + op->sreg * 4) : NULL;
            if (op->sub ? e != NULL : e == NULL) return NFT_BREAK;
            if (e && op->dreg >= 0) {
                if (op->dreg == 0 && e->has_verdict) {
                    reg[0] = (uint32_t)e->verdict;
                    for (int k = 0; k < NFT_NAME_MAX; k++) jchain[k] = e->vchain[k];
                } else if (op->dreg > 0) mcpy(R + op->dreg * 4, e->data, e->dlen);
            }
            break;
        }
        case OP_NAT: {
            p->nat = op->sub == 0 ? 1 : 2;
            p->nat_has_addr = op->sreg >= 0;
            if (op->sreg >= 0) mcpy(p->nat_addr, R + op->sreg * 4, (uint32_t)alen(p->fam));
            p->nat_has_port = op->dreg >= 0;
            if (op->dreg >= 0) { p->nat_pmin = rd16(R + op->dreg * 4); p->nat_pmax = rd16(R + op->dreg2 * 4); }
            reg[0] = NF_ACCEPT_;
            return NF_ACCEPT_;
        }
        case OP_MASQ: case OP_REDIR: {
            struct net_device* d = op->kind == OP_MASQ ? p->out : p->in;
            p->nat = op->kind == OP_MASQ ? 1 : 2;
            p->nat_has_addr = 1;
            if (p->fam == 2) {
                uint32_t ip = d ? d->ip : 0x7f000001u;
                p->nat_addr[0] = (uint8_t)(ip >> 24); p->nat_addr[1] = (uint8_t)(ip >> 16);
                p->nat_addr[2] = (uint8_t)(ip >> 8); p->nat_addr[3] = (uint8_t)ip;
            } else if (d) net6_pick_src(d, op->kind == OP_MASQ ? p_dst(p) : p_src(p), p->nat_addr);
            p->nat_has_port = op->dreg >= 0;
            if (op->dreg >= 0) { p->nat_pmin = rd16(R + op->dreg * 4); p->nat_pmax = rd16(R + op->dreg2 * 4); }
            if (!d && op->kind == OP_MASQ) p->nat = 0;
            reg[0] = NF_ACCEPT_;
            return NF_ACCEPT_;
        }
        case OP_FIB:    fib_eval(p, op, reg + op->dreg); break;
        case OP_LOG:
            if (g_log_lines < 50) {
                g_log_lines++;
                kprintf("nft log: hook %d ns %d fam %d proto %d len %u\n", p->hook, p->ns, p->fam, p->l4proto, p->len);
            }
            break;
        case OP_REJECT: reg[0] = NF_DROP_; return NF_DROP_;
        default: break;
        }
        if ((int32_t)reg[0] != NFT_CONTINUE) return (int32_t)reg[0];
    }
    return NFT_CONTINUE;
}

/* Run a chain: NF_ACCEPT / NF_DROP, or NFT_RETURN when it fell off its end or
 * returned (the caller decides: a base chain then applies its policy). */
static int run_chain(struct nft_chain* c, struct nft_pkt* p, int depth) {
    if (depth > 16) return NF_DROP_;                 /* a loop of jumps: Linux refuses those at load */
    uint32_t reg[NFT_REG_WORDS];
    char jchain[NFT_NAME_MAX];
    for (struct nft_rule* r = c->rules; r; r = r->next) {
        jchain[0] = 0;
        int v = run_rule(r, p, reg, jchain);
        if (v == NFT_CONTINUE || v == NFT_BREAK) continue;
        if (v == NF_ACCEPT_ || v == NF_DROP_) return v;
        if (v == NFT_RETURN) return NFT_RETURN;
        if (v == NFT_JUMP || v == NFT_GOTO) {
            struct nft_chain* t = nft_chain_find(g_cur_table, jchain, 0);
            if (!t) return NF_DROP_;
            int sub = run_chain(t, p, depth + 1);
            if (sub == NF_ACCEPT_ || sub == NF_DROP_) return sub;
            if (v == NFT_GOTO) return NFT_RETURN;
            continue;
        }
        if (v >= 2) return NF_ACCEPT_;               /* STOLEN / QUEUE / REPEAT: not here */
    }
    return NFT_RETURN;
}

/* The base chains of `hook` for this packet, in priority order. */
static int hook_chains(struct nft_net* n, int fam, int hook, struct nft_chain** cs, struct nft_table** ts, int max) {
    int k = 0;
    for (struct nft_table* t = n->tables; t; t = t->next) {
        if (t->flags & 1 /* DORMANT */) continue;
        if (!(t->family == fam || t->family == 1 /* inet */)) continue;
        for (struct nft_chain* c = t->chains; c; c = c->next) {
            if (!c->base || c->hooknum != hook || k >= max) continue;
            int at = k;
            while (at > 0 && cs[at - 1]->prio > c->prio) { cs[at] = cs[at - 1]; ts[at] = ts[at - 1]; at--; }
            cs[at] = c; ts[at] = t; k++;
        }
    }
    return k;
}

int nft_active(int ns) {
    struct nft_net* n = nft_net_of(ns);
    return n && n->tables;
}

/* THE ENTRY: one packet at one hook.  `nh` holds the network header and is
 * rewritten in place by NAT.  Returns NF_ACCEPT (1) or NF_DROP (0). */
int nft_ip_hook(int ns, int fam, int hook, struct net_device* in, struct net_device* out,
                uint8_t* nh, uint32_t len) {
    struct nft_net* n = nft_net_of(ns);
    if (!n || !n->tables) return NF_ACCEPT_;
    struct nft_pkt p;
    for (unsigned k = 0; k < sizeof p; k++) ((uint8_t*)&p)[k] = 0;
    p.ns = ns; p.fam = fam; p.hook = hook; p.nh = nh; p.len = len; p.in = in; p.out = out;
    if (fam == 2) {
        if (len < 20) return NF_ACCEPT_;
        p.thoff = (uint32_t)(nh[0] & 0x0F) * 4u;
        p.l4proto = nh[9];
        uint32_t tl = rd16(nh + 2);
        if (tl < p.len) p.len = tl;
        p.pkttype = (nh[16] & 0xf0) == 0xe0 ? 2 : (nh[16] == 255 && nh[19] == 255) ? 1 : 0;
    } else {
        if (len < 40) return NF_ACCEPT_;
        p.thoff = 40;
        p.l4proto = nh[6];
        uint32_t tl = (uint32_t)rd16(nh + 4) + 40;
        if (tl < p.len) p.len = tl;
        p.pkttype = nh[24] == 0xff ? 2 : 0;
        if (p.l4proto == 0 && p.len >= 48) { p.thoff = 40 + (uint32_t)(nh[41] + 1) * 8; p.l4proto = nh[40]; }
        if (p.l4proto == 58 && p.thoff < p.len) {
            uint8_t t = nh[p.thoff];
            if (t >= 133 && t <= 137) return NF_ACCEPT_;       /* NDP is never tracked or filtered here */
        }
    }
    uint32_t f = nft_lock();
    g_evals++;
    ct_classify(&p);
    int pre = hook == 0 /* PREROUTING */ || hook == 3 /* OUTPUT */;
    if (pre) ct_apply(&p, 1);
    struct nft_chain* cs[16]; struct nft_table* ts[16];
    int k = hook_chains(n, fam, hook, cs, ts, 16);
    int verdict = NF_ACCEPT_;
    int first = p.ct && p.ctstate == CTS_NEW && p.dir == 0 &&
                !(p.ct->status & (pre ? IPS_DST_NAT : IPS_SRC_NAT));
    for (int i = 0; i < k; i++) {
        int is_nat = s_eq(cs[i]->type, "nat");
        if (is_nat && !first) continue;            /* NAT is decided by a connection's first packet */
        g_cur_table = ts[i];
        p.nat = 0;
        int v = run_chain(cs[i], &p, 0);
        if (v == NFT_RETURN) v = cs[i]->policy == 0 ? NF_DROP_ : NF_ACCEPT_;
        if (v == NF_DROP_) { verdict = NF_DROP_; break; }
        if (is_nat && p.nat) ct_bind_nat(&p);
    }
    if (verdict == NF_ACCEPT_ && !pre) ct_apply(&p, 0);
    if (p.ct && p.mark != p.ct->mark && p.mark) p.ct->mark = p.mark;
    if (verdict == NF_DROP_) g_dropped++;
    nft_unlock(f);
    return verdict;
}

/* A namespace's end takes its connections. */
void nft_ct_ns_gone(int ns) {
    uint32_t f = nft_lock();
    for (int i = 0; i < CT_MAX; i++) if (g_ct[i].used && g_ct[i].ns == ns) g_ct[i].used = 0;
    nft_unlock(f);
}

void nft_hook_stats(uint32_t* evals, uint32_t* dropped, uint32_t* ct_new, uint32_t* nat_bind, uint32_t* ct_live) {
    uint32_t live = 0;
    for (int i = 0; i < CT_MAX; i++) if (g_ct[i].used) live++;
    if (evals) *evals = g_evals;
    if (dropped) *dropped = g_dropped;
    if (ct_new) *ct_new = g_ct_new;
    if (nat_bind) *nat_bind = g_nat_bind;
    if (ct_live) *ct_live = live;
    (void)g_ct_full;
}
