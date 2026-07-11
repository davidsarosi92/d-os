/* =============================================================================
 * net.h — abstract network-device registry + the L2/L3 entry points (§M24.1).
 *
 * A "net device" is anything that can send and receive raw Ethernet frames.
 * Concrete implementation today: virtio-net (under QEMU).  Coming later:
 * Intel e1000 for real hardware.
 *
 * This mirrors block.h deliberately: drivers register a `struct net_device`
 * with a `transmit` (send one L2 frame) and a `poll` (pump the RX ring)
 * callback; the portable stack (Ethernet demux → ARP / IPv4 / ICMP) sits on
 * top and never talks to a specific driver.  The only coupling between a NIC
 * driver and the stack is this struct + `net_rx()`.
 *
 * Byte order: all `uint32_t ip` fields in `struct net_device` and in the
 * stack's public API are in **host** byte order (little-endian on x86); the
 * on-the-wire structs use network (big-endian) order and are converted at the
 * boundary with htons/htonl.  Keeping the API host-order means callers write
 * `net_parse_ip("10.0.2.2", &ip)` and never juggle endianness.
 *
 * Concurrency: §M24.1 drives RX by *polling from the calling task* (no IRQ,
 * no background thread) — so ARP/ping run entirely in one task context and
 * need no locking.  IRQ-driven RX + a `netd` poll task are a follow-up
 * (§M24 stage note); the interface below is already shaped for it (a driver
 * may fire `net_rx` from an ISR once that lands).
 * ============================================================================= */

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

/* ----------------------- Ethernet constants ------------------------------- */

#define ETH_ALEN         6                    /* MAC address length          */
#define ETH_HLEN         14                   /* dst(6)+src(6)+type(2)       */
#define ETH_MTU          1500
#define ETH_FRAME_MAX    (ETH_HLEN + ETH_MTU) /* 1514, no FCS (NIC adds it)  */

#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806

/* IPv4 protocol numbers we care about. */
#define IP_PROTO_ICMP    1
#define IP_PROTO_UDP     17                   /* §M24.2                      */
#define IP_PROTO_TCP     6                    /* §M24.3                      */

/* Build a host-order IPv4 address from its four octets. */
#define IPV4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)| \
                       ((uint32_t)(c)<<8)|(uint32_t)(d))

/* ----------------------- The device abstraction --------------------------- */

struct net_device {
    const char* name;                         /* e.g. "eth0"                 */
    uint8_t     mac[ETH_ALEN];

    /* L3 configuration — host byte order.  Set by the driver at register
     * time from sane defaults (QEMU SLIRP: 10.0.2.15/24 gw 10.0.2.2);
     * later tunable via `setconf net.eth0.*` (§M24 Linux-divergence note). */
    uint32_t    ip;
    uint32_t    netmask;
    uint32_t    gateway;
    uint32_t    mtu;

    /* Transmit one complete L2 frame (Ethernet header already prepended).
     * Returns 0 on success.  `len` ≤ ETH_FRAME_MAX. */
    int  (*transmit)(struct net_device* dev, const void* frame, uint32_t len);

    /* Pump the receive ring: for every frame the NIC has delivered, call
     * net_rx(dev, frame, len).  Called in a bounded loop by the stack while
     * it waits for a reply.  May be NULL for a purely IRQ-driven driver. */
    void (*poll)(struct net_device* dev);

    void* priv;                               /* driver-private state        */
    struct net_device* next;                  /* registry link               */

    /* Statistics (for `lsnic` / /proc/net). */
    uint32_t rx_packets, tx_packets;
    uint32_t rx_bytes,   tx_bytes;
    uint32_t rx_dropped;
};

/* ----------------------- Registry ----------------------------------------- */

int  net_register(struct net_device* dev);
struct net_device* net_find(const char* name);
struct net_device* net_primary(void);         /* first registered, or NULL   */
typedef void (*net_iter_fn)(struct net_device* dev, void* ctx);
void net_for_each(net_iter_fn fn, void* ctx);
void net_list(void);                          /* backs the `lsnic` command    */

/* ----------------------- RX entry (driver → stack) ------------------------ */

/* A driver calls this for each received L2 frame.  Demuxes the EtherType to
 * the ARP or IPv4 handler.  `frame` points at the Ethernet header; `len` is
 * the frame length (no virtio/NIC header, no FCS). */
void net_rx(struct net_device* dev, const uint8_t* frame, uint32_t len);

/* ----------------------- Byte-order helpers ------------------------------- */
/* x86 is little-endian; network order is big-endian. */

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Internet checksum (RFC 1071): 16-bit one's-complement sum over `len` bytes,
 * folded and inverted.  Operates on network-order data; result is stored
 * directly into the on-wire checksum field. */
uint16_t net_checksum(const void* data, uint32_t len);

/* ----------------------- L3 output ---------------------------------------- */

/* Send an IPv4 packet: prepend an IPv4 header (proto, our src, given dst),
 * resolve the next-hop MAC via ARP (gateway if `dst` is off-subnet), prepend
 * the Ethernet header and transmit.  `payload` is the L4 payload (ICMP/UDP/
 * TCP message).  Returns 0 on success, <0 on ARP failure / TX error. */
int net_ipv4_send(struct net_device* dev, uint32_t dst_ip, uint8_t proto,
                  const void* payload, uint32_t len);

/* Resolve `ip` (must be on-subnet) to a MAC, blocking-polling ARP until a
 * reply arrives or a timeout elapses.  Returns 0 + fills `mac_out` on
 * success, <0 on timeout. */
int net_arp_resolve(struct net_device* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN]);

/* ----------------------- UDP (§M24.2) ------------------------------------- */

/* Send one UDP datagram (host-order ports, IPv4 checksum omitted per RFC 768
 * — legal and what SLIRP accepts).  Returns 0 on success. */
int net_udp_send(struct net_device* dev, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 const void* payload, uint32_t len);

/* A bound receive handler: called for each datagram whose dst_port matches.
 * Only one binding per port in this first slice (enough for a stub resolver /
 * a `nc -u` test).  Passing NULL unbinds. */
typedef void (*udp_recv_fn)(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, uint32_t len, void* ctx);
int  net_udp_bind(uint16_t port, udp_recv_fn fn, void* ctx);
void net_udp_unbind(uint16_t port);

/* ----------------------- DNS stub resolver (§M24.2) ----------------------- */

/* Resolve `hostname` to an IPv4 address via the SLIRP DNS proxy (10.0.2.3),
 * blocking-polling for the reply.  Returns 0 + fills `out_ip` (host order) on
 * success, <0 on failure/timeout.  The precursor to getaddrinfo (§M39). */
int net_dns_query(struct net_device* dev, const char* hostname, uint32_t* out_ip);

/* ----------------------- TCP + HTTP (§M24.3) ------------------------------ */

/* Open a TCP connection to (ip:port), send a minimal HTTP/1.0 GET for `path`
 * (Host: `host`, Connection: close), stream the response to the console, then
 * close.  Single connection at a time, client-only.  Returns the number of
 * response bytes received, or <0 on connect failure.
 *
 * Simplifications (this first slice, safe on QEMU SLIRP's lossless local
 * link): no congestion control, no retransmit timers, in-order delivery only.
 * A proper socket-backed TCP with timers is §M24 stage 5-6 / §M25. */
int net_http_get(struct net_device* dev, uint32_t ip, uint16_t port,
                 const char* host, const char* path);

/* Access the last response body accumulated by net_http_get (for the shell
 * `wget` command to print).  Returns the buffer; *len_out gets its length. */
const uint8_t* net_http_body(uint32_t* len_out);

/* ----------------------- ICMP echo (ping) --------------------------------- */

/* Send `count` ICMP echo requests to `ip`, polling for replies.  Prints a
 * line per reply.  Returns the number of replies received. */
int net_ping(struct net_device* dev, uint32_t ip, int count);

/* ----------------------- Address parse / format --------------------------- */

/* Parse dotted-quad "a.b.c.d" → host-order uint32.  Returns 0 on success. */
int  net_parse_ip(const char* s, uint32_t* out);
/* Format a host-order IPv4 into "a.b.c.d" (buf ≥ 16 bytes). */
void net_fmt_ip(uint32_t ip, char* buf);
/* Format a MAC into "aa:bb:cc:dd:ee:ff" (buf ≥ 18 bytes).  The kernel printf
 * has no width/zero-pad, so callers format-then-print with %s. */
void net_fmt_mac(const uint8_t* mac, char* buf);

#endif /* NET_H */
