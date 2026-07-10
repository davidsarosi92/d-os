/* =============================================================================
 * watchdog.c — heartbeat-based freeze detection (M31).  See watchdog.h.
 *
 * A single sweep task (child of init) runs both detectors every WD_SWEEP_MS:
 *   - Layer 1: walks the opt-in per-task heartbeat table; a task past its
 *     deadline is logged + kill-tree'd (a supervised service is then restarted
 *     by its M29 supervisor).
 *   - Layer 2: snapshots every online CPU's percpu.ticks; a counter that
 *     didn't advance since the last sweep is a softlockup warning.
 *
 * Limitation (documented in watchdog.h): the sweep runs on ONE CPU, so a wedge
 * on that same CPU can starve the sweep itself.  On a 1–2 CPU teaching box the
 * common case (a wedge on a *different* core than the sweep) is caught.
 * ============================================================================= */

#include "watchdog.h"
#include "task.h"
#include "lock.h"
#include "klog.h"
#include "timer.h"
#include "percpu.h"
#include "procfs.h"
#include <stddef.h>
#include <stdint.h>

#define WD_MAX        32       /* opt-in per-task heartbeat slots        */
#define WD_SWEEP_MS   500      /* how often the sweep runs               */
#define WD_MAX_CPUS   64       /* softlockup snapshot array bound        */

struct wd_entry {
    int      pid;              /* watched task, or -1 for a free slot     */
    uint32_t timeout_ms;       /* deadline between kicks                   */
    uint64_t last_kick;        /* timer_ticks_ms of the last kick         */
    int      hung_reported;    /* one-shot so we don't spam / re-kill     */
};

static struct wd_entry wd[WD_MAX];
static spinlock_t       wd_lock = SPINLOCK_INIT;

/* Softlockup: last-seen per-CPU tick counter.  0 = "no baseline yet". */
static uint64_t cpu_tick_seen[WD_MAX_CPUS];

/* Counters for /proc/watchdog. */
static volatile uint32_t g_hang_events;
static volatile uint32_t g_softlockup_events;

/* ------------------------------------------------------------------- */
/* Registration API (runs on the watched task).                         */
/* ------------------------------------------------------------------- */

static int wd_find_pid_locked(int pid) {
    for (int i = 0; i < WD_MAX; i++) if (wd[i].pid == pid) return i;
    return -1;
}

void watchdog_register(uint32_t timeout_ms) {
    struct task* self = task_current();
    if (!self) return;
    int pid = self->pid;

    uint32_t f = spin_lock_irqsave(&wd_lock);
    int idx = wd_find_pid_locked(pid);
    if (idx < 0) idx = wd_find_pid_locked(-1);        /* claim a free slot */
    if (idx >= 0) {
        wd[idx].pid           = pid;
        wd[idx].timeout_ms    = timeout_ms ? timeout_ms : 1000;
        wd[idx].last_kick     = timer_ticks_ms();
        wd[idx].hung_reported = 0;
    }
    spin_unlock_irqrestore(&wd_lock, f);
}

void watchdog_kick(void) {
    struct task* self = task_current();
    if (!self) return;
    uint32_t f = spin_lock_irqsave(&wd_lock);
    int idx = wd_find_pid_locked(self->pid);
    if (idx >= 0) { wd[idx].last_kick = timer_ticks_ms(); wd[idx].hung_reported = 0; }
    spin_unlock_irqrestore(&wd_lock, f);
}

void watchdog_unregister(void) {
    struct task* self = task_current();
    if (!self) return;
    uint32_t f = spin_lock_irqsave(&wd_lock);
    int idx = wd_find_pid_locked(self->pid);
    if (idx >= 0) wd[idx].pid = -1;
    spin_unlock_irqrestore(&wd_lock, f);
}

/* ------------------------------------------------------------------- */
/* The sweep (runs on the watchdog task).                               */
/* ------------------------------------------------------------------- */

/* Layer 1: one pass over the heartbeat table.  We collect the pid + name of an
 * over-deadline task under the lock, then act (log + kill) after releasing it
 * — task_find / task_kill_tree take the scheduler's master_lock, which must not
 * nest under wd_lock. */
static void sweep_heartbeats(void) {
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < WD_MAX; i++) {
        int      pid;
        uint32_t timeout;
        uint64_t last;
        int      already;

        uint32_t f = spin_lock_irqsave(&wd_lock);
        pid = wd[i].pid; timeout = wd[i].timeout_ms;
        last = wd[i].last_kick; already = wd[i].hung_reported;
        spin_unlock_irqrestore(&wd_lock, f);
        if (pid < 0) continue;

        /* Reap stale entries whose task is gone / dead. */
        struct task* t = task_find(pid);
        if (!t) {
            uint32_t f2 = spin_lock_irqsave(&wd_lock);
            if (wd[i].pid == pid) wd[i].pid = -1;
            spin_unlock_irqrestore(&wd_lock, f2);
            continue;
        }

        if (!already && (now - last) > timeout) {
            g_hang_events++;
            klog(KLOG_ERR, "watchdog",
                 "task '%s' (pid %d) missed heartbeat (%ums > %ums) — killing tree\n",
                 t->name, pid, (unsigned)(now - last), (unsigned)timeout);
            uint32_t f3 = spin_lock_irqsave(&wd_lock);
            if (wd[i].pid == pid) wd[i].hung_reported = 1;
            spin_unlock_irqrestore(&wd_lock, f3);
            task_kill_tree(pid);       /* lands at the task's next yield point */
        }
    }
}

/* Layer 2: softlockup — a CPU whose scheduler-tick counter didn't move. */
static void sweep_softlockup(void) {
    int n = smp_ncpus();
    if (n > WD_MAX_CPUS) n = WD_MAX_CPUS;
    for (int i = 0; i < n; i++) {
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) { cpu_tick_seen[i] = 0; continue; }
        uint64_t cur = p->ticks;
        if (cpu_tick_seen[i] != 0 && cur == cpu_tick_seen[i]) {
            g_softlockup_events++;
            klog(KLOG_WARN, "watchdog",
                 "CPU %d softlockup? no scheduler tick in ~%ums\n",
                 i, (unsigned)WD_SWEEP_MS);
        }
        cpu_tick_seen[i] = cur;
    }
}

static void watchdog_entry(void) {
    klog(KLOG_INFO, "watchdog", "up as pid %d (sweep %ums)\n",
         task_current() ? task_current()->pid : -1, (unsigned)WD_SWEEP_MS);
    for (;;) {
        task_msleep(WD_SWEEP_MS);
        sweep_heartbeats();
        sweep_softlockup();
    }
}

/* ------------------------------------------------------------------- */
/* /proc/watchdog + boot wiring.                                        */
/* ------------------------------------------------------------------- */

static void gen_watchdog(struct procfs_writer* w) {
    pw_puts(w, "sweep_ms "); pw_put_uint(w, (unsigned)WD_SWEEP_MS); pw_putc(w, '\n');
    pw_puts(w, "hang_events "); pw_put_uint(w, (unsigned)g_hang_events); pw_putc(w, '\n');
    pw_puts(w, "softlockup_events "); pw_put_uint(w, (unsigned)g_softlockup_events);
    pw_putc(w, '\n');
    pw_puts(w, "# watched tasks (pid  timeout_ms  ms_since_kick  hung)\n");
    uint64_t now = timer_ticks_ms();
    uint32_t f = spin_lock_irqsave(&wd_lock);
    for (int i = 0; i < WD_MAX; i++) {
        if (wd[i].pid < 0) continue;
        pw_put_uint(w, (unsigned)wd[i].pid); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)wd[i].timeout_ms); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)(now - wd[i].last_kick)); pw_putc(w, ' ');
        pw_puts(w, wd[i].hung_reported ? "yes" : "no");
        pw_putc(w, '\n');
    }
    spin_unlock_irqrestore(&wd_lock, f);
}

static struct procfs_node nd_watchdog = { .name = "watchdog", .gen = gen_watchdog };

void watchdog_init(void) {
    for (int i = 0; i < WD_MAX; i++) wd[i].pid = -1;
    procfs_register(&nd_watchdog);
    task_spawn_detached("watchdog", watchdog_entry);
}
