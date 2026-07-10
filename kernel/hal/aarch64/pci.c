/* =============================================================================
 * pci.c — AArch64 PCIe access via ECAM (M15 USB on ARM).
 *
 * The x86 pci.c pokes the legacy config ports 0xCF8/0xCFC — meaningless on ARM.
 * QEMU's `virt` board has a PCIe host bridge (GPEX) whose config space is
 * memory-mapped (ECAM) at 0x40_1000_0000, so config access is plain MMIO
 * loads/stores at ECAM_BASE + (bus<<20)+(slot<<15)+(func<<12)+offset.  This file
 * provides the SAME pci.h API the portable drivers (xhci.c) expect, so they
 * link unchanged.
 *
 * BAR assignment: booting raw via `-kernel` there is no firmware to program the
 * BARs, so every BAR reads back 0.  We size each memory BAR (write all-ones,
 * read the mask) and assign it an address from the board's 32-bit MMIO window
 * (0x1000_0000..), which the MMU already maps as Device memory (mmu.c index 0),
 * then enable memory decode + bus-master.  The x86 port relies on the BIOS for
 * this step.
 *
 * References: PCI Firmware Spec (ECAM); QEMU hw/arm/virt.c memory map.
 * ============================================================================= */

#include "pci.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

void mmu_map_device_1gib(uint64_t va);          /* mmu.c */

/* QEMU `virt` PCIe host-bridge windows (stable for the board; see virt.c). */
#define ECAM_BASE   0x4010000000ULL             /* config space (bus 0..255)   */
#define MMIO_BASE   0x10000000UL                /* 32-bit MMIO window (Device) */
#define MMIO_LIMIT  0x3eff0000UL

static uint64_t g_mmio_next = MMIO_BASE;        /* bump allocator for BARs     */
static int      g_mapped;

static void pci_map_ecam(void) {
    if (g_mapped) return;
    mmu_map_device_1gib(ECAM_BASE);             /* reach the config space       */
    g_mapped = 1;
}

static inline volatile void* cfg(uint8_t bus, uint8_t slot, uint8_t func, uint32_t off) {
    return (volatile void*)(uintptr_t)(ECAM_BASE
        + ((uint64_t)bus << 20) + ((uint64_t)slot << 15)
        + ((uint64_t)func << 12) + off);
}

/* ---- config accessors (ECAM supports sub-dword MMIO access) ----------------- */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    pci_map_ecam(); return *(volatile uint32_t*)cfg(bus, slot, func, off & 0xFC);
}
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    pci_map_ecam(); return *(volatile uint16_t*)cfg(bus, slot, func, off & 0xFE);
}
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    pci_map_ecam(); return *(volatile uint8_t*)cfg(bus, slot, func, off);
}
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    pci_map_ecam(); *(volatile uint32_t*)cfg(bus, slot, func, off & 0xFC) = v;
}
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    pci_map_ecam(); *(volatile uint16_t*)cfg(bus, slot, func, off & 0xFE) = v;
}

/* No port-I/O BARs on ARM — the drivers we run are all MMIO. */
uint16_t pci_bar_io_base(uint32_t bar) { (void)bar; return 0; }

/* Walk the capability list for `cap_id`; returns its config offset or 0. */
uint8_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id) {
    if (!(pci_read16(bus, slot, func, PCI_STATUS) & PCI_STATUS_CAPLIST)) return 0;
    uint8_t off = pci_read8(bus, slot, func, PCI_CAP_PTR) & 0xFC;
    for (int guard = 0; off && guard < 48; guard++) {
        if (pci_read8(bus, slot, func, off) == cap_id) return off;
        off = pci_read8(bus, slot, func, off + 1) & 0xFC;
    }
    return 0;
}

/* ---- BAR assignment -------------------------------------------------------- */
static void assign_bars(uint8_t bus, uint8_t slot, uint8_t func) {
    for (int i = 0; i < 6; i++) {
        uint8_t off = PCI_BAR0 + i * 4;
        uint32_t bar = pci_read32(bus, slot, func, off);
        if (bar & 0x1) continue;                        /* I/O BAR — skip       */
        int is64 = ((bar >> 1) & 0x3) == 0x2;

        /* Size the BAR: write all-ones, read the mask back, restore. */
        pci_write32(bus, slot, func, off, 0xFFFFFFFFu);
        uint32_t mask = pci_read32(bus, slot, func, off) & 0xFFFFFFF0u;
        pci_write32(bus, slot, func, off, bar);
        if (mask == 0) { if (is64) i++; continue; }     /* unimplemented BAR    */

        uint32_t size = (~mask) + 1;                    /* 32-bit size (< 4 GiB)*/
        uint64_t addr = (g_mmio_next + size - 1) & ~((uint64_t)size - 1);
        if (addr + size > MMIO_LIMIT) { if (is64) i++; continue; }  /* no space */
        g_mmio_next = addr + size;

        pci_write32(bus, slot, func, off, (uint32_t)addr);
        if (is64) { pci_write32(bus, slot, func, off + 4, 0); i++; }  /* hi = 0 */
    }
    /* Enable memory decode + bus-mastering so the controller can be driven. */
    uint16_t cmd = pci_read16(bus, slot, func, PCI_COMMAND);
    pci_write16(bus, slot, func, PCI_COMMAND, cmd | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
}

/* ---- enumeration ----------------------------------------------------------- */
static void fill_device(struct pci_device* d, uint8_t bus, uint8_t slot, uint8_t func) {
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor_id   = pci_read16(bus, slot, func, PCI_VENDOR_ID);
    d->device_id   = pci_read16(bus, slot, func, PCI_DEVICE_ID);
    d->revision    = pci_read8 (bus, slot, func, PCI_REVISION);
    d->prog_if     = pci_read8 (bus, slot, func, PCI_PROG_IF);
    d->subclass    = pci_read8 (bus, slot, func, PCI_SUBCLASS);
    d->class_code  = pci_read8 (bus, slot, func, PCI_CLASS);
    d->header_type = pci_read8 (bus, slot, func, PCI_HEADER_TYPE);
    d->irq_line    = pci_read8 (bus, slot, func, PCI_INTERRUPT_LINE);
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, slot, func, PCI_BAR0 + i * 4);
}

/* Enumerate the root bus (bus 0 — QEMU `virt` has no bridges we care about),
 * assign BARs, and hand each function to the visitor. */
void pci_scan(pci_visit_fn fn, void* ctx) {
    pci_map_ecam();
    for (int slot = 0; slot < 32; slot++) {
        if (pci_read16(0, slot, 0, PCI_VENDOR_ID) == 0xFFFF) continue;
        int nfunc = (pci_read8(0, slot, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
        for (int func = 0; func < nfunc; func++) {
            if (pci_read16(0, slot, func, PCI_VENDOR_ID) == 0xFFFF) continue;
            assign_bars(0, slot, func);
            struct pci_device d;
            fill_device(&d, 0, slot, func);
            fn(&d, ctx);
        }
    }
}

static void find_visit(const struct pci_device* d, void* ctx_) {
    struct { uint16_t v, dv; struct pci_device* out; int found; }* c = ctx_;
    if (!c->found && d->vendor_id == c->v && d->device_id == c->dv) {
        *c->out = *d; c->found = 1;
    }
}
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device* out) {
    struct { uint16_t v, dv; struct pci_device* out; int found; } c = { vendor, device, out, 0 };
    pci_scan(find_visit, &c);
    return c.found ? 0 : -1;
}
