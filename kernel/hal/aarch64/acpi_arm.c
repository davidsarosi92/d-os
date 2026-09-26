/* =============================================================================
 * acpi_arm.c — the ARM machine, described by ACPI (§M85 stage 3, 2026-09-26).
 *
 * A UEFI firmware on an ARM server (sbsa-ref, and EDK2 on `virt` by default)
 * describes the machine with ACPI instead of a device tree.  This reads the
 * tables that matter for bring-up into the same `struct board` dtb.c fills:
 *
 *   MADT  the GIC (distributor, v2 CPU interface or v3 redistributors) and the
 *         CPUs (one GICC entry each);
 *   GTDT  the EL1 non-secure physical timer's interrupt;
 *   SPCR  the console UART and its interrupt;
 *   MCFG  the PCIe ECAM window and its bus range;
 *   FADT  the PSCI conduit — HVC or SMC, which decides whether the firmware
 *         that answers CPU_ON lives at EL2 or EL3;
 *   DSDT  the devices ONLY AML describes: virtio-mmio transports on `virt`
 *         (_HID LNRO0005), and the PCIe host's 32-bit memory window.
 *
 * THE AML READER IS DELIBERATELY NARROW.  AML is a bytecode with methods,
 * conditionals and an interpreter's worth of semantics; this does not execute
 * any of it.  It walks Device() blocks and reads a `_HID` given as a string or
 * EISA id and a `_CRS` given as a static Buffer — which is how QEMU's and
 * EDK2's tables describe the devices above.  A device whose _CRS is a METHOD
 * is not found, and the report says how many devices were seen, so "the
 * firmware has none" and "this reader could not read it" stay different
 * answers.
 *
 * Tables are reached through the direct map (reach()).
 * ============================================================================= */

#include "board.h"
#include "printf.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

struct sdt { char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6], oemtab[8];
             uint32_t oemrev, creator, creatorrev; } __attribute__((packed));

static uint32_t g_ncpu_acpi;
int acpi_arm_ncpu(void) { return (int)g_ncpu_acpi; }

static uint8_t  r8 (const uint8_t* p) { return p[0]; }
static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t* p) { return (uint32_t)r16(p) | ((uint32_t)r16(p + 2) << 16); }
static uint64_t r64(const uint8_t* p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }

/* A physical table address as a pointer, through the direct map (§M85 stage 4:
 * RAM is no longer identity-mapped, and on sbsa-ref the tables sit above
 * 1 TiB).  Its GiB is mapped first; a table below the direct map's base cannot
 * be reached and is refused by name. */
void mmu_direct_map_range(uint64_t base, uint64_t size);
extern uint64_t aarch64_phys_offset;
static const uint8_t* reach(uint64_t phys, const char* what) {
    if (!phys) return NULL;
    if (phys < aarch64_phys_offset) {
        kprintf("acpi: %s at %p is below the direct map - not read\n",
                what, (void*)(uintptr_t)phys);
        return NULL;
    }
    mmu_direct_map_range(phys, 0x10000);
    return (const uint8_t*)phys_to_virt(phys);
}

static int csum_ok(const uint8_t* p, uint32_t n) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

/* ---- MADT ---------------------------------------------------------------- */
static void parse_madt(const uint8_t* t, uint32_t len) {
    for (uint32_t o = 44; o + 2 <= len; ) {
        uint8_t type = t[o], elen = t[o + 1];
        if (elen < 2 || o + elen > len) break;
        const uint8_t* e = t + o;
        if (type == 0x0B && elen >= 76) {                     /* GICC */
            if (r32(e + 12) & 1) g_ncpu_acpi++;               /* Enabled  */
            if (!g_board.gicc) g_board.gicc = r64(e + 32);    /* v2 CPU IF */
            if (!g_board.gicr && r64(e + 60)) g_board.gicr = r64(e + 60);
        } else if (type == 0x0C && elen >= 24) {              /* GICD */
            g_board.gicd = r64(e + 8);
            uint8_t v = e[20];
            g_board.gic_version = v ? v : 2;                  /* 0 = "from HW" */
        } else if (type == 0x0E && elen >= 16) {              /* GICR range */
            g_board.gicr = r64(e + 4);
            g_board.gicr_size = r32(e + 12);
        }
        o += elen;
    }
    if (g_board.gic_version == 2 || (g_board.gic_version == 0 && g_board.gicc)) g_board.gic_version = 2;
    if (g_board.gic_version >= 3) g_board.gic_version = 3;
}

/* ---- GTDT / SPCR / MCFG / FADT -------------------------------------------- */
static void parse_gtdt(const uint8_t* t, uint32_t len) {
    if (len >= 64) g_board.timer_intid = r32(t + 56);         /* NS EL1 GSIV  */
}

static void parse_spcr(const uint8_t* t, uint32_t len) {
    if (len < 58) return;
    uint8_t type = t[36];
    /* 3 = ARM PL011, 0x0E = ARM SBSA generic UART (a PL011 subset). */
    if (type != 3 && type != 0x0E) {
        kprintf("acpi: SPCR console type %u is not a PL011 - ignored\n", type);
        return;
    }
    g_board.uart = r64(t + 44);
    if (t[52] & 8) g_board.uart_intid = r32(t + 54);          /* GIC-type IRQ */
}

static void parse_mcfg(const uint8_t* t, uint32_t len) {
    if (len < 44 + 16) return;
    const uint8_t* e = t + 44;
    uint64_t base = r64(e);
    uint16_t seg = r16(e + 8);
    uint8_t  sb = r8(e + 10), eb = r8(e + 11);
    if (seg != 0) return;
    g_board.ecam = base + ((uint64_t)sb << 20);
    g_board.ecam_size = ((uint64_t)(eb - sb) + 1) << 20;
}

static uint64_t g_dsdt;
static void parse_fadt(const uint8_t* t, uint32_t len) {
    if (len >= 131) {
        uint16_t arm = r16(t + 129);                          /* ARM boot flags */
        if (arm & 1) g_board.psci_smc = (arm & 2) ? 0 : 1;    /* bit1 = use HVC */
    }
    g_dsdt = (len >= 148 && r64(t + 140)) ? r64(t + 140) : (len >= 44 ? r32(t + 40) : 0);
}

/* ---- the narrow AML reader ------------------------------------------------- */
static uint32_t pkglen(const uint8_t* p, uint32_t* nbytes) {
    uint8_t b0 = p[0];
    uint32_t n = b0 >> 6;
    *nbytes = n + 1;
    if (!n) return b0 & 0x3F;
    uint32_t v = b0 & 0x0F;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    return v;
}

static int seg_eq(const uint8_t* p, const char* s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] && p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

/* EISA id (compressed PNP) → 7 chars. */
static void eisa(uint32_t v, char out[8]) {
    v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
    out[0] = (char)('@' + ((v >> 26) & 31)); out[1] = (char)('@' + ((v >> 21) & 31));
    out[2] = (char)('@' + ((v >> 16) & 31));
    const char* hx = "0123456789ABCDEF";
    out[3] = hx[(v >> 12) & 15]; out[4] = hx[(v >> 8) & 15];
    out[5] = hx[(v >> 4) & 15];  out[6] = hx[v & 15]; out[7] = 0;
}

static int str_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int g_aml_devices, g_aml_used;

/* Every Device() the reader saw, for the `acpi` command — so "which device is
 * this machine's disk controller" is answered by the machine, not recalled. */
#define ACPI_MAX_DEV 64
struct acpi_dev { char hid[9]; uint64_t mem; uint32_t irq; uint8_t used; };
static struct acpi_dev g_dev[ACPI_MAX_DEV];
static int g_ndev;
int acpi_arm_find(const char* hid, int nth, uint64_t* mem, uint32_t* irq);

/* Walk one resource template: the first fixed memory range and the first
 * interrupt, plus (for a PCIe host) the first 32-bit memory address space. */
static void crs(const uint8_t* p, uint32_t n, uint64_t* mem, uint32_t* irq,
                uint64_t* m32, uint64_t* m32len) {
    for (uint32_t o = 0; o < n; ) {
        uint8_t tag = p[o];
        if (!(tag & 0x80)) {                                  /* small item */
            if ((tag >> 3) == 0x0F) break;                    /* end tag    */
            o += 1 + (tag & 7);
            continue;
        }
        if (o + 3 > n) break;
        uint16_t l = r16(p + o + 1);
        const uint8_t* d = p + o + 3;
        if (tag == 0x86 && l >= 9 && !*mem) *mem = r32(d + 1);        /* Memory32Fixed */
        if (tag == 0x89 && l >= 6 && !*irq) *irq = r32(d + 2);        /* Ext. Interrupt */
        if (tag == 0x87 && l >= 23 && d[0] == 0 && !*m32) {           /* DWord memory */
            *m32 = r32(d + 7); *m32len = r32(d + 19);
        }
        o += 3 + l;
    }
}

static void aml_scan(const uint8_t* a, uint32_t n) {
    for (uint32_t i = 0; i + 8 < n; i++) {
        if (a[i] != 0x5B || a[i + 1] != 0x82) continue;       /* DeviceOp   */
        uint32_t nb, plen = pkglen(a + i + 2, &nb);
        uint32_t start = i + 2, end = start + plen;
        if (plen < 8 || end > n) continue;
        g_aml_devices++;
        char hid[9] = { 0 };
        uint64_t mem = 0, m32 = 0, m32len = 0;
        uint32_t irq = 0;
        /* Up to the next nested Device: this device's own names only. */
        for (uint32_t k = start + nb; k + 6 < end; k++) {
            if (a[k] == 0x5B && a[k + 1] == 0x82) break;
            if (a[k] == 0x08 && seg_eq(a + k + 1, "_HID") && !hid[0]) {
                if (a[k + 5] == 0x0D) {                       /* string */
                    for (int c = 0; c < 8 && a[k + 6 + c]; c++) hid[c] = (char)a[k + 6 + c];
                } else if (a[k + 5] == 0x0C) {                /* EISA id dword */
                    eisa(r32(a + k + 6), hid);
                }
            }
            if (a[k] == 0x08 && seg_eq(a + k + 1, "_CRS") && a[k + 5] == 0x11) {
                uint32_t nb2, blen = pkglen(a + k + 6, &nb2);
                const uint8_t* b = a + k + 6 + nb2;           /* BufferSize term */
                uint32_t skip = b[0] == 0x0A ? 2 : b[0] == 0x0B ? 3 : b[0] == 0x0C ? 5 : 1;
                if (blen > nb2 + skip)
                    crs(b + skip, blen - nb2 - skip, &mem, &irq, &m32, &m32len);
            }
        }
        int used0 = g_aml_used;
        if (str_eq(hid, "LNRO0005") && mem && g_board.nvirtio < BOARD_MAX_VIRTIO) {
            g_board.virtio_base[g_board.nvirtio]  = mem;
            g_board.virtio_intid[g_board.nvirtio] = irq;
            g_board.nvirtio++; g_aml_used++;
        } else if ((str_eq(hid, "PNP0A08") || str_eq(hid, "PNP0A03")) && m32 && !g_board.pci_mmio32) {
            g_board.pci_mmio32 = m32; g_board.pci_mmio32_size = m32len; g_aml_used++;
        } else if (str_eq(hid, "LNRO0004") && mem && !g_board.rtc) {   /* PL031 */
            g_board.rtc = mem; g_aml_used++;
        }
        if (g_ndev < ACPI_MAX_DEV && hid[0]) {
            struct acpi_dev* dv = &g_dev[g_ndev++];
            for (int c = 0; c < 9; c++) dv->hid[c] = hid[c];
            dv->mem = mem; dv->irq = irq; dv->used = g_aml_used != used0;
        }
    }
}

/* ---- entry ------------------------------------------------------------------ */
int acpi_arm_init(uint64_t rsdp_phys) {
    const uint8_t* rsdp = reach(rsdp_phys, "RSDP");
    if (!rsdp || r32(rsdp) != 0x20445352u /* "RSD " */) return -1;
    uint64_t xsdt_phys = rsdp[15] >= 2 ? r64(rsdp + 24) : 0;
    const uint8_t* x = reach(xsdt_phys, "XSDT");
    if (!x || !csum_ok(x, r32(x + 4))) { kprintf("acpi: no valid XSDT\n"); return -1; }

    g_board.src = BOARD_ACPI;
    g_board.psci_smc = 0;
    uint32_t n = (r32(x + 4) - 36) / 8;
    kprintf("acpi: XSDT with %u table(s):", n);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* t = reach(r64(x + 36 + 8 * i), "table");
        if (!t) continue;
        uint32_t len = r32(t + 4);
        kprintf(" %c%c%c%c", t[0], t[1], t[2], t[3]);
        if (!csum_ok(t, len)) { kprintf("(bad checksum)"); continue; }
        if (seg_eq(t, "APIC")) parse_madt(t, len);
        else if (seg_eq(t, "GTDT")) parse_gtdt(t, len);
        else if (seg_eq(t, "SPCR")) parse_spcr(t, len);
        else if (seg_eq(t, "MCFG")) parse_mcfg(t, len);
        else if (seg_eq(t, "FACP")) parse_fadt(t, len);
    }
    kprintf("\n");
    const uint8_t* d = reach(g_dsdt, "DSDT");
    if (d && seg_eq(d, "DSDT")) {
        aml_scan(d + 36, r32(d + 4) - 36);
        kprintf("acpi: DSDT - %d device(s) seen, %d used (virtio %d); PSCI via %s; %u CPU(s)\n",
                g_aml_devices, g_aml_used, g_board.nvirtio,
                g_board.psci_smc ? "SMC" : "HVC", g_ncpu_acpi);
    }
    return 0;
}

/* A device the DSDT describes by _HID (the nth one), for drivers of platform
 * devices that no bus enumerates — sbsa-ref's AHCI and xHCI sit on its system
 * bus.  0 and its window + interrupt, or -1. */
int acpi_arm_find(const char* hid, int nth, uint64_t* mem, uint32_t* irq) {
    for (int i = 0; i < g_ndev; i++)
        if (str_eq(g_dev[i].hid, hid) && g_dev[i].mem && nth-- == 0) {
            *mem = g_dev[i].mem; *irq = g_dev[i].irq; g_dev[i].used = 1;
            return 0;
        }
    return -1;
}

#include "shellcmd.h"
static void cmd_acpi(const char* args) {
    (void)args;
    if (g_board.src != BOARD_ACPI) { kprintf("acpi: this machine was not described by ACPI\n"); return; }
    kprintf("acpi: %d device(s) in the DSDT (%d seen, reader limit %d)\n", g_ndev, g_aml_devices, ACPI_MAX_DEV);
    for (int i = 0; i < g_ndev; i++)
        kprintf("  %s  mem %p  irq %u  %s\n", g_dev[i].hid, (void*)(uintptr_t)g_dev[i].mem,
                g_dev[i].irq, g_dev[i].used ? "used" : "-");
}
SHELL_CMD(acpi) = { "acpi", "", "the devices this machine's ACPI tables describe",
                    SHELL_G_DEV, cmd_acpi, SHELL_P_ANY };
