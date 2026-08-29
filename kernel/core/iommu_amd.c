/* =============================================================================
 * iommu_amd.c — AMD-Vi (AMD's IOMMU), the second backend.
 *
 * -----------------------------------------------------------------------------
 * WHY A SECOND BACKEND WAS WORTH BUILDING BEFORE ANYBODY NEEDED AMD
 *
 * Not for AMD.  `iommu.h` read as though it were vendor-neutral and was not:
 * the DMAR walk WAS `iommu_init`, and `struct iommu_info` carried `cap`,
 * `ecap`, `sagaw_39`, `sagaw_48` and counted "how many DRHDs".  Every field was
 * Intel's vocabulary in a struct that claimed to speak for the machine.
 *
 * *One implementation behind an interface is not an interface, it is a claim* —
 * and the only way to find out which you have is to write the second one.  This
 * file is the test.  It answers the same questions from a completely different
 * ACPI table (IVRS, not DMAR) with a completely different register layout, and
 * everything above it — `drv domain`, the isolation verdict, the device manager
 * — is unchanged.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS HERE AND WHAT IS NOT, SAID PLAINLY
 *
 * DETECTION AND CAPABILITY REPORTING ARE COMPLETE.  Programming is NOT: this
 * backend leaves `enable` NULL, so `iommu on` refuses with the reason instead
 * of pretending.  AMD-Vi's device table and its I/O page tables are a different
 * format from Intel's root/context tables, and writing them half-understood
 * would produce the one failure §M33 spent a milestone refusing — a boundary
 * that reports itself in place and is not there.
 *
 * The verdict is unaffected either way, and that is the design rather than a
 * consolation.  `domain_isolation_of` deliberately does not consult the IOMMU:
 * finding one must never improve the answer, because an IOMMU in passthrough
 * restricts nothing.  What detection buys is a REASON — and *"this machine has
 * AMD-Vi and we cannot yet drive it"* and *"this machine has none"* leave a
 * driver equally exposed while calling for entirely different decisions.
 *
 * -----------------------------------------------------------------------------
 * THE TABLE (AMD I/O Virtualization Technology Specification, rev 3.05, §5.2)
 *
 * IVRS header: the standard ACPI header, then IVinfo (4 bytes) and 8 reserved.
 * IVinfo bits 15..8 hold the physical address size the unit can translate, and
 * bits 22..16 the virtual address size — the analogue of DMAR's host address
 * width, in a different place and a different encoding.
 *
 * Then a list of IVHD blocks, each: type (1), flags (1), length (2),
 * device id (2), capability offset (2), **IOMMU base address (8)**, PCI segment
 * (2), IOMMU info (2), attributes/feature (4).  Types 0x10, 0x11 and 0x40
 * describe a unit; 0x11 and 0x40 are the extended forms that add an EFR field.
 *
 * WALKED BY DECLARED LENGTH, never by assuming a fixed size — the same rule the
 * DMAR walk follows.  An unknown block type is SKIPPED by its length rather
 * than treated as an error: firmware is allowed to describe things we have not
 * heard of, and a parser that stops at the first unknown block would report no
 * IOMMU on a machine that has one.
 * ============================================================================= */

#include "iommu.h"
#include "iommu_backend.h"
#include "acpi.h"
#include "printf.h"
#include "klog.h"
#include <stddef.h>
#include <stdint.h>

#if defined(__i386__) || defined(__x86_64__)

/* The ACPI header, repeated here rather than shared: this file's business is
 * one table's layout, and reaching into ACPI's private structs would couple two
 * modules that currently agree only on "here is a pointer". */
struct ivrs_header {
    char     signature[4];          /* "IVRS" */
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
    uint32_t ivinfo;
    uint8_t  reserved[8];
} __attribute__((packed));

struct ivhd_header {
    uint8_t  type;
    uint8_t  flags;
    uint16_t length;
    uint16_t device_id;
    uint16_t cap_offset;
    uint64_t base_address;          /* the unit's MMIO window */
    uint16_t pci_segment;
    uint16_t iommu_info;
    uint32_t attributes;
} __attribute__((packed));

#define IVHD_TYPE_10  0x10
#define IVHD_TYPE_11  0x11
#define IVHD_TYPE_40  0x40

/* AMD-Vi's own capability facts, kept private for exactly the reason the VT-d
 * ones were moved out of `struct iommu_info`. */
static uint64_t g_amd_base;
static uint32_t g_amd_ivinfo;
static uint8_t  g_amd_type;
static int      g_amd_units;
static int      g_amd_msi_num;

static int amd_detect(struct iommu_info* out) {
    const struct ivrs_header* t = (const struct ivrs_header*)acpi_ivrs();
    if (!t) return -1;                          /* not this vendor */

    g_amd_ivinfo = t->ivinfo;
    g_amd_units  = 0;
    g_amd_base   = 0;

    /* IVinfo bits 15..8: the physical address size, in bits, that this unit can
     * translate.  Intel puts the same idea in the DMAR header as
     * `host_addr_width` and encodes it as "value + 1"; AMD states it directly.
     * Two vendors, one neutral field — which is the whole point of the seam. */
    int paw = (int)((t->ivinfo >> 8) & 0x7F);

    const uint8_t* p   = (const uint8_t*)t + sizeof(struct ivrs_header);
    const uint8_t* end = (const uint8_t*)t + t->length;

    while (p + sizeof(struct ivhd_header) <= end) {
        const struct ivhd_header* h = (const struct ivhd_header*)p;
        /* A zero or absurd length would spin this loop forever on malformed
         * firmware — bounded rather than trusted, the same discipline §M54
         * applied to the runqueue walks. */
        if (h->length < sizeof(struct ivhd_header)) break;

        if (h->type == IVHD_TYPE_10 || h->type == IVHD_TYPE_11 ||
            h->type == IVHD_TYPE_40) {
            if (!g_amd_base) {
                g_amd_base   = h->base_address;
                g_amd_type   = h->type;
                /* IVHD "IOMMU info" bits 4..0 = the MSI message number; kept
                 * because it is the one field here that says how the unit will
                 * REPORT a fault, and a boundary whose faults nobody can read
                 * is one whose claims nobody can check. */
                g_amd_msi_num = (int)(h->iommu_info & 0x1F);
            }
            g_amd_units++;
        }
        p += h->length;
    }

    out->kind            = "AMD-Vi";
    out->units           = g_amd_units;
    out->reg_base        = g_amd_base;
    out->host_addr_width = paw;
    /* AMD-Vi's device table is indexed by the full 16-bit BDF, so the domain
     * space is 16 bits — stated from the architecture rather than read from a
     * register, and that difference is why it is a neutral field filled by the
     * backend rather than a register value the caller decodes. */
    out->domains     = 65536;
    out->page_levels = 4;
    out->scope_all   = 1;

    if (!g_amd_units || !g_amd_base) {
        out->state = IOMMU_UNUSABLE;
        out->why   = "an IVRS with no usable IOMMU block in it";
        klog(KLOG_WARN, "iommu", "IVRS present but declares no unit\n");
        return 0;                    /* AMD hardware, and this is its state */
    }

    /* PRESENT, AND NOT ONE WORD MORE.
     *
     * The hardware is here.  Nothing is programmed, so every device still reads
     * every byte — exactly as exposed as a machine with no IOMMU at all.  And
     * this backend cannot yet program it either way, which the state must not
     * hide: `programmable` is 0 because `enable` is NULL, and `iommu on`
     * refuses with the reason. */
    out->state = IOMMU_PRESENT;
    out->why   = "AMD-Vi found and readable, but this backend cannot program "
                   "it yet — every device still reads all of memory";
    klog(KLOG_INFO, "iommu",
         "AMD-Vi at %x: %d unit(s), IVHD type %x, %d-bit addresses — "
         "detected, NOT programmed\n",
         (unsigned)g_amd_base, g_amd_units, g_amd_type, paw);
    return 0;
}

static void amd_report_extra(void) {
    kprintf("  ivinfo %x, IVHD type %x, MSI message number %d\n",
            (unsigned)g_amd_ivinfo, g_amd_type, g_amd_msi_num);
    kprintf("  programming AMD-Vi is NOT implemented: its device table and I/O\n"
            "  page tables are a different format from Intel's root/context\n"
            "  tables, and a half-understood one would give a boundary that\n"
            "  reports itself in place and is not there\n");
}

/* NO `enable`, NO `restrict_dev`, and that is a declaration rather than a gap.
 * The dispatcher reads the NULLs and refuses with the reason; a stub returning
 * 0 would report success and leave every device reading all of memory, which is
 * precisely the isolation theatre §M33 refuses by name. */
static const struct iommu_backend g_amd = {
    .name         = "AMD-Vi",
    .detect       = amd_detect,
    .report_extra = amd_report_extra,
};

const struct iommu_backend* iommu_backend_amd(void) { return &g_amd; }

#else   /* not x86 */

/* aarch64 has neither; the dispatcher answers for the arch before it gets
 * here, and this exists so the link does not depend on the arch. */
const struct iommu_backend* iommu_backend_amd(void) { return NULL; }

#endif
