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
#include "drvguard.h"    /* §M33 Tier 0 — guarded driver entry points */
#include "domain.h"       /* §M33 — declared placement */
#include "drvrt.h"        /* §M33 stage 2 — drv res */
#include "drvuser.h"      /* §M33 Tier 1 — placing a driver in ring 3 */
#include "task.h"         /* §M33 Tier 2 — killing/waiting on a placed driver */
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

/* ----------------------------------------------------------------------
 * §M33 Tier 0 — every driver entry point goes through the guard.
 *
 * Three one-line wrappers rather than each call site spelling out
 * `drvguard_call(d, "init", d->ops->init, d->ctx)`: there are nine call sites
 * in this file, and a guard applied to eight of them is a guard nobody can
 * rely on.  One place per hook, and the hook's own NULL check lives inside
 * drvguard_call.
 *
 * A FAULTED call is reported to the caller as a failure, which is what makes
 * this transparent: `driver_start` already handles "init failed", so it needs
 * no new case for "init died" — same path, different reason printed.
 * ---------------------------------------------------------------------- */
static int drv_probe(struct driver* d) {
    if (!d->ops || !d->ops->probe) return 0;      /* no probe = always present */
    int r = drvguard_call(d, "probe", d->ops->probe, d->ctx);
    return (r == DRVGUARD_FAULTED) ? -1 : r;
}

/* §M33 Tier 1 — WHERE THE PLACEMENT ACTUALLY HAPPENS.
 *
 * This is the whole of "config chooses where a driver runs": the resolved
 * domain is consulted once, and DOMAIN_USER means *do not call the driver's
 * init at all* — spawn its ring-3 image instead and let it bring the device up
 * from there.  Everything above and below this function is unchanged, which is
 * what makes the placement a deployment decision rather than a code one.
 *
 * The ring-3 launch REPLACES init rather than wrapping it, and that has to be
 * true: calling init here would bring the device up in the kernel and then
 * start a second driver for the same hardware in ring 3, which is two drivers
 * fighting over one 8042. */
static int drv_init(struct driver* d) {
    if (driver_domain(d) == DOMAIN_USER) {
        int rc = drvuser_launch(d);
        if (rc == 0) return 0;
        /* A failed ring-3 launch does NOT silently fall back to the kernel.
         * The user asked for a placement; running the driver somewhere else and
         * saying nothing would be the isolation theatre this milestone refuses,
         * just in the other direction. */
        kprintf("drv: '%s' could not be placed in ring 3 (%d) — not started\n",
                d->name, rc);
        return -1;
    }
    if (!d->ops || !d->ops->init) return 0;
    return drvguard_call(d, "init", d->ops->init, d->ctx);
}

/* Shutdown is guarded too, and it matters MORE than the others rather than
 * less: it runs on the power-off path and from `rmmod`, so a fault there would
 * take the machine down at exactly the moment the user asked it to stop
 * cleanly.  A faulted shutdown is reported as a REFUSAL (non-zero), which is
 * the safe reading — §M67 made a refusal mean "the driver is still live and its
 * registrations still stand", and after a fault that is precisely what we do
 * not know to be false. */
static int drv_shutdown(struct driver* d) {
    /* §M33 TIER 2 — STOP THE DRIVER THAT IS ACTUALLY RUNNING.
     *
     * A BUG TIER 1 SHIPPED WITH, found by writing the restart path: `drv_init`
     * routes by domain and this did not, so stopping a placed driver ran the
     * IN-KERNEL shutdown hook — code belonging to a driver that is not the one
     * on the hardware, touching ports the kernel no longer owns, while the
     * ring-3 process carried on driving the device.  It went unnoticed because
     * the two halves are the same source file and the hook is written to be
     * harmless when it has nothing to do.  A placement is only a placement if
     * EVERY lifecycle edge honours it, not just the one that establishes it.
     *
     * THE TEST IS THE LIVE PROCESS, NOT THE CONFIGURED DOMAIN, and the
     * difference is not pedantry — it is the entire window in which a placement
     * change is applied.  `driver.<name>.domain = user` states an INTENTION;
     * the driver keeps running wherever it already was until a restart carries
     * it over.  Asking config where the driver is would answer "ring 3" for a
     * driver still in the kernel and skip the shutdown that has to happen
     * before it can move — so the change would take effect by leaving the old
     * driver live, which is the two-drivers-one-8042 failure drv_init exists to
     * avoid, arrived at from the other side. */
    if (drvuser_pid(d->name) > 0)
        return drvuser_stop(d->name) == 0 ? 0 : -1;

    if (!d->ops || !d->ops->shutdown) return 0;
    int r = drvguard_call(d, "shutdown", d->ops->shutdown, d->ctx);
    return (r == DRVGUARD_FAULTED) ? -1 : r;
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
        if (drv_shutdown(sl->d) != 0) { refused++; continue; }
        sl->state &= (uint8_t)~DRV_S_INITED;
        stopped++;
    }
    if (refused) kprintf("drivers: %d stopped, %d still busy\n", stopped, refused);
    else         kprintf("drivers: %d stopped\n", stopped);
}

/* Walk the registry as it ACTUALLY is.  §M66 replaced the linker array with a
 * slot table and §M67 started adding entries the linker never placed; anything
 * still iterating `__start_drivers` is reporting a subset and calling it the
 * whole.  /proc/drivers was doing exactly that. */
int driver_count_all(void) { return (int)driver_count; }

struct driver* driver_at(int i) {
    if (i < 0 || (uint32_t)i >= driver_count) return NULL;
    return g_slots[i].d;
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
            if (drv_probe(d) != 0) {
                sl->state = DRV_S_PROBE_FAIL;
                absent++;
                continue;
            }
        }
        st |= DRV_S_PROBED;
        probed++;

        /* Init — NULL init means "no setup needed". */
        if (d->ops && d->ops->init) {
            int r = drv_init(d);
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
        kprintf("  [%s] %s v%s — %s%s",
                sl->d->class ? sl->d->class : "?", sl->d->name,
                sl->d->version ? sl->d->version : "?",
                state_label(sl->state),
                sl->dynamic ? "  (loaded)" : "");
        /* §M33 Tier 2 — where it is, for a driver that is not in the kernel.
         * Only printed when the answer is not the default: adding "at kernel"
         * to every row would make the one line that matters the hardest to see.
         *
         * The LIVE PROCESS is what is reported, and the pending case gets its
         * own words.  Config says where a driver SHOULD run; a restart is what
         * moves it, and between the two the honest answer is neither "ring 3"
         * nor silence — it is "still in the kernel, and here is why that is not
         * what you asked for". */
        int pid = drvuser_pid(sl->d->name);
        if (pid > 0)
            kprintf("  [ring 3, pid %d, %d restart(s), %d event(s)]",
                    pid, drvuser_restarts(sl->d->name),
                    drvuser_events(sl->d->name));
        else if (driver_domain(sl->d) == DOMAIN_USER)
            kprintf("  [in kernel — config asks for ring 3, restart to apply]");
        kprintf("\n");
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
    /* The hook requirement below is about a driver IN THE KERNEL.  A driver in
     * ring 3 is stopped by being killed and its grants are returned by the
     * kernel either way, so demanding a hook it does not need would refuse to
     * stop exactly the drivers that are safest to stop.  Keyed on the live
     * process for drv_shutdown's reason: what matters is where the driver IS. */
    if (drvuser_pid(d->name) <= 0 && (!d->ops || !d->ops->shutdown)) {
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
    if (drv_shutdown(d) != 0) {
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
        drv_shutdown(d) == 0) {
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
    /* QUARANTINE IS CHECKED BEFORE "already running", and the order is a bug
     * fix rather than a preference.
     *
     * §M66's `driver_fault` can only clear INITED if the driver HAS a shutdown
     * hook and that hook succeeds — for a driver with none (or one that
     * refuses) the bit stays set even though the driver has demonstrably
     * stopped working.  With the old order, `lsdrv` said QUARANTINED and
     * `drv start` said "already running": two answers to one question, and the
     * documented way to clear a quarantine did not work on exactly the drivers
     * most likely to need it.
     *
     * An explicit start on a quarantined driver therefore RE-INITIALISES it.
     * That is what the user asked for, and a driver that faulted is not in a
     * state where "it is already running" means anything. */
    if (sl->state & DRV_S_QUARANTINE) {
        kprintf("drv: '%s' was quarantined — clearing and re-initialising\n", name);
        sl->state &= (uint8_t)~DRV_S_INITED;
    } else if (sl->state & DRV_S_INITED) {
        kprintf("drv: '%s' is already running\n", name);
        return -2;
    }
    if (drv_probe(d) != 0) {
        kprintf("drv: '%s' — hardware not present\n", name);
        sl->state |= DRV_S_PROBE_FAIL;
        return -3;
    }
    sl->state |= DRV_S_PROBED;
    /* An explicit start is the user overruling the quarantine — that is what
     * makes quarantine a pause rather than a death sentence. */
    sl->state &= (uint8_t)~(DRV_S_PROBE_FAIL | DRV_S_INIT_FAIL |
                            DRV_S_QUARANTINE | DRV_S_ADMIN_DOWN);
    if (drv_init(d) != 0) {
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
        if (drv_probe(d) != 0) continue;
        if (drv_init(d) != 0) {
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


/* ----------------------------------------------------------------------
 * §M33 — where a driver is PLACED, and the honesty gate on asking.
 *
 * `driver_domain()` answers "where does this driver actually run", which today
 * is always DOMAIN_KERNEL — but it answers it by RESOLVING config against the
 * driver's declared set rather than by returning a constant, so the day Tier 1
 * lands nothing above this function changes.
 *
 * The refusal ladder, in order, because each rung produces a different message
 * and a user who hits one should not have to discover the next by trying again:
 *   1. not a domain name at all
 *   2. the driver did not DECLARE it (a property of the code — config cannot
 *      widen a capability)
 *   3. the driver is boot-critical (it exists before anywhere else exists)
 *   4. this machine cannot ENFORCE it (domain.h's gate)
 * ---------------------------------------------------------------------- */
uint32_t driver_domain(const struct driver* d) {
    if (!d) return DOMAIN_KERNEL;
    const char* v = NULL;
    /* `driver.<name>.domain`, looked up per driver.  A `driver.profile`
     * (desktop|server) that sets many at once is §M33 stage 4 and wants a
     * placement policy to be worth having; with one reachable domain it would
     * be a key with one legal value. */
    char key[64];
    int n = 0;
    const char* pfx = "driver.";
    for (int i = 0; pfx[i] && n < (int)sizeof key - 1; i++) key[n++] = pfx[i];
    for (int i = 0; d->name && d->name[i] && n < (int)sizeof key - 8; i++)
        key[n++] = d->name[i];
    const char* sfx = ".domain";
    for (int i = 0; sfx[i] && n < (int)sizeof key - 1; i++) key[n++] = sfx[i];
    key[n] = 0;

    v = config_get(key, NULL);
    if (!v) return DOMAIN_KERNEL;
    uint32_t want = domain_parse(v);
    if (!want) return DOMAIN_KERNEL;

    /* A stored value that is no longer legal falls back to KERNEL rather than
     * refusing to run the driver: the config may outlive a rebuild in which
     * the driver stopped declaring a domain, and "your sound card does not
     * come up because of a stale setting" is a bad way to find that out. */
    uint32_t declared = d->domains ? d->domains : DOMAIN_KERNEL;
    if (!(declared & want)) return DOMAIN_KERNEL;
    if (domain_enforceable(want, NULL) != 0) return DOMAIN_KERNEL;
    return want;
}

/* Ask for a placement.  Returns 0 if it was accepted and stored. */
int driver_set_domain(const char* name, const char* domain_str) {
    struct driver* d = driver_find(name);
    if (!d) { kprintf("drv: no driver '%s'\n", name); return -1; }

    uint32_t want = domain_parse(domain_str);
    if (!want) {
        kprintf("drv: '%s' is not a domain (kernel | user | isolated)\n",
                domain_str ? domain_str : "");
        return -2;
    }

    uint32_t declared = d->domains ? d->domains : DOMAIN_KERNEL;
    if (!(declared & want)) {
        char buf[40];
        domain_set_str(declared, buf, sizeof buf);
        kprintf("drv: '%s' does not declare domain '%s' — it declares %s\n",
                name, domain_name(want), buf);
        kprintf("     (a domain is a capability of the CODE; config chooses "
                "among what the driver says it can do, and cannot widen it)\n");
        return -3;
    }

    if (d->flags & DRVF_BOOT_CRITICAL) {
        kprintf("drv: '%s' is boot-critical — it comes up before there is "
                "anywhere else to put it\n", name);
        return -4;
    }

    const char* why = NULL;
    if (domain_enforceable(want, &why) != 0) {
        kprintf("drv: cannot place '%s' in domain '%s' — %s\n",
                name, domain_name(want), why ? why : "not available");
        kprintf("     REFUSED rather than accepted and quietly run in the "
                "kernel: a boundary you believe in and do not have is worse "
                "than one you know you lack\n");
        return -5;
    }

    char key[64];
    int n = 0;
    const char* pfx = "driver.";
    for (int i = 0; pfx[i]; i++) key[n++] = pfx[i];
    for (int i = 0; d->name[i] && n < (int)sizeof key - 8; i++) key[n++] = d->name[i];
    const char* sfx = ".domain";
    for (int i = 0; sfx[i]; i++) key[n++] = sfx[i];
    key[n] = 0;
    config_apply(key, domain_name(want));
    kprintf("drv: '%s' will run in domain '%s' — restart it to apply\n",
            name, domain_name(want));
    return 0;
}


/* The placement table.  Shows the DECLARED set next to the resolved one, so
 * "why can I not move this" is answerable from the same line as "where is it" —
 * and the isolation column says what a placement would actually deliver, which
 * for a DMA driver in ring 3 without an IOMMU is not what "user" sounds like. */
void driver_domains_list(void) {
    kprintf("driver placement (%u):\n", driver_count);
    for (uint32_t i = 0; i < driver_count; i++) {
        struct driver* d = g_slots[i].d;
        char decl[40];
        domain_set_str(d->domains ? d->domains : DOMAIN_KERNEL, decl, sizeof decl);
        uint32_t at = driver_domain(d);
        enum domain_isolation iso =
            domain_isolation_of(at, (d->flags & DRVF_DMA) ? 1 : 0,
                                drvuser_confined(d->name));
        kprintf("  %s: at %s, can be %s, isolation %s%s%s\n",
                d->name, domain_name(at), decl, domain_isolation_name(iso),
                (d->flags & DRVF_BOOT_CRITICAL) ? ", boot-critical" : "",
                (d->flags & DRVF_DMA) ? ", DMA" : "");
    }
    kprintf("faults contained so far: %u\n", drvguard_fault_count());

    /* §M33 stage 5 — WHY a DMA driver can only ever be advisory here, printed
     * ONCE at the bottom rather than on every row: it is a property of the
     * MACHINE, not of any driver, so repeating it per line would make it look
     * like a per-driver finding and bury the rows.
     *
     * It is printed at all because "ADVISORY(!)" says a placement would not
     * isolate and does not say whether that is a limit of this hardware or work
     * we have not finished — and somebody deciding whether to place a NIC in
     * ring 3 needs exactly that distinction. */
    const char* why = domain_isolation_reason(1);
    if (why) kprintf("a DMA driver can only be advisory here: %s\n", why);
}

/* ----------------------------------------------------------------------
 * `drv crash <name>` — make a driver fault ON PURPOSE, inside a guarded entry
 * point, so Tier 0's containment can be falsified rather than asserted.
 *
 * IT MUST FAULT FOR REAL, and getting that right needed §M62's lesson: writing
 * to a low address does NOT fault here, because low memory is identity-mapped
 * — the first version of that milestone's test "succeeded" silently and the
 * feature looked fine for the innocent reason that nothing had crashed.  So
 * the address is deliberately far outside anything this kernel maps.
 *
 * It runs through `drvguard_call` rather than faulting inline, because what is
 * being tested is the GUARD, not the fault: a fault outside a guarded call is
 * the old behaviour and would prove nothing about the new one.
 * ---------------------------------------------------------------------- */
static int crash_victim(void* ctx) {
    (void)ctx;
    /* Volatile so the compiler cannot decide this is undefined and elide it —
     * a test that gets optimised away passes by not running. */
    volatile unsigned* p = (volatile unsigned*)(uintptr_t)0xDEAD0000u;
    *p = 0x1234;                 /* unmapped on every arch here */
    return 0;                    /* never reached */
}

void driver_crash(const char* name) {
    while (name && *name == ' ') name++;
    if (!name || !*name) { kprintf("drv: crash <name>\n"); return; }
    struct driver* d = driver_find(name);
    if (!d) { kprintf("drv: no driver '%s'\n", name); return; }

    /* §M33 TIER 2 — ONE VERB, BOTH PLACEMENTS.
     *
     * "Make this driver fail and show me what happens" is the same question
     * wherever the driver runs, and the answers are what should differ: in
     * ring 0 the guard catches the fault and the call unwinds; in ring 3 the
     * process dies and the supervisor puts it back.  A second command for the
     * second case would let one of the two paths quietly stop being exercised —
     * §M31's argument that a safety net nobody has fallen into is one nobody
     * has tested, applied to the net's other half. */
    int pid = drvuser_pid(name);
    if (pid > 0) {
        int before = drvuser_restarts(name);
        kprintf("drv: killing '%s' (ring-3 pid %d) to see whether it comes back\n",
                name, pid);
        /* FORCE, not the cooperative kill.  `task_kill` would deliver DRV_ESTOP
         * and the driver would shut down tidily — which is `drv stop`, and
         * proves the wrong thing.  A crash test has to produce a driver that
         * stops without unwinding, because that is the failure the supervisor
         * exists for. */
        task_force_kill(pid);
        /* POLL for the new pid rather than sleeping a guessed interval.  The
         * force-kill lands at the victim's next timer preemption taken in user
         * mode, which for a driver blocked on a one-second wait can be most of
         * a second away — a fixed 800 ms wait reported "NOT recovered" for a
         * driver that came back perfectly well 200 ms later, and I believed it
         * once before checking. */
        /* Wait for a DIFFERENT, LIVE pid.  Both halves matter: the slot reads
         * back 0 while the supervisor is between releasing the old process and
         * spawning the replacement, and the first version treated that
         * transient as the final answer and reported "NOT recovered" about a
         * driver that came back a moment later. */
        int now = pid;
        for (int k = 0; k < 200; k++) {                              /* ~10 s */
            task_msleep(50);
            now = drvuser_pid(name);
            /* A DIFFERENT pid that has finished BRING-UP.  Waiting on the pid
             * alone reported "recovered" about a replacement that failed its
             * handshake moments later — the process existed and the device was
             * not being driven, which is the exact distinction this milestone
             * is about. */
            if (now > 0 && now != pid && drvuser_ready(name)) break;
        }
        if (now > 0 && now != pid && drvuser_ready(name))
            kprintf("drv: recovered — '%s' is pid %d now, device up "
                    "(%d restart(s))\n", name, now, drvuser_restarts(name));
        else if (drvuser_restarts(name) != before)
            kprintf("drv: '%s' restarted but is not running now\n", name);
        else
            kprintf("drv: NOT recovered — '%s' has no ring-3 process\n", name);
        return;
    }

    kprintf("drv: making '%s' fault on purpose (inside a guarded call)\n", name);
    int r = drvguard_call(d, "crash-test", crash_victim, d->ctx);
    if (r == DRVGUARD_FAULTED)
        kprintf("drv: contained — '%s' faulted and the call unwound; "
                "we are still here\n", name);
    else
        kprintf("drv: NOT CONTAINED — the call returned %d without faulting, "
                "so this proved nothing\n", r);
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
    if (str_eq(verb, "domain")) {
        char a[24]; n = 0;
        while (*args && *args != ' ' && n < (int)sizeof a - 1) a[n++] = *args++;
        a[n] = 0;
        while (*args == ' ') args++;
        if (!a[0]) { driver_domains_list(); return; }
        if (!*args) { kprintf("drv: domain <name> <kernel|user|isolated>\n"); return; }
        driver_set_domain(a, args);
        return;
    }
    /* §M33 Tier 0's own falsification.  `drv fault` REPORTS a fault somebody
     * else noticed; this one MAKES the driver actually fault, inside a guarded
     * entry point, which is the only way to find out whether the containment
     * works.  A safety net nobody has fallen into is a safety net nobody has
     * tested — §M31's argument for `hardlock`, one layer over. */
    if (str_eq(verb, "crash")) { driver_crash(args); return; }
    /* §M33 stage 2 — what each driver actually HOLDS.  Worth its own verb
     * because a resource conflict's message names the holder, and the first
     * question after reading one is "what else does it have". */
    if (str_eq(verb, "res"))   { drv_res_dump(); return; }

    kprintf("drv: list | rescan | stop <name> | start <name> | swap <from> <to>"
            " | fault <name> | domain [<name> <where>] | crash <name> | res\n");
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
