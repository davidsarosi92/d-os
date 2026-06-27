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
#include <stddef.h>

/* Parallel state — one byte per registered driver, filled during
 * driver_init_all.  NULL until then; queries before init read 0. */
static uint8_t* driver_state_arr = NULL;
static uint32_t driver_count     = 0;

static uint32_t driver_index(const struct driver* d) {
    return (uint32_t)(d - __start_drivers);
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
        kprintf("  [%s] %s — %s\n",
                d->class ? d->class : "?",
                d->name  ? d->name  : "(unnamed)",
                state_label(driver_state_arr ? driver_state_arr[i] : 0));
    }
}
