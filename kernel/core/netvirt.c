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
 * enforcement between ports (all are 1500 here), and forwarding DATABASE
 * entries never age (a container's MAC is gone with its veth, which flushes
 * them).
 * ============================================================================= */

#include "net.h"
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
    struct { uint8_t mac[ETH_ALEN]; struct net_device* port; int used; } fdb[FDB_MAX];
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

static void fdb_learn(struct vdev* br, const uint8_t* mac, struct net_device* port) {
    if (mac_bcast(mac)) return;
    for (int i = 0; i < FDB_MAX; i++)
        if (br->fdb[i].used && mac_eq(br->fdb[i].mac, mac)) { br->fdb[i].port = port; return; }
    for (int i = 0; i < FDB_MAX; i++)
        if (!br->fdb[i].used) {
            br->fdb[i].used = 1; br->fdb[i].port = port;
            for (int k = 0; k < ETH_ALEN; k++) br->fdb[i].mac[k] = mac[k];
            return;
        }
}
static struct net_device* fdb_find(struct vdev* br, const uint8_t* mac) {
    for (int i = 0; i < FDB_MAX; i++)
        if (br->fdb[i].used && mac_eq(br->fdb[i].mac, mac)) return br->fdb[i].port;
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

    struct tcp_conn* lA = net_tcp_listen_ns(A, 0, 7000, 4);
    NT(lA != NULL, "listen on :7000 in A");
    NT(t_talk(B, IPV4(10, 9, 0, 1), 7000, lA), "B talks to A over the veth");
    NT(net_tcp_connect_ns(0, IPV4(10, 9, 0, 1), 7000, 800) == NULL, "the machine cannot reach A");
    struct tcp_conn* l127 = net_tcp_listen_ns(A, IPV4(127, 0, 0, 1), 7100, 4);
    NT(l127 && t_talk(A, IPV4(127, 0, 0, 1), 7100, l127), "A's own loopback works");
    NT(net_tcp_connect_ns(B, IPV4(127, 0, 0, 1), 7100, 800) == NULL, "B's 127.0.0.1 is not A's");
    if (l127) net_tcp_close(l127);
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
SHELL_CMD(netnstest) = { "netnstest", "", "network namespaces: veth, bridge, isolation, loopback per namespace",
                         SHELL_G_TEST, cmd_netnstest, SHELL_P_ANY };
