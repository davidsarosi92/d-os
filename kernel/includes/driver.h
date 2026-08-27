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
#include "version.h"        /* DOS_VERSION — the default per-driver version */

/* Per-driver lifecycle hooks.  All can be NULL — a missing probe means
 * "always present", a missing init means "nothing to initialize", a
 * missing shutdown means "no cleanup needed". */
struct driver_ops {
    int  (*probe)   (void* ctx);    /* return 0 if hardware/resource is present */
    int  (*init)    (void* ctx);    /* class-specific bring-up; 0 = success */
    /* Clean stop.  0 = stopped, non-zero = REFUSED (the class registry has a
     * live user, so the hardware is still going and the registrations still
     * stand).
     *
     * §M67 CHANGED THIS FROM `void`, and the reason is worth keeping.  §M66
     * already had drivers that could refuse — `audio_unregister` waits for
     * users and declines rather than unlinking under one — but a `void` hook
     * had no way to say so, and `driver_stop` marked the driver stopped
     * regardless.  That was survivable for exactly one reason: a built-in
     * driver's CODE cannot be freed, so the worst case was a registry pointing
     * at a driver that was still there.
     *
     * A loadable module's code IS freed, by `rmmod`, immediately after the
     * stop.  So a refusal nobody propagates became a use-after-free on the
     * first call into a device that was never really withdrawn.
     *
     * Note what this cost: the signature changed WITHOUT changing any struct's
     * size, which is precisely the case module_abi.h's automatic fingerprint
     * cannot see — and precisely why DOS_MODULE_ABI, the number a human bumps,
     * exists next to it.  It was bumped for this. */
    int  (*shutdown)(void* ctx);
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
    const char*              version;       /* defaults to DOS_VERSION (see DRIVER) */
};

/* Boundary symbols emitted by linker.ld around the `drivers` section. */
extern struct driver __start_drivers[];
extern struct driver __stop_drivers[];

/* Walk every registered driver and run probe → init in declaration order.
 * Allocates a parallel state byte per driver from the kernel heap, so this
 * must run AFTER kmalloc_init.  Failures are logged but don't abort. */
void driver_init_all(void);

/* Stop every driver that came up, in REVERSE init order (init order is
 * dependency order).  Call it on a DELIBERATE power-off or reboot — never
 * from a fault or watchdog path, where the machine's state is unknown and
 * running arbitrary driver code turns a crash report into a second crash.
 *
 * NB: this exists because `shutdown` above was declared in §M8, documented as
 * "called on power-off / reboot", and never actually called by anything. */
void driver_shutdown_all(void);

/* Diagnostic — print the registry to the console with each driver's
 * runtime state.  Backs the `lsdrv` shell command. */
void driver_list(void);

/* Deliberate power transitions: stop the drivers, then ask the machine to go.
 * Use these from anything a PERSON triggered.  A fault or watchdog path must
 * NOT — it calls hal_reboot() directly, because running driver code with the
 * machine in an unknown state is how a crash report becomes a second crash. */
void system_power_off(void);
void system_reboot(void);

/* ----------------------------------------------------------------------
 * Runtime control (steps 3-5 of the driver-agility work).
 *
 * A driver can be stopped, started and re-probed while the system runs.  All
 * three go through the SAME state array `driver_init_all` fills, so `lsdrv`
 * keeps telling the truth about what is running.
 *
 * What makes stopping safe is not this layer: it is that the class registry
 * the driver publishes into (audio, block, ...) can REFUSE to let go while
 * somebody is inside a call.  A stop that the class refuses leaves the driver
 * running and says so — see audio_unregister().
 * ---------------------------------------------------------------------- */
struct driver* driver_find(const char* name);

/* Add a descriptor the LINKER did not place — what a module loader calls once
 * it has relocated a driver into kernel memory.  The registry keeps slots
 * rather than indexing the `drivers` section precisely so that a loaded driver
 * and a built-in one are the same kind of thing everywhere else. */
int driver_attach(struct driver* d);

/* The mirror: remove a slot the loader added, so its module's memory can be
 * freed.  Refuses a driver that is still running (its class registrations are
 * live) and refuses a BUILT-IN one outright — that descriptor is in the
 * kernel's own rodata and detaching it would hide a driver that still exists
 * rather than remove one.  Returns 0 on success. */
int driver_detach(struct driver* d);

/* Stop one driver: run its shutdown hook and clear INITED.  Returns 0 on
 * success, non-zero if it was not running or has no way to stop. */
int driver_stop(const char* name);

/* Report that a running driver has misbehaved: stop it if it can be stopped,
 * and quarantine it so nothing restarts it behind the user's back.  Meant to
 * be called BY a subsystem that noticed — a device that stopped answering, a
 * driver that returned nonsense — rather than by the driver itself. */
void driver_fault(const char* name, const char* why);

/* Probe + init one driver that is not currently running. */
int driver_start(const char* name);

/* Re-probe every driver that is NOT running, and init the ones whose hardware
 * has appeared.  This is what makes newly attached hardware usable: cheap,
 * idempotent, and safe to call at any time.  Returns how many came up. */
int driver_rescan(void);

/* The `drv` shell command — implemented next to the registry rather than in a
 * shell, so the ARM serial REPL runs the same one (§M24's rule). */
void driver_cmd(const char* args);

/* Per-driver state bits exposed in case future code wants to query without
 * going through the human-readable list. */
#define DRV_S_PROBED      0x01  /* probe() returned 0 (or NULL probe) */
#define DRV_S_INITED      0x02  /* init() returned 0 (or NULL init) */
#define DRV_S_PROBE_FAIL  0x04
#define DRV_S_INIT_FAIL   0x08
/* §M33-lite — QUARANTINED: this driver misbehaved and will not be started
 * again automatically.  It takes an explicit `drv start` (which clears it), so
 * a broken driver costs one failed attempt rather than a restart loop that
 * fills the log and fixes nothing — §M29's crash-loop reasoning, applied to
 * hardware bring-up.
 *
 * NOTE WHAT THIS IS AND IS NOT.  It contains the CONSEQUENCES of a driver that
 * fails or misreports; it does NOT contain a driver that corrupts memory,
 * because in one address space the damage is already done by the time anything
 * notices.  That is §M33's execution domains, and calling this isolation would
 * be exactly the "isolation theatre" that plan refuses. */
#define DRV_S_QUARANTINE  0x10
/* Stopped BY HAND.  Distinct from quarantine — "the user turned it off" and
 * "it misbehaved" are different facts and `lsdrv` should not conflate them —
 * but with the same effect on the automatic paths: neither is restarted
 * behind the user's back.  Without this a `drv stop` (or the stop half of a
 * swap) was silently undone by the next hot-plug rescan, about two seconds
 * later, which looks like the command did nothing. */
#define DRV_S_ADMIN_DOWN  0x20

uint8_t driver_state(const struct driver* d);

/* Macro hygiene mirrors MODULE() in module.h — `aligned(4)` must match
 * `sizeof(struct driver)` to keep array-stride iteration correct.  See the
 * file-level comment for the M2 lesson learned. */
#define DRIVER(_name, _class, _ops_ptr, _ctx_ptr)                         \
    static const struct driver                                            \
    __attribute__((used, section("drivers"), aligned(4)))                 \
    __drv_def_##_name = {                                                 \
        .name    = #_name,                                                \
        .class   = (_class),                                              \
        .ops     = (_ops_ptr),                                            \
        .ctx     = (_ctx_ptr),                                            \
        .version = DOS_VERSION,                                           \
    }

#endif
