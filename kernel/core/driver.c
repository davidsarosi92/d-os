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
#include "cron.h"
#include "config.h"
#include "klog.h"
#include "settings.h"
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
    if (st & DRV_S_QUARANTINE) return "QUARANTINED";
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

/* ---------------------------------------------------------------------- */
/* Runtime control.                                                        */
/* ---------------------------------------------------------------------- */

static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

struct driver* driver_find(const char* name) {
    uint32_t n = (uint32_t)(__stop_drivers - __start_drivers);
    for (uint32_t i = 0; i < n; i++)
        if (__start_drivers[i].name && str_eq(__start_drivers[i].name, name))
            return &__start_drivers[i];
    return NULL;
}

int driver_stop(const char* name) {
    struct driver* d = driver_find(name);
    if (!d) { kprintf("drv: no driver '%s'\n", name); return -1; }
    uint32_t i = driver_index(d);
    if (!driver_state_arr || i >= driver_count) return -1;
    if (!(driver_state_arr[i] & DRV_S_INITED)) {
        kprintf("drv: '%s' is not running\n", name);
        return -2;
    }
    if (!d->ops || !d->ops->shutdown) {
        /* REFUSED, not forced.  A driver with no shutdown hook has no way to
         * put its hardware down or withdraw its registrations, and stopping it
         * would mean leaving a live DMA engine and a dangling device behind —
         * the exact failure this work exists to prevent. */
        kprintf("drv: '%s' has no shutdown hook — refusing to stop it\n", name);
        return -3;
    }
    d->ops->shutdown(d->ctx);
    driver_state_arr[i] &= (uint8_t)~DRV_S_INITED;
    kprintf("drv: stopped '%s'\n", name);
    return 0;
}

void driver_fault(const char* name, const char* why) {
    struct driver* d = driver_find(name);
    if (!d || !driver_state_arr) return;
    uint32_t i = driver_index(d);
    if (i >= driver_count) return;

    klog(KLOG_WARN, "drv", "'%s' faulted: %s\n", name, why ? why : "no reason given");

    /* Stop it if it CAN be stopped.  If it cannot, quarantine still applies:
     * the point is that nothing restarts it, not that we can always take it
     * down cleanly — and saying which of the two happened matters more than
     * pretending they are the same. */
    if ((driver_state_arr[i] & DRV_S_INITED) && d->ops && d->ops->shutdown) {
        d->ops->shutdown(d->ctx);
        driver_state_arr[i] &= (uint8_t)~DRV_S_INITED;
        kprintf("drv: '%s' stopped and quarantined\n", name);
    } else {
        kprintf("drv: '%s' quarantined (could not be stopped)\n", name);
    }
    driver_state_arr[i] |= DRV_S_QUARANTINE;
}

int driver_start(const char* name) {
    struct driver* d = driver_find(name);
    if (!d) { kprintf("drv: no driver '%s'\n", name); return -1; }
    uint32_t i = driver_index(d);
    if (!driver_state_arr || i >= driver_count) return -1;
    if (driver_state_arr[i] & DRV_S_INITED) {
        kprintf("drv: '%s' is already running\n", name);
        return -2;
    }
    if (d->ops && d->ops->probe && d->ops->probe(d->ctx) != 0) {
        kprintf("drv: '%s' — hardware not present\n", name);
        driver_state_arr[i] |= DRV_S_PROBE_FAIL;
        return -3;
    }
    driver_state_arr[i] |= DRV_S_PROBED;
    /* An explicit start is the user overruling the quarantine — that is what
     * makes quarantine a pause rather than a death sentence. */
    driver_state_arr[i] &= (uint8_t)~(DRV_S_PROBE_FAIL | DRV_S_INIT_FAIL |
                                      DRV_S_QUARANTINE);
    if (d->ops && d->ops->init && d->ops->init(d->ctx) != 0) {
        driver_state_arr[i] |= DRV_S_INIT_FAIL;
        kprintf("drv: '%s' failed to start\n", name);
        return -4;
    }
    driver_state_arr[i] |= DRV_S_INITED;
    kprintf("drv: started '%s'\n", name);
    return 0;
}

int driver_rescan(void) {
    if (!driver_state_arr) return 0;
    int started = 0;
    for (uint32_t i = 0; i < driver_count; i++) {
        struct driver* d = &__start_drivers[i];
        if (driver_state_arr[i] & DRV_S_INITED) continue;
        /* A driver that FAILED to init is not retried here: re-running an
         * init that just failed, on a timer, is a loop that fills the log and
         * fixes nothing (§M29's crash-loop backoff, same reasoning).  It takes
         * an explicit `drv start`. */
        if (driver_state_arr[i] & DRV_S_INIT_FAIL) continue;
        if (driver_state_arr[i] & DRV_S_QUARANTINE) continue;   /* held back */
        if (d->ops && d->ops->probe && d->ops->probe(d->ctx) != 0) continue;
        if (d->ops && d->ops->init && d->ops->init(d->ctx) != 0) {
            driver_state_arr[i] |= DRV_S_INIT_FAIL;
            continue;
        }
        driver_state_arr[i] |= DRV_S_PROBED | DRV_S_INITED;
        driver_state_arr[i] &= (uint8_t)~DRV_S_PROBE_FAIL;
        kprintf("drv: '%s' appeared — started\n", d->name);
        started++;
    }
    return started;
}

void driver_cmd(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { driver_list(); return; }

    char verb[16]; int n = 0;
    while (*args && *args != ' ' && n < (int)sizeof verb - 1) verb[n++] = *args++;
    verb[n] = 0;
    while (*args == ' ') args++;

    if (str_eq(verb, "list"))    { driver_list(); return; }
    if (str_eq(verb, "rescan"))  {
        int k = driver_rescan();
        kprintf("drv: rescan — %d driver(s) came up\n", k);
        return;
    }
    if (str_eq(verb, "stop"))    { driver_stop(args);  return; }
    if (str_eq(verb, "fault"))   { driver_fault(args, "asked for by hand"); return; }
    if (str_eq(verb, "start"))   { driver_start(args); return; }
    if (str_eq(verb, "swap")) {
        /* Two names: stop the first, start the second.  The order matters —
         * the class registry hands out the FIRST registered device, so the
         * outgoing one has to be gone before the incoming one registers or
         * the swap would appear to do nothing. */
        char a[24]; n = 0;
        while (*args && *args != ' ' && n < (int)sizeof a - 1) a[n++] = *args++;
        a[n] = 0;
        while (*args == ' ') args++;
        if (!a[0] || !*args) { kprintf("drv: swap <from> <to>\n"); return; }
        if (driver_stop(a) != 0) { kprintf("drv: swap aborted\n"); return; }
        int rc = driver_start(args);
        /* -2 is "already running", which for a SWAP is success: the target is
         * bound and the outgoing one is gone.  The first version reported it
         * as a failure — "nothing is bound now" — while the class registry had
         * in fact just handed everything to the new driver.  A message that
         * contradicts what happened is worse than no message. */
        if (rc == 0 || rc == -2)
            kprintf("drv: '%s' is now bound\n", args);
        else
            kprintf("drv: '%s' did not start — nothing is bound now\n", args);
        return;
    }
    kprintf("drv: list | rescan | stop <name> | start <name> | swap <from> <to>"
            " | fault <name>\n");
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

/* ----------------------------------------------------------------------
 * Hot-plug, step 5: hardware that appears becomes usable on its own.
 *
 * POLLED, and that is a deliberate trade rather than an oversight.  Being
 * told about a new PCI device needs the ACPI hot-plug GPE decoded and routed,
 * which is a large piece of machinery for one event; re-probing the drivers
 * that are NOT running is cheap — probe() is defined as side-effect-free and
 * usually reads a couple of config words — and it costs nothing at all once
 * every driver is up, because the loop then has nothing to look at.
 *
 * So the honest claim is "usable within one interval", not "on the instant",
 * and the interval is a setting.  0 turns it off for anyone who would rather
 * type `drv rescan`.
 * ---------------------------------------------------------------------- */
CONFIG_KEY(ck_drv_rescan) = {
    .key = "drivers.rescan_ms", .group = "System", .type = CFG_INT,
    .min = 0, .max = 60000, .def = "2000",
    .help = "how often to look for newly attached hardware, 0 = never",
};

static void job_driver_rescan(void) {
    const char* v = config_get("drivers.rescan_ms", "2000");
    int ms = 0;
    for (; *v >= '0' && *v <= '9'; v++) ms = ms * 10 + (*v - '0');
    if (ms == 0) return;                     /* switched off */
    driver_rescan();                         /* silent unless something starts */
}

CRON_JOB("driver-rescan", job_driver_rescan, 2000);
