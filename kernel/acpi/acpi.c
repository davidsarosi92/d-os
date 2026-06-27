/* =============================================================================
 * acpi.c — ACPI table discovery + minimal _S5_ (soft-off) support.
 *
 * ACPI is how every x86 machine newer than ~2000 exposes firmware-controlled
 * power, thermal, and bus information to the OS.  The way in is a chain of
 * tables that starts from a small pointer structure (the RSDP) planted in
 * low memory by the BIOS; from there we follow size-prefixed descriptor
 * tables until we find what we need.
 *
 * For this milestone we care about exactly one thing: powering the machine
 * off by triggering ACPI sleep state S5 ("soft off").  That requires:
 *
 *   1. Find the RSDP (contains a pointer to the RSDT).
 *   2. Walk the RSDT, grabbing the FADT ("FACP" signature).
 *   3. From the FADT, read PM1a_CNT / PM1b_CNT port addresses and the
 *      address of the DSDT.
 *   4. In the DSDT's AML bytecode, locate the `_S5_` object and pick out
 *      SLP_TYPa and SLP_TYPb (the two per-sleep-state magic numbers the
 *      chipset wants back).
 *   5. Write `(SLP_TYP << 10) | (1 << 13)` to the PM1a_CNT (and PM1b_CNT,
 *      if present and non-zero) register to actually trigger the transition.
 *
 * We do NOT implement a real AML interpreter.  Instead, `parse_s5` relies on
 * the extremely common encoding `_S5_` + optional NameOp + PackageOp + small
 * byte constants — which handles QEMU's SeaBIOS, almost every physical
 * board shipped in the last twenty years, and fails gracefully otherwise.
 *
 * References:
 *   - ACPI Specification v6.5, §5.2 (RSDP), §5.2.5 (RSDT), §5.2.9 (FADT),
 *     §5.2.11.3 (DSDT), §7.3.4.2 (`_S5_` object), §4.8.3.2 (PM1 control).
 *   - OSDev Wiki, "ACPI" and "Shutdown".
 *
 * The RSDT we support is revision 0 (ACPI 1.0) with 32-bit physical entry
 * pointers.  XSDT (64-bit) and full 64-bit ACPI machines will need a later
 * pass; they are out of scope for a 32-bit kernel anyway.
 * =========================================================================== */

#include "acpi.h"
#include "hal.h"
#include "printf.h"
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Table layouts.  All are packed because ACPI defines them byte-for-byte and
 * we read them through raw physical pointers — any compiler-added padding
 * would misalign our loads.                                                 */
/* ------------------------------------------------------------------------- */

/* Root System Description Pointer (ACPI 1.0, 20 bytes).  The signature is
 * exactly the 8-byte ASCII "RSD PTR " (note the trailing space).  The
 * checksum byte is chosen so the sum of the first 20 bytes is zero mod 256. */
struct rsdp {
    char     signature[8];              /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;                  /* 0 for ACPI 1.0, 2 for 2.0+ */
    uint32_t rsdt_address;              /* physical address of the RSDT */
} __attribute__((packed));

/* Every descriptor table (RSDT, FADT, DSDT, SSDT, ...) starts with this
 * 36-byte header.  `length` covers the whole table, header included. */
struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;                  /* sum of first `length` bytes == 0 */
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Fixed ACPI Description Table.  Only the prefix up to PM1b_CNT is defined
 * here — the real FADT is much longer but later fields (C-state latencies,
 * GPE blocks, 64-bit extensions, ...) are not used by this milestone.
 * Keeping the struct short reduces the surface for packing mistakes. */
struct fadt {
    struct sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;                      /* physical address of the DSDT */
    uint8_t  _pad1;                     /* reserved in ACPI 1.0 */
    uint8_t  preferred_pm_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;        /* PM1a_CNT — where we write S5 */
    uint32_t pm1b_control_block;        /* PM1b_CNT — optional second half */
} __attribute__((packed));

/* ------------------------------------------------------------------------- */
/* Cached state filled by acpi_init and consumed by acpi_shutdown.           */
/* ------------------------------------------------------------------------- */

static const struct fadt* g_fadt      = 0;
static uint16_t           g_pm1a_cnt  = 0;
static uint16_t           g_pm1b_cnt  = 0;
static const uint8_t*     g_dsdt_body = 0;   /* bytes immediately after the DSDT SDT header */
static uint32_t           g_dsdt_len  = 0;   /* length of that body region */
static uint8_t            g_slp_typ_a = 0;   /* SLP_TYPa value to write */
static uint8_t            g_slp_typ_b = 0;   /* SLP_TYPb value to write */
static int                g_have_s5   = 0;   /* set to 1 once parse_s5 succeeds */

/* ------------------------------------------------------------------------- */
/* Helpers.                                                                  */
/* ------------------------------------------------------------------------- */

/* ACPI tables carry a checksum byte such that the entire table sums to
 * zero modulo 256.  A mismatch means either corruption or we picked up
 * garbage at a wrong address, and we must reject the table. */
static int checksum_ok(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += b[i];
    return sum == 0;
}

/* Compare two 4-byte ACPI signatures by character (avoids pulling in
 * memcmp and makes the intent obvious at call sites). */
static int sig4(const char* a, const char* b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3];
}

/* 8-byte variant for the "RSD PTR " signature in the RSDP. */
static int sig8(const char* a, const char* b) {
    for (int i = 0; i < 8; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Search a physical memory window, 16-byte aligned, for the RSDP signature
 * followed by a matching checksum.  Returns the first valid RSDP or NULL. */
static const struct rsdp* scan_rsdp_in(uint32_t start, uint32_t end) {
    for (uint32_t a = start; a + sizeof(struct rsdp) <= end; a += 16) {
        const struct rsdp* r = (const struct rsdp*)a;
        if (sig8(r->signature, "RSD PTR ") && checksum_ok(r, 20)) return r;
    }
    return 0;
}

/* Two canonical places the BIOS may put the RSDP, per ACPI §5.2.5.1:
 *   1. The first kilobyte of the Extended BIOS Data Area (EBDA), whose
 *      base segment is stored at physical 0x40E in the real-mode BDA.
 *   2. The range 0xE0000 – 0xFFFFF (the upper 128 KiB of legacy BIOS ROM).
 * Always 16-byte aligned.  We try (1) first since it's cheaper and, on
 * most real firmware, actually contains the pointer. */
static const struct rsdp* find_rsdp(void) {
    uint32_t ebda = ((uint32_t)(*(volatile uint16_t*)0x40E)) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        const struct rsdp* r = scan_rsdp_in(ebda, ebda + 1024);
        if (r) return r;
    }
    return scan_rsdp_in(0xE0000, 0x100000);
}

/* -------------------------------------------------------------------------
 * parse_s5 — tiny AML peephole parser.
 *
 * We scan the DSDT byte-by-byte looking for the four-character tag "_S5_".
 * When found, the bytes that follow typically encode a Name declaration
 * holding a Package of four ByteConsts.  The first two ByteConsts are the
 * SLP_TYPa / SLP_TYPb values the chipset expects to see on the way into
 * sleep state 5.  Layouts we accept:
 *
 *   _S5_  08  12  <pkglen> <num_elems> <entry_a> <entry_b> ...
 *              \_/           \____________________________/
 *          (sometimes absent)         two ByteConsts
 *
 * where each entry is one of:
 *   0x0A <byte>   — ByteConst, value = byte
 *   0x00          — ZeroOp,    value = 0
 *   0x01          — OneOp,     value = 1
 *
 * This is intentionally dumb but handles every board we've met.  A real
 * AML interpreter will be needed if we ever want to read runtime method
 * results, but _S5_ in practice is always a constant package.
 *
 * `pkglen` is variable-length (1–4 bytes); only the first byte's top two
 * bits matter for figuring out how much to skip.
 * ------------------------------------------------------------------------- */
static int parse_s5(const uint8_t* start, uint32_t len,
                    uint8_t* a_out, uint8_t* b_out) {
    for (uint32_t i = 0; i + 5 < len; i++) {
        if (start[i]!='_' || start[i+1]!='S' || start[i+2]!='5' || start[i+3]!='_') {
            continue;
        }
        const uint8_t* p = start + i + 4;

        if (*p == 0x08) p++;                /* NameOp — sometimes present */
        if (*p != 0x12) continue;           /* expect PackageOp */
        p++;

        uint8_t lead = *p++;                /* PkgLength leading byte */
        p += (lead >> 6) & 0x3;             /* skip `follow` extra length bytes */
        p++;                                /* NumElements byte */

        uint8_t vals[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            if      (*p == 0x0A) { p++; vals[k] = *p++; }
            else if (*p == 0x00) { p++; vals[k] = 0; }
            else if (*p == 0x01) { p++; vals[k] = 1; }
            else                 { return -1; }
        }
        *a_out = vals[0];
        *b_out = vals[1];
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Public interface.                                                         */
/* ------------------------------------------------------------------------- */

/* Discover ACPI tables and extract everything we need for shutdown.
 * Returns 0 on success, -1 on any failure; on failure the cached state is
 * left in its init-to-zero form so `acpi_shutdown` safely no-ops. */
int acpi_init(void) {
    const struct rsdp* rsdp = find_rsdp();
    if (!rsdp) {
        kprintf("ACPI: RSDP not found\n");
        return -1;
    }

    const struct sdt_header* rsdt = (const struct sdt_header*)rsdp->rsdt_address;
    if (!sig4(rsdt->signature, "RSDT") || !checksum_ok(rsdt, rsdt->length)) {
        kprintf("ACPI: bad RSDT at %p\n", (void*)rsdt);
        return -1;
    }

    /* RSDT payload is an array of 32-bit physical pointers, one per table.
     * Count = (table length − header length) / 4. */
    uint32_t n = (rsdt->length - sizeof(struct sdt_header)) / 4;
    const uint32_t* entries = (const uint32_t*)(rsdt + 1);
    for (uint32_t i = 0; i < n; i++) {
        const struct sdt_header* h = (const struct sdt_header*)entries[i];
        if (sig4(h->signature, "FACP") && checksum_ok(h, h->length)) {
            g_fadt = (const struct fadt*)h;
            break;
        }
    }
    if (!g_fadt) {
        kprintf("ACPI: FADT not found\n");
        return -1;
    }

    /* I/O port addresses are 32-bit in the FADT but always fit in 16 bits
     * in practice; cast explicitly to avoid a warning later. */
    g_pm1a_cnt = (uint16_t)g_fadt->pm1a_control_block;
    g_pm1b_cnt = (uint16_t)g_fadt->pm1b_control_block;

    const struct sdt_header* dsdt = (const struct sdt_header*)g_fadt->dsdt;
    if (!sig4(dsdt->signature, "DSDT") || !checksum_ok(dsdt, dsdt->length)) {
        kprintf("ACPI: bad DSDT at %p\n", (void*)dsdt);
        return -1;
    }
    g_dsdt_body = (const uint8_t*)(dsdt + 1);
    g_dsdt_len  = dsdt->length - sizeof(struct sdt_header);

    if (parse_s5(g_dsdt_body, g_dsdt_len, &g_slp_typ_a, &g_slp_typ_b) == 0) {
        g_have_s5 = 1;
        kprintf("ACPI: PM1a=%x PM1b=%x SLP_TYPa=%d SLP_TYPb=%d\n",
                g_pm1a_cnt, g_pm1b_cnt, g_slp_typ_a, g_slp_typ_b);
    } else {
        kprintf("ACPI: _S5_ not parseable (PM1a=%x)\n", g_pm1a_cnt);
    }
    return 0;
}

/* Trigger S5.  Must be called from ring 0 with interrupts in whatever state
 * we want the firmware to see; we don't try to mask them here since this
 * kernel hasn't brought IDT/PIC up yet.  On a successful shutdown this
 * function never returns — the chipset powers the machine down between
 * our I/O write and the next instruction fetch. */
void acpi_shutdown(void) {
    if (!g_have_s5 || !g_pm1a_cnt) return;

    /* PM1_CNT encoding (ACPI §4.8.3.2.1):
     *   bits 10..12 — SLP_TYP  (sleep state selector)
     *   bit 13      — SLP_EN   (commit: transition on next opcode)
     * All other bits are hardware-specific; writing zeros is safe. */
    uint16_t val_a = ((uint16_t)g_slp_typ_a << 10) | (1 << 13);
    outw(g_pm1a_cnt, val_a);

    /* Some chipsets split PM1 across two register blocks; write the b half
     * only if the FADT actually provided a PM1b address. */
    if (g_pm1b_cnt) {
        uint16_t val_b = ((uint16_t)g_slp_typ_b << 10) | (1 << 13);
        outw(g_pm1b_cnt, val_b);
    }
}
