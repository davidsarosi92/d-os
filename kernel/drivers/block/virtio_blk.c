/* =============================================================================
 * virtio_blk.c — legacy virtio-blk driver (PCI vendor 0x1AF4, device 0x1001).
 *
 * Polling, single-request-at-a-time, no IRQ wiring.  Enough for QEMU
 * smoke-tests and for filesystem bring-up (§M12).  Real-world
 * performance and concurrency are §M18 territory.
 *
 * --------------------------------------------------------------------------
 * virtio legacy (1.0 transitional) overview
 * --------------------------------------------------------------------------
 * The PCI device exposes a single I/O BAR.  Common config registers
 * (offsets from BAR0):
 *
 *   0x00 (4)  DEVICE_FEATURES  RO — feature bits the device supports
 *   0x04 (4)  DRIVER_FEATURES  WO — feature bits we accept
 *   0x08 (4)  QUEUE_PFN        RW — physical page number of selected queue
 *   0x0C (2)  QUEUE_SIZE       RO — number of descriptors in that queue
 *   0x0E (2)  QUEUE_SELECT     WO — which queue's config we want
 *   0x10 (2)  QUEUE_NOTIFY     WO — write queue index to kick the device
 *   0x12 (1)  DEVICE_STATUS    RW — handshake bits (ACK/DRIVER/...)
 *   0x13 (1)  ISR_STATUS       RC — interrupt cause (cleared on read)
 *   0x14 +    device-specific  RO — for blk: 64-bit `capacity` (sectors)
 *
 * --------------------------------------------------------------------------
 * Virtqueue layout (legacy, page-aligned base, ALIGN=PAGE)
 * --------------------------------------------------------------------------
 *   offset 0                          desc[QSIZE]    (16 * QSIZE bytes)
 *   right after desc                  avail header + ring + used_event
 *                                     (6 + 2*QSIZE bytes)
 *   aligned up to PAGE                used header + ring + avail_event
 *                                     (6 + 8*QSIZE bytes)
 *
 * With QSIZE=8, used lands on offset 4096; total = 4096 + 70 = 4166 →
 * round to 8192 bytes (2 contiguous physical frames).
 *
 * --------------------------------------------------------------------------
 * Block request format
 * --------------------------------------------------------------------------
 * Three descriptors per request, chained via the NEXT flag:
 *   desc[0]  header   16 B  device reads:  {type, reserved, sector}
 *   desc[1]  data     N*512 device reads (for write) or writes (for read)
 *   desc[2]  status   1 B   device writes 0=OK / 1=IOERR / 2=UNSUPP
 *
 * We pin the three descriptors at indices 0/1/2 — there's never more
 * than one outstanding request, so freeing/reusing them is trivial.
 *
 * Reference: virtio 1.0 spec §4.1 (PCI transport) and §5.2 (block).
 * ============================================================================= */

#include "block.h"
#include "pci.h"
#include "hal.h"
#include "hal_api.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "printf.h"
#include "driver.h"
#include "hwdev.h"
#include "devfs.h"
#include "idt.h"      /* irq_install — the completion interrupt */
#include "kmutex.h"
#include "ktimer.h"
#include "timer.h"
#include "task.h"
#include "lock.h"
#include <stdint.h>
#include <stddef.h>

/* ----------------------- Layout constants --------------------------------- */

#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_BLK_DEVICE    0x1001    /* legacy / transitional */

#define VBLK_OFF_DEV_FEAT    0x00
#define VBLK_OFF_DRV_FEAT    0x04
#define VBLK_OFF_QUEUE_PFN   0x08
#define VBLK_OFF_QUEUE_SIZE  0x0C
#define VBLK_OFF_QUEUE_SEL   0x0E
#define VBLK_OFF_QUEUE_NOTIFY 0x10
#define VBLK_OFF_DEV_STATUS  0x12
#define VBLK_OFF_ISR_STATUS  0x13
#define VBLK_OFF_CAPACITY    0x14      /* 64-bit, device-specific area */

/* Device status bits. */
#define VSTAT_ACKNOWLEDGE    0x01
#define VSTAT_DRIVER         0x02
#define VSTAT_DRIVER_OK      0x04
#define VSTAT_FEATURES_OK    0x08
#define VSTAT_DEVICE_NEEDS_RESET 0x40
#define VSTAT_FAILED         0x80

/* Descriptor flags. */
#define VRING_DESC_F_NEXT    0x01
#define VRING_DESC_F_WRITE   0x02      /* device writes to this buffer */
#define VRING_DESC_F_INDIRECT 0x04
#define VRING_AVAIL_F_NO_INTERRUPT 0x01   /* "do not interrupt me on completion" */

/* Block request types. */
#define VIRTIO_BLK_T_IN      0          /* read */
#define VIRTIO_BLK_T_OUT     1          /* write */
#define VIRTIO_BLK_T_FLUSH   4

#define VIRTIO_BLK_S_OK      0
#define VIRTIO_BLK_S_IOERR   1
#define VIRTIO_BLK_S_UNSUPP  2

#define SECTOR_SIZE          512

/* In legacy virtio the QUEUE_SIZE register is read-only; we must size
 * our descriptor/avail/used tables to match whatever the device
 * advertises.  QEMU's virtio-blk reports 256 entries — picking a
 * value smaller than that is NOT a valid optimization, because the
 * device computes offsets within the queue using its own qsize.
 *
 * Memory layout for QSIZE = 256:
 *   desc[256]              16 * 256 = 4096 bytes (page 0)
 *   avail (hdr + ring +    4 + 2*256 + 2 = 518 bytes (in page 1)
 *          used_event)
 *   pad to PAGE_SIZE
 *   used (hdr + ring +     4 + 8*256 + 2 = 2054 bytes (page 2)
 *         avail_event)
 *
 *   total ≤ 12 KiB → 3 contiguous physical frames. */
#define QSIZE                256
#define QUEUE_BYTES          (4096 * 3)

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

struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* ----------------------- Per-device state --------------------------------- */

struct virtio_blk {
    uint16_t io_base;                   /* PCI BAR0 I/O port base */
    uint64_t capacity_sectors;

    /* Queue memory.  `queue_phys` is the page-aligned physical address
     * we hand to QUEUE_PFN.  Since paging is identity-mapped for the
     * region the PMM allocates from, queue_phys == queue_virt.
     *
     * M20.6.3 audit (2026-06-30): stays uint32_t while the PMM is low-mem
     * only.  Legacy virtio writes the queue location as a PAGE FRAME
     * NUMBER (PFN = phys >> 12) via outl, so the 32-bit field gives 12
     * bits of headroom (up to ~16 TiB phys is addressable).  But the
     * descriptor->addr fields below are 64-bit in the spec — we still
     * pass uint32_t physical pointers there via vmm_translate, which
     * truncates to 32 bits.  Real >4 GiB DMA needs vmm_translate +
     * descriptor->addr widened to uint64_t. */
    uint32_t queue_phys;
    struct virtq_desc*  desc;
    struct virtq_avail* avail;
    struct virtq_used*  used;

    /* Last seen used->idx; bumped by every completed request. */
    uint16_t last_used_idx;

    /* Pinned header / status buffers — one outstanding request at a time. */
    struct virtio_blk_req_hdr* req_hdr;
    volatile uint8_t*          req_status;
};

/* Singleton — only one virtio-blk supported today.  When multi-device
 * support lands, this becomes a list and the block_device.priv points
 * at the per-device state. */
static struct virtio_blk g_vblk;
static struct block_device g_vda;
static int g_vblk_present = 0;

/* ----------------------- Small helpers ------------------------------------ */

static void vblk_write_status(uint16_t io, uint8_t v) {
    outb(io + VBLK_OFF_DEV_STATUS, v);
}
static uint8_t vblk_read_status(uint16_t io) {
    return inb(io + VBLK_OFF_DEV_STATUS);
}

/* Compiler barrier — keeps the compiler from reordering stores across
 * device-visible writes.  On a UP x86, this plus the serializing nature
 * of `outX` is enough memory ordering for the virtio handshake. */
static inline void barrier(void) {
    __asm__ volatile ("" : : : "memory");
}

/* ----------------------- Queue setup -------------------------------------- */

static int vblk_init_queue(struct virtio_blk* v) {
    uint16_t io = v->io_base;

    outw(io + VBLK_OFF_QUEUE_SEL, 0);
    uint16_t qsize = inw(io + VBLK_OFF_QUEUE_SIZE);
    if (qsize != QSIZE) {
        /* Legacy virtio: QUEUE_SIZE is read-only, so the device's view of
         * the queue layout is fixed.  If it doesn't match the QSIZE we
         * compiled with, the descriptor / avail / used offsets disagree
         * and the request never completes.  Bail loudly. */
        kprintf("virtio-blk: device reports qsize=%u but we built with %u\n",
                qsize, QSIZE);
        return -1;
    }

    /* Three contiguous frames: desc fills page 0, avail spills into page
     * 1, used starts at page 2 (page-aligned per legacy virtio rule). */
    pmm_phys_t phys = pmm_alloc_contiguous_dma32(3);
    if (!phys) {
        kprintf("virtio-blk: no contiguous frames for queue\n");
        return -2;
    }

    /* Zero the queue area.  Identity-mapped virt == phys for this range. */
    uint8_t* q = (uint8_t*)phys_to_virt(phys);
    for (uint32_t i = 0; i < QUEUE_BYTES; i++) q[i] = 0;

    v->queue_phys = phys;
    v->desc  = (struct virtq_desc*) q;
    v->avail = (struct virtq_avail*)(q + sizeof(struct virtq_desc) * QSIZE);
    v->used  = (struct virtq_used*) (q + 4096 * 2); /* page 2 — matches qsize=256 */
    v->last_used_idx = 0;

    /* WE POLL, SO ASK THE DEVICE NOT TO INTERRUPT.  This driver waits for a
     * completion by watching the used ring and never installed an interrupt
     * handler — but it never told the device that, so every completed request
     * RAISED the PCI line anyway.  On QEMU's PIIX that line is IRQ 10, shared
     * (PIRQA/PIRQB), LEVEL-triggered, and nobody read this device's ISR
     * register, which is what lowers it.  The line stayed asserted, and
     * whichever driver did own vector 42 took an interrupt storm: EOI, re-fire,
     * forever.  Vector 42 outranks the PIT (32) and the keyboard (33), so both
     * starved — the millisecond clock stopped (every task_msleep and cron job
     * with it) and PS/2 bytes overflowed QEMU's queue, which is what "the
     * harness loses keystrokes after `gui stop`" actually was.  The LAPIC timer
     * (vector 64) outranks it, so the BSP kept petting the hardware watchdog
     * and nothing ever said NMI.  Measured with QEMU's `info lapic`: ISR=42,
     * IRR=32, PPR=0x20; `info pic`: pin 10 Remote IRR set.  (§M82 session.)
     *
     * This flag is advisory in the spec — hence the ISR read after every
     * completion below as well. */
    /* 2026-09-25: interrupts are WANTED now — the driver sleeps on them (see
     * vblk_wait) and ACKNOWLEDGES each one by reading the ISR register in
     * vblk_irq, which is the half that was missing above.  Until the first
     * interrupt arrives the flag stays set and the wait polls, so a machine
     * whose line never fires loses latency, not the disk. */
    v->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    /* Tell the device where the queue is.  QUEUE_PFN is the page-frame
     * number (phys >> 12). */
    outl(io + VBLK_OFF_QUEUE_PFN, phys >> 12);

    return 0;
}

/* ----------------------- Pinned request buffers --------------------------- */

static int vblk_init_buffers(struct virtio_blk* v) {
    /* Allocate the header + status from a PMM frame so virt == phys
     * (we're inside the 256 MiB identity map).  Heap-backed kmalloc
     * is NOT safe here because the device DMAs to descriptor addresses
     * as physical addresses, and heap pages live at virtual
     * 0xD0000000+ which doesn't match their physical backing. */
    pmm_phys_t f = pmm_alloc_frame_dma32();
    if (!f) return -1;
    v->req_hdr    = (struct virtio_blk_req_hdr*)phys_to_virt(f);
    v->req_status = (volatile uint8_t*)((uintptr_t)phys_to_virt(f) + sizeof(*v->req_hdr));
    return 0;
}

/* ----------------------- Sync request issue ------------------------------- */

/* ----------------------- Completion: interrupt, then sleep ---------------- *
 *
 * ONE REQUEST AT A TIME, BY A LOCK.  The driver owns a single descriptor chain
 * and a single header/status pair; two tasks inside vblk_request would write
 * each other's descriptors.  Nothing prevented that — it survived because the
 * busy-wait made the window short, not closed, and `diskstorm` on 4 CPUs is
 * where short stopped being enough.
 *
 * THE INTERRUPT is shared (PIIX routes PIRQA/B to one level line) and chained
 * (idt.c), so the handler reads THIS device's ISR register — which is also what
 * lowers the line — and returns quietly when the bit is not ours.  It does two
 * things and no third: acknowledge and wake (§M55's rule for ISRs).  The wait
 * learns that interrupts work by RECEIVING one; until then, and whenever it
 * may not sleep (boot, preemption off), it polls, and it never sleeps without a
 * 2 ms backstop — a lost interrupt costs latency, never the request. */
#define VBLK_TIMEOUT_MS  5000u
static struct kmutex     vblk_lock = KMUTEX_INIT("virtio-blk");
static struct waitq      vblk_wq   = WAITQ_INIT;
static volatile uint32_t vblk_irqs;

static void vblk_irq(struct int_frame* f);
static int vblk_may_sleep(void) {
    struct task* t = task_current();
    return t && !t->is_idle && preempt_count() == 0;
}
static void vblk_backstop(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t fl = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, fl);
}
static int vblk_wait(struct virtio_blk* v) {
    uint64_t deadline = timer_ticks_ms() + VBLK_TIMEOUT_MS;
    while (*(volatile uint16_t*)&v->used->idx == v->last_used_idx) {
        if (timer_ticks_ms() > deadline) return -1;
        if (!vblk_irqs || !vblk_may_sleep()) { hal_cpu_pause(); continue; }
        {
            static int told;
            if (!told) {
                told = 1;
                kprintf("virtio-blk: completion interrupts work - requests now "
                        "sleep instead of polling (%u so far)\n", vblk_irqs);
            }
        }
        struct ktimer t = { 0, 0, 0, 0, 0 };
        ktimer_arm_after(&t, 2000000ull, vblk_backstop, &vblk_wq);
        uint32_t fl = waitq_lock(&vblk_wq);
        if (*(volatile uint16_t*)&v->used->idx == v->last_used_idx)
            waitq_block(&vblk_wq);
        waitq_unlock(&vblk_wq, fl);
        ktimer_cancel(&t);
    }
    return 0;
}

static int vblk_request_unlocked(struct virtio_blk* v, uint32_t type, uint64_t lba,
                                 void* buf, uint32_t nsectors);
static int vblk_request(struct virtio_blk* v, uint32_t type, uint64_t lba,
                        void* buf, uint32_t nsectors) {
    kmutex_lock(&vblk_lock);
    int rc = vblk_request_unlocked(v, type, lba, buf, nsectors);
    kmutex_unlock(&vblk_lock);
    return rc;
}

static void vblk_irq(struct int_frame* f) {
    (void)f;
    if (!g_vblk.io_base) return;
    uint8_t isr = inb(g_vblk.io_base + VBLK_OFF_ISR_STATUS);   /* read-to-clear */
    if (!(isr & 1)) return;                                    /* not ours      */
    vblk_irqs++;           /* the first one proves the line: the wait may sleep */
    uint32_t fl = waitq_lock(&vblk_wq);
    waitq_wake_all(&vblk_wq);
    waitq_unlock(&vblk_wq, fl);
}

static int vblk_request_unlocked(struct virtio_blk* v, uint32_t type, uint64_t lba,
                        void* buf, uint32_t nsectors) {
    /* 1. Fill header. */
    v->req_hdr->type     = type;
    v->req_hdr->reserved = 0;
    v->req_hdr->sector   = lba;
    *v->req_status = 0xFF;                          /* sentinel */

    /* 2. Wire three descriptors.  For reads the data buffer is
     *    device-writable; for writes it isn't. */
    int data_flags = VRING_DESC_F_NEXT
                   | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);

    /* Descriptor addresses are physical (the device's view of memory).
     * Header + status are PMM-allocated so virt == phys, but the caller's
     * `buf` may be heap-backed (virt 0xD0000000+) — translate that one
     * through the page tables.  Single-page buffers only for now (a
     * larger buffer would need to be split into per-page descriptors). */
    v->desc[0].addr  = kptr_phys(v->req_hdr);
    v->desc[0].len   = sizeof(struct virtio_blk_req_hdr);
    v->desc[0].flags = VRING_DESC_F_NEXT;
    v->desc[0].next  = 1;

    /* Physical, and at FULL width: the descriptor addr field is 64-bit in the
     * spec, and since §M48 the block cache's pages can genuinely live above
     * 4 GiB.  Narrowing here would aim the device's DMA at a truncated
     * address — a write to memory that belongs to something else. */
    v->desc[1].addr  = kptr_phys(buf);
    v->desc[1].len   = nsectors * SECTOR_SIZE;
    v->desc[1].flags = data_flags;
    v->desc[1].next  = 2;

    v->desc[2].addr  = kptr_phys((const void*)v->req_status);
    v->desc[2].len   = 1;
    v->desc[2].flags = VRING_DESC_F_WRITE;          /* device writes status */
    v->desc[2].next  = 0;

    /* 3. Place head of chain into avail ring; bump idx. */
    uint16_t pos = v->avail->idx % QSIZE;
    v->avail->ring[pos] = 0;                        /* desc head index */
    barrier();
    v->avail->idx++;
    barrier();

    /* 4. Kick the device. */
    outw(v->io_base + VBLK_OFF_QUEUE_NOTIFY, 0);

    /* 5. Wait for the used ring's idx to change — the device completed our
     *    request.  SLEEPING, not spinning (2026-09-25): this was a
     *    `hal_cpu_pause` loop with a timeout of 50 million iterations, so every
     *    disk request burned a CPU for its whole duration and the deadline was
     *    a different number of seconds on every machine.  Bounded in TIME now. */
    if (vblk_wait(v) != 0) {
        kprintf("vblk: timeout after %u ms (isr=%x dev_status=%x)\n", VBLK_TIMEOUT_MS,
                inb(v->io_base + VBLK_OFF_ISR_STATUS),
                inb(v->io_base + VBLK_OFF_DEV_STATUS));
        return -1;
    }
    v->last_used_idx = v->used->idx;

    /* Read-to-clear the ISR register, so that even a device which ignores
     * NO_INTERRUPT drops the shared line instead of leaving it asserted for
     * whichever driver owns the vector (see the note at the ring setup). */
    (void)inb(v->io_base + VBLK_OFF_ISR_STATUS);

    /* 6. Check status. */
    uint8_t st = *v->req_status;
    if (st != VIRTIO_BLK_S_OK) {
        kprintf("virtio-blk: request status %u\n", st);
        return -1;
    }
    return 0;
}

/* ----------------------- block_device callbacks --------------------------- */

static int vblk_read_op(struct block_device* dev, uint64_t lba,
                        uint32_t count, void* buf) {
    struct virtio_blk* v = (struct virtio_blk*)dev->priv;
    return vblk_request(v, VIRTIO_BLK_T_IN, lba, buf, count);
}

static int vblk_write_op(struct block_device* dev, uint64_t lba,
                         uint32_t count, const void* buf) {
    struct virtio_blk* v = (struct virtio_blk*)dev->priv;
    /* The virtio descriptor table takes a void*; the device only reads. */
    return vblk_request(v, VIRTIO_BLK_T_OUT, lba, (void*)buf, count);
}

/* ----------------------- devfs adapter ------------------------------------ *
 * REMOVED.  The "treat the disk as one big file" view needs nothing this
 * driver knows — sector size and the read/write ops all live in `struct
 * block_device` — so it moved into blk_register(), which every block driver on
 * every architecture already calls.  It was here, and only here, which is why
 * /dev/vda existed on x86 and not on aarch64.
 * -------------------------------------------------------------------------- */

/* ----------------------- DRIVER() lifecycle ------------------------------- */

static int vblk_probe(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE, &pd) != 0) {
        return -1;                                  /* device absent */
    }
    return 0;
}

static int vblk_init(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_BLK_DEVICE, &pd) != 0) return -1;

    uint16_t io = pci_bar_io_base(pd.bar[0]);
    if (!io) {
        kprintf("virtio-blk: BAR0 is not I/O-space\n");
        return -2;
    }

    /* Enable I/O space + bus master in the PCI command register so the
     * device can read/write our descriptor memory. */
    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND, cmd);

    g_vblk.io_base = io;

    /* Reset + handshake. */
    vblk_write_status(io, 0);
    vblk_write_status(io, VSTAT_ACKNOWLEDGE);
    vblk_write_status(io, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    /* Accept zero features — we use only the baseline read/write/flush. */
    outl(io + VBLK_OFF_DRV_FEAT, 0);
    vblk_write_status(io, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    if ((vblk_read_status(io) & VSTAT_FEATURES_OK) == 0) {
        kprintf("virtio-blk: FEATURES_OK rejected\n");
        return -3;
    }

    /* Read capacity (in 512-byte sectors) from device-specific config. */
    uint32_t cap_lo = inl(io + VBLK_OFF_CAPACITY);
    uint32_t cap_hi = inl(io + VBLK_OFF_CAPACITY + 4);
    g_vblk.capacity_sectors = ((uint64_t)cap_hi << 32) | cap_lo;

    if (vblk_init_queue(&g_vblk) != 0)    return -4;
    if (vblk_init_buffers(&g_vblk) != 0) return -5;

    /* Driver is ready. */
    vblk_write_status(io, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER
                        | VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);

    /* The completion interrupt — only on a real, routed line: a hot-added
     * device may have none, and line 0 is the TIMER (§M66's lesson).  With it
     * installed, ask the device to interrupt; the wait still polls until the
     * first one proves the line (vblk_irq), so a line that never fires costs
     * latency and nothing else.  Without it, completions stay polled. */
    if (pd.irq_line != 0xFF && pd.irq_line != 0) {
        irq_install(pd.irq_line, vblk_irq);
        g_vblk.avail->flags = 0;
    }

    /* Register the abstract block device. */
    g_vda.name         = "vda";
    g_vda.sector_size  = SECTOR_SIZE;
    g_vda.sector_count = g_vblk.capacity_sectors;
    g_vda.read         = vblk_read_op;
    g_vda.write        = vblk_write_op;
    g_vda.flush        = NULL;
    g_vda.priv         = &g_vblk;
    g_vda.next         = NULL;
    blk_register(&g_vda);

    /* /dev/vda is published by blk_register() now — by the layer every block
     * driver shares, so aarch64's driver gets it too instead of this being an
     * x86-only convenience.  Registering here as well would be a duplicate
     * name in devfs. */

    g_vblk_present = 1;
    kprintf("virtio-blk: %u sectors at PCI %u:%u.%u io=%x irq=%u\n",
            (unsigned)g_vblk.capacity_sectors,
            pd.bus, pd.slot, pd.func, io, pd.irq_line);
    return 0;
}

static const struct driver_ops vblk_ops = {
    .probe    = vblk_probe,
    .init     = vblk_init,
    .shutdown = NULL,
};

/* BOOT-CRITICAL: the root volume is mounted through this driver before there
 * is any process substrate to move it into, and DMA-capable: the device writes
 * into memory by itself, so ring 3 without an IOMMU would be a placement, not
 * an isolation. */
/* What this driver drives, declared where the driver is (hwdev.h).  Without
 * it the device manager can see the hardware and cannot say what should be
 * driving it. */
DRIVER_MATCH(m_vblk) = { .driver = "virtio_blk",
                         .vendor = VIRTIO_VENDOR, .device = VIRTIO_BLK_DEVICE };

DRIVER_EX(virtio_blk, "block", &vblk_ops, NULL,
          DOMAIN_KERNEL, DRVF_BOOT_CRITICAL | DRVF_DMA);
