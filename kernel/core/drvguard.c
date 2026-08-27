/* =============================================================================
 * drvguard.c — the portable half of §M33 Tier 0.  See drvguard.h for the
 * design, the honest limit, and the reason the lock check is load-bearing.
 *
 * The arch-specific half is two assembly routines per target:
 *   drvguard_save(regs)  — store callee-saved regs + sp + return address; 0
 *   drvguard_land()      — entered from the fault handler with the ctx pointer
 *                          in a fixed register; restores and returns non-zero
 * ============================================================================= */

#include "drvguard.h"
#include "driver.h"
#include "percpu.h"
#include "printf.h"
#include "klog.h"
#include "crash.h"
#include <stddef.h>

/* One slot per CPU.  Not in `struct percpu` deliberately: that struct is
 * touched by the scheduler on every switch and is read by assembly in three
 * ports, and §M51 spent an hour on measurements that were fiction because a
 * field was added to it without a full rebuild.  A separate array keyed by CPU
 * index costs one indexing operation and cannot disturb any of that. */
#define DRVGUARD_MAX_CPUS 8
static struct drvguard_ctx g_guard[DRVGUARD_MAX_CPUS];
static volatile uint32_t   g_fault_count;

int    drvguard_save(uintptr_t* regs);       /* arch asm */
static void drvguard_deliver(struct driver* d, const char* what);

static struct drvguard_ctx* my_slot(void) {
    int c = this_cpu_id();
    if (c < 0 || c >= DRVGUARD_MAX_CPUS) return NULL;
    return &g_guard[c];
}

struct driver* drvguard_current(const char** what) {
    struct drvguard_ctx* g = my_slot();
    if (!g || !g->armed) return NULL;
    if (what) *what = g->what;
    return g->drv;
}

uint32_t drvguard_fault_count(void) { return g_fault_count; }

/* Fault facts captured in the handler, reported in task context.  Small and
 * plain — this is written from an exception with a rewritten frame pending, so
 * it must not allocate, lock, or call anything that could fault. */
struct drv_fault_info { int exc; const char* name; uintptr_t pc, addr; };
static struct drv_fault_info g_last[DRVGUARD_MAX_CPUS];

int drvguard_call(struct driver* d, const char* what,
                  int (*fn)(void*), void* ctx) {
    if (!fn) return 0;

    struct drvguard_ctx* g = my_slot();
    /* No slot (a CPU index beyond the table) or already armed: run the call
     * UNGUARDED rather than refusing it.
     *
     * Refusing would turn "we could not protect this" into "this driver does
     * not come up", which is a worse outcome than the status quo for a
     * mechanism whose entire purpose is to make failures less severe.  It is
     * logged, once, so a nested arm is visible rather than mysterious. */
    if (!g || g->armed) {
        if (g && g->armed)
            klog(KLOG_WARN, "drv", "guard already armed (%s in '%s') — "
                 "running '%s' unguarded", g->what,
                 g->drv && g->drv->name ? g->drv->name : "?", what);
        return fn(ctx);
    }

    g->drv            = d;
    g->what           = what;
    g->preempt_at_arm = this_cpu()->preempt_count;
    g->armed          = 1;

    int rc;
    if (drvguard_save(g->regs) == 0) {
        rc = fn(ctx);                 /* the normal path */
        g->armed = 0;
        g->drv   = NULL;
    } else {
        /* We came back through drvguard_land.  The guard was disarmed by
         * drvguard_recover before the frame was rewritten, so we are on the
         * ordinary stack in ordinary context with nothing armed.
         *
         * THE REPORT RUNS HERE, NOT IN THE FAULT HANDLER, AND THAT IS A BUG
         * FIX RATHER THAN TIDINESS.  The first version reported and quarantined
         * from inside the handler — and quarantining runs the driver's
         * `shutdown`, which goes through drvguard_call, which RE-ARMS the guard
         * and OVERWRITES the very context the landing pad was about to restore.
         * The pad then jumped through interrupt-context values: the fault
         * handler's own report was followed by `EXCEPTION 14 at eip=0x00000001`
         * and an NMI hard lockup.  §M54's defect class exactly — a saved
         * context reused by something that ran in between.
         *
         * It is also §M47's split, which this tree already had the right shape
         * for: CAPTURE in fault context (copy a few scalars, touch nothing),
         * DELIVER in ordinary context (where allocating, locking and calling
         * driver code are all allowed). */
        rc = DRVGUARD_FAULTED;
        g->drv = NULL;
        drvguard_deliver(d, what);
    }
    return rc;
}

/* The report, in ONE place so the three fault handlers cannot drift into three
 * different descriptions of the same event.  Called from the handler AFTER
 * drvguard_recover has disarmed, so a fault inside the report itself is not
 * caught by the guard it is reporting on.
 *
 * It also QUARANTINES the driver, which is §M66's mechanism doing exactly what
 * it was built for: a driver that faulted is not restarted behind the user's
 * back, and takes an explicit `drv start` to clear.  Without that a driver that
 * faults in probe would be re-probed by the hot-plug rescan two seconds later,
 * forever. */
/* CAPTURE — called from the fault handler.  Copies four scalars into a per-CPU
 * slot and does nothing else: no printing, no locking, no driver code.  The
 * frame is about to be rewritten and anything that faults here has no guard
 * left to catch it. */
void drvguard_report(int exc, const char* exc_name,
                     uintptr_t pc, uintptr_t addr) {
    int c = this_cpu_id();
    if (c < 0 || c >= DRVGUARD_MAX_CPUS) return;
    g_last[c].exc  = exc;
    g_last[c].name = exc_name;
    g_last[c].pc   = pc;
    g_last[c].addr = addr;
}

/* DELIVER — called from drvguard_call once the unwind has landed, on the
 * ordinary stack in ordinary context, where quarantining (which runs the
 * driver's shutdown hook) is a legal thing to do. */
static void drvguard_deliver(struct driver* d, const char* what) {
    int c = this_cpu_id();
    struct drv_fault_info f = { 0, "?", 0, 0 };
    if (c >= 0 && c < DRVGUARD_MAX_CPUS) f = g_last[c];
    const char* nm = (d && d->name) ? d->name : "?";

    kprintf("\n!! DRIVER FAULT contained — '%s' died in %s: "
            "exception %d (%s) at pc=%p addr=%p\n",
            nm, what ? what : "?", f.exc, f.name ? f.name : "?",
            (void*)f.pc, (void*)f.addr);
    kprintf("   the driver is quarantined; the system is still running\n");
    klog(KLOG_ERR, "drv", "'%s' faulted in %s at %p — quarantined",
         nm, what ? what : "?", (void*)f.pc);

    /* §M47 — a record, so whatever reporting sink is armed can deliver it.
     * CRASH_KERNEL_FAULT is the right kind: it WAS a ring-0 fault.  What
     * changed is that it no longer ends the boot. */
    crash_report(CRASH_KERNEL_FAULT, -1, nm, f.pc, f.addr, 0,
                 f.name ? f.name : "driver fault");

    /* Quarantine LAST, because it runs the driver's own shutdown hook — which
     * goes back through drvguard_call.  Legal here (nothing is armed, we are on
     * a normal stack) and a fault inside THAT shutdown is contained in turn. */
    if (d) driver_fault(nm, "faulted in a driver entry point");
}

int drvguard_recover(uintptr_t fault_pc, uintptr_t* resume_ip,
                     uintptr_t* resume_arg) {
    (void)fault_pc;
    struct drvguard_ctx* g = my_slot();
    if (!g || !g->armed) return 0;

    /* THE LOCK CHECK.  A spinlock in this tree disables preemption, so a count
     * that has moved since the guard was armed means the driver is holding one
     * — and unwinding past a held lock leaves it held forever.  A deadlocked
     * machine is worse than a panicked one, because a panic says what happened.
     *
     * Refusing here is therefore the CORRECT outcome, not a gap: the system
     * genuinely cannot continue, and the old fault policy will say so. */
    if (this_cpu()->preempt_count != g->preempt_at_arm) {
        g->armed = 0;                /* do not try again on the way down */
        kprintf("drv: '%s' faulted in %s while holding a lock — "
                "cannot unwind, falling through to kernel.fault_policy\n",
                g->drv && g->drv->name ? g->drv->name : "?", g->what);
        return 0;
    }

    /* DISARM BEFORE RETURNING.  Everything that happens between here and the
     * landing pad — the report, the log — is ordinary kernel code, and a fault
     * inside it must not be caught by this same guard and sent round again. */
    g->armed = 0;
    g_fault_count++;

    *resume_ip  = (uintptr_t)drvguard_land;
    *resume_arg = (uintptr_t)g->regs;
    return 1;
}
