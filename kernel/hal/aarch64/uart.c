/* =============================================================================
 * uart.c — early PL011 UART console for the AArch64 port (M21).
 *
 * QEMU's `virt` machine wires a single ARM PrimeCell PL011 UART at physical
 * 0x09000000 (this is fixed in the machine's memory map and also advertised
 * in the device tree; we hard-code it for the early path, before any DTB
 * parsing exists).  QEMU routes it to the `-serial` chardev, so writing a
 * byte to the data register is exactly what appears on the serial console.
 *
 * This is the AArch64 analogue of the x86 boot.s inline `print_byte` helper:
 * a dependency-free, MMU-agnostic (identity/physical) polled console used to
 * prove bring-up before the full serial-driver + console-sink stack is wired
 * up.  It intentionally does NOT configure baud/line-control — QEMU's chardev
 * ignores those, exactly like the x86 ports rely on for COM1.
 *
 * All accesses go through volatile MMIO helpers so the compiler never elides
 * or reorders a register poke.  No port I/O exists on ARM — every device,
 * including the UART, is memory-mapped (CLAUDE.md arch-portability rule; this
 * file is arch-specific and lives under kernel/hal/aarch64/).
 * ============================================================================= */

#include <stdint.h>

/* PL011 register block base on QEMU `virt`. */
#define PL011_BASE   0x09000000UL

/* Register offsets (Arm PrimeCell PL011 TRM, DDI 0183). */
#define UART_DR      0x00      /* Data register.                              */
#define UART_FR      0x18      /* Flag register.                              */
#define UART_FR_TXFF (1u << 5) /* Transmit FIFO full.                         */
#define UART_FR_RXFE (1u << 4) /* Receive FIFO empty.                         */
#define UART_FR_BUSY (1u << 3) /* UART busy transmitting.                     */

static inline void mmio_write32(uintptr_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}
static inline uint32_t mmio_read32(uintptr_t addr) {
    return *(volatile uint32_t*)addr;
}

/* Block until the transmit FIFO can accept a byte, then push it. */
void uart_early_putc(char c) {
    while (mmio_read32(PL011_BASE + UART_FR) & UART_FR_TXFF) {
        /* spin — FIFO full */
    }
    mmio_write32(PL011_BASE + UART_DR, (uint32_t)(uint8_t)c);
}

/* NUL-terminated string, translating '\n' → CRLF for raw serial consoles. */
void uart_early_puts(const char* s) {
    for (; *s; s++) {
        if (*s == '\n') uart_early_putc('\r');
        uart_early_putc(*s);
    }
}

/* Non-blocking receive: return the next input byte, or -1 if the RX FIFO is
 * empty.  The M21 Phase-D serial shell polls this (yielding between empty
 * reads) as its keyboard-input path — the ARM analogue of keyboard_getchar.
 * (A PL011 RX interrupt would let the shell block instead of poll; deferred —
 * polling + task_yield is simple and correct, and the timer keeps preemption
 * alive underneath it.) */
int uart_early_getchar(void) {
    if (mmio_read32(PL011_BASE + UART_FR) & UART_FR_RXFE) return -1;
    return (int)(mmio_read32(PL011_BASE + UART_DR) & 0xFF);
}

/* 64-bit value as "0x…" hex — used by the early diagnostics and the
 * exception dumper before kprintf is available. */
void uart_early_puthex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    uart_early_putc('0');
    uart_early_putc('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        uart_early_putc(digits[(v >> shift) & 0xf]);
    }
}
