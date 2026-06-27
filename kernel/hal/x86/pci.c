/* =============================================================================
 * pci.c — i386 PCI configuration-space access via port I/O.
 *
 * See pci.h for the address-register layout.  Quick reminder:
 *
 *   addr = 0x80000000
 *        | (bus  << 16)
 *        | (slot << 11)
 *        | (func <<  8)
 *        | (offset & 0xFC);
 *
 *   outl 0xCF8, addr;
 *   value = inl 0xCFC;        — for the 32-bit dword at `offset & ~3`
 *
 * Sub-dword reads (uint8 / uint16) read the full dword then shift +
 * mask.  Writes for sub-dwords do read-modify-write, which is what
 * `pci_write16` does inline.
 *
 * The whole file is x86-specific; under the §M17 portability cut it
 * moves behind a `hal_pci_*` interface so ARM (ECAM) and x86_64
 * (MMConfig) can implement the same API.
 * ============================================================================= */

#include "pci.h"
#include "hal.h"
#include <stdint.h>

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

static uint32_t make_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u
         | ((uint32_t)bus  << 16)
         | ((uint32_t)slot << 11)
         | ((uint32_t)func <<  8)
         | ((uint32_t)off  &  0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(CONFIG_ADDR, make_address(bus, slot, func, off));
    return inl(CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, off & 0xFC);
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, slot, func, off & 0xFC);
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(CONFIG_ADDR, make_address(bus, slot, func, off));
    outl(CONFIG_DATA, v);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    /* Read-modify-write the containing dword. */
    uint32_t full = pci_read32(bus, slot, func, off & 0xFC);
    int shift = (off & 2) * 8;
    full &= ~(0xFFFFu << shift);
    full |= (uint32_t)v << shift;
    pci_write32(bus, slot, func, off & 0xFC, full);
}

/* ----------------------------------------------------------------------- */
/* Enumeration.                                                            */
/* ----------------------------------------------------------------------- */

static void fill_device(struct pci_device* d,
                        uint8_t bus, uint8_t slot, uint8_t func) {
    d->bus  = bus;
    d->slot = slot;
    d->func = func;
    d->vendor_id   = pci_read16(bus, slot, func, PCI_VENDOR_ID);
    d->device_id   = pci_read16(bus, slot, func, PCI_DEVICE_ID);
    d->revision    = pci_read8 (bus, slot, func, PCI_REVISION);
    d->prog_if     = pci_read8 (bus, slot, func, PCI_PROG_IF);
    d->subclass    = pci_read8 (bus, slot, func, PCI_SUBCLASS);
    d->class_code  = pci_read8 (bus, slot, func, PCI_CLASS);
    d->header_type = pci_read8 (bus, slot, func, PCI_HEADER_TYPE);
    d->irq_line    = pci_read8 (bus, slot, func, PCI_INTERRUPT_LINE);
    for (int i = 0; i < 6; i++) {
        d->bar[i] = pci_read32(bus, slot, func, PCI_BAR0 + i * 4);
    }
}

void pci_scan(pci_visit_fn fn, void* ctx) {
    /* Bus 0 only.  Adding bridge traversal is a half-day's work — when
     * we need it (e.g. multiple buses on real hardware) the recursion
     * over secondary buses fits cleanly here. */
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint16_t vendor = pci_read16(0, slot, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) continue;             /* slot empty */

        struct pci_device d;
        fill_device(&d, 0, slot, 0);
        fn(&d, ctx);

        /* Multi-function devices (header_type bit 7) have additional
         * functions at func=1..7.  Each function may have an entirely
         * different role. */
        if (d.header_type & 0x80) {
            for (uint8_t func = 1; func < 8; func++) {
                uint16_t v = pci_read16(0, slot, func, PCI_VENDOR_ID);
                if (v == 0xFFFF) continue;
                struct pci_device dd;
                fill_device(&dd, 0, slot, func);
                fn(&dd, ctx);
            }
        }
    }
}

/* Helper that pci_find_device hooks against pci_scan via this ctx. */
struct find_ctx {
    uint16_t want_vendor;
    uint16_t want_device;
    struct pci_device* out;
    int found;
};
static void find_visit(const struct pci_device* d, void* ctx_) {
    struct find_ctx* c = (struct find_ctx*)ctx_;
    if (c->found) return;
    if (d->vendor_id != c->want_vendor) return;
    if (c->want_device != 0xFFFF && d->device_id != c->want_device) return;
    if (c->out) *c->out = *d;
    c->found = 1;
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device* out) {
    struct find_ctx c = { .want_vendor = vendor, .want_device = device,
                          .out = out, .found = 0 };
    pci_scan(find_visit, &c);
    return c.found ? 0 : -1;
}

uint16_t pci_bar_io_base(uint32_t bar) {
    if ((bar & 0x1) == 0) return 0;             /* memory-space BAR */
    return (uint16_t)(bar & ~0x3u);
}
