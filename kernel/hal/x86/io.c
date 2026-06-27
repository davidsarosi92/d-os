/* =============================================================================
 * io.c — x86 port I/O primitives plus high-level power control (shutdown, reboot).
 *
 * The inline asm here is the lowest level the kernel talks to: reads and
 * writes of 8- and 16-bit values to the CPU I/O port space using the
 * `in`/`out` family of instructions.  Everything higher (keyboard driver,
 * ACPI table walker, ICH reset) eventually lands in these functions.
 *
 * Constraint reference:
 *   "a"  — value must live in %al/%ax (for in/out, those registers are fixed).
 *   "Nd" — port is either an 8-bit immediate or %dx (what `in`/`out` accept).
 * =========================================================================== */

#include "hal.h"
#include "acpi.h"
#include <stdint.h>

/* Read one byte from `port`.  `volatile` prevents the compiler from caching
 * the value or reordering the access with respect to other volatile ops —
 * essential when the port has side effects (e.g. keyboard output buffer
 * auto-advance). */
uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* Write one byte to `port`. */
void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Write one 16-bit word to `port`.  Used for ACPI PM1 control register writes
 * (the spec requires a 16-bit access) and for the legacy QEMU/Bochs shutdown
 * hacks in `hal_shutdown`. */
void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* 32-bit port I/O.  Same operand pattern as the 8/16-bit variants; the
 * "a" constraint picks %eax (the only register `in`/`out` accept for
 * the 32-bit variants), and "Nd" allows an 8-bit immediate port or %dx. */
void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* -------------------------------------------------------------------------
 * hal_reboot — reset the CPU.
 *
 * The canonical PC-compatible trick is to poke the 8042 keyboard controller
 * command port (0x64) with the "pulse output line" command (0xFE).  On a
 * real PC the keyboard controller's output line P0 bit 0 is wired to the
 * CPU RESET pin, so this triggers a full reset.  QEMU emulates the same
 * behavior.
 *
 * Before writing the command we spin until the input buffer is empty
 * (status bit 0x02 clear) — otherwise the controller may drop our byte.
 *
 * Newer chipsets (ICH/PCH) also accept a "fast reset" via PCI config port
 * 0xCF9; we write 0x06 (full reset) as a second try in case the 8042 route
 * is unavailable (some modern boards omit the PS/2 superio).  If neither
 * works we've done what we can; halt to avoid executing into garbage.
 * ------------------------------------------------------------------------- */
void hal_reboot(void) {
    while (inb(0x64) & 0x02) { /* spin: wait for input buffer empty */ }
    outb(0x64, 0xFE);

    outb(0xCF9, 0x06);                                  /* ICH fast reset fallback */

    for (;;) __asm__ volatile ("cli; hlt");
}

/* -------------------------------------------------------------------------
 * hal_shutdown — soft power off.
 *
 * Preferred path: ACPI S5 via `acpi_shutdown()`, which writes the proper
 * PM1a_CNT / PM1b_CNT values discovered from the FADT + DSDT at boot.
 * That works on both real hardware and any emulator that exposes the
 * ACPI interface (modern QEMU does).
 *
 * If `acpi_shutdown` returns — meaning ACPI wasn't available or the DSDT
 * had no parseable _S5_ — we fall through to a series of well-known
 * emulator-specific ports:
 *   QEMU >= 2.0        : outw 0x604, 0x2000   (the same bits ACPI uses, just
 *                        mapped at a legacy debug port)
 *   QEMU < 2.0         : outw 0xB004, 0x2000
 *   Bochs              : write the ASCII string "Shutdown" to port 0x8900
 *
 * On bare metal without ACPI none of those do anything, so we end with
 * `cli; hlt` forever to at least idle the CPU instead of spinning.
 * ------------------------------------------------------------------------- */
void hal_shutdown(void) {
    acpi_shutdown();                                    /* real-hardware path */

    outw(0x604,  0x2000);                               /* QEMU >= 2.0 */
    outw(0xB004, 0x2000);                               /* QEMU < 2.0 */
    const char* s = "Shutdown";                         /* Bochs */
    while (*s) outw(0x8900, (uint16_t)(uint8_t)*s++);

    for (;;) __asm__ volatile ("cli; hlt");
}
