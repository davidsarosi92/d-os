/* =============================================================================
 * virtio_gpu.c — virtio-gpu (2D) over the virtio-MMIO transport (M21 Phase I).
 *
 * The ARM proof that "the framebuffer is a device too": QEMU's `virt` board has
 * no VGA/Bochs-VBE and no linear-VRAM BAR — the display is a virtio-gpu device
 * on one of the 32 virtio-MMIO transport slots (a `-device virtio-gpu-device`).
 * Unlike a plain framebuffer, virtio-gpu is a COMMAND device: the guest owns an
 * ordinary RAM buffer, tells the host to treat it as the backing store of a 2D
 * resource, binds that resource to a scanout, and then — for every update —
 * asks the host to copy the dirty rect out of guest RAM (TRANSFER_TO_HOST_2D)
 * and present it (RESOURCE_FLUSH).  That is exactly what the fb_present backend
 * interface abstracts, so the portable fb_terminal renderer (kernel/drivers/
 * terminal/fb_terminal.c) runs here unchanged: it writes pixels into the RAM
 * buffer and calls fb_present_flush() for each region it touched.
 *
 * Transport: virtio-MMIO **version 2** (modern), same handshake + split
 * virtqueue mechanics as the Phase-F block driver (virtio_mmio_blk.c) — only
 * the device type (16 = GPU) and the command set differ.  Completion is POLLED
 * (spin on used->idx); DMA on QEMU is coherent with the CPU caches, so no cache
 * maintenance is needed around the shared rings or the framebuffer.
 *
 * Bring-up sequence (all on the control queue, queue 0):
 *   1. RESOURCE_CREATE_2D    — a host-side 2D resource (id 1, B8G8R8X8, WxH).
 *   2. RESOURCE_ATTACH_BACKING — point it at our contiguous RAM framebuffer.
 *   3. SET_SCANOUT           — bind the resource to scanout 0.
 *   4. TRANSFER + FLUSH      — thereafter, on every fb_present_flush().
 * ============================================================================= */

#include "fb_present.h"
#include "fb_backend.h"
#include "pmm.h"
#include "printf.h"
#include "hal_api.h"
#include "display.h"
#include "settings.h"
#include "board.h"   /* §M85 — virtio slots from the device tree */   /* kptr_phys — device addresses are PHYSICAL (§M86) */
#include <stdint.h>
#include <stddef.h>

/* fb_terminal.c — bring the portable console up from explicit geometry. */
extern int fb_term_init_direct(uint64_t phys, uint32_t width, uint32_t height,
                               uint32_t pitch_bytes);

/* ---- MMIO transport map (QEMU `virt`, shared with virtio_mmio_blk.c) ------- */

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

#define ST_ACK          1
#define ST_DRIVER       2
#define ST_DRIVER_OK    4
#define ST_FEATURES_OK  8

#define VIRTIO_MAGIC            0x74726976u
#define VIRTIO_DEVID_GPU        16
#define VIRTIO_F_VERSION_1_BIT  0            /* feature bit 32 → sel=1, bit 0 */

/* ---- split virtqueue (control queue 0), same layout as the block driver ---- */
#define QSIZE   8

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QSIZE]; uint16_t used_event; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZE]; uint16_t avail_event; } __attribute__((packed));

#define VRING_DESC_F_NEXT   0x01
#define VRING_DESC_F_WRITE  0x02

static struct virtq_desc  q_desc[QSIZE] __attribute__((aligned(16)));
static struct virtq_avail q_avail       __attribute__((aligned(16)));
static struct virtq_used  q_used        __attribute__((aligned(16)));

/* ---- virtio-gpu 2D command protocol --------------------------------------- */
enum {
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF        = 0x0102,   /* §M61 — mode change */
    VIRTIO_GPU_CMD_SET_SCANOUT           = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH        = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,   /* §M61 */
    VIRTIO_GPU_RESP_OK_NODATA            = 0x1100,
};

/* B8G8R8X8: in a little-endian 32-bit word that is 0xXXRRGGBB — exactly how
 * fb_terminal packs its colours (0x00RRGGBB), so pixels display as-is. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  2
#define GPU_RESOURCE_ID   1
#define GPU_SCANOUT_ID    0

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect { uint32_t x, y, width, height; } __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_mem_entry { uint64_t addr; uint32_t length; uint32_t padding; } __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    struct virtio_gpu_mem_entry entry;   /* single contiguous backing region */
} __attribute__((packed));

/* §M61 — both take just an id: "stop using this resource's memory" and "the
 * resource is gone".  Sent in that order, because between them the device
 * still owns a pointer into RAM we are about to free. */
struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* DMA command + response buffers (device-readable / device-writable).  Big
 * enough for the largest command (transfer_to_host_2d = 56 bytes). */
static uint8_t g_cmd[64]  __attribute__((aligned(16)));
static struct virtio_gpu_ctrl_hdr g_resp __attribute__((aligned(16)));

/* ---- device state ---------------------------------------------------------- */
#define FB_WIDTH   1280
#define FB_HEIGHT  800
#define FB_BPP     4
#define FB_PITCH   (FB_WIDTH * FB_BPP)

static uintptr_t g_base;                 /* MMIO transport base, 0 = absent    */
static uint16_t  g_last_used;
static uint64_t  g_fb_phys;              /* contiguous RAM framebuffer         */
static int       g_ready;

/* §M61 — the geometry is RUNTIME state now, not three #defines.  FB_WIDTH /
 * FB_HEIGHT remain the boot mode; everything after bring-up reads these. */
static uint32_t  g_w = FB_WIDTH, g_h = FB_HEIGHT;
static uint32_t  g_pitch  = FB_PITCH;
static uint32_t  g_frames;               /* frames backing g_fb_phys           */
static uint32_t  g_res_id = GPU_RESOURCE_ID;

/* ---- MMIO + barrier helpers ------------------------------------------------ */
static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_base + off) = v; }
static inline uint32_t r32(uint32_t off)             { return *(volatile uint32_t*)(g_base + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* Submit the command currently staged in g_cmd (cmd_len bytes) and poll for
 * completion.  Returns the response type (VIRTIO_GPU_RESP_OK_NODATA on ok). */
static uint32_t gpu_submit(uint32_t cmd_len) {
    q_desc[0].addr  = kptr_phys(g_cmd);
    q_desc[0].len   = cmd_len;
    q_desc[0].flags = VRING_DESC_F_NEXT;
    q_desc[0].next  = 1;

    q_desc[1].addr  = kptr_phys(&g_resp);
    q_desc[1].len   = sizeof g_resp;
    q_desc[1].flags = VRING_DESC_F_WRITE;
    q_desc[1].next  = 0;

    g_resp.type = 0;

    uint16_t ai = q_avail.idx;
    q_avail.ring[ai % QSIZE] = 0;
    dsb();
    q_avail.idx = ai + 1;
    dsb();

    w32(R_QUEUENOTIFY, 0);

    while (*(volatile uint16_t*)&q_used.idx == g_last_used) dsb();
    g_last_used++;
    dsb();

    if (r32(R_INTSTATUS) & 1) w32(R_INTACK, 1);
    return g_resp.type;
}

/* ---- fb_present backend implementation ------------------------------------- */

/* The framebuffer is ordinary RAM the boot page tables already map Normal-WB,
 * so there is nothing to do — virt == phys and it is writable. */

/* Bochs-VBE double-buffer page flip (compositor).  virtio-gpu has no such
 * hardware pan, so report "unavailable" — gui.c then keeps its single-buffer
 * blit, which on ARM is followed by fb_present_flush() to push the dirty rect
 * to the scanout.  These stubs let the portable gui.c link on aarch64. */
/* (map / page flip: fb_present.c answers for every backend — §M85.) */

/* ==========================================================================
 * §M61 — MODE SETTING on virtio-gpu.
 *
 * The x86 backend changes mode with four register writes.  Here it is a
 * sequence of device commands and one allocation, and the allocation is what
 * makes it interesting: the framebuffer must be CONTIGUOUS guest RAM, so a
 * bigger mode is a fresh buddy block (whose maximum order is the ceiling M22.6
 * met at 1920x1200 = 9.2 MiB), and every step can fail with the display
 * half-configured.
 *
 * THE RULE THAT SHAPES THE CODE — the same one x86 follows: build the NEW
 * everything first, switch the scanout in one command, and only then take the
 * old one apart.  A failure anywhere before the switch leaves the display
 * exactly as it was, which is the only acceptable outcome for an operation
 * whose failure mode is "no picture".
 *
 * The resource id ALTERNATES between two values rather than being reused: the
 * old resource is still bound to the scanout while the new one is being built,
 * and a device that is asked to create a resource with a live id is entitled
 * to refuse.
 * ========================================================================== */

static const struct fb_mode gpu_modes[] = {
    {  640,  480, 32 }, {  800,  600, 32 }, { 1024,  768, 32 },
    { 1280,  720, 32 }, { 1280,  800, 32 }, { 1280, 1024, 32 },
    { 1440,  900, 32 }, { 1600,  900, 32 }, { 1680, 1050, 32 },
    { 1920, 1080, 32 }, { 1920, 1200, 32 },
};
#define N_GPU_MODES ((int)(sizeof gpu_modes / sizeof gpu_modes[0]))

static int vgpu_mode_count(void) {
    /* Without a device there is exactly one mode: whatever we booted with.
     * Callers read a count of 1 as "this display cannot be asked to change"
     * (fb_present.h), which is still the honest answer here. */
    return g_ready ? N_GPU_MODES : 1;
}

static int vgpu_mode_current(struct fb_mode* out) {
    if (!out) return -1;
    out->w = (uint16_t)g_w; out->h = (uint16_t)g_h; out->bpp = 32;
    return 0;
}

static int vgpu_mode_get(int index, struct fb_mode* out) {
    if (!out) return -1;
    if (!g_ready) return index == 0 ? vgpu_mode_current(out) : -1;
    if (index < 0 || index >= N_GPU_MODES) return -1;
    *out = gpu_modes[index];
    return 0;
}

/* ONE SCANOUT'S STATE — the primary's and (§M88) the second output's are two
 * instances of it, driven by ONE switch routine: a second copy of the
 * build-then-switch-then-tear-down sequence would be a second chance to get
 * its ordering wrong. */
struct vscan {
    uint32_t scanout;              /* the device's scanout index            */
    uint32_t id_a, id_b;           /* the two resource ids it alternates     */
    uint32_t res_id;               /* 0 = nothing bound yet                  */
    uint64_t fb_phys;              /* PMM_ALLOC_FAIL = none                  */
    uint32_t frames;
    uint32_t w, h, pitch;
};

/* Build a w x h framebuffer + resource and switch `sc` to it; the old one is
 * taken apart only after the switch.  0 on success; on failure nothing that
 * was showing has changed. */
static int vgpu_scanout_switch(struct vscan* sc, uint32_t w, uint32_t h) {
    uint32_t pitch   = w * FB_BPP;
    uint64_t bytes   = (uint64_t)pitch * h;
    uint32_t nframes = (uint32_t)((bytes + 4095) / 4096);

    /* 1. THE NEW FRAMEBUFFER FIRST.  If this fails, nothing has been touched
     *    and the display is still showing the old mode. */
    uint64_t nfb = pmm_alloc_contiguous_dma32(nframes);
    if (nfb == PMM_ALLOC_FAIL) {
        kprintf("virtio-gpu: %ux%u needs %u contiguous frames — refused "
                "(buddy order ceiling)\n", w, h, nframes);
        return -5;
    }
    {   /* a new monitor shows the desktop's dark, not whatever RAM held */
        uint32_t* px = (uint32_t*)phys_to_virt(nfb);
        for (uint64_t k = 0; k < (uint64_t)w * h; k++) px[k] = 0xFF101820u;
    }

    uint32_t new_id = (sc->res_id == sc->id_a) ? sc->id_b : sc->id_a;

    /* 2. New resource + backing.  Both can fail; both undo cleanly because the
     *    scanout still points at the old resource. */
    struct virtio_gpu_resource_create_2d* c = (void*)g_cmd;
    *c = (struct virtio_gpu_resource_create_2d){
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D },
        .resource_id = new_id,
        .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width = w, .height = h,
    };
    if (gpu_submit(sizeof *c) != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("virtio-gpu: create_2d(%ux%u) refused\n", w, h);
        pmm_free_contiguous(nfb, nframes);
        return -6;
    }

    struct virtio_gpu_resource_attach_backing* b = (void*)g_cmd;
    *b = (struct virtio_gpu_resource_attach_backing){
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING },
        .resource_id = new_id, .nr_entries = 1,
        .entry = { .addr = nfb, .length = (uint32_t)bytes },
    };
    if (gpu_submit(sizeof *b) != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("virtio-gpu: attach_backing(%ux%u) refused\n", w, h);
        struct virtio_gpu_resource_unref* u = (void*)g_cmd;
        *u = (struct virtio_gpu_resource_unref){
            .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_UNREF }, .resource_id = new_id };
        gpu_submit(sizeof *u);
        pmm_free_contiguous(nfb, nframes);
        return -7;
    }

    /* 3. THE SWITCH — one command, and the point of no return. */
    struct virtio_gpu_set_scanout* ss = (void*)g_cmd;
    *ss = (struct virtio_gpu_set_scanout){
        .hdr = { .type = VIRTIO_GPU_CMD_SET_SCANOUT },
        .r = { .x = 0, .y = 0, .width = w, .height = h },
        .scanout_id = sc->scanout, .resource_id = new_id,
    };
    if (gpu_submit(sizeof *ss) != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("virtio-gpu: set_scanout %u (%ux%u) refused — staying at %ux%u\n",
                sc->scanout, w, h, sc->w, sc->h);
        struct virtio_gpu_resource_unref* u = (void*)g_cmd;
        *u = (struct virtio_gpu_resource_unref){
            .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_UNREF }, .resource_id = new_id };
        gpu_submit(sizeof *u);
        pmm_free_contiguous(nfb, nframes);
        return -8;
    }

    /* 4. Only now is the old one dead.  Detach before unref: the device is
     *    holding a pointer into RAM we are about to hand back to the
     *    allocator, and the order is how it learns to stop. */
    uint64_t old_fb     = sc->fb_phys;
    uint32_t old_frames = sc->frames;
    uint32_t old_id     = sc->res_id;
    if (old_id) {
        struct virtio_gpu_resource_detach_backing* d = (void*)g_cmd;
        *d = (struct virtio_gpu_resource_detach_backing){
            .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING },
            .resource_id = old_id };
        gpu_submit(sizeof *d);
        struct virtio_gpu_resource_unref* u = (void*)g_cmd;
        *u = (struct virtio_gpu_resource_unref){
            .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_UNREF }, .resource_id = old_id };
        gpu_submit(sizeof *u);
    }

    sc->fb_phys = nfb;
    sc->frames  = nframes;
    sc->res_id  = new_id;
    sc->w = w; sc->h = h; sc->pitch = pitch;
    if (old_id && old_fb != PMM_ALLOC_FAIL && old_frames)
        pmm_free_contiguous(old_fb, old_frames);
    return 0;
}

static struct vscan g_prim_scan = { .scanout = GPU_SCANOUT_ID,
    .id_a = GPU_RESOURCE_ID, .id_b = GPU_RESOURCE_ID + 1 };

static int vgpu_mode_set(uint32_t w, uint32_t h, uint32_t bpp) {
    if (!g_ready) return -2;                 /* no device — cannot change     */
    if (bpp != 32) return -3;                /* one pixel format on purpose   */
    if (w < 320 || h < 200 || w > 4096 || h > 4096) return -4;
    if (w == g_w && h == g_h) return 0;      /* already there                 */

    g_prim_scan.res_id = g_res_id; g_prim_scan.fb_phys = g_fb_phys;
    g_prim_scan.frames = g_frames; g_prim_scan.w = g_w; g_prim_scan.h = g_h;
    int rc = vgpu_scanout_switch(&g_prim_scan, w, h);
    if (rc) return rc;
    g_fb_phys = g_prim_scan.fb_phys;
    g_frames  = g_prim_scan.frames;
    g_res_id  = g_prim_scan.res_id;
    g_w = w; g_h = h; g_pitch = g_prim_scan.pitch;

    /* The console + GUI read the geometry from fb_terminal, so tell it before
     * anything draws — a renderer using the old pitch writes diagonal stripes,
     * which looks like a device bug and is arithmetic. */
    fb_adopt_mode((volatile uint32_t*)phys_to_virt(g_fb_phys), w, h, g_pitch);
    kprintf("virtio-gpu: mode %ux%u (resource %u, %u frames)\n",
            w, h, g_res_id, g_frames);
    return 0;
}

/* ==========================================================================
 * §M88 rung (2026-09-29) — THE DEVICE'S SECOND SCANOUT AS A SECOND MONITOR.
 *
 * QEMU's virtio-gpu can drive several heads (`max_outputs=N`); the config
 * space says how many (`num_scanouts`, offset 8).  Scanout 1 becomes the
 * display output "virtio1": its own RAM framebuffer (the compositor draws
 * straight into it — this memory is ordinary RAM, not Device memory, so no
 * shadow is needed) and a flush that is the primary's transfer + resource
 * flush with its own resource id.  Not asked for when there is one head.
 * ========================================================================== */
static struct vscan g_sec_scan = { .scanout = 1, .id_a = 10, .id_b = 11,
                                   .fb_phys = PMM_ALLOC_FAIL };
static struct display_output g_sec_out;

static void vgpu_rect_push(uint32_t res, uint32_t pitch, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h) {
    dsb();   /* make the CPU's pixel writes visible to the device's DMA read */
    struct virtio_gpu_transfer_to_host_2d* t = (void*)g_cmd;
    t->hdr = (struct virtio_gpu_ctrl_hdr){ .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D };
    t->r = (struct virtio_gpu_rect){ .x = x, .y = y, .width = w, .height = h };
    t->offset = (uint64_t)y * pitch + (uint64_t)x * FB_BPP;
    t->resource_id = res;
    t->padding = 0;
    gpu_submit(sizeof *t);

    struct virtio_gpu_resource_flush* f = (void*)g_cmd;
    f->hdr = (struct virtio_gpu_ctrl_hdr){ .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH };
    f->r = (struct virtio_gpu_rect){ .x = x, .y = y, .width = w, .height = h };
    f->resource_id = res;
    f->padding = 0;
    gpu_submit(sizeof *f);
}

static void vsec_flush(struct display_output* o, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > o->w) w = o->w - x;
    if (y + h > o->h) h = o->h - y;
    if (w <= 0 || h <= 0) return;
    vgpu_rect_push(g_sec_scan.res_id, g_sec_scan.pitch, (uint32_t)x, (uint32_t)y,
                   (uint32_t)w, (uint32_t)h);
}

static int vsec_set_mode(struct display_output* o, int w, int h) {
    if (w < 320 || h < 200 || w > 4096 || h > 4096) return -1;
    if (vgpu_scanout_switch(&g_sec_scan, (uint32_t)w, (uint32_t)h) != 0) return -1;
    o->px = (uint32_t*)phys_to_virt(g_sec_scan.fb_phys);
    o->w = w; o->h = h; o->stride = w;
    vsec_flush(o, 0, 0, w, h);
    return 0;
}

static void vgpu_second_scanout(void) {
    uint32_t n = r32(0x100 + 8);                  /* config.num_scanouts */
    if (n < 2) return;
    if (vgpu_scanout_switch(&g_sec_scan, FB_WIDTH, FB_HEIGHT) != 0) {
        kprintf("virtio-gpu: %u scanouts, but the second could not be set up\n", n);
        return;
    }
    g_sec_out = (struct display_output){
        .w = (int)g_sec_scan.w, .h = (int)g_sec_scan.h, .stride = (int)g_sec_scan.w,
        .px = (uint32_t*)phys_to_virt(g_sec_scan.fb_phys),
        .flush = vsec_flush, .set_mode = vsec_set_mode };
    const char* nm = "virtio1";
    for (int i = 0; nm[i]; i++) g_sec_out.name[i] = nm[i];
    vsec_flush(&g_sec_out, 0, 0, g_sec_out.w, g_sec_out.h);
    if (display_register(&g_sec_out) >= 0)
        kprintf("virtio-gpu: scanout 1 is a second monitor, %ux%u (%u scanouts)\n",
                g_sec_scan.w, g_sec_scan.h, n);
}

CONFIG_KEY(ck_virtio1_mode) = {
    .key = "display.virtio1.mode", .group = "Monitors", .type = CFG_ENUM,
    .values = "1280x800 1024x768 1600x900 1920x1080 1920x1200", .def = "1280x800",
    .help = "the second monitor's resolution",
};
CONFIG_KEY(ck_virtio1_pos) = {
    .key = "display.virtio1.position", .group = "Monitors", .type = CFG_ENUM,
    .values = "right left below above off", .def = "right",
    .help = "where the second monitor sits beside the primary, or off",
};

/* Copy a dirty rect out of guest RAM into the host resource, then present it.
 * fb_terminal calls this after every render primitive.  Rects are clamped to
 * the framebuffer; a degenerate rect is ignored. */
static void vgpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_ready) return;
    if (x >= g_w || y >= g_h || w == 0 || h == 0) return;
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;

    vgpu_rect_push(g_res_id, g_pitch, x, y, w, h);
}

/* ---- bring-up -------------------------------------------------------------- */

static int gpu_transport_init(void) {
    for (int i = 0; i < board_virtio_count(); i++) {
        uintptr_t base = (uintptr_t)board_virtio_base(i);
        if (*(volatile uint32_t*)(base + R_MAGIC) != VIRTIO_MAGIC) continue;
        uint32_t ver = *(volatile uint32_t*)(base + R_VERSION);
        uint32_t dev = *(volatile uint32_t*)(base + R_DEVICEID);
        if (dev != VIRTIO_DEVID_GPU) continue;
        if (ver != 2) { kprintf("virtio-gpu: slot %d is legacy (ver %u), skipping\n", i, ver); continue; }
        g_base = base;
        break;
    }
    if (!g_base) return -1;

    /* Reset → ACK → DRIVER → negotiate VIRTIO_F_VERSION_1 → FEATURES_OK. */
    w32(R_STATUS, 0);
    w32(R_STATUS, ST_ACK);
    w32(R_STATUS, ST_ACK | ST_DRIVER);
    w32(R_DEVFEATSEL, 1); (void)r32(R_DEVFEAT);
    w32(R_DRVFEATSEL, 1); w32(R_DRVFEAT, 1u << VIRTIO_F_VERSION_1_BIT);
    w32(R_DEVFEATSEL, 0); (void)r32(R_DEVFEAT);
    w32(R_DRVFEATSEL, 0); w32(R_DRVFEAT, 0);
    w32(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(r32(R_STATUS) & ST_FEATURES_OK)) { kprintf("virtio-gpu: features rejected\n"); return -1; }

    /* Control queue 0. */
    w32(R_QUEUESEL, 0);
    if (r32(R_QUEUEREADY) != 0) { kprintf("virtio-gpu: queue busy\n"); return -1; }
    if (r32(R_QUEUENUMMAX) < QSIZE) { kprintf("virtio-gpu: QueueNumMax too small\n"); return -1; }
    w32(R_QUEUENUM, QSIZE);
    uint64_t d = kptr_phys(q_desc);
    uint64_t a = kptr_phys(&q_avail);
    uint64_t u = kptr_phys(&q_used);
    w32(R_QDESC_LO, (uint32_t)d);  w32(R_QDESC_HI, (uint32_t)(d >> 32));
    w32(R_QDRV_LO,  (uint32_t)a);  w32(R_QDRV_HI,  (uint32_t)(a >> 32));
    w32(R_QDEV_LO,  (uint32_t)u);  w32(R_QDEV_HI,  (uint32_t)(u >> 32));
    w32(R_QUEUEREADY, 1);
    w32(R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);
    return 0;
}

/* Probe the MMIO slots for a virtio-gpu, allocate a contiguous RAM
 * framebuffer, create + back + scan out a 2D resource, then hand the buffer to
 * the portable framebuffer terminal.  Returns 0 on success, -1 if no GPU is
 * present or a step failed (the caller then stays on the serial console). */
int virtio_gpu_init(void) {
    if (gpu_transport_init() != 0) return -1;

    /* Contiguous framebuffer: 1280*800*4 = 4,096,000 B = exactly 1000 frames. */
    uint32_t nframes = (FB_PITCH * FB_HEIGHT + 4095) / 4096;
    g_fb_phys = pmm_alloc_contiguous_dma32(nframes);
    if (g_fb_phys == PMM_ALLOC_FAIL) { kprintf("virtio-gpu: FB alloc (%u frames) failed\n", nframes); return -1; }
    g_frames = nframes;          /* §M61 — remembered so a mode change can free it */

    /* 1. Create the 2D resource. */
    struct virtio_gpu_resource_create_2d* c = (void*)g_cmd;
    *c = (struct virtio_gpu_resource_create_2d){
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D },
        .resource_id = GPU_RESOURCE_ID,
        .format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
        .width = FB_WIDTH, .height = FB_HEIGHT,
    };
    if (gpu_submit(sizeof *c) != VIRTIO_GPU_RESP_OK_NODATA) { kprintf("virtio-gpu: create_2d failed\n"); return -1; }

    /* 2. Attach the RAM framebuffer as the resource's backing store. */
    struct virtio_gpu_resource_attach_backing* b = (void*)g_cmd;
    *b = (struct virtio_gpu_resource_attach_backing){
        .hdr = { .type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING },
        .resource_id = GPU_RESOURCE_ID,
        .nr_entries = 1,
        .entry = { .addr = g_fb_phys, .length = FB_PITCH * FB_HEIGHT },
    };
    if (gpu_submit(sizeof *b) != VIRTIO_GPU_RESP_OK_NODATA) { kprintf("virtio-gpu: attach_backing failed\n"); return -1; }

    /* 3. Bind the resource to scanout 0. */
    struct virtio_gpu_set_scanout* s = (void*)g_cmd;
    *s = (struct virtio_gpu_set_scanout){
        .hdr = { .type = VIRTIO_GPU_CMD_SET_SCANOUT },
        .r = { .x = 0, .y = 0, .width = FB_WIDTH, .height = FB_HEIGHT },
        .scanout_id = GPU_SCANOUT_ID,
        .resource_id = GPU_RESOURCE_ID,
    };
    if (gpu_submit(sizeof *s) != VIRTIO_GPU_RESP_OK_NODATA) { kprintf("virtio-gpu: set_scanout failed\n"); return -1; }

    g_ready = 1;
    {
        /* §M85 — behind fb_present.h through the backend table, so another
         * display (sbsa-ref's bochs-display) can take the same seam. */
        static const struct fb_backend vgpu_backend = {
            .name = "virtio-gpu", .flush = vgpu_flush,
            .mode_count = vgpu_mode_count, .mode_get = vgpu_mode_get,
            .mode_current = vgpu_mode_current, .mode_set = vgpu_mode_set,
        };
        if (fb_backend_register(&vgpu_backend) != 0) {
            kprintf("virtio-gpu: another display is already active - standing aside\n");
            g_ready = 0;
            return -1;
        }
    }
    kprintf("virtio-gpu: %dx%d scanout up, FB @ %p (%u frames) at slot base %p\n",
            FB_WIDTH, FB_HEIGHT, (void*)(uintptr_t)g_fb_phys, nframes, (void*)g_base);
    vgpu_second_scanout();                        /* §M88 — a second head, if any */

    /* Hand the framebuffer to the portable console.  fb_term_init_direct fills
     * the buffer with the background colour, which its fb_present_flush pushes
     * to the scanout — so the screen clears to the console background here. */
    return fb_term_init_direct(g_fb_phys, FB_WIDTH, FB_HEIGHT, FB_PITCH);
}
