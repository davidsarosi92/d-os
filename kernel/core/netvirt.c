/* =============================================================================
 * netvirt.c — VIRTUAL network devices: a namespace's loopback, veth pairs and
 * bridges.  §M90 rung 5, 2026-10-10.
 *
 * WHY.  A container gets its own network namespace (net.c) and needs a way out
 * of it.  Docker's default network is Linux's: a BRIDGE `docker0` in the
 * machine's namespace with an address (172.17.0.1/16), and per container a
 * VETH PAIR — one end a port of the bridge, the other moved into the
 * container's namespace and renamed eth0.  These are those three device kinds,
 * as net_device instances the stack cannot tell from hardware.
 *
 * DELIVERY IS QUEUED, NEVER NESTED.  A device's transmit is called with the
 * stack lock held, from inside the stack's emit path — whose frame lives in a
 * static buffer (g_txframe).  Delivering a frame by calling net_rx() on the
 * receiving device from there would run the receiver's stack (which may reply,
 * through that same static buffer) underneath the sender's.  So a transmit
 * COPIES the frame into the receiving device's queue, and the device's poll —
 * called by the stack's pump like any NIC's — hands it to net_rx().  That is
 * the loopback driver's shape (kernel/drivers/net/loopback.c), per instance.
 * net_kick() wakes the pump when it waits for an interrupt.
 *
 *   loopback   transmit → its own queue.  Born DOWN in a new namespace, as on
 *              Linux; the runtime raises it.
 *   veth       transmit → the PEER's queue.  Destroying either end destroys
 *              both; a frame sent while the peer is down is dropped (counted).
 *   bridge     a learning switch.  A frame arriving on a PORT (a device whose
 *              `master` is the bridge — net_rx hands it to port_rx) teaches the
 *              bridge where its source MAC lives, then goes to the port that
 *              owns its destination, or to every other port when unknown or
 *              broadcast; a frame for the bridge's own MAC (or a broadcast) is
 *              also delivered to the bridge's own stack, which is how the host
 *              talks to its containers at 172.17.0.1.  The bridge's own
 *              transmit switches the same way.
 *
 * WHAT IT IS NOT: no STP, no VLANs, no hairpin mode, no IGMP snooping, no MTU
 * enforcement between ports (all are 1500 here).  Forwarding database entries
 * age out after 300 s, as Linux's default.
 * ============================================================================= */

#include "net.h"
#include "nft_hook.h"   /* §M90 3b — br_netfilter */
#include "nftables.h"  /* brnftest feeds nft_netlink */
#include "procfs.h"
#include "kmalloc.h"
#include "printf.h"
#include "timer.h"
#include <stdint.h>
#include <stddef.h>

#define VQ_SLOTS   64
#define VNAME_MAX  16
#define FDB_MAX    64

struct vframe { uint32_t len; uint8_t data[ETH_FRAME_MAX]; };

struct vdev {
    struct net_device dev;              /* FIRST: the stack's pointer is ours */
    char              name[VNAME_MAX];
    struct vdev*      peer;             /* veth */
    struct vframe*    q;                /* frames delivered TO this device   */
    uint32_t          qh, qt, qdrop;
    struct { uint8_t mac[ETH_ALEN]; struct net_device* port; int used; uint64_t ms; } fdb[FDB_MAX];
    struct vdev*      vnext;            /* every virtual device */
};

static struct vdev* g_vdevs;
static uint32_t     g_mac_seq = 1;

static struct vdev* V(struct net_device* d) { return (struct vdev*)d; }
static int is_virtual(struct net_device* d) {
    for (struct vdev* v = g_vdevs; v; v = v->vnext) if (&v->dev == d) return 1;
    return 0;
}

static void s_cpy(char* d, const char* s, size_t cap) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}
static int mac_eq(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < ETH_ALEN; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int mac_bcast(const uint8_t* a) { return (a[0] & 1) != 0; }   /* broadcast or multicast */

/* A locally administered, unique MAC (02:42 like Docker's, then a counter). */
static void new_mac(uint8_t* m) {
    uint32_t n = g_mac_seq++;
    m[0] = 0x02; m[1] = 0x42; m[2] = 0xd0;
    m[3] = (uint8_t)(n >> 16); m[4] = (uint8_t)(n >> 8); m[5] = (uint8_t)n;
}

/* ---- the per-device queue (stack lock held) -------------------------------- */

static int vq_put(struct vdev* to, const void* frame, uint32_t len) {
    if (!to || !to->q || len > ETH_FRAME_MAX) return -1;
    uint32_t nx = (to->qh + 1) % VQ_SLOTS;
    if (nx == to->qt) { to->qdrop++; to->dev.rx_dropped++; return -1; }
    const uint8_t* p = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) to->q[to->qh].data[i] = p[i];
    to->q[to->qh].len = len;
    to->qh = nx;
    net_kick();
    return 0;
}

static void v_poll(struct net_device* d) {
    struct vdev* v = V(d);
    uint32_t budget = VQ_SLOTS;
    while (v->qt != v->qh && budget--) {
        struct vframe* f = &v->q[v->qt];
        v->qt = (v->qt + 1) % VQ_SLOTS;
        net_rx(d, f->data, f->len);
    }
}

/* ---- loopback ------------------------------------------------------------------ */

static int lo_tx(struct net_device* d, const void* frame, uint32_t len) {
    d->tx_packets++; d->tx_bytes += len;
    return vq_put(V(d), frame, len);
}

/* ---- veth ---------------------------------------------------------------------- */

static int veth_tx(struct net_device* d, const void* frame, uint32_t len) {
    struct vdev* p = V(d)->peer;
    if (!p || p->dev.admin_down) { d->rx_dropped++; return -1; }
    d->tx_packets++; d->tx_bytes += len;
    return vq_put(p, frame, len);
}

/* ---- bridge -------------------------------------------------------------------- */

/* Entries AGE OUT after 300 s without traffic from that MAC (Linux's default
 * ageing_time): a station that moved to another port is then flooded and
 * relearned instead of being sent to its old port forever, and a full table
 * reuses its oldest entry. */
#define FDB_AGE_MS 300000u
static void fdb_learn(struct vdev* br, const uint8_t* mac, struct net_device* port) {
    if (mac_bcast(mac)) return;
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < FDB_MAX; i++)
        if (br->fdb[i].used && mac_eq(br->fdb[i].mac, mac)) { br->fdb[i].port = port; br->fdb[i].ms = now; return; }
    int slot = -1;
    uint64_t oldest = ~0ull;
    for (int i = 0; i < FDB_MAX; i++) {
        if (br->fdb[i].used && now - br->fdb[i].ms > FDB_AGE_MS) br->fdb[i].used = 0;
        if (!br->fdb[i].used) { slot = i; break; }
        if (br->fdb[i].ms < oldest) { oldest = br->fdb[i].ms; slot = i; }
    }
    if (slot < 0) return;
    br->fdb[slot].used = 1; br->fdb[slot].port = port; br->fdb[slot].ms = now;
    for (int k = 0; k < ETH_ALEN; k++) br->fdb[slot].mac[k] = mac[k];
}
static struct net_device* fdb_find(struct vdev* br, const uint8_t* mac) {
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < FDB_MAX; i++) {
        if (!br->fdb[i].used || !mac_eq(br->fdb[i].mac, mac)) continue;
        if (now - br->fdb[i].ms > FDB_AGE_MS) { br->fdb[i].used = 0; return NULL; }   /* aged out */
        return br->fdb[i].port;
    }
    return NULL;
}
static void fdb_forget_port(struct vdev* br, struct net_device* port) {
    for (int i = 0; i < FDB_MAX; i++) if (br->fdb[i].port == port) br->fdb[i].used = 0;
}

/* Out of every port of `br` except `from` (NULL = none excluded). */
static void br_flood(struct vdev* br, struct net_device* from, const void* frame, uint32_t len) {
    for (struct vdev* v = g_vdevs; v; v = v->vnext)
        if (v->dev.master == &br->dev && &v->dev != from && !v->dev.admin_down)
            v->dev.transmit(&v->dev, frame, len);
}

/* ---- br_netfilter (§M90 3b) ----------------------------------------------------
 *
 * /proc/sys/net/bridge/bridge-nf-call-iptables and -ip6tables, per namespace
 * (Linux's since 5.3), default 1 as when br_netfilter is loaded: an IPv4 (or
 * IPv6) frame BRIDGED from one port to another passes the IP hooks as Linux's
 * br_netfilter passes it — PREROUTING, FORWARD, POSTROUTING, with the BRIDGE
 * as the logical in and out device (iifname/oifname read "docker0"), so
 * Docker's per-network FORWARD chains and its inter-container rules apply to
 * container-to-container traffic.  A destination changed by DNAT in
 * PREROUTING is no longer the bridge's to deliver: the frame goes up to the
 * bridge's IP layer, which routes it (Linux re-routes it the same way) —
 * which is how a container reaches a port published on the host and DNATed
 * to another container.  Only unicast frames between ports are hooked;
 * broadcast/multicast are flooded unfiltered (stated), and frames for the
 * host itself meet the hooks in the host's own IP input.  Docker refuses to
 * run without the userland proxy (or with icc=false) if these files are
 * missing. */
static uint8_t g_brnf[2][NET_NS_MAX];        /* [0] iptables, [1] ip6tables; 1 = call */
static uint8_t g_brnf_set[NET_NS_MAX];       /* the namespace's values were initialised */
static uint32_t g_brnf_dropped, g_brnf_up;
static int brnf_on(int ns, int v6) {
    if (ns < 0 || ns >= NET_NS_MAX) return 0;
    if (!g_brnf_set[ns]) { g_brnf_set[ns] = 1; g_brnf[0][ns] = g_brnf[1][ns] = 1; }
    return g_brnf[v6][ns];
}
/* 1 = the frame continues to be bridged; 0 = it was dropped or handed up. */
static int br_nf_bridged(struct net_device* bd, uint8_t* fr, uint32_t len) {
    int ns = bd->netns;
    uint16_t et = (uint16_t)(fr[12] << 8 | fr[13]);
    int fam = et == 0x0800 ? 2 : et == 0x86dd ? 10 : 0;
    if (!fam || !brnf_on(ns, fam == 10) || !nft_active(ns)) return 1;
    uint8_t* ip = fr + ETH_HLEN;
    uint32_t il = len - ETH_HLEN;
    uint8_t dst0[16];
    int an = fam == 10 ? 16 : 4, doff = fam == 10 ? 24 : 16;
    if (il < (uint32_t)(doff + an)) return 1;
    for (int i = 0; i < an; i++) dst0[i] = ip[doff + i];
    if (!nft_ip_hook(ns, fam, NFT_HOOK_PREROUTING, bd, NULL, ip, il)) { g_brnf_dropped++; return 0; }
    for (int i = 0; i < an; i++)
        if (ip[doff + i] != dst0[i]) {                /* DNATed: the IP layer routes it */
            for (int k = 0; k < ETH_ALEN; k++) fr[k] = bd->mac[k];
            g_brnf_up++;
            bd->rx_packets++; bd->rx_bytes += len;
            net_rx(bd, fr, len);
            return 0;
        }
    if (!nft_ip_hook(ns, fam, NFT_HOOK_FORWARD, bd, bd, ip, il) ||
        !nft_ip_hook(ns, fam, NFT_HOOK_POSTROUTING, bd, bd, ip, il)) { g_brnf_dropped++; return 0; }
    return 1;
}

void netvirt_ns_reset(int ns) { if (ns > 0 && ns < NET_NS_MAX) g_brnf_set[ns] = 0; }
static void brnf_gen(struct procfs_writer* w, const struct procfs_node* self) {
    int ns = net_cur_ns();
    pw_put_uint(w, (unsigned)brnf_on(ns, self->priv != NULL)); pw_putc(w, '\n');
}
static long brnf_write(const struct procfs_node* self, const char* buf, size_t len) {
    int ns = net_cur_ns();
    if (!len || (buf[0] != '0' && buf[0] != '1') || ns < 0 || ns >= NET_NS_MAX) return -22;
    brnf_on(ns, 0);                                   /* initialise the namespace's pair */
    g_brnf[self->priv != NULL][ns] = (uint8_t)(buf[0] - '0');
    return (long)len;
}
static struct procfs_node nd_brnf4 = { .name = "sys/net/bridge/bridge-nf-call-iptables",
                                       .gen2 = brnf_gen, .write2 = brnf_write };
static struct procfs_node nd_brnf6 = { .name = "sys/net/bridge/bridge-nf-call-ip6tables",
                                       .gen2 = brnf_gen, .write2 = brnf_write, .priv = (void*)1 };
void netvirt_procfs_init(void) { procfs_register(&nd_brnf4); procfs_register(&nd_brnf6); }

/* A frame that arrived on one of the bridge's ports (net_rx → port_rx). */
static void br_port_rx(struct net_device* bd, struct net_device* port, const uint8_t* frame, uint32_t len) {
    struct vdev* br = V(bd);
    if (bd->admin_down || len < ETH_HLEN) return;
    const uint8_t* dst = frame;
    const uint8_t* src = frame + ETH_ALEN;
    fdb_learn(br, src, port);
    if (mac_eq(dst, bd->mac)) {                     /* for the host itself */
        bd->rx_packets++; bd->rx_bytes += len;
        net_rx(bd, frame, len);
        return;
    }
    if (mac_bcast(dst)) {
        br_flood(br, port, frame, len);
        bd->rx_packets++; bd->rx_bytes += len;
        net_rx(bd, frame, len);                     /* the host hears broadcasts */
        return;
    }
    /* §M90 3b — bridged unicast meets the IP hooks (br_netfilter) on a
     * writable copy: NAT may rewrite it.  Static: the RX path holds the stack
     * lock, and the transmits below copy the frame into the port's queue. */
    static uint8_t brfr[ETH_FRAME_MAX];
    if (len <= sizeof brfr && nft_active(bd->netns)) {
        for (uint32_t i = 0; i < len; i++) brfr[i] = frame[i];
        if (!br_nf_bridged(bd, brfr, len)) return;
        frame = brfr;
        dst = frame;
    }
    struct net_device* out = fdb_find(br, dst);
    if (out && out != port) out->transmit(out, frame, len);
    else if (!out) br_flood(br, port, frame, len);
}

/* The host sending through the bridge (its own address). */
static int br_tx(struct net_device* bd, const void* frame, uint32_t len) {
    struct vdev* br = V(bd);
    bd->tx_packets++; bd->tx_bytes += len;
    const uint8_t* dst = (const uint8_t*)frame;
    if (!mac_bcast(dst)) {
        struct net_device* out = fdb_find(br, dst);
        if (out) return out->transmit(out, frame, len);
    }
    br_flood(br, NULL, frame, len);
    return 0;
}

static void br_poll(struct net_device* d) { (void)d; }   /* fed synchronously by its ports */

/* ---- creation / destruction ------------------------------------------------------ */

static struct vdev* vdev_new(int ns, const char* name, uint32_t kind) {
    struct vdev* v = (struct vdev*)kcalloc(1, sizeof *v);
    if (!v) return NULL;
    if (kind != NETDEV_K_BRIDGE) {
        v->q = (struct vframe*)kmalloc(sizeof(struct vframe) * VQ_SLOTS);
        if (!v->q) { kfree(v); return NULL; }
    }
    s_cpy(v->name, name, sizeof v->name);
    v->dev.name = v->name;
    v->dev.netns = ns;
    v->dev.kind = kind;
    v->dev.mtu = ETH_MTU;
    v->dev.admin_down = 1;                          /* born down, as on Linux */
    v->dev.poll = kind == NETDEV_K_BRIDGE ? br_poll : v_poll;
    new_mac(v->dev.mac);
    return v;
}

static void vdev_link(struct vdev* v) {
    uint32_t f = net_lock();
    v->vnext = g_vdevs;
    g_vdevs = v;
    net_unlock(f);
    net_register(&v->dev);
}

struct net_device* netvirt_lo_create(int ns) {
    struct vdev* v = vdev_new(ns, "lo", NETDEV_K_LOOPBACK);
    if (!v) return NULL;
    v->dev.flags = NETDEV_F_LOOPBACK;
    for (int i = 0; i < ETH_ALEN; i++) v->dev.mac[i] = 0;
    v->dev.ip = IPV4(127, 0, 0, 1);
    v->dev.netmask = IPV4(255, 0, 0, 0);
    v->dev.transmit = lo_tx;
    vdev_link(v);
    return &v->dev;
}

/* A veth pair: `name` in namespace `ns`, `peer_name` in `peer_ns`.  0 / -1. */
int netvirt_veth_create(int ns, const char* name, int peer_ns, const char* peer_name,
                        struct net_device** a_out, struct net_device** b_out) {
    if (net_find_ns(ns, name) || net_find_ns(peer_ns, peer_name)) return -2;   /* EEXIST */
    struct vdev* a = vdev_new(ns, name, NETDEV_K_VETH);
    struct vdev* b = vdev_new(peer_ns, peer_name, NETDEV_K_VETH);
    if (!a || !b) {
        if (a) { kfree(a->q); kfree(a); }
        if (b) { kfree(b->q); kfree(b); }
        return -1;
    }
    a->peer = b; b->peer = a;
    a->dev.transmit = veth_tx; b->dev.transmit = veth_tx;
    vdev_link(a);
    vdev_link(b);
    if (a_out) *a_out = &a->dev;
    if (b_out) *b_out = &b->dev;
    return 0;
}

struct net_device* netvirt_bridge_create(int ns, const char* name) {
    if (net_find_ns(ns, name)) return NULL;
    struct vdev* v = vdev_new(ns, name, NETDEV_K_BRIDGE);
    if (!v) return NULL;
    v->dev.transmit = br_tx;
    v->dev.port_rx = br_port_rx;
    vdev_link(v);
    return &v->dev;
}

/* Make `port` a port of `bridge` (NULL = leave its bridge).  -1 = not a
 * bridge, or the two are in different namespaces. */
int netvirt_set_master(struct net_device* port, struct net_device* bridge) {
    if (!port) return -1;
    if (bridge && (!bridge->port_rx || bridge->netns != port->netns || bridge == port)) return -1;
    uint32_t f = net_lock();
    if (port->master && is_virtual(port->master)) fdb_forget_port(V(port->master), port);
    port->master = bridge;
    net_unlock(f);
    return 0;
}

int netvirt_rename(struct net_device* dev, const char* name) {
    if (!dev || !is_virtual(dev) || !name || !name[0]) return -1;
    struct net_device* other = net_find_ns(dev->netns, name);
    if (other && other != dev) return -2;
    s_cpy(V(dev)->name, name, VNAME_MAX);
    net6_procfs_dev(dev);                           /* §M90 3a — conf/<new name>/ */
    net4_procfs_dev(dev);                           /* §M90 3b — ipv4/conf/<new name>/ */
    return 0;
}

static void vdev_free_one(struct vdev* v) {
    uint32_t f = net_lock();
    /* off every bridge it was a port of, and every port off it */
    for (struct vdev* o = g_vdevs; o; o = o->vnext) {
        if (o->dev.master == &v->dev) o->dev.master = NULL;
        if (v->dev.master == &o->dev) fdb_forget_port(o, &v->dev);
    }
    struct vdev** pp = &g_vdevs;
    while (*pp && *pp != v) pp = &(*pp)->vnext;
    if (*pp) *pp = v->vnext;
    if (v->peer) v->peer->peer = NULL;
    net_unlock(f);
    net_unregister(&v->dev);
    kfree(v->q);
    kfree(v);
}

/* Destroy a virtual device (a veth takes its peer along).  -1 = hardware. */
int netvirt_destroy(struct net_device* dev) {
    if (!dev || !is_virtual(dev)) return -1;
    struct vdev* v = V(dev);
    struct vdev* p = v->peer;
    vdev_free_one(v);
    if (p) vdev_free_one(p);
    return 0;
}

int netvirt_is_virtual(struct net_device* dev) { return is_virtual(dev); }

/* A namespace's last reference went: its virtual devices go (a veth whose
 * other end lives elsewhere takes that end too — Linux's rule). */
void netvirt_ns_gone(int ns) {
    for (;;) {
        struct vdev* hit = NULL;
        uint32_t f = net_lock();
        for (struct vdev* v = g_vdevs; v; v = v->vnext) if (v->dev.netns == ns) { hit = v; break; }
        net_unlock(f);
        if (!hit) break;
        netvirt_destroy(&hit->dev);
    }
}

/* ---- `netnstest` — namespaces, veth and a bridge, each rule made to show ----
 *
 *   A ⟷ B over a veth pair (10.9.0.1 / 10.9.0.2): a TCP conversation crosses
 *   it, while the machine's namespace — which has no device in that subnet —
 *   cannot reach either end, and 127.0.0.1 in B is not A's loopback.
 *   Then the Docker shape: a bridge br-t (10.8.0.1/24) in the machine's
 *   namespace, a veth whose one end is its port and whose other end lives in C
 *   as 10.8.0.2: the host reaches C's listener through the bridge and C
 *   reaches the host's.  Destroying the namespaces destroys their devices. */
#include "shellcmd.h"
#include "task.h"
#include "nsproxy.h"
#include "vfs.h"
int procfs_ns_handle(struct file* f, int* kind, struct nsobj** obj, int* mnt, uint32_t* ino);

static int t_talk(int from_ns, uint32_t ip, uint16_t port, struct tcp_conn* listener) {
    struct tcp_conn* c = net_tcp_connect_ns(from_ns, ip, port, 1500);
    if (!c) return 0;
    struct tcp_conn* s = net_tcp_accept(listener, 1500);
    int ok = 0;
    if (s) {
        net_tcp_send(c, "ping", 4, 0);
        char b[8] = {0};
        int r = net_tcp_recv(s, b, sizeof b, 0, 1500);
        ok = r == 4 && b[0] == 'p' && b[3] == 'g';
        net_tcp_close(s);
    }
    net_tcp_close(c);
    return ok;
}

static void up(struct net_device* d, uint32_t ip, uint32_t mask) {
    d->ip = ip; d->netmask = mask; d->admin_down = 0;
}

static void cmd_netnstest(const char* args) {
    (void)args;
    int ok = 1;
    #define NT(cond, what) do { int _c = (cond); kprintf("netns: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    int A = net_ns_create(), B = net_ns_create(), C = net_ns_create();
    NT(A > 0 && B > 0 && C > 0 && A != B, "three namespaces");
    struct net_device* loA = net_find_ns(A, "lo");
    NT(loA && loA != net_find_ns(B, "lo") && loA->admin_down, "each has its own loopback, born down");
    if (loA) loA->admin_down = 0;
    struct net_device* loB = net_find_ns(B, "lo");
    if (loB) loB->admin_down = 0;

    struct net_device *va = NULL, *vb = NULL;
    NT(netvirt_veth_create(A, "va", B, "vb", &va, &vb) == 0 && va && vb, "a veth pair across A and B");
    if (!va || !vb) { kprintf("netns: FAIL\n"); return; }
    up(va, IPV4(10, 9, 0, 1), IPV4(255, 255, 255, 0));
    up(vb, IPV4(10, 9, 0, 2), IPV4(255, 255, 255, 0));
    NT(net_find_ns(0, "va") == NULL && net_find_ns(A, "va") == va, "va is visible only in A");
    /* §M90 3b — 127/8 on a wire is martian unless route_localnet says not */
    {
        int mf = net_martian_selftest(va, IPV4(10, 9, 0, 2), vb);
        if (mf) kprintf("netns: martian failures mask %d\n", mf);
        NT(mf == 0, "127/8 in and out of va is martian, and passes with route_localnet");
    }
    /* ... and the switch is reachable as Docker writes it, through the VFS */
    {
        struct net_device* lo0 = net_find_ns(net_cur_ns(), "lo");
        char rb[8] = {0};
        struct file* f = vfs_open("/proc/sys/net/ipv4/conf/lo/route_localnet", VFS_RDONLY);
        long rn = f ? vfs_read(f, rb, sizeof rb - 1) : -1;
        if (f) vfs_close(f);
        f = vfs_open("/proc/sys/net/ipv4/conf/lo/route_localnet", VFS_RDWR);
        long wn = f ? vfs_write(f, "1\n", 2) : -1;
        if (f) vfs_close(f);
        int set = lo0 && lo0->ip4_route_localnet == 1;
        f = vfs_open("/proc/sys/net/ipv4/conf/lo/route_localnet", VFS_RDWR);
        if (f) { vfs_write(f, "0\n", 2); vfs_close(f); }
        f = vfs_open("/proc/sys/net/ipv4/conf/nosuchif0/route_localnet", VFS_RDONLY);
        int none = f == NULL;
        if (f) vfs_close(f);
        NT(rn == 2 && rb[0] == '0' && wn == 2 && set && lo0 && !lo0->ip4_route_localnet && none,
           "/proc/sys/net/ipv4/conf/lo/route_localnet reads 0, takes 1 and 0; an unknown name has no file");
    }

    struct tcp_conn* lA = net_tcp_listen_ns(A, 0, 7000, 4);
    NT(lA != NULL, "listen on :7000 in A");
    NT(t_talk(B, IPV4(10, 9, 0, 1), 7000, lA), "B talks to A over the veth");
    NT(net_tcp_connect_ns(0, IPV4(10, 9, 0, 1), 7000, 800) == NULL, "the machine cannot reach A");
    struct tcp_conn* l127 = net_tcp_listen_ns(A, IPV4(127, 0, 0, 1), 7100, 4);
    NT(l127 && t_talk(A, IPV4(127, 0, 0, 1), 7100, l127), "A's own loopback works");
    NT(net_tcp_connect_ns(B, IPV4(127, 0, 0, 1), 7100, 800) == NULL, "B's 127.0.0.1 is not A's");
    if (l127) net_tcp_close(l127);
    /* A connection whose DEVICE goes while it is open ends as a reset, and
     * nothing of it touches the device afterwards: a connection over va/vb,
     * then the pair destroyed under it.  Before
     * the fix the closing side kept `dev` and its retransmit ran through the
     * freed device 200 ms later (x86_64: a jump to rip 3).  This uses the
     * A/B pair itself, so it comes last for it. */
    {
        struct tcp_conn* cc = net_tcp_connect_ns(B, IPV4(10, 9, 0, 1), 7000, 1500);
        struct tcp_conn* sc = cc ? net_tcp_accept(lA, 1500) : NULL;
        int ok_open = cc && sc;
        netvirt_destroy(va);                       /* vb goes with it */
        va = vb = NULL;
        int rs = 0, rc = 0;
        if (cc) net_tcp_state(cc, NULL, NULL, NULL, &rs, NULL);
        if (sc) net_tcp_state(sc, NULL, NULL, NULL, &rc, NULL);
        char b1[4];
        int sendr = cc ? net_tcp_send(cc, "x", 1, 1) : 0;
        int recvr = sc ? net_tcp_recv(sc, b1, sizeof b1, 1, 0) : 0;
        NT(ok_open && rs && rc && sendr < 0 && recvr <= 0,
           "a device destroyed under open connections resets both ends");
        if (cc) net_tcp_close(cc);
        if (sc) net_tcp_close(sc);
    }
    net_tcp_close(lA);

    /* the Docker shape */
    struct net_device* br = netvirt_bridge_create(0, "br-t");
    struct net_device *hp = NULL, *cp = NULL;
    NT(br && netvirt_veth_create(0, "vh-t", C, "eth0", &hp, &cp) == 0, "a bridge and a veth into C");
    if (br && hp && cp) {
        up(br, IPV4(10, 8, 0, 1), IPV4(255, 255, 255, 0));
        hp->admin_down = 0;
        NT(netvirt_set_master(hp, br) == 0, "the host end is a port of the bridge");
        up(cp, IPV4(10, 8, 0, 2), IPV4(255, 255, 255, 0));
        NT(net_route_add(C, 0, 0, IPV4(10, 8, 0, 1), cp, 0) == 0, "C's default route via the bridge");
        struct tcp_conn* lC = net_tcp_listen_ns(C, 0, 8000, 4);
        NT(lC && t_talk(0, IPV4(10, 8, 0, 2), 8000, lC), "the host reaches C through the bridge");
        if (lC) net_tcp_close(lC);
        struct tcp_conn* lH = net_tcp_listen_ns(0, 0, 8001, 4);
        NT(lH && t_talk(C, IPV4(10, 8, 0, 1), 8001, lH), "C reaches the host");
        if (lH) net_tcp_close(lH);
        /* §M90 rung 5d — beyond the host: forwarding + masquerade.  The
         * machine's gateway (QEMU's 10.0.2.2) answers an echo from C only if
         * the host forwards it and rewrites its source to 10.0.2.15. */
        int was = net_ip_forward(0);
        net_set_ip_forward(0, 0);
        int off = net_ping(cp, IPV4(10, 0, 2, 2), 1);
        net_set_ip_forward(0, 1);
        uint32_t o0, i0, o1, i1;
        net_fwd_stats(NULL, NULL, NULL, &o0, &i0, NULL);
        int on = net_ping(cp, IPV4(10, 0, 2, 2), 2);
        net_fwd_stats(NULL, NULL, NULL, &o1, &i1, NULL);
        net_set_ip_forward(0, was);
        NT(off == 0, "with ip_forward 0 the gateway is unreachable from C");
        /* Host forwarding between two namespaces, to a next hop the host
         * has never talked to: the packet must WAIT for the ARP answer,
         * not be dropped (the connect would then need a SYN retransmit,
         * a second, and the 800 ms budget below would fail). */
        int D = net_ns_create();
        struct net_device *h2 = NULL, *d2 = NULL;
        if (D > 0 && netvirt_veth_create(0, "vh2-t", D, "eth0", &h2, &d2) == 0) {
            up(h2, IPV4(10, 7, 0, 1), IPV4(255, 255, 255, 0));
            up(d2, IPV4(10, 7, 0, 2), IPV4(255, 255, 255, 0));
            net_route_add(D, 0, 0, IPV4(10, 7, 0, 1), d2, 0);
            struct net_device* loD = net_find_ns(D, "lo");
            if (loD) loD->admin_down = 0;
            net_set_ip_forward(0, 1);
            uint32_t rel0 = net_arp_released();
            struct tcp_conn* lD = net_tcp_listen_ns(D, 0, 8002, 4);
            struct tcp_conn* cc = lD ? net_tcp_connect_ns(C, IPV4(10, 7, 0, 2), 8002, 800) : NULL;
            struct tcp_conn* sc = cc ? net_tcp_accept(lD, 800) : NULL;
            NT(cc && sc && net_arp_released() > rel0,
               "C reaches D through the host's forwarding, the first packet held for ARP");
            if (sc) net_tcp_close(sc);
            if (cc) net_tcp_close(cc);
            if (lD) net_tcp_close(lD);
        } else {
            NT(0, "C reaches D through the host's forwarding (setup)");
        }
        net_ns_destroy(D);
        NT(on >= 1 && o1 > o0 && i1 > i0, "with ip_forward 1 C reaches the gateway, masqueraded");
        netvirt_destroy(hp);
        NT(net_find_ns(C, "eth0") == NULL, "destroying a veth end destroys its peer");
        netvirt_destroy(br);
    }
    net_ns_destroy(A);
    NT(net_find_ns(B, "vb") == NULL && net_find_ns(A, "lo") == NULL,
       "a namespace's end takes its devices (and the veth's other end)");
    net_ns_destroy(B);
    net_ns_destroy(C);

    /* A namespace handle PINNED by a bind names the namespace it named at
     * bind time, whoever opens it later (Docker's /var/run/docker/netns). */
    {
        struct task* me = task_current();
        extern int procfs_ns_pin(const char* path, char* out, size_t cap);
        extern void procfs_ns_unpin(struct dentry* d);
        int r = ns_unshare(me, NS_CLONE_NEWNET);
        int mine = ns_net_id(me);
        char pinned[64];
        int p = r == 0 ? procfs_ns_pin("/proc/self/ns/net", pinned, sizeof pinned) : -1;
        ns_enter(me, NSK_NET, NULL);                   /* back to the machine's */
        int named = -1;
        struct file* f = p == 0 ? vfs_open(pinned, VFS_RDONLY) : NULL;
        if (f) {
            int kind; struct nsobj* o = NULL;
            if (procfs_ns_handle(f, &kind, &o, NULL, NULL) && kind == NSK_NET) named = o ? o->net_id : 0;
            vfs_close(f);
        }
        NT(r == 0 && mine > 0 && named == mine, "a pinned handle names its namespace, not the opener's");
        struct dentry* pd = p == 0 ? vfs_resolve(pinned) : NULL;
        if (pd) procfs_ns_unpin(pd);
        NT(mine > 0 && !net_ns_alive(mine), "releasing the last pin ends the namespace");
    }
    #undef NT
    kprintf("netns: %s\n", ok ? "ALL ok" : "FAIL");
}
/* ---- `brnftest` — br_netfilter, made to show (§M90 3b) ----------------------------
 *
 * A bridge "brt" (10.6.0.1/24) in a namespace H of its own, and two
 * "containers" P (10.6.0.2) and Q (10.6.0.3), each a veth into it.  Rules are
 * installed in H the way nft would (NETLINK_NETFILTER messages, here handed to
 * nft_netlink directly):
 *   1. `oifname "brt" tcp dport 9100 drop` in FORWARD: P can no longer reach
 *      Q's :9100 although the traffic never leaves the bridge — so bridged
 *      frames DO pass the IP hooks, with the BRIDGE as the logical device;
 *      with bridge-nf-call-iptables 0 they do not (the switch is real).
 *   2. Docker's hairpin: `ip daddr 10.6.0.1 tcp dport 9200 dnat to
 *      10.6.0.3:9100` in PREROUTING.  P connects to the bridge's address; the
 *      host DNATs it to Q and forwards it back out of brt; Q's answer is
 *      BRIDGED straight to P and is un-DNATed only because br_netfilter puts
 *      it through the hooks.  With the switch off the same connect fails. */
static uint8_t g_tb[2048];
static uint32_t g_tl, g_tmsg, g_tnest[8], g_tdepth, g_tseq = 1;
static void tb_put(const void* p, uint32_t n) { const uint8_t* q = (const uint8_t*)p; for (uint32_t i = 0; i < n; i++) g_tb[g_tl++] = q[i]; }
static void tb_pad(void) { while (g_tl & 3) g_tb[g_tl++] = 0; }
static void tb_msg(uint16_t type, uint16_t flags, uint8_t fam) {
    g_tmsg = g_tl;
    uint32_t h[4] = { 0, (uint32_t)type | (uint32_t)flags << 16, g_tseq++, 0 };
    tb_put(h, 16);
    uint8_t nfg[4] = { fam, 0, 0, 10 };
    tb_put(nfg, 4);
}
static void tb_end(void) { uint32_t l = g_tl - g_tmsg; for (int i = 0; i < 4; i++) g_tb[g_tmsg + i] = (uint8_t)(l >> (8 * i)); }
static void tb_attr(uint16_t type, const void* p, uint16_t n) {
    uint16_t h[2] = { (uint16_t)(4 + n), type };
    tb_put(h, 4); tb_put(p, n); tb_pad();
}
static void tb_str(uint16_t type, const char* s) { uint16_t n = 0; while (s[n]) n++; tb_attr(type, s, (uint16_t)(n + 1)); }
static void tb_be32(uint16_t type, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; tb_attr(type, b, 4); }
static void tb_nest(uint16_t type) { g_tnest[g_tdepth++] = g_tl; uint16_t h[2] = { 0, (uint16_t)(type | 0x8000) }; tb_put(h, 4); }
static void tb_nend(void) { uint32_t at = g_tnest[--g_tdepth]; uint16_t l = (uint16_t)(g_tl - at); g_tb[at] = (uint8_t)l; g_tb[at + 1] = (uint8_t)(l >> 8); }
static void tb_ex(const char* name) { tb_nest(1); tb_str(1, name); tb_nest(2); }
static void tb_exend(void) { tb_nend(); tb_nend(); }
static void tb_cmp(const void* v, uint16_t n) { tb_ex("cmp"); tb_be32(1, 1); tb_be32(2, 0); tb_nest(3); tb_attr(1, v, n); tb_nend(); tb_exend(); }
static void tb_payload(uint32_t base, uint32_t off, uint32_t len) {
    tb_ex("payload"); tb_be32(1, 1); tb_be32(2, base); tb_be32(3, off); tb_be32(4, len); tb_exend();
}
static void tb_meta(uint32_t key) { tb_ex("meta"); tb_be32(1, 1); tb_be32(2, key); tb_exend(); }
static void tb_imm(uint32_t reg, const void* v, uint16_t n) { tb_ex("immediate"); tb_be32(1, reg); tb_nest(2); tb_attr(1, v, n); tb_nend(); tb_exend(); }
static void tb_verdict(uint32_t code) {
    tb_ex("immediate"); tb_be32(1, 0); tb_nest(2); tb_nest(2); tb_be32(1, code); tb_nend(); tb_nend(); tb_exend();
}
static void tb_begin(void) { g_tl = 0; tb_msg(16, 1, 0); tb_end(); }
static void tb_chain(const char* t, const char* c, const char* type, uint32_t hook, int32_t prio) {
    tb_msg(10 << 8 | 3, 1 | 0x400 | 4, 2); tb_str(1, t); tb_str(3, c);
    tb_nest(4); tb_be32(1, hook); tb_be32(2, (uint32_t)prio); tb_nend();
    tb_be32(5, 1); tb_str(7, type); tb_end();
}
static int tb_commit(int ns) {
    tb_msg(17, 1, 0); tb_end();
    uint8_t* out = NULL; uint32_t ol = 0;
    nft_netlink(ns, 0, g_tb, g_tl, &out, &ol);
    int err = 0;
    for (uint32_t o = 0; out && o + 20 <= ol;) {
        uint32_t l = (uint32_t)out[o] | (uint32_t)out[o + 1] << 8 | (uint32_t)out[o + 2] << 16 | (uint32_t)out[o + 3] << 24;
        uint16_t ty = (uint16_t)(out[o + 4] | out[o + 5] << 8);
        if (l < 16) break;
        if (ty == 2) {
            int32_t e = (int32_t)((uint32_t)out[o + 16] | (uint32_t)out[o + 17] << 8 |
                                  (uint32_t)out[o + 18] << 16 | (uint32_t)out[o + 19] << 24);
            if (e && !err) err = e;
        }
        o += (l + 3) & ~3u;
    }
    kfree(out);
    return err;
}
static int tb_deltable(int ns, const char* t) {
    tb_begin(); tb_msg(10 << 8 | 2, 1 | 4, 2); tb_str(1, t); tb_end();
    return tb_commit(ns);
}

static void cmd_brnftest(const char* args) {
    (void)args;
    int ok = 1;
    #define BT(cond, what) do { int _c = (cond); kprintf("brnf: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    int H = net_ns_create(), P = net_ns_create(), Q = net_ns_create();
    struct net_device* br = H > 0 ? netvirt_bridge_create(H, "brt") : NULL;
    struct net_device *hp = NULL, *pp = NULL, *hq = NULL, *qq = NULL;
    BT(br && P > 0 && Q > 0 &&
       netvirt_veth_create(H, "bp", P, "eth0", &hp, &pp) == 0 &&
       netvirt_veth_create(H, "bq", Q, "eth0", &hq, &qq) == 0, "a bridge with two containers");
    if (!ok) { kprintf("brnf: FAIL\n"); return; }
    up(br, IPV4(10, 6, 0, 1), IPV4(255, 255, 255, 0));
    hp->admin_down = 0; hq->admin_down = 0;
    netvirt_set_master(hp, br); netvirt_set_master(hq, br);
    up(pp, IPV4(10, 6, 0, 2), IPV4(255, 255, 255, 0));
    up(qq, IPV4(10, 6, 0, 3), IPV4(255, 255, 255, 0));
    net_route_add(P, 0, 0, IPV4(10, 6, 0, 1), pp, 0);
    net_route_add(Q, 0, 0, IPV4(10, 6, 0, 1), qq, 0);
    net_set_ip_forward(H, 1);
    struct tcp_conn* lq = net_tcp_listen_ns(Q, 0, 9100, 4);
    BT(lq && t_talk(P, IPV4(10, 6, 0, 3), 9100, lq), "P reaches Q across the bridge (no rules)");

    /* 1. bridged traffic meets FORWARD, with the bridge as oifname */
    static const char brname[16] = "brt";
    uint8_t tcp = 6; static const uint8_t p9100[2] = { 9100 >> 8, 9100 & 0xff };   /* network order */
    tb_begin();
    tb_msg(10 << 8 | 0, 1 | 0x400 | 4, 2); tb_str(1, "f"); tb_end();
    tb_chain("f", "fwd", "filter", 2 /* FORWARD */, 0);
    tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "f"); tb_str(2, "fwd"); tb_nest(4);
    tb_meta(7 /* OIFNAME */); tb_cmp(brname, 16);
    tb_meta(16 /* L4PROTO */); tb_cmp(&tcp, 1);
    tb_payload(2, 2, 2); tb_cmp(p9100, 2);
    tb_verdict(0 /* DROP */);
    tb_nend(); tb_end();
    BT(tb_commit(H) == 0, "rule: oifname brt tcp dport 9100 drop (FORWARD)");
    uint32_t d0 = g_brnf_dropped;
    BT(!t_talk(P, IPV4(10, 6, 0, 3), 9100, lq) && g_brnf_dropped > d0,
       "the bridged connection is dropped by the IP-level rule");
    brnf_on(H, 0); g_brnf[0][H] = 0;
    BT(t_talk(P, IPV4(10, 6, 0, 3), 9100, lq), "with bridge-nf-call-iptables 0 the rule does not see it");
    g_brnf[0][H] = 1;
    BT(tb_deltable(H, "f") == 0, "table deleted");

    /* 2. the hairpin: DNAT on the host, the reply bridged */
    uint32_t a1 = IPV4(10, 6, 0, 1), a3 = IPV4(10, 6, 0, 3);
    uint8_t d1[4] = { (uint8_t)(a1 >> 24), (uint8_t)(a1 >> 16), (uint8_t)(a1 >> 8), (uint8_t)a1 };
    uint8_t d3[4] = { (uint8_t)(a3 >> 24), (uint8_t)(a3 >> 16), (uint8_t)(a3 >> 8), (uint8_t)a3 };
    static const uint8_t p9200[2] = { 9200 >> 8, 9200 & 0xff };
    tb_begin();
    tb_msg(10 << 8 | 0, 1 | 0x400 | 4, 2); tb_str(1, "n"); tb_end();
    tb_chain("n", "pre", "nat", 0 /* PREROUTING */, -100);
    tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "n"); tb_str(2, "pre"); tb_nest(4);
    tb_payload(1, 16, 4); tb_cmp(d1, 4);
    tb_meta(16); tb_cmp(&tcp, 1);
    tb_payload(2, 2, 2); tb_cmp(p9200, 2);
    tb_imm(1, d3, 4); tb_imm(2, p9100, 2);
    tb_ex("nat"); tb_be32(1, 1 /* DNAT */); tb_be32(2, 2); tb_be32(3, 1); tb_be32(5, 2); tb_be32(7, 3); tb_exend();
    tb_nend(); tb_end();
    BT(tb_commit(H) == 0, "rule: ip daddr 10.6.0.1 tcp dport 9200 dnat to 10.6.0.3:9100");
    /* (Q answers to the MAC the SYN came from — the bridge's — so this reply
     * goes up through the host, not across the bridge: what this proves is the
     * host forwarding back out of the interface it came in on.) */
    BT(t_talk(P, a1, 9200, lq), "P reaches Q through the host's DNAT (hairpin, out the way it came in)");
    BT(tb_deltable(H, "n") == 0, "table deleted");

    /* 3. NAT on the BRIDGED path, both directions: P dials Q's own address on
     * 9300, PREROUTING (on the bridge) moves it to 9100, and Q's answer —
     * bridged straight back to P — must be un-DNATed to 9300 by the same
     * hooks.  With the switch off neither happens and the connect fails. */
    static const uint8_t p9300[2] = { 9300 >> 8, 9300 & 0xff };
    tb_begin();
    tb_msg(10 << 8 | 0, 1 | 0x400 | 4, 2); tb_str(1, "m"); tb_end();
    tb_chain("m", "pre", "nat", 0, -100);
    tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "m"); tb_str(2, "pre"); tb_nest(4);
    tb_payload(1, 16, 4); tb_cmp(d3, 4);
    tb_meta(16); tb_cmp(&tcp, 1);
    tb_payload(2, 2, 2); tb_cmp(p9300, 2);
    tb_imm(1, d3, 4); tb_imm(2, p9100, 2);
    tb_ex("nat"); tb_be32(1, 1); tb_be32(2, 2); tb_be32(3, 1); tb_be32(5, 2); tb_be32(7, 3); tb_exend();
    tb_nend(); tb_end();
    BT(tb_commit(H) == 0, "rule: ip daddr 10.6.0.3 tcp dport 9300 dnat to 10.6.0.3:9100");
    BT(t_talk(P, a3, 9300, lq), "a bridged connection is DNATed and its bridged replies un-DNATed");
    g_brnf[0][H] = 0;
    BT(!t_talk(P, a3, 9300, lq), "with bridge-nf-call-iptables 0 the bridged path is not translated");
    g_brnf[0][H] = 1;
    tb_deltable(H, "m");

    /* 4. Docker's `-p` from the host itself (--userland-proxy=false): the
     * host dials its OWN address on the published port; OUTPUT DNATs it to
     * the container, POSTROUTING masquerades it out of the bridge ("fib saddr
     * type local masquerade"), and the container's answer is un-SNATed in
     * PREROUTING and un-DNATed at INPUT before the host's socket sees it. */
    static const uint8_t p9400[2] = { 9400 >> 8, 9400 & 0xff };
    tb_begin();
    tb_msg(10 << 8 | 0, 1 | 0x400 | 4, 2); tb_str(1, "d"); tb_end();
    tb_chain("d", "out", "nat", 3 /* OUTPUT */, -100);
    tb_chain("d", "post", "nat", 4 /* POSTROUTING */, 100);
    tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "d"); tb_str(2, "out"); tb_nest(4);
    tb_meta(16); tb_cmp(&tcp, 1);
    tb_payload(2, 2, 2); tb_cmp(p9400, 2);
    tb_imm(1, d3, 4); tb_imm(2, p9100, 2);
    tb_ex("nat"); tb_be32(1, 1); tb_be32(2, 2); tb_be32(3, 1); tb_be32(5, 2); tb_be32(7, 3); tb_exend();
    tb_nend(); tb_end();
    tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "d"); tb_str(2, "post"); tb_nest(4);
    tb_meta(7); tb_cmp(brname, 16);
    tb_ex("masq"); tb_exend();
    tb_nend(); tb_end();
    BT(tb_commit(H) == 0, "rules: tcp dport 9400 dnat to 10.6.0.3:9100 (OUTPUT), oifname brt masquerade");
    BT(t_talk(H, a1, 9400, lq), "the host reaches the container through its own address (Docker's -p)");
    tb_deltable(H, "d");

    /* 5. a container reaching the outside through Docker's masquerade rule
     * (`oifname != "brt" ip saddr 10.6.0.0/24 masquerade`): R stands for the
     * internet and knows no route back to 10.6.0.0/24, so its echo replies
     * reach P only if the host translated the request and un-translates the
     * reply. */
    int R = net_ns_create();
    struct net_device *hu = NULL, *ru = NULL;
    if (R > 0 && netvirt_veth_create(H, "up0", R, "eth0", &hu, &ru) == 0) {
        up(hu, IPV4(10, 5, 0, 1), IPV4(255, 255, 255, 0));
        up(ru, IPV4(10, 5, 0, 2), IPV4(255, 255, 255, 0));
        net_set_ip_forward(H, 1);                       /* the new device forwards too */
        uint8_t net6[4] = { 10, 6, 0, 0 }, mask[4] = { 255, 255, 255, 0 }, zero4[4] = { 0 };
        tb_begin();
        tb_msg(10 << 8 | 0, 1 | 0x400 | 4, 2); tb_str(1, "q"); tb_end();
        tb_chain("q", "post", "nat", 4, 100);
        tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "q"); tb_str(2, "post"); tb_nest(4);
        tb_meta(7); tb_ex("cmp"); tb_be32(1, 1); tb_be32(2, 1 /* NEQ */); tb_nest(3); tb_attr(1, brname, 16); tb_nend(); tb_exend();
        tb_payload(1, 12, 4);
        tb_ex("bitwise"); tb_be32(1, 1); tb_be32(2, 1); tb_be32(3, 4);
        tb_nest(4); tb_attr(1, mask, 4); tb_nend(); tb_nest(5); tb_attr(1, zero4, 4); tb_nend(); tb_exend();
        tb_cmp(net6, 4);
        tb_ex("masq"); tb_exend();
        tb_nend(); tb_end();
        /* and Docker's raw rule, priority -300 — BEFORE conntrack and NAT, so
         * it must see a masqueraded reply still addressed to the host: run
         * after the un-SNAT (as this hook once did) it drops every reply */
        static const uint8_t pip[4] = { 10, 6, 0, 2 };
        tb_chain("q", "raw", "filter", 0 /* PREROUTING */, -300);
        tb_msg(10 << 8 | 6, 1 | 0x400 | 4, 2); tb_str(1, "q"); tb_str(2, "raw"); tb_nest(4);
        tb_payload(1, 16, 4); tb_cmp(pip, 4);
        tb_meta(6 /* IIFNAME */); tb_ex("cmp"); tb_be32(1, 1); tb_be32(2, 1 /* NEQ */); tb_nest(3); tb_attr(1, brname, 16); tb_nend(); tb_exend();
        tb_verdict(0);
        tb_nend(); tb_end();
        BT(tb_commit(H) == 0, "rules: oifname != brt ip saddr 10.6.0.0/24 masquerade; raw: ip daddr 10.6.0.2 iifname != brt drop");
        BT(net_ping(pp, IPV4(10, 5, 0, 2), 2) == 2, "P pings the outside through the masquerade");
        struct tcp_conn* lr = net_tcp_listen_ns(R, 0, 9500, 4);
        /* (No "without the rule it fails" control here: this stack answers a
         * frame to the MAC it came from (§M55), so R would reply even with no
         * route back.  What proves the translation is what R SEES.) */
        struct tcp_conn* pc = lr ? net_tcp_connect_ns(P, IPV4(10, 5, 0, 2), 9500, 1500) : NULL;
        struct tcp_conn* rs = pc ? net_tcp_accept(lr, 1500) : NULL;
        uint32_t seen = 0; uint16_t sport = 0;
        if (rs) net_tcp_peer(rs, &seen, &sport);
        BT(rs && seen == IPV4(10, 5, 0, 1), "P talks TCP to the outside, which sees the host's address");
        if (rs) net_tcp_close(rs);
        if (pc) net_tcp_close(pc);
        if (lr) net_tcp_close(lr);
        tb_deltable(H, "q");
    } else BT(0, "an uplink namespace");

    if (lq) net_tcp_close(lq);
    net_ns_destroy(P); net_ns_destroy(Q); if (R > 0) net_ns_destroy(R); net_ns_destroy(H);
    kprintf("brnf: %s\n", ok ? "ALL ok" : "FAIL");
    #undef BT
}
SHELL_CMD(brnftest) = { "brnftest", "", "br_netfilter: bridged traffic through the IP hooks, the DNAT hairpin",
                        SHELL_G_TEST, cmd_brnftest, SHELL_P_ANY };

SHELL_CMD(netnstest) = { "netnstest", "", "network namespaces: veth, bridge, isolation, loopback per namespace",
                         SHELL_G_TEST, cmd_netnstest, SHELL_P_ANY };

/* ---- `icmpsocktest` — §M90: ping from a container ----------------------------
 *
 *   A ping socket (SOCK_DGRAM/IPPROTO_ICMP) sends an echo to 127.0.0.1 and
 *   receives the reply carrying ITS id (the kernel rewrote whatever the caller
 *   put there); a raw socket (SOCK_RAW/IPPROTO_ICMP) sees the whole IP packet.
 *   Then the namespace rule: a socket made in a namespace whose loopback is
 *   DOWN cannot reach 127.0.0.1, even after its creator has gone back to the
 *   machine's namespace where loopback works — a socket belongs to the
 *   namespace it was made in, not to whoever uses it. */
#include "syscall.h"
int sys_socket(int domain, int type, int proto);
int sys_close(int fd);

static int t_echo(int fd, uint16_t want_id_any, uint8_t* got, int cap) {
    uint8_t m[16] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1, 'd', '-', 'o', 's', 0, 0, 0, 0 };
    long w = sys_sendto_k(fd, m, sizeof m, IPV4(127, 0, 0, 1), 0);
    (void)want_id_any;
    uint32_t ip = 0;
    int r = w == (long)sizeof m ? (int)sys_recvfrom_k(fd, got, (size_t)cap, &ip, NULL) : -100;
    kprintf("icmpsock: sent %d, received %d (type %d)\n", (int)w, r, r > 0 ? got[0] : -1);
    return r;
}

static void cmd_icmpsocktest(const char* args) {
    (void)args;
    int ok = 1;
    #define IT(cond, what) do { int _c = (cond); kprintf("icmpsock: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    uint8_t b[128];

    int p = sys_socket(2, 2, 1);
    IT(p >= 0, "a ping socket");
    int r = p >= 0 ? t_echo(p, 0, b, sizeof b) : -1;
    IT(r >= 16 && b[0] == 0 && b[8] == 'd' && !(b[4] == 0x12 && b[5] == 0x34),
       "an echo reply arrives, with the kernel's id, not the caller's");
    if (p >= 0) sys_close(p);

    int s = sys_socket(2, 3, 1);
    IT(s >= 0, "a raw ICMP socket (CAP_NET_RAW)");
    r = s >= 0 ? t_echo(s, 0, b, sizeof b) : -1;
    IT(r >= 36 && (b[0] >> 4) == 4 && b[9] == 1, "a raw socket receives the IP header too");
    if (s >= 0) sys_close(s);

    struct task* me = task_current();
    int q = -1;
    if (ns_unshare(me, NS_CLONE_NEWNET) == 0) {
        q = sys_socket(2, 2, 1);                    /* made where lo is DOWN */
        ns_enter(me, NSK_NET, NULL);                /* back to the machine's */
    }
    uint8_t m[16] = { 8, 0, 0, 0, 0, 0, 0, 1 };
    IT(q >= 0 && sys_sendto_k(q, m, sizeof m, IPV4(127, 0, 0, 1), 0) < 0,
       "a socket keeps its namespace: 127.0.0.1 unreachable there, whoever sends");
    if (q >= 0) sys_close(q);

    /* listen on port 0 = an ephemeral port, read back by getsockname
     * (containerd's CRI streaming server listens on 127.0.0.1:0) */
    {
        int l = sys_socket(2, 1, 6);
        uint32_t lip = 0; int lport = 0;
        int ok0 = l >= 0 && sys_bind(l, IPV4(127, 0, 0, 1), 0) == 0 && sys_listen(l, 4) == 0
               && sys_getsockname_k(l, &lip, &lport) == 0 && lport != 0;
        struct tcp_conn* c = ok0 ? net_tcp_connect_ns(0, IPV4(127, 0, 0, 1), (uint16_t)lport, 1500) : NULL;
        IT(ok0 && c, "listen on port 0 gets an ephemeral port that accepts");
        if (c) net_tcp_close(c);
        if (l >= 0) sys_close(l);
    }
    #undef IT
    kprintf("icmpsock: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(icmpsocktest) = { "icmpsocktest", "", "ICMP ping and raw sockets, and a socket's namespace",
                            SHELL_G_TEST, cmd_icmpsocktest, SHELL_P_ANY };
