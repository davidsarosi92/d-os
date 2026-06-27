/* hal.h — Hardware Abstraction Layer public interface.
 *
 * Declarations visible to every caller that needs to poke the CPU's I/O
 * port space or request a power-state transition.  Implementations live
 * under kernel/hal/<arch>/.  For now only x86 is implemented. */

#ifndef HAL_H
#define HAL_H

#include <stdint.h>

/* 8-bit port I/O.  Wrappers for the `in`/`out` instructions. */
uint8_t inb(uint16_t port);
void    outb(uint16_t port, uint8_t value);

/* 16-bit port output.  Needed for ACPI PM control register writes. */
void     outw(uint16_t port, uint16_t value);
uint16_t inw(uint16_t port);

/* 32-bit port I/O.  Used by PCI config space access (0xCF8/0xCFC) and
 * any future device that exposes 32-bit registers in I/O space. */
void     outl(uint16_t port, uint32_t value);
uint32_t inl (uint16_t port);

/* Soft power-off.  Tries ACPI S5 first; falls back to QEMU / Bochs hacks
 * so emulator-driven testing works.  Never returns on success. */
void hal_shutdown(void);

/* Reboot via the 8042 keyboard-controller reset line (with ICH 0xCF9 as
 * a backup).  Works on most PC-compatible hardware and under QEMU.
 * Never returns on success. */
void hal_reboot(void);

#endif
