/* =============================================================================
 * virtio_mmio_net.c — virtio-net over the virtio-MMIO transport (§M24, ARM64).
 *
 * WHY THIS FILE EXISTS AT ALL.  The portable stack (kernel/core/net.c) has been
 * in the aarch64 build since the port began — ARP, IPv4, ICMP, UDP, TCP, the
 * poller, all of it — with nothing underneath it: the x86 NIC driver speaks
 * virtio over PCI BAR *port I/O*, which does not exist on this architecture.
 * So every network feature this project shipped was untested on a third of its
 * targets, and the port's own plan listed "x86_64/aarch64" as an open §M24
 * item for a year.  This is that item: ~250 lines of transport, after which
 * ARM runs the same stack the two x86 arches do.
 *
 * It is the SIBLING of virtio_mmio_blk.c, deliberately so — same register map,
 * same split-ring mechanics, same identity-mapped DMA — with the two
 * differences a NIC brings over a disk:
 *
 *   1. TWO queues.  Queue 0 is the receiveq and queue 1 the transmitq
 *      (virtio 1.0 §5.1.2), rather than one request queue.
 *   2. RX buffers are PRE-POSTED.  A disk is asked for data; a network hands
 *      it over unbidden, so the device must already hold a pool of empty
 *      buffers to put frames in, and each one is recycled after we consume it.
 *
 * Every frame carries a 12-byte `virtio_net_hdr` in front of the Ethernet
 * header.  Twelve, not the legacy ten: with VIRTIO_F_VERSION_1 negotiated the
 * `num_buffers` field is always present, whether or not mergeable receive
 * buffers were negotiated.  Getting that number wrong does not fail loudly —
 * it shifts every frame by two bytes, which looks like a corrupt network.
 *
 * POLLED, not interrupt-driven, and that is a deliberate first cut.  §M55 made
 * the stack learn that interrupts work by RECEIVING one, so a driver that
 * never fires one degrades to the poller's timed fallback instead of hanging:
 * the network works, and `lsnic` says `rx-irq: none seen (polled)` rather than
 * pretending otherwise.  Wiring the GIC SPI for the transport slot is the next
 * step and is not needed to prove the stack.
 * ============================================================================= */

#include "net.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* ---- MMIO transport map (QEMU `virt`) — identical to the block sibling ---- */
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200
#define VIRTIO_MMIO_SLOTS   32

#define R_MAGIC        0x000
#define R_VERSION      0x004
#define R_DEVICEID     0x008
#define R_DEVFEAT      0x010
#define R_DEVFEATSEL   0x014
#define R_DRVFEAT      0x020
#define R_DRVFEATSEL   0x024
#define R_QUEUESEL     0x030
#define R_QUEUENUMMAX  0x034
#define R_QUEUENUM     0x038
#define R_QUEUEREADY   0x044
#define R_QUEUENOTIFY  0x050
#define R_INTSTATUS    0x060
#define R_INTACK       0x064
#define R_STATUS       0x070
#define R_QDESC_LO     0x080
#define R_QDESC_HI     0x084
#define R_QDRV_LO      0x090
#define R_QDRV_HI      0x094
#define R_QDEV_LO      0x0a0
#define R_QDEV_HI      0x0a4
#define R_CONFIG       0x100

#define ST_ACK          1
#define ST_DRIVER       2
#define ST_DRIVER_OK    4
#define ST_FEATURES_OK  8

#define VIRTIO_MAGIC   0x74726976u
#define VIRTIO_DEV_NET 1
#define VIRTIO_F_VERSION_1_BIT 0        /* feature bit 32 → sel=1, bit 0      */
#define VIRTIO_NET_F_MAC_BIT   5        /* feature bit 5  → sel=0, bit 5      */

#define VRING_DESC_F_NEXT   0x01
#define VRING_DESC_F_WRITE  0x02
#define VRING_AVAIL_F_NO_INTERRUPT 0x01

#define QSIZE       16                  /* per queue                          */
#define RX_BUFS     QSIZE
#define BUF_BYTES   2048                /* hdr + a full 1514-byte frame       */
#define VNET_HDR    12                  /* modern virtio-net header           */

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QSIZE]; uint16_t used_event; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZE]; uint16_t avail_event; } __attribute__((packed));

/* One queue's three rings, kept together so a queue is one object rather than
 * three parallel arrays indexed by a number nobody checks. */
struct vq {
    struct virtq_desc  desc[QSIZE]  __attribute__((aligned(16)));
    struct virtq_avail avail        __attribute__((aligned(16)));
    struct virtq_used  used         __attribute__((aligned(16)));
    uint16_t           last_used;
};

static struct vq g_rxq __attribute__((aligned(16)));
static struct vq g_txq __attribute__((aligned(16)));
static uint8_t   g_rxbuf[RX_BUFS][BUF_BYTES] __attribute__((aligned(16)));
static uint8_t   g_txbuf[BUF_BYTES]          __attribute__((aligned(16)));

static uintptr_t g_base;
static struct net_device g_eth0;

static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_base + off) = v; }
static inline uint32_t r32(uint32_t off)             { return *(volatile uint32_t*)(g_base + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

static void vq_program(struct vq* q, uint32_t sel) {
    w32(R_QUEUESEL, sel);
    if (r32(R_QUEUEREADY) != 0) { kprintf("virtio-net: queue %u busy\n", sel); return; }
    uint32_t qmax = r32(R_QUEUENUMMAX);
    if (qmax < QSIZE) { kprintf("virtio-net: QueueNumMax %u < %u\n", qmax, QSIZE); return; }
    w32(R_QUEUENUM, QSIZE);
    uint64_t d = (uint64_t)(uintptr_t)q->desc;
    uint64_t a = (uint64_t)(uintptr_t)&q->avail;
    uint64_t u = (uint64_t)(uintptr_t)&q->used;
    w32(R_QDESC_LO, (uint32_t)d); w32(R_QDESC_HI, (uint32_t)(d >> 32));
    w32(R_QDRV_LO,  (uint32_t)a); w32(R_QDRV_HI,  (uint32_t)(a >> 32));
    w32(R_QDEV_LO,  (uint32_t)u); w32(R_QDEV_HI,  (uint32_t)(u >> 32));
    dsb();
    w32(R_QUEUEREADY, 1);
}

/* Hand one empty buffer back to the device. */
static void rx_post(uint16_t idx) {
    g_rxq.desc[idx].addr  = (uint64_t)(uintptr_t)g_rxbuf[idx];
    g_rxq.desc[idx].len   = BUF_BYTES;
    g_rxq.desc[idx].flags = VRING_DESC_F_WRITE;
    g_rxq.desc[idx].next  = 0;
    uint16_t ai = g_rxq.avail.idx;
    g_rxq.avail.ring[ai % QSIZE] = idx;
    dsb();
    g_rxq.avail.idx = ai + 1;
    dsb();
}

/* Transmit one complete Ethernet frame.  Synchronous: we wait for the used
 * ring, exactly like the x86 sibling and the block driver, which is why TX
 * completion interrupts are suppressed below — an interrupt whose only effect
 * is to wake somebody with nothing to do is pure cost. */
static int vnet_transmit(struct net_device* dev, const void* frame, uint32_t len) {
    (void)dev;
    if (len > BUF_BYTES - VNET_HDR) return -1;
    for (uint32_t i = 0; i < VNET_HDR; i++) g_txbuf[i] = 0;
    const uint8_t* p = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) g_txbuf[VNET_HDR + i] = p[i];

    g_txq.desc[0].addr  = (uint64_t)(uintptr_t)g_txbuf;
    g_txq.desc[0].len   = VNET_HDR + len;
    g_txq.desc[0].flags = 0;
    g_txq.desc[0].next  = 0;

    uint16_t ai = g_txq.avail.idx;
    g_txq.avail.ring[ai % QSIZE] = 0;
    dsb();
    g_txq.avail.idx = ai + 1;
    dsb();
    w32(R_QUEUENOTIFY, 1);

    /* Bounded wait.  A wedged device must cost one frame, not the machine —
     * the stack calls this with its lock held and interrupts off. */
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if (*(volatile uint16_t*)&g_txq.used.idx != g_txq.last_used) {
            g_txq.last_used++;
            dev->tx_packets++; dev->tx_bytes += len;
            return 0;
        }
        dsb();
    }
    return -1;
}

/* Drain whatever the device has delivered.  Called by the stack's poller with
 * the stack lock held; net_rx runs underneath and must not block. */
static void vnet_poll(struct net_device* dev) {
    int recycled = 0;
    while (*(volatile uint16_t*)&g_rxq.used.idx != g_rxq.last_used) {
        struct virtq_used_elem* e = &g_rxq.used.ring[g_rxq.last_used % QSIZE];
        uint16_t idx = (uint16_t)e->id;
        uint32_t tot = e->len;
        g_rxq.last_used++;
        dsb();
        if (idx < RX_BUFS && tot > VNET_HDR)
            net_rx(dev, g_rxbuf[idx] + VNET_HDR, tot - VNET_HDR);
        if (idx < RX_BUFS) { rx_post(idx); recycled = 1; }
    }
    /* Only notify if a buffer actually went back: a kick for an unchanged
     * ring is a trap into the hypervisor for nothing. */
    if (recycled) w32(R_QUEUENOTIFY, 0);
    if (r32(R_INTSTATUS) & 1) w32(R_INTACK, 1);
}

int virtio_mmio_net_init(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t*)(base + R_MAGIC) != VIRTIO_MAGIC) continue;
        uint32_t ver = *(volatile uint32_t*)(base + R_VERSION);
        uint32_t dev = *(volatile uint32_t*)(base + R_DEVICEID);
        if (dev != VIRTIO_DEV_NET || ver != 2) continue;
        g_base = base;
        break;
    }
    if (!g_base) return -1;                      /* no NIC attached           */

    w32(R_STATUS, 0);
    w32(R_STATUS, ST_ACK);
    w32(R_STATUS, ST_ACK | ST_DRIVER);

    /* Feature negotiation.  We want VIRTIO_F_VERSION_1 (bit 32) and, if the
     * device offers it, VIRTIO_NET_F_MAC (bit 5) — without the latter the
     * config-space MAC is not valid and we would be inventing an address. */
    w32(R_DEVFEATSEL, 0); uint32_t feat_lo = r32(R_DEVFEAT);
    int have_mac = (feat_lo >> VIRTIO_NET_F_MAC_BIT) & 1;
    w32(R_DEVFEATSEL, 1); (void)r32(R_DEVFEAT);
    w32(R_DRVFEATSEL, 1); w32(R_DRVFEAT, 1u << VIRTIO_F_VERSION_1_BIT);
    w32(R_DRVFEATSEL, 0); w32(R_DRVFEAT, have_mac ? (1u << VIRTIO_NET_F_MAC_BIT) : 0);

    w32(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(r32(R_STATUS) & ST_FEATURES_OK)) {
        kprintf("virtio-net: device rejected features\n");
        return -1;
    }

    vq_program(&g_rxq, 0);
    vq_program(&g_txq, 1);
    /* We poll the TX ring synchronously, so ask the device not to interrupt
     * for completions we are already standing there waiting for. */
    g_txq.avail.flags = VRING_AVAIL_F_NO_INTERRUPT;

    for (uint16_t i = 0; i < RX_BUFS; i++) rx_post(i);
    dsb();
    w32(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    w32(R_QUEUENOTIFY, 0);                       /* the device may take them  */

    g_eth0.name = "eth0";
    if (have_mac) {
        for (int i = 0; i < ETH_ALEN; i++)
            g_eth0.mac[i] = *(volatile uint8_t*)(g_base + R_CONFIG + i);
    } else {
        /* Locally administered, and DIFFERENT from the x86 default so a
         * capture with both guests on one bridge is not a mystery. */
        const uint8_t fallback[ETH_ALEN] = {0x52,0x54,0x00,0xAA,0xBB,0xCC};
        for (int i = 0; i < ETH_ALEN; i++) g_eth0.mac[i] = fallback[i];
    }
    /* The same QEMU SLIRP defaults the x86 driver uses — and, as of §M24
     * stage 7, only a starting point: `dhcp` replaces them with whatever the
     * network actually says. */
    g_eth0.ip       = IPV4(10, 0, 2, 15);
    g_eth0.netmask  = IPV4(255, 255, 255, 0);
    g_eth0.gateway  = IPV4(10, 0, 2, 2);
    g_eth0.mtu      = ETH_MTU;
    g_eth0.transmit = vnet_transmit;
    g_eth0.poll     = vnet_poll;
    net_register(&g_eth0);

    kprintf("virtio-net: up at mmio %x (mac from %s)\n",
            (unsigned)g_base, have_mac ? "device config" : "fallback");
    return 0;
}
