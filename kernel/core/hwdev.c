/* =============================================================================
 * hwdev.c — enumerate the machine's hardware and say what is driving it.
 *
 * See hwdev.h for why this is device-centric rather than driver-centric.  The
 * short version: a driver list cannot report the hardware it does not cover,
 * and that is exactly the hardware somebody needs to be told about.
 * ============================================================================= */

#include "hwdev.h"
#include "pci.h"
#include "driver.h"
#include <stddef.h>

static int hw_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------------------------------------------------------------- */
/* Names.                                                            */
/* ---------------------------------------------------------------- */

/* Exact IDs worth spelling out.  DELIBERATELY SHORT: this is a courtesy on top
 * of the class-code rule below, not the mechanism.  Shipping the full PCI ID
 * database would be megabytes that go stale, to answer a question the class
 * code already answers usefully. */
static const struct { uint16_t v, d; const char* name; } known[] = {
    /* virtio — the transport this machine gets nearly everything through. */
    { 0x1AF4, 0x1000, "virtio network card" },
    { 0x1AF4, 0x1001, "virtio block device" },
    { 0x1AF4, 0x1002, "virtio memory balloon" },
    { 0x1AF4, 0x1003, "virtio console" },
    { 0x1AF4, 0x1004, "virtio SCSI controller" },
    { 0x1AF4, 0x1005, "virtio entropy source" },
    { 0x1AF4, 0x1009, "virtio 9P filesystem" },
    { 0x1AF4, 0x1050, "virtio GPU" },
    { 0x1AF4, 0x1052, "virtio input device" },
    { 0x1AF4, 0x1059, "virtio sound card" },
    /* Intel south-bridge parts QEMU and real machines both present. */
    { 0x8086, 0x2415, "Intel 82801AA AC'97 audio" },
    { 0x8086, 0x2668, "Intel 82801FB HD Audio (ICH6)" },
    { 0x8086, 0x293E, "Intel 82801I HD Audio (ICH9)" },
    { 0x8086, 0x2922, "Intel ICH9 SATA controller (AHCI)" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE controller" },
    { 0x8086, 0x7113, "Intel PIIX4 power management" },
    { 0x8086, 0x100E, "Intel 82540EM gigabit ethernet" },
    { 0x8086, 0x1237, "Intel 440FX host bridge" },
    { 0x8086, 0x29C0, "Intel Q35 host bridge" },
    { 0x8086, 0x2918, "Intel ICH9 LPC bridge" },
    { 0x8086, 0x24CD, "Intel ICH5 USB2 (EHCI)" },
    /* QEMU's own. */
    { 0x1234, 0x1111, "QEMU standard VGA" },
    { 0x1234, 0x11E8, "QEMU educational device" },
    { 0x1B36, 0x000D, "QEMU xHCI USB controller" },
    /* Oracle VirtualBox — listed because a guest that cannot NAME the host's
     * guest-additions device cannot tell the user what it is missing. */
    { 0x80EE, 0xCAFE, "VirtualBox guest device (VMMDev)" },
    { 0x80EE, 0xBEEF, "VirtualBox graphics adapter" },
    /* Common on real iron. */
    { 0x10EC, 0x8139, "Realtek RTL8139 ethernet" },
    { 0x1013, 0x00B8, "Cirrus Logic GD 5446 VGA" },
    { 0, 0, NULL }
};

/* PCI class codes (PCI 2.3 §D).  THIS IS THE ONE THAT MATTERS: every device
 * carries a class, so hardware nobody has taught us about still gets a name
 * that tells the user what it IS — which is the whole point when the row is
 * there to say "this needs a driver". */
static const char* class_name(uint8_t cls, uint8_t sub) {
    switch (cls) {
    case 0x00: return "unclassified device";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI storage controller";
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04:
        return (sub == 0x03) ? "audio device" : "multimedia controller";
    case 0x05: return "memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device controller";
    case 0x0A: return "docking station";
    case 0x0B: return "processor";
    case 0x0C:
        switch (sub) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    case 0x0D: return "wireless controller";
    case 0x10: return "encryption controller";
    case 0x11: return "signal processing controller";
    default:   return "PCI device";
    }
}

const char* hw_name_for(uint16_t vendor, uint16_t device,
                        uint8_t class_code, uint8_t subclass) {
    for (int i = 0; known[i].name; i++)
        if (known[i].v == vendor && known[i].d == device) return known[i].name;
    return class_name(class_code, subclass);
}

int hw_needs_driver(uint8_t class_code, uint8_t subclass) {
    if (class_code == 0x06) {
        /* Bridges.  The host bridge IS the root complex, and a PCI-to-PCI
         * bridge is enumeration's business — neither is a device somebody
         * should go looking for software for.  The ISA/LPC bridge is the
         * interesting exception and is NOT excused: it carries the PCI
         * interrupt routing, which is real and which this system currently
         * takes on trust from whatever the firmware left in config space. */
        if (subclass == 0x00) return 0;      /* host bridge          */
        if (subclass == 0x04) return 0;      /* PCI-to-PCI bridge    */
    }
    return 1;
}

/* ---------------------------------------------------------------- */
/* Which driver claims an ID.                                        */
/* ---------------------------------------------------------------- */

/* ID matches are tried BEFORE class matches, and the order is the design: a
 * class match is a claim over a whole category, so a driver written for one
 * specific card must win over the generic one for that card.  Getting this
 * backwards would silently attribute a device to whichever driver happened to
 * be declared first. */
const char* hw_driver_for(uint16_t vendor, uint16_t device,
                          uint8_t class_code, uint8_t subclass) {
    for (struct driver_match* m = __start_driver_matches;
         m < __stop_driver_matches; m++) {
        if (m->vendor == HW_ANY_VENDOR) continue;
        if (m->vendor != vendor) continue;
        if (m->device != HW_ANY_DEVICE && m->device != device) continue;
        return m->driver;
    }
    for (struct driver_match* m = __start_driver_matches;
         m < __stop_driver_matches; m++) {
        if (m->vendor != HW_ANY_VENDOR) continue;
        if (m->cls != class_code) continue;
        if (m->sub != HW_ANY_SUB && m->sub != subclass) continue;
        return m->driver;
    }
    return NULL;
}

/* The reverse direction: what is this driver FOR?  Needed to describe hardware
 * that is not present — there is no device to read a class code from, so the
 * only sources are what the driver declared and, failing that, its class.
 * Printing the class ("audio", "misc") is the honest floor and a poor name,
 * which is why the declaration carries one. */
const char* hw_hardware_of(const char* driver_name) {
    for (struct driver_match* m = __start_driver_matches;
         m < __stop_driver_matches; m++) {
        if (!hw_streq(m->driver, driver_name)) continue;
        if (m->name) return m->name;
        if (m->vendor != HW_ANY_VENDOR && m->vendor != 0)
            return hw_name_for(m->vendor, m->device, 0, 0);
        if (m->vendor == HW_ANY_VENDOR)
            return class_name(m->cls, m->sub);
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* The scan.                                                         */
/* ---------------------------------------------------------------- */

struct scan_ctx { struct hw_device* out; int cap, n; };

static void visit(const struct pci_device* d, void* vctx) {
    struct scan_ctx* c = (struct scan_ctx*)vctx;
    if (c->n >= c->cap) return;
    struct hw_device* h = &c->out[c->n++];
    h->online     = 1;
    h->is_pci     = 1;
    h->bdf        = (uint16_t)((d->bus << 8) | (d->slot << 3) | d->func);
    h->vendor     = d->vendor_id;
    h->device     = d->device_id;
    h->class_code = d->class_code;
    h->subclass   = d->subclass;
    h->name       = hw_name_for(d->vendor_id, d->device_id,
                                d->class_code, d->subclass);
    h->driver     = hw_driver_for(d->vendor_id, d->device_id,
                                  d->class_code, d->subclass);
}

int hw_enumerate(struct hw_device* out, int cap) {
    struct scan_ctx c = { out, cap, 0 };
    if (cap <= 0) return 0;

    /* 1. Everything on the PCI bus, whether we can drive it or not. */
    pci_scan(visit, &c);

    /* 2. PLATFORM DEVICES — hardware that is not on any enumerable bus.
     *
     * The PS/2 controller, the timer and the UART are real hardware and appear
     * on no bus we can walk, so the only evidence of them is that a driver
     * probed successfully.  Listing them from the RUNNING drivers is therefore
     * an inference, not an enumeration, and the difference is worth being
     * honest about: we can say a platform device is here because something is
     * talking to it, and we cannot say anything about one nothing has claimed.
     *
     * A driver that MATCHED a PCI device is skipped — it was already listed
     * above as the hardware it drives, and listing it again would double-count
     * the device under two rows. */
    for (int i = 0; i < driver_count_all() && c.n < cap; i++) {
        struct driver* d = driver_at(i);
        if (!d || !d->name) continue;
        if (!(driver_state(d) & DRV_S_INITED)) continue;   /* not running */

        /* A declaration with vendor 0 is a PLATFORM name, not a bus claim —
         * so it must NOT suppress the row it exists to label. */
        int claims_pci = 0;
        for (struct driver_match* m = __start_driver_matches;
             m < __stop_driver_matches; m++)
            if (hw_streq(m->driver, d->name) && m->vendor != 0)
                { claims_pci = 1; break; }
        if (claims_pci) continue;

        struct hw_device* h = &out[c.n++];
        h->online = 1;
        h->is_pci = 0;
        h->bdf = 0; h->vendor = 0; h->device = 0;
        h->class_code = h->subclass = 0;
        const char* hw = hw_hardware_of(d->name);
        h->name   = hw ? hw : (d->class ? d->class : "platform device");
        h->driver = d->name;
    }

    /* 3. HARDWARE WE KNOW AND DO NOT HAVE.
     *
     * A driver whose probe failed is not a fault — it is the description of a
     * machine that could drive an AC97 and has not got one.  It belongs in the
     * list because "we support this and it is absent" and "we have never heard
     * of this" are different answers, and a list that shows only what is
     * present cannot tell them apart. */
    for (int i = 0; i < driver_count_all() && c.n < cap; i++) {
        struct driver* d = driver_at(i);
        if (!d || !d->name) continue;
        uint8_t st = driver_state(d);
        if (st & DRV_S_INITED) continue;                   /* already listed */
        /* A driver that is merely STOPPED still has its hardware present — it
         * was listed by the PCI scan above if it is a PCI device.  Only report
         * the ones whose hardware was not found. */
        if (!(st & DRV_S_PROBE_FAIL) && !(st & DRV_S_INIT_FAIL) && st != 0)
            continue;
        int claims_pci = 0;
        /* A NEVER-PROBED driver (state 0 — §M67 attaches a module without
         * starting it) that claims no bus device is a SOFTWARE device, and
         * calling it "hardware we have not got" is simply false: the loopback
         * interface is as present as the machine is.  It goes in the present
         * group as not-started.  A never-probed driver that DOES claim a PCI ID
         * the scan did not find is genuinely absent, and falls through. */
        if (st == 0) {
            int has_bus_claim = 0;
            for (struct driver_match* m = __start_driver_matches;
                 m < __stop_driver_matches; m++)
                if (hw_streq(m->driver, d->name) && m->vendor != 0)
                    { has_bus_claim = 1; break; }
            if (!has_bus_claim) {
                if (c.n >= cap) break;
                struct hw_device* sw = &out[c.n++];
                const char* nm = hw_hardware_of(d->name);
                sw->online = 1;
                sw->is_pci = 0;
                sw->bdf = 0; sw->vendor = 0; sw->device = 0;
                sw->class_code = sw->subclass = 0;
                sw->name   = nm ? nm : (d->class ? d->class : "software device");
                sw->driver = d->name;
                continue;
            }
        }
        for (struct driver_match* m = __start_driver_matches;
             m < __stop_driver_matches; m++)
            if (hw_streq(m->driver, d->name) && m->vendor != 0)
                { claims_pci = 1; break; }
        if (claims_pci) {
            /* If the PCI scan already found its device, it is online with a
             * driver that failed — not offline.  Do not list it twice. */
            int listed = 0;
            for (int k = 0; k < c.n; k++)
                if (out[k].is_pci && hw_streq(out[k].driver, d->name))
                    { listed = 1; break; }
            if (listed) continue;
        }

        struct hw_device* h = &out[c.n++];
        h->online = 0;
        h->is_pci = 0;
        h->bdf = 0; h->vendor = 0; h->device = 0;
        h->class_code = h->subclass = 0;
        const char* hw = hw_hardware_of(d->name);
        h->name   = hw ? hw : (d->class ? d->class : "device");
        h->driver = d->name;
    }

    return c.n;
}
