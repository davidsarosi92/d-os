/* =============================================================================
 * pci.h — PCI configuration-space access + bus enumeration.
 *
 * The PCI configuration space is a per-device 256-byte map that holds
 * vendor ID, device ID, class, BARs (Base Address Registers), IRQ line,
 * and other identification.  On x86, access is via two I/O ports:
 *
 *   0xCF8 (4 bytes, write) — config address register
 *                            bit 31    = enable
 *                            bits 30-24= reserved (zero)
 *                            bits 23-16= bus
 *                            bits 15-11= device (slot)
 *                            bits 10-8 = function
 *                            bits  7-2 = offset (4-byte aligned)
 *                            bits  1-0 = always zero
 *   0xCFC (4 bytes, r/w)   — config data register; reads/writes the
 *                            32-bit dword at the address above.
 *
 * Future arches won't use these ports — ARM uses memory-mapped ECAM,
 * x86_64 uses MMConfig.  When the §M17 portability cut lands, the
 * `pci_read32` / `pci_write32` primitives move behind a HAL hook;
 * everything in `pci_scan` / `struct pci_device` stays portable.
 *
 * Reference: PCI 2.3 spec §6 (Configuration Space).
 * ============================================================================= */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* Snapshot of the fields we read once per device during enumeration.
 * BAR values are raw — bit 0 indicates I/O space (1) vs memory space
 * (0), and the actual base is masked accordingly. */
struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  irq_line;          /* the legacy 8259 IRQ; 0xFF means none */
    uint32_t bar[6];
};

/* Standard config-space offsets we care about. */
#define PCI_VENDOR_ID       0x00    /* uint16 */
#define PCI_DEVICE_ID       0x02    /* uint16 */
#define PCI_COMMAND         0x04    /* uint16 */
#define PCI_STATUS          0x06    /* uint16 */
#define PCI_REVISION        0x08    /* uint8  */
#define PCI_PROG_IF         0x09    /* uint8  */
#define PCI_SUBCLASS        0x0A    /* uint8  */
#define PCI_CLASS           0x0B    /* uint8  */
#define PCI_HEADER_TYPE     0x0E    /* uint8  */
#define PCI_BAR0            0x10    /* uint32, repeat every 4 bytes for BAR1..5 */
#define PCI_INTERRUPT_LINE  0x3C    /* uint8  */

/* PCI command register bits we toggle. */
#define PCI_CMD_IO_SPACE    0x0001
#define PCI_CMD_MEM_SPACE   0x0002
#define PCI_CMD_BUS_MASTER  0x0004

/* Raw config access — implemented in kernel/hal/x86/pci.c. */
uint8_t  pci_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);

/* Walk every PCI slot looking for present devices.  Calls `on_device`
 * once per device found.  `ctx` is passed through.  Only bus 0 is
 * scanned today (no bridges) — enough for QEMU virtio-blk and friends.
 * Multi-function devices (header_type bit 7) are also enumerated. */
typedef void (*pci_visit_fn)(const struct pci_device* d, void* ctx);
void pci_scan(pci_visit_fn fn, void* ctx);

/* Convenience: find the first device matching (vendor, device), or
 * vendor only if device==0xFFFF.  Returns 0 on success, fills `out`. */
int  pci_find_device(uint16_t vendor, uint16_t device, struct pci_device* out);

/* Convenience: extract an I/O-space BAR base (mask low bits).  Returns
 * 0 if `bar` is a memory-space BAR or unpopulated. */
uint16_t pci_bar_io_base(uint32_t bar);

#endif
