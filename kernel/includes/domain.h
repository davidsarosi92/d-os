/* =============================================================================
 * domain.h — execution domains: WHERE a service runs (§M33).
 *
 * The generalisation §M33 asks for: do not hard-code "kernel vs user" as a
 * binary baked into each subsystem.  Make the domain a first-class, DECLARED
 * property of the code, and let config choose among what the code says it can
 * do.
 *
 * -----------------------------------------------------------------------------
 * THE TWO HALVES, AND KEEPING THEM APART IS THE WHOLE DESIGN
 *
 *   `.domains`  — a CAPABILITY OF THE CODE.  "This driver is written so that it
 *                 could run in ring 3."  Set by the author, in the source, next
 *                 to the driver.  Config cannot widen it.
 *   the choice  — a DEPLOYMENT DECISION.  `driver.<name>.domain = user`.  Picks
 *                 among the declared set and nothing else.
 *
 * Without the first half, config could ask for something the code cannot do and
 * the failure would be at runtime, in the field, on somebody else's machine.
 * With it, the refusal happens at the moment of asking and names the reason.
 *
 * -----------------------------------------------------------------------------
 * THE HONESTY GATE — the rule this file exists to enforce
 *
 * A domain is only offered when the machinery to ENFORCE it exists.  Today the
 * user-mode driver backend does not (§M33 Tier 1), so `DOMAIN_USER` and
 * `DOMAIN_ISOLATED` are **refused, loudly, with the reason** rather than
 * accepted and silently downgraded to kernel.
 *
 * That refusal is the feature.  A system that accepts `domain = isolated` and
 * runs the driver in ring 0 anyway has not given the user isolation, it has
 * taken away their ability to find out that they do not have it — and the
 * report they then file is about the wrong thing.  §M33's plan calls this
 * "isolation theatre" and refuses it by name; §M23 made the same argument for
 * why a missing sound device and a muted one need different icons.
 *
 * `domain_enforceable()` is the single place that knows what is real, so when
 * Tier 1 lands there is ONE function to change and every caller inherits it.
 * ============================================================================= */

#ifndef DOMAIN_H
#define DOMAIN_H

#include <stdint.h>

/* A bitmask, because `.domains` is a SET — "this code can run in kernel or in
 * user mode" is one declaration, not two. */
#define DOMAIN_KERNEL    0x01   /* ring 0 / EL1, shared address space          */
#define DOMAIN_USER      0x02   /* ring 3 / EL0, own address space             */
#define DOMAIN_ISOLATED  0x04   /* USER + a restricted capability set          */

/* What isolation a chosen domain ACTUALLY delivers on this machine.  Reported
 * rather than assumed, because the answer depends on the hardware (§M68's
 * ladder) and on whether the driver does DMA — a DMA-capable driver in ring 3
 * with no IOMMU is not isolated from anything, which is what kernel-bypass
 * networking demonstrates for a living. */
enum domain_isolation {
    ISOL_NONE = 0,      /* shared address space; a fault is contained, memory is not */
    ISOL_ADVISORY,      /* placed out-of-kernel, but something can still bypass it   */
    ISOL_FULL,          /* the boundary is enforced by hardware                      */
};

/* Parse / print.  One pair, so a config value, a `/proc` line and a command all
 * spell a domain the same way. */
const char* domain_name(uint32_t domain);        /* single bit -> "kernel" ...  */
uint32_t    domain_parse(const char* s);         /* "user" -> DOMAIN_USER; 0 = bad */
/* "kernel|user" for a declared SET, into a caller-supplied buffer. */
void        domain_set_str(uint32_t domains, char* out, int cap);

/* Can this machine actually enforce `domain` today?  Returns 0 if yes; a
 * negative code otherwise, and `*why` (if non-NULL) is set to a sentence
 * naming what is missing.  THE ONE PLACE that knows what is real. */
int domain_enforceable(uint32_t domain, const char** why);

/* What isolation would a component placed in `domain` actually get, given
 * whether it drives DMA?  Separate from the above because "allowed" and
 * "isolated" are different questions and conflating them is how the DMA case
 * gets quietly mis-reported. */
/* `device_confined`: 1 when an IOMMU holds this driver's DEVICE in a domain
 * containing only that driver's buffers.  Passed in rather than discovered here
 * — this file must not know how a driver is placed — and a caller that cannot
 * establish it passes 0, so the cautious answer stays the default. */
enum domain_isolation domain_isolation_of(uint32_t domain, int does_dma,
                                          int device_confined);
const char* domain_isolation_name(enum domain_isolation i);

/* WHY a DMA-capable driver is only advisory on this machine.  NULL when the
 * question does not arise.  Kept separate from the verdict on purpose: finding
 * an IOMMU must not improve the verdict — an unprogrammed one restricts
 * nothing — but it does change whether the gap is a hardware limit or work we
 * have not done, and those call for different decisions. */
const char* domain_isolation_reason(int does_dma);

#endif /* DOMAIN_H */
