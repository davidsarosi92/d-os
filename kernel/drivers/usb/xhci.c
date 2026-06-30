/* =============================================================================
 * xhci.c — eXtensible Host Controller Interface (USB 3.x host) driver.
 *
 * Scope of this M15 first cut: detect a single xHCI controller on PCI,
 * bring up the bare minimum to talk to one device on root-port 0, do
 * the four enumeration commands (Enable Slot → Address Device → Get
 * Device Descriptor → Get Config Descriptor + Set Configuration), and
 * then poll a single interrupt-IN endpoint via the timer tick.  Class
 * dispatch is hard-coded: if the device's interface is HID/Boot/Kbd,
 * we hand reports to usb_hid_kbd_handle_report().
 *
 * Deliberately out of scope:
 *   - Hubs (no recursive enumeration; root ports only)
 *   - Multiple simultaneous devices
 *   - Bulk / isochronous transfers
 *   - MSI / MSI-X (we poll the Event Ring from the PIT IRQ)
 *   - 64-byte device contexts (we assert CSZ=0 → 32-byte)
 *   - Scratchpad buffers (we abort if MaxScratchpadBufs > 0; QEMU's
 *     qemu-xhci reports 0 so this is fine for the test target)
 *
 * Reference: xHCI 1.2 spec, §4 (Software architecture overview) and §6
 * (Data Structures).
 *
 * High-level data structures, all DMA-coherent (PMM-frame allocated):
 *
 *   DCBAA  — Device Context Base Address Array (256 × 8 B = 2 KiB).
 *            DCBAA[slot] points at that slot's Device Context.
 *
 *   Device Context — Slot Context + 31 Endpoint Contexts (32 entries
 *            × 32 B = 1 KiB).  Owned by the HC; the driver only writes
 *            it via Input Context + Address Device / Configure Endpoint.
 *
 *   Input Context — Input Control Context + Slot Context + 31 EP
 *            Contexts (33 × 32 B = 1.04 KiB; we round to a frame).
 *            Used as the parameter to Address Device / Configure
 *            Endpoint commands.
 *
 *   Command Ring — 256 TRBs (4 KiB).  Driver enqueues commands; HC
 *            consumes them.  Cycle Bit (CCS) toggles each wrap.
 *
 *   Event Ring  — 1 segment of 256 TRBs (4 KiB) + a 1-entry Event Ring
 *            Segment Table (ERST).  HC enqueues events; driver
 *            consumes.  Cycle Bit (CCS) toggles each wrap.
 *
 *   Transfer Ring — one per active endpoint (EP0 and the HID interrupt
 *            IN endpoint).  Same TRB layout as Command Ring.
 *
 * TRB format (16 bytes):
 *   [0..3]   Parameter (low 32 bits) — usually pointer to data
 *   [4..7]   Parameter (high 32 bits) — 0 on 32-bit kernels (no PAE today)
 *   [8..11]  Status   — length / completion code
 *   [12..15] Control  — cycle bit, TRB type, flags
 * ============================================================================= */

#include "pci.h"
#include "hal.h"
#include "vmm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "printf.h"
#include "module.h"
#include "driver.h"
#include "timer.h"
#include "usb.h"
#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Register layout.  We use byte offsets from the relevant base.
 * --------------------------------------------------------------------------- */

/* Capability registers (read-only). */
#define XHCI_CAP_CAPLENGTH   0x00      /* uint8  — operational regs offset */
#define XHCI_CAP_HCIVERSION  0x02      /* uint16 */
#define XHCI_CAP_HCSPARAMS1  0x04      /* uint32 — MaxSlots:0-7 MaxIntrs:8-18 MaxPorts:24-31 */
#define XHCI_CAP_HCSPARAMS2  0x08      /* uint32 — IST/ERST/scratchpad */
#define XHCI_CAP_HCCPARAMS1  0x10      /* uint32 — AC64:0 CSZ:2 PPC:3 xECP:16-31 */
#define XHCI_CAP_DBOFF       0x14      /* uint32 — doorbell array offset */
#define XHCI_CAP_RTSOFF      0x18      /* uint32 — runtime registers offset */

/* Operational registers (offset = CAPLENGTH). */
#define XHCI_OP_USBCMD       0x00      /* R/S:0 HCRST:1 INTE:2 HSEE:3 */
#define XHCI_OP_USBSTS       0x04      /* HCH:0 HSE:2 EINT:3 PCD:4 CNR:11 */
#define XHCI_OP_PAGESIZE     0x08
#define XHCI_OP_DNCTRL       0x14
#define XHCI_OP_CRCR_LO      0x18      /* RCS:0 CS:1 CA:2 CRR:3 + low pointer */
#define XHCI_OP_CRCR_HI      0x1C
#define XHCI_OP_DCBAAP_LO    0x30
#define XHCI_OP_DCBAAP_HI    0x34
#define XHCI_OP_CONFIG       0x38      /* low 8 bits = MaxSlotsEn */
#define XHCI_OP_PORTSC_BASE  0x400     /* +0x10 per port */

/* USBCMD bits. */
#define XHCI_USBCMD_RS       (1u << 0)
#define XHCI_USBCMD_HCRST    (1u << 1)
#define XHCI_USBCMD_INTE     (1u << 2)

/* USBSTS bits. */
#define XHCI_USBSTS_HCH      (1u << 0)
#define XHCI_USBSTS_CNR      (1u << 11)

/* PORTSC bits (subset). */
#define XHCI_PORTSC_CCS      (1u << 0)   /* current connect status */
#define XHCI_PORTSC_PED      (1u << 1)   /* port enabled (read-only by SW) */
#define XHCI_PORTSC_PR       (1u << 4)   /* port reset */
#define XHCI_PORTSC_PLS_MASK (0xfu << 5) /* port link state */
#define XHCI_PORTSC_PP       (1u << 9)   /* port power */
#define XHCI_PORTSC_SPEED(p) (((p) >> 10) & 0xf)
#define XHCI_PORTSC_CSC      (1u << 17)  /* connect status change */
#define XHCI_PORTSC_PRC      (1u << 21)  /* port reset change */

/* Runtime registers — interrupter 0 starts at RTSOFF + 0x20. */
#define XHCI_RT_IR0_IMAN     0x20
#define XHCI_RT_IR0_IMOD     0x24
#define XHCI_RT_IR0_ERSTSZ   0x28
#define XHCI_RT_IR0_ERSTBA_LO 0x30
#define XHCI_RT_IR0_ERSTBA_HI 0x34
#define XHCI_RT_IR0_ERDP_LO  0x38
#define XHCI_RT_IR0_ERDP_HI  0x3C

/* Doorbell registers — DB[0] = Command Ring, DB[slot] = device. */
#define XHCI_DB(n)           ((n) * 4)

/* TRB types (Control field bits 10..15, after shift). */
#define TRB_TYPE_NORMAL          1
#define TRB_TYPE_SETUP           2
#define TRB_TYPE_DATA            3
#define TRB_TYPE_STATUS          4
#define TRB_TYPE_LINK            6
#define TRB_TYPE_NOOP_CMD        23
#define TRB_TYPE_ENABLE_SLOT     9
#define TRB_TYPE_ADDRESS_DEVICE  11
#define TRB_TYPE_CONFIG_EP       12
#define TRB_TYPE_EVAL_CTX        13

#define TRB_TYPE_TRANSFER_EVENT  32
#define TRB_TYPE_CMD_COMPLETION  33
#define TRB_TYPE_PORT_STATUS_CHG 34

/* TRB control-field shifts. */
#define TRB_CYCLE_BIT           (1u << 0)
#define TRB_TC_BIT              (1u << 1)   /* Toggle Cycle (Link TRB) */
#define TRB_CH_BIT              (1u << 4)   /* Chain bit */
#define TRB_IOC_BIT             (1u << 5)   /* Interrupt-On-Completion */
#define TRB_IDT_BIT             (1u << 6)   /* Immediate Data */
#define TRB_TYPE_SHIFT          10
#define TRB_DIR_IN              (1u << 16)  /* Setup/Data: direction */
#define TRB_TRT_NO_DATA         (0u << 16)  /* Setup: Transfer Type field */
#define TRB_TRT_OUT_DATA        (2u << 16)
#define TRB_TRT_IN_DATA         (3u << 16)
#define TRB_SLOT_SHIFT          24

/* Completion codes (Status field bits 24..31). */
#define CC_SUCCESS              1

/* ---------------------------------------------------------------------------
 * TRB struct — just 4 dwords, no bitfields (portable, predictable layout).
 * --------------------------------------------------------------------------- */
struct xhci_trb {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed));

/* Ring state — applies to Command Ring, Transfer Rings, and Event Ring.
 *
 * M20.6.2 audit (2026-06-30): `phys` stays uint32_t because PMM only
 * manages low memory (BUDDY_MAX_FRAMES * 4 KiB ≤ 1 GiB today on both
 * archs).  When §M19.5.1 populates HIGHMEM the field needs to widen
 * to uint64_t and the `_HI` MMIO writes (currently constant-zero in
 * evt_drain, init_minimum, address_device, etc.) need to carry the
 * actual high 32 bits.  The spec already supports 64-bit DMA (xHCI
 * 1.2 §5.4.2 "DCBAAP", §5.5.2.3.3 "ERDP", etc.) — we're just opting
 * into <4 GiB DMA on day one to keep the diff small. */
struct xhci_ring {
    struct xhci_trb* trbs;          /* virt = phys (identity-mapped frame) */
    uint32_t         phys;
    uint32_t         size;          /* TRB count (last slot is Link for cmd/tr) */
    uint32_t         enqueue;       /* index of next TRB to fill */
    uint32_t         dequeue;       /* index of next TRB to consume (event ring) */
    uint32_t         cycle;         /* current Producer Cycle State */
};

/* The single HC instance (no multi-controller support today). */
static struct {
    int             present;
    volatile uint8_t* mmio;         /* mapped base */
    volatile uint8_t* op;           /* mmio + CAPLENGTH */
    volatile uint8_t* rt;           /* mmio + RTSOFF */
    volatile uint8_t* db;           /* mmio + DBOFF */

    uint32_t        max_slots;
    uint32_t        max_ports;
    uint32_t        ctx_size;       /* 32 or 64 (we require 32) */

    uint64_t*       dcbaa;          /* 256 entries */

    struct xhci_ring cmd_ring;
    struct xhci_ring evt_ring;
    uint32_t         erst_phys;     /* Event Ring Segment Table base (phys) */

    /* The single enumerated device — slot 1's EP0 + HID Interrupt IN. */
    int              slot_id;
    int              device_addressed;
    uint8_t*         input_ctx;
    uint8_t*         dev_ctx;
    uint32_t         input_ctx_phys;
    uint32_t         dev_ctx_phys;

    struct xhci_ring ep0_ring;
    struct xhci_ring intr_in_ring;
    int              hid_present;       /* set after Configure Endpoint succeeds */
    int              hid_ep_num;        /* 1..15 endpoint number */
    uint32_t         hid_pkt_size;
    uint8_t*         intr_in_buf;       /* 8-byte HID report DMA target */
    uint32_t         intr_in_buf_phys;
} xhc;

/* ---------------------------------------------------------------------------
 * MMIO helpers — explicit volatile loads/stores.
 * --------------------------------------------------------------------------- */
static inline uint32_t mmio_r32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}
static inline void mmio_w32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}
static inline uint8_t mmio_r8(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint8_t*)(base + off);
}
static inline uint16_t mmio_r16(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint16_t*)(base + off);
}

/* ---------------------------------------------------------------------------
 * Ring helpers.  All rings live in one PMM frame (4 KiB / 16 = 256 TRBs).
 * For Command Ring and Transfer Rings we reserve the last TRB as a Link
 * back to TRB 0; for the Event Ring we don't (HC follows ERST entries).
 * --------------------------------------------------------------------------- */

static int ring_alloc(struct xhci_ring* r, int with_link_trb) {
    uint32_t frame = pmm_alloc_frame();
    if (!frame) return -1;
    r->trbs    = (struct xhci_trb*)(uintptr_t)frame;
    r->phys    = frame;
    r->size    = 4096 / sizeof(struct xhci_trb);   /* 256 */
    r->enqueue = 0;
    r->dequeue = 0;
    r->cycle   = 1;                                 /* PCS starts at 1 */
    /* Zero the whole frame so unused TRBs have cycle=0 (HC treats as
     * not yet produced).  PMM hands us an uninitialized frame. */
    uint32_t* p = (uint32_t*)(uintptr_t)frame;
    for (int i = 0; i < 1024; i++) p[i] = 0;
    if (with_link_trb) {
        /* Last TRB = Link to TRB 0 with Toggle Cycle, so the producer
         * cycle inverts on wrap and the consumer can tell new from old. */
        struct xhci_trb* link = &r->trbs[r->size - 1];
        link->param_lo = r->phys;
        link->param_hi = 0;
        link->status   = 0;
        link->control  = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TC_BIT;
        /* No cycle bit on Link initially; cycle gets set when we wrap. */
        r->size -= 1;       /* effective producer slots */
    }
    return 0;
}

/* Enqueue one TRB into a producer ring (Command Ring or Transfer Ring).
 * Caller fills the 16 bytes; we set the cycle bit and bump the enqueue
 * pointer (wrapping over the Link TRB and toggling our cycle). */
static struct xhci_trb* ring_enqueue(struct xhci_ring* r,
                                     uint32_t plo, uint32_t phi,
                                     uint32_t status, uint32_t control) {
    struct xhci_trb* t = &r->trbs[r->enqueue];
    t->param_lo = plo;
    t->param_hi = phi;
    t->status   = status;
    /* Compose control with our current cycle bit. */
    uint32_t c = control & ~TRB_CYCLE_BIT;
    if (r->cycle) c |= TRB_CYCLE_BIT;
    t->control  = c;

    r->enqueue++;
    if (r->enqueue >= r->size) {
        /* About to step on the Link TRB.  Update its cycle bit to our
         * current cycle (so HC processes it), then toggle our cycle and
         * wrap to 0. */
        struct xhci_trb* link = &r->trbs[r->size];   /* size was decremented at alloc */
        uint32_t lc = link->control & ~TRB_CYCLE_BIT;
        if (r->cycle) lc |= TRB_CYCLE_BIT;
        link->control = lc;
        r->cycle ^= 1;
        r->enqueue = 0;
    }
    return t;
}

/* ---------------------------------------------------------------------------
 * Event Ring consumer.
 *
 * The HC writes TRBs with the OPPOSITE cycle bit from our consumer
 * cycle to flip "produced" → "consumed" on wrap.  Walk the segment in
 * order, processing every TRB whose cycle matches, then write the
 * ERDP back (with the Event Handler Busy bit clear).
 * --------------------------------------------------------------------------- */

/* Called whenever the driver wants events drained.  Returns the count
 * of events consumed.  Optionally fills `out_completion` with the first
 * Command Completion Event seen (used by command_submit_wait). */
static int evt_drain(struct xhci_trb* out_completion) {
    int n = 0;
    int saw_completion = (out_completion == NULL);
    for (;;) {
        struct xhci_trb* t = &xhc.evt_ring.trbs[xhc.evt_ring.dequeue];
        uint32_t cyc = t->control & TRB_CYCLE_BIT;
        if (cyc != xhc.evt_ring.cycle) break;       /* nothing new */

        uint32_t type = (t->control >> TRB_TYPE_SHIFT) & 0x3F;
        if (type == TRB_TYPE_CMD_COMPLETION && out_completion && !saw_completion) {
            *out_completion = *t;
            saw_completion = 1;
        } else if (type == TRB_TYPE_TRANSFER_EVENT) {
            /* For the HID interrupt-IN endpoint, the report has already
             * been DMA'd into intr_in_buf — hand it to the class
             * driver.  We don't differentiate endpoints today; only the
             * HID endpoint is active. */
            uint32_t cc = (t->status >> 24) & 0xFF;
            if (cc == CC_SUCCESS && xhc.hid_present && xhc.intr_in_buf) {
                usb_hid_kbd_handle_report(xhc.intr_in_buf);
                /* Re-arm: queue another Normal TRB on the interrupt ring. */
                ring_enqueue(&xhc.intr_in_ring,
                             xhc.intr_in_buf_phys, 0,
                             xhc.hid_pkt_size,
                             (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC_BIT);
                /* Ring this endpoint's doorbell.  EP0=1; EP1 IN=3 (2*N+1).
                 * We snapshotted hid_ep_num at config time. */
                uint32_t db_target = (xhc.hid_ep_num * 2) + 1;
                mmio_w32(xhc.db, XHCI_DB(xhc.slot_id), db_target);
            }
        }
        /* Advance dequeue with wrap (no Link TRB on event ring — the
         * ERST tells the HC where to wrap on its side). */
        xhc.evt_ring.dequeue++;
        if (xhc.evt_ring.dequeue >= xhc.evt_ring.size) {
            xhc.evt_ring.dequeue = 0;
            xhc.evt_ring.cycle ^= 1;
        }
        n++;
    }
    /* Update ERDP — write the current dequeue pointer with the Event
     * Handler Busy bit (bit 3) set to clear it. */
    if (n > 0) {
        uint32_t erdp_phys = xhc.evt_ring.phys
                           + xhc.evt_ring.dequeue * sizeof(struct xhci_trb);
        mmio_w32(xhc.rt, XHCI_RT_IR0_ERDP_LO, erdp_phys | (1u << 3));
        mmio_w32(xhc.rt, XHCI_RT_IR0_ERDP_HI, 0);
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Command submit + wait.
 *
 * Enqueue a command TRB on the Command Ring, ring DB[0] (command
 * doorbell uses target=0), then poll the Event Ring for a Command
 * Completion Event.  Returns the completion code (1 = success).
 * --------------------------------------------------------------------------- */
static int cmd_submit_wait(uint32_t plo, uint32_t phi,
                           uint32_t status, uint32_t control,
                           uint32_t* out_slot_id) {
    ring_enqueue(&xhc.cmd_ring, plo, phi, status, control);
    mmio_w32(xhc.db, XHCI_DB(0), 0);

    /* Poll for completion with a generous timeout — qemu-xhci usually
     * responds within microseconds, but we don't have IRQ-driven
     * delivery yet so a tick-based bound is enough. */
    uint64_t deadline = timer_ticks_ms() + 200;
    while (timer_ticks_ms() < deadline) {
        struct xhci_trb evt;
        int n = evt_drain(&evt);
        if (n > 0 && ((evt.control >> TRB_TYPE_SHIFT) & 0x3F) == TRB_TYPE_CMD_COMPLETION) {
            uint32_t cc = (evt.status >> 24) & 0xFF;
            if (out_slot_id) *out_slot_id = (evt.control >> TRB_SLOT_SHIFT) & 0xFF;
            return (int)cc;
        }
    }
    return -1;  /* timeout */
}

/* ---------------------------------------------------------------------------
 * Control transfer on EP0 — Setup + (optional Data) + Status.
 *
 * Builds the TRBs in the EP0 Transfer Ring, rings DB[slot] with
 * target=1 (= EP0 IN/OUT control), and polls the Event Ring for the
 * Transfer Event matching the Status TRB.  Returns 0 on success.
 * --------------------------------------------------------------------------- */
static int ep0_control_xfer(uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                            void* data_buf, uint32_t data_phys) {
    /* Setup Stage TRB — 8 bytes of setup packet packed into the param
     * field (Immediate Data).  We hand-pack rather than using the
     * struct to avoid endian games. */
    uint32_t plo = (uint32_t)bmRequestType
                 | ((uint32_t)bRequest << 8)
                 | ((uint32_t)wValue   << 16);
    uint32_t phi = (uint32_t)wIndex | ((uint32_t)wLength << 16);

    uint32_t trt = (wLength == 0) ? TRB_TRT_NO_DATA
                : (bmRequestType & USB_DIR_IN) ? TRB_TRT_IN_DATA
                                               : TRB_TRT_OUT_DATA;
    ring_enqueue(&xhc.ep0_ring, plo, phi, 8 /* TRB Transfer Length */,
                 (TRB_TYPE_SETUP << TRB_TYPE_SHIFT) | TRB_IDT_BIT | trt);

    /* Data Stage TRB (omitted if wLength == 0). */
    if (wLength > 0) {
        uint32_t ctrl = (TRB_TYPE_DATA << TRB_TYPE_SHIFT);
        if (bmRequestType & USB_DIR_IN) ctrl |= TRB_DIR_IN;
        ring_enqueue(&xhc.ep0_ring, data_phys, 0, wLength, ctrl);
    }

    /* Status Stage TRB — direction is opposite of the data direction;
     * for no-data transfers, direction is IN.  IOC so we get an event. */
    uint32_t status_ctrl = (TRB_TYPE_STATUS << TRB_TYPE_SHIFT) | TRB_IOC_BIT;
    int data_was_in = (wLength > 0) && (bmRequestType & USB_DIR_IN);
    if (wLength == 0 || !data_was_in) status_ctrl |= TRB_DIR_IN;
    ring_enqueue(&xhc.ep0_ring, 0, 0, 0, status_ctrl);

    /* Doorbell: EP0 control endpoint = target 1. */
    mmio_w32(xhc.db, XHCI_DB(xhc.slot_id), 1);

    /* Wait for the Status-stage Transfer Event. */
    uint64_t deadline = timer_ticks_ms() + 200;
    while (timer_ticks_ms() < deadline) {
        struct xhci_trb evt;
        /* Drain any pending events; transfer events fold into the
         * Event Ring without us needing the dummy out_completion path. */
        /* We watch for ANY Transfer Event here — but evt_drain
         * currently consumes them without returning, so we have to
         * peek differently.  Simplify: poll the dequeue pointer
         * directly for the next event-cycle TRB. */
        struct xhci_trb* t = &xhc.evt_ring.trbs[xhc.evt_ring.dequeue];
        if ((t->control & TRB_CYCLE_BIT) == xhc.evt_ring.cycle) {
            uint32_t type = (t->control >> TRB_TYPE_SHIFT) & 0x3F;
            evt = *t;
            xhc.evt_ring.dequeue++;
            if (xhc.evt_ring.dequeue >= xhc.evt_ring.size) {
                xhc.evt_ring.dequeue = 0;
                xhc.evt_ring.cycle ^= 1;
            }
            uint32_t erdp_phys = xhc.evt_ring.phys
                               + xhc.evt_ring.dequeue * sizeof(struct xhci_trb);
            mmio_w32(xhc.rt, XHCI_RT_IR0_ERDP_LO, erdp_phys | (1u << 3));
            mmio_w32(xhc.rt, XHCI_RT_IR0_ERDP_HI, 0);

            if (type == TRB_TYPE_TRANSFER_EVENT) {
                uint32_t cc = (evt.status >> 24) & 0xFF;
                /* In data buf, ignore — caller asked for `wLength`. */
                (void)data_buf;
                return (cc == CC_SUCCESS) ? 0 : -1;
            }
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Reset + bring-up.
 * --------------------------------------------------------------------------- */

static int xhci_reset_and_init(void) {
    /* Read cap regs. */
    uint8_t caplen = mmio_r8(xhc.mmio, XHCI_CAP_CAPLENGTH);
    uint32_t hcs1  = mmio_r32(xhc.mmio, XHCI_CAP_HCSPARAMS1);
    uint32_t hcs2  = mmio_r32(xhc.mmio, XHCI_CAP_HCSPARAMS2);
    uint32_t hcc1  = mmio_r32(xhc.mmio, XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = mmio_r32(xhc.mmio, XHCI_CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff= mmio_r32(xhc.mmio, XHCI_CAP_RTSOFF) & ~0x1Fu;

    xhc.op = xhc.mmio + caplen;
    xhc.rt = xhc.mmio + rtsoff;
    xhc.db = xhc.mmio + dboff;
    xhc.max_slots = hcs1 & 0xFF;
    xhc.max_ports = (hcs1 >> 24) & 0xFF;
    xhc.ctx_size  = (hcc1 & (1u << 2)) ? 64 : 32;

    /* Scratchpad buffer count = HCSPARAMS2 bits 27..21 (high) + bits 31..27 (low).
     * We bail if any are required — adds work for no benefit on qemu-xhci. */
    uint32_t spb = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);
    if (spb > 0) {
        kprintf("xhci: scratchpad buffers required (%u) — not supported\n", spb);
        return -1;
    }
    if (xhc.ctx_size != 32) {
        kprintf("xhci: 64-byte device context required by HC — not supported\n");
        return -1;
    }

    kprintf("xhci: cap_len=%u slots=%u ports=%u ctx=%u\n",
            caplen, xhc.max_slots, xhc.max_ports, xhc.ctx_size);

    /* Halt + reset. */
    mmio_w32(xhc.op, XHCI_OP_USBCMD, 0);            /* clear RUN */
    uint64_t deadline = timer_ticks_ms() + 100;
    while (!(mmio_r32(xhc.op, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH)) {
        if (timer_ticks_ms() > deadline) { kprintf("xhci: HC didn't halt\n"); return -1; }
    }
    mmio_w32(xhc.op, XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);
    deadline = timer_ticks_ms() + 100;
    while (mmio_r32(xhc.op, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) {
        if (timer_ticks_ms() > deadline) { kprintf("xhci: HCRST stuck\n"); return -1; }
    }
    deadline = timer_ticks_ms() + 100;
    while (mmio_r32(xhc.op, XHCI_OP_USBSTS) & XHCI_USBSTS_CNR) {
        if (timer_ticks_ms() > deadline) { kprintf("xhci: CNR stuck\n"); return -1; }
    }

    /* MaxSlotsEn. */
    mmio_w32(xhc.op, XHCI_OP_CONFIG, xhc.max_slots);

    /* DCBAA — single frame, zeroed. */
    uint32_t dcbaa_frame = pmm_alloc_frame();
    if (!dcbaa_frame) return -1;
    xhc.dcbaa = (uint64_t*)(uintptr_t)dcbaa_frame;
    for (int i = 0; i < 512; i++) xhc.dcbaa[i] = 0;
    mmio_w32(xhc.op, XHCI_OP_DCBAAP_LO, dcbaa_frame);
    mmio_w32(xhc.op, XHCI_OP_DCBAAP_HI, 0);

    /* Command Ring. */
    if (ring_alloc(&xhc.cmd_ring, 1) != 0) return -1;
    mmio_w32(xhc.op, XHCI_OP_CRCR_LO, xhc.cmd_ring.phys | 1 /* RCS=1 */);
    mmio_w32(xhc.op, XHCI_OP_CRCR_HI, 0);

    /* Event Ring + 1-entry ERST (lives in same frame as ERST trick:
     * dedicate a frame to the segment and a tiny ERST nearby).  We
     * just allocate a separate frame for the ERST. */
    if (ring_alloc(&xhc.evt_ring, 0) != 0) return -1;
    uint32_t erst_frame = pmm_alloc_frame();
    if (!erst_frame) return -1;
    uint32_t* erst = (uint32_t*)(uintptr_t)erst_frame;
    for (int i = 0; i < 1024; i++) erst[i] = 0;
    erst[0] = xhc.evt_ring.phys;          /* segment base low */
    erst[1] = 0;                          /* base high */
    erst[2] = xhc.evt_ring.size;          /* segment size in TRBs */
    erst[3] = 0;
    xhc.erst_phys = erst_frame;

    mmio_w32(xhc.rt, XHCI_RT_IR0_ERSTSZ, 1);
    /* ERDP must be set BEFORE ERSTBA per spec — clears Event Handler Busy. */
    mmio_w32(xhc.rt, XHCI_RT_IR0_ERDP_LO, xhc.evt_ring.phys | (1u << 3));
    mmio_w32(xhc.rt, XHCI_RT_IR0_ERDP_HI, 0);
    mmio_w32(xhc.rt, XHCI_RT_IR0_ERSTBA_LO, erst_frame);
    mmio_w32(xhc.rt, XHCI_RT_IR0_ERSTBA_HI, 0);

    /* Disable interrupts on the interrupter — we poll. */
    mmio_w32(xhc.rt, XHCI_RT_IR0_IMAN, 0);

    /* Run! */
    mmio_w32(xhc.op, XHCI_OP_USBCMD, XHCI_USBCMD_RS);
    deadline = timer_ticks_ms() + 100;
    while (mmio_r32(xhc.op, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) {
        if (timer_ticks_ms() > deadline) { kprintf("xhci: didn't start\n"); return -1; }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Port enumeration — walk every root port, pick the first one with
 * a device attached, reset it, enable a slot, address the device.
 * --------------------------------------------------------------------------- */

static int find_connected_port(uint32_t* speed_out) {
    for (uint32_t p = 0; p < xhc.max_ports; p++) {
        uint32_t portsc = mmio_r32(xhc.op, XHCI_OP_PORTSC_BASE + p * 0x10);
        if (!(portsc & XHCI_PORTSC_CCS)) continue;
        /* Reset to bring the link to Enabled. */
        uint32_t v = portsc & ~(XHCI_PORTSC_CSC | XHCI_PORTSC_PRC);
        v |= XHCI_PORTSC_PR | XHCI_PORTSC_PP;
        mmio_w32(xhc.op, XHCI_OP_PORTSC_BASE + p * 0x10, v);

        uint64_t deadline = timer_ticks_ms() + 200;
        while (timer_ticks_ms() < deadline) {
            uint32_t s = mmio_r32(xhc.op, XHCI_OP_PORTSC_BASE + p * 0x10);
            if (s & XHCI_PORTSC_PRC) {
                /* Clear PRC by writing 1 (RW1C semantics). */
                mmio_w32(xhc.op, XHCI_OP_PORTSC_BASE + p * 0x10, s);
                if (s & XHCI_PORTSC_PED) {
                    if (speed_out) *speed_out = XHCI_PORTSC_SPEED(s);
                    return (int)p;
                }
            }
        }
    }
    return -1;
}

/* MaxPacketSize0 lookup by port speed (xHCI 4.5.2, table 4-8 / USB spec). */
static uint32_t default_maxpkt0(uint32_t speed) {
    switch (speed) {
        case 1: return 64;      /* Full Speed — actually 8 sometimes; safe to start */
        case 2: return 8;       /* Low  Speed — keyboards live here */
        case 3: return 64;      /* High Speed */
        case 4: return 512;     /* Super Speed */
        default: return 8;
    }
}

/* Fill an Endpoint Context for an Interrupt IN endpoint.  Offsets per
 * xHCI 6.2.3. */
static void fill_ep_ctx(uint8_t* ep_ctx, uint32_t ep_type, uint32_t max_pkt,
                        uint32_t tr_phys) {
    uint32_t* w = (uint32_t*)ep_ctx;
    w[0] = 0;
    w[1] = (3u << 1)            /* CErr = 3 */
         | (ep_type << 3)        /* EP Type (4 = Interrupt IN, 7 if OUT diff bit) */
         | (max_pkt << 16);
    w[2] = (tr_phys | 1u);       /* DCS = 1, TR Dequeue Ptr low */
    w[3] = 0;
    w[4] = max_pkt;              /* Average TRB length = packet size */
    w[5] = 0;
    w[6] = 0;
    w[7] = 0;
}

static int enumerate_root_device(void) {
    uint32_t speed = 0;
    int port = find_connected_port(&speed);
    if (port < 0) { kprintf("xhci: no device on any port\n"); return -1; }
    kprintf("xhci: device on port %d speed=%u\n", port, speed);

    /* Enable Slot command (slot type 0). */
    uint32_t slot = 0;
    int cc = cmd_submit_wait(0, 0, 0,
                             (TRB_TYPE_ENABLE_SLOT << TRB_TYPE_SHIFT), &slot);
    if (cc != CC_SUCCESS || slot == 0) {
        kprintf("xhci: Enable Slot failed cc=%d\n", cc);
        return -1;
    }
    xhc.slot_id = slot;
    kprintf("xhci: slot %u assigned\n", slot);

    /* Allocate Device Context and Input Context (one frame each). */
    uint32_t dev_frame = pmm_alloc_frame();
    uint32_t ic_frame  = pmm_alloc_frame();
    uint32_t tr_frame_addr; /* later */
    if (!dev_frame || !ic_frame) {
        kprintf("xhci: OOM for contexts\n"); return -1;
    }
    xhc.dev_ctx = (uint8_t*)(uintptr_t)dev_frame;
    xhc.input_ctx = (uint8_t*)(uintptr_t)ic_frame;
    xhc.dev_ctx_phys = dev_frame;
    xhc.input_ctx_phys = ic_frame;
    /* Zero. */
    for (int i = 0; i < 1024; i++) ((uint32_t*)xhc.dev_ctx)[i] = 0;
    for (int i = 0; i < 1024; i++) ((uint32_t*)xhc.input_ctx)[i] = 0;
    xhc.dcbaa[slot] = (uint64_t)dev_frame;

    /* Allocate EP0 Transfer Ring. */
    if (ring_alloc(&xhc.ep0_ring, 1) != 0) return -1;
    tr_frame_addr = xhc.ep0_ring.phys;

    /* Input Context layout (32-byte): [0]=Input Control, [1]=Slot, [2]=EP0, ...
     * Input Control: A1 (slot) + A2 (EP0) → set bits 0 and 1 of "Add Context".
     */
    uint32_t* ic = (uint32_t*)xhc.input_ctx;
    ic[0] = 0;                         /* Drop Context flags */
    ic[1] = (1u << 0) | (1u << 1);     /* Add Slot + EP0 */

    /* Slot Context = ic + 32 bytes. */
    uint32_t* slot_ctx = ic + 8;
    /* dword0: Route String=0, Speed (bits 20-23), Context Entries=1 (bits 27-31) */
    slot_ctx[0] = (speed << 20) | (1u << 27);
    /* dword1: Root Hub Port Number (bits 16-23). */
    slot_ctx[1] = ((uint32_t)(port + 1)) << 16;
    slot_ctx[2] = 0;
    slot_ctx[3] = 0;

    /* EP0 Context = slot_ctx + 32 bytes. */
    uint8_t* ep0_ctx = (uint8_t*)(slot_ctx + 8);
    uint32_t max_pkt0 = default_maxpkt0(speed);
    fill_ep_ctx(ep0_ctx, 4 /* Control */, max_pkt0, tr_frame_addr);

    /* Address Device command — Input Context pointer in param.  Slot ID
     * in control[31..24]. */
    cc = cmd_submit_wait(ic_frame, 0, 0,
                         (TRB_TYPE_ADDRESS_DEVICE << TRB_TYPE_SHIFT)
                         | (slot << TRB_SLOT_SHIFT),
                         NULL);
    if (cc != CC_SUCCESS) {
        kprintf("xhci: Address Device failed cc=%d\n", cc);
        return -1;
    }
    xhc.device_addressed = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Get descriptors, find HID interface, configure endpoint.
 * --------------------------------------------------------------------------- */

static int enumerate_and_configure(void) {
    /* Allocate a DMA buffer for descriptors (one frame). */
    uint32_t buf_frame = pmm_alloc_frame();
    if (!buf_frame) return -1;
    uint8_t* buf = (uint8_t*)(uintptr_t)buf_frame;
    uint32_t buf_phys = buf_frame;

    /* Get the full Device Descriptor (18 bytes). */
    if (ep0_control_xfer(USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_GET_DESCRIPTOR,
                         (USB_DT_DEVICE << 8), 0, 18, buf, buf_phys) != 0) {
        kprintf("xhci: GET_DESCRIPTOR(device) failed\n");
        return -1;
    }
    struct usb_device_descriptor* dd = (struct usb_device_descriptor*)buf;
    kprintf("xhci: device vid=%x pid=%x class=%u\n",
            dd->idVendor, dd->idProduct, dd->bDeviceClass);

    /* Get the full Configuration Descriptor (up to one frame). */
    if (ep0_control_xfer(USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_GET_DESCRIPTOR,
                         (USB_DT_CONFIG << 8), 0, 256, buf, buf_phys) != 0) {
        kprintf("xhci: GET_DESCRIPTOR(config) failed\n");
        return -1;
    }
    struct usb_config_descriptor* cd = (struct usb_config_descriptor*)buf;
    uint32_t total = cd->wTotalLength;
    if (total > 4096) total = 4096;
    uint8_t config_value = cd->bConfigurationValue;

    /* Walk the descriptor blob for an HID/Boot/Kbd interface and its
     * Interrupt IN endpoint. */
    int hid_iface = -1;
    int hid_ep_num = -1;
    uint32_t hid_pkt = 8;
    int hid_interval = 8;
    uint32_t off = cd->bLength;
    while (off + 2 <= total) {
        uint8_t len = buf[off];
        uint8_t type = buf[off + 1];
        if (len == 0) break;
        if (type == USB_DT_INTERFACE && off + sizeof(struct usb_interface_descriptor) <= total) {
            struct usb_interface_descriptor* id = (struct usb_interface_descriptor*)(buf + off);
            if (id->bInterfaceClass == USB_CLASS_HID
             && id->bInterfaceSubClass == HID_SUBCLASS_BOOT
             && id->bInterfaceProtocol == HID_PROTO_KEYBOARD) {
                hid_iface = id->bInterfaceNumber;
            }
        } else if (type == USB_DT_ENDPOINT && hid_iface >= 0
                && off + sizeof(struct usb_endpoint_descriptor) <= total) {
            struct usb_endpoint_descriptor* ed = (struct usb_endpoint_descriptor*)(buf + off);
            if ((ed->bEndpointAddress & 0x80) /* IN */
             && (ed->bmAttributes & 0x3) == USB_EP_TYPE_INTERRUPT) {
                hid_ep_num = ed->bEndpointAddress & 0xF;
                hid_pkt    = ed->wMaxPacketSize & 0x7FF;
                hid_interval = ed->bInterval;
                break;
            }
        }
        off += len;
    }

    if (hid_iface < 0 || hid_ep_num < 0) {
        kprintf("xhci: no boot HID keyboard interface found\n");
        return -1;
    }
    kprintf("xhci: HID kbd iface=%d ep=%d pkt=%u interval=%d\n",
            hid_iface, hid_ep_num, hid_pkt, hid_interval);

    /* Set Configuration. */
    if (ep0_control_xfer(USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                         USB_REQ_SET_CONFIG, config_value, 0, 0, NULL, 0) != 0) {
        kprintf("xhci: SET_CONFIGURATION failed\n"); return -1;
    }

    /* Set Protocol = BOOT on the HID interface so we get the fixed
     * 8-byte report regardless of report descriptor. */
    if (ep0_control_xfer(USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                         HID_REQ_SET_PROTOCOL,
                         HID_PROTOCOL_BOOT, hid_iface, 0, NULL, 0) != 0) {
        kprintf("xhci: SET_PROTOCOL(BOOT) failed (may be OK)\n");
        /* qemu-xhci's usb-kbd accepts but some real HW stalls — ignore */
    }

    /* Allocate Transfer Ring for the interrupt-IN endpoint. */
    if (ring_alloc(&xhc.intr_in_ring, 1) != 0) return -1;

    /* Build a new Input Context for Configure Endpoint with EP `n` IN
     * (DCI = 2*n + 1). */
    uint32_t* ic = (uint32_t*)xhc.input_ctx;
    for (int i = 0; i < 1024; i++) ic[i] = 0;
    uint32_t dci = 2 * hid_ep_num + 1;
    ic[0] = 0;
    ic[1] = (1u << 0) | (1u << dci);    /* Add Slot + this EP */

    uint32_t* slot_ctx = ic + 8;
    /* Need at least dci entries — re-fill enough of the slot context. */
    /* Use the live device context as base (route, port, speed already set
     * by Address Device), then bump Context Entries to dci. */
    uint32_t* dev_slot = (uint32_t*)xhc.dev_ctx;
    slot_ctx[0] = (dev_slot[0] & 0x07FFFFFFu) | (dci << 27);
    slot_ctx[1] = dev_slot[1];
    slot_ctx[2] = 0;
    slot_ctx[3] = 0;

    /* EP context for the interrupt IN endpoint at offset dci * 32. */
    uint8_t* ep_ctx = xhc.input_ctx + (1 + dci) * 32;
    fill_ep_ctx(ep_ctx, 7 /* Interrupt IN */, hid_pkt, xhc.intr_in_ring.phys);

    int cc = cmd_submit_wait(xhc.input_ctx_phys, 0, 0,
                             (TRB_TYPE_CONFIG_EP << TRB_TYPE_SHIFT)
                             | (xhc.slot_id << TRB_SLOT_SHIFT),
                             NULL);
    if (cc != CC_SUCCESS) {
        kprintf("xhci: Configure Endpoint failed cc=%d\n", cc);
        return -1;
    }

    /* DMA buffer for the periodic report. */
    uint32_t rb_frame = pmm_alloc_frame();
    if (!rb_frame) return -1;
    xhc.intr_in_buf = (uint8_t*)(uintptr_t)rb_frame;
    xhc.intr_in_buf_phys = rb_frame;
    for (int i = 0; i < (int)hid_pkt; i++) xhc.intr_in_buf[i] = 0;
    xhc.hid_pkt_size = hid_pkt;
    xhc.hid_ep_num = hid_ep_num;
    xhc.hid_present = 1;

    /* Queue the first Normal TRB on the interrupt-IN ring and ring its
     * doorbell — the HC will DMA the next report into our buffer and
     * post a Transfer Event. */
    ring_enqueue(&xhc.intr_in_ring,
                 xhc.intr_in_buf_phys, 0,
                 hid_pkt,
                 (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC_BIT);
    uint32_t db_target = (hid_ep_num * 2) + 1;
    mmio_w32(xhc.db, XHCI_DB(xhc.slot_id), db_target);

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public poll function — called from the PIT IRQ tick (every quantum)
 * to drain the Event Ring so HID reports arrive without their own IRQ.
 * --------------------------------------------------------------------------- */

void xhci_poll(void) {
    if (!xhc.present) return;
    evt_drain(NULL);
}

/* ---------------------------------------------------------------------------
 * Driver registration (DRIVER scaffold from M8).
 * --------------------------------------------------------------------------- */

static struct pci_device xhci_pci;
static int xhci_pci_found = 0;

/* Visitor for pci_scan — first xHCI controller we see wins. */
static void xhci_pci_visit(const struct pci_device* d, void* ctx_) {
    (void)ctx_;
    if (xhci_pci_found) return;
    if (d->class_code == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) {
        xhci_pci = *d;
        xhci_pci_found = 1;
    }
}

static int xhci_probe(void* ctx) {
    (void)ctx;
    /* xHCI: class=0x0C (Serial Bus), subclass=0x03 (USB), prog_if=0x30. */
    xhci_pci_found = 0;
    pci_scan(xhci_pci_visit, NULL);
    return xhci_pci_found ? 0 : -1;
}

static int xhci_init(void* ctx) {
    (void)ctx;
    /* Enable bus mastering + memory space on the device. */
    uint16_t cmd = pci_read16(xhci_pci.bus, xhci_pci.slot, xhci_pci.func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(xhci_pci.bus, xhci_pci.slot, xhci_pci.func, PCI_COMMAND, cmd);

    /* BAR0 = MMIO base, 64-bit on xHCI (BAR0 low + BAR1 high).  We
     * mask the low bits per BAR rules. */
    uint32_t bar0 = xhci_pci.bar[0];
    if (bar0 & 0x1) { kprintf("xhci: BAR0 is I/O? %x\n", bar0); return -1; }
    uint32_t mmio_phys = bar0 & ~0xFu;

    /* Map a generous window (one 4 MiB PSE PDE) so all of cap/op/rt/db
     * regions land in a single contiguous virtual range. */
    uint32_t mmio_aligned = mmio_phys & 0xFFC00000u;
    if (vmm_map_4mib(mmio_aligned, mmio_aligned,
                     VMM_WRITABLE | VMM_CACHE_DIS) != 0 ) {
        /* If the region was already mapped earlier (PSE identity range
         * extends to 256 MiB) we silently accept that and continue. */
    }
    xhc.mmio = (volatile uint8_t*)(uintptr_t)mmio_phys;

    if (xhci_reset_and_init() != 0) {
        kprintf("xhci: init failed\n"); return -1;
    }
    if (enumerate_root_device() != 0) {
        kprintf("xhci: no device usable\n"); return 0;   /* still alive */
    }
    if (enumerate_and_configure() != 0) {
        kprintf("xhci: configure failed\n"); return 0;
    }
    xhc.present = 1;
    kprintf("xhci: ready, polling for HID reports\n");
    return 0;
}

static void xhci_shutdown(void* ctx) {
    (void)ctx;
    if (!xhc.present) return;
    mmio_w32(xhc.op, XHCI_OP_USBCMD, 0);  /* halt */
}

static const struct driver_ops xhci_ops = {
    .probe    = xhci_probe,
    .init     = xhci_init,
    .shutdown = xhci_shutdown,
};

DRIVER(xhci, "usb-host", &xhci_ops, NULL);
