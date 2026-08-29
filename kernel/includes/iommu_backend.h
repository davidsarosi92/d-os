/* =============================================================================
 * iommu_backend.h — the seam under iommu.h.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS FILE EXISTS: ONE IMPLEMENTATION BEHIND AN INTERFACE IS NOT AN
 * INTERFACE, IT IS A CLAIM
 *
 * `iommu.h` reads as though it were vendor-neutral — a state, `confine`,
 * `is_confined`, a fault count.  It was not.  The DMAR walk was wired straight
 * into `iommu_init`, and the giveaway was in the data rather than the code:
 * `struct iommu_info` carried `cap`, `ecap`, `sagaw_39`, `sagaw_48` and counted
 * "how many DRHDs the DMAR declared".  Every one of those is Intel's vocabulary.
 * A second vendor could not have filled them in, which is the test an interface
 * has to pass and this one had never been asked.
 *
 * So the neutral facts stay in `struct iommu_info` and everything vendor-shaped
 * moved behind this vtable.  What made it honest is that a SECOND backend now
 * exists and has to answer the same questions from a completely different table
 * (AMD's IVRS) and a completely different register layout.
 *
 * -----------------------------------------------------------------------------
 * WHAT A BACKEND MUST AND MUST NOT DO
 *
 * `detect` fills the neutral half of `iommu_info` and returns 0 if this
 * machine has that vendor's hardware.  It must NOT enable anything: §M33's rule
 * is that finding an IOMMU may never improve the isolation verdict, because an
 * IOMMU in passthrough restricts nothing, and a report that flatters because
 * the chipset is capable is the most convincing kind of isolation theatre.
 *
 * A backend that can detect but not yet program says so by leaving `enable`
 * NULL — the dispatcher then refuses with a reason instead of pretending.  That
 * is the same honesty gate `domain_enforceable` applies one layer up: a
 * boundary you believe in and do not have is worse than one you know you lack.
 * ============================================================================= */

#ifndef IOMMU_BACKEND_H
#define IOMMU_BACKEND_H

#include <stdint.h>
#include "iommu.h"

struct iommu_backend {
    /* "VT-d", "AMD-Vi" — printed, and what `iommu` names as the kind. */
    const char* name;

    /* Is this vendor's hardware here?  Fills the neutral fields of `info`
     * (reg_base, units, host_addr_width, domains, page_levels) and returns 0.
     * Non-zero means "not this vendor", NOT an error. */
    int (*detect)(struct iommu_info* info);

    /* Turn translation on with every device in one identity domain.  NULL when
     * the backend can see the hardware and cannot yet drive it — the honest
     * state for a port in progress, and the dispatcher reports it as such. */
    int (*enable)(void);

    /* Confine one device to [base, base+len).  Grants ACCUMULATE into that
     * device's domain: a driver allocates its ring, then its data, then more
     * later, and replacing instead of adding would look right in every
     * single-buffer test and be wrong for every real driver. */
    int (*restrict_dev)(uint16_t bdf, uint64_t base, uint64_t len);

    /* The mirror: return the device to the identity domain and free its
     * domain.  Not optional for a backend that implements `restrict_dev` —
     * without it a restart loop widens the boundary one grant at a time. */
    int (*release_dev)(uint16_t bdf);

    /* Does this device sit in a domain of its own?  What the isolation verdict
     * is allowed to consult. */
    int (*is_confined)(uint16_t bdf);

    /* Faults, READ FROM THE HARDWARE's own records rather than counted by us.
     * A count our software keeps proves our software believes something; the
     * unit's registers are the only witness to the claim. */
    uint32_t (*fault_count)(void);
    void     (*fault_dump)(void);

    /* Vendor-specific lines for `iommu`, printed under the neutral ones.  This
     * is where CAP/ECAP and their AMD equivalents live now — reported, but no
     * longer part of the shared struct every caller can see. */
    void (*report_extra)(void);
};

/* Registered by each backend's file; the dispatcher tries them in order and
 * keeps the first that detects.  A linker section would be the usual shape here
 * and is deliberately not used: there are two, the order between them is a
 * decision rather than a link-order accident, and a machine cannot have both. */
const struct iommu_backend* iommu_backend_vtd(void);
const struct iommu_backend* iommu_backend_amd(void);

#endif /* IOMMU_BACKEND_H */
