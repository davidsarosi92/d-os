/* =============================================================================
 * domain.c — execution domains: the vocabulary and the honesty gate (§M33).
 *
 * See domain.h for the design.  This file is deliberately small: the value of
 * §M33 stage 1 is in WHERE the decisions are made, not in how much code makes
 * them.
 * ============================================================================= */

#include "domain.h"
#include "printf.h"
#include "iommu.h"
#include "audit.h"
#include "driver.h"
#include "drvuser.h"    /* §M33 stage 5 — the REASON, never the verdict */
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
int domain_enforceable(uint32_t domain, const char* driver, const char** why) {
    (void)driver;
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
        /* §M78.1 — THE QUESTION IS WHAT THIS DRIVER NEEDS, NOT WHAT THE
         * MACHINE CAN GRANT IN GENERAL.
         *
         * This used to be `#if defined(__i386__) || defined(__x86_64__)`, on
         * the reasoning below — and BOTH of its premises have since stopped
         * being true for some drivers:
         *
         *   "aarch64 has no port space" — still true, and irrelevant to a
         *   driver that asks for no ports.  The gate conflated *this machine
         *   cannot grant ports* with *this machine cannot place a driver*.
         *
         *   "mapping MMIO into a driver's own space is unwritten" — it was
         *   written by §M33 Tier 1, in `drvuser_sys_mmio`, and it is PORTABLE:
         *   `vmm_space_map` with VMM_USER, which every architecture here
         *   implements.  The note outlived the code it described.
         *
         * §M52's shape, in the function this milestone appointed as the single
         * place that knows what is real.  *A gate is only as honest as its
         * premises, and premises expire.*
         *
         * So the arch check is now a PORT check, asked per driver.  §M78's
         * PL031 is the case that exposed it: MMIO only, no DMA, no interrupt,
         * no ports — nothing aarch64 cannot grant. */
#if !defined(__i386__) && !defined(__x86_64__)
        if (drvuser_needs_ports(driver) == 1) {
            if (why) *why = "this driver needs port I/O and this architecture "
                            "has no I/O address space — no instruction, no "
                            "bitmap, nothing to grant";
            return -1;
        }
#endif
        /* REAL NOW.  On x86 for any driver; elsewhere for one that needs no
         * ports.
         *
         * What makes it real rather than a claim: the driver's ports come from
         * a kernel-side manifest and are enforced by the CPU (an ungranted `in`
         * is a #GP), its interrupt is a syscall it blocks in, its events reach
         * the input stack through one publish call, and `drv_init` LAUNCHES the
         * ring-3 image INSTEAD OF calling init — so nothing brings the device
         * up in the kernel as well. */
        return 0;

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
enum domain_isolation domain_isolation_of(uint32_t domain, int does_dma,
                                          int device_confined) {
    if (domain == DOMAIN_KERNEL) return ISOL_NONE;
    /* §M33 COMPLETE — THE VERDICT MOVES, AND ONLY THIS FAR.
     *
     * A DMA driver in ring 3 whose DEVICE is confined by an IOMMU to that
     * driver's own buffers is isolated in both directions that matter: it
     * cannot reach kernel memory (its address space is its own) and neither can
     * the hardware it commands (its domain holds nothing else).  That second
     * half is what `ADVISORY(!)` has meant since stage 1, and it is now a fact
     * the caller can establish rather than a hope.
     *
     * `device_confined` is passed IN rather than looked up here, because this
     * file must not know how a driver is placed — and because a caller that
     * cannot establish it passes 0, which keeps the cautious answer the
     * default. */
    if (does_dma && device_confined) return ISOL_FULL;
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
        /* TRANSLATION BEING ON IS NOT ISOLATION FOR THIS DRIVER, and saying
         * only "translation is on" would read as though it were — the exact
         * theatre this milestone keeps refusing, now available in a new form
         * because the hardware really is doing something.
         *
         * Every device sits in ONE identity domain that maps all of RAM, so a
         * device can still reach every byte; what stage 5's second half proved
         * is that the machinery WORKS, not that any driver is confined by it.
         * Per-driver domains are what `drv_dma_request` would have to build,
         * and they are not built. */
        return "translation is ON and a driver's own buffers become its "
               "domain, so a DMA driver PLACED IN RING 3 reports `full`.  Still "
               "advisory for the rest, and for two named reasons: a driver in "
               "the KERNEL is not isolated by anything the IOMMU does, and "
               "virtio devices bypass the unit entirely because our drivers are "
               "legacy (`iommu` lists which)";
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

/* =============================================================================
 * §M71 — THE PLACEMENT AUDIT.
 *
 * THE INVARIANT: every driver is EXECUTING in the domain it was PLACED in.
 *
 * That sounds tautological and is not, because the two facts live in different
 * places and are established at different times.  `driver_domain()` says what
 * config asked for and this machine agreed to; `drvuser_pid()` says whether a
 * ring-3 process actually exists.  Nothing has ever compared them.
 *
 * THIS BUG CLASS HAS ALREADY BITTEN, TWICE, IN THE MILESTONE THAT INTRODUCED
 * PLACEMENT:
 *
 *   - `drv stop` ran the IN-KERNEL shutdown hook for a driver running in
 *     ring 3.  §M33's own note: *a placement is only a placement if EVERY
 *     lifecycle edge honours it, and the test is the LIVE PROCESS, not the
 *     configured domain.*
 *   - `drv start` and the rescan job could place ONE driver TWICE, after which
 *     it quarantined itself fighting itself for the 8042.
 *
 * Both were found by reading a serial log after something visibly broke.  This
 * finds them by asking, on demand, before anything breaks.
 *
 * WHY THE CHECK IS WORTH MORE THAN ITS SIZE: the failure it catches is the one
 * §M33 refuses by name.  A driver configured `domain = user` that is in fact
 * executing in ring 0 has not merely lost isolation — it has taken away the
 * operator's ability to find that out, because every report keeps agreeing
 * with the intention.  That is "isolation theatre", and the whole of domain.h
 * exists to make it impossible to reach by accident.  An audit is what makes
 * it impossible to reach by ACCIDENT TWICE.
 *
 * HOW TO MAKE IT FAIL (rule 1 in audit.h — a check nobody has seen fail is a
 * check nobody has tested): `drv domain <name> user` on a machine where the
 * placement is enforceable, then kill the placed process with `fkill <pid>`
 * and run `audit` before the supervisor restarts it.  Violation A fires.
 * =========================================================================== */

static int au_placement(int verbose) {
    int n = driver_count_all();
    if (n <= 0) return AUDIT_SKIP;

    int bad = 0, looked = 0;
    for (int i = 0; i < n; i++) {
        struct driver* d = driver_at(i);
        if (!d || !d->name) continue;

        uint8_t st = driver_state(d);
        /* A driver that never came up has no domain to be in.  Skipping it is
         * not leniency: `drv start` is what puts it in one, and auditing a
         * driver that is deliberately stopped would report the operator's own
         * decision as a fault (§M66's ADMIN_DOWN vs QUARANTINE distinction). */
        if (!(st & DRV_S_INITED)) continue;

        uint32_t placed = driver_domain(d);     /* what config asked and we agreed to */
        int      pid    = drvuser_pid(d->name); /* what is actually executing         */
        looked++;

        /* A — PLACED OUT OF THE KERNEL, RUNNING IN IT.  The isolation-theatre
         * case, and the one that must never be silent. */
        if ((placed & (DOMAIN_USER | DOMAIN_ISOLATED)) && pid <= 0) {
            kprintf("!! audit placement: '%s' is placed '%s' but has NO ring-3 "
                    "process — it is executing in the kernel\n",
                    d->name, domain_name(placed));
            bad++;
            continue;
        }

        /* B — PLACED IN THE KERNEL, A RING-3 PROCESS STILL ALIVE.  The reverse,
         * and it is not harmless: that process still holds the grants its
         * bring-up took, so the device now has two drivers. */
        if (placed == DOMAIN_KERNEL && pid > 0) {
            kprintf("!! audit placement: '%s' is placed 'kernel' but ring-3 pid "
                    "%d is still alive — two drivers for one device\n",
                    d->name, pid);
            bad++;
            continue;
        }

        /* C — RUNNING IN A DOMAIN THE CODE NEVER DECLARED.  `driver_domain()`
         * already refuses to widen `.domains`, so reaching this means that
         * guard did not hold — which is worth knowing precisely BECAUSE it
         * should be impossible.  §M52's lesson: the checks that matter most
         * are the ones protecting an assumption nothing re-tests. */
        uint32_t declared = d->domains ? d->domains : DOMAIN_KERNEL;
        if (!(declared & placed)) {
            char set[48];
            domain_set_str(declared, set, sizeof set);
            kprintf("!! audit placement: '%s' runs '%s' but declares only '%s' "
                    "— config widened a capability\n",
                    d->name, domain_name(placed), set);
            bad++;
            continue;
        }

        /* D — CLAIMS FULL ISOLATION WITHOUT THE HARDWARE BACKING IT.  Not the
         * same as C: here the placement is legal and the REPORT is wrong.
         * §M33 stage 5's rule is that finding an IOMMU must not improve the
         * verdict, so the verdict and the device's actual confinement have to
         * be able to disagree — and when they do, the report is the fault. */
        if (pid > 0) {
            int confined = drvuser_confined(d->name);
            int dma      = (d->flags & DRVF_DMA) ? 1 : 0;
            enum domain_isolation say = domain_isolation_of(placed, dma, confined);
            if (say == ISOL_FULL && dma && !confined) {
                kprintf("!! audit placement: '%s' reports isolation 'full' while "
                        "its device is NOT confined by an IOMMU\n", d->name);
                bad++;
                continue;
            }
        }

        if (verbose) {
            int dma = (d->flags & DRVF_DMA) ? 1 : 0;
            kprintf("       %s: %s", d->name, domain_name(placed));
            if (pid > 0) kprintf(" pid %d", pid);
            kprintf(", isolation %s%s\n",
                    domain_isolation_name(
                        domain_isolation_of(placed, dma,
                                            pid > 0 ? drvuser_confined(d->name) : 0)),
                    dma ? ", DMA" : "");
        }
    }

    /* NOTHING RUNNING IS NOT THE SAME AS NOTHING WRONG.  Rule 3: an audit with
     * no rows to look at must say so rather than report a pass. */
    if (!looked) return AUDIT_SKIP;
    return bad ? bad : AUDIT_OK;
}

AUDIT(placement) = {
    "driver-placement",
    "every driver executes in the domain it was placed in, and reports the "
    "isolation the hardware actually gives it",
    au_placement
};
