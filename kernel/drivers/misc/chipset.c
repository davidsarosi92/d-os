/* =============================================================================
 * chipset.c — the display adapter and the two PIIX chipset functions.
 *
 * -----------------------------------------------------------------------------
 * THESE ARE NOT FOUR DRIVERS, AND SAYING SO IS THE POINT
 *
 * The device manager listed five unclaimed devices on an ordinary boot, and it
 * is tempting to read that as five drivers to write.  It is not.  Asked what
 * each of them actually needs, the answers are different:
 *
 *   Standard VGA          A REAL CLAIM over code that already exists.  This
 *                         system has been driving the framebuffer since M21 —
 *                         through `fb_present.h` and the Bochs VBE registers —
 *                         but nothing ever REGISTERED the adapter, so the one
 *                         device the user is looking at while reading the list
 *                         appeared in it as unclaimed hardware.
 *
 *   PIIX3 ISA bridge      REAL CONTENT: it carries the PCI interrupt routing
 *                         (the PIRQ registers).  This system takes an
 *                         interrupt line on trust from whatever the firmware
 *                         left in config space, and §M66 already met the case
 *                         where that trust is misplaced — a hot-added device
 *                         arrives with line 0, which is the TIMER.  Reading the
 *                         routing out is a diagnostic we did not have.
 *
 *   PIIX4 power mgmt      REAL CONTENT, mostly already used: this function is
 *                         where the ACPI PM1a control block lives, which is how
 *                         `system_power_off` turns the machine off.  A driver
 *                         makes the dependency VISIBLE instead of implicit.
 *
 *   440FX host bridge     **NOTHING.**  A host bridge is the PCI root complex
 *                         itself; there is no device behind it to program, and
 *                         Linux has no driver for it either.  Writing one would
 *                         be a file whose init function returns 0 — which is
 *                         worse than no driver, because it makes the device
 *                         manager report a claim that means nothing.  It is
 *                         named (so the list reads well) and `hw_needs_driver`
 *                         excuses it, which is the honest treatment.
 *
 * One file, because they are one chipset and three small things.  Splitting
 * them into three files would suggest three subsystems.
 * ============================================================================= */

#include "driver.h"
#include "hwdev.h"
#include "pci.h"
#include "printf.h"
#include "klog.h"
#include "fb_present.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/* 1. The display adapter.                                           */
/* ---------------------------------------------------------------- */

/* WHAT THIS DRIVER DOES AND DOES NOT DO, because the distinction is the whole
 * reason it is honest to ship it:
 *
 * It does NOT bring the framebuffer up.  That happens far earlier than any
 * driver — `fb_terminal` needs a console before `driver_init_all` runs, and the
 * multiboot header asks the firmware for a linear framebuffer before the kernel
 * has executed an instruction.  Reordering that to fit a driver model would
 * mean booting without a screen to report a failure on.
 *
 * What it does is CLAIM the adapter and report what is behind it, so the device
 * the user is looking at stops appearing in the hardware list as unclaimed.
 * The mode-setting code it fronts is §M61's, reached through `fb_present.h`. */
static int vga_probe(void* ctx) {
    (void)ctx;
    struct fb_mode m;
    /* The test is whether we HAVE a framebuffer, not whether a particular
     * adapter is present: this driver's claim is over the display this kernel
     * is actually using, and on a machine where the console came up on VGA text
     * there is nothing here to speak for. */
    return fb_mode_current(&m) == 0 ? 0 : -1;
}

static int vga_init(void* ctx) {
    (void)ctx;
    struct fb_mode m;
    if (fb_mode_current(&m) != 0) return -1;
    int modes = fb_mode_count();
    kprintf("vga: %ux%u@%u, %d mode(s) available%s\n",
            m.w, m.h, m.bpp, modes,
            modes > 1 ? " (`mode` can change it)"
                      : " — this display cannot be asked to change");
    klog(KLOG_INFO, "vga", "display %ux%u@%u\n", m.w, m.h, m.bpp);
    return 0;
}

static int vga_shutdown(void* ctx) {
    (void)ctx;
    /* Nothing to withdraw: this driver registered no device with any class
     * registry and holds no resource.  Returning 0 rather than refusing is
     * correct — and it is what lets the driver be STOPPED, which a driver with
     * no shutdown hook cannot be (§M67 refuses those at load). */
    return 0;
}

static const struct driver_ops vga_ops = {
    .probe = vga_probe, .init = vga_init, .shutdown = vga_shutdown,
};

DRIVER_MATCH(m_vga_qemu) = { .driver = "vga", .name = "QEMU standard VGA",
                             .vendor = 0x1234, .device = 0x1111 };
DRIVER_MATCH(m_vga_class) = { .driver = "vga", .name = "display adapter",
                              .vendor = HW_ANY_VENDOR, .cls = 0x03,
                              .sub = HW_ANY_SUB };

DRIVER_EX(vga, "display", &vga_ops, NULL, DOMAIN_KERNEL, DRVF_BOOT_CRITICAL);

/* ---------------------------------------------------------------- */
/* 2. The ISA bridge, and the interrupt routing it holds.            */
/* ---------------------------------------------------------------- */

/* PIIX PIRQ route control: four bytes at config offset 0x60.  Each maps one PCI
 * interrupt pin (PIRQA..PIRQD) onto an 8259 line; bit 7 set means the pin is
 * DISABLED, and the low four bits are the IRQ. */
#define PIIX_PIRQ_ROUTE 0x60

static struct pci_device isa_pd;
static int isa_found;

static void isa_visit(const struct pci_device* d, void* ctx) {
    (void)ctx;
    if (isa_found) return;
    if (d->class_code == 0x06 && d->subclass == 0x01) { isa_pd = *d; isa_found = 1; }
}

static int isa_probe(void* ctx) {
    (void)ctx;
    isa_found = 0;
    pci_scan(isa_visit, NULL);
    return isa_found ? 0 : -1;
}

static int isa_init(void* ctx) {
    (void)ctx;
    if (!isa_found) return -1;

    /* READ IT, DO NOT PROGRAM IT.  The firmware has already routed these and
     * every device's `irq_line` was read from config space on that basis; a
     * driver that re-routed them at this point would invalidate every interrupt
     * already installed, and it would do so on a machine that was working.
     *
     * The value is in being able to SEE the mapping.  §M66 shipped a case where
     * a hot-added device arrives with line 0 — the timer — and the only way to
     * tell a genuinely unrouted device from a badly routed one is to look at
     * what the bridge thinks. */
    uint32_t r = pci_read32(isa_pd.bus, isa_pd.slot, isa_pd.func, PIIX_PIRQ_ROUTE);
    kprintf("isa: PCI interrupt routing —");
    for (int i = 0; i < 4; i++) {
        uint8_t b = (uint8_t)(r >> (i * 8));
        kprintf(" PIRQ%c=", 'A' + i);
        if (b & 0x80) kprintf("off");
        else          kprintf("irq%d", b & 0x0F);
    }
    kprintf("\n");
    klog(KLOG_INFO, "isa", "PIRQ routing %x\n", r);
    return 0;
}

static int isa_shutdown(void* ctx) { (void)ctx; return 0; }

static const struct driver_ops isa_ops = {
    .probe = isa_probe, .init = isa_init, .shutdown = isa_shutdown,
};

DRIVER_MATCH(m_isa) = { .driver = "isa", .name = "PCI-to-ISA bridge",
                        .vendor = HW_ANY_VENDOR, .cls = 0x06, .sub = 0x01 };

DRIVER_EX(isa, "bridge", &isa_ops, NULL, DOMAIN_KERNEL, DRVF_BOOT_CRITICAL);

/* ---------------------------------------------------------------- */
/* 3. Power management.                                              */
/* ---------------------------------------------------------------- */

/* This function is where the ACPI PM1a control block lives — the register
 * `system_power_off` writes to turn the machine off.  The dependency already
 * existed and was invisible: `acpi_init` finds the address in the FADT and
 * nothing ever mentioned which piece of hardware it belongs to.
 *
 * The driver's job is therefore NOT to take the power path over — that works
 * and is reached from `system_power_off` through ACPI, which is the portable
 * route.  It is to make the device visible and to say what depends on it, so
 * that a machine where power-off does nothing has somewhere to look. */
static struct pci_device pm_pd;
static int pm_found;

static void pm_visit(const struct pci_device* d, void* ctx) {
    (void)ctx;
    if (pm_found) return;
    /* Class 06 subclass 80 = "other bridge", which is where PIIX4's power
     * management function lives.  Matching the ID as well is what keeps this
     * from claiming an unrelated bridge on a machine we have not seen. */
    if (d->vendor_id == 0x8086 && (d->device_id == 0x7113 || d->device_id == 0x7100))
        { pm_pd = *d; pm_found = 1; }
}

static int pm_probe(void* ctx) {
    (void)ctx;
    pm_found = 0;
    pci_scan(pm_visit, NULL);
    return pm_found ? 0 : -1;
}

static int pm_init(void* ctx) {
    (void)ctx;
    if (!pm_found) return -1;
    /* PMBA at config 0x40: the I/O base of the power-management block.  Bit 0
     * is the space indicator, as in a BAR. */
    uint32_t base = pci_read32(pm_pd.bus, pm_pd.slot, pm_pd.func, 0x40) & ~0x3Fu;
    kprintf("pm: PIIX4 power management at %x:%x.%x, PM block at io %x — "
            "ACPI reaches it for power-off\n",
            pm_pd.bus, pm_pd.slot, pm_pd.func, base);
    klog(KLOG_INFO, "pm", "PIIX4 PM block at io %x\n", base);
    return 0;
}

static int pm_shutdown(void* ctx) { (void)ctx; return 0; }

static const struct driver_ops pm_ops = {
    .probe = pm_probe, .init = pm_init, .shutdown = pm_shutdown,
};

DRIVER_MATCH(m_pm) = { .driver = "piix_pm", .name = "PIIX4 power management",
                       .vendor = 0x8086, .device = 0x7113 };

DRIVER_EX(piix_pm, "power", &pm_ops, NULL, DOMAIN_KERNEL, 0);

/* ---------------------------------------------------------------- */
/* 4. The host bridge — named, NOT driven.                           */
/* ---------------------------------------------------------------- */

/* NO DRIVER, AND THAT IS THE ANSWER RATHER THAN AN OMISSION.
 *
 * A host bridge IS the PCI root complex: the thing every other device is
 * enumerated THROUGH.  There is no device behind it to program, no interrupt to
 * take, no resource to hold.  A driver here would be an init function that
 * returns 0, and the device manager would then report it as claimed — a claim
 * that means nothing, in the column a person consults to find out what is
 * unattended.  *A registry entry that conveys no information makes the entries
 * that do harder to trust.*
 *
 * What it gets instead is a NAME (so the hardware list reads as English) while
 * `hw_needs_driver` returns 0 for its class, so the list says "no driver
 * needed" rather than asking the user to go and find one. */
DRIVER_MATCH(m_hostbridge) = { .driver = "", .name = "PCI host bridge",
                               .vendor = HW_ANY_VENDOR,
                               .cls = 0x06, .sub = 0x00 };
