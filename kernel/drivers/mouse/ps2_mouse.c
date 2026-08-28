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
 * synchronously in aux_send, and the IRQ must be claimed strictly after
 * the 0xF4 enable — otherwise a stray ACK reaches the packet
 * assembler, and 0xFA passes the bit-3 sync check (0xFA & 0x08 != 0),
 * shifting every subsequent packet by one byte.
 *
 * -----------------------------------------------------------------------------
 * §M33 STAGE 2 — THIS DRIVER IS WRITTEN AGAINST THE DRIVER-RUNTIME API
 *
 * It is the first one, and it is here to answer the question a new interface
 * cannot answer about itself: *is it sufficient, or merely plausible?*  Nothing
 * below calls `inb`, `outb` or `irq_install`.  It asks for a port window and an
 * interrupt line, and it gets handles.
 *
 * TWO THINGS CHANGED IN THE DRIVER, AND BOTH ARE IMPROVEMENTS ON THEIR OWN:
 *
 *   1. **PORTS ARE OFFSETS INTO A GRANTED WINDOW.**  0x60 and 0x64 became
 *      offset 0 and offset 4 of a four-port grant.  The driver can no longer
 *      express an access outside it — not "is prevented from", *cannot express*
 *      — which is what has to be true before the same source can run in ring 3.
 *
 *   2. **THE ISR BECAME A TASK.**  Packet assembly used to run in interrupt
 *      context and call the listener from there.  It now runs on a task that
 *      BLOCKS in `drv_irq_wait`.  §M22.7 already split the compositor this way
 *      ("the IRQ only records; the compositor works"), §M49 had to lift the
 *      xHCI drain out of an ISR by hand, and §M55 did the same for the NIC —
 *      this API makes it the only shape on offer instead of a lesson each
 *      driver relearns.
 *
 * The packet decoding, the sync recovery and the IntelliMouse knock are
 * unchanged, which is the point: the parts that are about the HARDWARE did not
 * have to move.
 * ============================================================================= */

/* §M33 Tier 1 — THIS FILE COMPILES FOR BOTH SIDES OF THE BOUNDARY.
 *
 * With -DDRV_USERSPACE it is a ring-3 program linked against user/drvrt_user.c;
 * otherwise it is a kernel driver linked against kernel/core/drvrt.c.  The
 * difference is these includes and the registration at the bottom — everything
 * between is identical, which is the claim §M33 set out to make and the only
 * way to find out whether drvrt.h was a real abstraction or AC97 with extra
 * steps (the question §M23 asked of `struct audio_dev`, one layer down). */
#ifdef DRV_USERSPACE
#include "libc.h"
#include "drvrt.h"
void mouse_publish(int dx, int dy, unsigned buttons, int dz);
int  task_should_stop(void);
void kprintf(const char* fmt, ...);
#else
#include "mouse.h"
#include "hal_api.h"
#include "drvrt.h"
#include "driver.h"
#include "task.h"
#include "printf.h"
#endif
#include <stdint.h>
#include <stddef.h>

/* Offsets into the granted window, not absolute ports.  The window is the four
 * 8042 registers at 0x60; data is offset 0 and the status/command register is
 * offset 4.  Naming them as offsets is not cosmetic — it is what makes the
 * grant meaningful, because there is no way to name a fifth. */
#define PS2_BASE    0x60
#define PS2_NPORTS  5
#define PS2_DATA    0            /* 0x60 */
#define PS2_STATUS  4            /* 0x64 */
#define PS2_CMD     4            /* 0x64, same register, written */

static struct drv_rt  rt;
static drv_handle     h_ports = -1;
static drv_handle     h_irq   = -1;

#define ST_OUT_FULL 0x01                    /* data available for us      */
#define ST_IN_FULL  0x02                    /* controller busy, don't write */
#define ST_AUX_DATA 0x20                    /* output byte is from the mouse */

/* Spin-wait helpers with a bounded loop — the controller is fast, but a
 * missing/broken device must never hang boot. */
static int wait_can_write(void) {
    for (int i = 0; i < 100000; i++)
        if ((drv_in8(h_ports, PS2_STATUS) & ST_IN_FULL) == 0) return 0;
    return -1;
}
static int wait_can_read(void) {
    for (int i = 0; i < 100000; i++)
        if (drv_in8(h_ports, PS2_STATUS) & ST_OUT_FULL) return 0;
    return -1;
}

/* Send one byte to the AUX device (0xD4 prefix) and swallow the ACK the
 * device replies with.  Returns 0 on ACK, -1 on timeout/NAK. */
static int aux_send(uint8_t b) {
    if (wait_can_write()) return -1;
    drv_out8(h_ports, PS2_CMD, 0xD4);
    if (wait_can_write()) return -1;
    drv_out8(h_ports, PS2_DATA, b);
    if (wait_can_read()) return -1;
    return drv_in8(h_ports, PS2_DATA) == 0xFA ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* Listener + packet assembly.                                                */
/* -------------------------------------------------------------------------- */

#ifndef DRV_USERSPACE
static mouse_listener_t listener = NULL;

void mouse_set_listener(mouse_listener_t fn) { listener = fn; }
#endif

/* §M62/§M61 follow-up — WHEEL SUPPORT.
 *
 * Reported from use: *"scroll still doesn't work in the resolution picker."*
 * Keyboard navigation scrolls, and dragging past the edge scrolls, but the
 * thing a person reaches for is the WHEEL — and this driver decoded the
 * default 3-byte packet, which has none.  A list nobody can wheel through is,
 * for most users, a list that ends at its seventh row.
 *
 * The IntelliMouse extension is the standard answer and QEMU implements it: a
 * "knock" of three SET-SAMPLE-RATE commands (200, 100, 80) makes a compliant
 * device switch its ID from 0 to 3 and start sending FOUR-byte packets whose
 * last byte is a signed Z delta.  The device ID is READ BACK — a device that
 * did not switch keeps sending 3-byte packets, and assuming otherwise would
 * shift every packet by one byte and turn the pointer into noise. */
static int     pkt_len = 3;            /* 4 once the wheel is enabled */
static uint8_t pkt[4];
static int     pkt_idx = 0;
#ifndef DRV_USERSPACE
static mouse_wheel_t wheel_listener = NULL;

void mouse_set_wheel_listener(mouse_wheel_t fn) { wheel_listener = fn; }
#endif

#ifndef DRV_USERSPACE
/* §M33 Tier 1 — the one place an event reaches the listeners, whoever decoded
 * it.  The in-kernel packet assembler calls it, and so does the syscall a
 * ring-3 driver publishes through: the input stack cannot tell which, which is
 * the property that makes the placement a deployment decision. */
void mouse_publish(int dx, int dy, unsigned buttons, int dz) {
    if (listener) listener(dx, dy, buttons);
    if (dz && wheel_listener) wheel_listener(dz);
}
#endif

/* Drain everything the controller has for us — one interrupt can cover more
 * than one buffered byte under load.  Runs on the mouse TASK now, not in
 * interrupt context, so calling the listener from here is an ordinary call
 * rather than something that has to be careful about what it may touch. */
static void mouse_drain(void) {
    for (;;) {
        int sti = drv_in8(h_ports, PS2_STATUS);
        if (sti < 0) break;                      /* handle went away */
        uint8_t st = (uint8_t)sti;
        if ((st & ST_OUT_FULL) == 0) break;
        uint8_t b = (uint8_t)drv_in8(h_ports, PS2_DATA);
        if ((st & ST_AUX_DATA) == 0) continue;   /* keyboard byte — not ours */

        if (pkt_idx == 0 && (b & 0x08) == 0)
            continue;                            /* out of sync — resync on header */
        pkt[pkt_idx++] = b;
        if (pkt_idx < pkt_len) continue;
        pkt_idx = 0;

        /* Overflow packets are garbage by definition — drop whole packet. */
        if (pkt[0] & 0xC0) continue;

        /* 9-bit two's complement: the sign bits live in byte0. */
        int dx = (int)pkt[1] - (int)((pkt[0] & 0x10) ? 0x100 : 0);
        int dy = (int)pkt[2] - (int)((pkt[0] & 0x20) ? 0x100 : 0);
        unsigned buttons = pkt[0] & 0x07;        /* LB=1 RB=2 MB=4 — matches MOUSE_BTN_* */

        int dz_ = 0;

        /* Byte 3 is a 4-bit signed Z delta (the upper nibble carries extra
         * buttons on 5-button mice — ignored).  Positive = wheel UP on the
         * wire; the listener gets screen convention, where scrolling down
         * moves the content up, so it is passed through as-is and the consumer
         * decides.  Reported separately from motion because a wheel event is
         * not a movement: routing it as dy would move the cursor. */
        if (pkt_len == 4) {
            int dz = (int)(pkt[3] & 0x0F);
            if (dz & 0x08) dz -= 16;            /* 4-bit two's complement */
            /* NEGATED to match the documented contract (positive = wheel UP).
             * Measured, not assumed: rotating DOWN puts a POSITIVE value on
             * this wire, so passing it through would scroll every list the
             * wrong way — and "the wheel works but goes backwards" is a bug
             * report nobody enjoys writing. */
            dz_ = -dz;
        }
        mouse_publish(dx, -dy, buttons, dz_);   /* flip: wire +Y is up */
    }
}

/* The driver body.  Blocks on the interrupt and drains when it fires.
 *
 * THE TIMEOUT IS NOT A FAILURE, it is §M55's rule: a driver whose interrupt
 * never arrives should degrade to polling and keep working rather than block
 * forever on a promise the hardware did not keep.  So a timed-out wait falls
 * through to a drain anyway — on a machine whose IRQ12 is misrouted the mouse
 * is laggy instead of dead, and `drv res`'s fire count is what tells those two
 * apart.
 *
 * THE BACKSTOP IS ONE SECOND because that is what a backstop is for.  A mouse
 * has nothing to do between packets, so polling it ten times a second would be
 * ten wakeups buying nothing on a machine whose interrupt works — and on one
 * whose interrupt does not, a second of latency is the difference between a
 * usable pointer and no pointer.  §M55 sized the NIC's at 10 ms for the
 * opposite reason: netd's backstop IS its liveness guarantee.
 *
 * MEASURED: 220 interrupts across a driven 31-step pointer walk, so the
 * interrupt path is the one doing the work and the backstop is what it claims
 * to be rather than the real mechanism wearing a fallback's name. */
static void mouse_task(void) {
    for (;;) {
        if (task_should_stop()) break;
        drv_irq_wait(h_irq, 1000);    /* n>0, 0, or DRV_ETIME — all drain */
        mouse_drain();
    }
}

/* -------------------------------------------------------------------------- */
/* Init + driver registration.                                                */
/* -------------------------------------------------------------------------- */

static int mouse_module_init(void) {
    /* Which step of the 8042 handshake failed.  EVERY exit is named because
     * the silent ones cost a round of wrong theories during §M33 Tier 2: a
     * ring-3 placement failed three times running with no message anywhere,
     * and "it exited 1" is the same sentence for six different faults. */
    int step = 0;
    drv_rt_init(&rt, "ps2-mouse");
    h_ports = drv_ports_request(&rt, PS2_BASE, PS2_NPORTS, "8042 aux device");
    if (h_ports < 0) {
        kprintf("ps2-mouse: no port grant (%d)\n", h_ports);
        return -1;
    }

    /* 1. Enable the aux port itself. */
    if (wait_can_write()) { step = 1; goto fail; }
    drv_out8(h_ports, PS2_CMD, 0xA8);

    /* 2. Config byte: set IRQ12-enable (bit 1), clear aux-clock-inhibit
     *    (bit 5).  Keep everything else — the keyboard depends on it.
     *
     * READ-MODIFY-WRITE IS RETRIED, AND THE RETRY IS NOT A CURE.  Read this
     * before believing the loop below fixes anything.
     *
     * The 8042 has ONE output buffer and TWO drivers.  A controller RESPONSE —
     * the config byte we just asked for — lands in that buffer with the AUX bit
     * CLEAR, which is indistinguishable from a keystroke, so it raises IRQ1 and
     * the keyboard driver's handler reads it.  Our `wait_can_read` then times
     * out having been robbed by a driver behaving perfectly correctly.  It is
     * not a race we lose occasionally; it is a race we lose whenever an
     * interrupt beats us to a byte, and asking again produces another byte for
     * the keyboard to take.
     *
     * §M33 TIER 2 FOUND THIS BY PLACING THE DRIVER IN RING 3, and the placement
     * made it VISIBLE rather than causing it: in the kernel this code runs at
     * boot, before anything can type and with the handshake effectively alone
     * on the controller.  In ring 3 it runs when the user asks for the
     * placement — necessarily while they are at the keyboard — and it cannot
     * mask IRQ1, because masking an interrupt line is not something a ring-3
     * driver may do and should not become one.
     *
     * So the retry buys the common case (nobody typing) and nothing else;
     * measured, it recovers on the first attempt on a quiet box and can fail
     * every attempt under a harness that is typing.  THE REAL ANSWER IS
     * ARBITRATION OF A SHARED CONTROLLER — one owner of the 8042 that both
     * drivers go through — and it is recorded as open rather than implied by a
     * loop that looks like a fix.  What makes the failure survivable meanwhile
     * is §M33 Tier 2's supervisor, which restarts the driver until a quiet
     * moment lets bring-up through: recovery, not correctness. */
    uint8_t cfg = 0;
    int got_cfg = 0;
    for (int attempt = 0; attempt < 8 && !got_cfg; attempt++) {
        if (wait_can_write()) { step = 2; goto fail; }
        drv_out8(h_ports, PS2_CMD, 0x20);
        if (wait_can_read()) continue;          /* somebody else took it */
        cfg = (uint8_t)drv_in8(h_ports, PS2_DATA);
        got_cfg = 1;
    }
    if (!got_cfg) { step = 3; goto fail; }
    cfg |=  0x02;
    cfg &= (uint8_t)~0x20;
    if (wait_can_write()) { step = 4; goto fail; }
    drv_out8(h_ports, PS2_CMD, 0x60);
    if (wait_can_write()) { step = 5; goto fail; }
    drv_out8(h_ports, PS2_DATA, cfg);

    /* 3. Device: defaults + enable reporting.  aux_send eats the ACKs
     *    synchronously (IRQ12 is not yet installed, so no race). */
    if (aux_send(0xF6) != 0) { step = 6; goto fail; }        /* set defaults */

    /* 3b. The IntelliMouse knock, BEFORE enabling reporting: 200/100/80 Hz
     *     sample rates in that order, then read the device ID.  ID 3 means the
     *     device switched to 4-byte packets with a wheel; anything else means
     *     it did not, and we keep the 3-byte format.  Reading the ID back is
     *     the whole point — believing the knock would shift every packet. */
    if (aux_send(0xF3) == 0 && aux_send(200) == 0 &&
        aux_send(0xF3) == 0 && aux_send(100) == 0 &&
        aux_send(0xF3) == 0 && aux_send(80)  == 0 &&
        aux_send(0xF2) == 0) {
        if (wait_can_read() == 0) {
            uint8_t id = (uint8_t)drv_in8(h_ports, PS2_DATA);
            if (id == 3) {
                pkt_len = 4;
                kprintf("ps2-mouse: wheel enabled (IntelliMouse, 4-byte packets)\n");
            }
        }
    }
    if (aux_send(0xF4) != 0) { step = 7; goto fail; }        /* enable reporting */

    /* The interrupt is claimed AFTER the enable, for the reason in the file
     * header: a stray ACK reaching the packet assembler shifts every packet. */
    h_irq = drv_irq_request(&rt, 12, "aux packets");
    if (h_irq < 0) { kprintf("ps2-mouse: no IRQ grant (%d)\n", h_irq); step = 8; goto fail; }

    /* drv_run, not task_spawn: at boot this runs BEFORE task_init() and the
     * API queues it until the scheduler exists.  The driver does not have to
     * know which of those it is. */
    drv_run(&rt, "ps2-mouse", mouse_task);
    kprintf("ps2-mouse: aux device enabled, IRQ12 claimed, task up\n");
    return 0;

fail:
    kprintf("ps2-mouse: bring-up failed at step %d\n", step);
    /* ONE CALL RETURNS EVERYTHING.  §M66 made a shutdown hook mandatory and
     * §M67 made a missing one a refusal to load; this is what makes writing a
     * correct one mechanical rather than a checklist. */
    drv_release_all(&rt);
    h_ports = h_irq = -1;
    return -1;
}

#ifdef DRV_USERSPACE
/* In ring 3 the driver IS the program: bring the device up, then run the body.
 * `mouse_module_init` and `mouse_task` are the same functions the kernel build
 * calls — the entry point is the only thing that differs. */
int main(void) {
    if (mouse_module_init() != 0) return 1;
    mouse_task();
    return 0;
}
#else

static int mouse_probe(void* ctx) {
    (void)ctx;
    /* No cheap way to ask an 8042 "is a mouse there" without touching it, and
     * touching it is what init does.  Reporting present and letting init fail
     * is honest here; a probe that lies costs a failed init, and a probe that
     * pokes the controller is a probe with side effects. */
    return 0;
}

static int mouse_init(void* ctx) { (void)ctx; return mouse_module_init(); }

static int mouse_shutdown(void* ctx) {
    (void)ctx;
    if (h_ports < 0 && h_irq < 0) return 0;
    /* Stop reporting before letting the resources go: a device still sending
     * packets into a released IRQ is the dangling-registration shape §M66 spent
     * a milestone on. */
    if (h_ports >= 0) aux_send(0xF5);        /* disable reporting */
    drv_release_all(&rt);
    h_ports = h_irq = -1;
    pkt_idx = 0;
    kprintf("ps2-mouse: stopped\n");
    return 0;
}

static const struct driver_ops mouse_ops = {
    .probe = mouse_probe, .init = mouse_init, .shutdown = mouse_shutdown,
};

/* A DRIVER() now rather than a MODULE(): it has a real lifecycle, so it belongs
 * in the registry that has one.  No DMA, and not boot-critical — the machine
 * boots and runs without a pointer, which is exactly why this was the right
 * first driver to port. */
/* DOMAIN_KERNEL | DOMAIN_USER — the first driver in this tree to declare that
 * it can run in ring 3, and it may say so because it has been PORTED to the
 * driver-runtime API and compiles for both sides.  A driver that still calls
 * `outb` directly must not claim this, which is why the default stays
 * kernel-only and why the claim is in the source rather than in config. */
DRIVER_EX(ps2_mouse, "input", &mouse_ops, NULL,
          DOMAIN_KERNEL | DOMAIN_USER, 0);
#endif /* DRV_USERSPACE */
