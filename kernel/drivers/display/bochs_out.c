/* =============================================================================
 * bochs_out.c — a SECOND monitor: QEMU's bochs-display as an extra output
 * (§M88), one source for all three arches.
 *
 * The device (PCI 1234:1111, class 0x0380 "display, other") is a linear
 * framebuffer in BAR0 and the Bochs DISPI registers memory-mapped at BAR2 +
 * 0x500, sixteen bits wide.  x86's boot VGA carries the SAME vendor/device id
 * but class 0x0300, and on sbsa-ref a bochs-display IS the primary display
 * (aarch64's bochs_display.c drives it) — so this driver takes a 1234:1111
 * that is class 0x0380 AND is not the primary's device.
 *
 * Written against drvrt.h (like edu.c), because the MMIO mapping is the one
 * arch-specific thing a display driver needs and drvrt already answers it on
 * all three arches.
 *
 * A RAM SHADOW, NOT THE BAR.  The compositor draws into ordinary memory and
 * `flush` copies the damaged rectangle into VRAM with aligned 32-bit stores:
 * on aarch64 the BAR is Device memory, which forbids the unaligned accesses a
 * memcpy makes, and doing the same on x86 keeps one code path.  The cost is one
 * copy of what changed — the shape virtio-gpu already has, so the compositor
 * cannot tell the outputs apart.
 *
 * The mode is fixed at DISPLAY2_W x DISPLAY2_H in this first cut (PLAN §M88,
 * rung 5: per-output mode setting comes later).
 * ============================================================================= */

#include "driver.h"
#include "drvrt.h"
#include "display.h"
#include "pci.h"
#include "kmalloc.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

#define BO_VENDOR  0x1234
#define BO_DEVICE  0x1111
#define DISPLAY2_W 1280
#define DISPLAY2_H 800

#define DISPI_ID       0x0
#define DISPI_XRES     0x1
#define DISPI_YRES     0x2
#define DISPI_BPP      0x3
#define DISPI_ENABLE   0x4
#define DISPI_VWIDTH   0x6
#define DISPI_VHEIGHT  0x7
#define DISPI_XOFF     0x8
#define DISPI_YOFF     0x9
#define DISPI_ENABLED  0x01
#define DISPI_LFB      0x40
#define DISPI_MMIO     0x500

static struct drv_rt rt;
static volatile uint16_t* g_regs;
static volatile uint32_t* g_vram;
static struct display_output g_out;
static int g_bdf = -1;

/* aarch64's primary bochs driver overrides this with the BDF it drives; on
 * every other machine no bochs-display is the primary. */
__attribute__((weak)) int bochs_display_primary_bdf(void) { return -1; }

struct bo_find { struct pci_device pd; int found; };
static void bo_visit(const struct pci_device* d, void* ctx) {
    struct bo_find* f = (struct bo_find*)ctx;
    if (f->found || d->vendor_id != BO_VENDOR || d->device_id != BO_DEVICE) return;
    if (d->class_code != 0x03 || d->subclass != 0x80) return;   /* not the boot VGA */
    int bdf = (d->bus << 8) | (d->slot << 3) | d->func;
    if (bdf == bochs_display_primary_bdf()) return;             /* sbsa's primary  */
    f->pd = *d;
    f->found = 1;
}

static int bo_probe(void* ctx) {
    (void)ctx;
    struct bo_find f = { .found = 0 };
    pci_scan(bo_visit, &f);
    return f.found ? 0 : -1;
}

static inline void wr(int idx, uint16_t v) { g_regs[idx] = v; }
static inline uint16_t rd(int idx) { return g_regs[idx]; }

static void bo_flush(struct display_output* o, int x, int y, int w, int h) {
    if (!g_vram || x >= o->w || y >= o->h || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > o->w) w = o->w - x;
    if (y + h > o->h) h = o->h - y;
    for (int row = y; row < y + h; row++) {
        const uint32_t* src = o->px + (size_t)row * (size_t)o->stride + x;
        volatile uint32_t* dst = g_vram + (size_t)row * (size_t)o->w + x;
        for (int i = 0; i < w; i++) dst[i] = src[i];          /* aligned stores */
    }
}

static int bo_init(void* ctx) {
    (void)ctx;
    struct bo_find f = { .found = 0 };
    pci_scan(bo_visit, &f);
    if (!f.found) return -1;
    drv_rt_init(&rt, "bochs_out");
    g_bdf = (f.pd.bus << 8) | (f.pd.slot << 3) | f.pd.func;
    drv_bind_device(&rt, (uint16_t)g_bdf);

    uint64_t fb = 0, fblen = 0, mm = 0, mmlen = 0;
    if (drv_device_window(&rt, 0, &fb, &fblen) != 0 || !fb ||
        drv_device_window(&rt, 2, &mm, &mmlen) != 0 || !mm) {
        kprintf("bochs_out: device %x has no framebuffer/register window\n", g_bdf);
        return -1;
    }
    uint64_t need = (uint64_t)DISPLAY2_W * DISPLAY2_H * 4;
    if (fblen < need) {
        kprintf("bochs_out: framebuffer window %u KiB is smaller than %ux%u needs\n",
                (unsigned)(fblen >> 10), DISPLAY2_W, DISPLAY2_H);
        return -1;
    }
    drv_handle hr = drv_mmio_request(&rt, mm, 4096, "bochs_out registers");
    drv_handle hf = drv_mmio_request(&rt, fb, (size_t)need, "bochs_out framebuffer");
    if (hr < 0 || hf < 0) { kprintf("bochs_out: MMIO refused (%d, %d)\n", hr, hf); return -1; }
    g_regs = (volatile uint16_t*)((volatile uint8_t*)drv_mmio_ptr(hr) + DISPI_MMIO);
    g_vram = (volatile uint32_t*)drv_mmio_ptr(hf);
    if (!g_regs || !g_vram) return -1;

    uint16_t id = rd(DISPI_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        /* The ID register is checked, not assumed from the PCI id — a register
         * window that does not decode reads as garbage, and a mode written into
         * garbage is a monitor that stays dark with nothing saying why. */
        kprintf("bochs_out: DISPI id reads %x - register window not decoding\n", id);
        return -1;
    }
    wr(DISPI_ENABLE, 0);
    wr(DISPI_BPP, 32);
    wr(DISPI_XRES, DISPLAY2_W);
    wr(DISPI_YRES, DISPLAY2_H);
    wr(DISPI_VWIDTH, DISPLAY2_W);
    wr(DISPI_VHEIGHT, DISPLAY2_H);
    wr(DISPI_XOFF, 0);
    wr(DISPI_YOFF, 0);
    wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    int w = rd(DISPI_XRES), h = rd(DISPI_YRES);   /* read BACK: the device clamps */
    if (w != DISPLAY2_W || h != DISPLAY2_H || rd(DISPI_BPP) != 32) {
        kprintf("bochs_out: asked for %dx%d, the device reports %dx%d - not used\n",
                DISPLAY2_W, DISPLAY2_H, w, h);
        return -1;
    }

    uint32_t* shadow = (uint32_t*)kmalloc((size_t)need);
    if (!shadow) { kprintf("bochs_out: no memory for the shadow buffer\n"); return -1; }
    for (size_t i = 0; i < (size_t)w * h; i++) shadow[i] = 0xFF101820u;   /* dark, not black */

    g_out = (struct display_output){ .w = w, .h = h, .px = shadow, .stride = w,
                                     .flush = bo_flush };
    const char* nm = "bochs1";
    for (int i = 0; nm[i]; i++) g_out.name[i] = nm[i];
    bo_flush(&g_out, 0, 0, w, h);
    if (display_register(&g_out) < 0) return -1;
    kprintf("bochs_out: second monitor %dx%d on device %x\n", w, h, g_bdf);
    return 0;
}

/* A monitor the desktop is drawing on cannot be stopped under it. */
static int bo_shutdown(void* ctx) { (void)ctx; return -1; }

static const struct driver_ops bo_ops = {
    .probe = bo_probe, .init = bo_init, .shutdown = bo_shutdown,
};
DRIVER(bochs_out, "display", &bo_ops, NULL);
