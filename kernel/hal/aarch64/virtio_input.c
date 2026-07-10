/* =============================================================================
 * virtio_input.c — virtio-input (keyboard + mouse) over virtio-MMIO (M21 Phase J).
 *
 * QEMU's `virt` board has no PS/2 controller, so keyboard + pointer input come
 * from virtio-input devices on virtio-MMIO slots (`-device virtio-keyboard-device`
 * and `-device virtio-mouse-device`).  A virtio-input device delivers Linux
 * evdev events — `struct virtio_input_event { u16 type; u16 code; u32 value; }`
 * — through its event virtqueue (queue 0), which the driver fills with
 * receive buffers the device writes into.
 *
 * This driver brings up every input device it finds, and a dedicated polling
 * task (`input_poll_task`) drains their event queues and dispatches:
 *   - keyboard  → evdev keycode → HID usage → the shared keymap → vc_kbd_push,
 *                 exactly the path ps2_keyboard.c / usb_hid.c feed on x86, so
 *                 the VC layer + GUI keyboard hooks work unchanged;
 *   - mouse     → REL_X/REL_Y deltas + button state, flushed on EV_SYN to the
 *                 mouse listener (mouse_set_listener), the same seam the GUI
 *                 compositor registers on (gui_mouse).
 *
 * Polling (not IRQ) keeps it simple and sidesteps per-slot GIC INTID wiring;
 * the task yields between drains so it never hogs a CPU.  DMA on QEMU is
 * cache-coherent, so no maintenance around the shared rings.
 * ============================================================================= */

#include "keymap.h"
#include "mouse.h"
#include "printf.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

/* vc.c — decoded chars / raw keycodes flow into the focused virtual console. */
void vc_kbd_push(char c);
int  vc_focused(void);
int  vc_raw_kbd_dispatch(uint8_t keycode, uint8_t mods);

/* ---- MMIO transport (shared layout with virtio_mmio_blk.c) ----------------- */
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200
#define VIRTIO_MMIO_SLOTS   32

#define R_MAGIC 0x000
#define R_VERSION 0x004
#define R_DEVICEID 0x008
#define R_DEVFEAT 0x010
#define R_DEVFEATSEL 0x014
#define R_DRVFEAT 0x020
#define R_DRVFEATSEL 0x024
#define R_QUEUESEL 0x030
#define R_QUEUENUMMAX 0x034
#define R_QUEUENUM 0x038
#define R_QUEUEREADY 0x044
#define R_QUEUENOTIFY 0x050
#define R_INTSTATUS 0x060
#define R_INTACK 0x064
#define R_STATUS 0x070
#define R_QDESC_LO 0x080
#define R_QDESC_HI 0x084
#define R_QDRV_LO 0x090
#define R_QDRV_HI 0x094
#define R_QDEV_LO 0x0a0
#define R_QDEV_HI 0x0a4

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8

#define VIRTIO_MAGIC 0x74726976u
#define VIRTIO_DEVID_INPUT 18
#define VIRTIO_F_VERSION_1_BIT 0

#define QSIZE 16

struct virtq_desc  { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[QSIZE]; uint16_t used_event; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used  { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[QSIZE]; uint16_t avail_event; } __attribute__((packed));

#define VRING_DESC_F_WRITE 0x02

struct virtio_input_event { uint16_t type; uint16_t code; uint32_t value; } __attribute__((packed));

/* evdev event types + codes we care about. */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define REL_X  0x00
#define REL_Y  0x01
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

/* Per-device state: one event virtqueue + its receive buffers.  `qsz` is the
 * negotiated ring size (min of QSIZE and the device's QueueNumMax) — QEMU's
 * virtio-input event queue is smaller than a block queue, so posting a fixed
 * QSIZE buffers would overrun it ("Virtqueue size exceeded"). */
struct in_dev {
    uintptr_t base;
    struct virtq_desc  desc[QSIZE] __attribute__((aligned(16)));
    struct virtq_avail avail       __attribute__((aligned(16)));
    struct virtq_used  used        __attribute__((aligned(16)));
    struct virtio_input_event ev[QSIZE] __attribute__((aligned(16)));
    uint16_t last_used;
    uint16_t qsz;
};

#define MAX_INDEV 4
static struct in_dev g_dev[MAX_INDEV];
static int g_ndev;

/* ---- MMIO helpers ---------------------------------------------------------- */
static inline void     w32(uintptr_t b, uint32_t off, uint32_t v) { *(volatile uint32_t*)(b + off) = v; }
static inline uint32_t r32(uintptr_t b, uint32_t off)             { return *(volatile uint32_t*)(b + off); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* ---- keyboard: evdev keycode → HID usage ID -------------------------------- */
/* Enough of the main + navigation keys to drive the shell and the GUI; the
 * shared keymap (keymap.c/layouts.c) then turns (usage, mods) into a char. */
static const uint8_t evdev_to_hid[128] = {
    [1]=0x29, [2]=0x1e,[3]=0x1f,[4]=0x20,[5]=0x21,[6]=0x22,[7]=0x23,[8]=0x24,[9]=0x25,[10]=0x26,[11]=0x27,
    [12]=0x2d,[13]=0x2e,[14]=0x2a,[15]=0x2b,
    [16]=0x14,[17]=0x1a,[18]=0x08,[19]=0x15,[20]=0x17,[21]=0x1c,[22]=0x18,[23]=0x0c,[24]=0x12,[25]=0x13,
    [26]=0x2f,[27]=0x30,[28]=0x28,[29]=0xe0,
    [30]=0x04,[31]=0x16,[32]=0x07,[33]=0x09,[34]=0x0a,[35]=0x0b,[36]=0x0d,[37]=0x0e,[38]=0x0f,
    [39]=0x33,[40]=0x34,[41]=0x35,[42]=0xe1,[43]=0x31,
    [44]=0x1d,[45]=0x1b,[46]=0x06,[47]=0x19,[48]=0x05,[49]=0x11,[50]=0x10,
    [51]=0x36,[52]=0x37,[53]=0x38,[54]=0xe5,[55]=0x55,[56]=0xe2,[57]=0x2c,[58]=0x39,
    [59]=0x3a,[60]=0x3b,[61]=0x3c,[62]=0x3d,[63]=0x3e,[64]=0x3f,[65]=0x40,[66]=0x41,[67]=0x42,[68]=0x43,
    [97]=0xe4,          /* KEY_RIGHTCTRL */
    [100]=0xe6,         /* KEY_RIGHTALT  */
    [102]=0x4a,[103]=0x52,[104]=0x4b,[105]=0x50,[106]=0x4f,[107]=0x4d,[108]=0x51,[109]=0x4e,
    [110]=0x49,[111]=0x4c,
};

static uint8_t g_mods;                              /* KBD_MOD_* bitmask */

/* Update g_mods for a modifier HID usage; return 1 if it was a modifier. */
static int apply_modifier(uint8_t usage, int pressed) {
    uint8_t bit = 0;
    switch (usage) {
        case 0xe0: bit = KBD_MOD_LCTRL;  break;
        case 0xe1: bit = KBD_MOD_LSHIFT; break;
        case 0xe2: bit = KBD_MOD_LALT;   break;
        case 0xe3: bit = KBD_MOD_LGUI;   break;
        case 0xe4: bit = KBD_MOD_RCTRL;  break;
        case 0xe5: bit = KBD_MOD_RSHIFT; break;
        case 0xe6: bit = KBD_MOD_RALT;   break;
        case 0xe7: bit = KBD_MOD_RGUI;   break;
        default: return 0;
    }
    if (pressed) g_mods |= bit; else g_mods &= ~bit;
    return 1;
}

static void handle_key(uint16_t code, int32_t value) {
    if (code >= 128) return;
    uint8_t usage = evdev_to_hid[code];
    if (!usage) return;
    int pressed = (value != 0);                     /* 1 press / 2 repeat; 0 release */
    if (apply_modifier(usage, pressed)) return;     /* modifier make/break only */
    if (!pressed) return;                            /* emit on press + repeat */

    /* Raw keycode dispatch first (arrows / nav → widget events, GUI Alt-Tab). */
    if (vc_raw_kbd_dispatch(usage, g_mods)) return;
    char c = keymap_translate(usage, g_mods);
    if (c && vc_focused()) vc_kbd_push(c);
}

/* ---- mouse: the listener seam the GUI registers on (mouse_set_listener) ----
 * ps2_mouse.c owns this symbol on x86; on ARM (no PS/2) this driver provides
 * it.  We accumulate a report frame and flush it to the listener on EV_SYN. */
static mouse_listener_t g_listener;
void mouse_set_listener(mouse_listener_t fn) { g_listener = fn; }

static int g_mdx, g_mdy;
static unsigned g_mbtn;

static void handle_rel(uint16_t code, int32_t value) {
    if (code == REL_X) g_mdx += value;
    else if (code == REL_Y) g_mdy += value;
}
static void handle_btn(uint16_t code, int32_t value) {
    unsigned bit = 0;
    if (code == BTN_LEFT)   bit = 1;
    else if (code == BTN_RIGHT)  bit = 2;
    else if (code == BTN_MIDDLE) bit = 4;
    else return;
    if (value) g_mbtn |= bit; else g_mbtn &= ~bit;
}
static void flush_mouse(void) {
    /* EV_SYN only follows real REL/BTN events, so a flush here always reflects
     * a change (motion and/or a button transition). */
    if (g_listener) g_listener(g_mdx, g_mdy, g_mbtn);
    g_mdx = g_mdy = 0;
}

static void dispatch_event(struct virtio_input_event* e) {
    switch (e->type) {
        case EV_KEY:
            if (e->code >= BTN_LEFT && e->code <= BTN_MIDDLE) handle_btn(e->code, e->value);
            else handle_key(e->code, e->value);
            break;
        case EV_REL: handle_rel(e->code, e->value); break;
        case EV_SYN: flush_mouse(); break;
        default: break;
    }
}

/* ---- virtqueue receive-buffer posting -------------------------------------- */
static void post_buffer(struct in_dev* d, int i) {
    d->desc[i].addr  = (uint64_t)(uintptr_t)&d->ev[i];
    d->desc[i].len   = sizeof d->ev[i];
    d->desc[i].flags = VRING_DESC_F_WRITE;          /* device writes the event */
    d->desc[i].next  = 0;
    uint16_t ai = d->avail.idx;
    d->avail.ring[ai % d->qsz] = i;
    dsb();
    d->avail.idx = ai + 1;
}

/* Drain all completed events from one device, dispatch, and re-post the buffers. */
static void drain_device(struct in_dev* d) {
    while (d->last_used != *(volatile uint16_t*)&d->used.idx) {
        dsb();
        struct virtq_used_elem* ue = &d->used.ring[d->last_used % d->qsz];
        int i = ue->id;
        dispatch_event(&d->ev[i]);
        d->last_used++;
        post_buffer(d, i);
    }
    dsb();
    w32(d->base, R_QUEUENOTIFY, 0);
    if (r32(d->base, R_INTSTATUS) & 1) w32(d->base, R_INTACK, 1);
}

/* ---- polling task ----------------------------------------------------------
 * A dedicated scheduler task drains input events and yields between passes.
 * Polling (vs. a per-slot GIC IRQ) keeps the driver simple; the task_yield
 * lets other tasks run.  (pid 0 becomes the idle task after boot, so this task
 * is not starved.) */
static void input_poll_task(void) {
    for (;;) {
        for (int i = 0; i < g_ndev; i++) drain_device(&g_dev[i]);
        task_yield();
    }
}

/* ---- per-device bring-up --------------------------------------------------- */
static int input_dev_init(struct in_dev* d, uintptr_t base) {
    d->base = base;
    w32(base, R_STATUS, 0);
    w32(base, R_STATUS, ST_ACK);
    w32(base, R_STATUS, ST_ACK | ST_DRIVER);
    w32(base, R_DEVFEATSEL, 1); (void)r32(base, R_DEVFEAT);
    w32(base, R_DRVFEATSEL, 1); w32(base, R_DRVFEAT, 1u << VIRTIO_F_VERSION_1_BIT);
    w32(base, R_DEVFEATSEL, 0); (void)r32(base, R_DEVFEAT);
    w32(base, R_DRVFEATSEL, 0); w32(base, R_DRVFEAT, 0);
    w32(base, R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK);
    if (!(r32(base, R_STATUS) & ST_FEATURES_OK)) return -1;

    w32(base, R_QUEUESEL, 0);                        /* eventq */
    if (r32(base, R_QUEUEREADY) != 0) return -1;
    uint32_t nmax = r32(base, R_QUEUENUMMAX);
    if (nmax == 0) return -1;
    d->qsz = (nmax < QSIZE) ? (uint16_t)nmax : QSIZE; /* clamp to the device's ring */
    w32(base, R_QUEUENUM, d->qsz);
    uint64_t dd = (uint64_t)(uintptr_t)d->desc;
    uint64_t aa = (uint64_t)(uintptr_t)&d->avail;
    uint64_t uu = (uint64_t)(uintptr_t)&d->used;
    w32(base, R_QDESC_LO, (uint32_t)dd); w32(base, R_QDESC_HI, (uint32_t)(dd >> 32));
    w32(base, R_QDRV_LO,  (uint32_t)aa); w32(base, R_QDRV_HI,  (uint32_t)(aa >> 32));
    w32(base, R_QDEV_LO,  (uint32_t)uu); w32(base, R_QDEV_HI,  (uint32_t)(uu >> 32));
    w32(base, R_QUEUEREADY, 1);
    w32(base, R_STATUS, ST_ACK | ST_DRIVER | ST_FEATURES_OK | ST_DRIVER_OK);

    /* Fill the event queue with receive buffers, leaving one ring slot free
     * (post qsz-1) so the avail ring is never 100% full — some QEMU virtqueue
     * paths mis-handle a completely full ring as "size exceeded". */
    d->last_used = 0;
    int nfill = (d->qsz > 1) ? d->qsz - 1 : 1;
    for (int i = 0; i < nfill; i++) post_buffer(d, i);
    dsb();
    w32(base, R_QUEUENOTIFY, 0);
    return 0;
}

/* Scan the MMIO slots for virtio-input devices, bring each up, and start the
 * polling task.  Returns the number of input devices found. */
int virtio_input_init(void) {
    for (int i = 0; i < VIRTIO_MMIO_SLOTS && g_ndev < MAX_INDEV; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + (uintptr_t)i * VIRTIO_MMIO_STRIDE;
        if (*(volatile uint32_t*)(base + R_MAGIC) != VIRTIO_MAGIC) continue;
        if (*(volatile uint32_t*)(base + R_VERSION) != 2) continue;
        if (*(volatile uint32_t*)(base + R_DEVICEID) != VIRTIO_DEVID_INPUT) continue;
        if (input_dev_init(&g_dev[g_ndev], base) == 0) {
            kprintf("virtio-input: device at slot %d (base %p)\n", i, (void*)base);
            g_ndev++;
        }
    }
    if (g_ndev > 0) task_spawn("input", input_poll_task);
    kprintf("virtio-input: %d device(s) ready\n", g_ndev);
    return g_ndev;
}
