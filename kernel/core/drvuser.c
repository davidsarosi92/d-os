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
 * ============================================================================= */

#include "drvuser.h"
#include "drvrt.h"
#include "task.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include "hal_api.h"
#include "mouse.h"
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
};

static const struct drv_manifest g_manifest[] = {
    /* The 8042 aux device: four registers at 0x60 and IRQ 12.  Exactly what
     * ps2_mouse.c asks the in-kernel backend for — the same numbers, now
     * written down where the driver cannot reach them. */
    { "ps2_mouse", 0x60, 5, 12 },
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
};

static struct drvuser g_du[DRVUSER_MAX];

static struct drvuser* du_of_pid(int pid) {
    for (int i = 0; i < DRVUSER_MAX; i++)
        if (g_du[i].used && g_du[i].pid == pid) return &g_du[i];
    return NULL;
}

static struct drvuser* du_current(void) {
    struct task* t = task_current();
    return t ? du_of_pid(t->pid) : NULL;
}

int drvuser_attach(int pid, const char* driver_name) {
    const struct drv_manifest* mf = drvuser_manifest(driver_name);
    if (!mf) return -1;
    for (int i = 0; i < DRVUSER_MAX; i++) {
        if (g_du[i].used) continue;
        struct drvuser* d = &g_du[i];
        d->used = 1;
        d->pid  = pid;
        d->mf   = mf;
        d->h_ports = d->h_irq = -1;
        d->bitmap  = NULL;
        drv_rt_init(&d->rt, mf->name);
        return 0;
    }
    return -1;
}

void drvuser_detach(int pid) {
    struct drvuser* d = du_of_pid(pid);
    if (!d) return;
    /* Release the kernel-side resources FIRST, then the bitmap.  The other
     * order would leave a task briefly permitted to touch ports whose grant
     * has already been handed back — a window nothing would ever notice and
     * that costs nothing to close. */
    drv_release_all(&d->rt);
    if (d->bitmap) { kfree(d->bitmap); d->bitmap = NULL; }
    d->used = 0;
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
    mouse_publish(dx, dy, buttons, dz);
    return 0;
}
