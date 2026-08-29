/* =============================================================================
 * drvrt.h — the driver-runtime API (§M33 stage 2): the narrow waist a driver
 * is written against instead of calling `outb` / `vmm_map` / `irq_install`.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS EXISTS, AND WHY ITS SHAPE IS DECIDED NOW
 *
 * §M33's goal is that the SAME driver source runs in ring 0 or in ring 3, with
 * the choice made by config.  That is only possible if the driver never names a
 * kernel address, never installs a callback the kernel will call, and never
 * touches a port it was not given.  Every one of those is impossible to bolt on
 * afterwards — they are properties of the interface, not of the implementation.
 *
 * So the API is defined in its FINAL, IPC-SHAPED form while only the in-kernel
 * backend exists.  That is convention #5 in CLAUDE.md, and this milestone is
 * the case it was written for: a "we'll wrap it later" version of this file
 * would work perfectly and would not fit a process boundary, and nobody would
 * find out until Tier 1.
 *
 * -----------------------------------------------------------------------------
 * THE FOUR RULES THE SHAPE FOLLOWS
 *
 * 1. **HANDLES, NOT POINTERS.**  Every resource is a small integer.  A pointer
 *    is meaningless in another address space; an integer is the same number
 *    everywhere.  This costs a lookup in the in-kernel backend and is the only
 *    reason the user backend is writable at all.
 *
 * 2. **OFFSETS, NOT ABSOLUTE ADDRESSES.**  `drv_out8(h, off, v)` and not
 *    `outb(port, v)`: the handle carries the base and the length, so a driver
 *    CANNOT REACH A PORT IT WAS NOT GRANTED — the arithmetic to do so does not
 *    exist in the interface.  The bound is checked in the in-kernel backend
 *    too, deliberately: a check that only runs in the configuration nobody
 *    tests is a check that does not work.
 *
 * 3. **THE DRIVER WAITS; IT IS NOT CALLED.**  `drv_irq_wait()` BLOCKS.  A
 *    callback cannot cross a process boundary, so an interrupt has to become
 *    something a driver waits on — and the in-kernel backend's ISR then does
 *    exactly one thing (wake) and no second thing.  That is §M49's xHCI lesson
 *    (a drain inside an ISR reached code that blocks) and §M55's NIC lesson
 *    (the ISR acks and wakes; it does not drain) promoted from "something each
 *    driver rediscovers" to the only shape the API offers.
 *
 * 4. **ONE CONTEXT OWNS EVERYTHING.**  A driver's resources hang off a
 *    `struct drv_rt`, and `drv_release_all()` returns the lot.  §M66 made a
 *    shutdown hook mandatory and §M67 made a missing one a refusal to load;
 *    this makes writing a correct one mechanical instead of a checklist.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS DELIBERATELY ABSENT
 *
 * No `drv_malloc`.  A driver's private state is its own business and does not
 * cross the boundary; `kmalloc` is already exported to modules (§M67).  DMA is
 * separate precisely because it DOES cross — the device sees it.
 *
 * No callback registration of any kind.  See rule 3.
 * ============================================================================= */

#ifndef DRVRT_H
#define DRVRT_H

#include <stdint.h>
#include <stddef.h>

#define DRV_MAX_RES 8           /* resources one driver may hold */

/* A handle is an index into the owning context, +1 so that 0 is never valid.
 * Negative values are errors, and they are the SAME errors in both backends. */
typedef int drv_handle;

#define DRV_EBUSY   (-1)        /* somebody else holds it — the report names who */
#define DRV_ERANGE  (-2)        /* offset outside the granted window */
#define DRV_ENORES  (-3)        /* the context is full, or the kernel is out */
#define DRV_EBAD    (-4)        /* not a handle this context owns */
#define DRV_ENOSYS  (-5)        /* this machine has no such resource kind */
#define DRV_ETIME   (-6)        /* drv_irq_wait timed out */
/* The kernel is asking this driver to stop.  A COOPERATIVE STOP THAT CROSSES
 * THE PROCESS BOUNDARY: in ring 0 the body polls task_should_stop(), and a
 * driver in ring 3 cannot see that flag at all — so the request rides back on
 * the one call the body is always inside, and the user-mode backend turns it
 * into the same task_should_stop() the in-kernel driver already checks.
 *
 * Without it a placed driver was effectively UNKILLABLE for the length of its
 * own timeout: `task_kill` woke it, the ring-3 stub answered "no, keep going",
 * and it blocked again.  The kernel's force-kill lands only at a timer
 * preemption taken IN USER MODE, which a driver that spends its life blocked in
 * a syscall reaches about once per timeout — so a one-second wait meant a
 * one-second window, and a stop looked like it had been ignored. */
#define DRV_ESTOP   (-7)

struct drv_res;

/* One driver's resources.  Embedded in the driver's own state, so there is no
 * allocation to fail and nothing to leak if bring-up dies half way. */
struct drv_rt {
    const char*    owner;                 /* driver name, for conflict reports */
    struct drv_res* res[DRV_MAX_RES];
    int             nres;
    /* Which PCI device this driver speaks for, (bus<<8)|(slot<<3)|func, or
     * 0xFFFF for "not a PCI driver / not declared".  See drv_bind_device. */
    uint16_t        bdf;
    /* The task drv_run spawned, so drv_release_all can stop it.  A driver's
     * resources and the loop that uses them have ONE lifetime, and keeping
     * them separate meant a stopped driver left a task spinning on handles
     * that no longer existed — see drv_release_all. */
    int             body_pid;
};

/* Start a runtime context.  `owner` must outlive it (a string literal or the
 * driver's own name field) — it is what a conflict message quotes.
 *
 * `owner` MUST BE THE REGISTRY NAME, spelled exactly as `struct driver.name`
 * and as drvuser's manifest.  It is not merely a label: everything that asks
 * "what does this driver hold" matches on this string, so a driver that spells
 * it differently in one place appears to the resource table as a SECOND driver.
 * The PS/2 mouse did exactly that — `ps2-mouse` here against `ps2_mouse` in the
 * registry and the manifest — so its in-kernel and ring-3 placements held their
 * ports and IRQ under two different owners.  It was invisible until something
 * tried to join the two views up. */
void drv_rt_init(struct drv_rt* rt, const char* owner);

/* Return every resource: ports released, MMIO unmapped, IRQ handler removed,
 * DMA freed.  Idempotent, so a shutdown hook that runs twice is harmless — and
 * §M66 does call shutdown from more than one path. */
void drv_release_all(struct drv_rt* rt);

/* ----------------------------------------------------------------------
 * Port I/O.  x86 only; on a machine with no port space every call returns
 * DRV_ENOSYS rather than pretending — a driver that needs ports is a driver
 * for hardware that arch does not have.
 * ---------------------------------------------------------------------- */
drv_handle drv_ports_request(struct drv_rt* rt, uint16_t base, uint16_t count,
                             const char* why);
int  drv_in8  (drv_handle h, uint16_t off);          /* <0 on error */
int  drv_in16 (drv_handle h, uint16_t off);
long drv_in32 (drv_handle h, uint16_t off);
int  drv_out8 (drv_handle h, uint16_t off, uint8_t v);
int  drv_out16(drv_handle h, uint16_t off, uint16_t v);
int  drv_out32(drv_handle h, uint16_t off, uint32_t v);

/* ----------------------------------------------------------------------
 * EXCLUSIVE ACCESS TO A SHARED CONTROLLER.
 *
 * Some devices are shared by two drivers that cannot see each other.  The 8042
 * is the case this exists for: it has ONE output buffer, and a controller's
 * answer to a command is indistinguishable from a keystroke — same port, AUX
 * bit clear — so it raises the KEYBOARD's interrupt and the keyboard driver
 * reads it.  Whoever asked the question gets a timeout.
 *
 * WHY IT IS AN OPERATION AND NOT SOMETHING THE DRIVER DOES ITSELF.  In ring 0
 * a driver would mask the other line and be done.  A driver in ring 3 cannot,
 * and should not be able to — that is one of the privileges the placement
 * exists to take away.  So the exclusion is something the RUNTIME performs on
 * request, identically on both sides, and the driver's source is unchanged
 * between placements.  Which line competes is the KERNEL's knowledge: a driver
 * allowed to name it could name the timer's.
 *
 * THE CLAIM IS ALWAYS BOUNDED.  `max_ms` is clamped, and when it expires the
 * kernel takes the claim back and says so — because a ring-3 driver holds this
 * across a return to user mode, where it can be killed or preempted or simply
 * wrong, and a line left masked is a device that has silently stopped working.
 * It is also released if the driver's resources go back, so a crash cannot
 * leave the keyboard dead behind it.
 *
 * Hold it for a transaction, never for a phase of work.
 * ---------------------------------------------------------------------- */
int drv_ports_lock(drv_handle h, int max_ms);
int drv_ports_unlock(drv_handle h);

/* ----------------------------------------------------------------------
 * WHERE IS MY DEVICE?
 *
 * A driver in ring 0 finds its own BAR by reading PCI config space.  A driver in
 * ring 3 CANNOT, and should not be able to: config space reaches every device on
 * the machine, so handing it over would give a placed driver more reach than the
 * kernel one it replaced — the opposite of the point.
 *
 * That is not a gap discovered late; it is what the first MMIO driver written
 * against this API immediately ran into.  So a driver asks the RUNTIME where its
 * window is, and the kernel — which enumerated the bus and decided this driver
 * may speak for this device — answers.  The in-kernel backend reads config space
 * on the driver's behalf; the user backend asks across the boundary.  Same call,
 * same answer, and the driver's source does not know which.
 *
 * The kernel also performs the privileged half of bring-up (memory space and bus
 * master enable) at the same point, because those are config-space writes for the
 * same reason.
 * ---------------------------------------------------------------------- */
int drv_device_window(struct drv_rt* rt, int bar, uint64_t* phys, uint64_t* len);

/* ----------------------------------------------------------------------
 * MMIO.  The pointer is REAL in the in-kernel backend and would be a mapping
 * into the driver's own space in the user one — which is why the driver gets it
 * from a handle rather than computing it: the number differs per backend and
 * the driver must not care.
 * ---------------------------------------------------------------------- */
drv_handle     drv_mmio_request(struct drv_rt* rt, uint64_t phys, size_t len,
                                const char* why);
volatile void* drv_mmio_ptr(drv_handle h);

/* ----------------------------------------------------------------------
 * Interrupts.  See rule 3 — this is the part of the API whose shape matters
 * most, and the part that changes how a driver is written.
 * ---------------------------------------------------------------------- */
drv_handle drv_irq_request(struct drv_rt* rt, int line, const char* why);

/* Block until the line fires, or until `timeout_ms` passes (0 = no wait, <0 =
 * forever).  Returns the number of interrupts observed since the last call —
 * NOT a boolean, because a driver that slept through three of them needs to
 * know that rather than to process one and lose two.  DRV_ETIME on timeout.
 *
 * A TIMEOUT IS NOT AN ERROR AND MUST NOT BE TREATED AS ONE.  §M55's rule: a
 * driver whose interrupt never arrives should degrade to polling and keep
 * working, not block forever on a promise the hardware did not keep. */
int drv_irq_wait(drv_handle h, int timeout_ms);

/* How many times this line has fired in total.  Diagnostics — an interrupt
 * count that stays at zero is how "the device is wired but silent" is told
 * apart from "the driver is not looking". */
uint32_t drv_irq_count(drv_handle h);

/* ----------------------------------------------------------------------
 * WHICH DEVICE THIS DRIVER SPEAKS FOR.
 *
 * Bookkeeping until there is an IOMMU, and then the load-bearing piece: the
 * kernel can build a domain holding exactly this driver's DMA buffers, and it
 * cannot put the DEVICE into that domain without being told which device the
 * driver is for.  A DMA driver that never calls this is one the kernel can only
 * leave in the identity domain — reachable by every byte of RAM — and it will
 * be REPORTED that way rather than assumed to be fine.
 *
 * Called before the first drv_dma_request; calling it later still works, and
 * confines the device from that point on.
 * ---------------------------------------------------------------------- */
void drv_bind_device(struct drv_rt* rt, uint16_t bdf);

/* ----------------------------------------------------------------------
 * DMA.  Two addresses, always, because they are two different numbers and
 * conflating them is the classic driver bug: the CPU pointer to touch it with,
 * and the address to PROGRAM INTO THE DEVICE.  They are equal today on every
 * target here; they are not equal under an IOMMU, which is exactly what §M33
 * stage 5 introduces — so the interface separates them now, while the
 * separation is free.
 * ---------------------------------------------------------------------- */
/* `addr_bits` — HOW MANY ADDRESS BITS THE DEVICE ACTUALLY HAS.  32 for most
 * things, and this argument exists because the FIRST driver written against
 * this API needed something else.
 *
 * The educational device addresses 28 bits.  Without the parameter it was
 * handed a DMA32 buffer at ~1023 MiB, the device silently CLAMPED the address
 * to 28 bits and read some other page — the transfer "completed", the status
 * register cleared, and 255 of 256 bytes came back wrong with nothing anywhere
 * saying why.  *A device quietly truncating an address it cannot hold is the
 * classic DMA bug, and an API that cannot express the constraint guarantees
 * every driver rediscovers it.*
 *
 * The allocation is checked against the limit afterwards and REFUSED if it does
 * not fit, because a zone is a coarse approximation of an arbitrary bit width
 * and handing back an address the device cannot reach is exactly the failure
 * being prevented. */
drv_handle drv_dma_request(struct drv_rt* rt, size_t bytes, int addr_bits,
                           const char* why);
void*      drv_dma_cpu(drv_handle h);
uint64_t   drv_dma_device(drv_handle h);

/* ----------------------------------------------------------------------
 * Running the driver body.
 *
 * A driver that waits on an interrupt IS a task, so the API supplies the task
 * rather than making each driver spawn one — and, more importantly, it solves a
 * chicken-and-egg the drivers cannot: **`driver_init_all()` runs BEFORE
 * `task_init()`**, so a driver bringing itself up at boot has no scheduler to
 * spawn into yet.
 *
 * `drv_run()` therefore spawns immediately if the scheduler is up, and QUEUES
 * otherwise; `drvrt_start_deferred()` is called once, from each boot path, as
 * soon as tasks exist.  A driver started later (a hot-plug rescan, `drv start`,
 * an `insmod`) takes the immediate path and never notices the difference.
 *
 * §M33's plan flags the same problem for boot-critical drivers and answers it
 * by pinning them to DOMAIN_KERNEL.  This is the other half: the ones that are
 * NOT boot-critical still start before the scheduler, and needed somewhere for
 * that fact to live other than in each driver's head.
 * ---------------------------------------------------------------------- */
int  drv_run(struct drv_rt* rt, const char* name, void (*body)(void));

/* Spawn every queued driver body.  Called once per boot path, right after
 * task_init().  Idempotent. */
void drvrt_start_deferred(void);

/* ----------------------------------------------------------------------
 * Recording a grant the KERNEL made on a placed driver's behalf.
 *
 * A ring-3 driver's MMIO window and DMA buffer are not obtained through
 * `drv_mmio_request` / `drv_dma_request`: the kernel maps them into the
 * DRIVER's address space, not its own, so those calls would do the wrong
 * thing.  The consequence was that a placed driver's two most important
 * holdings appeared in no resource table at all — `drv res` listed its ports
 * and its IRQ and silently omitted the register window and the DMA buffer,
 * which for a DMA driver is very nearly the whole answer.
 *
 * These record the grant without performing it, so ONE table answers "what
 * does this driver hold" for both placements — and so `drv_release_all` frees
 * the DMA frames on exactly one path instead of the caller keeping a private
 * copy of the address and the count.
 * ---------------------------------------------------------------------- */
drv_handle drv_res_note_mmio(struct drv_rt* rt, uint64_t phys, uint64_t len,
                             uintptr_t va, const char* why);
drv_handle drv_res_note_dma(struct drv_rt* rt, uint64_t phys, uint64_t len,
                            uintptr_t va, uint64_t dev, const char* why);

/* Diagnostics: print what a driver is holding.  Backs `drv res`. */
void drv_res_dump(void);

/* The same facts as one short string — "ports, irq 12, dma" — for a caller
 * that has a COLUMN rather than a console.  Written because the device manager
 * needs it and `drv_res_dump` can only print: a panel that re-walked the
 * resource table itself would be a second reader of a structure this file owns,
 * and the two would drift the first time a resource kind is added.
 *
 * Always NUL-terminates.  An owner holding nothing yields an empty string
 * rather than a word, because the caller decides how "nothing" reads in its
 * own layout. */
void drv_res_summary(const char* owner, char* out, int cap);

#endif /* DRVRT_H */
