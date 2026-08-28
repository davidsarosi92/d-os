/* =============================================================================
 * iommu.c — §M33 stage 5, first half: discover the DMA remapping hardware and
 * report what it can do, WITHOUT touching the isolation verdict.
 *
 * See iommu.h for the rule this file is written under.  The short form: finding
 * an IOMMU must not make anything report better isolation, because an IOMMU
 * sitting in passthrough restricts nothing.  What detection buys is the ability
 * to tell "this machine cannot do it" from "this machine can and we have not
 * built it yet" — two different facts that call for two different decisions and
 * that were indistinguishable while nobody had looked.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS READ, AND WHY EACH FIELD IS KEPT RATHER THAN REDUCED
 *
 * The DMAR table lists remapping units (DRHDs), each a register window.  Two
 * registers in that window describe the unit: CAP and ECAP.  We keep the raw
 * values and three decoded facts, because the interesting failures are
 * SPECIFIC: a unit that offers no page-table depth we can build is a different
 * problem from one with no domain IDs, and "unusable" alone would send somebody
 * looking in the wrong place.
 *
 * SAGAW is the field that decides whether stage 5's second half is possible at
 * all: it says which second-level page-table depths the unit supports.  A
 * 3-level (39-bit) or 4-level (48-bit) table is what we would build; a unit
 * offering neither cannot be programmed by anything we would write, and saying
 * so now is cheaper than finding out halfway through building it.
 *
 * -----------------------------------------------------------------------------
 * THE ONE PIECE OF CARE THAT IS NOT OBVIOUS
 *
 * The register window has to be MAPPED before it is read, and it lives high in
 * physical memory (0xFED90000 on QEMU) where i386's identity map does not
 * reach.  ACPI's own tables had exactly this problem and §M48 fixed it there by
 * mapping on demand; the same applies here, and getting it wrong is not a wrong
 * answer but a ring-0 page fault during boot — a dead machine before the shell.
 * ============================================================================= */

#include "iommu.h"
#include "acpi.h"
#include "printf.h"
#include "klog.h"
#include "vmm.h"
#include "hal_api.h"   /* KERNEL_DIRECT_MAP_BASE / phys_to_virt */
#include <stddef.h>

static struct iommu_info g_info;
static int               g_done;

#if defined(__i386__) || defined(__x86_64__)
/* ---------------------------------------------------------------------- */
/* DMAR table layout (Intel VT-d spec, §8).                               */
/* ---------------------------------------------------------------------- */

struct dmar_header {
    char     signature[4];          /* "DMAR" */
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    char     creator_id[4];
    uint32_t creator_revision;
    uint8_t  host_addr_width;       /* width - 1, so 38 means 39 bits */
    uint8_t  flags;
    uint8_t  reserved[10];
} __attribute__((packed));

/* Every remapping structure starts with a type and a length; walking by the
 * declared length rather than by a known size is what keeps an unknown entry
 * type from desynchronising the whole walk.  §M60's RIFF lesson, one table
 * format over. */
struct dmar_entry {
    uint16_t type;
    uint16_t length;
} __attribute__((packed));

#define DMAR_TYPE_DRHD  0

struct dmar_drhd {
    struct dmar_entry hdr;
    uint8_t  flags;                 /* bit 0 = INCLUDE_PCI_ALL */
    uint8_t  reserved;
    uint16_t segment;
    uint64_t register_base;
} __attribute__((packed));

#define DRHD_INCLUDE_PCI_ALL 0x01

/* Register offsets inside a remapping unit's window (VT-d spec §10.4). */
#define DMAR_REG_CAP   0x08
#define DMAR_REG_ECAP  0x10

/* Map the unit's register window.
 *
 * TWO THINGS HERE ARE NOT OPTIONAL, and the first version got both wrong in the
 * same line by copying ACPI's table-reaching helper.
 *
 * **`phys_to_virt` IS THE WRONG ANSWER FOR MMIO.**  x86_64's direct map covers
 * RAM; a remapping unit lives at 0xFED90000, which is not RAM, so the direct-map
 * address for it is not mapped at all.  The first boot with an IOMMU attached
 * took a ring-0 page fault at 0xffff8000fed90008 — dead before the shell.  The
 * lapic and ioapic drivers had this right already: identity-map the page and use
 * the physical address as the pointer, which works the same on both arches.
 *
 * **`VMM_CACHE_DIS` IS LOAD-BEARING.**  A device register mapped cacheable reads
 * back whatever the CPU cached the first time, so a capability register looks
 * plausible and stops tracking the hardware — the failure is silent and looks
 * like the device lying to you. */
static volatile uint8_t* reach_regs(uint64_t phys) {
    uintptr_t p = (uintptr_t)phys;
    if (p != phys) return NULL;              /* above this arch's reach */
    uintptr_t aligned = p & ~(uintptr_t)0xFFF;
    if (vmm_map(aligned, aligned, VMM_WRITABLE | VMM_CACHE_DIS) != 0)
        return NULL;
    return (volatile uint8_t*)p;
}

static uint64_t reg64(volatile uint8_t* base, int off) {
    /* Two 32-bit reads rather than one 64-bit.  On i386 there is no 64-bit
     * load, and the compiler would synthesise exactly this — spelling it out
     * keeps both arches reading the register the same way, which matters for a
     * register whose halves must not be interleaved with anything else. */
    volatile uint32_t* p = (volatile uint32_t*)(base + off);
    uint32_t lo = p[0], hi = p[1];
    return ((uint64_t)hi << 32) | lo;
}
#endif /* x86 */

const char* iommu_state_name(enum iommu_state s) {
    switch (s) {
    case IOMMU_ACTIVE:   return "active";
    case IOMMU_PRESENT:  return "present, NOT programming it";
    case IOMMU_UNUSABLE: return "present but unusable";
    default:             return "none";
    }
}

void iommu_init(void) {
    if (g_done) return;
    g_done = 1;
    g_info.state = IOMMU_NONE;
    g_info.why   = "no DMAR table — this machine has no DMA remapping hardware";

#if !defined(__i386__) && !defined(__x86_64__)
    /* AARCH64 ANSWERS "NONE", AND THE REASON IS NAMED RATHER THAN IMPLIED.
     *
     * The ARM analogue is the SMMU, described by the IORT table (or the device
     * tree on a machine that boots without ACPI, which is how this port boots).
     * Nothing here looks for it, so the honest answer is that we do not know of
     * one — which for every purpose that consults this is the same as not having
     * one, because an IOMMU nobody has found programs nothing.
     *
     * Stated as unlooked-for rather than absent, because those become different
     * facts the moment somebody asks why ARM cannot place a DMA driver. */
    g_info.why = "no IOMMU support on this arch — the ARM analogue is an SMMU "
                 "(IORT / device tree) and nothing here looks for one yet";
    klog(KLOG_INFO, "iommu", "not searched for on this arch — DMA is "
                             "unrestricted\n");
    return;
#else
    const struct dmar_header* d = (const struct dmar_header*)acpi_dmar();
    if (!d) {
        klog(KLOG_INFO, "iommu", "no DMAR — DMA is unrestricted on this machine\n");
        return;
    }

    g_info.host_addr_width = (int)d->host_addr_width + 1;

    /* Walk the remapping structures BY THEIR DECLARED LENGTH.  An unknown type
     * is skipped, never guessed at: skipping by a size we assumed would
     * desynchronise the walk and produce confident nonsense from the entries
     * after it. */
    const uint8_t* p   = (const uint8_t*)d + sizeof *d;
    const uint8_t* end = (const uint8_t*)d + d->length;
    while (p + sizeof(struct dmar_entry) <= end) {
        const struct dmar_entry* e = (const struct dmar_entry*)p;
        if (e->length < sizeof *e) break;            /* malformed — stop */
        if (e->type == DMAR_TYPE_DRHD && e->length >= sizeof(struct dmar_drhd)) {
            const struct dmar_drhd* h = (const struct dmar_drhd*)p;
            g_info.units++;
            if (h->flags & DRHD_INCLUDE_PCI_ALL) g_info.scope_all = 1;
            if (!g_info.reg_base) g_info.reg_base = h->register_base;
        }
        p += e->length;
    }

    if (!g_info.units || !g_info.reg_base) {
        g_info.state = IOMMU_UNUSABLE;
        g_info.why   = "a DMAR with no usable remapping unit in it";
        klog(KLOG_WARN, "iommu", "DMAR present but declares no usable unit\n");
        return;
    }

    volatile uint8_t* regs = reach_regs(g_info.reg_base);
    if (!regs) {
        g_info.state = IOMMU_UNUSABLE;
        g_info.why   = "its register window is beyond this arch's reach";
        return;
    }

    g_info.cap  = reg64(regs, DMAR_REG_CAP);
    g_info.ecap = reg64(regs, DMAR_REG_ECAP);

    /* CAP.SAGAW is bits 8..12: which second-level page-table depths this unit
     * supports.  Bit 1 = 3-level (39-bit addresses), bit 2 = 4-level (48-bit).
     * These are the two we could build; a unit offering neither cannot be
     * programmed by anything we would write, and finding that out here is much
     * cheaper than finding it out with the page tables half written. */
    uint32_t sagaw = (uint32_t)((g_info.cap >> 8) & 0x1F);
    g_info.sagaw_39 = (sagaw & 0x2) ? 1 : 0;
    g_info.sagaw_48 = (sagaw & 0x4) ? 1 : 0;

    /* CAP.ND is bits 0..2: domain-id width, encoded as 4 + 2*ND bits. */
    uint32_t nd = (uint32_t)(g_info.cap & 0x7);
    g_info.domains = (nd <= 6) ? (1 << (4 + 2 * nd)) : 0;

    if (g_info.cap == 0 || g_info.cap == 0xFFFFFFFFFFFFFFFFull) {
        g_info.state = IOMMU_UNUSABLE;
        g_info.why   = "its capability register reads back as all-ones or zero "
                       "— the window is not decoding";
        return;
    }
    if (!g_info.sagaw_39 && !g_info.sagaw_48) {
        g_info.state = IOMMU_UNUSABLE;
        g_info.why   = "it offers no second-level page-table depth we could "
                       "build (neither 3- nor 4-level)";
        return;
    }

    /* PRESENT, and deliberately not more than that.
     *
     * The hardware is here and it is capable.  Nothing is programmed, so every
     * device still reads every byte of memory — which is exactly as exposed as
     * a machine with no IOMMU at all.  The state is named for what is TRUE
     * rather than for what is possible, because the difference between those
     * two is the whole subject of this milestone. */
    g_info.state = IOMMU_PRESENT;
    g_info.why   = "found and usable, but translation is NOT enabled — every "
                   "device still reads all of memory";
    klog(KLOG_INFO, "iommu",
         "VT-d at %x: %d unit(s), %d-bit host, sagaw%s%s, %d domains — "
         "NOT programmed\n",
         (unsigned)g_info.reg_base, g_info.units, g_info.host_addr_width,
         g_info.sagaw_39 ? " 39" : "", g_info.sagaw_48 ? " 48" : "",
         g_info.domains);
#endif
}

const struct iommu_info* iommu_get(void) { return &g_info; }

void iommu_report(void) {
    const struct iommu_info* i = &g_info;
    kprintf("iommu: %s\n", iommu_state_name(i->state));
    kprintf("  %s\n", i->why ? i->why : "?");
    if (i->state == IOMMU_NONE) {
        /* Say what it MEANS, not just what was found.  "No DMAR table" is a
         * fact about firmware; "any device can read any byte" is the fact the
         * reader actually needs, and the two are not obviously the same
         * sentence to anybody who has not read the VT-d spec. */
        kprintf("  consequence: a device can read and write ALL of memory, so a\n"
                "  DMA-capable driver is not isolated by being placed in ring 3\n");
        return;
    }
    kprintf("  units %d, register base %x, host address width %d bits\n",
            i->units, (unsigned)i->reg_base, i->host_addr_width);
    kprintf("  cap %x%x ecap %x%x\n",
            (unsigned)(i->cap >> 32), (unsigned)i->cap,
            (unsigned)(i->ecap >> 32), (unsigned)i->ecap);
    kprintf("  page-table depths: %s%s, domains %d, scope %s\n",
            i->sagaw_39 ? "3-level " : "", i->sagaw_48 ? "4-level" : "",
            i->domains, i->scope_all ? "all PCI devices" : "listed devices only");
    if (i->state == IOMMU_PRESENT)
        kprintf("  consequence: UNCHANGED until translation is enabled — a\n"
                "  capable IOMMU in passthrough restricts nothing\n");
}
