/* =============================================================================
 * bochs_display.c — QEMU's bochs-display on PCIe, the display of sbsa-ref
 * (§M85).
 *
 * `virt` gets its picture from a virtio-gpu; sbsa-ref has no virtio at all.
 * What it has is `bochs-display` (PCI 1234:1111): a linear framebuffer in BAR0
 * and a small register file in BAR2, whose DISPI registers are the same
 * XRES/YRES/BPP/ENABLE set the x86 port drives through I/O ports — here they
 * are memory-mapped at BAR2 + 0x500, sixteen bits wide, one per index.
 *
 * WHY A SHADOW BUFFER AND NOT THE BAR ITSELF.  Low physical memory on this
 * port is mapped as DEVICE memory (§M86: RAM is reached only through the
 * direct map), and Device memory forbids unaligned accesses — while the
 * portable renderer (fb_terminal, the compositor) uses memcpy/memmove freely.
 * Mapping the BAR as Normal-NC instead needs a 2 MiB-grain carve-out of a
 * 1 GiB block that also holds other devices' registers.  So the pixels are
 * drawn into ordinary RAM, and fb_present_flush — which every renderer
 * already calls with its dirty rectangle, because virtio-gpu needs exactly
 * that — copies the rectangle into the BAR with ALIGNED 32-bit stores.  The
 * shape is virtio-gpu's (draw in RAM, flush a rect), so nothing above this
 * file can tell the two displays apart.  The cost is one extra copy of what
 * changed; a later Normal-NC mapping can remove it without touching a caller.
 * ============================================================================= */

#include "fb_backend.h"
#include "fb_present.h"
#include "pci.h"
#include "pmm.h"
#include "printf.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

#define BOCHS_VENDOR   0x1234
#define BOCHS_DEVICE   0x1111

/* DISPI register indices (the Bochs VBE interface). */
#define DISPI_ID          0x0
#define DISPI_XRES        0x1
#define DISPI_YRES        0x2
#define DISPI_BPP         0x3
#define DISPI_ENABLE      0x4
#define DISPI_VIRT_WIDTH  0x6
#define DISPI_VIRT_HEIGHT 0x7
#define DISPI_X_OFFSET    0x8
#define DISPI_Y_OFFSET    0x9
#define DISPI_VIDEO_MEM64K 0xA
#define DISPI_ENABLED     0x01
#define DISPI_LFB         0x40
#define DISPI_MMIO_BASE   0x500      /* within BAR2 */

#define FB_W_DEFAULT 1280
#define FB_H_DEFAULT 800

extern int fb_term_init_direct(uint64_t phys, uint32_t width, uint32_t height,
                               uint32_t pitch);
void mmu_map_device_1gib(uint64_t va);          /* mmu.c */

static volatile uint16_t* g_regs;    /* BAR2 + 0x500                        */
static volatile uint32_t* g_vram;    /* BAR0, Device memory                 */
static uint64_t g_vram_bytes;
static uint64_t g_shadow_phys;       /* the RAM the renderer draws into     */
static uint32_t g_shadow_frames;
static uint32_t g_w, g_h, g_pitch;
static int      g_ready;
/* §M88 — which device this driver made the PRIMARY display, so the portable
 * second-monitor driver (bochs_out.c) takes a different one. */
static int      g_primary_bdf = -1;
int bochs_display_primary_bdf(void) { return g_primary_bdf; }

static inline void wr(uint32_t idx, uint16_t v) { g_regs[idx] = v; }
static inline uint16_t rd(uint32_t idx)         { return g_regs[idx]; }

/* 64-bit BARs are read as a pair; the high half matters on a firmware that
 * places the prefetchable window above 4 GiB. */
static uint64_t bar_addr(const struct pci_device* d, int i) {
    uint32_t lo = d->bar[i];
    uint64_t a = lo & ~0xFull;
    if (((lo >> 1) & 3) == 2 && i < 5) a |= (uint64_t)d->bar[i + 1] << 32;
    return a;
}

/* A BAR above the always-mapped low 4 GiB gets its 1 GiB Device block. */
static void* map_bar(uint64_t pa) {
    if (pa >= (1ull << 32)) mmu_map_device_1gib(pa);
    return (void*)(uintptr_t)pa;
}

static void bochs_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_ready || x >= g_w || y >= g_h || !w || !h) return;
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    const uint8_t* shadow = (const uint8_t*)phys_to_virt(g_shadow_phys);
    for (uint32_t row = y; row < y + h; row++) {
        const uint32_t* src = (const uint32_t*)(shadow + (uint64_t)row * g_pitch) + x;
        volatile uint32_t* dst =
            (volatile uint32_t*)((volatile uint8_t*)g_vram + (uint64_t)row * g_pitch) + x;
        for (uint32_t i = 0; i < w; i++) dst[i] = src[i];   /* aligned stores */
    }
}

/* Program the DISPI registers and read the geometry BACK: the device clamps
 * what it cannot do, and believing the write would leave us drawing at a size
 * the display is not showing (the x86 backend's rule, §M61). */
static int dispi_set(uint32_t w, uint32_t h) {
    wr(DISPI_ENABLE, 0);
    wr(DISPI_BPP, 32);
    wr(DISPI_XRES, (uint16_t)w);
    wr(DISPI_YRES, (uint16_t)h);
    wr(DISPI_VIRT_WIDTH, (uint16_t)w);
    wr(DISPI_VIRT_HEIGHT, (uint16_t)h);
    wr(DISPI_X_OFFSET, 0);
    wr(DISPI_Y_OFFSET, 0);
    wr(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    return (rd(DISPI_XRES) == w && rd(DISPI_YRES) == h && rd(DISPI_BPP) == 32) ? 0 : -1;
}

static const struct fb_mode modes[] = {
    {  640,  480, 32 }, {  800,  600, 32 }, { 1024,  768, 32 },
    { 1280,  720, 32 }, { 1280,  800, 32 }, { 1280, 1024, 32 },
    { 1440,  900, 32 }, { 1600,  900, 32 }, { 1680, 1050, 32 },
    { 1920, 1080, 32 }, { 1920, 1200, 32 },
};
#define N_MODES ((int)(sizeof modes / sizeof modes[0]))

static int fits(uint32_t w, uint32_t h) {
    return (uint64_t)w * h * 4 <= g_vram_bytes;
}

static int bochs_mode_count(void) { return N_MODES; }
static int bochs_mode_current(struct fb_mode* out) {
    if (!out) return -1;
    out->w = (uint16_t)g_w; out->h = (uint16_t)g_h; out->bpp = 32;
    return 0;
}
static int bochs_mode_get(int i, struct fb_mode* out) {
    if (!out || i < 0 || i >= N_MODES) return -1;
    *out = modes[i];
    return 0;
}

/* Same order as every backend here: build the new shadow FIRST, switch, and
 * only then free the old — a failure before the switch leaves the picture as
 * it was. */
static int bochs_mode_set(uint32_t w, uint32_t h, uint32_t bpp) {
    if (!g_ready) return -2;
    if (bpp != 32) return -3;
    if (w < 320 || h < 200 || w > 4096 || h > 4096) return -4;
    if (!fits(w, h)) return -5;
    if (w == g_w && h == g_h) return 0;
    uint32_t pitch = w * 4;
    uint32_t nframes = (uint32_t)(((uint64_t)pitch * h + 4095) / 4096);
    uint64_t ns = pmm_alloc_contiguous(nframes);
    if (ns == PMM_ALLOC_FAIL) return -6;
    uint8_t* p = (uint8_t*)phys_to_virt(ns);
    for (uint64_t i = 0; i < (uint64_t)nframes * 4096; i++) p[i] = 0;
    if (dispi_set(w, h) != 0) {
        dispi_set(g_w, g_h);                    /* put the old mode back */
        pmm_free_contiguous(ns, nframes);
        kprintf("bochs-display: %ux%u refused by the device - staying at %ux%u\n",
                w, h, g_w, g_h);
        return -7;
    }
    uint64_t old = g_shadow_phys;
    uint32_t oldn = g_shadow_frames;
    g_shadow_phys = ns; g_shadow_frames = nframes;
    g_w = w; g_h = h; g_pitch = pitch;
    fb_adopt_mode((volatile uint32_t*)phys_to_virt(ns), w, h, pitch);
    pmm_free_contiguous(old, oldn);
    kprintf("bochs-display: mode %ux%u\n", w, h);
    return 0;
}

static const struct fb_backend bochs_backend = {
    .name = "bochs-display", .flush = bochs_flush,
    .mode_count = bochs_mode_count, .mode_get = bochs_mode_get,
    .mode_current = bochs_mode_current, .mode_set = bochs_mode_set,
};

int bochs_display_init(void) {
    struct pci_device pd;
    if (pci_find_device(BOCHS_VENDOR, BOCHS_DEVICE, &pd) != 0) return -1;
    if (fb_backend_active()) return -1;          /* a display already won */

    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND, cmd | PCI_CMD_MEM_SPACE);

    uint64_t fb_pa = bar_addr(&pd, 0);
    uint64_t io_pa = bar_addr(&pd, 2);
    if (!fb_pa || !io_pa) {
        kprintf("bochs-display: BARs not assigned (fb %p, regs %p)\n",
                (void*)(uintptr_t)fb_pa, (void*)(uintptr_t)io_pa);
        return -1;
    }
    g_vram = (volatile uint32_t*)map_bar(fb_pa);
    g_regs = (volatile uint16_t*)((volatile uint8_t*)map_bar(io_pa) + DISPI_MMIO_BASE);

    uint16_t id = rd(DISPI_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        kprintf("bochs-display: DISPI id %x is not a Bochs VBE\n", id);
        return -1;
    }
    g_vram_bytes = (uint64_t)rd(DISPI_VIDEO_MEM64K) * 65536u;
    if (!g_vram_bytes) g_vram_bytes = 16u << 20;

    g_w = FB_W_DEFAULT; g_h = FB_H_DEFAULT; g_pitch = g_w * 4;
    g_shadow_frames = (g_pitch * g_h + 4095) / 4096;
    g_shadow_phys = pmm_alloc_contiguous(g_shadow_frames);
    if (g_shadow_phys == PMM_ALLOC_FAIL) {
        kprintf("bochs-display: no RAM for a %ux%u shadow\n", g_w, g_h);
        return -1;
    }
    if (dispi_set(g_w, g_h) != 0) {
        kprintf("bochs-display: the device refused %ux%u\n", g_w, g_h);
        pmm_free_contiguous(g_shadow_phys, g_shadow_frames);
        return -1;
    }
    g_ready = 1;
    g_primary_bdf = (pd.bus << 8) | (pd.slot << 3) | pd.func;
    fb_backend_register(&bochs_backend);
    kprintf("bochs-display: %ux%u at PCI %u:%u.%u, fb %p (%u MiB VRAM), regs %p\n",
            g_w, g_h, pd.bus, pd.slot, pd.func, (void*)(uintptr_t)fb_pa,
            (unsigned)(g_vram_bytes >> 20), (void*)(uintptr_t)io_pa);
    /* The console clears the shadow and flushes it — which is what paints the
     * first frame into VRAM. */
    return fb_term_init_direct(g_shadow_phys, g_w, g_h, g_pitch);
}
