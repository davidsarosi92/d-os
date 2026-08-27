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
 * Concurrency (§M55): ONE poller task (`netd`) is the only caller of
 * `dev->poll`, and every other task BLOCKS on the stack's wait queue until the
 * state it cares about changes or a real deadline passes.  `net_rx` and
 * everything under it run with the stack lock held and must never block.  The
 * full rationale — and the rule about `_locked` vs unlocked send helpers — is
 * in net.c's header.  A driver may fire `net_rx` from an ISR once the NIC
 * interrupt lands; it must take the stack lock to do so.
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

/* Device flags. */
/* §M24.8 — a LOOPBACK device delivers what it is given straight back to the
 * stack.  The flag is not decoration: two rules key off it.  (1) Its peers are
 * never resolved — there is no wire and therefore no ARP, and asking would
 * block forever for an answer that cannot come.  (2) It is never the DEFAULT
 * route: `net_primary()` skips it, so every caller written before loopback
 * existed keeps meaning "the device that reaches the outside world" even though
 * the registry now has two entries. */
#define NETDEV_F_LOOPBACK  0x00000001u

struct net_device {
    const char* name;                         /* e.g. "eth0"                 */
    uint32_t    flags;                        /* NETDEV_F_*                  */
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

/* Withdraw a device (§M67 — a loadable driver must be removable; the registry
 * would otherwise keep a pointer into memory `rmmod` has freed).  0 on success,
 * -1 if it was not registered.  See the note at the definition for what it does
 * NOT do about concurrent users. */
int  net_unregister(struct net_device* dev);
struct net_device* net_find(const char* name);

/* The DEFAULT-ROUTE device: the first registered non-loopback one, or NULL.
 *
 * The definition changed with §M24.8 and the wording matters.  It used to be
 * "the first registered device", which was the same thing while `eth0` was the
 * only device there could be; the moment `lo` joined the registry that reading
 * would have handed every existing caller — ping, DNS, wget, NetSurf's fetcher
 * — a device that cannot reach anything, purely on registration order.  A
 * loopback interface is never a route to the world, so it is never primary. */
struct net_device* net_primary(void);

/* Pick the device a packet for `dst_ip` must leave through: the loopback for
 * 127.0.0.0/8, otherwise the first device whose subnet contains `dst_ip`,
 * otherwise the default-route device.  Returns NULL if nothing can carry it.
 *
 * This is the whole routing table.  It is a FUNCTION rather than a table
 * because with two devices — one loopback, one NIC — a table would have
 * exactly two rows that can never be edited; when a second NIC or a static
 * route arrives, this is the one place that grows. */
struct net_device* net_route(uint32_t dst_ip);

/* Is `ip` an address this host answers to?  Used by the TCP layer to decide
 * whether a passive socket bound to a specific address should take a segment. */
int net_is_local_ip(uint32_t ip);
typedef void (*net_iter_fn)(struct net_device* dev, void* ctx);
void net_for_each(net_iter_fn fn, void* ctx);
void net_list(void);                          /* backs the `lsnic` command    */
void net_procfs_init(void);                   /* /proc/net/{dev,arp,route,tcp,stat} */

/* ----------------------- RX entry (driver → stack) ------------------------ */

/* A driver calls this for each received L2 frame.  Demuxes the EtherType to
 * the ARP or IPv4 handler.  `frame` points at the Ethernet header; `len` is
 * the frame length (no virtio/NIC header, no FCS). */
void net_rx(struct net_device* dev, const uint8_t* frame, uint32_t len);

/* A NIC driver's ISR calls this to say "there may be work on the RX ring".
 *
 * It does NOT deliver a frame, and that is the point: the ISR must not drain
 * the ring itself, because draining calls net_rx, which can generate a TCP ACK,
 * which spins waiting on the TX virtqueue.  All this does is wake the poller.
 * The driver still owns acknowledging its own interrupt before calling.
 *
 * The stack learns that interrupts work by RECEIVING one — a driver that wires
 * an interrupt which never fires leaves the poller in its timed fallback rather
 * than blocking forever on a promise. */
void net_rx_irq(struct net_device* dev);

/* "Nothing right now, ask again" — the stream/datagram answer that is NOT an
 * error and NOT an end of file.  Kept as a net.h constant rather than the
 * syscall layer's errno so the stack does not have to know what a syscall is;
 * usyscall.c translates it into EAGAIN at the boundary. */
#define NET_EAGAIN  (-2)

/* ----------------------- Loopback loss injection (a test instrument) ------ */

/* Make the loopback device DROP a share of the frames handed to it (per
 * thousand; 0 = a perfect link).  Declared here rather than in a driver header
 * because it is part of how this subsystem is tested, and a test instrument
 * that is hard to find is one nobody uses: without a way to lose a packet, a
 * retransmission timer is a feature no test in the build can falsify. */
/* §M67 — WEAK, because loopback ships as a loadable MODULE now.
 *
 * These two are the `lo drop` test surface, and they are the only place in the
 * tree where the kernel reaches INTO a driver by name.  That direction of
 * dependency is exactly what a module cannot satisfy: the kernel is linked
 * first, so a strong reference to a symbol that lives in a .ko does not link at
 * all.  (It did not — this is what the first attempt at making loopback a
 * module failed on.)
 *
 * Weak turns "must be present at link time" into "may be present at run time",
 * which is the truth: `lo drop` works when the module is loaded and says so
 * when it is not.  Every caller MUST check for NULL — a weak symbol that is
 * absent is a call through zero. */
void loopback_set_drop(uint32_t permille) __attribute__((weak));
void loopback_stats(uint32_t* drop_permille, uint32_t* injected, uint32_t* qfull)
    __attribute__((weak));

/* ----------------------- Waiting for the network (§M55) ------------------- */

/* Take / release the stack lock.  A consumer that keeps its own state inside
 * the stack (a socket's RX ring, say) must hold this while it looks at that
 * state, because the RX path fills it with the lock held.  Returns the saved
 * IRQ flags — pass them back verbatim. */
uint32_t net_lock(void);
void     net_unlock(uint32_t flags);

/* Block until `cond` reports true or `timeout_ms` elapses; returns 1 if the
 * condition became true.  `cond` is evaluated WITH THE STACK LOCK HELD, so it
 * may read stack state freely but must not take the lock or block.  Waiting
 * here also starts the poller, so a caller never has to think about it. */
int net_wait_cond(int (*cond)(void* arg), void* arg, uint32_t timeout_ms);

/* Say that this task is waiting for network state to change (and, when the
 * last one leaves, that nobody is).  net_wait_cond does this itself; a task
 * blocked in poll()/epoll_wait() on a socket must do it explicitly, or the
 * poller stays parked and the frame it is waiting for is never collected. */
void net_waiter_enter(void);
void net_waiter_leave(void);

/* Give the device exactly one chance to deliver, for a caller that will NOT
 * wait (a non-blocking recv).  This exists so "pump once" stays a stack
 * operation: a bare dev->poll() from another file is a second poller. */
void net_pump_once(void);

/* Poller counters for `lsnic` — a change to the concurrency model is only
 * worth making if it can be measured afterwards.  Several of these fields are
 * kept apart rather than summed on purpose:
 *
 *   inline_pumps  pumps NOT driven by netd.  A rising figure means the poller
 *                 is not doing its job, which should be visible rather than
 *                 inferred from a total.
 *   irqs          "the interrupt is wired" and "the interrupt is delivering"
 *                 are different claims; a zero here answers the second.
 *   backstops     waits that ended on the poller's own timer instead.  On a
 *                 quiet wire this is the DESIGN, not a defect: with nothing
 *                 coming, the timer is the only thing that can wake it.
 *   missed_irqs   backstops whose next pump found frames waiting.  THIS is
 *                 the fault reading: the data was there and nothing said so. */
struct net_poller_stats {
    uint32_t pumps, inline_pumps, frames;
    uint32_t irqs, backstops, missed_irqs;
    int      waiters, running, irq_live;
};
void net_poller_stats(struct net_poller_stats* out);

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
/* Broadcast a datagram from an explicit source address — the shape DHCP needs
 * before this host has an address of its own (see net.c). */
int net_udp_broadcast(struct net_device* dev, uint32_t src_ip,
                      uint16_t src_port, uint16_t dst_port,
                      const void* payload, uint32_t len);

/* The resolver's nameserver.  Settable because DHCP learns it (option 6). */
void     net_set_dns(uint32_t ip);
uint32_t net_get_dns(void);

typedef void (*udp_recv_fn)(uint32_t src_ip, uint16_t src_port,
                            const uint8_t* data, uint32_t len, void* ctx);
int  net_udp_bind(uint16_t port, udp_recv_fn fn, void* ctx);
void net_udp_unbind(uint16_t port);

/* ----------------------- DNS stub resolver (§M24.2) ----------------------- */

/* Resolve `hostname` to an IPv4 address via the SLIRP DNS proxy (10.0.2.3),
 * blocking-polling for the reply.  Returns 0 + fills `out_ip` (host order) on
 * success, <0 on failure/timeout.  The precursor to getaddrinfo (§M39). */
int net_dns_query(struct net_device* dev, const char* hostname, uint32_t* out_ip);

/* ----------------------- TCP (§M24.3 + §M24.9/.10/.11) -------------------- */

/* A connection is an OPAQUE handle.  Callers hold a pointer and never learn
 * what is behind it, which is what let the single `g_tcp` become a table
 * without touching a line of the socket layer's logic.
 *
 * LIFETIME, stated once so nobody has to derive it: the handle is valid from
 * the call that produced it (connect / listen / accept) until the matching
 * `net_tcp_close`.  The RX path never releases one — it only changes state —
 * so a handle cannot go stale underneath a socket that still exists. */
struct tcp_conn;

/* Open a connection.  Picks its own device via net_route(), resolves the next
 * hop once, and blocks until the handshake completes or `timeout_ms` passes
 * (0 = the default).  Returns NULL on refusal (an RST arrives promptly, so a
 * closed port fails fast rather than at the timeout) or on timeout. */
struct tcp_conn* net_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);

/* Passive open.  `local_ip` 0 means every local address — a listener has no
 * device of its own, because each connection it accepts takes the device its
 * SYN arrived on.  Returns NULL if the port is already in use. */
struct tcp_conn* net_tcp_listen(uint32_t local_ip, uint16_t port, int backlog);

/* Take the next established connection off a listener's backlog, waiting up to
 * `timeout_ms` (0 = do not wait).  Returns NULL if none arrived. */
struct tcp_conn* net_tcp_accept(struct tcp_conn* l, uint32_t timeout_ms);
int  net_tcp_pending(struct tcp_conn* l);   /* how many are queued (readiness) */

/* Queue bytes for transmission.  Returns the number ACCEPTED (which may be
 * less than `len` — the send buffer is finite), NET_EAGAIN if a non-blocking
 * socket would have had to wait, or -1 on a broken connection. */
int  net_tcp_send(struct tcp_conn* c, const void* buf, uint32_t len, int nonblock);

/* Read.  >0 = bytes, 0 = the peer closed AND the buffer is drained, NET_EAGAIN
 * = nothing arrived within the timeout (or would have blocked), -1 = the
 * connection is broken.  The timeout and the end of stream are DIFFERENT
 * answers: the first slice returned 0 for both, which made a slow server
 * indistinguishable from a finished one. */
int  net_tcp_recv(struct tcp_conn* c, void* buf, uint32_t len, int nonblock,
                  uint32_t timeout_ms);

void net_tcp_shutdown(struct tcp_conn* c);   /* half-close: FIN, keep the fd */
void net_tcp_close(struct tcp_conn* c);

/* Readiness for poll/epoll — every field sampled in ONE locked read, because a
 * caller that read them separately could see "no data" and "no FIN" from two
 * different instants and conclude the connection was merely quiet (§M56).
 * `reset` distinguishes an RST from a FIN: an orderly EOF from a broken
 * connection, which is the difference between POLLHUP and POLLERR, and between
 * "the server answered nothing" and "the server refused you".  Any pointer may
 * be NULL. */
void net_tcp_state(struct tcp_conn* c, int* readable, int* writable,
                   int* peer_fin, int* reset, int* connected);

void net_tcp_peer (struct tcp_conn* c, uint32_t* ip, uint16_t* port);
void net_tcp_local(struct tcp_conn* c, uint32_t* ip, uint16_t* port);

/* Diagnostics — backs `netstat` and /proc/net/tcp.  A per-connection retransmit
 * counter is not decoration: it is the only way to tell a link that recovered
 * from one that never lost anything, which is what makes the loss test a test. */
struct net_tcp_info {
    const char* state;
    const char* dev;
    uint32_t local_ip, peer_ip;
    uint16_t local_port, peer_port;
    uint32_t rx_queued, tx_queued;
    uint32_t tx_segs, rx_segs, retrans;
    int      listener;
};
typedef void (*net_tcp_iter_fn)(const struct net_tcp_info* info, void* ctx);
void net_tcp_foreach(net_tcp_iter_fn fn, void* ctx);
void net_tcp_counters(uint32_t* rst_sent, uint32_t* rst_rcvd, uint32_t* noconn);
void net_tcp_timing_counters(uint32_t* persists, uint32_t* rtos, uint32_t* zerowin);
void net_tcp_output_counters(uint32_t* datasegs, uint32_t* wndblocked);
int  net_tcp_capacity(void);      /* table size — a full table refuses (RST) */

/* ----------------------- HTTP helper (the `wget` engine) ------------------ */

/* Open a TCP connection to (ip:port), send a minimal HTTP/1.0 GET for `path`
 * (Host: `host`, Connection: close), accumulate the response, then close.
 * Returns the number of response bytes received, or <0 on connect failure.
 * `dev` is ignored — the route decides which device carries it — and is kept
 * only so the shell's existing call sites read the same.
 *
 * The response is accumulated in the helper's OWN buffer rather than in the
 * connection's receive ring.  They used to be one object, which is why a
 * response larger than the ring could not be received at all: nothing drained
 * it, so the window went to zero and the transfer stopped there. */
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
