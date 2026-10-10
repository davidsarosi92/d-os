/* =============================================================================
 * packet.c — AF_PACKET sockets, §M90 rung 5.
 *
 * WHY.  A program that has to put a frame on a link that is not IP — an
 * unsolicited ARP announcing an address, a DHCP client before it has one, a
 * packet capture — asks for the link itself: socket(AF_PACKET, ...).  dockerd
 * does exactly that when it brings a container's interface up (an unsolicited
 * ARP for the new address, so neighbours forget a stale MAC), and logs a
 * failure on every `docker run` when the family does not exist.
 *
 * WHAT IT IS.  Linux's two shapes:
 *   SOCK_RAW    the whole Ethernet frame, header included, both ways;
 *   SOCK_DGRAM  the payload only: the kernel writes the header on send from
 *               the destination in the sockaddr_ll and strips it on receive.
 * `protocol` (network order in the call, host order here) picks the
 * ethertype to receive: ETH_P_ALL (3) = every frame, 0 = none until bind
 * names one.  bind() narrows to one interface; sendto names the interface in
 * its sockaddr_ll, or the bound one is used.
 *
 * WHERE FRAMES COME FROM.  net_rx hands every frame a device ACCEPTS (its own
 * MAC or broadcast — the device is not promiscuous) to packet_tap_locked
 * before the protocol switch.  Frames this machine SENDS are not looped back
 * to ETH_P_ALL sockets as Linux does; a capture here sees the inbound half.
 * Bridge-port frames go to the bridge, not to a socket bound to the port.
 *
 * LOCKING.  The receive ring is guarded by the stack lock, like a UDP
 * socket's (usyscall.c), because the producer is the RX path that already
 * holds it; a reader waits with net_wait_cond, which is woken when frames are
 * delivered, and a poller is woken by the pump (§M55/§M56's contract).
 *
 * PRIVILEGE.  CAP_NET_RAW, both types — a packet socket can forge any frame,
 * any source address.  A socket belongs to the namespace it was made in and
 * sees only that namespace's devices.
 * ============================================================================= */

#include "packet.h"
#include "net.h"
#include "kmalloc.h"
#include "task.h"
#include "cred.h"
#include <stddef.h>
#include <stdint.h>

#define PKT_SLOTS     8
#define PKT_FRAME_MAX (ETH_HLEN + ETH_MTU)
#define PKT_MAX       32              /* open packet sockets, machine-wide */

struct pkt_frame {
    uint16_t len;
    uint16_t proto;                   /* ethertype, host order            */
    int      ifindex;
    uint8_t  pkttype;                 /* PACKET_HOST 0 / BROADCAST 1       */
    uint8_t  src[ETH_ALEN];
    uint8_t  data[PKT_FRAME_MAX];
};

struct pktsock {
    int      type;                    /* 3 = SOCK_RAW, 2 = SOCK_DGRAM      */
    uint16_t proto;                   /* what to receive, host order       */
    int      ifindex;                 /* 0 = every device of the namespace */
    int      netns;
    int      head, tail;              /* the ring, under the stack lock    */
    struct pkt_frame rx[PKT_SLOTS];
    uint32_t drops;
};

static struct pktsock* g_pkt[PKT_MAX];        /* under the stack lock */

struct pktsock* packet_create(int type, uint16_t proto_host) {
    if (type != 3 && type != 2) return NULL;
    struct pktsock* s = (struct pktsock*)kcalloc(1, sizeof *s);
    if (!s) return NULL;
    s->type  = type;
    s->proto = proto_host;
    s->netns = net_cur_ns();
    uint32_t f = net_lock();
    int i = 0;
    for (; i < PKT_MAX; i++) if (!g_pkt[i]) { g_pkt[i] = s; break; }
    net_unlock(f);
    if (i == PKT_MAX) { kfree(s); return NULL; }
    return s;
}

void packet_close(struct pktsock* s) {
    if (!s) return;
    uint32_t f = net_lock();
    for (int i = 0; i < PKT_MAX; i++) if (g_pkt[i] == s) g_pkt[i] = NULL;
    net_unlock(f);
    kfree(s);
}

int packet_bind(struct pktsock* s, uint16_t proto_host, int ifindex) {
    if (!s) return -9;
    if (ifindex && !net_by_index(s->netns, ifindex)) return -19;     /* ENODEV */
    uint32_t f = net_lock();
    s->proto   = proto_host;
    s->ifindex = ifindex;
    net_unlock(f);
    return 0;
}

void packet_name(struct pktsock* s, uint16_t* proto, int* ifindex, uint8_t mac[ETH_ALEN]) {
    if (proto) *proto = s ? s->proto : 0;
    if (ifindex) *ifindex = s ? s->ifindex : 0;
    struct net_device* d = s && s->ifindex ? net_by_index(s->netns, s->ifindex) : NULL;
    for (int i = 0; i < ETH_ALEN; i++) mac[i] = d ? d->mac[i] : 0;
}

/* The RX tap.  Stack lock held (net_rx's caller holds it). */
void packet_tap_locked(struct net_device* dev, const uint8_t* frame, uint32_t len) {
    if (len < ETH_HLEN || len > PKT_FRAME_MAX) return;
    uint16_t et = (uint16_t)(frame[12] << 8 | frame[13]);
    int bcast = 1;
    for (int i = 0; i < ETH_ALEN; i++) if (frame[i] != 0xFF) bcast = 0;
    for (int i = 0; i < PKT_MAX; i++) {
        struct pktsock* s = g_pkt[i];
        if (!s || s->netns != dev->netns) continue;
        if (s->ifindex && s->ifindex != dev->ifindex) continue;
        if (s->proto != 3 /* ETH_P_ALL */ && s->proto != et) continue;
        int nx = (s->head + 1) % PKT_SLOTS;
        if (nx == s->tail) { s->drops++; continue; }
        struct pkt_frame* p = &s->rx[s->head];
        uint32_t off = s->type == 3 ? 0 : ETH_HLEN;
        p->len = (uint16_t)(len - off);
        for (uint32_t b = 0; b < p->len; b++) p->data[b] = frame[off + b];
        p->proto = et; p->ifindex = dev->ifindex; p->pkttype = bcast ? 1 : 0;
        for (int b = 0; b < ETH_ALEN; b++) p->src[b] = frame[6 + b];
        s->head = nx;
    }
}

static int pkt_has(void* a) { struct pktsock* s = (struct pktsock*)a; return s->head != s->tail; }
int packet_can_read(struct pktsock* s) { return s && s->head != s->tail; }

/* One frame into `buf` (kernel memory).  -11 when non-blocking and empty. */
long packet_recv(struct pktsock* s, void* buf, size_t n, int block, struct packet_from* from) {
    if (!s) return -9;
    if (!block) {
        if (!pkt_has(s)) net_pump_once();
        if (!pkt_has(s)) return -11;
    } else {
        while (!net_wait_cond(pkt_has, s, 1000))
            if (task_should_stop()) return -4;
    }
    uint32_t f = net_lock();
    if (s->head == s->tail) { net_unlock(f); return -11; }
    struct pkt_frame* p = &s->rx[s->tail];
    size_t take = p->len < n ? p->len : n;
    for (size_t i = 0; i < take; i++) ((uint8_t*)buf)[i] = p->data[i];
    if (from) {
        from->proto = p->proto; from->ifindex = p->ifindex; from->pkttype = p->pkttype;
        for (int i = 0; i < ETH_ALEN; i++) from->addr[i] = p->src[i];
    }
    s->tail = (s->tail + 1) % PKT_SLOTS;
    net_unlock(f);
    return (long)take;
}

/* Send one frame.  RAW: `buf` IS the frame.  DGRAM: `buf` is the payload and
 * the header is ours, from `dst` + `proto_host` (0 = the socket's). */
long packet_send(struct pktsock* s, const void* buf, size_t n, int ifindex,
                 const uint8_t* dst, uint16_t proto_host) {
    if (!s) return -9;
    if (!ifindex) ifindex = s->ifindex;
    if (!ifindex) return -6;                                        /* ENXIO: which link? */
    uint32_t f = net_lock();
    struct net_device* d = net_by_index(s->netns, ifindex);
    if (!d) { net_unlock(f); return -19; }
    if (d->admin_down) { net_unlock(f); return -100; }              /* ENETDOWN */
    uint32_t total = s->type == 3 ? (uint32_t)n : (uint32_t)n + ETH_HLEN;
    if ((s->type == 3 && n < ETH_HLEN) || total > PKT_FRAME_MAX) { net_unlock(f); return -90; }
    static uint8_t frame[PKT_FRAME_MAX];                            /* under the stack lock */
    const uint8_t* p = (const uint8_t*)buf;
    if (s->type == 3) {
        for (size_t i = 0; i < n; i++) frame[i] = p[i];
    } else {
        if (!dst) { net_unlock(f); return -89; }                    /* EDESTADDRREQ */
        uint16_t et = proto_host ? proto_host : s->proto;
        for (int i = 0; i < ETH_ALEN; i++) { frame[i] = dst[i]; frame[6 + i] = d->mac[i]; }
        frame[12] = (uint8_t)(et >> 8); frame[13] = (uint8_t)et;
        for (size_t i = 0; i < n; i++) frame[ETH_HLEN + i] = p[i];
    }
    int r = d->transmit(d, frame, total);
    if (r == 0) { d->tx_packets++; d->tx_bytes += total; }
    net_unlock(f);
    net_kick();                          /* a virtual device delivers on the pump */
    return r == 0 ? (long)n : -105;
}

/* ---- the shell's view, and a test that makes each rule show ---------------- */
#include "shellcmd.h"
#include "printf.h"
#include "nsproxy.h"

static void cmd_packettest(const char* args) {
    (void)args;
    int ok = 1;
    #define PT(cond, what) do { int _c = (cond); kprintf("packet: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    /* Two namespaces joined by a veth: a frame sent on one end must arrive on
     * the other, to the socket bound there — and not to one in a third. */
    int A = net_ns_create(), B = net_ns_create(), C = net_ns_create();
    struct net_device *va = NULL, *vb = NULL;
    if (A <= 0 || B <= 0 || C <= 0 || netvirt_veth_create(A, "pa", B, "pb", &va, &vb) != 0) {
        kprintf("packet: FAIL (setup)\n");
        net_ns_destroy(A); net_ns_destroy(B); net_ns_destroy(C);
        return;
    }
    va->admin_down = 0; vb->admin_down = 0;
    struct task* me = task_current();
    int home = net_cur_ns();
    (void)home;
    /* sockets are made "in" a namespace: the creator's at the time */
    struct pktsock *sa, *sb, *sc, *sraw;
    sa = packet_create(2, 0x88B5);
    sb = packet_create(2, 0x88B5);
    sc = packet_create(2, 0x88B5);
    sraw = packet_create(3, 3);
    if (!sa || !sb || !sc || !sraw) { kprintf("packet: FAIL (sockets)\n"); goto out; }
    sa->netns = A; sb->netns = B; sc->netns = C; sraw->netns = B;
    PT(packet_bind(sb, 0x88B5, vb->ifindex) == 0, "bind to pb in B");
    PT(packet_bind(sa, 0x88B5, 999) == -19, "bind to a missing index is ENODEV");
    {
        static const uint8_t bc[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        const char msg[] = "d-os link-layer";
        PT(packet_send(sa, msg, sizeof msg, va->ifindex, bc, 0) == (long)sizeof msg,
           "a SOCK_DGRAM send on pa (header built by the kernel)");
        uint8_t got[64]; struct packet_from fr;
        long r = packet_recv(sb, got, sizeof got, 1, &fr);
        PT(r == (long)sizeof msg && got[0] == 'd' && fr.proto == 0x88B5 && fr.ifindex == vb->ifindex
           && fr.pkttype == 1 && fr.addr[0] == va->mac[0] && fr.addr[5] == va->mac[5],
           "it arrives on pb: payload only, sender's MAC, broadcast");
        r = packet_recv(sraw, got, sizeof got, 0, &fr);
        PT(r == (long)(sizeof msg + ETH_HLEN) && got[12] == 0x88 && got[13] == 0xB5,
           "a SOCK_RAW/ETH_P_ALL socket in B sees the whole frame");
        PT(packet_recv(sc, got, sizeof got, 0, NULL) == -11, "a socket in another namespace sees nothing");
    }
    (void)me;
out:
    packet_close(sa); packet_close(sb); packet_close(sc); packet_close(sraw);
    net_ns_destroy(A); net_ns_destroy(B); net_ns_destroy(C);
    #undef PT
    kprintf("packet: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(packettest) = { "packettest", "", "AF_PACKET: raw and datagram link-layer sockets across a veth",
                          SHELL_G_TEST, cmd_packettest, SHELL_P_ANY };
