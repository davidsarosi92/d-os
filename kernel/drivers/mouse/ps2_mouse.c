/* =============================================================================
 * ps2_mouse.c — PS/2 (8042 aux port) mouse driver, IRQ12-driven (M22).
 *
 * The 8042 controller has two device ports: the keyboard (IRQ1) and the
 * auxiliary device (IRQ12).  Bytes for the aux device are written by
 * first sending 0xD4 to the command port, then the payload byte to the
 * data port; replies (0xFA ACK etc.) come back through the data port
 * like any other input byte.
 *
 * Bring-up sequence (gentle on purpose — the keyboard driver already
 * initialised the controller, we must not disturb its state):
 *   1. Enable the aux port          (command 0xA8).
 *   2. Read the config byte         (command 0x20), set bit 1 (IRQ12
 *      enable), clear bit 5 (aux clock inhibit), write it back (0x60).
 *   3. Tell the mouse: set defaults (0xF6), enable reporting (0xF4).
 *
 * Wire format (default 3-byte packet, no wheel):
 *   byte0: YV XV YS XS 1 MB RB LB   — overflow / sign / always-1 / buttons
 *   byte1: X movement (9-bit two's complement with XS)
 *   byte2: Y movement (ditto; POSITIVE = UP on the wire, so we flip it
 *          to screen convention before handing it to the listener)
 *
 * Sync: bit 3 of byte0 is always 1.  If we're at packet offset 0 and
 * see it clear, we drop bytes until it looks like a header again —
 * that's the standard recovery for a missed byte.
 *
 * Ordering matters (bring-up): device ACKs (0xFA) MUST be consumed
 * synchronously in aux_send, and irq_install must come strictly after
 * the 0xF4 enable — otherwise a stray ACK reaches the packet
 * assembler, and 0xFA passes the bit-3 sync check (0xFA & 0x08 != 0),
 * shifting every subsequent packet by one byte.
 * ============================================================================= */

#include "mouse.h"
#include "hal.h"
#include "hal_api.h"
#include "idt.h"
#include "module.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define ST_OUT_FULL 0x01                    /* data available for us      */
#define ST_IN_FULL  0x02                    /* controller busy, don't write */
#define ST_AUX_DATA 0x20                    /* output byte is from the mouse */

/* Spin-wait helpers with a bounded loop — the controller is fast, but a
 * missing/broken device must never hang boot. */
static int wait_can_write(void) {
    for (int i = 0; i < 100000; i++)
        if ((inb(PS2_STATUS) & ST_IN_FULL) == 0) return 0;
    return -1;
}
static int wait_can_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & ST_OUT_FULL) return 0;
    return -1;
}

/* Send one byte to the AUX device (0xD4 prefix) and swallow the ACK the
 * device replies with.  Returns 0 on ACK, -1 on timeout/NAK. */
static int aux_send(uint8_t b) {
    if (wait_can_write()) return -1;
    outb(PS2_CMD, 0xD4);
    if (wait_can_write()) return -1;
    outb(PS2_DATA, b);
    if (wait_can_read()) return -1;
    return inb(PS2_DATA) == 0xFA ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* Listener + packet assembly.                                                */
/* -------------------------------------------------------------------------- */

static mouse_listener_t listener = NULL;

void mouse_set_listener(mouse_listener_t fn) { listener = fn; }

static uint8_t pkt[3];
static int     pkt_idx = 0;

static void mouse_irq(struct int_frame* f) {
    (void)f;
    /* Drain everything the controller has for us — one IRQ can cover
     * more than one buffered byte under load. */
    for (;;) {
        uint8_t st = inb(PS2_STATUS);
        if ((st & ST_OUT_FULL) == 0) break;
        uint8_t b = inb(PS2_DATA);
        if ((st & ST_AUX_DATA) == 0) continue;   /* keyboard byte — not ours */

        if (pkt_idx == 0 && (b & 0x08) == 0)
            continue;                            /* out of sync — resync on header */
        pkt[pkt_idx++] = b;
        if (pkt_idx < 3) continue;
        pkt_idx = 0;

        /* Overflow packets are garbage by definition — drop whole packet. */
        if (pkt[0] & 0xC0) continue;

        /* 9-bit two's complement: the sign bits live in byte0. */
        int dx = (int)pkt[1] - (int)((pkt[0] & 0x10) ? 0x100 : 0);
        int dy = (int)pkt[2] - (int)((pkt[0] & 0x20) ? 0x100 : 0);
        unsigned buttons = pkt[0] & 0x07;        /* LB=1 RB=2 MB=4 — matches MOUSE_BTN_* */

        if (listener) listener(dx, -dy, buttons);   /* flip: wire +Y is up */
    }
}

/* -------------------------------------------------------------------------- */
/* Init + module registration.                                                */
/* -------------------------------------------------------------------------- */

static int mouse_module_init(void) {
    /* 1. Enable the aux port itself. */
    if (wait_can_write()) { kprintf("ps2-mouse: controller timeout\n"); return 0; }
    outb(PS2_CMD, 0xA8);

    /* 2. Config byte: set IRQ12-enable (bit 1), clear aux-clock-inhibit
     *    (bit 5).  Keep everything else — the keyboard depends on it. */
    if (wait_can_write()) return 0;
    outb(PS2_CMD, 0x20);
    if (wait_can_read())  return 0;
    uint8_t cfg = inb(PS2_DATA);
    cfg |=  0x02;
    cfg &= (uint8_t)~0x20;
    if (wait_can_write()) return 0;
    outb(PS2_CMD, 0x60);
    if (wait_can_write()) return 0;
    outb(PS2_DATA, cfg);

    /* 3. Device: defaults + enable reporting.  aux_send eats the ACKs
     *    synchronously (IRQ12 is not yet installed, so no race). */
    if (aux_send(0xF6) != 0) { kprintf("ps2-mouse: no device (0xF6 NAK)\n"); return 0; }
    if (aux_send(0xF4) != 0) { kprintf("ps2-mouse: enable failed\n");        return 0; }

    irq_install(12, mouse_irq);              /* unmasks IRQ12 (+ slave cascade) */
    kprintf("ps2-mouse: aux device enabled, IRQ12 installed\n");
    return 0;
}

MODULE("ps2-mouse", "input", mouse_module_init);
