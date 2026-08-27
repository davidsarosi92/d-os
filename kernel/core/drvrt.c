/* =============================================================================
 * drvrt.c — the IN-KERNEL backend of the driver-runtime API (§M33 stage 2).
 *
 * See drvrt.h for the interface and the four rules its shape follows.  This
 * file is the backend that exists; the user-mode one (Tier 1) is the same
 * header with port grants going into the TSS I/O bitmap, MMIO mapped into the
 * driver's own address space, and `drv_irq_wait` becoming a syscall.
 *
 * THE BOUNDS CHECKS RUN HERE TOO, and that is deliberate rather than
 * defensive.  In-kernel they protect nothing a determined driver could not
 * bypass — it can still call `outb` directly.  But a check that only runs in
 * the configuration nobody tests is a check that does not work, and every
 * driver ported to this API is exercising the user backend's validation logic
 * long before that backend exists.
 * ============================================================================= */

#include "drvrt.h"
#include "printf.h"
#include "klog.h"
#include "kmalloc.h"
#include "pmm.h"
#include "hal_api.h"
#include "waitq.h"
#include "timer.h"
#include "ktimer.h"
#include "task.h"
#include <stddef.h>

#if defined(__i386__) || defined(__x86_64__)
#include "hal.h"        /* inb / outb */
#include "idt.h"        /* irq_install */
#include "vmm.h"
#define DRVRT_HAS_PORTS 1
#else
#define DRVRT_HAS_PORTS 0
void gic_register_handler(uint32_t intid, void (*fn)(uint32_t));
void gic_enable_irq(uint32_t intid);
void mmu_map_device_1gib(uint64_t va);
#endif

enum res_kind { RES_PORTS = 1, RES_MMIO, RES_IRQ, RES_DMA };

struct drv_res {
    int            used;
    enum res_kind  kind;
    const char*    owner;              /* driver name — quoted in conflicts */
    const char*    why;                /* what it is for — quoted too */

    uint64_t       base;               /* port base / phys addr / irq line   */
    uint64_t       len;                /* port count / byte length           */
    uintptr_t      va;                 /* MMIO pointer or DMA cpu pointer    */
    uint64_t       dev;                /* DMA device-visible address         */

    /* IRQ only. */
    struct waitq   wq;
    volatile uint32_t count;           /* total fires                        */
    volatile uint32_t seen;            /* what the driver has consumed       */
};

/* A flat table rather than per-context allocation.  The count is the ceiling on
 * resources IN THE WHOLE SYSTEM, which is what makes a conflict report
 * possible: to say "port 0x60 is held by ps2-keyboard" something has to know
 * about every grant, not just this driver's. */
#define DRVRT_MAX_RES 48
static struct drv_res g_res[DRVRT_MAX_RES];

/* IRQ line -> resource, so the shared ISR trampoline can find the waitq.
 * Bounded by the vector space; a line outside it is refused rather than
 * indexed. */
#define DRVRT_MAX_IRQ 64
static struct drv_res* g_irq[DRVRT_MAX_IRQ];

/* ---- handles -------------------------------------------------------------- */

static struct drv_res* res_of(drv_handle h) {
    if (h <= 0 || h > DRVRT_MAX_RES) return NULL;
    struct drv_res* r = &g_res[h - 1];
    return r->used ? r : NULL;
}

static drv_handle res_alloc(struct drv_rt* rt, enum res_kind kind,
                            const char* why) {
    if (!rt || rt->nres >= DRV_MAX_RES) return DRV_ENORES;
    for (int i = 0; i < DRVRT_MAX_RES; i++) {
        if (g_res[i].used) continue;
        struct drv_res* r = &g_res[i];
        r->used  = 1;
        r->kind  = kind;
        r->owner = rt->owner;
        r->why   = why;
        r->base = r->len = r->dev = 0;
        r->va = 0;
        r->count = r->seen = 0;
        rt->res[rt->nres++] = r;
        return (drv_handle)(i + 1);
    }
    return DRV_ENORES;
}

void drv_rt_init(struct drv_rt* rt, const char* owner) {
    if (!rt) return;
    rt->owner = owner ? owner : "?";
    rt->nres  = 0;
    for (int i = 0; i < DRV_MAX_RES; i++) rt->res[i] = NULL;
}

/* ---- conflicts ------------------------------------------------------------ */

#if DRVRT_HAS_PORTS
/* Report a clash BY NAME.  `irq_install` does not chain in this tree (§M23
 * stage 6), and until now a collision was something you inferred from a device
 * not working.  "IRQ 12 is held by ps2-mouse" is the difference between a
 * five-minute problem and an afternoon. */
static struct drv_res* find_port_clash(uint16_t base, uint16_t count) {
    for (int i = 0; i < DRVRT_MAX_RES; i++) {
        struct drv_res* r = &g_res[i];
        if (!r->used || r->kind != RES_PORTS) continue;
        if (base + count <= r->base || r->base + r->len <= base) continue;
        return r;
    }
    return NULL;
}

/* ---- ports ---------------------------------------------------------------- */

drv_handle drv_ports_request(struct drv_rt* rt, uint16_t base, uint16_t count,
                             const char* why) {
    if (!count) return DRV_ERANGE;
    struct drv_res* clash = find_port_clash(base, count);
    if (clash) {
        kprintf("drv-rt: '%s' wants ports %x..%x for %s — held by '%s' (%s)\n",
                rt ? rt->owner : "?", base, base + count - 1,
                why ? why : "?", clash->owner, clash->why ? clash->why : "?");
        return DRV_EBUSY;
    }
    drv_handle h = res_alloc(rt, RES_PORTS, why);
    if (h < 0) return h;
    struct drv_res* r = res_of(h);
    r->base = base;
    r->len  = count;
    return h;
}

/* The bound check, in one place for all six accessors.  Returns the absolute
 * port, or a negative error — so every accessor is three lines and none of
 * them can forget the check. */
static long port_at(drv_handle h, uint16_t off) {
    struct drv_res* r = res_of(h);
    if (!r || r->kind != RES_PORTS) return DRV_EBAD;
    if (off >= r->len) return DRV_ERANGE;
    return (long)(r->base + off);
}

int  drv_in8 (drv_handle h, uint16_t off) { long p = port_at(h, off); return p < 0 ? (int)p : (int)inb((uint16_t)p); }
int  drv_in16(drv_handle h, uint16_t off) { long p = port_at(h, off); return p < 0 ? (int)p : (int)inw((uint16_t)p); }
long drv_in32(drv_handle h, uint16_t off) { long p = port_at(h, off); return p < 0 ? p : (long)inl((uint16_t)p); }
int  drv_out8 (drv_handle h, uint16_t off, uint8_t v)  { long p = port_at(h, off); if (p < 0) return (int)p; outb((uint16_t)p, v); return 0; }
int  drv_out16(drv_handle h, uint16_t off, uint16_t v) { long p = port_at(h, off); if (p < 0) return (int)p; outw((uint16_t)p, v); return 0; }
int  drv_out32(drv_handle h, uint16_t off, uint32_t v) { long p = port_at(h, off); if (p < 0) return (int)p; outl((uint16_t)p, v); return 0; }
#else
/* No port space on this machine.  Every entry point says DRV_ENOSYS rather
 * than pretending — a driver that needs ports is a driver for hardware this
 * arch does not have, and it should find that out at the request rather than
 * from a device that never answers. */
drv_handle drv_ports_request(struct drv_rt* rt, uint16_t base, uint16_t count,
                             const char* why) {
    (void)rt; (void)base; (void)count; (void)why; return DRV_ENOSYS;
}
int  drv_in8 (drv_handle h, uint16_t off) { (void)h; (void)off; return DRV_ENOSYS; }
int  drv_in16(drv_handle h, uint16_t off) { (void)h; (void)off; return DRV_ENOSYS; }
long drv_in32(drv_handle h, uint16_t off) { (void)h; (void)off; return DRV_ENOSYS; }
int  drv_out8 (drv_handle h, uint16_t off, uint8_t v)  { (void)h; (void)off; (void)v; return DRV_ENOSYS; }
int  drv_out16(drv_handle h, uint16_t off, uint16_t v) { (void)h; (void)off; (void)v; return DRV_ENOSYS; }
int  drv_out32(drv_handle h, uint16_t off, uint32_t v) { (void)h; (void)off; (void)v; return DRV_ENOSYS; }
#endif

/* ---- MMIO ----------------------------------------------------------------- */

drv_handle drv_mmio_request(struct drv_rt* rt, uint64_t phys, size_t len,
                            const char* why) {
    drv_handle h = res_alloc(rt, RES_MMIO, why);
    if (h < 0) return h;
    struct drv_res* r = res_of(h);
    r->base = phys;
    r->len  = len;
#if DRVRT_HAS_PORTS
    /* x86: map it page by page into the kernel's space, exactly as hda.c did
     * by hand before this API existed. */
    uintptr_t va = (uintptr_t)phys;
    for (size_t o = 0; o < len; o += 4096)
        vmm_map(va + o, (uintptr_t)phys + o, VMM_WRITABLE);
    r->va = va;
#else
    /* aarch64: the kernel identity-maps device space in 1 GiB blocks, so the
     * physical address IS the pointer once its block is present. */
    mmu_map_device_1gib(phys);
    r->va = (uintptr_t)phys;
#endif
    return h;
}

volatile void* drv_mmio_ptr(drv_handle h) {
    struct drv_res* r = res_of(h);
    if (!r || r->kind != RES_MMIO) return NULL;
    return (volatile void*)r->va;
}

/* ---- interrupts ----------------------------------------------------------- */

/* THE ISR DOES TWO THINGS AND NO THIRD: count, and wake.  Written once, here,
 * so no ported driver can get it wrong — which is the point of moving the shape
 * into the API rather than leaving it as advice.  §M49's xHCI drain and §M55's
 * NIC pump are both this lesson learnt the expensive way. */
static void drvrt_isr_common(int line) {
    if (line < 0 || line >= DRVRT_MAX_IRQ) return;
    struct drv_res* r = g_irq[line];
    if (!r) return;
    r->count++;
    /* waitq's own contract: the wake takes the lock.  §M23 stage 4 broke this
     * rule in three places and it presented as an NMI hard lockup. */
    uint32_t fl = waitq_lock(&r->wq);
    waitq_wake_all(&r->wq);
    waitq_unlock(&r->wq, fl);
}

#if DRVRT_HAS_PORTS
static void drvrt_isr_x86(struct int_frame* f) {
    drvrt_isr_common((int)f->int_no - 32);
}
#else
static void drvrt_isr_arm(uint32_t intid) { drvrt_isr_common((int)intid); }
#endif

drv_handle drv_irq_request(struct drv_rt* rt, int line, const char* why) {
    if (line < 0 || line >= DRVRT_MAX_IRQ) return DRV_ERANGE;
    if (g_irq[line]) {
        kprintf("drv-rt: '%s' wants IRQ %d for %s — held by '%s' (%s)\n",
                rt ? rt->owner : "?", line, why ? why : "?",
                g_irq[line]->owner, g_irq[line]->why ? g_irq[line]->why : "?");
        return DRV_EBUSY;
    }
    drv_handle h = res_alloc(rt, RES_IRQ, why);
    if (h < 0) return h;
    struct drv_res* r = res_of(h);
    r->base = (uint64_t)line;
    waitq_init(&r->wq);
    g_irq[line] = r;
#if DRVRT_HAS_PORTS
    irq_install(line, drvrt_isr_x86);
#else
    gic_register_handler((uint32_t)line, drvrt_isr_arm);
    gic_enable_irq((uint32_t)line);
#endif
    return h;
}

/* The callback signature is `void(*)(struct ktimer*)` — the context comes back
 * through `t->arg`, which the kernel stores and never interprets. */
static void irq_timeout_fired(struct ktimer* t) {
    struct drv_res* r = (struct drv_res*)t->arg;
    uint32_t fl = waitq_lock(&r->wq);
    waitq_wake_all(&r->wq);
    waitq_unlock(&r->wq, fl);
}

int drv_irq_wait(drv_handle h, int timeout_ms) {
    struct drv_res* r = res_of(h);
    if (!r || r->kind != RES_IRQ) return DRV_EBAD;

    /* Consume whatever arrived while we were away FIRST, without sleeping.
     * The count is what the caller gets, not a boolean: a driver that slept
     * through three interrupts must be told three, or it processes one and
     * silently loses two — and under load that is the normal case, not the
     * exception. */
    uint32_t n = r->count - r->seen;
    if (n) { r->seen = r->count; return (int)n; }
    if (timeout_ms == 0) return 0;

    /* A DEADLINE FROM THE CLOCK, and the timer only WAKES.
     *
     * §M61's lesson, and it cost that milestone a visible bug: a counter
     * decremented by a re-arming timer measures how many timer EVENTS have
     * happened, not how much time has passed, and under emulation a missed or
     * doubled firing lands straight in what the caller reads.  `remaining =
     * deadline - now` cannot drift, and the ktimer's only job is to make sure
     * somebody looks. */
    uint64_t deadline = 0;
    struct ktimer t;
    int armed = 0;
    if (timeout_ms > 0) {
        deadline = timer_now_ns() + (uint64_t)timeout_ms * 1000000ull;
        ktimer_arm(&t, deadline, irq_timeout_fired, r);
        armed = 1;
    }

    /* waitq.h's discipline, verbatim: hold the lock, LOOP on the condition,
     * and remember that waitq_block RE-ACQUIRES the lock before returning.
     * §M23 stage 4 treated it as returning unlocked and produced a task
     * deadlocking against itself with interrupts masked. */
    uint32_t fl = waitq_lock(&r->wq);
    while (r->count == r->seen) {
        if (armed && timer_now_ns() >= deadline) break;
        waitq_block(&r->wq);
        if (task_should_stop()) break;
    }
    waitq_unlock(&r->wq, fl);
    if (armed) ktimer_cancel(&t);

    n = r->count - r->seen;
    r->seen = r->count;
    return n ? (int)n : DRV_ETIME;
}

uint32_t drv_irq_count(drv_handle h) {
    struct drv_res* r = res_of(h);
    return (r && r->kind == RES_IRQ) ? r->count : 0;
}

/* ---- DMA ------------------------------------------------------------------ */

drv_handle drv_dma_request(struct drv_rt* rt, size_t bytes, const char* why) {
    if (!bytes) return DRV_ERANGE;
    drv_handle h = res_alloc(rt, RES_DMA, why);
    if (h < 0) return h;
    struct drv_res* r = res_of(h);
    size_t frames = (bytes + 4095) / 4096;
    uint64_t phys = pmm_alloc_contiguous_dma32((uint32_t)frames);
    if (!phys) { r->used = 0; return DRV_ENORES; }
    r->base = phys;
    r->len  = frames * 4096;
    r->va   = (uintptr_t)phys;      /* identity/direct on every target here */
    /* THE DEVICE ADDRESS IS RECORDED SEPARATELY even though it is the same
     * number today.  Under an IOMMU (§M33 stage 5) it will not be, and a
     * driver that used the CPU pointer because "they are equal" would be the
     * bug that milestone has to go and find in every driver. */
    r->dev  = phys;
    return h;
}

void*    drv_dma_cpu(drv_handle h)    { struct drv_res* r = res_of(h); return (r && r->kind == RES_DMA) ? (void*)r->va : NULL; }
uint64_t drv_dma_device(drv_handle h) { struct drv_res* r = res_of(h); return (r && r->kind == RES_DMA) ? r->dev : 0; }

/* ---- teardown ------------------------------------------------------------- */

void drv_release_all(struct drv_rt* rt) {
    if (!rt) return;
    for (int i = 0; i < rt->nres; i++) {
        struct drv_res* r = rt->res[i];
        if (!r || !r->used) continue;
        if (r->kind == RES_IRQ && r->base < DRVRT_MAX_IRQ) {
            g_irq[r->base] = NULL;
            /* The handler stays installed: `irq_install` has no uninstall in
             * this tree, and inventing one here would be a change to the
             * interrupt path smuggled in under a driver API.  Clearing the
             * table entry is what matters — the ISR then finds nothing and
             * returns, which is the same as not being installed except for a
             * few instructions.  Written down rather than left as a surprise. */
        }
        if (r->kind == RES_DMA && r->base)
            pmm_free_contiguous((pmm_phys_t)r->base, (uint32_t)(r->len / 4096));
        r->used = 0;
    }
    rt->nres = 0;
}

/* ---- running the driver body ---------------------------------------------- */

/* Queued bodies, for drivers that came up before the scheduler did.  Small and
 * fixed: this is only ever the built-in drivers that init at boot, and a
 * ceiling that is reported beats an allocation that can fail on the one path
 * with no way to report anything. */
#define DRVRT_MAX_DEFER 8
struct deferred { const char* name; void (*body)(void); };
static struct deferred g_defer[DRVRT_MAX_DEFER];
static int  g_ndefer;
static int  g_sched_up;

int drv_run(struct drv_rt* rt, const char* name, void (*body)(void)) {
    (void)rt;
    if (!body) return DRV_EBAD;
    if (g_sched_up) {
        /* Detached, so it outlives whoever called init — the boot task, a
         * `drv start`, or a hot-plug rescan — and is reaped by init rather
         * than by a parent that has moved on (§M27). */
        return task_spawn_detached(name, body) ? 0 : DRV_ENORES;
    }
    if (g_ndefer >= DRVRT_MAX_DEFER) {
        kprintf("drv-rt: too many deferred driver tasks (%d) — '%s' not started\n",
                DRVRT_MAX_DEFER, name);
        return DRV_ENORES;
    }
    g_defer[g_ndefer].name = name;
    g_defer[g_ndefer].body = body;
    g_ndefer++;
    return 0;
}

void drvrt_start_deferred(void) {
    if (g_sched_up) return;
    g_sched_up = 1;
    for (int i = 0; i < g_ndefer; i++) {
        if (!task_spawn_detached(g_defer[i].name, g_defer[i].body))
            kprintf("drv-rt: could not start '%s'\n", g_defer[i].name);
    }
    if (g_ndefer) kprintf("drv-rt: %d deferred driver task(s) started\n", g_ndefer);
    g_ndefer = 0;
}

/* ---- diagnostics ---------------------------------------------------------- */

void drv_res_dump(void) {
    int n = 0;
    for (int i = 0; i < DRVRT_MAX_RES; i++) {
        struct drv_res* r = &g_res[i];
        if (!r->used) continue;
        if (!n) kprintf("driver resources:\n");
        n++;
        switch (r->kind) {
        case RES_PORTS:
            kprintf("  %s: ports %x..%x  (%s)\n", r->owner, (unsigned)r->base,
                    (unsigned)(r->base + r->len - 1), r->why ? r->why : "?");
            break;
        case RES_MMIO:
            kprintf("  %s: mmio %p +%d  (%s)\n", r->owner, (void*)(uintptr_t)r->base,
                    (int)r->len, r->why ? r->why : "?");
            break;
        case RES_IRQ:
            kprintf("  %s: irq %d, %u fired  (%s)\n", r->owner, (int)r->base,
                    r->count, r->why ? r->why : "?");
            break;
        case RES_DMA:
            kprintf("  %s: dma %p +%d dev=%p  (%s)\n", r->owner,
                    (void*)(uintptr_t)r->va, (int)r->len,
                    (void*)(uintptr_t)r->dev, r->why ? r->why : "?");
            break;
        }
    }
    if (!n) kprintf("no driver resources held\n");
}
