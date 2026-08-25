/* =============================================================================
 * driver.c — DRIVER() registry walker + state tracking.
 *
 * Companion to module.c.  See driver.h for the design rationale and PLAN.md
 * §M8 for the milestone notes.
 *
 * State storage: the `drivers` linker section holds the static descriptors
 * (`struct driver`).  Those go in rodata-style storage and we don't mutate
 * them.  Per-driver runtime state (probed / initialized / failed) lives in
 * a parallel `driver_state_arr` byte array allocated from the heap at
 * `driver_init_all` time and indexed by (d - __start_drivers).
 * ============================================================================= */

#include "driver.h"
#include "kmalloc.h"
#include "printf.h"
#include "hal_api.h"
#include "hal.h"
#include <stddef.h>

/* Parallel state — one byte per registered driver, filled during
 * driver_init_all.  NULL until then; queries before init read 0. */
static uint8_t* driver_state_arr = NULL;
static uint32_t driver_count     = 0;

static uint32_t driver_index(const struct driver* d) {
    return (uint32_t)(d - __start_drivers);
}

/* Stop every driver that came up, in REVERSE init order.
 *
 * WHY THIS FUNCTION HAD TO BE WRITTEN AT ALL: `driver_ops.shutdown` has been
 * declared since §M8 and documented as "called on power-off / reboot", and
 * NOTHING IN THE TREE EVER CALLED IT.  Every driver that bothered to write one
 * had it dead-code the whole time.  That is §M52's shape exactly — a contract
 * stated in a header, believed by everyone reading it, and never executed —
 * and it is the first thing to fix before anything is built on top of the
 * lifecycle.
 *
 * REVERSE ORDER because init order is dependency order: the later a driver
 * came up, the more likely it sits on top of an earlier one.  Taking them down
 * front-to-back would pull the floor out from under whatever is still running.
 *
 * NOT called from a fault or watchdog path.  Those reboot from inside an
 * interrupt with the machine in an unknown state, where calling arbitrary
 * driver code is how a crash report turns into a second crash — they go
 * straight to hal_reboot() and the comment at those call sites says so. */
void driver_shutdown_all(void) {
    if (!driver_state_arr || driver_count == 0) return;
    int stopped = 0;
    for (uint32_t k = driver_count; k > 0; k--) {
        struct driver* d = &__start_drivers[k - 1];
        if (!(driver_state_arr[k - 1] & DRV_S_INITED)) continue;
        if (!d->ops || !d->ops->shutdown) continue;
        d->ops->shutdown(d->ctx);
        driver_state_arr[k - 1] &= (uint8_t)~DRV_S_INITED;
        stopped++;
    }
    kprintf("drivers: %d stopped\n", stopped);
}

uint8_t driver_state(const struct driver* d) {
    if (!driver_state_arr) return 0;
    uint32_t i = driver_index(d);
    if (i >= driver_count) return 0;
    return driver_state_arr[i];
}

void driver_init_all(void) {
    driver_count = (uint32_t)(__stop_drivers - __start_drivers);
    if (driver_count == 0) {
        kprintf("drivers: registry empty\n");
        return;
    }

    driver_state_arr = (uint8_t*)kcalloc(driver_count, 1);
    if (!driver_state_arr) {
        kprintf("drivers: OOM allocating state array\n");
        return;
    }

    int probed = 0, inited = 0, absent = 0, failed = 0;
    for (uint32_t i = 0; i < driver_count; i++) {
        struct driver* d = &__start_drivers[i];
        uint8_t st = 0;

        /* Probe — NULL probe means "always present". */
        if (d->ops && d->ops->probe) {
            int r = d->ops->probe(d->ctx);
            if (r != 0) {
                st |= DRV_S_PROBE_FAIL;
                driver_state_arr[i] = st;
                absent++;
                continue;
            }
        }
        st |= DRV_S_PROBED;
        probed++;

        /* Init — NULL init means "no setup needed". */
        if (d->ops && d->ops->init) {
            int r = d->ops->init(d->ctx);
            if (r != 0) {
                st |= DRV_S_INIT_FAIL;
                driver_state_arr[i] = st;
                kprintf("driver %s (%s) init failed: %d\n",
                        d->name, d->class, r);
                failed++;
                continue;
            }
        }
        st |= DRV_S_INITED;
        driver_state_arr[i] = st;
        inited++;
    }

    kprintf("drivers: %u registered (%d ok, %d absent, %d failed)\n",
            driver_count, inited, absent, failed);
}

/* Map state bits to a short label for the lsdrv view. */
static const char* state_label(uint8_t st) {
    if (st & DRV_S_INITED)     return "OK";
    if (st & DRV_S_INIT_FAIL)  return "init-fail";
    if (st & DRV_S_PROBE_FAIL) return "absent";
    if (st & DRV_S_PROBED)     return "probed";
    return "registered";
}

void driver_list(void) {
    if (driver_count == 0) {
        kprintf("drivers: registry empty\n");
        return;
    }
    kprintf("drivers (%u registered):\n", driver_count);
    for (uint32_t i = 0; i < driver_count; i++) {
        struct driver* d = &__start_drivers[i];
        kprintf("  [%s] %s v%s — %s\n",
                d->class ? d->class : "?",
                d->name  ? d->name  : "(unnamed)",
                d->version ? d->version : "?",
                state_label(driver_state_arr ? driver_state_arr[i] : 0));
    }
}

/* ----------------------------------------------------------------------
 * Orderly power transitions.
 *
 * ONE place, not five.  `hal_shutdown` / `hal_reboot` are called from the
 * shell, the rescue shell and the desktop's Start menu, and adding the driver
 * teardown at each of those is the shape §M63 paid for twice: miss one and
 * that route silently skips it.  These two functions ARE the deliberate route;
 * the fault paths keep calling the HAL directly, on purpose.
 * ---------------------------------------------------------------------- */
void system_power_off(void) {
    driver_shutdown_all();
    hal_shutdown();
}

void system_reboot(void) {
    driver_shutdown_all();
    hal_reboot();
}
