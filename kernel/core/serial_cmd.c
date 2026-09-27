/* =============================================================================
 * serial_cmd.c — a command channel on the serial line, independent of GUI focus.
 *
 * PORTABLE since 2026-09-25: the line discipline, ring, dispatch and policy
 * live here; an architecture supplies two primitives (serial.h's
 * hal_serial_rx_getc / hal_serial_rx_enable) — COM1 on x86 (serial.c), the
 * PL011 on aarch64 (uart.c).  aarch64's DISPLAY boot path had exactly the
 * x86 problem (its shell is on a VC, the harness can only reach the UART), and
 * its SERIAL boot path is the one place this channel must stand aside, because
 * serial_shell.c already owns the UART there (serial_cmd_disable()).
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

#include "charset.h"
#include "waitq.h"
#include "ktimer.h"
#include "task.h"
#include "service.h"
#include "config.h"
#include "settings.h"
#include "shellcmd.h"
#include "printf.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

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

static volatile int       sc_disabled;

/* Drain the UART into the ring.  Caller holds sc_wq's lock (which also means
 * IRQs are off here).  A full ring DROPS and counts — never blocks, because
 * the caller may be an interrupt. */
static void sc_pull_locked(void) {
    int budget = 64;                                 /* bounded: a stuck UART */
    int b;
    while (budget-- > 0 && (b = hal_serial_rx_getc()) >= 0) {
        if (sc_head - sc_tail >= SC_RING) { sc_dropped++; continue; }
        sc_ring[sc_head & (SC_RING - 1)] = (uint8_t)b;
        sc_head++;
    }
}

/* Called by the architecture's receive interrupt (see serial.h). */
void serial_cmd_rx_irq(void) {
    if (sc_disabled) return;
    uint32_t fl = waitq_lock(&sc_wq);
    sc_irqs++;
    sc_pull_locked();
    waitq_wake_all(&sc_wq);
    waitq_unlock(&sc_wq, fl);
}

void serial_cmd_disable(void) { sc_disabled = 1; }

static void sc_backstop(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t fl = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, fl);
}

/* Next byte, blocking.  Returns -1 when the task was asked to stop. */
static int sc_getc(void) {
    for (;;) {
        if (task_should_stop() || sc_disabled) return -1;
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
    /* Somebody else owns the line (aarch64's serial REPL): stand aside, and
     * stay parked rather than exiting — the supervisor restarts a service
     * that returns, and a crash-looping service to express "not needed here"
     * would be noise in every log. */
    if (sc_disabled) {
        while (!task_should_stop()) task_msleep(60000);
        return;
    }
    /* Arm the receive interrupt.  Returns 0 when this arch cannot deliver one;
     * the 100 ms backstop poll in sc_getc then carries the channel alone. */
    if (!hal_serial_rx_enable(serial_cmd_rx_irq))
        kprintf("serial-cmd: no receive interrupt on this machine - polling every 100 ms\n");

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
            /* §M59 — a host terminal types UTF-8; d-os text is ISO-8859-2.
             * Without this, `clip copy árvíz` over COM1 stored two bytes per
             * accented letter and every later consumer showed mojibake.  Only
             * a line that IS valid UTF-8 with a multi-byte character is
             * converted, so ASCII and raw Latin-2 pass untouched. */
            {
                int n = 0; while (line[n]) n++;
                if (charset_is_utf8_text(line, n)) {
                    char l2[SC_LINE];
                    int m = charset_utf8_to_latin2(line, n, l2, SC_LINE - 1);
                    for (int i = 0; i < m; i++) line[i] = l2[i];
                    line[m] = 0;
                }
            }
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
    hal_serial_rx_enable(NULL);
}

SERVICE("serial-cmd", serial_cmd_entry, 1, SVC_RESTART_ALWAYS);

/* `serialcmd` — the channel's own counters.  Distinguishes "the interrupt
 * works" from "the backstop poll is carrying it" (irqs 0 with commands
 * running), the same two-way report the NIC and the disk give. */
static void cmd_serialcmd(const char* args) {
    (void)args;
    kprintf("serial-cmd: %s, irqs %u, dropped %u byte(s), ring %u/%u\n",
            sc_disabled ? "standing aside (another shell owns the line)" :
            config_get_long("console.serial_commands", 1) ? "enabled" : "disabled",
            sc_irqs, sc_dropped, sc_head - sc_tail, (unsigned)SC_RING);
}
SHELL_CMD(serialcmd) = {
    "serialcmd", "", "COM1 command channel status",
    SHELL_G_SYS, cmd_serialcmd, SHELL_P_ANY };
