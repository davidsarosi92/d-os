/* =============================================================================
 * io.c — x86_64 port I/O primitives plus power control (shutdown, reboot).
 *
 * Word-for-word port of the i386 version.  Port I/O is one of the few
 * places where x86 and x86_64 are truly identical: the `in`/`out`
 * opcodes have the same encodings in both modes (no REX prefix needed
 * for 8/16/32-bit accesses), the operand constraints "a" / "Nd" mean
 * the same registers, and the legacy PC port map is the same on both
 * arches.
 *
 * If/when this file diverges from its i386 sibling, the divergence is
 * almost certainly a hardware-vs-emulator quirk and should be
 * documented inline.  Today it's a straight copy.
 * =========================================================================== */

#include "hal.h"
#include "acpi.h"
#include <stdint.h>

uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* -------------------------------------------------------------------------
 * hal_reboot — same 8042 + ICH-CF9 sequence as i386.  See i386 io.c for
 * the rationale; nothing about the reboot path is mode-dependent.
 * ------------------------------------------------------------------------- */
void hal_reboot(void) {
    while (inb(0x64) & 0x02) { /* spin: wait for input buffer empty */ }
    outb(0x64, 0xFE);
    outb(0xCF9, 0x06);
    for (;;) __asm__ volatile ("cli; hlt");
}

void hal_shutdown(void) {
    acpi_shutdown();
    outw(0x604,  0x2000);
    outw(0xB004, 0x2000);
    const char* s = "Shutdown";
    while (*s) outw(0x8900, (uint16_t)(uint8_t)*s++);
    for (;;) __asm__ volatile ("cli; hlt");
}
