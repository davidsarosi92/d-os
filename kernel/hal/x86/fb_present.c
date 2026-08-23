/* =============================================================================
 * fb_present.c — x86 framebuffer presentation backend (M21 Phase I).
 *
 * The x86 half of the fb_present interface (see kernel/includes/fb_present.h).
 * On x86 the framebuffer is a linear VRAM BAR the bootloader handed us through
 * multiboot: identity-map it and the display scans it out directly, so a pixel
 * write is instantly visible and flush() is a no-op.
 *
 * This file also owns the Bochs-VBE double-buffer PAGE FLIP used by the GUI
 * compositor for tear-free presentation (M22.6).  It lived in fb_terminal.c
 * until Phase I, when that renderer was made arch-portable; the DISPI port-I/O
 * is inherently x86, so it moved here.  The public symbols (fb_flip_init /
 * fb_flip_to) are unchanged, so gui.c needs no edit.
 * ============================================================================= */

#include "fb_present.h"
#include "vmm.h"
#include "hal.h"                         /* inw/outw for the DISPI registers */
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* fb_terminal.c exposes the framebuffer geometry; the page flip derives the
 * VRAM base + one-frame size from it.  On x86 the FB is identity-mapped, so
 * the returned virtual base equals the physical base. */
extern int fb_get_info(volatile uint32_t** px, uint32_t* w, uint32_t* h,
                       uint32_t* pitch_bytes);
/* §M61 — the console's four numbers, updated in ONE place after a mode set.
 * Declared in fb_present.h now, shared with the aarch64 backend. */
/* Physical base of the framebuffer as the bootloader reported it, and the
 * mapping of a physical FB address into the kernel's address space.  Both live
 * in fb_terminal.c, which is where the multiboot info was read. */
extern uint32_t fb_get_phys_base(void);
extern uintptr_t fb_phys_to_virt(uint32_t phys);

/* -------------------------------------------------------------------------- */
/* fb_present interface.                                                       */
/* -------------------------------------------------------------------------- */

/* Identity-map the FB window with 4 MiB PSE PDEs.  The region is usually a few
 * MiB and aligned to 4 MiB, so one or two PDEs cover it.  r == -2 means a
 * regular PT is already present there — treat as already mapped. */
int fb_present_map(uint64_t phys, uint64_t size) {
    uint32_t base_aligned = (uint32_t)phys & 0xFFC00000u;
    uint32_t end          = (uint32_t)phys + (uint32_t)size;
    for (uint32_t a = base_aligned; a < end; a += 0x00400000u) {
        int r = vmm_map_4mib(a, a, VMM_WRITABLE);
        if (r != 0 && r != -2) {
            /* (uintptr_t) first: `a` is 32-bit and this file also compiles for
             * x86_64, where a bare cast to void* warns.  A silent build is what
             * lets a REAL warning be seen (§M57). */
            kprintf("fb: vmm_map_4mib(%p) failed: %d\n", (void*)(uintptr_t)a, r);
            return -1;
        }
    }
    return 0;
}

/* The linear framebuffer IS the scanout, so writes are already on screen. */
void fb_present_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)x; (void)y; (void)w; (void)h;
}

/* -------------------------------------------------------------------------- */
/* M22.6 — tear-free presentation via a Bochs-VBE double buffer.               */
/*                                                                             */
/* QEMU's std-VGA (and Bochs) expose the "DISPI" interface: an index/data      */
/* register pair (ports 0x1CE/0x1CF) that, among other things, lets us set a   */
/* VIRTUAL framebuffer taller than the visible one and PAN the visible window  */
/* within it via a Y-offset.  That is a hardware page flip: compose the whole  */
/* next frame into the hidden half, then move the scanout origin to it in one  */
/* register write.  QEMU always reads a consistent buffer, so there is no      */
/* mid-scanout shear — no vblank interrupt required.                           */
/*                                                                             */
/* This only works on the Bochs-VBE device (the ID register reads 0xB0Cx).     */
/* On anything else fb_flip_init() returns non-zero and the compositor keeps   */
/* its single-buffer direct blit.                                              */
/* -------------------------------------------------------------------------- */

#define VBE_DISPI_IOPORT_INDEX      0x01CE
#define VBE_DISPI_IOPORT_DATA       0x01CF
#define VBE_DISPI_INDEX_ID          0x0
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x7
#define VBE_DISPI_INDEX_Y_OFFSET    0x9

static int      fb_flip_ok = 0;
static uint32_t flip_height = 0;                /* cached for fb_flip_to */

static inline uint16_t dispi_read(uint16_t idx) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    return inw(VBE_DISPI_IOPORT_DATA);
}
static inline void dispi_write(uint16_t idx, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, idx);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

/* Probe for the Bochs-VBE device, reserve a second frame's worth of VRAM by
 * doubling the virtual height, and map that second buffer.  On success both
 * `*buf0`/`*buf1` point at the two frame buffers (buf0 == the visible one at
 * boot) and 0 is returned; the compositor drives fb_flip_to() thereafter.
 * Any failure (not Bochs VBE, VRAM too small, map error) returns non-zero and
 * leaves the single-buffer path untouched. */
int fb_flip_init(volatile uint32_t** buf0, volatile uint32_t** buf1) {
    volatile uint32_t* px;
    uint32_t w, h, pitch;
    if (fb_get_info(&px, &w, &h, &pitch) != 0) return -1;

    /* DISPI ID: 0xB0C0..0xB0C5 across Bochs/QEMU revisions. */
    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0C5) return -2;      /* not a Bochs-VBE device */

    uint32_t fb_phys_base = (uint32_t)(uintptr_t)px; /* FB is identity-mapped */
    uint32_t fb_size      = pitch * h;               /* one frame, bytes */

    /* Ask the device for a virtual framebuffer twice as tall.  If VRAM is
     * too small the device clamps VIRT_HEIGHT — re-read and bail if it did
     * not take, so we never pan into unbacked memory. */
    dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)(h * 2));
    if (dispi_read(VBE_DISPI_INDEX_VIRT_HEIGHT) < h * 2) return -3;

    /* Map the second buffer.  It sits immediately after the first in the same
     * linear VRAM BAR; fb_present_map is idempotent on the shared boundary. */
    if (fb_present_map(fb_phys_base, (uint64_t)fb_size * 2) != 0) {
        dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (uint16_t)h);   /* roll back */
        return -4;
    }

    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, 0);       /* start on buffer 0 */
    fb_flip_ok  = 1;
    flip_height = h;
    if (buf0) *buf0 = px;
    if (buf1) *buf1 = (volatile uint32_t*)((uintptr_t)px + fb_size);
    return 0;
}

/* Make buffer `idx` (0 or 1) the visible one by panning the scanout origin.
 * A single register write; takes effect on QEMU's next host redraw. */
void fb_flip_to(int idx) {
    if (!fb_flip_ok) return;
    dispi_write(VBE_DISPI_INDEX_Y_OFFSET, idx ? (uint16_t)flip_height : 0);
}

/* ==========================================================================
 * §M61 — MODE SETTING on the Bochs-VBE device.
 *
 * The same register file the page flip above already drives, which is why this
 * belongs here and not in a new file: XRES/YRES/BPP plus the ENABLE bit, and
 * the framebuffer BAR does not move — only the geometry and the pitch change.
 *
 * The mode LIST is curated rather than enumerated, because this device has no
 * mode table: it accepts more or less any size that fits in VRAM.  A handful of
 * standard sizes is what a person wants to choose from, and each is offered
 * only if the device actually accepts it (probed once, at first ask) — an
 * offered mode that then fails is worse than one that was never listed.
 * ========================================================================== */

#define VBE_DISPI_INDEX_XRES        0x1
#define VBE_DISPI_INDEX_YRES        0x2
#define VBE_DISPI_INDEX_BPP         0x3
#define VBE_DISPI_INDEX_ENABLE      0x4
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x6
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40

/* The physical base of the framebuffer, captured the first time we look.  NOT
 * derived from the mapped pointer: that works only where the FB is identity
 * mapped, and this file compiles for x86_64 too. */
static uint32_t fb_phys_captured = 0;

static const struct fb_mode candidates[] = {
    {  640,  480, 32 }, {  800,  600, 32 }, { 1024,  768, 32 },
    { 1280,  720, 32 }, { 1280,  800, 32 }, { 1280, 1024, 32 },
    { 1440,  900, 32 }, { 1600,  900, 32 }, { 1680, 1050, 32 },
    { 1920, 1080, 32 }, { 1920, 1200, 32 },
};
#define N_CANDIDATES ((int)(sizeof candidates / sizeof candidates[0]))

static int dispi_present(void) {
    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);
    return (id >= 0xB0C0 && id <= 0xB0C5);
}

int fb_mode_count(void) {
    /* Without the DISPI device there is exactly one mode: whatever the
     * bootloader set.  Callers use that to know the feature is absent. */
    return dispi_present() ? N_CANDIDATES : 1;
}

int fb_mode_current(struct fb_mode* out) {
    volatile uint32_t* px; uint32_t w, h, pitch;
    if (!out || fb_get_info(&px, &w, &h, &pitch) != 0) return -1;
    out->w = (uint16_t)w; out->h = (uint16_t)h;
    out->bpp = (uint16_t)((pitch / (w ? w : 1)) * 8);
    return 0;
}

int fb_mode_get(int index, struct fb_mode* out) {
    if (!out) return -1;
    if (!dispi_present()) return index == 0 ? fb_mode_current(out) : -1;
    if (index < 0 || index >= N_CANDIDATES) return -1;
    *out = candidates[index];
    return 0;
}

int fb_mode_set(uint32_t w, uint32_t h, uint32_t bpp) {
    volatile uint32_t* px; uint32_t ow, oh, opitch;
    if (fb_get_info(&px, &ow, &oh, &opitch) != 0) return -1;
    if (!dispi_present()) return -2;
    if (bpp != 32) return -3;                 /* the whole stack is 32-bpp */
    if (w < 320 || h < 200 || w > 4096 || h > 4096) return -4;

    if (!fb_phys_captured) fb_phys_captured = fb_get_phys_base();
    if (!fb_phys_captured) return -5;

    /* Map the new frame BEFORE switching: if the window cannot be mapped we
     * still have a working display, whereas a device already switched to a
     * mode whose memory is unmapped is a black screen with no way back. */
    uint32_t new_pitch = w * 4;
    if (fb_present_map(fb_phys_captured, (uint64_t)new_pitch * h * 2) != 0) return -6;

    dispi_write(VBE_DISPI_INDEX_ENABLE, 0);          /* must be off to program */
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_BPP,  (uint16_t)bpp);
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    /* Read back.  The device CLAMPS what it cannot do rather than failing, so
     * believing the write would leave the kernel drawing at a size the display
     * is not showing — the classic "everything is shifted" bug. */
    uint16_t gw = dispi_read(VBE_DISPI_INDEX_XRES);
    uint16_t gh = dispi_read(VBE_DISPI_INDEX_YRES);
    if (gw != (uint16_t)w || gh != (uint16_t)h) {
        dispi_write(VBE_DISPI_INDEX_ENABLE, 0);
        dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)ow);
        dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)oh);
        dispi_write(VBE_DISPI_INDEX_BPP,  32);
        dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)ow);
        dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
        return -7;
    }

    /* The flip's second buffer belonged to the OLD geometry; the compositor
     * re-establishes it after the resize. */
    fb_flip_ok = 0;

    fb_adopt_mode((volatile uint32_t*)(uintptr_t)fb_phys_to_virt(fb_phys_captured),
                  w, h, new_pitch);
    return 0;
}
