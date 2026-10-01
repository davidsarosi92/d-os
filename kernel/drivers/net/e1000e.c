/* =============================================================================
 * e1000e.c — Intel 82574L (e1000e) Ethernet, polled (§M85 stage 4b, 2026-09-26).
 *
 * Written for QEMU's `sbsa-ref`, whose only NIC is an e1000e on PCIe — the
 * machine a standard ARM server looks like has no virtio-mmio — and portable by
 * construction: nothing here is ARM-specific, so the same file drives an
 * e1000e on x86 (`-M q35 -device e1000e`) and on real hardware of that family.
 *
 * THE SHAPE, and why each piece:
 *   - LEGACY descriptors (16 bytes: address, length, status).  The 82574's
 *     extended and advanced formats buy offloads this stack does not use; the
 *     legacy format is the one every member of the family accepts.
 *   - 64-bit DMA.  Descriptor bases are programmed with both halves: sbsa-ref
 *     has no RAM below 4 GiB, so a driver that assumed 32-bit DMA would have
 *     no memory to give the card at all (the same lesson AHCI and xHCI paid
 *     for in this stage).
 *   - INTERRUPT-DRIVEN WHEN THE LINE IS KNOWN (2026-09-27).  The stack's poller
 *     (netd, §M55) calls ->poll with a 10 ms backstop, so correctness never
 *     depended on an IRQ; with pci_device.irq_line routed (the ACPI _PRT on
 *     sbsa-ref, the legacy line on x86) the ISR reads ICR — which is what
 *     deasserts the level-triggered INTx — and wakes netd, so a frame is
 *     picked up when it arrives instead of at the next backstop.  An unrouted
 *     line (0xFF) keeps the old polled behaviour, and says so.
 *   - The MAC comes from RAL0/RAH0, which the card loads from its EEPROM at
 *     reset; nothing is invented.
 *
 * References: Intel 82574 GbE Controller Family datasheet, §10 (registers),
 * §7.1/§7.2 (receive/transmit descriptors).
 * ============================================================================= */

#include "driver.h"
#include "hwdev.h"
#include "net.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "printf.h"
#include "task.h"
#include "timer.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

#define E1000_VENDOR   0x8086
#define E1000E_82574L  0x10D3

/* Registers (byte offsets in BAR0). */
#define R_CTRL    0x0000
#define R_STATUS  0x0008
#define R_ICR     0x00C0
#define R_IMS     0x00D0
#define R_IMC     0x00D8
/* Interrupt causes this driver asks for: link change, RX descriptors low,
 * RX overrun, RX timer (a frame arrived). */
#define IMS_WANT  ((1u << 2) | (1u << 4) | (1u << 6) | (1u << 7))
#define R_RCTL    0x0100
#define R_TCTL    0x0400
#define R_TIPG    0x0410
#define R_RDBAL   0x2800
#define R_RDBAH   0x2804
#define R_RDLEN   0x2808
#define R_RDH     0x2810
#define R_RDT     0x2818
#define R_TDBAL   0x3800
#define R_TDBAH   0x3804
#define R_TDLEN   0x3808
#define R_TDH     0x3810
#define R_TDT     0x3818
#define R_MTA     0x5200
#define R_RAL0    0x5400
#define R_RAH0    0x5404

#define CTRL_RST   (1u << 26)
#define CTRL_SLU   (1u << 6)
#define CTRL_ASDE  (1u << 5)
#define STATUS_LU  (1u << 1)

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)       /* strip the CRC: the stack wants frames */

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define TXCMD_EOP  0x01
#define TXCMD_IFCS 0x02
#define TXCMD_RS   0x08
#define DESC_DD    0x01

#define NRX  32                      /* ring lengths: multiples of 8 (128 B) */
#define NTX  16
#define BUFSZ 2048

struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

static struct {
    volatile uint8_t* regs;
    struct rx_desc*   rx;  uint64_t rx_phys;
    struct tx_desc*   tx;  uint64_t tx_phys;
    uint8_t*          rxbuf; uint64_t rxbuf_phys;
    uint8_t*          txbuf; uint64_t txbuf_phys;
    uint32_t          rx_next, tx_next;
    struct pci_device pd;
    int               present;
} g_e;

static struct net_device g_dev;

static inline uint32_t rd(uint32_t off)             { return *(volatile uint32_t*)(g_e.regs + off); }
static inline void     wr(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_e.regs + off) = v; }

static int e1000e_transmit(struct net_device* dev, const void* frame, uint32_t len) {
    (void)dev;
    if (len > BUFSZ) return -1;
    uint32_t i = g_e.tx_next;
    struct tx_desc* d = &g_e.tx[i];
    /* The slot is free once the card has written it back (DD) — or was never
     * used (cmd 0).  Bounded: a card that stopped completing costs a dropped
     * frame, not a hung sender. */
    for (int spin = 0; d->cmd && !(d->status & DESC_DD); spin++) {
        if (spin > 100000) { dev->rx_dropped++; return -1; }
        __asm__ volatile ("" ::: "memory");
    }
    uint8_t* b = g_e.txbuf + i * BUFSZ;
    const uint8_t* f = frame;
    for (uint32_t k = 0; k < len; k++) b[k] = f[k];
    d->addr   = g_e.txbuf_phys + i * BUFSZ;
    d->len    = (uint16_t)len;
    d->cso    = 0; d->css = 0; d->special = 0;
    d->status = 0;
    d->cmd    = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
    __asm__ volatile ("" ::: "memory");
    g_e.tx_next = (i + 1) % NTX;
    wr(R_TDT, g_e.tx_next);                   /* hand the descriptor over */
    g_dev.tx_packets++; g_dev.tx_bytes += len;
    return 0;
}

static void e1000e_poll(struct net_device* dev) {
    for (int budget = 0; budget < NRX; budget++) {
        uint32_t i = g_e.rx_next;
        struct rx_desc* d = &g_e.rx[i];
        if (!(d->status & DESC_DD)) break;
        uint16_t len = d->len;
        if (!d->errors && len >= 14)
            net_rx(dev, g_e.rxbuf + i * BUFSZ, len);
        else
            dev->rx_dropped++;
        d->status = 0;
        g_e.rx_next = (i + 1) % NRX;
        wr(R_RDT, i);                         /* the slot is the card's again */
    }
}

/* §M87 — carrier, straight from STATUS.LU. */
static int e1000e_link(struct net_device* dev) {
    (void)dev;
    return (rd(R_STATUS) & STATUS_LU) != 0;
}

/* The ISR does two things and no third (§M55): ACK (reading ICR clears the
 * causes and drops the shared level line) and WAKE the poller.  It does not
 * drain the ring — draining runs the stack, which may transmit and wait.
 * A zero ICR means the line was raised by a device sharing it. */
static void e1000e_irq(void) {
    if (!g_e.regs) return;
    uint32_t icr = rd(R_ICR);
    if (!icr || !g_e.present) return;
    net_rx_irq(&g_dev);
}

static int e1000e_probe(void* ctx) {
    (void)ctx;
    return pci_find_device(E1000_VENDOR, E1000E_82574L, &g_e.pd) == 0 ? 0 : -1;
}

static int e1000e_init(void* ctx) {
    (void)ctx;
    if (pci_find_device(E1000_VENDOR, E1000E_82574L, &g_e.pd) != 0) return -1;
    struct pci_device* pd = &g_e.pd;
    uint16_t cmd = pci_read16(pd->bus, pd->slot, pd->func, PCI_COMMAND);
    pci_write16(pd->bus, pd->slot, pd->func, PCI_COMMAND,
                cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    uint32_t bar0 = pd->bar[0];
    if (bar0 & 1) { kprintf("e1000e: BAR0 is I/O space?\n"); return -1; }
    uint32_t mmio = bar0 & ~0xFu;
    if (!mmio) { kprintf("e1000e: BAR0 is not programmed\n"); return -1; }
    /* §M90 — through hal_mmio_map (where this arch keeps device memory). */
    g_e.regs = (volatile uint8_t*)hal_mmio_map(mmio, 0x20000);
    if (!g_e.regs) { kprintf("e1000e: cannot map the registers\n"); return -1; }

    /* Reset, then mask every interrupt source: this driver polls. */
    wr(R_IMC, 0xFFFFFFFFu);
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000 && (rd(R_CTRL) & CTRL_RST); i++) task_msleep(1);
    wr(R_IMC, 0xFFFFFFFFu);
    (void)rd(R_ICR);
    wr(R_CTRL, rd(R_CTRL) | CTRL_SLU | CTRL_ASDE);

    uint32_t ral = rd(R_RAL0), rah = rd(R_RAH0);
    for (int k = 0; k < 4; k++) g_dev.mac[k] = (uint8_t)(ral >> (8 * k));
    g_dev.mac[4] = (uint8_t)rah; g_dev.mac[5] = (uint8_t)(rah >> 8);
    for (int k = 0; k < 128; k++) wr(R_MTA + 4 * k, 0);

    /* Rings and buffers: one allocation each, 64-bit reachable. */
    pmm_phys_t r = pmm_alloc_contiguous_dma(1, 64);
    pmm_phys_t rb = pmm_alloc_contiguous_dma((NRX * BUFSZ) / 4096, 64);
    pmm_phys_t tb = pmm_alloc_contiguous_dma((NTX * BUFSZ) / 4096, 64);
    if (!r || !rb || !tb) { kprintf("e1000e: no DMA memory\n"); return -1; }
    uint8_t* rv = (uint8_t*)phys_to_virt(r);
    for (int k = 0; k < 4096; k++) rv[k] = 0;
    g_e.rx = (struct rx_desc*)rv;                 g_e.rx_phys = r;
    g_e.tx = (struct tx_desc*)(rv + 2048);        g_e.tx_phys = r + 2048;
    g_e.rxbuf = (uint8_t*)phys_to_virt(rb);       g_e.rxbuf_phys = rb;
    g_e.txbuf = (uint8_t*)phys_to_virt(tb);       g_e.txbuf_phys = tb;

    for (int k = 0; k < NRX; k++) g_e.rx[k].addr = g_e.rxbuf_phys + (uint64_t)k * BUFSZ;
    wr(R_RDBAL, (uint32_t)g_e.rx_phys);  wr(R_RDBAH, (uint32_t)(g_e.rx_phys >> 32));
    wr(R_RDLEN, NRX * sizeof(struct rx_desc));
    wr(R_RDH, 0);  wr(R_RDT, NRX - 1);
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);  /* BSIZE 00 = 2048 bytes */

    wr(R_TDBAL, (uint32_t)g_e.tx_phys);  wr(R_TDBAH, (uint32_t)(g_e.tx_phys >> 32));
    wr(R_TDLEN, NTX * sizeof(struct tx_desc));
    wr(R_TDH, 0);  wr(R_TDT, 0);
    wr(R_TIPG, 0x0060200Au);
    wr(R_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));

    g_dev.name     = net_find("eth0") ? "eth1" : "eth0";
    g_dev.ip       = IPV4(10, 0, 2, 15);          /* SLIRP defaults; DHCP */
    g_dev.netmask  = IPV4(255, 255, 255, 0);       /* replaces them if run */
    g_dev.gateway  = IPV4(10, 0, 2, 2);
    g_dev.mtu      = ETH_MTU;
    g_dev.transmit = e1000e_transmit;
    g_dev.poll     = e1000e_poll;
    g_dev.link     = e1000e_link;
    g_dev.priv     = &g_e;
    net_register(&g_dev);
    g_e.present = 1;

    int irq_ok = 0;
    if (pd->irq_line != 0xFF && pd->irq_line != 0 &&
        hal_irq_attach(pd->irq_line, e1000e_irq) == 0) {
        (void)rd(R_ICR);                          /* nothing stale pending */
        wr(R_IMS, IMS_WANT);
        irq_ok = 1;
    }

    kprintf("e1000e: %s up at PCI %u:%u.%u, regs %x, mac %x:%x:%x:%x:%x:%x, link %s, "
            "%s %u\n", g_dev.name, pd->bus, pd->slot, pd->func, mmio,
            g_dev.mac[0], g_dev.mac[1], g_dev.mac[2], g_dev.mac[3], g_dev.mac[4], g_dev.mac[5],
            (rd(R_STATUS) & STATUS_LU) ? "up" : "down",
            irq_ok ? "interrupt on line" : "polled (no routed INTx), line",
            (unsigned)pd->irq_line);
    return 0;
}

static int e1000e_shutdown(void* ctx) {
    (void)ctx;
    if (!g_e.present) return 0;
    if (net_unregister(&g_dev) != 0) return -1;
    wr(R_IMC, 0xFFFFFFFFu);                       /* the ISR stays attached: quiet it */
    wr(R_RCTL, 0); wr(R_TCTL, 0);
    g_e.present = 0;
    return 0;
}

static const struct driver_ops e1000e_ops = {
    .probe = e1000e_probe, .init = e1000e_init, .shutdown = e1000e_shutdown,
};

DRIVER_MATCH(m_e1000e) = { .driver = "e1000e", .vendor = E1000_VENDOR, .device = E1000E_82574L };
DRIVER_EX(e1000e, "net", &e1000e_ops, NULL, DOMAIN_KERNEL, DRVF_DMA);
