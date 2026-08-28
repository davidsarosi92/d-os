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
#include "pmm.h"
#include "pci.h"
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

/* VIRTIO DEVICES BYPASS THE IOMMU HERE, AND THIS IS THE MOST IMPORTANT SENTENCE
 * IN THE FILE.
 *
 * A virtio device only routes its DMA through the IOMMU once the guest has
 * negotiated VIRTIO_F_ACCESS_PLATFORM (feature bit 33) — which this kernel's
 * virtio drivers do not, and which QEMU's devices default to off.  Without it
 * the device uses physical addresses directly and the remapping hardware never
 * sees the access at all.
 *
 * MEASURED, NOT INFERRED: with translation on, giving the virtio NIC a domain
 * that maps NOTHING left it pinging 3/3 with ZERO fault records, while the same
 * treatment of the AC97 codec produced a refused read the unit recorded by
 * device and address.  The register writes succeeded in both cases.
 *
 * So a report that said "translation is on" and stopped there would let somebody
 * believe every device on the machine was behind a boundary, when the disk and
 * the network — the two that matter most — were not behind it at all.  That is
 * the isolation theatre §M33 refuses, arrived at through hardware rather than
 * through a lie in our code, which makes it harder to see and no less false. */
#define VIRTIO_VENDOR_ID 0x1AF4

static int device_bypasses(uint16_t vendor) { return vendor == VIRTIO_VENDOR_ID; }


struct list_ctx { int translated, bypass; };

static void list_one(const struct pci_device* d, void* vctx) {
    struct list_ctx* c = (struct list_ctx*)vctx;
    int bypass = device_bypasses(d->vendor_id);
    if (bypass) c->bypass++; else c->translated++;
    kprintf("  %x:%x.%x  %x:%x  %s\n", d->bus, d->slot, d->func,
            d->vendor_id, d->device_id,
            bypass ? "BYPASSES the IOMMU (virtio without platform access)"
                   : "translated");
}

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
#if defined(__i386__) || defined(__x86_64__)
    if (i->state == IOMMU_ACTIVE) {
        /* WHICH DEVICES ARE ACTUALLY BEHIND THE BOUNDARY.  "Translation is on"
         * is true and is not the answer somebody needs: a device that bypasses
         * is exactly as exposed as it was before, and it is invisible in every
         * other line of this report. */
        struct list_ctx c = { 0, 0 };
        kprintf("  devices:\n");
        pci_scan(list_one, &c);
        kprintf("  %d translated, %d BYPASSING\n", c.translated, c.bypass);
        if (c.bypass)
            kprintf("  a bypassing device is NOT behind this boundary at all —\n"
                    "  its DMA never reaches the unit, so blocking it does\n"
                    "  nothing and produces no fault record\n");
    }
#endif
}

/* =============================================================================
 * §M33 stage 5, SECOND HALF — programming the unit.
 *
 * -----------------------------------------------------------------------------
 * THE STRUCTURE, IN ONE PARAGRAPH
 *
 * A device is identified by its PCI bus/device/function.  The ROOT TABLE has one
 * entry per bus pointing at a CONTEXT TABLE; the context table has one entry per
 * device:function, and that entry names a DOMAIN — an address space, expressed
 * as second-level page tables that look very much like ordinary x86 ones.  A DMA
 * address from that device is walked through those tables, and an address with
 * no entry is not a wild write: it is a FAULT the unit records and refuses.
 *
 * -----------------------------------------------------------------------------
 * WHY THE FIRST DOMAIN MAPS EVERYTHING, AND WHY THAT IS NOT A PLACEHOLDER
 *
 * Turning translation on with tables that do not cover what the devices are
 * ALREADY doing does not fail politely.  The disk stops answering, the NIC stops
 * answering, and the machine dies without saying why — because every path we
 * would use to find out is itself a device.  There is no console left to print
 * the diagnosis to and no disk to write it on.
 *
 * So the first thing translation does must be to change NOTHING OBSERVABLE: an
 * identity domain over all of RAM, which is what the devices already had.  That
 * makes the enable step falsifiable on its own terms — if the box still boots,
 * mounts, and talks to the network with translation ON, the tables are right.
 * Only then is taking one device's permission away a test of the boundary rather
 * than a test of whether we can crash the machine.
 *
 * -----------------------------------------------------------------------------
 * 2 MiB PAGES, AND THE REASON IS NOT ONLY SIZE
 *
 * Identity-mapping RAM with 4 KiB leaves would cost a page of tables per 2 MiB
 * of RAM — megabytes of tables on a 1 GiB box, allocated at boot, on a path that
 * must not fail.  Superpages make it one leaf entry per 2 MiB.  CAP.SLLPS says
 * whether the unit supports them, and it is CHECKED rather than assumed: a unit
 * without them would read our superpage entry as a pointer to a next-level table
 * and walk into the middle of RAM.  That failure is not a refused DMA, it is a
 * device writing wherever the bits happened to point.
 * ============================================================================= */

#define VTD_REG_GCMD    0x18
#define VTD_REG_GSTS    0x1C
#define VTD_REG_RTADDR  0x20
#define VTD_REG_CCMD    0x28
#define VTD_REG_FSTS    0x34
#define VTD_REG_FECTL   0x38

#define GCMD_TE         (1u << 31)   /* translation enable */
#define GCMD_SRTP       (1u << 30)   /* set root table pointer */
#define GSTS_TES        (1u << 31)
#define GSTS_RTPS       (1u << 30)

/* Context-cache invalidate: global, and the "invalidate" bit is bit 63. */
#define CCMD_ICC        (1ull << 63)
#define CCMD_CIRG_GLOBAL (1ull << 61)

/* Second-level paging entry bits. */
#define SL_READ         (1ull << 0)
#define SL_WRITE        (1ull << 1)
#define SL_SUPERPAGE    (1ull << 7)

#define IOMMU_DOMAIN_DEFAULT  1
#define IOMMU_DOMAIN_FIRST    2      /* restricted domains start here */

static volatile uint8_t* g_regs;
static uint64_t* g_root;             /* 256 root entries, 2 x u64 each */
static uint64_t  g_root_phys;
static uint64_t* g_default_pml4;     /* the identity domain's top level */
static uint64_t  g_default_phys;
static int       g_levels;           /* 3 or 4 */
static int       g_next_domain = IOMMU_DOMAIN_FIRST;

static uint32_t reg32(int off)            { return *(volatile uint32_t*)(g_regs + off); }
static uint64_t reg64_at(int off) {
    volatile uint32_t* p = (volatile uint32_t*)(g_regs + off);
    uint32_t lo = p[0], hi = p[1];
    return ((uint64_t)hi << 32) | lo;
}
static void wreg64_at(int off, uint64_t v) {
    volatile uint32_t* p = (volatile uint32_t*)(g_regs + off);
    p[1] = (uint32_t)(v >> 32);
    p[0] = (uint32_t)v;
}
static void     wreg32(int off, uint32_t v){ *(volatile uint32_t*)(g_regs + off) = v; }
static void     wreg64(int off, uint64_t v) {
    /* Two 32-bit stores, LOW HALF LAST for RTADDR: the unit latches on the write
     * that completes the register, and a half-written pointer latched early is a
     * root table at a garbage address — which is every device losing memory
     * access at once. */
    volatile uint32_t* p = (volatile uint32_t*)(g_regs + off);
    p[1] = (uint32_t)(v >> 32);
    p[0] = (uint32_t)v;
}

/* One zeroed page of table.  Contiguous-of-1 rather than a kmalloc: these are
 * walked by the DEVICE, so they must be physically addressed and page-aligned,
 * and the heap promises neither. */
static uint64_t* table_alloc(uint64_t* out_phys) {
    pmm_phys_t p = pmm_alloc_contiguous(1);
    if (!p) return NULL;
    uint64_t* v = (uint64_t*)phys_to_virt((uintptr_t)p);
    for (int i = 0; i < 512; i++) v[i] = 0;
    *out_phys = (uint64_t)p;
    return v;
}

/* Map [va, va+len) to itself in `top`, using 2 MiB leaves. */
static int domain_map(uint64_t* top, uint64_t base, uint64_t len) {
    for (uint64_t a = base & ~0x1FFFFFull; a < base + len; a += 0x200000ull) {
        uint64_t* tbl = top;
        /* Walk down to the level above the 2 MiB leaf, creating as we go.  The
         * index arithmetic is x86's: 9 bits per level, leaf at level 2. */
        for (int lvl = g_levels; lvl > 2; lvl--) {
            int idx = (int)((a >> (12 + 9 * (lvl - 1))) & 0x1FF);
            if (!(tbl[idx] & SL_READ)) {
                uint64_t np;
                uint64_t* n = table_alloc(&np);
                if (!n) return -1;
                tbl[idx] = np | SL_READ | SL_WRITE;
            }
            tbl = (uint64_t*)phys_to_virt((uintptr_t)(tbl[idx] & ~0xFFFull));
        }
        int idx = (int)((a >> 21) & 0x1FF);
        tbl[idx] = a | SL_READ | SL_WRITE | SL_SUPERPAGE;
    }
    return 0;
}

/* The context entry for one device, created on demand. */
static uint64_t* context_for(uint8_t bus) {
    uint64_t* re = &g_root[bus * 2];
    if (re[0] & 1) return (uint64_t*)phys_to_virt((uintptr_t)(re[0] & ~0xFFFull));
    uint64_t cp;
    uint64_t* c = table_alloc(&cp);
    if (!c) return NULL;
    re[1] = 0;
    re[0] = cp | 1;                 /* present */
    return c;
}

static int context_set(uint16_t bdf, uint64_t sl_phys, int domain) {
    uint64_t* c = context_for((uint8_t)(bdf >> 8));
    if (!c) return -1;
    int devfn = bdf & 0xFF;
    /* AW field: 2 = 48-bit (4-level), 1 = 39-bit (3-level).  It must MATCH the
     * table we actually built — a mismatch makes the unit start its walk at the
     * wrong level, which produces translations that look almost right. */
    uint64_t aw = (g_levels == 4) ? 2 : 1;
    c[devfn * 2 + 1] = ((uint64_t)domain << 8) | aw;
    c[devfn * 2 + 0] = sl_phys | 1;     /* present, translation type 0 */
    return 0;
}

struct assign_ctx { int n; };

static void assign_one(const struct pci_device* d, void* vctx) {
    struct assign_ctx* c = (struct assign_ctx*)vctx;
    uint16_t bdf = (uint16_t)((d->bus << 8) | (d->slot << 3) | d->func);
    if (context_set(bdf, g_default_phys, IOMMU_DOMAIN_DEFAULT) == 0) c->n++;
}

/* TWO CACHES, AND MISSING THE SECOND ONE IS A SILENT NO-OP.
 *
 * The context cache holds "which domain is this device in"; the IOTLB holds the
 * translations themselves.  The first version invalidated only the context
 * cache, and the result was the most misleading possible outcome: `iommu block`
 * reported success, the unit accepted every register write, and the device
 * CARRIED ON WORKING — three pings, 3/3 replies, from a NIC that had just been
 * told it could reach nothing.  A boundary that reports itself in place and is
 * not there is exactly what this milestone exists to refuse, and it was one
 * missing register away.
 *
 * The IOTLB registers are not at a fixed offset: ECAP.IRO gives it, in 16-byte
 * units.  Hard-coding an offset would work on this emulator and read a
 * different register on somebody's chipset. */
static void invalidate_all(void) {
    /* Context cache, globally. */
    wreg64(VTD_REG_CCMD, CCMD_ICC | CCMD_CIRG_GLOBAL);
    for (int i = 0; i < 1000000; i++)
        if (!(*(volatile uint32_t*)(g_regs + VTD_REG_CCMD + 4) & (1u << 31))) break;

    /* IOTLB, globally.  IVT (bit 63) starts it, IIRG = 01 (bits 61:60) means
     * global, and the unit clears IVT when it is done. */
    int iro = (int)(((g_info.ecap >> 8) & 0x3FF) * 16);
    int iotlb = iro + 8;
    wreg64(iotlb, (1ull << 63) | (1ull << 60));
    for (int i = 0; i < 1000000; i++)
        if (!(*(volatile uint32_t*)(g_regs + iotlb + 4) & (1u << 31))) break;
}

int iommu_enable(void) {
#if !defined(__i386__) && !defined(__x86_64__)
    return -1;
#else
    if (g_info.state == IOMMU_ACTIVE) return 0;
    if (g_info.state != IOMMU_PRESENT) {
        kprintf("iommu: cannot enable — %s\n", g_info.why ? g_info.why : "?");
        return -1;
    }
    g_regs = reach_regs(g_info.reg_base);
    if (!g_regs) return -1;

    /* CAP.SLLPS bit 0 = 2 MiB superpages.  CHECKED, not assumed: a unit without
     * them reads our leaf as a pointer to another table and walks into the
     * middle of RAM — not a refused DMA but a device writing wherever the bits
     * point, which is the failure this whole milestone exists to prevent. */
    if (!((g_info.cap >> 34) & 0x1)) {
        kprintf("iommu: this unit has no 2 MiB superpages — refusing to enable\n"
                "  (4 KiB leaves would need megabytes of tables at boot, on a\n"
                "   path that must not fail; unimplemented rather than risked)\n");
        return -1;
    }

    g_levels = g_info.sagaw_48 ? 4 : 3;

    g_root = table_alloc(&g_root_phys);
    if (!g_root) return -1;
    g_default_pml4 = table_alloc(&g_default_phys);
    if (!g_default_pml4) return -1;

    /* THE IDENTITY DOMAIN.  Every frame the PMM knows about, mapped to itself,
     * because that is exactly what the devices had a moment ago. */
    uint64_t ram = (uint64_t)pmm_nr_frames * 4096ull;
    if (domain_map(g_default_pml4, 0, ram) != 0) {
        kprintf("iommu: out of memory building the identity domain\n");
        return -1;
    }

    struct assign_ctx ctx = { 0 };
    pci_scan(assign_one, &ctx);

    /* Root table pointer, then translation.  SRTP is a separate command from TE
     * and both must be acknowledged in GSTS before the next step — polling the
     * status is not politeness, it is the only way to know the unit accepted a
     * pointer we are about to make it depend on. */
    wreg64(VTD_REG_RTADDR, g_root_phys);
    wreg32(VTD_REG_GCMD, GCMD_SRTP);
    int ok = 0;
    for (int i = 0; i < 1000000; i++)
        if (reg32(VTD_REG_GSTS) & GSTS_RTPS) { ok = 1; break; }
    if (!ok) { kprintf("iommu: unit did not accept the root table pointer\n"); return -1; }

    invalidate_all();

    wreg32(VTD_REG_GCMD, GCMD_TE);
    ok = 0;
    for (int i = 0; i < 1000000; i++)
        if (reg32(VTD_REG_GSTS) & GSTS_TES) { ok = 1; break; }
    if (!ok) { kprintf("iommu: unit did not enable translation\n"); return -1; }

    g_info.state = IOMMU_ACTIVE;
    g_info.why   = "translation is ON, every device in an identity domain — "
                   "nothing is restricted until a device is given a narrower one";
    kprintf("iommu: translation ON — %d device(s) in the identity domain, "
            "%d-level tables, %u MiB mapped\n",
            ctx.n, g_levels, (unsigned)(ram >> 20));
    klog(KLOG_INFO, "iommu", "translation enabled, %d device(s) identity-mapped\n",
         ctx.n);
    return 0;
#endif
}

/* One device's own domain, remembered so a second grant ADDS to it.
 *
 * A per-driver domain is built one buffer at a time — a driver allocates its
 * ring, then its data, then more later — so "confine to a window" has to be
 * cumulative or it can only ever express a device with exactly one buffer.
 * Making it replace instead of add would have looked right in every test with a
 * single allocation and been wrong for every real driver. */
#define IOMMU_MAX_DOMAINS 8
static struct { uint16_t bdf; int used, dom; uint64_t top_phys; uint64_t* top; }
    g_dom[IOMMU_MAX_DOMAINS];

static int domain_for(uint16_t bdf, uint64_t** top, uint64_t* top_phys, int* dom,
                      int* is_new) {
    for (int i = 0; i < IOMMU_MAX_DOMAINS; i++)
        if (g_dom[i].used && g_dom[i].bdf == bdf) {
            *top = g_dom[i].top; *top_phys = g_dom[i].top_phys;
            *dom = g_dom[i].dom; *is_new = 0;
            return 0;
        }
    for (int i = 0; i < IOMMU_MAX_DOMAINS; i++) {
        if (g_dom[i].used) continue;
        uint64_t ph;
        uint64_t* t = table_alloc(&ph);
        if (!t) return -1;
        g_dom[i].used = 1; g_dom[i].bdf = bdf;
        g_dom[i].top = t; g_dom[i].top_phys = ph;
        g_dom[i].dom = g_next_domain++;
        *top = t; *top_phys = ph; *dom = g_dom[i].dom; *is_new = 1;
        return 0;
    }
    return -1;
}

int iommu_restrict(uint16_t bdf, uint64_t base, uint64_t len) {
#if !defined(__i386__) && !defined(__x86_64__)
    (void)bdf; (void)base; (void)len; return -1;
#else
    if (g_info.state != IOMMU_ACTIVE) {
        kprintf("iommu: not active — `iommu on` first\n");
        return -1;
    }
    uint64_t top_phys; uint64_t* top; int dom, is_new;
    if (domain_for(bdf, &top, &top_phys, &dom, &is_new) != 0) return -1;
    if (len && domain_map(top, base, len) != 0) return -1;

    if (context_set(bdf, top_phys, dom) != 0) return -1;
    invalidate_all();

    /* NO WIDTH SPECIFIERS in this printf — `%02x` prints literally, which §M66
     * wrote down the same way after it turned a bring-up dump into garbage
     * exactly when it was needed.  Plain %x, and the b:s.f punctuation carries
     * the structure instead. */
    /* "ONLY" is true of the FIRST grant and a lie about the second, because the
     * domain accumulates.  Saying it anyway would mislead precisely the person
     * building up a per-driver domain buffer by buffer — the one use this
     * primitive exists for. */
    kprintf("iommu: %x:%x.%x %s %x..%x (domain %d)\n",
            bdf >> 8, (bdf >> 3) & 0x1F, bdf & 7,
            is_new ? "now sees ONLY" : "may ALSO reach",
            (unsigned)base, (unsigned)(base + len), dom);
    if (!len)
        kprintf("  it can reach NOTHING — every DMA it attempts is now a fault\n");

    /* WARN AT THE POINT OF THE FALSE ACTION, not only in the report.  Somebody
     * restricting a virtio device gets every success message and no effect
     * whatsoever, and would have no reason to doubt it. */
    {
        uint16_t vendor = pci_read16((uint8_t)(bdf >> 8), (uint8_t)((bdf >> 3) & 0x1F),
                                     (uint8_t)(bdf & 7), PCI_VENDOR_ID);
        if (device_bypasses(vendor))
            kprintf("  BUT THIS DEVICE BYPASSES THE IOMMU — it is virtio and has\n"
                    "  not negotiated platform access, so its DMA never reaches\n"
                    "  the unit.  This restriction will have NO EFFECT.\n");
    }
    return 0;
#endif
}

uint32_t iommu_fault_count(void) {
#if !defined(__i386__) && !defined(__x86_64__)
    return 0;
#else
    if (!g_regs) return 0;
    /* FSTS.PPF (bit 1) says at least one fault record is present; FRI (bits
     * 8..15) indexes the most recent one.  READ FROM THE UNIT rather than
     * counted by us on the side: a count our own software keeps would prove
     * that our software thinks a DMA was refused, which is not the claim. */
    uint32_t fsts = reg32(VTD_REG_FSTS);
    return (fsts & 0x2) ? 1 + ((fsts >> 8) & 0xFF) : 0;
#endif
}

#if defined(__i386__) || defined(__x86_64__)
/* THE RECORDS THEMSELVES, READ OUT OF THE UNIT.
 *
 * A count alone says a number went up.  The record says WHICH DEVICE tried to
 * touch WHICH ADDRESS and was refused — which is the difference between "the
 * software thinks something was blocked" and evidence that the hardware blocked
 * a specific access.  That distinction is the whole reason this milestone reads
 * the unit instead of keeping a counter.
 *
 * FRO (CAP bits 24:33, in 16-byte units) says where the records live and NFR
 * (bits 40:47) how many there are.  Both are READ rather than assumed: a
 * hard-coded offset works on this emulator and reads some other register on a
 * different chipset. */
static void fault_dump(void) {
    if (!g_regs) { kprintf("  (not programmed)\n"); return; }
    int fro = (int)(((g_info.cap >> 24) & 0x3FF) * 16);
    int nfr = (int)(((g_info.cap >> 40) & 0xFF) + 1);
    int shown = 0;
    for (int i = 0; i < nfr; i++) {
        int off = fro + i * 16;
        uint64_t lo = reg64_at(off);
        uint64_t hi = reg64_at(off + 8);
        if (!(hi >> 63)) continue;                  /* F bit clear = empty slot */
        shown++;
        unsigned sid = (unsigned)(hi & 0xFFFF);
        kprintf("  record %d: device %x:%x.%x tried to %s %x — REFUSED "
                "(reason %x)\n", i, sid >> 8, (sid >> 3) & 0x1F, sid & 7,
                ((hi >> 62) & 1) ? "read" : "write",
                (unsigned)(lo & ~0xFFFull), (unsigned)((hi >> 32) & 0xFF));
        /* Clear it by writing 1 back to F, so the next check starts from a known
         * state.  A record left standing would make a later run report a fault
         * it did not cause — the failure mode of every counter nobody resets. */
        wreg64_at(off + 8, hi | (1ull << 63));
    }
    if (!shown) kprintf("  (the unit's own records are empty)\n");
    wreg32(VTD_REG_FSTS, reg32(VTD_REG_FSTS));   /* sticky bits are write-1-clear */
}
#endif

/* ----------------------------------------------------------------------
 * The `iommu` verb.  Lives here rather than in a shell so both shells and all
 * three arches run ONE implementation — §M24's rule.
 * ---------------------------------------------------------------------- */
static int str_eq_i(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint64_t parse_hex(const char* s, const char** end) {
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
           (*s >= 'A' && *s <= 'F')) {
        int d = (*s <= '9') ? *s - '0' : ((*s | 32) - 'a' + 10);
        v = v * 16 + (uint64_t)d;
        s++;
    }
    if (end) *end = s;
    return v;
}

void iommu_cmd(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { iommu_report(); return; }

    if (str_eq_i(args, "on")) { iommu_enable(); return; }

    if (args[0] == 'b' && args[1] == 'l' && args[2] == 'o' && args[3] == 'c' &&
        args[4] == 'k') {
        /* `iommu block <bus>:<slot>.<func>` — the falsification.
         *
         * A domain that maps NOTHING, so the next DMA that device attempts is
         * refused by the unit.  Deliberately the bluntest form: a mechanism
         * that translates correctly and never refuses anything is
         * indistinguishable from passthrough, and the only way to know the
         * boundary exists is to cross it and be stopped. */
        const char* p = args + 5;
        while (*p == ' ') p++;
        const char* e;
        uint64_t bus = parse_hex(p, &e);
        if (*e != ':') { kprintf("iommu: block <bus>:<slot>.<func>\n"); return; }
        uint64_t slot = parse_hex(e + 1, &e);
        uint64_t func = 0;
        if (*e == '.') func = parse_hex(e + 1, &e);
        uint16_t bdf = (uint16_t)((bus << 8) | ((slot & 0x1F) << 3) | (func & 7));
        iommu_restrict(bdf, 0, 0);
        return;
    }

    if (args[0]=='l'&&args[1]=='i'&&args[2]=='m'&&args[3]=='i'&&args[4]=='t') {
        /* `iommu limit <b>:<s>.<f> <base> <len>` — confine a device to a WINDOW.
         *
         * `block` proves the extreme case: a domain that maps nothing refuses
         * everything.  That alone does not show the boundary is where we PUT
         * it, only that we can switch a device off.  A window shows both edges:
         * give a device a range that excludes its buffers and its real DMA is
         * refused AT ITS OWN BUFFER ADDRESS; give it one that includes them and
         * the same DMA goes through untouched.
         *
         * This is exactly the granularity a per-driver DMA domain would use, so
         * proving it here is proving the mechanism that work would rest on. */
        const char* p = args + 5;
        while (*p == ' ') p++;
        const char* e;
        uint64_t bus = parse_hex(p, &e);
        if (*e != ':') { kprintf("iommu: limit <b>:<s>.<f> <base> <len>\n"); return; }
        uint64_t slot = parse_hex(e + 1, &e);
        uint64_t func = 0;
        if (*e == '.') func = parse_hex(e + 1, &e);
        while (*e == ' ') e++;
        uint64_t base = parse_hex(e, &e);
        while (*e == ' ') e++;
        uint64_t len = parse_hex(e, &e);
        if (!len) { kprintf("iommu: a zero length is `block` — say that instead\n"); return; }
        iommu_restrict((uint16_t)((bus << 8) | ((slot & 0x1F) << 3) | (func & 7)),
                       base, len);
        return;
    }

    if (str_eq_i(args, "faults")) {
        uint32_t n = iommu_fault_count();
        kprintf("iommu: %u fault record(s) in the unit\n", n);
#if defined(__i386__) || defined(__x86_64__)
        fault_dump();
#endif
        if (!n)
            kprintf("  none — which is what a device that stayed inside its "
                    "domain looks like, and also what passthrough looks like\n");
        return;
    }

    kprintf("iommu: (no args) | on | block <b>:<s>.<f> | "
            "limit <b>:<s>.<f> <base> <len> | faults\n");
}

