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
 * RX model for this first slice is *poll from the calling task*: the driver's
 * dev->poll() pumps the RX ring into net_rx(), and the blocking helpers
 * (arp resolve, ping) call it in a bounded spin loop.  Everything therefore
 * runs in one task context → no locking.  IRQ-driven RX + a `netd` task are a
 * documented follow-up; net_rx() is already shaped to be called from an ISR.
 *
 * All multi-byte on-wire fields are big-endian; we convert at the boundary
 * with htons/htonl (net.h).  The stack's own state is host byte order.
 * ============================================================================= */

#include "net.h"
#include "hal.h"
#include "hal_api.h"
#include "printf.h"
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
}

/* ----------------------- L2 transmit -------------------------------------- */

/* Single-task poll model → a static assembly buffer is safe (no reentrancy). */
static uint8_t g_txframe[ETH_FRAME_MAX];

static int eth_send(struct net_device* dev, const uint8_t* dst_mac,
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

static void arp_send(struct net_device* dev, uint16_t oper,
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
    eth_send(dev, dst, ETHERTYPE_ARP, &a, sizeof(a));
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
        /* Somebody wants our MAC → reply to the sender. */
        arp_send(dev, ARP_OP_REPLY, a->sha, spa);
    }
}

int net_arp_resolve(struct net_device* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    if (arp_cache_get(ip, mac_out) == 0) return 0;
    /* Fire a request, then poll the RX ring until the reply lands. */
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send(dev, ARP_OP_REQUEST, BCAST_MAC, ip);
        for (uint32_t spins = 0; spins < 20000000u; spins++) {
            if (dev->poll) dev->poll(dev);
            if (arp_cache_get(ip, mac_out) == 0) return 0;
            hal_cpu_pause();
        }
    }
    return -1;
}

/* ----------------------- ICMP --------------------------------------------- */

/* Reply-tracking state for an in-flight ping (single outstanding at a time). */
static volatile int      g_ping_active  = 0;
static volatile uint16_t g_ping_id      = 0;
static volatile uint16_t g_ping_got_seq = 0;
static volatile int      g_ping_replied = 0;

static void icmp_input(struct net_device* dev, uint32_t src_ip,
                       const uint8_t* p, uint32_t len) {
    if (len < sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr* ic = (const struct icmp_hdr*)p;

    if (ic->type == ICMP_ECHO_REQUEST) {
        /* Someone is pinging us → echo it straight back (swap type, keep
         * id/seq/payload, recompute checksum). */
        static uint8_t reply[ETH_MTU];
        if (len > sizeof(reply)) return;
        for (uint32_t i = 0; i < len; i++) reply[i] = p[i];
        struct icmp_hdr* r = (struct icmp_hdr*)reply;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = net_checksum(reply, len);
        net_ipv4_send(dev, src_ip, IP_PROTO_ICMP, reply, len);
    } else if (ic->type == ICMP_ECHO_REPLY) {
        if (g_ping_active && ntohs(ic->id) == g_ping_id) {
            g_ping_got_seq = ntohs(ic->seq);
            g_ping_replied = 1;
        }
    }
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

        g_ping_replied = 0;
        g_ping_active  = 1;

        if (net_ipv4_send(dev, ip, IP_PROTO_ICMP, msg, sizeof(msg)) != 0) {
            kprintf("  seq=%d: send failed (ARP?)\n", seq);
            g_ping_active = 0;
            continue;
        }

        /* Poll for the reply, bounded. */
        int got = 0;
        for (uint32_t spins = 0; spins < 20000000u; spins++) {
            if (dev->poll) dev->poll(dev);
            if (g_ping_replied && g_ping_got_seq == (uint16_t)seq) { got = 1; break; }
            hal_cpu_pause();
        }
        g_ping_active = 0;

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

int net_udp_bind(uint16_t port, udp_recv_fn fn, void* ctx) {
    if (!fn) { net_udp_unbind(port); return 0; }
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == port) { g_udp[i].fn = fn; g_udp[i].ctx = ctx; return 0; }
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (!g_udp[i].used) { g_udp[i].used = 1; g_udp[i].port = port; g_udp[i].fn = fn; g_udp[i].ctx = ctx; return 0; }
    return -1;
}
void net_udp_unbind(uint16_t port) {
    for (int i = 0; i < UDP_BINDINGS; i++)
        if (g_udp[i].used && g_udp[i].port == port) g_udp[i].used = 0;
}

int net_udp_send(struct net_device* dev, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 const void* payload, uint32_t len) {
    static uint8_t buf[ETH_MTU];
    uint32_t total = sizeof(struct udp_hdr) + len;
    if (total > sizeof(buf)) return -1;
    struct udp_hdr* uh = (struct udp_hdr*)buf;
    uh->src_port = htons(src_port);
    uh->dst_port = htons(dst_port);
    uh->len      = htons((uint16_t)total);
    uh->checksum = 0;                          /* omitted (legal for IPv4)   */
    const uint8_t* p = (const uint8_t*)payload;
    for (uint32_t i = 0; i < len; i++) buf[sizeof(struct udp_hdr) + i] = p[i];
    return net_ipv4_send(dev, dst_ip, IP_PROTO_UDP, buf, total);
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

    g_dns.done = 0; g_dns.ok = 0; g_dns.ip = 0;
    net_udp_bind(DNS_LOCAL_PORT, dns_recv, NULL);

    if (net_udp_send(dev, DNS_SERVER, DNS_LOCAL_PORT, DNS_PORT, q, off) != 0) {
        net_udp_unbind(DNS_LOCAL_PORT);
        return -1;
    }

    int rc = -1;
    for (uint32_t spins = 0; spins < 30000000u; spins++) {
        if (dev->poll) dev->poll(dev);
        if (g_dns.done) { rc = g_dns.ok ? 0 : -2; break; }
        hal_cpu_pause();
    }
    net_udp_unbind(DNS_LOCAL_PORT);
    if (rc == 0 && out_ip) *out_ip = g_dns.ip;
    return rc;
}

/* ----------------------- TCP (client-only, single connection) ------------- */

enum { TCP_ST_CLOSED = 0, TCP_ST_SYN_SENT, TCP_ST_ESTABLISHED, TCP_ST_CLOSING };

static struct {
    int      state;
    uint32_t peer_ip;
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
static int tcp_send_seg(struct net_device* dev, uint8_t flags,
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
    return net_ipv4_send(dev, g_tcp.peer_ip, IP_PROTO_TCP, seg, total);
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
            tcp_send_seg(dev, TCP_ACK, NULL, 0);   /* finish the handshake     */
        }
        return;
    }

    /* ESTABLISHED / CLOSING — accept in-order data, ACK it. */
    if (dlen > 0 && their_seq == g_tcp.rcv_nxt) {
        for (uint32_t i = 0; i < dlen && g_tcp_rxlen < TCP_RX_CAP; i++)
            g_tcp_rx[g_tcp_rxlen++] = data[i];
        g_tcp.rcv_nxt += dlen;
        tcp_send_seg(dev, TCP_ACK, NULL, 0);
    } else if (dlen > 0) {
        /* Duplicate / out-of-order — re-ACK what we have. */
        tcp_send_seg(dev, TCP_ACK, NULL, 0);
    }

    if (flags & TCP_FIN) {
        /* FIN occupies the seq right after its data. */
        if (their_seq + dlen == g_tcp.rcv_nxt) {
            g_tcp.rcv_nxt += 1;
            tcp_send_seg(dev, TCP_ACK, NULL, 0);
            g_tcp.peer_fin = 1;
            g_tcp.state = TCP_ST_CLOSING;
        }
    }
}

/* Bounded poll helper: pump RX for up to `spin_budget` iterations or until
 * `*flag` becomes non-zero.  Returns 1 if the flag fired. */
static int tcp_poll_until(struct net_device* dev, volatile int* flag,
                          uint32_t spin_budget) {
    for (uint32_t s = 0; s < spin_budget; s++) {
        if (dev->poll) dev->poll(dev);
        if (*flag) return 1;
        hal_cpu_pause();
    }
    return 0;
}

int net_http_get(struct net_device* dev, uint32_t ip, uint16_t port,
                 const char* host, const char* path) {
    /* Fresh connection state. */
    g_tcp.state       = TCP_ST_SYN_SENT;
    g_tcp.peer_ip     = ip;
    g_tcp.peer_port   = port;
    g_tcp.local_port  = 0xE000 + (g_tcp.local_port & 0x0FFF) + 1;  /* vary port */
    g_tcp.snd_nxt     = 0x2000;                /* our ISN                      */
    g_tcp.rcv_nxt     = 0;
    g_tcp.established  = 0;
    g_tcp.peer_fin     = 0;
    g_tcp_rxlen        = 0;

    /* Handshake: SYN → (SYN-ACK) → ACK. */
    tcp_send_seg(dev, TCP_SYN, NULL, 0);
    g_tcp.snd_nxt += 1;                         /* SYN consumes a seq          */
    if (!tcp_poll_until(dev, &g_tcp.established, 20000000u)) {
        kprintf("http: connect to %u.%u.%u.%u:%u timed out\n",
                (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF, port);
        g_tcp.state = TCP_ST_CLOSED;
        return -1;
    }

    /* Build + send the request. */
    char req[512]; int n = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\n\r\n" };
    for (int pi = 0; pi < 5; pi++)
        for (const char* c = parts[pi]; *c && n < (int)sizeof(req) - 1; c++) req[n++] = *c;
    tcp_send_seg(dev, TCP_PSH | TCP_ACK, req, (uint32_t)n);
    g_tcp.snd_nxt += (uint32_t)n;

    /* Drain the response until the peer FINs or we go quiet. */
    uint32_t last_len = 0, quiet = 0;
    for (uint32_t s = 0; s < 60000000u; s++) {
        if (dev->poll) dev->poll(dev);
        if (g_tcp.peer_fin) break;
        if (g_tcp_rxlen != last_len) { last_len = g_tcp_rxlen; quiet = 0; }
        else if (++quiet > 8000000u) break;    /* no new data for a while      */
        hal_cpu_pause();
    }

    /* Politely close our side. */
    tcp_send_seg(dev, TCP_FIN | TCP_ACK, NULL, 0);
    g_tcp.snd_nxt += 1;
    tcp_poll_until(dev, &g_tcp.peer_fin, 4000000u);
    g_tcp.state = TCP_ST_CLOSED;

    return (int)g_tcp_rxlen;
}

/* ---- general TCP client API (M24 socket API) -----------------------------
 * A minimal connect/send/recv/close over the same single-connection g_tcp
 * engine (one TCP socket at a time).  Backs SOCK_STREAM sockets in
 * usyscall.c.  Data is delivered in-order; recv drains the accumulation
 * buffer (blocking-poll until data or peer FIN). */

int net_tcp_connect(struct net_device* dev, uint32_t ip, uint16_t port) {
    g_tcp.state       = TCP_ST_SYN_SENT;
    g_tcp.peer_ip     = ip;
    g_tcp.peer_port   = port;
    g_tcp.local_port  = 0xE000 + (g_tcp.local_port & 0x0FFF) + 1;
    g_tcp.snd_nxt     = 0x2000;
    g_tcp.rcv_nxt     = 0;
    g_tcp.established  = 0;
    g_tcp.peer_fin     = 0;
    g_tcp_rxlen        = 0;
    g_tcp_rxconsumed   = 0;

    tcp_send_seg(dev, TCP_SYN, NULL, 0);
    g_tcp.snd_nxt += 1;
    if (!tcp_poll_until(dev, &g_tcp.established, 20000000u)) {
        g_tcp.state = TCP_ST_CLOSED;
        return -1;
    }
    return 0;
}

int net_tcp_send(struct net_device* dev, const void* buf, uint32_t len) {
    if (g_tcp.state != TCP_ST_ESTABLISHED && g_tcp.state != TCP_ST_CLOSING) return -1;
    /* One segment (callers send small requests; segmentation is a follow-up). */
    if (len > ETH_MTU - 40) len = ETH_MTU - 40;
    tcp_send_seg(dev, TCP_PSH | TCP_ACK, buf, len);
    g_tcp.snd_nxt += len;
    return (int)len;
}

int net_tcp_recv(struct net_device* dev, void* buf, uint32_t len) {
    /* Block-poll until unconsumed data is available or the peer closes. */
    for (uint32_t s = 0; s < 60000000u; s++) {
        if (g_tcp_rxconsumed < g_tcp_rxlen) break;
        if (g_tcp.peer_fin) break;
        if (dev->poll) dev->poll(dev);
        hal_cpu_pause();
    }
    uint32_t avail = g_tcp_rxlen - g_tcp_rxconsumed;
    if (avail == 0) return 0;                    /* EOF (peer FIN, drained) */
    uint32_t cnt = avail < len ? avail : len;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < cnt; i++) out[i] = g_tcp_rx[g_tcp_rxconsumed + i];
    g_tcp_rxconsumed += cnt;
    return (int)cnt;
}

void net_tcp_close(struct net_device* dev) {
    if (g_tcp.state == TCP_ST_CLOSED) return;
    tcp_send_seg(dev, TCP_FIN | TCP_ACK, NULL, 0);
    g_tcp.snd_nxt += 1;
    tcp_poll_until(dev, &g_tcp.peer_fin, 4000000u);
    g_tcp.state = TCP_ST_CLOSED;
}

/* Expose the accumulated response so the shell `wget` command can print it. */
const uint8_t* net_http_body(uint32_t* len_out) {
    if (len_out) *len_out = g_tcp_rxlen;
    return g_tcp_rx;
}

/* ----------------------- IPv4 --------------------------------------------- */

int net_ipv4_send(struct net_device* dev, uint32_t dst_ip, uint8_t proto,
                  const void* payload, uint32_t len) {
    /* Assemble [IPv4 header | payload] in a scratch buffer. */
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

    /* Next hop: the destination if it is on our subnet, else the gateway. */
    uint32_t nexthop = ((dst_ip & dev->netmask) == (dev->ip & dev->netmask))
                     ? dst_ip : dev->gateway;

    uint8_t mac[ETH_ALEN];
    if (net_arp_resolve(dev, nexthop, mac) != 0) return -2;

    return eth_send(dev, mac, ETHERTYPE_IPV4, pkt, total);
}

static void ipv4_input(struct net_device* dev, const uint8_t* p, uint32_t len) {
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
        case IP_PROTO_ICMP: icmp_input(dev, src, l4, l4len); break;
        case IP_PROTO_UDP:  udp_input (dev, src, l4, l4len); break;
        case IP_PROTO_TCP:  tcp_input (dev, src, l4, l4len); break;
        default: break;
    }
}

/* ----------------------- Ethernet demux (RX entry) ------------------------ */

void net_rx(struct net_device* dev, const uint8_t* frame, uint32_t len) {
    if (len < ETH_HLEN) { dev->rx_dropped++; return; }
    const struct eth_hdr* eh = (const struct eth_hdr*)frame;

    /* Accept frames addressed to us or broadcast. */
    if (!mac_eq(eh->dst, dev->mac) && !mac_eq(eh->dst, BCAST_MAC)) return;

    dev->rx_packets++; dev->rx_bytes += len;
    const uint8_t* payload = frame + ETH_HLEN;
    uint32_t plen = len - ETH_HLEN;

    switch (ntohs(eh->ethertype)) {
        case ETHERTYPE_ARP:  arp_input (dev, payload, plen); break;
        case ETHERTYPE_IPV4: ipv4_input(dev, payload, plen); break;
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
