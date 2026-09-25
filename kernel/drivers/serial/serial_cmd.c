/* =============================================================================
 * serial_cmd.c — a command channel on COM1 (x86), independent of GUI focus.
 *
 * WHY THIS EXISTS (2026-09-25, NEXT.md #3, DOCS §4.74's standing limit).
 *
 * The x86 shell runs on a VC and reads the KEYBOARD.  Once the desktop is up and
 * a window holds focus, every keystroke goes to that window — which is correct
 * for a person and fatal for a test harness, because the harness types through
 * the same keyboard (`sendkey`).  So the states most worth checking — a panel
 * open, a dialog up, an app mid-drag — were exactly the states in which no
 * command could be issued.  The project grew a family of workarounds for it:
 * `gui.stats_ms`, `gui.ui_dump`, `gui.wheeltest`, `audit.interval_s`, all config
 * keys set BEFORE the GUI took over so something could fire without typing.
 * Each was the right answer to "measure X without typing"; none answered the
 * question underneath, which is "how does a test talk to the machine?".
 *
 * aarch64 never had the problem: its serial REPL (serial_shell.c) IS the UART.
 * This gives x86 the same property without replacing anything: a line that
 * arrives on COM1 is dispatched through the ONE command registry
 * (shell_cmd_dispatch), on its own task, whatever holds keyboard focus.  The
 * keyboard path is untouched, and the harness keeps using it by default,
 * because keyboard tests are tests OF that path.
 *
 * SHAPE (§M49/§M55's rule: waiting must not cost the same as computing):
 *
 *   - IRQ 4 (the UART's "received data available") does two things and no
 *     third: move the bytes out of the FIFO into a ring, wake the task.  It
 *     does not parse and does not dispatch — a command can sleep, allocate
 *     and take mutexes, none of which an interrupt may do.
 *   - The task blocks on a waitq.  A 100 ms backstop timer means a line that
 *     never raises its interrupt (a UART whose IRQ is not routed) costs
 *     latency, not the channel — and it also polls the FIFO itself then, so
 *     the channel works with no interrupt at all, merely slower.
 *
 * WHAT A COMMAND GETS: output goes wherever kprintf goes for a task with no VC
 * — the serial sink and klog — which is what the harness reads.  There is no
 * INPUT source: a command that asks for a line (`passwd`, a confirmation) is
 * refused by shell_read_line's "no input source" path rather than blocking on
 * a keyboard it is not attached to.  `shell_current_vc()` answers NULL, so the
 * handful of terminal commands behave as on the ARM serial REPL: `pane` says
 * there are no panes, `clear` sends the ANSI clear down the UART — and neither
 * touches the VC the person at the keyboard is using, which is what a global
 * "current VC" used to get wrong (see shellcmd.c).
 *
 * TRUST — SAID PLAINLY.  A command here runs in a SYSTEM context, like the boot
 * console and the aarch64 serial REPL: whoever holds the serial line holds the
 * machine.  That is the same trust the physical console carries today, which is
 * why the default is on.  It is also why it is a KEY (`console.serial_commands`,
 * read per line, so turning it off takes effect on the next line): the day a
 * machine has a login on its console (§M82), this channel must either require
 * the same authentication or be off, and that decision belongs to whoever
 * deploys the machine, not to this file.
 * ============================================================================= */

#include "hal.h"
#include "idt.h"
#include "waitq.h"
#include "ktimer.h"
#include "task.h"
#include "service.h"
#include "config.h"
#include "settings.h"
#include "shellcmd.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

#define COM1       0x3F8
#define UART_DATA  (COM1 + 0)
#define UART_IER   (COM1 + 1)
#define UART_LSR   (COM1 + 5)
#define LSR_DATA_READY 0x01
#define IER_RX_AVAIL   0x01
#define COM1_IRQ       4

#define SC_RING   512            /* bytes; a power of two (index masking)  */
#define SC_LINE   256            /* longest command line accepted          */

CONFIG_KEY(ck_serial_commands) = {
    .key = "console.serial_commands", .group = "System", .type = CFG_BOOL,
    .def = "1",
    .help = "execute lines arriving on COM1 as shell commands (SYSTEM context)",
};

/* Single producer (the IRQ, or the task's own backstop poll with IRQs off)
 * and single consumer (the task).  Both sides touch the ring only with IRQs
 * off on their own CPU plus the waitq lock, so there is no index race even if
 * the IRQ is delivered to a different CPU from the one the task runs on. */
static struct waitq      sc_wq = WAITQ_INIT;
static uint8_t           sc_ring[SC_RING];
static volatile uint32_t sc_head, sc_tail;          /* head = write, tail = read */
static volatile uint32_t sc_irqs, sc_dropped;

/* Drain the UART FIFO into the ring.  Caller holds sc_wq's lock (which also
 * means IRQs are off here).  A full ring DROPS and counts — never blocks,
 * because the caller may be an interrupt. */
static void sc_pull_locked(void) {
    int budget = 64;                                 /* bounded: a stuck LSR */
    while ((inb(UART_LSR) & LSR_DATA_READY) && budget-- > 0) {
        uint8_t b = inb(UART_DATA);
        if (sc_head - sc_tail >= SC_RING) { sc_dropped++; continue; }
        sc_ring[sc_head & (SC_RING - 1)] = b;
        sc_head++;
    }
}

static void sc_irq(struct int_frame* f) {
    (void)f;
    uint32_t fl = waitq_lock(&sc_wq);
    sc_irqs++;
    sc_pull_locked();
    waitq_wake_all(&sc_wq);
    waitq_unlock(&sc_wq, fl);
}

static void sc_backstop(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t fl = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, fl);
}

/* Next byte, blocking.  Returns -1 when the task was asked to stop. */
static int sc_getc(void) {
    for (;;) {
        if (task_should_stop()) return -1;
        struct ktimer t = { 0, 0, 0, 0, 0 };
        ktimer_arm_after(&t, 100000000ull, sc_backstop, &sc_wq);   /* 100 ms */
        uint32_t fl = waitq_lock(&sc_wq);
        sc_pull_locked();                      /* the no-interrupt fallback */
        if (sc_head == sc_tail) waitq_block(&sc_wq);
        sc_pull_locked();
        int c = -1;
        if (sc_head != sc_tail) {
            c = sc_ring[sc_tail & (SC_RING - 1)];
            sc_tail++;
        }
        waitq_unlock(&sc_wq, fl);
        ktimer_cancel(&t);                     /* waits out a running callback */
        if (c >= 0) return c;
    }
}

static void serial_cmd_entry(void) {
    /* Arm the receive interrupt.  OUT2 (MCR bit 3, set by serial_init) is what
     * lets the UART's interrupt reach the PIC/IOAPIC at all. */
    irq_install(COM1_IRQ, sc_irq);
    outb(UART_IER, IER_RX_AVAIL);

    char line[SC_LINE];
    int  len = 0, overflow = 0;
    for (;;) {
        int c = sc_getc();
        if (c < 0) break;
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            int too_long = overflow;
            len = 0; overflow = 0;
            if (line[0] == 0 && !too_long) continue;    /* CRLF: the LF is empty */
            if (too_long) {
                kprintf("serial-cmd: line longer than %d bytes — refused\n",
                        SC_LINE - 1);
                continue;
            }
            /* Read per line, so `setconf console.serial_commands 0` sent over
             * this very channel is the last line it runs. */
            if (!config_get_long("console.serial_commands", 1)) {
                kprintf("serial-cmd: disabled (console.serial_commands = 0) — "
                        "ignored: %s\n", line);
                continue;
            }
            /* Echo the line as ONE record, so a log reads as a transcript and
             * a harness can tell which output belongs to which command. */
            kprintf("serial-cmd> %s\n", line);
            if (!shell_cmd_dispatch(line))
                kprintf("unknown command (try 'help')\n");
            continue;
        }
        if (c == '\b' || c == 127) { if (len > 0) len--; continue; }
        if (c < 0x20) continue;                /* other control bytes: ignored */
        if (len < SC_LINE - 1) line[len++] = (char)c;
        else overflow = 1;
    }
    outb(UART_IER, 0);
    irq_uninstall(COM1_IRQ, sc_irq);
}

SERVICE("serial-cmd", serial_cmd_entry, 1, SVC_RESTART_ALWAYS);

/* `serialcmd` — the channel's own counters.  Distinguishes "the interrupt
 * works" from "the backstop poll is carrying it" (irqs 0 with commands
 * running), the same two-way report the NIC and the disk give. */
static void cmd_serialcmd(const char* args) {
    (void)args;
    kprintf("serial-cmd: %s, irqs %u, dropped %u byte(s), ring %u/%u\n",
            config_get_long("console.serial_commands", 1) ? "enabled" : "disabled",
            sc_irqs, sc_dropped, sc_head - sc_tail, (unsigned)SC_RING);
}
SHELL_CMD(serialcmd) = {
    "serialcmd", "", "COM1 command channel status",
    SHELL_G_SYS, cmd_serialcmd, SHELL_P_ANY };
