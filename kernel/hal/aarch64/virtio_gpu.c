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
#include "pmm.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* fb_terminal.c — bring the portable console up from explicit geometry. */
extern int fb_term_init_direct(uint64_t phys, uint32_t width, uint32_t height,
                               uint32_t pitch_bytes);

/* ---- MMIO transport map (QEMU `virt`, shared with virtio_mmio_blk.c) ------- */
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200
#define VIRTIO_MMIO_SLOTS   32

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
    VIRTIO_GPU_CMD_SET_SCANOUT           = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH        = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
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

/* ---- MMIO + barrier helpers ------------------------------------------------ */
static inline void     w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_base + off) = v; }
static inline uint32_t r32(uint32_t off)             { return *(volatile uint32_t*)(g_base + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* Submit the command currently staged in g_cmd (cmd_len bytes) and poll for
 * completion.  Returns the response type (VIRTIO_GPU_RESP_OK_NODATA on ok). */
static uint32_t gpu_submit(uint32_t cmd_len) {
    q_desc[0].addr  = (uint64_t)(uintptr_t)g_cmd;
    q_desc[0].len   = cmd_len;
    q_desc[0].flags = VRING_DESC_F_NEXT;
    q_desc[0].next  = 1;

    q_desc[1].addr  = (uint64_t)(uintptr_t)&g_resp;
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
int fb_present_map(uint64_t phys, uint64_t size) {
    (void)phys; (void)size;
    return 0;
}

/* Bochs-VBE double-buffer page flip (compositor).  virtio-gpu has no such
 * hardware pan, so report "unavailable" — gui.c then keeps its single-buffer
 * blit, which on ARM is followed by fb_present_flush() to push the dirty rect
 * to the scanout.  These stubs let the portable gui.c link on aarch64. */
int fb_flip_init(volatile uint32_t** buf0, volatile uint32_t** buf1) {
    (void)buf0; (void)buf1;
    return -1;
}
void fb_flip_to(int idx) { (void)idx; }

/* Copy a dirty rect out of guest RAM into the host resource, then present it.
 * fb_terminal calls this after every render primitive.  Rects are clamped to
 * the framebuffer; a degenerate rect is ignored. */
void fb_present_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_ready) return;
    if (x >= FB_WIDTH || y >= FB_HEIGHT || w == 0 || h == 0) return;
    if (x + w > FB_WIDTH)  w = FB_WIDTH  - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;

    dsb();   /* make the CPU's pixel writes visible to the device's DMA read */

    struct virtio_gpu_transfer_to_host_2d* t = (void*)g_cmd;
    t->hdr = (struct virtio_gpu_ctrl_hdr){ .type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D };
    t->r = (struct virtio_gpu_rect){ .x = x, .y = y, .width = w, .height = h };
    t->offset = (uint64_t)y * FB_PITCH + (uint64_t)x * FB_BPP;
    t->resource_id = GPU_RESOURCE_ID;
    t->padding = 0;
    gpu_submit(sizeof *t);

    struct virtio_gpu_resource_flush* f = (void*)g_cmd;
    f->hdr = (struct virtio_gpu_ctrl_hdr){ .type = VIRTIO_GPU_CMD_RESOURCE_FLUSH };
    f->r = (struct virtio_gpu_rect){ .x = x, .y = y, .width = w, .height = h };
    f->resource_id = GPU_RESOURCE_ID;
    f->padding = 0;
    gpu_submit(sizeof *f);
}

/* ---- bring-up -------------------------------------------------------------- */

static int gpu_transport_init(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
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
    uint64_t d = (uint64_t)(uintptr_t)q_desc;
    uint64_t a = (uint64_t)(uintptr_t)&q_avail;
    uint64_t u = (uint64_t)(uintptr_t)&q_used;
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
    g_fb_phys = pmm_alloc_contiguous(nframes);
    if (g_fb_phys == PMM_ALLOC_FAIL) { kprintf("virtio-gpu: FB alloc (%u frames) failed\n", nframes); return -1; }

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
    kprintf("virtio-gpu: %dx%d scanout up, FB @ %p (%u frames) at slot base %p\n",
            FB_WIDTH, FB_HEIGHT, (void*)(uintptr_t)g_fb_phys, nframes, (void*)g_base);

    /* Hand the framebuffer to the portable console.  fb_term_init_direct fills
     * the buffer with the background colour, which its fb_present_flush pushes
     * to the scanout — so the screen clears to the console background here. */
    return fb_term_init_direct(g_fb_phys, FB_WIDTH, FB_HEIGHT, FB_PITCH);
}
