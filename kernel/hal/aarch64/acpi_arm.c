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

/* ---- §M85: PCI legacy interrupts (_PRT) ---------------------------------------
 *
 * A PCI function's INTx pin reaches the GIC through the host bridge's _PRT: a
 * package of {address (slot << 16 | 0xFFFF), pin (0 = INTA), source, index}.
 * `source` is either 0 — then `index` IS the GSI — or the NAME of a link device
 * (PNP0C0F), whose resource template carries the GSI.  On sbsa-ref both the
 * _PRT and the link devices' _PRS are STATIC data (their _CRS is a method that
 * returns _PRS), so this reader takes them without executing any AML — and says
 * nothing about a machine whose _PRT is a method, which is then reported as
 * "no route" rather than guessed.  On ARM a GSI is the GIC INTID.
 *
 * Found on the way: the firmware's own table routes slot 0 as four entries
 * for pin 0 (INTA) and none for B-D.  The FIRST match wins here, which gives
 * slot 0 INTA its intended link and leaves B-D unrouted, stated rather than
 * patched over. */
#define PRT_MAX 128
struct prt_ent { uint8_t slot, pin; char link[4]; uint32_t gsi; };
static struct prt_ent g_prt[PRT_MAX];
static int g_nprt;
#define LINK_MAX 16
struct link_dev { char name[4]; uint32_t gsi; };
static struct link_dev g_link[LINK_MAX];
static int g_nlink;

/* One AML integer at p: returns bytes consumed (0 = not an integer). */
static uint32_t aml_int(const uint8_t* p, uint64_t* v) {
    switch (p[0]) {
    case 0x00: *v = 0; return 1;                      /* ZeroOp  */
    case 0x01: *v = 1; return 1;                      /* OneOp   */
    case 0xFF: *v = ~0ull; return 1;                  /* OnesOp  */
    case 0x0A: *v = p[1]; return 2;                   /* Byte    */
    case 0x0B: *v = r16(p + 1); return 3;             /* Word    */
    case 0x0C: *v = r32(p + 1); return 5;             /* DWord   */
    case 0x0E: *v = r64(p + 1); return 9;             /* QWord   */
    }
    return 0;
}

/* A NameString at p: copies its LAST NameSeg (the link's own name — the path
 * prefix is not needed, every link we resolve lives in the same scope) and
 * returns bytes consumed, or 0. */
static uint32_t aml_name(const uint8_t* p, char out[4]) {
    uint32_t o = 0;
    while (p[o] == 0x5C || p[o] == 0x5E) o++;        /* root / parent prefix */
    uint32_t segs = 1;
    if (p[o] == 0x2E) { segs = 2; o++; }              /* DualNamePrefix */
    else if (p[o] == 0x2F) { segs = p[o + 1]; o += 2; } /* MultiNamePrefix */
    else if (!((p[o] >= 'A' && p[o] <= 'Z') || p[o] == '_')) return 0;
    for (uint32_t k = 0; k < 4; k++) out[k] = (char)p[o + (segs - 1) * 4 + k];
    return o + segs * 4;
}

static void parse_prt(const uint8_t* p, uint32_t len) {
    uint32_t nb, plen = pkglen(p, &nb);
    if (plen > len) return;
    const uint8_t* end = p + plen;
    uint32_t count = p[nb];
    const uint8_t* q = p + nb + 1;
    for (uint32_t e = 0; e < count && q < end && g_nprt < PRT_MAX; e++) {
        if (q[0] != 0x12) return;                     /* PackageOp */
        uint32_t nb2, l2 = pkglen(q + 1, &nb2);
        const uint8_t* it = q + 1 + nb2 + 1;          /* past NumElements */
        uint64_t addr, pin, idx;
        uint32_t k = aml_int(it, &addr); if (!k) return; it += k;
        k = aml_int(it, &pin);           if (!k) return; it += k;
        struct prt_ent* pe = &g_prt[g_nprt];
        pe->link[0] = 0;
        uint64_t zero;
        if ((k = aml_int(it, &zero)) != 0) it += k;   /* source 0: index = GSI */
        else if ((k = aml_name(it, pe->link)) != 0) it += k;
        else return;
        k = aml_int(it, &idx); if (!k) return;
        pe->slot = (uint8_t)((addr >> 16) & 0x1F);
        pe->pin  = (uint8_t)pin;
        pe->gsi  = pe->link[0] ? 0 : (uint32_t)idx;
        g_nprt++;
        q = q + 1 + l2;
    }
}

/* The GSI a function's INTx pin (1 = INTA .. 4 = INTD, as config space says
 * it) reaches, or -1 when the tables do not route it. */
int acpi_arm_pci_intx(uint8_t slot, uint8_t pin) {
    if (pin < 1 || pin > 4) return -1;
    for (int i = 0; i < g_nprt; i++) {
        struct prt_ent* e = &g_prt[i];
        if (e->slot != slot || e->pin != pin - 1) continue;
        if (!e->link[0]) return (int)e->gsi;
        for (int j = 0; j < g_nlink; j++)
            if (g_link[j].name[0] == e->link[0] && g_link[j].name[1] == e->link[1] &&
                g_link[j].name[2] == e->link[2] && g_link[j].name[3] == e->link[3])
                return g_link[j].gsi ? (int)g_link[j].gsi : -1;
        return -1;
    }
    return -1;
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
        /* This device's OWN names only — a nested Device is SKIPPED by its
         * length, not treated as the end.  Stopping at it (as this did) hid
         * everything a device declares after its children: sbsa-ref's PCI0
         * holds four link devices before its _PRT and _CRS, so the host
         * bridge's window and its interrupt routing were both invisible
         * (§M85's "DSDT PCIe window" open item was this, not the _CRS shape).
         * The outer loop still visits the nested device on its own. */
        char dname[4];
        for (int c = 0; c < 4; c++) dname[c] = (char)a[start + nb + c];
        uint32_t prs_irq = 0;
        for (uint32_t k = start + nb; k + 6 < end; k++) {
            if (a[k] == 0x5B && a[k + 1] == 0x82) {
                uint32_t nb3, l3 = pkglen(a + k + 2, &nb3);
                k = k + 2 + l3 - 1;                   /* the loop's k++ lands past it */
                continue;
            }
            if (a[k] == 0x08 && seg_eq(a + k + 1, "_PRT") && a[k + 5] == 0x12)
                parse_prt(a + k + 6, end - (k + 6));
            if (a[k] == 0x08 && seg_eq(a + k + 1, "_PRS") && a[k + 5] == 0x11) {
                uint32_t nb2, blen = pkglen(a + k + 6, &nb2);
                const uint8_t* b = a + k + 6 + nb2;
                uint32_t skip = b[0] == 0x0A ? 2 : b[0] == 0x0B ? 3 : b[0] == 0x0C ? 5 : 1;
                uint64_t m0 = 0, m1 = 0, m2 = 0;
                if (blen > nb2 + skip)
                    crs(b + skip, blen - nb2 - skip, &m0, &prs_irq, &m1, &m2);
            }
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
        if (str_eq(hid, "PNP0C0F") && g_nlink < LINK_MAX) {   /* PCI link */
            for (int c = 0; c < 4; c++) g_link[g_nlink].name[c] = dname[c];
            g_link[g_nlink].gsi = irq ? irq : prs_irq;
            if (!irq) irq = prs_irq;
            g_nlink++;
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
        kprintf("acpi: PCI routing - %d _PRT entr%s, %d link device(s); 32-bit window %p + %p\n",
                g_nprt, g_nprt == 1 ? "y" : "ies", g_nlink,
                (void*)(uintptr_t)g_board.pci_mmio32, (void*)(uintptr_t)g_board.pci_mmio32_size);
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
    kprintf("PCI INTx routing (_PRT, %d entries): ", g_nprt);
    for (int sl = 0; sl < 4; sl++) {
        kprintf(" slot %d:", sl);
        for (int pn = 1; pn <= 4; pn++) kprintf(" %d", acpi_arm_pci_intx((uint8_t)sl, (uint8_t)pn));
    }
    kprintf("\n");
}
SHELL_CMD(acpi) = { "acpi", "", "the devices this machine's ACPI tables describe",
                    SHELL_G_DEV, cmd_acpi, SHELL_P_ANY };
