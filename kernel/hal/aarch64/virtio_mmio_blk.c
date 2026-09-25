/* =============================================================================
 * virtio_mmio_blk.c — virtio-blk over the virtio-MMIO transport (M21 Phase F).
 *
 * The existing kernel/drivers/block/virtio_blk.c speaks virtio over *PCI* (BAR
 * I/O ports) — meaningless on ARM, which has no port I/O and exposes virtio
 * devices as plain MMIO register blocks instead.  QEMU's `virt` board wires 32
 * virtio-MMIO transport slots at 0x0a00_0000 (stride 0x200); a
 * `-device virtio-blk-device` lands in the first free one.  This is a fresh,
 * self-contained driver for that transport — the ARM proof of "every device is
 * MMIO" — that registers the disk with the arch-independent block layer as
 * `/dev/vda`, so the rest of the kernel (and the serial shell) sees a normal
 * block device.
 *
 * Transport: virtio-MMIO **version 2** (modern), the QEMU `virt` default.  The
 * virtqueue mechanics (split ring: descriptor table + avail + used, 3
 * descriptors per block request) are identical to the PCI driver — only the
 * register access (MMIO loads/stores vs. port I/O) and the queue-address
 * programming (Desc/Driver/Device low/high vs. a single PFN) differ.
 *
 * Completion is POLLED (spin on used->idx) — no IRQ wiring — which keeps the
 * driver simple and is fine because QEMU services the queue synchronously.
 * DMA on QEMU is coherent with the CPU caches, so no cache maintenance is
 * needed around the shared rings (a real non-coherent SoC would need it).
 * ============================================================================= */

#include "block.h"
#include "printf.h"
#include "kmutex.h"
#include "task.h"
#include "timer.h"
#include "lock.h"
#include "waitq.h"
#include "ktimer.h"
#include <stdint.h>
#include <stddef.h>

/* ---- MMIO transport map (QEMU `virt`) -------------------------------------- */
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200
#define VIRTIO_MMIO_SLOTS   32

#define R_MAGIC        0x000   /* 'virt' = 0x74726976                         */
#define R_VERSION      0x004   /* 2 = modern                                  */
#define R_DEVICEID     0x008   /* 2 = block                                   */
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
#define VIRTIO_F_VERSION_1_BIT 0        /* feature bit 32 → sel=1, bit 0      */

/* ---- virtqueue on-the-wire structs (same layout as the PCI driver) --------- */
#define QSIZE   8
#define SECTOR  512

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QSIZE]; uint16_t used_event; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZE]; uint16_t avail_event; } __attribute__((packed));
struct virtio_blk_req_hdr { uint32_t type; uint32_t reserved; uint64_t sector; } __attribute__((packed));

#define VRING_DESC_F_NEXT   0x01
#define VRING_DESC_F_WRITE  0x02        /* device writes into this buffer     */
#define VIRTIO_BLK_T_IN     0           /* read                               */
#define VIRTIO_BLK_T_OUT    1           /* write                              */

/* Queue memory — Normal RAM, identity-mapped so virt == phys for the device's
 * DMA.  16-byte aligned (desc needs it; avail/used are fine at 16 too). */
static struct virtq_desc  q_desc[QSIZE]        __attribute__((aligned(16)));
static struct virtq_avail q_avail              __attribute__((aligned(16)));
static struct virtq_used  q_used               __attribute__((aligned(16)));
static struct virtio_blk_req_hdr q_hdr         __attribute__((aligned(16)));
static volatile uint8_t   q_status             __attribute__((aligned(16)));

static uintptr_t g_base;
static int       g_slot = -1;   /* transport slot, for its SPI */
static uint16_t  g_last_used;
static struct block_device g_vda;

/* ---- MMIO + barrier helpers ------------------------------------------------ */
static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_base + off) = v; }
static inline uint32_t r32(uint32_t off)             { return *(volatile uint32_t*)(g_base + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* ---- one synchronous block request ----------------------------------------- *
 *
 * 2026-09-25, the same two defects as the x86 driver: ONE descriptor chain and
 * header with NO lock (two tasks on two cores wrote each other's requests), and
 * a completion wait with NO bound at all.  Now serialised by a kmutex, bounded
 * by 5 s of real time, and yielding the CPU while it waits where it may.
 *
 * THE COMPLETION INTERRUPT (2026-09-25, NEXT.md #2's leftover).  The x86
 * driver sleeps on its interrupt; this one polled with task_yield, which on an
 * otherwise idle CPU is a busy loop by another name — the waiting task is the
 * only runnable one, so yield returns at once and the core never halts.  Same
 * shape as virtio-blk on x86 and virtio-snd here: the SPI for this transport
 * slot (INTID 48 + slot on QEMU `virt`) acknowledges InterruptStatus (which is
 * what lowers the level line) and wakes the waiter — two things and no third.
 * The wait learns interrupts work by RECEIVING one; until then, and whenever it
 * may not sleep, it polls as before, and it never sleeps without a 2 ms
 * backstop, so a lost interrupt costs latency, never the request. */
#define VIRTIO_MMIO_INTID_BASE 48
void gic_register_handler(uint32_t intid, void (*fn)(uint32_t));
void gic_enable_irq(uint32_t intid);

static struct kmutex     vmb_lock = KMUTEX_INIT("virtio-mmio-blk");
static struct waitq      vmb_wq   = WAITQ_INIT;
static volatile uint32_t vmb_irqs;

static void vmb_irq(uint32_t intid) {
    (void)intid;
    if (!g_base) return;
    uint32_t st = r32(R_INTSTATUS);
    if (!st) return;                                  /* not ours */
    w32(R_INTACK, st);
    vmb_irqs++;            /* the first one proves the line: the wait may sleep */
    uint32_t fl = waitq_lock(&vmb_wq);
    waitq_wake_all(&vmb_wq);
    waitq_unlock(&vmb_wq, fl);
}

static void vmb_backstop(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t fl = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, fl);
}

static int vmb_done(void) {
    dsb();
    return *(volatile uint16_t*)&q_used.idx != g_last_used;
}

static int vmb_rw_unlocked(uint64_t lba, uint32_t count, void* buf, int is_write);
static int vmb_rw(uint64_t lba, uint32_t count, void* buf, int is_write) {
    kmutex_lock(&vmb_lock);
    int rc = vmb_rw_unlocked(lba, count, buf, is_write);
    kmutex_unlock(&vmb_lock);
    return rc;
}

static int vmb_rw_unlocked(uint64_t lba, uint32_t count, void* buf, int is_write) {
    q_hdr.type     = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    q_hdr.reserved = 0;
    q_hdr.sector   = lba;

    q_desc[0].addr = (uint64_t)(uintptr_t)&q_hdr;
    q_desc[0].len  = sizeof q_hdr;
    q_desc[0].flags = VRING_DESC_F_NEXT;
    q_desc[0].next = 1;

    q_desc[1].addr = (uint64_t)(uintptr_t)buf;
    q_desc[1].len  = count * SECTOR;
    q_desc[1].flags = VRING_DESC_F_NEXT | (is_write ? 0 : VRING_DESC_F_WRITE);
    q_desc[1].next = 2;

    q_desc[2].addr = (uint64_t)(uintptr_t)&q_status;
    q_desc[2].len  = 1;
    q_desc[2].flags = VRING_DESC_F_WRITE;
    q_desc[2].next = 0;

    q_status = 0xff;

    uint16_t ai = q_avail.idx;
    q_avail.ring[ai % QSIZE] = 0;          /* head descriptor index          */
    dsb();
    q_avail.idx = ai + 1;
    dsb();

    w32(R_QUEUENOTIFY, 0);                  /* kick queue 0                   */

    uint64_t deadline = timer_ticks_ms() + 5000;
    while (!vmb_done()) {
        if (timer_ticks_ms() > deadline) {
            kprintf("virtio-mmio-blk: request timed out after 5000 ms\n");
            return -1;
        }
        struct task* me = task_current();
        int may_sleep = me && !me->is_idle && preempt_count() == 0;
        if (!vmb_irqs || !may_sleep) {
            if (may_sleep) task_yield();
            continue;
        }
        {
            static int told;
            if (!told) {
                told = 1;
                kprintf("virtio-mmio-blk: completion interrupts work - requests "
                        "now sleep instead of polling (%u so far)\n", vmb_irqs);
            }
        }
        struct ktimer t = { 0, 0, 0, 0, 0 };
        ktimer_arm_after(&t, 2000000ull, vmb_backstop, &vmb_wq);
        uint32_t fl = waitq_lock(&vmb_wq);
        if (!vmb_done()) waitq_block(&vmb_wq);
        waitq_unlock(&vmb_wq, fl);
        ktimer_cancel(&t);
    }
    g_last_used++;
    dsb();

    if (r32(R_INTSTATUS) & 1) w32(R_INTACK, 1);
    return (q_status == 0) ? 0 : -1;
}

static int vmb_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buf) {
    (void)dev;
    for (uint32_t i = 0; i < count; i++)
        if (vmb_rw(lba + i, 1, (uint8_t*)buf + i * SECTOR, 0) != 0) return -1;
    return 0;
}
static int vmb_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buf) {
    (void)dev;
    for (uint32_t i = 0; i < count; i++)
        if (vmb_rw(lba + i, 1, (uint8_t*)(uintptr_t)buf + i * SECTOR, 1) != 0) return -1;
    return 0;
}

/* ---- probe + init ---------------------------------------------------------- */

/* Scan the 32 MMIO slots for a virtio-block (deviceID 2) modern transport,
 * negotiate features, set up queue 0, and register it as /dev/vda.  Called
 * once from the aarch64 bring-up; a no-op (returns -1) if no disk is attached. */
int virtio_mmio_blk_init(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        uint32_t magic = *(volatile uint32_t*)(base + R_MAGIC);
        if (magic != VIRTIO_MAGIC) continue;
        uint32_t ver = *(volatile uint32_t*)(base + R_VERSION);
        uint32_t dev = *(volatile uint32_t*)(base + R_DEVICEID);
        if (dev == 0) continue;                  /* empty transport slot       */
        kprintf("virtio-mmio: slot %d dev=%u ver=%u\n", i, dev, ver);
        if (dev != 2 || ver != 2) continue;      /* want a modern block device */
        g_base = base;
        g_slot = i;
        break;
    }
    if (!g_base) return -1;                      /* no virtio-blk attached     */

    /* Reset, then ACKNOWLEDGE + DRIVER. */
    w32(R_STATUS, 0);
    w32(R_STATUS, ST_ACK);
    w32(R_STATUS, ST_ACK | ST_DRIVER);

    /* Feature negotiation: accept only VIRTIO_F_VERSION_1 (feature bit 32). */
    w32(R_DEVFEATSEL, 1); (void)r32(R_DEVFEAT);
    w32(R_DRVFEATSEL, 1); w32(R_DRVFEAT, 1u << VIRTIO_F_VERSION_1_BIT);
    w32(R_DEVFEATSEL, 0); (void)r32(R_DEVFEAT);
    w32(R_DRVFEATSEL, 0); w32(R_DRVFEAT, 0);

    w32(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(r32(R_STATUS) & ST_FEATURES_OK)) {
        kprintf("virtio-mmio: device rejected features\n");
        return -1;
    }

    /* Set up virtqueue 0. */
    w32(R_QUEUESEL, 0);
    if (r32(R_QUEUEREADY) != 0) { kprintf("virtio-mmio: queue busy\n"); return -1; }
    uint32_t qmax = r32(R_QUEUENUMMAX);
    if (qmax < QSIZE) { kprintf("virtio-mmio: QueueNumMax %u < %u\n", qmax, QSIZE); return -1; }
    w32(R_QUEUENUM, QSIZE);

    uint64_t d = (uint64_t)(uintptr_t)q_desc;
    uint64_t a = (uint64_t)(uintptr_t)&q_avail;
    uint64_t u = (uint64_t)(uintptr_t)&q_used;
    w32(R_QDESC_LO, (uint32_t)d);  w32(R_QDESC_HI, (uint32_t)(d >> 32));
    w32(R_QDRV_LO,  (uint32_t)a);  w32(R_QDRV_HI,  (uint32_t)(a >> 32));
    w32(R_QDEV_LO,  (uint32_t)u);  w32(R_QDEV_HI,  (uint32_t)(u >> 32));
    w32(R_QUEUEREADY, 1);

    w32(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    /* The completion interrupt: handler first, then unmask (the install-then-
     * unmask split gic.c mirrors from x86). */
    gic_register_handler(VIRTIO_MMIO_INTID_BASE + (uint32_t)g_slot, vmb_irq);
    gic_enable_irq(VIRTIO_MMIO_INTID_BASE + (uint32_t)g_slot);

    /* Capacity (sectors) is the first u64 of the block config space. */
    uint64_t cap = (uint64_t)r32(R_CONFIG) | ((uint64_t)r32(R_CONFIG + 4) << 32);

    g_vda.name         = "vda";
    g_vda.sector_size  = SECTOR;
    g_vda.sector_count = cap;
    g_vda.read         = vmb_read;
    g_vda.write        = vmb_write;
    g_vda.flush        = NULL;
    g_vda.priv         = NULL;
    blk_register(&g_vda);

    kprintf("virtio-mmio: /dev/vda ready (%u sectors, %u MiB) at slot base %p\n",
            (unsigned)cap, (unsigned)(cap / 2048), (void*)g_base);
    return 0;
}
