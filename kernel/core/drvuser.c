/* =============================================================================
 * drvuser.c — §M33 Tier 1: the kernel side of a RING-3 driver.
 *
 * drvrt.h's in-kernel backend (drvrt.c) is what a driver in ring 0 calls
 * directly.  This is the same interface reached across the boundary: a process
 * placed as a driver asks for its resources through syscalls, and what it gets
 * back is the same handles with the same numbers.
 *
 * -----------------------------------------------------------------------------
 * WHAT ACTUALLY MAKES THIS ISOLATION, AND WHAT DOES NOT
 *
 * Two things are real here and they are worth separating, because only one of
 * them is new:
 *
 *   * The driver is in ITS OWN ADDRESS SPACE (§M25).  It cannot touch kernel
 *     memory at all, and when it dies only it dies.  This part is not new
 *     machinery, it is machinery §M25 built being used for something new.
 *
 *   * The driver may touch ONLY THE PORTS IT WAS GRANTED, enforced by the CPU
 *     through the TSS I/O permission bitmap.  A port outside the grant is a
 *     #GP, not a check that could be forgotten — and a port above 0x3FF is
 *     denied by the segment limit, i.e. by construction.
 *
 * -----------------------------------------------------------------------------
 * THE MANIFEST, AND WHY A GRANT IS NOT "WHATEVER IT ASKS FOR"
 *
 * A driver process asking for its own resources would let it ask for ANY port
 * under 0x400 — the PIC's, the PIT's, the other keyboard's.  That is a smaller
 * hole than ring 0 and it is still a hole, and closing it later would mean
 * changing an interface drivers were already written against.
 *
 * So a request is checked against a MANIFEST declared in the kernel next to the
 * driver's placement, and a request outside it is refused with the reason.  The
 * driver still asks — the API is unchanged — but the answer is bounded by
 * something the driver did not write.
 *
 * -----------------------------------------------------------------------------
 * §M33 TIER 2 — WHY A PLACEMENT WITHOUT SUPERVISION IS NOT ONE
 *
 * Tier 1 answered "can this driver run in ring 3".  It could, and the machine
 * survived its death — which is the whole benefit — but the DEVICE did not come
 * back, because nothing restarted the process.  So the honest reading of Tier 1
 * alone is: *the mouse now stops working instead of the machine stopping*.  A
 * better failure, and still a failure.
 *
 * That is the asymmetry this half closes, and it is the reason §M33's plan calls
 * reconnection the pervasive part rather than a finishing touch: in ring 0 a
 * driver that faults has already damaged whatever it was going to damage, so
 * recovery is a policy question.  In ring 3 the damage is bounded BY
 * CONSTRUCTION, which makes restarting it the obvious thing to do — and the
 * thing every client then depends on.
 *
 * Three things have to be true before a restart is more than a re-spawn:
 *
 *   1. **THE GRANTS MUST GO BACK FIRST.**  drvrt.c refuses a second claim on
 *      ports somebody already holds — correctly, by name.  A restart that
 *      re-spawned without releasing would be refused by our own conflict
 *      detector, and the driver would come up unable to reach its hardware.
 *
 *   2. **THE CLIENT MUST BE PUT BACK TO A NEUTRAL STATE.**  A driver that dies
 *      mid-drag has told the input stack a button is DOWN and will never say it
 *      came up.  The restarted driver reports movement from then on, so the
 *      pointer works and every motion is a drag — a desktop that is subtly,
 *      permanently wrong in a way nothing on screen explains.  A lost driver's
 *      last act, performed for it, is to release what it was holding.
 *
 *   3. **A CRASH LOOP MUST STOP.**  Restarting forever turns one broken driver
 *      into a machine that spends its life spawning it.  §M66's quarantine
 *      already means "nothing restarts this automatically", so the policy has a
 *      home: N failures inside a window and the driver stays down until a person
 *      says otherwise.
 * ============================================================================= */

#include "drvuser.h"
#include "drvrt.h"
#include "task.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include "hal_api.h"
#include "mouse.h"
#include "proc.h"
#include "driver.h"
#include "config.h"
#include "settings.h"   /* CONFIG_KEY — the restart policy is a setting */
#include "timer.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "iommu.h"
#include <stddef.h>

/* ----------------------------------------------------------------------
 * The manifest.  One row per driver that may be placed in ring 3.
 *
 * IT IS A TABLE IN THE KERNEL AND NOT A FIELD ON THE DRIVER, deliberately: the
 * whole point is that the bound comes from somewhere the driver's own code
 * cannot set.  A driver that is not listed cannot be placed in DOMAIN_USER at
 * all, which is why the list and `domain_enforceable` agree by construction
 * rather than by being kept in step.
 * ---------------------------------------------------------------------- */
struct drv_manifest {
    const char* name;
    uint16_t    port_base, port_count;
    int         irq;                 /* -1 = none */
    /* §M33 — the PCI device this driver may speak for, 0 for "not a PCI
     * driver".  The kernel resolves the BAR from it, which is what bounds an
     * MMIO grant: a placed driver names an offset within ITS OWN window and
     * cannot name somebody else's, because it never sees a physical address it
     * did not receive from here. */
    uint16_t    pci_vendor, pci_device;
    int         dma_bytes;           /* how much DMA it may be given, 0 = none */
    /* §M33 Tier 2 — what to tell this driver's CLIENTS when it dies.  See
     * quiesce_pointer below: a driver that vanishes mid-gesture has left state
     * latched in a client that will never hear the end of it, and only the
     * class knows what "nothing is happening" looks like. */
    void      (*quiesce)(void);
};

static void quiesce_pointer(void);

static const struct drv_manifest g_manifest[] = {
    /* The 8042 aux device: four registers at 0x60 and IRQ 12.  Exactly what
     * ps2_mouse.c asks the in-kernel backend for — the same numbers, now
     * written down where the driver cannot reach them. */
    { "ps2_mouse", 0x60, 5, 12, 0, 0, 0, quiesce_pointer },
    /* The educational device: no ports at all, which is why it is the first
     * driver aarch64 can place.  Its window comes from its BAR and its DMA
     * ceiling is declared here rather than asked for, for the manifest's own
     * reason — the bound must come from somewhere the driver cannot set. */
    { "edu", 0, 0, -1, 0x1234, 0x11E8, 4096, NULL },
};
#define MANIFEST_N ((int)(sizeof g_manifest / sizeof g_manifest[0]))

const struct drv_manifest* drvuser_manifest(const char* name) {
    for (int i = 0; i < MANIFEST_N; i++) {
        const char* a = g_manifest[i].name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return &g_manifest[i];
    }
    return NULL;
}

int drvuser_placeable(const char* name) { return drvuser_manifest(name) != NULL; }

/* ----------------------------------------------------------------------
 * Per-process driver state.  Keyed by pid rather than embedded in `struct
 * task`: a driver process is a handful on a big machine, and §M53 already
 * recorded why an embedded object with its own lifetime is a teardown hazard
 * (an interval timer cancelled at the wrong point fires into freed memory).
 * ---------------------------------------------------------------------- */
#define DRVUSER_MAX 4

struct drvuser {
    int          used;
    int          pid;
    const struct drv_manifest* mf;
    struct drv_rt rt;
    uint8_t*     bitmap;          /* the task's port permission */
    drv_handle   h_ports, h_irq;

    /* §M33 Tier 2 — supervision.  `drv` is what a restart re-spawns, so it is
     * recorded at the first launch rather than looked up later: a driver that
     * has been detached in the meantime must not be brought back by us. */
    const struct driver* drv;
    /* §M33 — the device this driver was given, and what it was given of it. */
    uint16_t     bdf;
    uint64_t     win_phys, win_len;
    uintptr_t    h_mmio_va;
    uint64_t     dma_phys;        /* recorded for the IOMMU release; the FRAMES
                                   * belong to the resource table */
    int          supervise;       /* 0 = it was stopped on purpose */
    int          restarts;        /* inside the current window */
    uint64_t     window_ns;       /* when the current window opened */
    /* Events this PROCESS has published.  Reset by a restart on purpose: what
     * proves a recovery is traffic through the NEW process, and a running total
     * would be satisfied by the dead one's. */
    uint32_t     events;
};

static struct drvuser g_du[DRVUSER_MAX];

static struct drvuser* du_of_pid(int pid) {
    if (pid <= 0) return NULL;          /* 0 = reserved, not yet spawned */
    for (int i = 0; i < DRVUSER_MAX; i++)
        if (g_du[i].used && g_du[i].pid == pid) return &g_du[i];
    return NULL;
}

static int du_name_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Which slot belongs to the calling task.
 *
 * A SLOT IS RESERVED BEFORE THE SPAWN and carries pid 0 until its process
 * exists — see drvuser_launch for why (two callers could otherwise both decide
 * to place the same driver).  That leaves a gap the launcher cannot close by
 * ordering alone: on SMP the child can be picked up by another CPU and reach
 * its first syscall before the launcher has written the pid down.
 *
 * So a task whose pid matches nothing may still CLAIM a reserved slot bearing
 * its own name.  This is not a loose match — a name is unique among reserved
 * slots by construction, because a second reservation for the same driver is
 * exactly what the launcher refuses. */
static struct drvuser* du_current(void) {
    struct task* t = task_current();
    if (!t) return NULL;
    struct drvuser* d = du_of_pid(t->pid);
    if (d) return d;
    for (int i = 0; i < DRVUSER_MAX; i++) {
        if (!g_du[i].used || g_du[i].pid != 0) continue;
        if (!du_name_eq(g_du[i].mf ? g_du[i].mf->name : NULL, t->name)) continue;
        g_du[i].pid = t->pid;
        return &g_du[i];
    }
    return NULL;
}

/* Hand back everything the kernel granted this process, WITHOUT giving up the
 * slot.  Split out of drvuser_detach because a restart needs precisely this and
 * must keep the slot: the restart counters live in it, and a driver that lost
 * its counters on every restart could never be observed to be in a crash loop —
 * the one thing the counters exist to notice.
 *
 * Resources first, bitmap second.  The other order would leave a task briefly
 * permitted to touch ports whose grant has already gone back. */
static void du_release(struct drvuser* d) {
    /* THE DEVICE'S DOMAIN GOES BACK FIRST, before `drv_release_all` returns the
     * frames to the allocator: the other order hands memory out again while the
     * unit still lets this device write to it.
     *
     * Nothing was doing this, and it is the kind of leak only a RESTART LOOP
     * makes visible.  `iommu_confine` ACCUMULATES by design — a driver
     * allocates its ring, then its data, then more later — so a placed driver
     * that died and came back got a fresh buffer and a fresh grant every time
     * while the old grants stayed.  After N restarts its device could reach N
     * buffers and `drv domain` still said "isolation full".  *A boundary that
     * widens quietly every time a driver crashes is worse than no boundary,
     * because the reports keep agreeing with the intention.*
     *
     * The FRAMES are freed by drv_release_all, on the same path as every other
     * resource — see drv_res_note_dma for why that is one owner rather than a
     * private copy of the address and the count kept here. */
    if (d->dma_phys) { iommu_release(d->bdf); d->dma_phys = 0; }
    drv_release_all(&d->rt);
    if (d->bitmap) {
        /* Before the free, not after: the cache in tss.c keys on the ADDRESS,
         * and the next allocation gets this one back. */
        hal_io_bitmap_forget(d->bitmap);
        kfree(d->bitmap);
        d->bitmap = NULL;
    }
    d->h_ports = d->h_irq = -1;
}

/* Arm a FRESH slot.  Only `du_reserve` calls this, and that matters: it resets
 * the resource list, so calling it on a slot whose process is already running
 * orphans everything that process has claimed.  The restart path deliberately
 * does not — see the note where it records the pid. */
static void du_arm(struct drvuser* d, int pid, const struct drv_manifest* mf) {
    d->used    = 1;
    d->pid     = pid;
    d->mf      = mf;
    d->h_ports = d->h_irq = -1;
    d->bitmap  = NULL;
    d->events  = 0;
    drv_rt_init(&d->rt, mf->name);
}

/* RESERVE a slot for this driver, before anything is spawned.  Returns the
 * slot index, or <0 if the driver already has one or the table is full.
 *
 * Reserving first is what makes "one placement per driver" an invariant rather
 * than a check with a window in it: `drv start` and §M66's rescan can both
 * decide a driver needs starting, and the observed result was two processes
 * fighting over the same 8042 until the restart budget ran out and the driver
 * quarantined itself.  A guard that ran after the spawn — which is where the
 * first version put it — is a guard both callers walk straight past. */
static int du_reserve(const char* driver_name) {
    const struct drv_manifest* mf = drvuser_manifest(driver_name);
    if (!mf) return -1;
    for (int i = 0; i < DRVUSER_MAX; i++)
        if (g_du[i].used && g_du[i].mf == mf) return -2;    /* already placed */
    for (int i = 0; i < DRVUSER_MAX; i++) {
        if (g_du[i].used) continue;
        struct drvuser* d = &g_du[i];
        du_arm(d, 0, mf);            /* pid 0 = reserved, process not yet up */
        d->drv       = NULL;
        d->supervise = 0;
        d->restarts  = 0;
        d->window_ns = 0;
        return i;
    }
    return -3;
}

int drvuser_attach(int pid, const char* driver_name) {
    int i = du_reserve(driver_name);
    if (i < 0) return i;
    g_du[i].pid = pid;
    return 0;
}

void drvuser_detach(int pid) {
    struct drvuser* d = du_of_pid(pid);
    if (!d) return;
    du_release(d);
    d->used = 0;
}

/* ----------------------------------------------------------------------
 * Launching a driver into ring 3.
 *
 * The image is embedded like every other in-tree program (§M25's pattern), and
 * the process is a REAL preemptible task rather than a synchronous excursion —
 * a driver that ran as an excursion on its launcher would take the launcher
 * down with it, which is the opposite of the point.
 *
 * ATTACH BEFORE SPAWN, and the order is load-bearing: the driver's very first
 * instructions ask for its ports, and a process that is not yet attached holds
 * no manifest and would be refused.  The pid is known before the task runs
 * because proc_spawn returns it.
 * ---------------------------------------------------------------------- */
extern const unsigned char _binary_user_ps2mouse_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_ps2mouse_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_edudrv_elf_start[]   __attribute__((weak));
extern const unsigned char _binary_user_edudrv_elf_end[]     __attribute__((weak));

struct drv_image { const char* name; const unsigned char** start; const unsigned char** end; };

static const unsigned char* ps2_s(void) { return _binary_user_ps2mouse_elf_start; }
static const unsigned char* ps2_e(void) { return _binary_user_ps2mouse_elf_end; }
static const unsigned char* edu_s(void) { return _binary_user_edudrv_elf_start; }
static const unsigned char* edu_e(void) { return _binary_user_edudrv_elf_end; }

/* Which image is this driver's?  A table keyed by name rather than a field on
 * `struct driver`, for the manifest's own reason: what may be placed in ring 3,
 * and which image is placed there, is the kernel's decision. */
static int du_image(const char* name, const unsigned char** img,
                    const unsigned char** end) {
    if (du_name_eq(name, "ps2_mouse")) { *img = ps2_s(); *end = ps2_e(); }
    else if (du_name_eq(name, "edu"))  { *img = edu_s(); *end = edu_e(); }
    else return -1;
    return *img ? 0 : -1;                   /* not embedded in this build */
}

static int du_spawn(const struct drv_manifest* mf, const unsigned char* img,
                    const unsigned char* end) {
    return proc_spawn_argv(mf->name, img, (size_t)(end - img), 0, NULL, 0);
}

int drvuser_launch(const struct driver* d) {
    if (!d || !d->name) return -1;
    const struct drv_manifest* mf = drvuser_manifest(d->name);
    if (!mf) return -2;                     /* no manifest = not placeable */

    const unsigned char* img = NULL; const unsigned char* end = NULL;
    if (du_image(d->name, &img, &end) != 0) return -3;

    /* CLAIM THE SLOT FIRST.  See du_reserve: one placement per driver has to be
     * decided before a process exists, or two callers each spawn one. */
    int slot = du_reserve(d->name);
    if (slot == -2) {
        kprintf("drv: '%s' is already placed in ring 3 as pid %d\n",
                d->name, drvuser_pid(d->name));
        return 0;                           /* already where it was asked to be */
    }
    if (slot < 0) return -5;

    struct drvuser* du = &g_du[slot];
    du->drv       = d;
    du->supervise = 1;
    du->window_ns = timer_now_ns();

    int pid = du_spawn(mf, img, end);
    if (pid < 0) { du->used = 0; return -4; }
    /* The child may already have claimed the slot by name (du_current), in
     * which case this writes the same number it already holds. */
    du->pid = pid;
    drvuser_supervisor_start();

    kprintf("drv: '%s' placed in ring 3 as pid %d\n", d->name, pid);
    klog(KLOG_INFO, "drv", "'%s' running in DOMAIN_USER, pid %d\n", d->name, pid);
    return 0;
}

/* ----------------------------------------------------------------------
 * §M33 Tier 2 — SUPERVISION.
 *
 * A driver process can leave in three ways and the supervisor deliberately does
 * not distinguish them: it faulted, it exited, or somebody killed it.  From
 * outside they are one fact — *the device has no driver* — and treating them
 * differently would mean guessing at intent from an exit code, which is exactly
 * the sort of inference that is right until it is not.
 *
 * Restart policy is READ FROM CONFIG rather than compiled in, for the reason
 * §M63 exists: whether a machine would rather have a mouse that keeps coming
 * back or a driver that stays down for inspection is a deployment decision, and
 * a developer chasing a crash loop wants the second.
 * ---------------------------------------------------------------------- */
CONFIG_KEY(ck_drv_restart) = {
    .key = "driver.restart_max", .group = "System", .type = CFG_INT,
    .min = 0, .max = 20, .def = "3",
    .help = "restarts of a ring-3 driver inside 30 s before it is quarantined; "
            "0 = never restart",
};

#define DRVUSER_WINDOW_NS   (30ull * 1000000000ull)
#define DRVUSER_POLL_MS     100
#define DRVUSER_BACKOFF_MS  200

static int cfg_restart_max(void) {
    const char* v = config_get("driver.restart_max", "3");
    int n = 0;
    for (; *v >= '0' && *v <= '9'; v++) n = n * 10 + (*v - '0');
    return n;
}

/* The client's last act, performed for a driver that did not get to perform it.
 *
 * ONE ROW TODAY AND STILL A MANIFEST FIELD rather than a call from the
 * supervisor: what "neutral" means belongs to the device CLASS, and the second
 * driver placed in ring 3 would otherwise put its own quiesce inside a loop
 * that has no business knowing about either of them. */
static void quiesce_pointer(void) {
    /* No motion, NO BUTTONS.  §M64's drag machinery latches a press and ends it
     * on the release, so a driver that died mid-drag leaves the desktop holding
     * a button forever — the pointer then works and every movement is a drag. */
    mouse_publish(0, 0, 0, 0);
}

/* Is this pid still a live process?  §M57's rule: poll for DISAPPEARANCE and
 * never `task_wait` — init is a universal reaper and may collect the task
 * first, after which a wait never completes.  A task that is DEAD but not yet
 * reaped counts as gone: it will never run its driver again. */
static int du_alive(int pid) {
    struct task* t = task_find(pid);
    return t && t->state != TASK_DEAD;
}

static void du_restart(struct drvuser* d) {
    const struct driver* drv = d->drv;
    const struct drv_manifest* mf = d->mf;
    int old = d->pid;

    /* 1. Hand the grants back.  drvrt.c would refuse the new process's claim on
     *    ports this slot still holds — and it would be right to. */
    du_release(d);
    /* The slot stays OURS (that is what keeps the restart counters and keeps a
     * second placement out) but it goes back to "reserved, no process": the
     * replacement may reach its first syscall before we record its pid, and
     * du_current can only let it claim a slot whose pid is 0.
     *
     * `events` is zeroed HERE, before anything is spawned, and not after —
     * what proves a recovery is traffic through the NEW process, and zeroing
     * it once the replacement is running would throw away exactly that. */
    d->pid    = 0;
    d->events = 0;

    /* 2. Put the client back where a clean shutdown would have left it. */
    if (mf->quiesce) mf->quiesce();

    int max = cfg_restart_max();
    uint64_t now = timer_now_ns();
    if (now - d->window_ns > DRVUSER_WINDOW_NS) {   /* a quiet spell resets it */
        d->window_ns = now;
        d->restarts  = 0;
    }

    if (max == 0 || d->restarts >= max) {
        /* 3. QUARANTINE.  §M66 already owns the meaning of the word — nothing
         *    automatic starts a quarantined driver — so the policy lands where
         *    `lsdrv`, `/proc/drivers` and `drv start` already read it, instead
         *    of in a second notion of "given up on" that only this file knows. */
        d->used = 0;
        kprintf("drv: '%s' (pid %d) died and will NOT be restarted "
                "(%d restart(s) in 30 s, limit %d)\n",
                mf->name, old, d->restarts, max);
        driver_fault(mf->name, "its ring-3 process kept dying");
        return;
    }

    const unsigned char* img = NULL; const unsigned char* end = NULL;
    if (du_image(mf->name, &img, &end) != 0) { d->used = 0; return; }

    /* A short pause before re-spawning.  Not to be gentle: a driver that dies
     * during its own bring-up would otherwise spin through the whole restart
     * budget faster than the window can see, and the quarantine would trigger
     * on a burst too short for anyone to read the log of. */
    task_msleep(DRVUSER_BACKOFF_MS);

    int pid = du_spawn(mf, img, end);
    if (pid < 0) {
        d->used = 0;
        kprintf("drv: '%s' died and could not be re-spawned\n", mf->name);
        driver_fault(mf->name, "its ring-3 process could not be re-spawned");
        return;
    }

    /* RECORD THE PID AND NOTHING ELSE — the same thing drvuser_launch does after
     * its spawn, and for the same reason.
     *
     * This used to call the full `du_arm`, which re-runs `drv_rt_init` and so
     * RESETS the resource list.  The replacement reaches its first syscall
     * before this line — that is the whole point of leaving the slot reserved
     * with pid 0 — so by the time it ran, the new process had already claimed
     * its ports, mapped its window and taken its DMA buffer, and all of it was
     * orphaned: still held in the global table, no longer reachable from the
     * slot, never freed.
     *
     * It became visible rather than merely wasteful once `du_release` learned
     * to give the IOMMU domain back: a defensive "release anything left over"
     * check fired here and tore down the domain the REPLACEMENT had just built,
     * putting the device back in the identity domain seconds after it came up.
     * The symptom was `edutest: FAIL — 8 of 256 bytes came back wrong` from a
     * driver whose own bring-up had reported success.
     *
     * *A defensive check whose premise is false is worse than no check* — the
     * premise being that nothing happens between the release and this line. */
    d->pid       = pid;
    d->drv       = drv;
    d->supervise = 1;
    d->restarts  = d->restarts + 1;

    kprintf("drv: '%s' died (pid %d) — restarted in ring 3 as pid %d "
            "(restart %d of %d)\n", mf->name, old, pid, d->restarts, max);
    klog(KLOG_WARN, "drv", "'%s' restarted in DOMAIN_USER: pid %d -> %d\n",
         mf->name, old, pid);
}

static void drvuser_supervisor(void) {
    for (;;) {
        for (int i = 0; i < DRVUSER_MAX; i++) {
            struct drvuser* d = &g_du[i];
            if (!d->used || !d->supervise) continue;
            if (du_alive(d->pid)) continue;
            du_restart(d);
        }
        task_msleep(DRVUSER_POLL_MS);
    }
}

/* Started on the first placement and not before, §M55's shape: a machine with
 * every driver in the kernel — which is every machine by default — does not
 * carry a task whose whole job is to watch an empty table. */
static int g_sup_started;

void drvuser_supervisor_start(void) {
    if (g_sup_started) return;
    g_sup_started = 1;
    /* DETACHED, so it is parented to init rather than to whoever happened to
     * place the first driver: the supervisor must outlive the shell command
     * that triggered the placement. */
    task_spawn_detached("drv-sup", drvuser_supervisor);
}

/* An admin stop.  The supervise flag is what tells a deliberate stop from a
 * death, and it is cleared BEFORE the kill — the other order leaves a window in
 * which the supervisor sees a dead pid it is still watching and helpfully
 * restarts the driver the user just asked to stop. */
int drvuser_stop(const char* name) {
    for (int i = 0; i < DRVUSER_MAX; i++) {
        struct drvuser* d = &g_du[i];
        if (!d->used || !d->mf) continue;
        const char* a = d->mf->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a != *b) continue;

        d->supervise = 0;
        int pid = d->pid;

        /* COOPERATIVE FIRST.  `task_kill` wakes the driver out of drv_irq_wait,
         * which answers DRV_ESTOP, and the driver's own loop exits — so its
         * shutdown path runs and the device is left disabled rather than merely
         * abandoned.  That is the difference between stopping a driver and
         * killing one, and it is worth having even though the kernel would
         * reclaim the resources either way. */
        task_kill(pid);

        /* THEN THE BACKSTOP.  A driver that ignores the request must still go —
         * otherwise "stop" means "ask nicely", and the one driver that needs
         * stopping is the one least likely to cooperate.  Polled for
         * DISAPPEARANCE (§M57: init may reap it first, so a wait would never
         * complete). */
        int gone = 0;
        for (int k = 0; k < 20 && !gone; k++) {      /* ~400 ms */
            if (!du_alive(pid)) { gone = 1; break; }
            task_msleep(20);
        }
        if (!gone) {
            kprintf("drv: '%s' did not stop when asked — forcing\n", name);
            task_force_kill(pid);
        }

        if (d->mf->quiesce) d->mf->quiesce();
        du_release(d);
        d->used = 0;
        kprintf("drv: stopped '%s' (ring-3 pid %d)%s\n", name, pid,
                gone ? "" : " (forced)");
        return 0;
    }
    return -1;
}

int drvuser_pid(const char* name) {
    for (int i = 0; i < DRVUSER_MAX; i++) {
        struct drvuser* d = &g_du[i];
        if (!d->used || !d->mf) continue;
        const char* a = d->mf->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return d->pid;
    }
    return -1;
}

/* Has the placed driver actually COME UP — not merely been spawned?
 *
 * A new pid proves a process exists; it does not prove the device is being
 * driven.  §M33 Tier 2's own recovery report claimed success on the pid alone
 * and said "recovered" about a process that failed its 8042 handshake half a
 * second later.  Holding BOTH grants is the earliest point at which the driver
 * has finished bring-up, so it is what "up" means here. */
int drvuser_ready(const char* name) {
    for (int i = 0; i < DRVUSER_MAX; i++) {
        struct drvuser* d = &g_du[i];
        if (!d->used || !d->mf) continue;
        const char* a = d->mf->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return (d->pid > 0 && d->h_ports >= 0 && d->h_irq >= 0);
    }
    return 0;
}

/* Is this placed driver's DEVICE confined to a domain of its own?  What the
 * isolation verdict is finally allowed to consult — and only for a driver that
 * is actually running in ring 3, because a confined device driven from the
 * kernel isolates nothing about the driver. */
int drvuser_confined(const char* name) {
    for (int i = 0; i < DRVUSER_MAX; i++) {
        struct drvuser* d = &g_du[i];
        if (!d->used || !d->mf) continue;
        if (!du_name_eq(d->mf->name, name)) continue;
        return d->bdf && iommu_is_confined(d->bdf);
    }
    return 0;
}

int drvuser_events(const char* name) {
    for (int i = 0; i < DRVUSER_MAX; i++) {
        struct drvuser* d = &g_du[i];
        if (!d->used || !d->mf) continue;
        const char* a = d->mf->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return (int)d->events;
    }
    return -1;
}

int drvuser_restarts(const char* name) {
    for (int i = 0; i < DRVUSER_MAX; i++) {
        struct drvuser* d = &g_du[i];
        if (!d->used || !d->mf) continue;
        const char* a = d->mf->name; const char* b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return d->restarts;
    }
    return 0;
}

/* ----------------------------------------------------------------------
 * The syscalls.
 * ---------------------------------------------------------------------- */

long drvuser_sys_ports(uint16_t base, uint16_t count) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;               /* not a driver process */

    /* THE MANIFEST CHECK.  A request must be inside what the kernel declared
     * for this driver — not merely inside the bitmap's reach. */
    if (base < d->mf->port_base ||
        (uint32_t)base + count > (uint32_t)d->mf->port_base + d->mf->port_count) {
        kprintf("drv-user: '%s' asked for ports %x..%x, manifest allows %x..%x\n",
                d->mf->name, base, base + count - 1, d->mf->port_base,
                d->mf->port_base + d->mf->port_count - 1);
        return DRV_EBUSY;
    }

    drv_handle h = drv_ports_request(&d->rt, base, count, "ring-3 driver");
    if (h < 0) return h;

    /* Now the part that makes it real: permit exactly these ports in the
     * task's I/O bitmap.  A 0 bit is permitted; everything else stays 1. */
    uint32_t nbytes = hal_io_bitmap_bytes();
    if (!nbytes) return DRV_ENOSYS;        /* no port space on this arch */
    if (!d->bitmap) {
        d->bitmap = (uint8_t*)kmalloc(nbytes);
        if (!d->bitmap) return DRV_ENORES;
        for (uint32_t i = 0; i < nbytes; i++) d->bitmap[i] = 0xFF;
    }
    for (uint32_t p = base; p < (uint32_t)base + count; p++) {
        if (p / 8 >= nbytes) break;        /* beyond the bitmap = denied */
        d->bitmap[p / 8] &= (uint8_t)~(1u << (p % 8));
    }

    struct task* t = task_current();
    if (t) {
        t->io_bitmap = d->bitmap;
        /* Install it NOW as well as at the next switch: the driver's very next
         * instruction may be an `in`, and waiting for a context switch would
         * make the grant's arrival depend on scheduling. */
        hal_set_io_bitmap(d->bitmap);
    }
    kprintf("drv-user: '%s' granted ports %x..%x in ring 3\n",
            d->mf->name, base, base + count - 1);
    d->h_ports = h;
    return h;
}

long drvuser_sys_irq(int line) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    if (line != d->mf->irq) {
        kprintf("drv-user: '%s' asked for IRQ %d, manifest allows %d\n",
                d->mf->name, line, d->mf->irq);
        return DRV_EBUSY;
    }
    drv_handle h = drv_irq_request(&d->rt, line, "ring-3 driver");
    if (h < 0) return h;
    d->h_irq = h;
    kprintf("drv-user: '%s' granted IRQ %d in ring 3\n", d->mf->name, line);
    return h;
}

/* Exclusive access to the shared controller, on the driver's own port handle.
 *
 * The handle check is the whole security of it: without it any process could
 * name a handle and have the kernel mask an interrupt line on its behalf.  A
 * driver may only ask about the window it was granted — which is the same rule
 * the manifest states for the grant itself. */
long drvuser_sys_ports_lock(drv_handle h, int max_ms) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    if (h != d->h_ports) return DRV_EBAD;
    return drv_ports_lock(h, max_ms);
}

long drvuser_sys_ports_unlock(drv_handle h) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    if (h != d->h_ports) return DRV_EBAD;
    return drv_ports_unlock(h);
}

long drvuser_sys_irq_wait(drv_handle h, int timeout_ms) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    /* A process may only wait on ITS OWN handle.  The handle space is global
     * in drvrt.c, so without this a driver could wait on another's interrupt —
     * harmless in itself and exactly the kind of thing that stops being
     * harmless once there are two drivers. */
    if (h != d->h_irq) return DRV_EBAD;
    return drv_irq_wait(h, timeout_ms);
}

/* Publishing an input event.  This is the "reach your clients" half of the
 * boundary, in the smallest form that serves the first driver: the kernel's
 * input layer is the client, and a mouse event is four integers.
 *
 * A GENERAL IPC PATH IS NOT THIS, and pretending otherwise would be the
 * mistake §M29's contracts exist to prevent.  A driver whose clients are not
 * the kernel wants the bus, and the bus is where that belongs. */
long drvuser_sys_input(int dx, int dy, unsigned buttons, int dz) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    d->events++;
    mouse_publish(dx, dy, buttons, dz);
    return 0;
}

/* A placed driver's own message.
 *
 * WHY THIS IS A SYSCALL AND NOT A write(1, ...).  A driver process is spawned
 * by the registry, not by a shell, so it has no console bound and its writes go
 * nowhere.  The first placement failure of this milestone therefore reported
 * ABSOLUTELY NOTHING — the driver exited 1 in its 8042 bring-up and the only
 * evidence anywhere was a kernel task's CPU-time column.
 *
 * ATTRIBUTED BY THE KERNEL, not by the driver: the name comes from the slot, so
 * a message cannot claim to be from a driver other than the one that sent it,
 * and the ring-0 and ring-3 forms of the same driver produce lines that can be
 * compared directly. */
long drvuser_sys_log(const char* msg) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    if (!msg) return -1;
    kprintf("drv-user: %s: %s\n", d->mf->name, msg);
    klog(KLOG_INFO, "drv-user", "%s: %s\n", d->mf->name, msg);
    return 0;
}

/* =============================================================================
 * §M33 — GIVING A PLACED DRIVER ITS DEVICE.
 *
 * A driver in ring 3 cannot read PCI config space and must not be able to: config
 * space reaches every device on the machine, so a placed driver holding it would
 * have MORE reach than the kernel driver it replaced.  So the kernel resolves the
 * device, performs the privileged half of bring-up, and hands back only what this
 * driver's manifest allows.
 *
 * EVERY ADDRESS THE DRIVER EVER SEES CAME FROM HERE, which is what makes the
 * bound real rather than checked: it cannot name a window it was not given,
 * because it has no way to learn one.
 * ============================================================================= */

/* Where is this driver's device, and turn it on.  Returns the BAR's physical
 * base and length into a small user struct. */
long drvuser_sys_window(int bar, uint64_t* out_phys, uint64_t* out_len) {
    struct drvuser* d = du_current();
    if (!d || !d->mf->pci_vendor) return DRV_EBAD;
    if (bar < 0 || bar > 5) return DRV_EBAD;

    struct pci_device pd;
    if (pci_find_device(d->mf->pci_vendor, d->mf->pci_device, &pd) != 0)
        return DRV_ENOSYS;

    uint16_t cmd = pci_read16(pd.bus, pd.slot, pd.func, PCI_COMMAND);
    pci_write16(pd.bus, pd.slot, pd.func, PCI_COMMAND,
                (uint16_t)(cmd | 0x0002 | 0x0004));   /* memory space + master */

    uint32_t off = (uint32_t)(PCI_BAR0 + bar * 4);
    uint32_t v = pci_read32(pd.bus, pd.slot, pd.func, (uint8_t)off);
    if (v & 1) return DRV_ENOSYS;                     /* I/O BAR, not MMIO */
    pci_write32(pd.bus, pd.slot, pd.func, (uint8_t)off, 0xFFFFFFFFu);
    uint32_t probe = pci_read32(pd.bus, pd.slot, pd.func, (uint8_t)off);
    pci_write32(pd.bus, pd.slot, pd.func, (uint8_t)off, v);
    uint32_t size = ~(probe & ~0xFu) + 1;

    d->win_phys = (uint64_t)(v & ~0xFu);
    d->win_len  = size ? (uint64_t)size : 0x1000ull;
    d->bdf      = (uint16_t)((pd.bus << 8) | (pd.slot << 3) | pd.func);
    if (out_phys) *out_phys = d->win_phys;
    if (out_len)  *out_len  = d->win_len;
    return 0;
}

/* Map that window into the driver's OWN address space.
 *
 * UNCACHED, and that is not a detail: a device register mapped cacheable reads
 * back what the CPU cached the first time and stops tracking the hardware — the
 * same trap §M33 stage 5 hit reading the IOMMU's own capability register, and
 * silent both times.
 *
 * Bounded by the window the kernel resolved above, so a request outside it is
 * refused with the reason rather than mapped. */
long drvuser_sys_mmio(uint64_t phys, uint64_t len) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    if (!d->win_phys) return DRV_EBAD;             /* ask for the window first */
    if (phys < d->win_phys || phys + len > d->win_phys + d->win_len) {
        kprintf("drv-user: '%s' asked to map %x..%x, its window is %x..%x\n",
                d->mf->name, (unsigned)phys, (unsigned)(phys + len),
                (unsigned)d->win_phys, (unsigned)(d->win_phys + d->win_len));
        return DRV_EBUSY;
    }
    struct task* t = task_current();
    if (!t || !t->mm) return DRV_EBAD;

    if (vmm_space_mmap_cursor(t->mm) == 0)
        vmm_space_set_mmap_cursor(t->mm, vmm_user_base() + 0x10000000u);
    uintptr_t va = vmm_space_mmap_cursor(t->mm);
    uint64_t n = (len + 4095) / 4096;
    for (uint64_t i = 0; i < n; i++)
        if (vmm_space_map(t->mm, va + (uintptr_t)i * 4096,
                          (uintptr_t)(phys + i * 4096),
                          VMM_USER | VMM_WRITABLE | VMM_CACHE_DIS | VMM_SHARED) != 0)
            return DRV_ENORES;
    vmm_space_set_mmap_cursor(t->mm, va + (uintptr_t)n * 4096);
    kprintf("drv-user: '%s' mapped its %x-byte register window at %x in ring 3\n",
            d->mf->name, (unsigned)len, (unsigned)va);
    d->h_mmio_va = va;
    drv_res_note_mmio(&d->rt, phys, len, va, "ring-3 register window");
    return (long)va;
}

/* A DMA buffer: allocated, CONFINED to this driver's IOMMU domain, and mapped
 * into its address space.  The device address is returned separately from the
 * CPU one — they are equal today and are not the thing being conflated. */
long drvuser_sys_dma(int bytes, int addr_bits, uint64_t* out_dev) {
    struct drvuser* d = du_current();
    if (!d) return DRV_EBAD;
    if (bytes <= 0 || bytes > d->mf->dma_bytes) {
        kprintf("drv-user: '%s' asked for %d DMA bytes, manifest allows %d\n",
                d->mf->name, bytes, d->mf->dma_bytes);
        return DRV_EBUSY;
    }
    if (addr_bits <= 0 || addr_bits > 64) addr_bits = 32;

    uint32_t frames = (uint32_t)((bytes + 4095) / 4096);
    uint64_t limit = (addr_bits >= 64) ? ~0ull : ((1ull << addr_bits) - 1);
    uint64_t phys;
    if (addr_bits < 32) {
        int order = 0; while ((1u << order) < frames) order++;
        phys = page_alloc_below(order, (pmm_phys_t)limit);
        if (phys == PMM_ALLOC_FAIL) phys = 0;
    } else {
        phys = pmm_alloc_contiguous_dma32(frames);
    }
    if (!phys) return DRV_ENORES;
    if (phys + (uint64_t)frames * 4096 - 1 > limit) return DRV_ENORES;

    /* CONFINE BEFORE MAPPING.  The device must not be able to reach the buffer
     * on a wider domain even briefly — and doing it in this order means a
     * failure here leaves a device that can reach less, never more. */
    if (d->bdf) iommu_confine(d->bdf, phys, (uint64_t)frames * 4096);

    struct task* t = task_current();
    if (!t || !t->mm) return DRV_EBAD;
    if (vmm_space_mmap_cursor(t->mm) == 0)
        vmm_space_set_mmap_cursor(t->mm, vmm_user_base() + 0x10000000u);
    uintptr_t va = vmm_space_mmap_cursor(t->mm);
    for (uint32_t i = 0; i < frames; i++)
        if (vmm_space_map(t->mm, va + (uintptr_t)i * 4096,
                          (uintptr_t)(phys + (uint64_t)i * 4096),
                          VMM_USER | VMM_WRITABLE | VMM_SHARED) != 0)
            return DRV_ENORES;
    vmm_space_set_mmap_cursor(t->mm, va + (uintptr_t)frames * 4096);

    d->dma_phys = phys;
    /* Record it where every other resource lives, so `drv res` and the device
     * manager stop omitting a placed driver's DMA buffer — and so the frames go
     * back on exactly one path. */
    drv_res_note_dma(&d->rt, phys, (uint64_t)frames * 4096, va, phys,
                     "ring-3 DMA buffer");
    if (out_dev) *out_dev = phys;
    kprintf("drv-user: '%s' got %d DMA bytes at %x, confined and mapped at %x\n",
            d->mf->name, bytes, (unsigned)phys, (unsigned)va);
    return (long)va;
}

