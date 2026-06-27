/* =============================================================================
 * driver.h — richer driver registry layered on top of the MODULE() framework.
 *
 * `MODULE()` (kernel/includes/module.h) registers a single init function — it
 * gets the driver booted, no questions asked.  That works fine for monolithic
 * legacy drivers (serial, PIT, ...) but starts to creak as we add hardware
 * we *might or might not* have (block devices behind PCI, USB controllers,
 * ACPI-discovered NICs).
 *
 * `DRIVER()` is the next-generation registration.  Each driver provides:
 *   - probe()    — cheap, side-effect-free check: "is this hardware present?"
 *   - init()     — bring it up; only called when probe succeeds.
 *   - shutdown() — clean stop; called on power-off / reboot, may be NULL.
 *
 * Plus class metadata so devfs (M9) and procfs (M10) can group / iterate.
 *
 * The two registries coexist: existing drivers stay on MODULE() until there's
 * a concrete reason to migrate.  New drivers prefer DRIVER().  See PLAN.md
 * §M8 and §DRV for rationale.
 *
 * Storage: same linker-section trick as module.h.  Iteration walks
 * [__start_drivers, __stop_drivers).  Stride must match `sizeof(struct
 * driver)` exactly — keep `aligned(4)` matched to the 16-byte struct size on
 * i386 (lesson from M2: alignment > sizeof leaves padding the iterator skips
 * over, → page fault).
 * ============================================================================= */

#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>

/* Per-driver lifecycle hooks.  All can be NULL — a missing probe means
 * "always present", a missing init means "nothing to initialize", a
 * missing shutdown means "no cleanup needed". */
struct driver_ops {
    int  (*probe)   (void* ctx);    /* return 0 if hardware/resource is present */
    int  (*init)    (void* ctx);    /* class-specific bring-up; 0 = success */
    void (*shutdown)(void* ctx);    /* clean stop; called on reboot/shutdown */
};

/* The registry entry.  Kept tight (5 fields → 20 bytes? no — only 4 pointers
 * = 16 bytes on i386) and `aligned(4)` so iteration stride matches sizeof.
 *
 * `ctx` is class-specific opaque state — for a console sink driver it's a
 * `struct console_sink*`, for a block driver a `struct block_device*`, etc.
 * The driver's own ops know what to cast it to. */
struct driver {
    const char*              name;          /* short identifier, e.g. "null" */
    const char*              class;         /* "char", "block", "input", ... */
    const struct driver_ops* ops;
    void*                    ctx;
};

/* Boundary symbols emitted by linker.ld around the `drivers` section. */
extern struct driver __start_drivers[];
extern struct driver __stop_drivers[];

/* Walk every registered driver and run probe → init in declaration order.
 * Allocates a parallel state byte per driver from the kernel heap, so this
 * must run AFTER kmalloc_init.  Failures are logged but don't abort. */
void driver_init_all(void);

/* Diagnostic — print the registry to the console with each driver's
 * runtime state.  Backs the `lsdrv` shell command. */
void driver_list(void);

/* Per-driver state bits exposed in case future code wants to query without
 * going through the human-readable list. */
#define DRV_S_PROBED      0x01  /* probe() returned 0 (or NULL probe) */
#define DRV_S_INITED      0x02  /* init() returned 0 (or NULL init) */
#define DRV_S_PROBE_FAIL  0x04
#define DRV_S_INIT_FAIL   0x08

uint8_t driver_state(const struct driver* d);

/* Macro hygiene mirrors MODULE() in module.h — `aligned(4)` must match
 * `sizeof(struct driver)` to keep array-stride iteration correct.  See the
 * file-level comment for the M2 lesson learned. */
#define DRIVER(_name, _class, _ops_ptr, _ctx_ptr)                         \
    static const struct driver                                            \
    __attribute__((used, section("drivers"), aligned(4)))                 \
    __drv_def_##_name = {                                                 \
        .name  = #_name,                                                  \
        .class = (_class),                                                \
        .ops   = (_ops_ptr),                                              \
        .ctx   = (_ctx_ptr),                                              \
    }

#endif
