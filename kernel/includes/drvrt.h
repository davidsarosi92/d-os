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

struct drv_res;

/* One driver's resources.  Embedded in the driver's own state, so there is no
 * allocation to fail and nothing to leak if bring-up dies half way. */
struct drv_rt {
    const char*    owner;                 /* driver name, for conflict reports */
    struct drv_res* res[DRV_MAX_RES];
    int             nres;
};

/* Start a runtime context.  `owner` must outlive it (a string literal or the
 * driver's own name field) — it is what a conflict message quotes. */
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
 * DMA.  Two addresses, always, because they are two different numbers and
 * conflating them is the classic driver bug: the CPU pointer to touch it with,
 * and the address to PROGRAM INTO THE DEVICE.  They are equal today on every
 * target here; they are not equal under an IOMMU, which is exactly what §M33
 * stage 5 introduces — so the interface separates them now, while the
 * separation is free.
 * ---------------------------------------------------------------------- */
drv_handle drv_dma_request(struct drv_rt* rt, size_t bytes, const char* why);
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

/* Diagnostics: print what a driver is holding.  Backs `drv res`. */
void drv_res_dump(void);

#endif /* DRVRT_H */
