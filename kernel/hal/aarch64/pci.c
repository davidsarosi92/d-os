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
#include "board.h"   /* §M85 — the machine, discovered */
#include <stdint.h>
#include <stddef.h>

void mmu_map_device_1gib(uint64_t va);          /* mmu.c */

/* QEMU `virt` PCIe host-bridge windows (stable for the board; see virt.c). */
/* §M85 stage 1 — from the board description, no longer `virt` constants. */
#define ECAM_BASE   (g_board.ecam)                              /* config space  */
#define MMIO_BASE   ((uintptr_t)g_board.pci_mmio32)             /* 32-bit window */
#define MMIO_LIMIT  ((uintptr_t)(g_board.pci_mmio32 + g_board.pci_mmio32_size))

static uint64_t g_mmio_next;                     /* bump allocator for BARs     */
static int      g_mapped;

/* 1 when the board has a PCIe ECAM at all.  A board description that names
 * none means there is none: config reads then answer "no device" (all ones)
 * instead of dereferencing address 0 plus a bus offset. */
static int pci_present(void) { return ECAM_BASE != 0; }

/* A bus the ECAM window actually covers (1 MiB of config space per bus).  The
 * window is the board's, not always 256 buses: `virt,highmem=off` gives 16 MiB
 * at 0x3f00_0000, which ENDS where RAM begins — so bus 16 there is the kernel
 * image, and a config WRITE to it (BAR sizing writes all-ones) would land in
 * our own code. */
static int bus_ok(uint8_t bus) { return ((uint64_t)bus << 20) < g_board.ecam_size; }

static void pci_map_ecam(void) {
    if (g_mapped || !pci_present()) return;
    mmu_map_device_1gib(ECAM_BASE);             /* reach the config space       */
    g_mmio_next = MMIO_BASE;
    g_mapped = 1;
}

static inline volatile void* cfg(uint8_t bus, uint8_t slot, uint8_t func, uint32_t off) {
    return (volatile void*)(uintptr_t)(ECAM_BASE
        + ((uint64_t)bus << 20) + ((uint64_t)slot << 15)
        + ((uint64_t)func << 12) + off);
}

/* ---- config accessors (ECAM supports sub-dword MMIO access) ----------------- */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    if (!pci_present() || !bus_ok(bus)) return 0xFFFFFFFFu;
    pci_map_ecam(); return *(volatile uint32_t*)cfg(bus, slot, func, off & 0xFC);
}
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    if (!pci_present() || !bus_ok(bus)) return 0xFFFFu;
    pci_map_ecam(); return *(volatile uint16_t*)cfg(bus, slot, func, off & 0xFE);
}
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    if (!pci_present() || !bus_ok(bus)) return 0xFFu;
    pci_map_ecam(); return *(volatile uint8_t*)cfg(bus, slot, func, off);
}
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    if (!pci_present() || !bus_ok(bus)) return;
    pci_map_ecam(); *(volatile uint32_t*)cfg(bus, slot, func, off & 0xFC) = v;
}
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    if (!pci_present() || !bus_ok(bus)) return;
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
        /* §M85 — a BAR the FIRMWARE already programmed is left alone.  On a raw
         * `virt` boot nothing has touched the bus and every BAR reads 0; under
         * UEFI (EDK2 on `virt`, sbsa-ref) the firmware enumerated it, and
         * reassigning from our own bump pointer would move a device the
         * firmware — and its ACPI tables — still describe at the old address. */
        if ((bar & ~0xFu) || (is64 && pci_read32(bus, slot, func, off + 4))) {
            if (is64) i++;
            continue;
        }

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
    /* §M85 — on ARM nothing programs INTERRUPT_LINE (it is a PC BIOS
     * convention), so the byte is meaningless here.  On an ACPI machine the
     * host bridge's _PRT says where the function's INTx pin lands; 0xFF means
     * "not routed" (the x86 drivers' own sentinel), so a driver falls back to
     * polling rather than hooking whatever number was left in the register. */
    d->irq_line = 0xFF;
    {
        extern int acpi_arm_pci_intx(uint8_t slot, uint8_t pin);
        extern int dtb_pci_intx(uint8_t slot, uint8_t pin);
        uint8_t pin = pci_read8(bus, slot, func, 0x3D);
        int gsi = -1;
        if (bus == 0 && pin)
            gsi = (g_board.src == BOARD_ACPI) ? acpi_arm_pci_intx(slot, pin)
                                               : dtb_pci_intx(slot, pin);
        if (gsi > 0 && gsi < 0xFF) d->irq_line = (uint8_t)gsi;
    }
    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, slot, func, PCI_BAR0 + i * 4);
}

/* Enumerate the root bus (bus 0 — QEMU `virt` has no bridges we care about),
 * assign BARs, and hand each function to the visitor. */
void pci_scan(pci_visit_fn fn, void* ctx) {
    if (!pci_present()) return;
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
