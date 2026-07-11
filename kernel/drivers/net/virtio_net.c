/* =============================================================================
 * virtio_net.c — legacy virtio-net driver (PCI vendor 0x1AF4, device 0x1000).
 *
 * Mirrors virtio_blk.c (same legacy PCI transport, same virtqueue layout) but
 * with the two differences a NIC brings:
 *
 *   1. TWO queues — queue 0 = receiveq, queue 1 = transmitq (virtio 1.0 §5.1.2).
 *   2. RX buffers must be *pre-posted*: we hand the device a pool of empty,
 *      device-writable buffers up front; it fills them as frames arrive and we
 *      recycle each one back into the avail ring after consuming it.
 *
 * Every packet on the wire is prefixed by a 10-byte `struct virtio_net_hdr`
 * (legacy, VIRTIO_NET_F_MRG_RXBUF NOT negotiated → 10 bytes, no num_buffers).
 * We zero it on TX and skip it on RX.
 *
 * Polling only (no IRQ): the stack calls vnet_poll() to drain the RX ring, and
 * TX waits synchronously on the used ring (one outstanding frame at a time,
 * exactly like virtio_blk).  IRQ-driven RX is a §M24 follow-up.
 *
 * DMA memory: queues + buffers come from the PMM so phys == virt inside the
 * 256 MiB identity map (heap-backed kmalloc would give a virtual pointer whose
 * physical backing the device can't reach) — same rule as virtio_blk.
 *
 * Reference: virtio 1.0 spec §4.1 (PCI transport) and §5.1 (network device).
 * ============================================================================= */

#include "net.h"
#include "pci.h"
#include "hal.h"
#include "hal_api.h"
#include "pmm.h"
#include "printf.h"
#include "driver.h"
#include <stdint.h>
#include <stddef.h>

/* ----------------------- Layout constants (shared with virtio_blk) -------- */

#define VIRTIO_VENDOR         0x1AF4
#define VIRTIO_NET_DEVICE     0x1000            /* legacy / transitional      */

#define VN_OFF_DEV_FEAT       0x00
#define VN_OFF_DRV_FEAT       0x04
#define VN_OFF_QUEUE_PFN      0x08
#define VN_OFF_QUEUE_SIZE     0x0C
#define VN_OFF_QUEUE_SEL      0x0E
#define VN_OFF_QUEUE_NOTIFY   0x10
#define VN_OFF_DEV_STATUS     0x12
#define VN_OFF_ISR_STATUS     0x13
#define VN_OFF_MAC            0x14              /* device-specific: mac[6]    */

#define VSTAT_ACKNOWLEDGE     0x01
#define VSTAT_DRIVER          0x02
#define VSTAT_DRIVER_OK       0x04
#define VSTAT_FEATURES_OK     0x08

#define VRING_DESC_F_NEXT     0x01
#define VRING_DESC_F_WRITE    0x02             /* device writes to buffer     */

/* virtio-net feature bits. */
#define VIRTIO_NET_F_MAC      (1u << 5)        /* config MAC is valid         */

#define QSIZE                 256              /* QEMU virtio-net queue size  */
#define QUEUE_BYTES           (4096 * 3)

#define RX_BUFFERS            32               /* pre-posted receive buffers  */
#define RX_BUF_SIZE           2048             /* hdr(10) + frame(1514) fits  */

#define VNET_HDR_LEN          10               /* legacy, no MRG_RXBUF        */

/* ----------------------- On-the-wire structs ------------------------------ */

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
    uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[QSIZE];
    uint16_t avail_event;
} __attribute__((packed));

/* ----------------------- Per-queue state ---------------------------------- */

struct vnet_queue {
    struct virtq_desc*  desc;
    struct virtq_avail* avail;
    struct virtq_used*  used;
    uint16_t last_used_idx;
};

struct virtio_net {
    uint16_t io_base;
    struct vnet_queue rx;
    struct vnet_queue tx;

    /* RX buffer pool — descriptor i permanently owns buffer i. */
    uint32_t rx_buf_phys[RX_BUFFERS];

    /* Single TX buffer (one outstanding frame at a time). */
    uint32_t tx_buf_phys;
};

static struct virtio_net g_vnet;
static struct net_device g_eth0;
static int g_present = 0;

/* ----------------------- Small helpers ------------------------------------ */

static inline void barrier(void) { __asm__ volatile ("" : : : "memory"); }

static void vn_select_queue(uint16_t io, uint16_t q) {
    outw(io + VN_OFF_QUEUE_SEL, q);
}

/* Allocate + wire one queue's memory (3 contiguous frames, qsize=256). */
static int vn_setup_queue(uint16_t io, uint16_t qidx, struct vnet_queue* q) {
    vn_select_queue(io, qidx);
    uint16_t qsize = inw(io + VN_OFF_QUEUE_SIZE);
    if (qsize != QSIZE) {
        kprintf("virtio-net: queue %u size=%u, expected %u\n", qidx, qsize, QSIZE);
        return -1;
    }
    uint32_t phys = pmm_alloc_contiguous(3);
    if (!phys) { kprintf("virtio-net: no frames for queue %u\n", qidx); return -2; }

    uint8_t* base = (uint8_t*)(uintptr_t)phys;
    for (uint32_t i = 0; i < QUEUE_BYTES; i++) base[i] = 0;

    q->desc  = (struct virtq_desc*) base;
    q->avail = (struct virtq_avail*)(base + sizeof(struct virtq_desc) * QSIZE);
    q->used  = (struct virtq_used*) (base + 4096 * 2);
    q->last_used_idx = 0;

    outl(io + VN_OFF_QUEUE_PFN, phys >> 12);
    return 0;
}

/* ----------------------- RX buffer posting -------------------------------- */

/* Put descriptor `di` (which permanently maps RX buffer di) into the RX avail
 * ring so the device can fill it. */
static void vn_rx_post(struct virtio_net* v, uint16_t di) {
    struct vnet_queue* q = &v->rx;
    q->desc[di].addr  = (uint64_t)v->rx_buf_phys[di];
    q->desc[di].len   = RX_BUF_SIZE;
    q->desc[di].flags = VRING_DESC_F_WRITE;     /* device writes into it       */
    q->desc[di].next  = 0;
    uint16_t pos = q->avail->idx % QSIZE;
    q->avail->ring[pos] = di;
    barrier();
    q->avail->idx++;
    barrier();
}

static int vn_init_rx(struct virtio_net* v) {
    for (uint16_t i = 0; i < RX_BUFFERS; i++) {
        uint32_t f = pmm_alloc_frame();          /* 4 KiB ≥ RX_BUF_SIZE        */
        if (!f) { kprintf("virtio-net: no RX frame %u\n", i); return -1; }
        v->rx_buf_phys[i] = f;
        vn_rx_post(v, i);
    }
    /* Kick the device: RX buffers are available. */
    outw(v->io_base + VN_OFF_QUEUE_NOTIFY, 0);   /* queue 0 = RX               */
    return 0;
}

static int vn_init_tx(struct virtio_net* v) {
    uint32_t f = pmm_alloc_frame();
    if (!f) { kprintf("virtio-net: no TX frame\n"); return -1; }
    v->tx_buf_phys = f;
    return 0;
}

/* ----------------------- net_device: transmit ----------------------------- */

static int vnet_transmit(struct net_device* dev, const void* frame, uint32_t len) {
    struct virtio_net* v = (struct virtio_net*)dev->priv;
    if (len > ETH_FRAME_MAX) return -1;

    /* Assemble [virtio_net_hdr | ethernet frame] into the TX buffer. */
    uint8_t* buf = (uint8_t*)(uintptr_t)v->tx_buf_phys;
    for (int i = 0; i < VNET_HDR_LEN; i++) buf[i] = 0;   /* zeroed header      */
    const uint8_t* f = (const uint8_t*)frame;
    for (uint32_t i = 0; i < len; i++) buf[VNET_HDR_LEN + i] = f[i];

    struct vnet_queue* q = &v->tx;
    q->desc[0].addr  = (uint64_t)v->tx_buf_phys;
    q->desc[0].len   = VNET_HDR_LEN + len;
    q->desc[0].flags = 0;                        /* device reads only          */
    q->desc[0].next  = 0;

    uint16_t pos = q->avail->idx % QSIZE;
    q->avail->ring[pos] = 0;
    barrier();
    q->avail->idx++;
    barrier();

    outw(v->io_base + VN_OFF_QUEUE_NOTIFY, 1);    /* queue 1 = TX               */

    /* Wait for completion (bounded, like virtio_blk). */
    uint32_t spins = 0;
    while (q->used->idx == q->last_used_idx) {
        hal_cpu_pause();
        if (++spins > 50000000u) { kprintf("virtio-net: TX timeout\n"); return -1; }
    }
    q->last_used_idx = q->used->idx;

    dev->tx_packets++; dev->tx_bytes += len;
    return 0;
}

/* ----------------------- net_device: RX poll ------------------------------ */

static void vnet_poll(struct net_device* dev) {
    struct virtio_net* v = (struct virtio_net*)dev->priv;
    struct vnet_queue* q = &v->rx;

    while (q->used->idx != q->last_used_idx) {
        uint16_t uidx = q->last_used_idx % QSIZE;
        struct virtq_used_elem* e = &q->used->ring[uidx];
        uint16_t di   = (uint16_t)e->id;
        uint32_t wlen = e->len;                  /* virtio hdr + frame bytes   */

        if (di < RX_BUFFERS && wlen > VNET_HDR_LEN) {
            const uint8_t* buf = (const uint8_t*)(uintptr_t)v->rx_buf_phys[di];
            net_rx(dev, buf + VNET_HDR_LEN, wlen - VNET_HDR_LEN);
        }

        q->last_used_idx++;
        /* Recycle the buffer back into the avail ring. */
        if (di < RX_BUFFERS) vn_rx_post(v, di);
    }
    /* Let the device know fresh buffers are available. */
    outw(v->io_base + VN_OFF_QUEUE_NOTIFY, 0);
}

/* ----------------------- DRIVER() lifecycle ------------------------------- */

static int vnet_probe(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    return pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_DEVICE, &pd) == 0 ? 0 : -1;
}

static int vnet_init(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_NET_DEVICE, &pd) != 0) return -1;

    uint16_t io = pci_bar_io_base(pd.bar[0]);
    if (!io) { kprintf("virtio-net: BAR0 is not I/O-space\n"); return -2; }

    /* Enable I/O + bus-master so the device can DMA our rings/buffers. */
    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND, cmd);

    g_vnet.io_base = io;

    /* Reset + handshake. */
    outb(io + VN_OFF_DEV_STATUS, 0);
    outb(io + VN_OFF_DEV_STATUS, VSTAT_ACKNOWLEDGE);
    outb(io + VN_OFF_DEV_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    /* Negotiate: accept only VIRTIO_NET_F_MAC (so the config MAC is valid).
     * We deliberately do NOT take MRG_RXBUF (keeps the header at 10 bytes)
     * or checksum-offload (we compute our own checksums). */
    uint32_t dev_feat = inl(io + VN_OFF_DEV_FEAT);
    uint32_t drv_feat = dev_feat & VIRTIO_NET_F_MAC;
    outl(io + VN_OFF_DRV_FEAT, drv_feat);
    outb(io + VN_OFF_DEV_STATUS,
         VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    if ((inb(io + VN_OFF_DEV_STATUS) & VSTAT_FEATURES_OK) == 0) {
        kprintf("virtio-net: FEATURES_OK rejected\n");
        return -3;
    }

    /* Read the MAC from device-specific config. */
    for (int i = 0; i < ETH_ALEN; i++)
        g_eth0.mac[i] = inb(io + VN_OFF_MAC + i);

    /* Set up both virtqueues. */
    if (vn_setup_queue(io, 0, &g_vnet.rx) != 0) return -4;   /* receiveq       */
    if (vn_setup_queue(io, 1, &g_vnet.tx) != 0) return -5;   /* transmitq      */
    if (vn_init_rx(&g_vnet) != 0) return -6;
    if (vn_init_tx(&g_vnet) != 0) return -7;

    /* Driver ready. */
    outb(io + VN_OFF_DEV_STATUS,
         VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);

    /* Register the abstract net device.  QEMU SLIRP defaults:
     * guest 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3. */
    g_eth0.name     = "eth0";
    g_eth0.ip       = IPV4(10, 0, 2, 15);
    g_eth0.netmask  = IPV4(255, 255, 255, 0);
    g_eth0.gateway  = IPV4(10, 0, 2, 2);
    g_eth0.mtu      = ETH_MTU;
    g_eth0.transmit = vnet_transmit;
    g_eth0.poll     = vnet_poll;
    g_eth0.priv     = &g_vnet;
    net_register(&g_eth0);

    g_present = 1;
    kprintf("virtio-net: up at PCI %u:%u.%u io=%x\n", pd.bus, pd.slot, pd.func, io);
    return 0;
}

static const struct driver_ops vnet_ops = {
    .probe    = vnet_probe,
    .init     = vnet_init,
    .shutdown = NULL,
};

DRIVER(virtio_net, "net", &vnet_ops, NULL);
