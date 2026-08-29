/* =============================================================================
 * hwdev.h — the machine's HARDWARE, enumerated, and what is driving each piece.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS EXISTS SEPARATELY FROM THE DRIVER REGISTRY
 *
 * The driver registry answers "what code do we have".  That is not the question
 * a person in front of a machine is asking, and the difference is not academic:
 *
 *   **A DEVICE WITH NO DRIVER IS INVISIBLE IN A DRIVER-CENTRIC VIEW.**
 *
 * It has no registry entry, nothing probes it, nothing logs it, and it appears
 * in no list — so the one case where somebody must go and FIND a driver is
 * precisely the case nothing tells them about.  A list of drivers can never
 * report the hardware it does not cover; only a list of hardware can.
 *
 * -----------------------------------------------------------------------------
 * THE TWO GROUPS, AND WHY THEY ARE DIFFERENT QUESTIONS
 *
 *   ONLINE   the device is physically here, right now.  It may have a driver
 *            bound, or it may have none — and "here with no driver" is the row
 *            that means *go and get one*.
 *   OFFLINE  we have a driver for it and the hardware is not present.  This is
 *            not a fault; it is the honest description of a machine that could
 *            drive an AC97 and does not have one.
 *
 * Collapsing them loses the distinction between "nothing is driving this" and
 * "there is nothing to drive" — which look identical in a state column and call
 * for opposite actions (§M23's argument for three sound icons rather than two,
 * §M66's for keeping "stopped" apart from "quarantined").
 *
 * -----------------------------------------------------------------------------
 * NAMING: THREE SOURCES, WORST CASE STILL USEFUL
 *
 * A raw `1af4:1001` is not a hardware name to anyone who has not memorised the
 * PCI ID list, so a name is resolved in order:
 *
 *   1. an exact (vendor, device) entry in the small table in hwdev.c;
 *   2. the PCI CLASS CODE — every device carries one, so an unknown card is
 *      still "SATA controller" rather than a pair of hex numbers.  This is the
 *      one that matters: it works for hardware nobody has taught us about,
 *      which is the hardware most in need of a name;
 *   3. "PCI device", plus the IDs, which are always shown regardless.
 *
 * We deliberately do NOT ship the full PCI ID database.  It is megabytes, it
 * goes stale, and the class code answers the question well enough that the
 * table is a courtesy rather than a dependency.
 *
 * -----------------------------------------------------------------------------
 * WHICH DRIVER DRIVES WHICH DEVICE — DECLARED BY THE DRIVER
 *
 * `DRIVER_MATCH()` puts the (vendor, device) → driver mapping NEXT TO THE
 * DRIVER, in a linker section, exactly as `DRIVER()`, `SERVICE()` and
 * `CONFIG_KEY()` do.  The alternative — a table in the device manager — would
 * make the panel accumulate knowledge of every driver in the tree, which is the
 * thing §M63's registries exist to prevent, and would go stale silently the
 * first time a driver learned a new ID.
 * ============================================================================= */

#ifndef HWDEV_H
#define HWDEV_H

#include <stdint.h>

/* A declaration that "driver <name> drives this hardware".
 *
 * TWO WAYS TO MATCH, because real drivers bind both ways and an interface that
 * only expressed the easy one would be wrong for the first general driver that
 * used it.  xHCI is exactly that case here: it claims *any* USB 3 controller by
 * PCI CLASS, and has never cared which vendor made it.
 *
 *   by ID     vendor + device            (virtio_blk, ac97, edu, …)
 *   by CLASS  HW_ANY_VENDOR + cls/sub    (xhci: 0x0C / 0x03)
 *
 * `device` may be HW_ANY_DEVICE for a driver that claims a whole vendor;
 * `sub` may be HW_ANY_SUB for a whole class. */
#define HW_ANY_DEVICE 0xFFFF
#define HW_ANY_VENDOR 0xFFFF
#define HW_ANY_SUB    0xFF

struct driver_match {
    const char* driver;     /* must be the registry name (struct driver.name) */
    /* What the HARDWARE is called, in words.  Optional for a PCI device (the
     * ID table and the class code already name it) and the only source for a
     * PLATFORM device, which is on no bus and carries no ID — without it the
     * best a list can print is the driver's class, and "input" is not the name
     * of a piece of hardware. */
    const char* name;
    uint16_t    vendor;     /* HW_ANY_VENDOR = match on class instead;
                             * 0 = a platform device, matched by name only */
    uint16_t    device;
    uint8_t     cls, sub;   /* only consulted when vendor == HW_ANY_VENDOR */
};

extern struct driver_match __start_driver_matches[];
extern struct driver_match __stop_driver_matches[];

/* KNOWN LIMITATION — A LOADABLE MODULE'S DECLARATION IS NOT VISIBLE HERE.
 * §M67 modules carry their own `driver_matches` section and the loader does not
 * merge it into the kernel's, so a module-provided driver has no declared
 * hardware name and no ID claim.  The list falls back to the driver's class,
 * which is why `hda` and `loopback` show "audio" and "net" where the built-in
 * drivers show a real name.
 *
 * It is written down rather than worked around: the fix is for `modload` to
 * register a module's matches the way `driver_attach` registers its driver —
 * the registry stopped being a plain linker-section walk for exactly this
 * reason once §M67 landed, and this section has not caught up.
 *
 * Use DESIGNATED initialisers — `.driver = …, .vendor = …` — never positional.
 * §M58 paid for that rule when a field added to the middle of `widget_ops`
 * silently re-bound every table in the tree by one slot, and here the same
 * mistake would bind a device ID onto a class code. */
#define DRIVER_MATCH(_var)                                               \
    static const struct driver_match                                     \
    __attribute__((used, section("driver_matches"), aligned(4)))         \
    _var##_registration

/* One row of the hardware list. */
struct hw_device {
    int         online;         /* 1 = physically present now                */
    int         is_pci;         /* 0 = a platform device (no bus address)    */
    uint16_t    bdf;            /* PCI bus:slot.func, packed as in iommu.c   */
    uint16_t    vendor, device;
    uint8_t     class_code, subclass;
    const char* name;           /* best available human name                 */
    const char* driver;         /* bound driver's registry name, NULL = none */
};

/* Fill `out` with every device this machine has: present hardware first, then
 * the hardware we have drivers for and do not have.  Returns how many rows were
 * written (never more than `cap`).
 *
 * Re-scanned on every call rather than cached — §M66 made hot-plug real, so a
 * cached list would be wrong exactly when somebody is watching for a change. */
int hw_enumerate(struct hw_device* out, int cap);

/* The name for a PCI device, by the three-source rule above.  Exposed because
 * a caller with a `struct pci_device` already in hand should not have to
 * re-scan to name it. */
const char* hw_name_for(uint16_t vendor, uint16_t device,
                        uint8_t class_code, uint8_t subclass);

/* ITERATE THE MATCH TABLE THROUGH THESE, never over the linker symbols.
 *
 * The declarations come from two places now: the ones the linker collected from
 * the kernel image, and the ones a §M67 MODULE brought with it.  Six loops in
 * hwdev.c walked `__start_driver_matches` directly, which meant module support
 * would have had to be added six times — and would have worked in five.
 *
 * `hw_match_count` covers both; `hw_match_at` returns them in that order, so a
 * built-in declaration still wins over a module's for the same ID. */
int hw_match_count(void);
const struct driver_match* hw_match_at(int i);

/* Register / withdraw a module's `driver_matches` section.  Called by the
 * module loader, which finds the section by name exactly as it finds `.dosmod`
 * — so a module declares its hardware the same way a built-in driver does and
 * needs no change to the module ABI at all.
 *
 * `remove` takes the same pointer `add` was given: a module's code is FREED by
 * rmmod, so a range left registered is a dangling pointer the next hardware
 * scan would walk (§M67's use-after-free, in a new table). */
int  hw_matches_add(const struct driver_match* m, int count);
void hw_matches_remove(const struct driver_match* m);

/* Does this device NEED a driver at all?
 *
 * The first version of the device manager printed "needs a driver" for every
 * unclaimed device, and that over-claims: a **host bridge is the PCI root
 * complex itself**, and a PCI-to-PCI bridge is handled by enumeration.  Neither
 * has anything to drive, and Linux has no driver for them either.  Telling a
 * user their machine needs four drivers it does not need is the same failure as
 * hiding the one it does — it makes the column untrustworthy, after which the
 * row that DOES matter reads like more of the same.
 *
 * Returns 0 when nothing should be expected to claim it. */
int hw_needs_driver(uint8_t class_code, uint8_t subclass);

/* Which registered driver claims this device, or NULL.  Answers from the
 * DRIVER_MATCH() declarations only — it says what SHOULD drive the device, not
 * what currently is, and the caller pairs it with `driver_state`. */
const char* hw_driver_for(uint16_t vendor, uint16_t device,
                          uint8_t class_code, uint8_t subclass);

/* What hardware driver `name` is for, in words — the reverse direction, used to
 * describe a driver whose device is NOT present.  Falls back to the driver's
 * own class when it has declared nothing. */
const char* hw_hardware_of(const char* driver_name);

#endif /* HWDEV_H */
