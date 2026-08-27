/* =============================================================================
 * driver.c — DRIVER() registry walker + state tracking.
 *
 * Companion to module.c.  See driver.h for the design rationale and PLAN.md
 * §M8 for the milestone notes.
 *
 * State storage: the `drivers` linker section holds the static descriptors
 * (`struct driver`).  Those go in rodata-style storage and we don't mutate
 * them.  Per-driver runtime state (probed / initialized / failed) lives in
 * a SLOT TABLE (see below) that holds the descriptor pointer, so a driver
 * loaded from outside the kernel image is the same kind of thing as one the
 * linker placed.
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

/* ----------------------------------------------------------------------
 * The registry is a SLOT TABLE, not the linker array.
 *
 * It used to index straight into the `drivers` section — state lived in a
 * parallel byte array addressed by `d - __start_drivers`.  That is exact and
 * cheap for drivers built into the image, and it makes a driver from anywhere
 * else IMPOSSIBLE: a descriptor that is not in the section produces a
 * meaningless difference, and the state lookup lands wherever that pointer
 * arithmetic happens to point.
 *
 * A slot holds the descriptor pointer instead, so a built-in and a loaded
 * driver differ only in where their `struct driver` lives.  Bounded like every
 * other table here, and the ceiling is reported rather than silently applied.
 * ---------------------------------------------------------------------- */
#define DRV_MAX_SLOTS 64

struct drv_slot {
    struct driver* d;
    uint8_t        state;
    uint8_t        dynamic;      /* came from outside the kernel image */
};

static struct drv_slot g_slots[DRV_MAX_SLOTS];
static uint32_t        driver_count = 0;
static int             registry_ready = 0;

static struct drv_slot* slot_of(const struct driver* d) {
    for (uint32_t i = 0; i < driver_count; i++)
        if (g_slots[i].d == d) return &g_slots[i];
    return NULL;
}

/* Append a descriptor the linker did not place.  This is what a module loader
 * calls; nothing else should. */
int driver_attach(struct driver* d) {
    if (!d || !d->name) return -1;
    if (!registry_ready) return -2;          /* before driver_init_all */
    if (driver_find(d->name)) {
        kprintf("drv: '%s' is already registered\n", d->name);
        return -3;
    }
    if (driver_count >= DRV_MAX_SLOTS) {
        kprintf("drv: registry full (%d) — '%s' not attached\n",
                DRV_MAX_SLOTS, d->name);
        return -4;
    }
    g_slots[driver_count].d       = d;
    g_slots[driver_count].state   = 0;
    g_slots[driver_count].dynamic = 1;
    driver_count++;
    kprintf("drv: attached '%s' (%s)\n", d->name, d->class ? d->class : "?");
    return 0;
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
    if (driver_count == 0) return;
    int stopped = 0, refused = 0;
    for (uint32_t k = driver_count; k > 0; k--) {
        struct drv_slot* sl = &g_slots[k - 1];
        if (!(sl->state & DRV_S_INITED)) continue;
        if (!sl->d->ops || !sl->d->ops->shutdown) continue;
        /* A power-off proceeds even if a driver declines: the machine is going
         * away either way, and refusing to shut down the OTHER drivers because
         * one is busy would be strictly worse.  It is COUNTED separately so the
         * line does not claim more than happened. */
        if (sl->d->ops->shutdown(sl->d->ctx) != 0) { refused++; continue; }
        sl->state &= (uint8_t)~DRV_S_INITED;
        stopped++;
    }
    if (refused) kprintf("drivers: %d stopped, %d still busy\n", stopped, refused);
    else         kprintf("drivers: %d stopped\n", stopped);
}

uint8_t driver_state(const struct driver* d) {
    struct drv_slot* sl = slot_of(d);
    return sl ? sl->state : 0;
}

void driver_init_all(void) {
    uint32_t built_in = (uint32_t)(__stop_drivers - __start_drivers);
    if (built_in == 0) {
        kprintf("drivers: registry empty\n");
        registry_ready = 1;
        return;
    }
    if (built_in > DRV_MAX_SLOTS) {
        kprintf("drivers: %u built in, only %d slots — the rest are ignored\n",
                built_in, DRV_MAX_SLOTS);
        built_in = DRV_MAX_SLOTS;
    }

    /* Seed the slot table from the linker section.  From here on nothing
     * indexes that section directly — a built-in driver and a loaded one are
     * the same thing to every operation below. */
    for (uint32_t i = 0; i < built_in; i++) {
        g_slots[i].d       = &__start_drivers[i];
        g_slots[i].state   = 0;
        g_slots[i].dynamic = 0;
    }
    driver_count = built_in;
    registry_ready = 1;

    int probed = 0, inited = 0, absent = 0, failed = 0;
    for (uint32_t i = 0; i < driver_count; i++) {
        struct drv_slot* sl = &g_slots[i];
        struct driver* d = sl->d;
        uint8_t st = 0;

        /* Probe — NULL probe means "always present". */
        if (d->ops && d->ops->probe) {
            if (d->ops->probe(d->ctx) != 0) {
                sl->state = DRV_S_PROBE_FAIL;
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
                sl->state = st;
                kprintf("driver %s (%s) init failed: %d\n", d->name, d->class, r);
                failed++;
                continue;
            }
        }
        st |= DRV_S_INITED;
        sl->state = st;
        inited++;
    }

    kprintf("drivers: %u registered (%d ok, %d absent, %d failed)\n",
            driver_count, inited, absent, failed);
    (void)probed;
}

static const char* state_label(uint8_t st) {
    if (st & DRV_S_QUARANTINE) return "QUARANTINED";
    if (st & DRV_S_ADMIN_DOWN) return "stopped";
    if (st & DRV_S_INITED)     return "OK";
    if (st & DRV_S_INIT_FAIL)  return "INIT FAILED";
    if (st & DRV_S_PROBE_FAIL) return "absent";
    if (st & DRV_S_PROBED)     return "probed";
    return "-";
}

void driver_list(void) {
    if (driver_count == 0) { kprintf("drivers: registry empty\n"); return; }
    kprintf("drivers (%u):\n", driver_count);
    for (uint32_t i = 0; i < driver_count; i++) {
        struct drv_slot* sl = &g_slots[i];
        kprintf("  [%s] %s v%s — %s%s\n",
                sl->d->class ? sl->d->class : "?", sl->d->name,
                sl->d->version ? sl->d->version : "?",
                state_label(sl->state),
                sl->dynamic ? "  (loaded)" : "");
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
    for (uint32_t i = 0; i < driver_count; i++)
        if (g_slots[i].d->name && str_eq(g_slots[i].d->name, name))
            return g_slots[i].d;
    return NULL;
}

/* Remove a slot the loader added.  The mirror of driver_attach, and §M67's
 * last requirement of this file.
 *
 * TWO REFUSALS, AND BOTH ARE THE POINT.  A driver that is still INITED is one
 * whose hardware is live and whose class registrations are still published —
 * detaching it would leave those registries pointing at a descriptor that is
 * about to be freed, which is §M54's defect class exactly.  And a BUILT-IN
 * driver cannot be detached at all: its descriptor lives in the kernel's own
 * rodata, so removing the slot would not free anything and would only make a
 * driver that still exists invisible to `lsdrv`.
 *
 * The compaction below preserves ORDER, because driver_shutdown_all walks the
 * table backwards to get reverse-init order.  Swapping the last slot into the
 * hole would be cheaper and would silently corrupt that ordering — a bug that
 * shows up only at power-off, on a machine that has unloaded a module. */
int driver_detach(struct driver* d) {
    if (!d) return -1;
    struct drv_slot* sl = slot_of(d);
    if (!sl) return -2;
    if (!sl->dynamic) {
        kprintf("drv: '%s' is built in — it cannot be detached\n", d->name);
        return -3;
    }
    if (sl->state & DRV_S_INITED) {
        kprintf("drv: '%s' is still running — stop it first\n", d->name);
        return -4;
    }
    uint32_t idx = (uint32_t)(sl - g_slots);
    for (uint32_t i = idx; i + 1 < driver_count; i++) g_slots[i] = g_slots[i + 1];
    driver_count--;
    g_slots[driver_count].d = NULL;
    g_slots[driver_count].state = 0;
    g_slots[driver_count].dynamic = 0;
    kprintf("drv: detached '%s'\n", d->name);
    return 0;
}

int driver_stop(const char* name) {
    struct driver* d = driver_find(name);
    if (!d) { kprintf("drv: no driver '%s'\n", name); return -1; }
    struct drv_slot* sl = slot_of(d);
    if (!sl) return -1;
    if (!(sl->state & DRV_S_INITED)) {
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
    /* §M67 — HONOUR THE REFUSAL.  The hook used to return void and this code
     * used to clear INITED unconditionally.  A class that declines to be
     * withdrawn (audio_unregister, while somebody is inside a call) then had
     * its objection recorded nowhere, and the driver was reported stopped while
     * its device was still live and still registered.  Harmless while every
     * driver was built in; a use-after-free the moment `rmmod` can free the
     * code behind it. */
    if (d->ops->shutdown(d->ctx) != 0) {
        kprintf("drv: '%s' refused to stop — it is still in use\n", name);
        return -4;
    }
    sl->state &= (uint8_t)~DRV_S_INITED;
    sl->state |= DRV_S_ADMIN_DOWN;          /* stays down until asked back */
    kprintf("drv: stopped '%s'\n", name);
    return 0;
}

void driver_fault(const char* name, const char* why) {
    struct driver* d = driver_find(name);
    if (!d) return;
    struct drv_slot* sl = slot_of(d);
    if (!sl) return;

    klog(KLOG_WARN, "drv", "'%s' faulted: %s\n", name, why ? why : "no reason given");

    /* Stop it if it CAN be stopped.  If it cannot, quarantine still applies:
     * the point is that nothing restarts it, not that we can always take it
     * down cleanly — and saying which of the two happened matters more than
     * pretending they are the same. */
    if ((sl->state & DRV_S_INITED) && d->ops && d->ops->shutdown &&
        d->ops->shutdown(d->ctx) == 0) {
        sl->state &= (uint8_t)~DRV_S_INITED;
        kprintf("drv: '%s' stopped and quarantined\n", name);
    } else {
        kprintf("drv: '%s' quarantined (could not be stopped)\n", name);
    }
    sl->state |= DRV_S_QUARANTINE;
}

int driver_start(const char* name) {
    struct driver* d = driver_find(name);
    if (!d) { kprintf("drv: no driver '%s'\n", name); return -1; }
    struct drv_slot* sl = slot_of(d);
    if (!sl) return -1;
    if (sl->state & DRV_S_INITED) {
        kprintf("drv: '%s' is already running\n", name);
        return -2;
    }
    if (d->ops && d->ops->probe && d->ops->probe(d->ctx) != 0) {
        kprintf("drv: '%s' — hardware not present\n", name);
        sl->state |= DRV_S_PROBE_FAIL;
        return -3;
    }
    sl->state |= DRV_S_PROBED;
    /* An explicit start is the user overruling the quarantine — that is what
     * makes quarantine a pause rather than a death sentence. */
    sl->state &= (uint8_t)~(DRV_S_PROBE_FAIL | DRV_S_INIT_FAIL |
                            DRV_S_QUARANTINE | DRV_S_ADMIN_DOWN);
    if (d->ops && d->ops->init && d->ops->init(d->ctx) != 0) {
        sl->state |= DRV_S_INIT_FAIL;
        kprintf("drv: '%s' failed to start\n", name);
        return -4;
    }
    sl->state |= DRV_S_INITED;
    kprintf("drv: started '%s'\n", name);
    return 0;
}

int driver_rescan(void) {
    int started = 0;
    for (uint32_t i = 0; i < driver_count; i++) {
        struct drv_slot* sl = &g_slots[i];
        struct driver* d = sl->d;
        if (sl->state & DRV_S_INITED) continue;
        /* A driver that FAILED to init is not retried here: re-running an
         * init that just failed, on a timer, is a loop that fills the log and
         * fixes nothing (§M29's crash-loop backoff, same reasoning).  It takes
         * an explicit `drv start`. */
        if (sl->state & DRV_S_INIT_FAIL) continue;
        if (sl->state & DRV_S_QUARANTINE) continue;   /* held back: faulted */
        if (sl->state & DRV_S_ADMIN_DOWN)  continue;   /* held back: by hand */
        if (d->ops && d->ops->probe && d->ops->probe(d->ctx) != 0) continue;
        if (d->ops && d->ops->init && d->ops->init(d->ctx) != 0) {
            sl->state |= DRV_S_INIT_FAIL;
            continue;
        }
        sl->state |= DRV_S_PROBED | DRV_S_INITED;
        sl->state &= (uint8_t)~DRV_S_PROBE_FAIL;
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
