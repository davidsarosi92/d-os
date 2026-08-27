/* =============================================================================
 * domain.c — execution domains: the vocabulary and the honesty gate (§M33).
 *
 * See domain.h for the design.  This file is deliberately small: the value of
 * §M33 stage 1 is in WHERE the decisions are made, not in how much code makes
 * them.
 * ============================================================================= */

#include "domain.h"
#include "printf.h"
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
        /* The substrate EXISTS — §M25 shipped per-process address spaces,
         * ring-3 processes, an fd table and unix-socket IPC, and §M34 added
         * fork/exec/signals.  What does not exist is the DRIVER-side backend:
         * a driver in ring 3 needs its I/O ports granted through the TSS I/O
         * bitmap, its MMIO mapped into its own space, its interrupt turned
         * into something it can wait on, and its clients reached over IPC.
         * That is §M33 Tier 1, and none of it is written.
         *
         * Saying so is the point.  Accepting `user` and running the driver in
         * ring 0 anyway would leave the user believing in a boundary that is
         * not there. */
        if (why) *why = "no user-mode driver backend yet (§M33 Tier 1): "
                        "port grants, MMIO mapping, IRQ forwarding and client "
                        "IPC are unwritten";
        return -1;

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
    if (does_dma)                return ISOL_ADVISORY;   /* until an IOMMU */
    return ISOL_FULL;
}

const char* domain_isolation_name(enum domain_isolation i) {
    switch (i) {
    case ISOL_FULL:     return "full";
    case ISOL_ADVISORY: return "ADVISORY(!)";
    default:            return "none";
    }
}
