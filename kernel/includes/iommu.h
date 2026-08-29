/* =============================================================================
 * iommu.h — §M33 stage 5: is there anything on this machine that can stop a
 * device reading memory it was not given?
 *
 * -----------------------------------------------------------------------------
 * WHY THIS EXISTS, AND WHY IT SHIPS BEFORE IT REMAPS ANYTHING
 *
 * §M33 Tier 1 can put a driver in ring 3, and for a driver that only touches
 * ports that is real isolation: its address space is its own and its port grant
 * is enforced by the CPU.  For a driver that does DMA it is NOT, and the code
 * has said so from the first day — `domain_isolation_of` returns ADVISORY(!)
 * for a DMA-capable driver whatever ring it sits in, because the DEVICE reads
 * memory, and the device does not care which ring asked it to.
 *
 * THAT SENTENCE IS CURRENTLY AN ASSUMPTION.  It is the right assumption, and it
 * is still one nobody on this machine has checked: the kernel has never looked
 * at whether an IOMMU exists.  So the first thing stage 5 delivers is not
 * remapping — it is the ANSWER TO THE QUESTION, discovered rather than assumed.
 *
 * -----------------------------------------------------------------------------
 * THE RULE THIS FILE IS WRITTEN UNDER
 *
 * **FINDING AN IOMMU MUST NOT IMPROVE THE ISOLATION VERDICT.**  A DMA driver on
 * a machine with an IOMMU we have not programmed is exactly as exposed as one
 * on a machine with no IOMMU at all — the hardware sits there in passthrough
 * and every device reads every byte.  Reporting "isolation full" because the
 * chipset is capable would be the isolation theatre §M33 refuses by name, and
 * it would be the most convincing kind, because the capability is real.
 *
 * What detection changes is the REASON, and the reason is what tells a user
 * whether the gap is a hardware limit or unfinished work.  Those are different
 * facts and they call for different decisions.
 * ============================================================================= */

#ifndef IOMMU_H
#define IOMMU_H

#include <stdint.h>

/* What we found.  Ordered by how much could be built on it, so a caller can
 * ask `>= IOMMU_PRESENT` without enumerating. */
enum iommu_state {
    IOMMU_NONE = 0,        /* no DMAR/IVRS table — the machine has none */
    IOMMU_UNUSABLE,        /* found, but it cannot do what remapping needs */
    IOMMU_PRESENT,         /* usable and NOT programmed — DMA is unrestricted */
    IOMMU_ACTIVE,          /* translation on (nothing reaches this yet) */
};

struct iommu_info {
    enum iommu_state state;
    const char* why;             /* set whenever state < IOMMU_PRESENT */

    /* WHICH VENDOR'S HARDWARE — "VT-d", "AMD-Vi", or NULL when there is none.
     * A field rather than an assumption: everything below used to be Intel's
     * vocabulary in a struct that claimed to be neutral. */
    const char* kind;

    uint64_t    reg_base;        /* remapping unit MMIO base, 0 if none      */
    int         units;           /* remapping units the firmware declared    */
    int         scope_all;       /* a unit covers every device on the bus    */
    int         host_addr_width; /* physical address width it can translate  */
    int         domains;         /* domain-id capacity                       */
    int         page_levels;     /* translation table depth it will be used at */

    /* CAN IT BE PROGRAMMED BY US, as opposed to merely found?  Detection and
     * capability are different answers, and conflating them is how a report
     * starts flattering: a backend that reads its own registers perfectly and
     * cannot yet build a page table must not look like one that can. */
    int         programmable;
};

/* Look for an IOMMU and read what it can do.  Safe to call before anything
 * else needs it; idempotent.  Never enables translation — see the file header
 * for why that separation is the point rather than an accident of staging. */
void iommu_init(void);

/* What was found.  Never NULL; on a machine with none the state is IOMMU_NONE
 * and `why` says so in words. */
const struct iommu_info* iommu_get(void);

/* One line for a report.  Kept here rather than in each caller so the shell,
 * /proc and the isolation column cannot drift into three descriptions of one
 * fact. */
const char* iommu_state_name(enum iommu_state s);

/* Backs the `iommu` command.  `iommu_cmd` handles the verbs; both shells call
 * it, so all three arches get one implementation (§M24's rule). */
void iommu_report(void);
void iommu_cmd(const char* args);

/* ----------------------------------------------------------------------
 * §M33 stage 5, second half — ACTUALLY PROGRAM IT.
 *
 * `iommu_enable` builds a root table, a context table per populated bus, and a
 * DEFAULT DOMAIN that identity-maps every frame of RAM, then points every PCI
 * device at that domain and turns translation on.
 *
 * THE IDENTITY DOMAIN IS NOT A PLACEHOLDER, IT IS THE ONLY SAFE FIRST STEP.
 * Enabling translation with a table that does not cover what the devices are
 * already using does not fail politely: the disk stops answering, the NIC stops
 * answering, and the machine dies with no way to say why — every diagnostic
 * path we would use to find out is itself a device.  So the first thing
 * translation does must be to change NOTHING observable, and the second thing
 * is to take a single device's permission away on purpose.
 *
 * OFF BY DEFAULT (`iommu.enable`).  Turning it on changes what every device on
 * the machine can reach, which is not a decision a boot should make for
 * somebody who merely happens to have the hardware.
 * ---------------------------------------------------------------------- */
int iommu_enable(void);

/* Give one device a domain that maps ONLY [base, base+len) — everything else
 * it touches becomes a fault the unit records.  This is the falsification: a
 * mechanism that translates correctly and never refuses anything is
 * indistinguishable from passthrough, so the only way to know the boundary is
 * there is to cross it on purpose and watch it stop you.
 *
 * `bdf` is (bus << 8) | (slot << 3) | func. */
int iommu_restrict(uint16_t bdf, uint64_t base, uint64_t len);

/* ADD a driver's buffer to that driver's own domain, and move the device into
 * it.  Called from `drv_dma_request` for every allocation a bound driver makes,
 * which is what turns "the machinery can confine a device" into "this driver's
 * device is confined".
 *
 * A NO-OP WHEN TRANSLATION IS NOT ON, and that has to be silent rather than an
 * error: a driver allocating DMA on a machine with no IOMMU is doing nothing
 * wrong, and failing its allocation because the machine lacks hardware would
 * make every DMA driver depend on a chipset feature.  What must NOT be silent
 * is the claim afterwards — `iommu` reports which devices are confined, so an
 * unconfined one is visible rather than assumed. */
int iommu_confine(uint16_t bdf, uint64_t base, uint64_t len);

/* The mirror of `confine`: return the device to the identity domain and free
 * the domain it had.  Called when a driver gives its resources back.
 *
 * IT IS NOT OPTIONAL, because `confine` accumulates: a driver that dies and is
 * restarted allocates a new buffer each time, and without this its device would
 * still reach every buffer of every previous incarnation while the reports kept
 * saying "confined to its own".  A restart loop turns that into a device with
 * the run of memory, one grant at a time. */
int iommu_release(uint16_t bdf);

/* Is this device confined to a domain of its own (1) or sitting in the shared
 * identity domain (0)?  What the isolation verdict is allowed to consult. */
int iommu_is_confined(uint16_t bdf);

/* How many DMA faults the unit has recorded, and the last one's address.  Read
 * from the fault-recording registers rather than counted by us: a fault that
 * only our software knows about would prove nothing about the hardware. */
uint32_t iommu_fault_count(void);

#endif /* IOMMU_H */
