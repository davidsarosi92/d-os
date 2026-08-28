/* =============================================================================
 * domain.c — execution domains: the vocabulary and the honesty gate (§M33).
 *
 * See domain.h for the design.  This file is deliberately small: the value of
 * §M33 stage 1 is in WHERE the decisions are made, not in how much code makes
 * them.
 * ============================================================================= */

#include "domain.h"
#include "printf.h"
#include "iommu.h"    /* §M33 stage 5 — the REASON, never the verdict */
#include <stddef.h>

static int d_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const char* domain_name(uint32_t domain) {
    switch (domain) {
    case DOMAIN_KERNEL:   return "kernel";
    case DOMAIN_USER:     return "user";
    case DOMAIN_ISOLATED: return "isolated";
    default:              return "?";
    }
}

uint32_t domain_parse(const char* s) {
    if (!s) return 0;
    if (d_streq(s, "kernel"))   return DOMAIN_KERNEL;
    if (d_streq(s, "user"))     return DOMAIN_USER;
    if (d_streq(s, "isolated")) return DOMAIN_ISOLATED;
    return 0;
}

void domain_set_str(uint32_t domains, char* out, int cap) {
    int n = 0;
    const uint32_t bits[] = { DOMAIN_KERNEL, DOMAIN_USER, DOMAIN_ISOLATED };
    for (int i = 0; i < 3; i++) {
        if (!(domains & bits[i])) continue;
        const char* nm = domain_name(bits[i]);
        if (n && n < cap - 1) out[n++] = '|';
        for (int k = 0; nm[k] && n < cap - 1; k++) out[n++] = nm[k];
    }
    if (cap > 0) out[n < cap ? n : cap - 1] = 0;
    if (n == 0 && cap > 1) { out[0] = '-'; out[1] = 0; }
}

/* -----------------------------------------------------------------------------
 * THE HONESTY GATE.
 *
 * One function, so that when the user-mode driver backend (§M33 Tier 1) lands
 * there is exactly one place to change and every caller — the config watcher,
 * the `drv domain` command, `/proc/drivers` — inherits the new answer.  Three
 * copies of this test would be three chances to have one of them still refusing
 * after the thing became possible, which is a feature that exists and cannot be
 * reached.
 * -------------------------------------------------------------------------- */
int domain_enforceable(uint32_t domain, const char** why) {
    switch (domain) {
    case DOMAIN_KERNEL:
        /* Always available: it is where everything runs today, and it is the
         * only domain whose "enforcement" is trivially true because it
         * enforces nothing. */
        return 0;

    case DOMAIN_USER:
        /* §M33 TIER 1 IS PARTLY BUILT, AND THE ANSWER STILL HAS TO BE NO.
         *
         * What exists and is measured: a ring-3 process gets its PORTS through
         * a syscall, bounded by a kernel-side manifest and enforced by the
         * CPU's I/O permission bitmap — a granted port reads, an ungranted one
         * is a #GP that kills only that process.  It can claim an interrupt and
         * block on it, and publish input events back.
         *
         * What does not: nothing PLACES a driver there.  The spawn path, MMIO
         * mapping into the driver's own space and client reconnection are
         * unwritten, so honouring `driver.<name>.domain = user` would be a
         * promise rather than a placement.
         *
         * THE DISTINCTION IS THE DISCIPLINE.  A mechanism working in a test is
         * not a placement being honoured, and reporting the first as the second
         * is exactly the isolation theatre §M33 refuses by name.  Accepting
         * `user` and running the driver in ring 0 anyway would leave the user
         * believing in a boundary that is not there. */
#if defined(__i386__) || defined(__x86_64__)
        /* REAL NOW, on the arches that have an I/O permission mechanism.
         *
         * What makes it real rather than a claim: the driver's ports come from
         * a kernel-side manifest and are enforced by the CPU (an ungranted `in`
         * is a #GP), its interrupt is a syscall it blocks in, its events reach
         * the input stack through one publish call, and `drv_init` LAUNCHES the
         * ring-3 image INSTEAD OF calling init — so nothing brings the device
         * up in the kernel as well. */
        return 0;
#else
        /* Still refused where there is nothing to enforce a hardware grant
         * with.  "Placed in ring 3" with unconstrained device access is a
         * location, not a boundary — and aarch64 has no port space, while
         * mapping MMIO into a driver's own address space is unwritten. */
        if (why) *why = "this arch has no mechanism to enforce a hardware grant "
                        "(no port space; MMIO mapping into a driver's own space "
                        "is unwritten)";
        return -1;
#endif

    case DOMAIN_ISOLATED:
        /* Strictly more than USER, so it fails for USER's reason first and for
         * its own second.  Both are named, because a user who fixes the first
         * should not have to discover the second by trying again. */
        if (why) *why = "needs the user-mode backend (§M33 Tier 1) AND an "
                        "IOMMU driver (§M33 stage 5) — without the latter a "
                        "device can DMA over kernel memory whatever ring its "
                        "driver sits in";
        return -1;

    default:
        if (why) *why = "not a domain";
        return -2;
    }
}

/* What a placement WOULD actually deliver.  Kept apart from "is it allowed"
 * because the DMA case is precisely where the two answers diverge: a
 * DMA-capable driver in ring 3 is ALLOWED (once Tier 1 exists) and is NOT
 * isolated until an IOMMU constrains the device.  A single boolean would have
 * to pick one of those to report, and either choice misleads. */
enum domain_isolation domain_isolation_of(uint32_t domain, int does_dma) {
    if (domain == DOMAIN_KERNEL) return ISOL_NONE;
    /* §M33 STAGE 5 — AND THE ANSWER DELIBERATELY DOES NOT CONSULT `iommu_get`.
     *
     * The machine may well have DMA remapping hardware; stage 5's first half
     * went and found out.  It changes NOTHING here, because an IOMMU we have
     * not programmed sits in passthrough and every device still reads every
     * byte — a DMA driver on that machine is exactly as exposed as on one with
     * no IOMMU at all.
     *
     * Reporting better isolation because the CHIPSET is capable would be the
     * most convincing kind of isolation theatre, since the capability is real
     * and checkable.  What the discovery buys is the REASON (see
     * domain_isolation_reason), and a reason is what tells a user whether the
     * gap is a hardware limit or unfinished work. */
    if (does_dma)                return ISOL_ADVISORY;
    return ISOL_FULL;
}

/* Why a DMA driver is only advisory here — the sentence that differs between
 * "this machine cannot" and "this machine can and we have not built it".  Both
 * leave the driver equally exposed, and they call for entirely different
 * decisions by whoever is reading. */
const char* domain_isolation_reason(int does_dma) {
    if (!does_dma) return NULL;
    switch (iommu_get()->state) {
    case IOMMU_ACTIVE:
        return "translation is on";
    case IOMMU_PRESENT:
        return "this machine HAS an IOMMU and we do not program it yet "
               "(§M33 stage 5) — unfinished work, not a hardware limit";
    case IOMMU_UNUSABLE:
        return "this machine's IOMMU cannot be programmed by us — see `iommu`";
    default:
        return "this machine has no IOMMU, so nothing can bound what a device "
               "reads — a hardware limit, not unfinished work";
    }
}

const char* domain_isolation_name(enum domain_isolation i) {
    switch (i) {
    case ISOL_FULL:     return "full";
    case ISOL_ADVISORY: return "ADVISORY(!)";
    default:            return "none";
    }
}
