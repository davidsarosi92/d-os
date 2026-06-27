/* =============================================================================
 * serial.c — minimal COM1 output driver.
 *
 * The 8250/16550 UART is programmed through a set of 8-bit I/O ports.  For
 * COM1 the base is 0x3F8 and each register is accessed as an offset from
 * there:
 *
 *   +0   data (or divisor low when DLAB=1)
 *   +1   interrupt enable (or divisor high when DLAB=1)
 *   +2   FIFO control / interrupt identification
 *   +3   line control          — DLAB bit is the MSB
 *   +4   modem control
 *   +5   line status           — bit 5 = transmitter holding register empty
 *   +6   modem status
 *   +7   scratch
 *
 * We set 38400 baud, 8 data bits, no parity, 1 stop bit, FIFO on, and poll
 * the transmit-empty bit before every byte.  The driver is output-only;
 * receive could be added trivially but isn't needed yet.
 * ============================================================================= */

#include "serial.h"
#include "hal.h"
#include "console.h"
#include "module.h"
#include "devfs.h"
#include <stdint.h>
#include <stddef.h>

#define COM1       0x3F8

#define UART_DATA  (COM1 + 0)
#define UART_IER   (COM1 + 1)
#define UART_FCR   (COM1 + 2)
#define UART_LCR   (COM1 + 3)
#define UART_MCR   (COM1 + 4)
#define UART_LSR   (COM1 + 5)

#define LSR_TX_EMPTY 0x20                       /* bit 5 of line status */
#define LCR_DLAB     0x80                       /* bit 7 of line control */

void serial_init(void) {
    outb(UART_IER, 0x00);                       /* disable all interrupts */
    outb(UART_LCR, LCR_DLAB);                   /* enable divisor latch access */
    outb(UART_DATA, 0x03);                      /* divisor low  = 3 → 38400 baud */
    outb(UART_IER,  0x00);                      /* divisor high = 0 */
    outb(UART_LCR,  0x03);                      /* 8 data, no parity, 1 stop; DLAB cleared */
    outb(UART_FCR,  0xC7);                      /* enable FIFO, clear TX/RX, 14-byte trigger */
    outb(UART_MCR,  0x0B);                      /* RTS / DSR / OUT2 set */
}

void serial_putchar(char c) {
    /* Wait until the transmit holding register is empty.  A dead UART will
     * leave this bit clear forever — on QEMU that never happens; on real
     * hardware with no serial chip we'd hang here, which is why a future
     * revision should add a short timeout or probe the UART at init. */
    while ((inb(UART_LSR) & LSR_TX_EMPTY) == 0) { /* spin */ }
    outb(UART_DATA, (uint8_t)c);

    /* Convention: a bare '\n' becomes CRLF on the wire so terminals that
     * don't auto-expand render correctly. */
    if (c == '\n') {
        while ((inb(UART_LSR) & LSR_TX_EMPTY) == 0) { }
        outb(UART_DATA, '\r');
    }
}

void serial_write(const char* s) {
    while (*s) serial_putchar(*s++);
}

/* -------------------------------------------------------------------------- */
/* Console-sink registration.                                                  */
/*                                                                            */
/* Serial is a special case in init ordering: we want it active *before*       */
/* any other subsystem so early kprintf during GDT/IDT/PMM setup is visible    */
/* on the host's stdio.  So `kernel_main` calls `serial_module_init` directly  */
/* very early, in addition to the module framework picking it up later.  The   */
/* function is idempotent — second call is a no-op — so both paths are safe.   */
/*                                                                            */
/* Category is "serial" so it doesn't conflict with the mutually-exclusive    */
/* "screen" sinks (FB / VGA).                                                  */
/* -------------------------------------------------------------------------- */

static struct console_sink serial_sink = {
    .name     = "com1",
    .category = "serial",
    .putchar  = serial_putchar,
    .clear    = NULL,                       /* no concept of clearing a TTY */
    .active   = 0,
    .next     = NULL,
};
static int serial_done = 0;

/* devfs adapter — every byte written to /dev/com1 lands on the UART.
 * Reads are not implemented yet (the UART receive path isn't wired). */
static ssize_t serial_devfs_write(void* ctx, const void* buf, size_t n, uint64_t off) {
    (void)ctx; (void)off;
    const char* s = (const char*)buf;
    for (size_t i = 0; i < n; i++) serial_putchar(s[i]);
    return (ssize_t)n;
}

static struct devfs_node serial_devfs_node = {
    .name = "com1", .kind = DEVFS_CHAR,
    .read = NULL, .write = serial_devfs_write, .ioctl = NULL, .ctx = NULL,
    ._next = NULL,
};

int serial_module_init(void) {
    if (serial_done) return 0;
    serial_init();
    serial_sink.active = 1;
    console_sink_register(&serial_sink);
    devfs_register(&serial_devfs_node);     /* queued; flushed by devfs_init */
    serial_done = 1;
    return 0;
}

MODULE("com1-serial", "console", serial_module_init);


