/* =============================================================================
 * serial.h — COM1 serial output, for headless debugging.
 *
 * When QEMU is launched with `-serial stdio` every byte we write to COM1
 * lands on the terminal that started QEMU, which makes boot-time debug
 * output visible even when the VGA / framebuffer terminal isn't ready
 * yet.  On real hardware a null-modem cable to another PC gives the same
 * effect.  Zero-risk otherwise: if nothing is listening on COM1 the bytes
 * simply get dropped.
 *
 * Reference: National Semiconductor 8250/16550 UART datasheet.
 * ============================================================================= */

#ifndef SERIAL_H
#define SERIAL_H

/* Program the UART for 38400 baud, 8N1, FIFO enabled.  Safe to call even
 * if no UART exists — the writes just go nowhere. */
void serial_init(void);

/* Send one byte.  Blocks briefly on a full TX FIFO. */
void serial_putchar(char c);

/* Send a NUL-terminated string. */
void serial_write(const char* s);

/* Idempotent module-style init: programs the UART AND registers the
 * serial console sink.  Called early by `kernel_main` so debug output
 * works during the rest of boot, then again by `module_init_all` (where
 * it's a no-op).  Public so kernel_main can reach it without the
 * module-framework iteration. */
int serial_module_init(void);

/* ---- receive side, for the serial command channel (core/serial_cmd.c) ----
 *
 * The two primitives an architecture supplies — COM1 on x86 (serial.c), the
 * PL011 on aarch64 (uart.c):
 *
 *   hal_serial_rx_getc()     the next received byte, or -1; never blocks.
 *   hal_serial_rx_enable(fn) arm the receive interrupt so that it calls fn
 *                            (IRQ context, with nothing drained — fn pulls
 *                            the bytes through hal_serial_rx_getc).  fn NULL
 *                            disarms.  Returns 1 when an interrupt is wired,
 *                            0 when the caller must poll.
 *
 * And the core's side of it: */
int  hal_serial_rx_getc(void);
int  hal_serial_rx_enable(void (*fn)(void));
void serial_cmd_rx_irq(void);
void serial_cmd_disable(void);   /* somebody else owns the line (ARM REPL) */

#endif
