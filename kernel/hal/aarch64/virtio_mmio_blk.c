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
#include "hal_api.h"
#include "board.h"   /* §M85 — virtio slots from the device tree */   /* kptr_phys — device addresses are PHYSICAL (§M86) */
#include <stdint.h>
#include <stddef.h>

/* ---- MMIO transport map (QEMU `virt`) -------------------------------------- */

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

/* §M87 open item (2026-10-01) — ONE OF SEVERAL DISKS.  Everything that was a
 * file-scope singleton (the ring, the request header, the lock, the wait queue,
 * the interrupt count, the block device) is per device: two disks are two
 * rings, and one lock across both would serialise disks that have nothing to
 * do with each other.  Ring memory is Normal RAM in the image; the device is
 * handed its PHYSICAL address (kptr_phys). */
#define VMB_MAX 4
struct vmb {
    struct virtq_desc  q_desc[QSIZE]        __attribute__((aligned(16)));
    struct virtq_avail q_avail              __attribute__((aligned(16)));
    struct virtq_used  q_used               __attribute__((aligned(16)));
    struct virtio_blk_req_hdr q_hdr         __attribute__((aligned(16)));
    volatile uint8_t   q_status             __attribute__((aligned(16)));
    uintptr_t          base;
    int                slot;               /* transport slot, for its SPI */
    uint16_t           last_used;
    struct kmutex      lock;
    struct waitq       wq;
    volatile uint32_t  irqs;
    char               name[8];
    struct block_device bd;
};
static struct vmb g_vmb[VMB_MAX];
static int g_nvmb;

/* ---- MMIO + barrier helpers ------------------------------------------------ */
static inline void     w32(struct vmb* v, uint32_t off, uint32_t x) { *(volatile uint32_t*)(v->base + off) = x; }
static inline uint32_t r32(struct vmb* v, uint32_t off)             { return *(volatile uint32_t*)(v->base + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* ---- one synchronous block request ----------------------------------------- *
 *
 * 2026-09-25, the same two defects as the x86 driver: ONE descriptor chain and
 * header with NO lock (two tasks on two cores wrote each other's requests), and
 * a completion wait with NO bound at all.  Now serialised by a kmutex, bounded
 * by 5 s of real time, and yielding the CPU while it waits where it may.
 *
 * THE COMPLETION INTERRUPT (2026-09-25, NEXT.md #2's leftover): the SPI for
 * this transport slot (INTID 48 + slot on QEMU `virt`) acknowledges
 * InterruptStatus (which is what lowers the level line) and wakes the waiter —
 * two things and no third.  The wait learns interrupts work by RECEIVING one;
 * until then, and whenever it may not sleep, it polls, and it never sleeps
 * without a 2 ms backstop, so a lost interrupt costs latency, never the
 * request. */
void gic_register_handler(uint32_t intid, void (*fn)(uint32_t));
void gic_enable_irq(uint32_t intid);

/* Each disk has its own SPI, so the handler finds its device by INTID. */
static void vmb_irq(uint32_t intid) {
    for (int i = 0; i < g_nvmb; i++) {
        struct vmb* v = &g_vmb[i];
        if (!v->base || board_virtio_intid(v->slot) != intid) continue;
        uint32_t st = r32(v, R_INTSTATUS);
        if (!st) return;                              /* not ours */
        w32(v, R_INTACK, st);
        v->irqs++;         /* the first one proves the line: the wait may sleep */
        uint32_t fl = waitq_lock(&v->wq);
        waitq_wake_all(&v->wq);
        waitq_unlock(&v->wq, fl);
        return;
    }
}

static void vmb_backstop(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t fl = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, fl);
}

static int vmb_done(struct vmb* v) {
    dsb();
    return *(volatile uint16_t*)&v->q_used.idx != v->last_used;
}

static int vmb_rw_unlocked(struct vmb* v, uint64_t lba, uint32_t count, void* buf, int is_write);
static int vmb_rw(struct vmb* v, uint64_t lba, uint32_t count, void* buf, int is_write) {
    kmutex_lock(&v->lock);
    int rc = vmb_rw_unlocked(v, lba, count, buf, is_write);
    kmutex_unlock(&v->lock);
    return rc;
}

static int vmb_rw_unlocked(struct vmb* v, uint64_t lba, uint32_t count, void* buf, int is_write) {
    v->q_hdr.type     = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    v->q_hdr.reserved = 0;
    v->q_hdr.sector   = lba;

    v->q_desc[0].addr = kptr_phys(&v->q_hdr);
    v->q_desc[0].len  = sizeof v->q_hdr;
    v->q_desc[0].flags = VRING_DESC_F_NEXT;
    v->q_desc[0].next = 1;

    v->q_desc[1].addr = kptr_phys(buf);
    v->q_desc[1].len  = count * SECTOR;
    v->q_desc[1].flags = VRING_DESC_F_NEXT | (is_write ? 0 : VRING_DESC_F_WRITE);
    v->q_desc[1].next = 2;

    v->q_desc[2].addr = kptr_phys((const void*)&v->q_status);
    v->q_desc[2].len  = 1;
    v->q_desc[2].flags = VRING_DESC_F_WRITE;
    v->q_desc[2].next = 0;

    v->q_status = 0xff;

    uint16_t ai = v->q_avail.idx;
    v->q_avail.ring[ai % QSIZE] = 0;       /* head descriptor index          */
    dsb();
    v->q_avail.idx = ai + 1;
    dsb();

    w32(v, R_QUEUENOTIFY, 0);               /* kick queue 0                   */

    uint64_t deadline = timer_ticks_ms() + 5000;
    while (!vmb_done(v)) {
        if (timer_ticks_ms() > deadline) {
            kprintf("virtio-mmio-blk: %s request timed out after 5000 ms\n", v->name);
            return -1;
        }
        struct task* me = task_current();
        int may_sleep = me && !me->is_idle && preempt_count() == 0;
        if (!v->irqs || !may_sleep) {
            if (may_sleep) task_yield();
            continue;
        }
        {
            static int told;
            if (!told) {
                told = 1;
                kprintf("virtio-mmio-blk: completion interrupts work - requests "
                        "now sleep instead of polling (%u so far)\n", v->irqs);
            }
        }
        struct ktimer t = { 0, 0, 0, 0, 0 };
        ktimer_arm_after(&t, 2000000ull, vmb_backstop, &v->wq);
        uint32_t fl = waitq_lock(&v->wq);
        if (!vmb_done(v)) waitq_block(&v->wq);
        waitq_unlock(&v->wq, fl);
        ktimer_cancel(&t);
    }
    v->last_used++;
    dsb();

    if (r32(v, R_INTSTATUS) & 1) w32(v, R_INTACK, 1);
    return (v->q_status == 0) ? 0 : -1;
}

static int vmb_read(struct block_device* dev, uint64_t lba, uint32_t count, void* buf) {
    struct vmb* v = (struct vmb*)dev->priv;
    for (uint32_t i = 0; i < count; i++)
        if (vmb_rw(v, lba + i, 1, (uint8_t*)buf + i * SECTOR, 0) != 0) return -1;
    return 0;
}
static int vmb_write(struct block_device* dev, uint64_t lba, uint32_t count, const void* buf) {
    struct vmb* v = (struct vmb*)dev->priv;
    for (uint32_t i = 0; i < count; i++)
        if (vmb_rw(v, lba + i, 1, (uint8_t*)(uintptr_t)buf + i * SECTOR, 1) != 0) return -1;
    return 0;
}

/* ---- probe + init ---------------------------------------------------------- */

static int vmb_init_one(struct vmb* v, int idx) {
    kmutex_init(&v->lock, "virtio-mmio-blk");
    waitq_init(&v->wq);

    /* Reset, then ACKNOWLEDGE + DRIVER. */
    w32(v, R_STATUS, 0);
    w32(v, R_STATUS, ST_ACK);
    w32(v, R_STATUS, ST_ACK | ST_DRIVER);

    /* Feature negotiation: accept only VIRTIO_F_VERSION_1 (feature bit 32). */
    w32(v, R_DEVFEATSEL, 1); (void)r32(v, R_DEVFEAT);
    w32(v, R_DRVFEATSEL, 1); w32(v, R_DRVFEAT, 1u << VIRTIO_F_VERSION_1_BIT);
    w32(v, R_DEVFEATSEL, 0); (void)r32(v, R_DEVFEAT);
    w32(v, R_DRVFEATSEL, 0); w32(v, R_DRVFEAT, 0);

    w32(v, R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(r32(v, R_STATUS) & ST_FEATURES_OK)) {
        kprintf("virtio-mmio: device rejected features\n");
        return -1;
    }

    /* Set up virtqueue 0. */
    w32(v, R_QUEUESEL, 0);
    if (r32(v, R_QUEUEREADY) != 0) { kprintf("virtio-mmio: queue busy\n"); return -1; }
    uint32_t qmax = r32(v, R_QUEUENUMMAX);
    if (qmax < QSIZE) { kprintf("virtio-mmio: QueueNumMax %u < %u\n", qmax, QSIZE); return -1; }
    w32(v, R_QUEUENUM, QSIZE);

    uint64_t d = kptr_phys(v->q_desc);
    uint64_t a = kptr_phys(&v->q_avail);
    uint64_t u = kptr_phys(&v->q_used);
    w32(v, R_QDESC_LO, (uint32_t)d);  w32(v, R_QDESC_HI, (uint32_t)(d >> 32));
    w32(v, R_QDRV_LO,  (uint32_t)a);  w32(v, R_QDRV_HI,  (uint32_t)(a >> 32));
    w32(v, R_QDEV_LO,  (uint32_t)u);  w32(v, R_QDEV_HI,  (uint32_t)(u >> 32));
    w32(v, R_QUEUEREADY, 1);

    w32(v, R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    /* The completion interrupt: handler first, then unmask (the install-then-
     * unmask split gic.c mirrors from x86). */
    gic_register_handler(board_virtio_intid(v->slot), vmb_irq);
    gic_enable_irq(board_virtio_intid(v->slot));

    /* Capacity (sectors) is the first u64 of the block config space. */
    uint64_t cap = (uint64_t)r32(v, R_CONFIG) | ((uint64_t)r32(v, R_CONFIG + 4) << 32);

    v->name[0] = 'v'; v->name[1] = 'd'; v->name[2] = (char)('a' + idx); v->name[3] = 0;
    v->bd.name         = v->name;
    v->bd.sector_size  = SECTOR;
    v->bd.sector_count = cap;
    v->bd.read         = vmb_read;
    v->bd.write        = vmb_write;
    v->bd.flush        = NULL;
    v->bd.priv         = v;
    blk_register(&v->bd);

    kprintf("virtio-mmio: /dev/%s ready (%u sectors, %u MiB) at slot base %p\n",
            v->name, (unsigned)cap, (unsigned)(cap / 2048), (void*)v->base);
    return 0;
}

/* Scan the MMIO slots for virtio-block (deviceID 2) modern transports and
 * register each as vda, vdb, ...  Called once from the aarch64 bring-up; -1
 * when no disk is attached.
 *
 * DESCENDING: QEMU `virt` hands the FIRST `-device` on the command line the
 * HIGHEST-numbered transport, so walking the slots downwards makes the
 * command-line order the vda/vdb order — the first disk stays the boot
 * volume's, as it was with one. */
int virtio_mmio_blk_init(void) {
    for (int i = board_virtio_count() - 1; i >= 0 && g_nvmb < VMB_MAX; i--) {
        uintptr_t base = (uintptr_t)hal_mmio_map(board_virtio_base(i), 0x200);   /* §M90 */
        if (!base) continue;
        uint32_t magic = *(volatile uint32_t*)(base + R_MAGIC);
        if (magic != VIRTIO_MAGIC) continue;
        uint32_t ver = *(volatile uint32_t*)(base + R_VERSION);
        uint32_t dev = *(volatile uint32_t*)(base + R_DEVICEID);
        if (dev == 0) continue;                  /* empty transport slot       */
        kprintf("virtio-mmio: slot %d dev=%u ver=%u\n", i, dev, ver);
        if (dev != 2 || ver != 2) continue;      /* want a modern block device */
        struct vmb* v = &g_vmb[g_nvmb];
        v->base = base;
        v->slot = i;
        g_nvmb++;                                /* the IRQ handler may see it now */
        if (vmb_init_one(v, g_nvmb - 1) != 0) { v->base = 0; g_nvmb--; }
    }
    return g_nvmb ? 0 : -1;
}
