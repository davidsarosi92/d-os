/* =============================================================================
 * cmd_pci.c — `lspci`: what the PCI bus holds (§M85 stage 4b, 2026-09-26).
 *
 * Every driver here finds its device through pci_scan(), and nothing let a
 * person ask the same question — which, on a machine nobody has booted before
 * (sbsa-ref), is the first one: "what is on this bus, and did the firmware
 * give it an address?".  Portable: pci_scan is the HAL's (port I/O on x86,
 * ECAM on aarch64), so this lists the same thing everywhere.
 * ============================================================================= */

#include "pci.h"
#include "printf.h"
#include "shellcmd.h"

static const char* cls_name(uint8_t c, uint8_t s) {
    switch (c) {
    case 0x01: return s == 0x06 ? "SATA (AHCI)" : s == 0x08 ? "NVMe" : "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x06: return "bridge";
    case 0x0C: return s == 0x03 ? "USB" : "serial bus";
    default:   return "other";
    }
}

static int g_n;
static void visit(const struct pci_device* d, void* ctx) {
    (void)ctx;
    g_n++;
    kprintf("  %u:%u.%u  %x:%x  class %x.%x  %s  bar0 %x", d->bus, d->slot, d->func,
            d->vendor_id, d->device_id, d->class_code, d->subclass,
            cls_name(d->class_code, d->subclass), d->bar[0]);
    /* §M85 — the interrupt line the platform routes this function's INTx to
     * (x86: the legacy IRQ; aarch64: the GIC INTID from _PRT / interrupt-map). */
    if (d->irq_line == 0xFF) kprintf("  irq -\n");
    else                     kprintf("  irq %u\n", d->irq_line);
}

static void cmd_lspci(const char* args) {
    (void)args;
    g_n = 0;
    pci_scan(visit, 0);
    kprintf("lspci: %d function(s)%s\n", g_n, g_n ? "" : " - no PCI bus, or nothing on it");
}
SHELL_CMD(lspci) = { "lspci", "", "list the devices on the PCI bus",
                     SHELL_G_DEV, cmd_lspci, SHELL_P_ANY };
