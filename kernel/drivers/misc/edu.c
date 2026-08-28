/* =============================================================================
 * edu.c — QEMU's educational PCI device, driven entirely through drvrt.h.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS DRIVER EXISTS, WHICH IS NOT "TO DRIVE THIS DEVICE"
 *
 * §M33 left three mechanisms finished and unreachable, all for the same reason:
 * nothing in the tree used them.  `drv_dma_request` could not build a per-driver
 * IOMMU domain because no driver written against drvrt does DMA; ring-3
 * `drv_mmio_request` refused, because no placeable driver needs MMIO; and
 * aarch64 could not place a driver at all, because the only ported one (the PS/2
 * mouse) needs a port space that arch does not have.
 *
 * Building those mechanisms anyway would have been the mistake §M59 declined to
 * make for Wayland's `wl_data_device`: *a surface with nothing to falsify it
 * against works until the first real user.*  So the honest way to finish §M33
 * was to find a client, and the smallest honest client is a device that exists
 * to be driven and that nothing depends on.
 *
 * QEMU's `edu` device is exactly that: an MMIO register window, a real DMA
 * engine, and an interrupt, in a device whose entire purpose is to be a teaching
 * example.  It is available on x86 AND on `-M virt`, which is what lets aarch64
 * place a driver for the first time.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS AND IS NOT SYNTHETIC HERE — read this before trusting the result
 *
 * The DEVICE's purpose is synthetic; the PATH is not.  The DMA is real bus-master
 * DMA, translated by a real IOMMU through real page tables, and refused by real
 * hardware when it goes outside its domain.  A mechanism proven this way is
 * proven; what a synthetic client cannot tell you is whether the INTERFACE is
 * pleasant to write a complicated driver against.
 *
 * That gap is named rather than papered over: porting AC97's DMA path to drvrt
 * stays on the list at low priority, and its whole value is to answer that
 * second question.  §M33's claim does not depend on it.
 *
 * -----------------------------------------------------------------------------
 * THE DEVICE (QEMU docs/specs/edu.rst)
 *
 * Vendor 0x1234, device 0x11E8.  BAR0 is a 1 MiB MMIO window:
 *
 *   0x00  identification, read-only, 0xRRrr00edu
 *   0x04  liveness check — write a value, read back its bitwise NOT
 *   0x08  factorial — write n, read n! once bit 0 of 0x20 clears
 *   0x20  status: bit 0 = computing, bit 7 = interrupt when the factorial ends
 *   0x24  interrupt status, read-only
 *   0x60  raise interrupt (write the bits to raise)
 *   0x64  acknowledge interrupt
 *   0x80  DMA source      0x88  DMA destination
 *   0x90  DMA count       0x98  DMA command
 *                              bit 0 start, bit 1 direction, bit 2 interrupt
 *
 * The device has 4 KiB of internal buffer at 0x40000 in ITS OWN address space —
 * a DMA address on the device side, not a host address, which is why the API's
 * separation of CPU and device addresses matters here rather than being
 * theoretical.
 * ============================================================================= */

#include "driver.h"
#include "drvrt.h"
#include "pci.h"
#include "printf.h"
#include "klog.h"
#include "task.h"
#include "config.h"
#include "settings.h"
#include "iommu.h"
#include <stddef.h>

#define EDU_VENDOR      0x1234
#define EDU_DEVICE      0x11E8

#define EDU_ID          0x00
#define EDU_LIVENESS    0x04
#define EDU_FACTORIAL   0x08
#define EDU_STATUS      0x20
#define EDU_IRQ_STATUS  0x24
#define EDU_IRQ_RAISE   0x60
#define EDU_IRQ_ACK     0x64
#define EDU_DMA_SRC     0x80
#define EDU_DMA_DST     0x88
#define EDU_DMA_CNT     0x90
#define EDU_DMA_CMD     0x98

#define EDU_STATUS_COMPUTING   0x01
#define EDU_STATUS_IRQ_ON_DONE 0x80

#define EDU_DMA_START   0x1
#define EDU_DMA_TO_DEV  0x0            /* RAM -> device */
#define EDU_DMA_FROM_DEV 0x2           /* device -> RAM */
#define EDU_DMA_IRQ     0x4

#define EDU_DEV_BUFFER  0x40000ull     /* inside the DEVICE's address space */
#define EDU_DMA_BYTES   4096

static struct drv_rt   rt;
static drv_handle      h_mmio = -1, h_irq = -1, h_dma = -1;
static volatile uint8_t* regs;
static uint16_t        edu_bdf;
static uint32_t        edu_irqs;

/* 32- and 64-bit register access.  The DMA address registers are 64-bit and the
 * device latches on the write that completes them, so the halves are written in
 * a fixed order rather than left to the compiler. */
static uint32_t r32(int off) { return *(volatile uint32_t*)(regs + off); }
static void     w32(int off, uint32_t v) { *(volatile uint32_t*)(regs + off) = v; }
static void     w64(int off, uint64_t v) {
    volatile uint32_t* p = (volatile uint32_t*)(regs + off);
    p[0] = (uint32_t)v;
    p[1] = (uint32_t)(v >> 32);
}

static int edu_probe(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    return pci_find_device(EDU_VENDOR, EDU_DEVICE, &pd) == 0 ? 0 : -1;
}

static int edu_init(void* ctx) {
    (void)ctx;
    struct pci_device pd;
    if (pci_find_device(EDU_VENDOR, EDU_DEVICE, &pd) != 0) return -1;

    drv_rt_init(&rt, "edu");
    edu_bdf = (uint16_t)((pd.bus << 8) | (pd.slot << 3) | pd.func);

    /* TELL THE KERNEL WHICH DEVICE THIS DRIVER IS.
     *
     * Not bookkeeping: it is what makes per-driver DMA confinement possible at
     * all.  The kernel can build a domain containing exactly this driver's
     * buffers, and it cannot put the DEVICE in that domain without being told
     * which device the driver speaks for.  A driver that does DMA and never
     * says is a driver the kernel can only leave in the identity domain. */
    drv_bind_device(&rt, edu_bdf);

    /* MEMORY SPACE AND BUS MASTER, BEFORE ANYTHING ELSE.
     *
     * A device cannot initiate DMA with the bus-master bit clear, and the
     * failure is silent in the worst way: the command register accepts the
     * transfer, the status register clears as if it had run, and the buffer
     * comes back untouched.  That is exactly what the first version of this
     * driver saw — 255 of 256 bytes wrong, with every register reporting
     * success — and it survived a first reading because the addresses and the
     * direction bits were all correct.
     *
     * Firmware usually leaves this set for devices it enumerated; a driver that
     * relies on that works until the first machine where it does not. */
    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND,
                (uint16_t)(cmd | 0x0002 /* memory space */ | 0x0004 /* bus master */));

    uint64_t bar = (uint64_t)(pd.bar[0] & ~0xFull);
    h_mmio = drv_mmio_request(&rt, bar, 0x100000, "edu registers");
    if (h_mmio < 0) { kprintf("edu: no MMIO (%d)\n", h_mmio); return -1; }
    regs = (volatile uint8_t*)drv_mmio_ptr(h_mmio);
    if (!regs) { kprintf("edu: MMIO mapped to nothing\n"); return -1; }

    uint32_t id = r32(EDU_ID);
    if ((id & 0xFFFF) != 0x00ED) {
        /* THE IDENTIFICATION REGISTER IS CHECKED, not assumed from the PCI ID.
         * A BAR that was never programmed reads back as all-ones or zero, and a
         * driver that trusts the PCI ID alone would then write DMA commands
         * into whatever the window actually decodes. */
        kprintf("edu: identification register reads %x — window not decoding\n", id);
        return -1;
    }

    /* Liveness: the device returns the bitwise NOT of whatever is written.  A
     * cheap round trip that proves reads and writes both reach the device,
     * which a read-only check cannot. */
    w32(EDU_LIVENESS, 0xA5A5A5A5u);
    uint32_t back = r32(EDU_LIVENESS);
    if (back != ~0xA5A5A5A5u) {
        kprintf("edu: liveness check failed (%x)\n", back);
        return -1;
    }

    /* HOW MANY ADDRESS BITS THE DEVICE HAS — a property of the INSTANCE, which
     * is why it is a setting rather than a constant.  QEMU's edu defaults to 28
     * and exposes `dma_mask` as a device property, so the guest-side
     * declaration has to be able to follow it.
     *
     * THE DEFAULT COST THE FIRST RUN OF THIS DRIVER ITS RESULT, and the lesson
     * is worth more than the fix.  With no width in the API at all, the driver
     * was handed a DMA32 buffer at ~1023 MiB; the device CLAMPED the address to
     * 28 bits, read some other page, cleared its status register to say the
     * transfer had succeeded, and 255 of 256 bytes came back wrong with nothing
     * anywhere explaining it.  *A device quietly truncating an address it
     * cannot hold is the classic DMA bug, and an API that cannot express the
     * constraint guarantees every driver rediscovers it.*
     *
     * AND THE SECOND FINDING IS ABOUT THIS KERNEL, NOT THE DEVICE: at 28 bits
     * the allocation is REFUSED, because the kernel image is ~61 MiB and
     * nothing is free low enough.  Refused, not truncated — which is the API
     * behaving correctly, and a fact about this system worth knowing before
     * somebody ports a genuinely narrow device. */
    int dma_bits = 28;
    { const char* v = config_get("driver.edu.dma_bits", "28");
      int n = 0; for (; *v >= '0' && *v <= '9'; v++) n = n * 10 + (*v - '0');
      if (n >= 20 && n <= 64) dma_bits = n; }
    h_dma = drv_dma_request(&rt, EDU_DMA_BYTES, dma_bits, "edu transfer buffer");
    if (h_dma < 0) {
        kprintf("edu: no DMA buffer at %d address bits (%d) — this kernel has "
                "nothing free that low\n", dma_bits, h_dma);
        return -1;
    }

    if (pd.irq_line != 0xFF) {
        h_irq = drv_irq_request(&rt, pd.irq_line, "edu completion");
        if (h_irq < 0) kprintf("edu: no IRQ grant (%d) — polling instead\n", h_irq);
    }

    kprintf("edu: up at PCI %x:%x.%x, id %x, %d-byte DMA buffer\n",
            pd.bus, pd.slot, pd.func, id, EDU_DMA_BYTES);
    klog(KLOG_INFO, "edu", "educational device ready — §M33's DMA client\n");
    return 0;
}

static int edu_shutdown(void* ctx) {
    (void)ctx;
    if (h_mmio < 0) return 0;
    w32(EDU_DMA_CMD, 0);                  /* stop any transfer first */
    drv_release_all(&rt);
    h_mmio = h_irq = h_dma = -1;
    regs = NULL;
    kprintf("edu: stopped\n");
    return 0;
}

CONFIG_KEY(ck_edu_dma_bits) = {
    .key = "driver.edu.dma_bits", .group = "System", .type = CFG_INT,
    .min = 20, .max = 64, .def = "28",
    .help = "how many address bits the edu device has — match QEMU's dma_mask",
};

static const struct driver_ops edu_ops = {
    .probe = edu_probe, .init = edu_init, .shutdown = edu_shutdown,
};

/* DOMAIN_KERNEL | DOMAIN_USER, and DRVF_DMA because it is one — which is what
 * makes it the first driver whose placement raises the isolation question with
 * a real answer rather than a note. */
DRIVER_EX(edu, "misc", &edu_ops, NULL, DOMAIN_KERNEL | DOMAIN_USER, DRVF_DMA);

/* ----------------------------------------------------------------------
 * `edutest` — a DMA round trip, and the only thing that can tell a working
 * translation from a broken one.
 *
 * Fill the buffer with a pattern, DMA it INTO the device, clear the buffer, DMA
 * it BACK, and compare.  Both directions on purpose: a one-way test passes on a
 * device that ignores the transfer entirely and leaves the buffer alone.
 * ---------------------------------------------------------------------- */
static int dma_wait(void) {
    for (int i = 0; i < 200; i++) {
        if (!(r32(EDU_DMA_CMD) & EDU_DMA_START)) return 0;
        task_msleep(5);
    }
    return -1;
}

void edu_test(void) {
    /* START THE DRIVER IF IT IS NOT UP.
     *
     * A test that requires three commands typed in the right order in front of
     * it is a test that mostly measures the harness — this one lost characters
     * often enough to produce two false results before the point was taken.
     * `drvtest` already sets up its own preconditions for the same reason. */
    if (h_mmio < 0 || h_dma < 0) {
        if (driver_find("edu")) driver_start("edu");
    }
    if (h_mmio < 0 || h_dma < 0) {
        kprintf("edutest: the edu device is not up.  Attach `-device edu`, and\n"
                "  if its DMA width is not the default 28 bits, say so with\n"
                "  `setconf driver.edu.dma_bits <n>` first.\n");
        return;
    }
    uint8_t*  cpu = (uint8_t*)drv_dma_cpu(h_dma);
    uint64_t  dev = drv_dma_device(h_dma);
    if (!cpu) { kprintf("edutest: no DMA buffer\n"); return; }

    /* THE TWO ADDRESSES ARE USED FOR THE TWO DIFFERENT THINGS, which is the
     * whole reason drvrt.h separates them: `cpu` to fill the buffer, `dev` to
     * program into the device.  They are equal today and will not be once a
     * driver's buffers live in a domain of their own. */
    for (int i = 0; i < 256; i++) cpu[i] = (uint8_t)(i ^ 0x5A);

    w64(EDU_DMA_SRC, dev);
    w64(EDU_DMA_DST, EDU_DEV_BUFFER);
    w64(EDU_DMA_CNT, 256);
    w32(EDU_DMA_CMD, EDU_DMA_START | EDU_DMA_TO_DEV);
    if (dma_wait() != 0) { kprintf("edutest: FAIL — transfer to the device never finished\n"); return; }

    for (int i = 0; i < 256; i++) cpu[i] = 0;

    w64(EDU_DMA_SRC, EDU_DEV_BUFFER);
    w64(EDU_DMA_DST, dev);
    w64(EDU_DMA_CNT, 256);
    w32(EDU_DMA_CMD, EDU_DMA_START | EDU_DMA_FROM_DEV);
    if (dma_wait() != 0) { kprintf("edutest: FAIL — transfer back never finished\n"); return; }

    int bad = 0;
    for (int i = 0; i < 256; i++) if (cpu[i] != (uint8_t)(i ^ 0x5A)) bad++;
    if (bad) {
        kprintf("edutest: FAIL — %d of 256 bytes came back wrong\n", bad);
        return;
    }
    kprintf("edutest: 256 bytes out and back through the device, all correct\n");
    kprintf("  cpu address %x, device address %x%s\n",
            (unsigned)(uintptr_t)cpu, (unsigned)dev,
            ((uintptr_t)cpu == (uintptr_t)dev) ? " (equal — no IOMMU remap yet)" : "");

    /* AND NOW THE OTHER HALF, IN THE SAME COMMAND.
     *
     * A round trip that works proves the device can reach what it was given.
     * It says nothing at all about whether it can reach anything ELSE — and on
     * a machine with no IOMMU the answer is "all of memory", which looks
     * identical from here.  So the test aims the device's own DMA engine at an
     * address 64 KiB past its buffer and reports what the HARDWARE did about
     * it, before and after.
     *
     * Both outcomes are printed as facts rather than as pass/fail, because both
     * are correct answers to different machines: unconfined is what this
     * kernel has always done, and confined is what §M33 set out to reach. */
    uint32_t before = iommu_fault_count();
    uint64_t outside = dev + 0x10000ull;
    kprintf("  now aiming it at %x, 64 KiB past its buffer\n", (unsigned)outside);

    w64(EDU_DMA_SRC, EDU_DEV_BUFFER);
    w64(EDU_DMA_DST, outside);
    w64(EDU_DMA_CNT, 256);
    w32(EDU_DMA_CMD, EDU_DMA_START | EDU_DMA_FROM_DEV);
    dma_wait();
    uint32_t after = iommu_fault_count();

    if (after > before)
        kprintf("  REFUSED by the hardware (%u fault record(s)) — the device is "
                "confined to its own buffer\n", after - before);
    else if (iommu_get()->state == IOMMU_ACTIVE)
        kprintf("  NOT refused, and translation IS on — this device is in the "
                "identity domain or bypasses the unit\n");
    else
        kprintf("  NOT refused — there is nothing on this machine that could "
                "have refused it (`iommu` says why)\n");
}

/* Deliberately reach OUTSIDE the granted buffer.
 *
 * This is `drv crash`'s shape for DMA: a safety net nobody has fallen into is
 * one nobody has tested.  The driver aims the device's own DMA engine at an
 * address it was never given, and what happens next is the whole question — in
 * the identity domain it succeeds silently and overwrites somebody's memory; in
 * a per-driver domain the IOMMU refuses it and records which address. */
void edu_escape(uint64_t phys) {
    if (h_mmio < 0) { kprintf("edu: not up\n"); return; }
    kprintf("edu: aiming the device's DMA at %x, which it was never granted\n",
            (unsigned)phys);
    w64(EDU_DMA_SRC, EDU_DEV_BUFFER);
    w64(EDU_DMA_DST, phys);
    w64(EDU_DMA_CNT, 256);
    w32(EDU_DMA_CMD, EDU_DMA_START | EDU_DMA_FROM_DEV);
    if (dma_wait() != 0)
        kprintf("edu: the transfer did not complete — consistent with a refusal\n");
    else
        kprintf("edu: the transfer COMPLETED — nothing stopped it\n");
}
