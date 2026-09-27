/* =============================================================================
 * cmd_task.c — scheduler and task shell commands (§M70).
 *
 * Split out of shell.c.  Two kinds live here and the difference is worth
 * naming: the REPORTS (`ps`, `sched`, `lscpu`) and the TORTURE TESTS
 * (`killstorm`, `schedstorm`, `rqcheck`).  The second kind exists because a
 * scheduler invariant nobody can falsify is a comment — §M57's `schedstorm`
 * first "passed" against the very kernel it was written to break, and only
 * driving four concurrent churners made it able to fail at all.
 *
 * `hardlock` is NOT here: it lives next to the watchdog it exercises, because
 * the thing it tests is that subsystem's, not the scheduler's.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "task.h"
#include "percpu.h"
#include "lock.h"
#include "timer.h"
#include "workqueue.h"
#include "hal.h"
#include "config.h"
#include "hal_api.h"
#include "proc.h"
#include "kmalloc.h"
#include <stdint.h>
#include <stddef.h>

static volatile int loop_stop_flag = 0;

#define SCHED_SNAP_MAX 64          /* tasks tracked per sample; plenty here */

struct sched_snap_task {
    int      pid;
    uint64_t cpu_ms;
    int      cpu_home;
    uint32_t demand;
    int      nice;
    char     name[24];
};





/* -------------------------------------------------------------------- */
/* Keyboard layout commands (M16).                                       */
/*                                                                      */
/* `lslayout`        → list registered layouts + show the active one   */
/* `setlayout <name>` → switch active layout (e.g. `setlayout hu`)      */
/*                                                                      */
/* Equivalent to `setconf keyboard.layout <name>` followed by reload,   */
/* but a one-shot command is friendlier and doesn't persist to disk.    */
/* -------------------------------------------------------------------- */




/* -------------------------------------------------------------------- */
/* CPU topology — `lscpu` (M18).                                         */
/* -------------------------------------------------------------------- */






/* --------------------------------------------------------------------
 * `sched [ms]` — measure how work is actually spread across the CPUs
 * (§M49).
 *
 * Why a sampling command and not a cumulative dump: since-boot totals
 * average away exactly what we need to see.  A CPU that was hammered
 * during boot and is idle now looks "half busy" forever, and a balancer
 * fix would be indistinguishable from no fix.  So this takes two
 * snapshots `ms` apart and reports the DELTA — the picture right now.
 *
 * BUSY% is the share of the window this CPU spent running a non-idle
 * task.  RQ is the instantaneous queue depth.  The two together are the
 * whole point: rq 1 + busy 100% is a saturated CPU with nothing to give
 * away, rq 4 + busy 100% is a CPU with three tasks' worth of work that
 * belongs somewhere else.  MIGR counts tasks pulled here by the
 * balancer — the cost side, and the way to catch a balancer that merely
 * shuttles the same task around.
 * -------------------------------------------------------------------- */


struct sched_snap {
    struct sched_snap_task t[SCHED_SNAP_MAX];
    int      n;
    uint64_t now;
};

struct wqtest_item {
    struct work w;
    int         idx;
    volatile int ran;
    volatile int cpu;
};

#define WQTEST_MAX 32

/* --- file-scope state these commands own (moved with them out of shell.c) --- */
static volatile int g_ks_alive;

static volatile int g_ks_round;          /* progress, readable from outside */

static volatile int g_ks_done;

static volatile int g_ks_rounds, g_ks_per;

static volatile int g_ks_spawned, g_ks_killed;

static volatile int g_ss_rounds, g_ss_done, g_ss_worst, g_ss_churn;

static volatile int g_ss_pids[12], g_ss_npids, g_ss_stop, g_ss_churners;

static struct rq_audit g_ss_worst_snap;

#define SS_CHURNERS 4

static struct wqtest_item wqt[WQTEST_MAX];

static void ticker_main(void) {
    int i = 0;
    for (;;) {
        kprintf("[tick %d]\n", i++);
        /* Busy-wait ~1 second using the millisecond tick. */
        uint64_t end = timer_ticks_ms() + 1000;
        while (timer_ticks_ms() < end) {
            task_yield();
        }
        if (i > 5) break;               /* finite demo */
    }
    kprintf("[ticker done]\n");
}

static void cmd_spawn(void) {
    struct task* t = task_spawn("ticker", ticker_main);
    if (!t) console_write("spawn: failed (OOM?)\n");
    else    kprintf("spawned pid %d\n", t->pid);
}

static void killstorm_victim(void) {
    while (!task_should_stop()) task_msleep(2);
    __atomic_sub_fetch(&g_ks_alive, 1, __ATOMIC_RELAXED);
}

/* The storm runs on its OWN task, not on the shell.  If it stalls, the shell
 * is still there to say so and to dump the task table — a test that hangs
 * along with the thing it is testing reports nothing at all, which is exactly
 * how two runs of this were wasted. */
static void killstorm_driver(void) {
    int rounds = g_ks_rounds, per = g_ks_per;
    for (int r = 0; r < rounds; r++) {
        int pids[32];
        int n = 0;
        for (int i = 0; i < per; i++) {
            struct task* t = task_spawn("ks-victim", killstorm_victim);
            if (!t) break;
            pids[n++] = t->pid;
            __atomic_add_fetch(&g_ks_alive, 1, __ATOMIC_RELAXED);
        }
        __atomic_add_fetch(&g_ks_spawned, n, __ATOMIC_RELAXED);
        /* Let them actually reach the blocked state — killing a task that has
         * not parked yet exercises a different (easier) path. */
        task_msleep(6);
        for (int i = 0; i < n; i++) task_kill(pids[i]);
        __atomic_add_fetch(&g_ks_killed, n, __ATOMIC_RELAXED);
        task_msleep(10);
        __atomic_store_n(&g_ks_round, r + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_ks_done, 1, __ATOMIC_RELEASE);
}

static void cmd_killstorm(const char* args) {
    int rounds = 0, per = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) rounds = rounds * 10 + (*args - '0');
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) per = per * 10 + (*args - '0');
    if (rounds <= 0) rounds = 20;
    if (per    <= 0) per    = 8;
    if (per > 32) per = 32;

    g_ks_round = g_ks_done = g_ks_spawned = g_ks_killed = 0;
    g_ks_rounds = rounds; g_ks_per = per;
    kprintf("killstorm: %d rounds x %d blocked tasks (cpus=%d)\n",
            rounds, per, smp_ncpus());
    struct task* drv = task_spawn("ks-driver", killstorm_driver);
    if (!drv) { console_write("killstorm: cannot spawn driver\n"); return; }
    int drvpid = drv->pid;

    /* Poll for completion, and give up on NO PROGRESS rather than on a total
     * time budget: a slow machine finishes late, a broken one stops moving. */
    int last = -1, stuck = 0;
    while (!__atomic_load_n(&g_ks_done, __ATOMIC_ACQUIRE)) {
        task_msleep(10);
        int now = __atomic_load_n(&g_ks_round, __ATOMIC_ACQUIRE);
        if (now == last) { if (++stuck > 400) break; }   /* ~4 s of silence */
        else             { last = now; stuck = 0; }
    }

    int done = __atomic_load_n(&g_ks_done, __ATOMIC_ACQUIRE);
    kprintf("killstorm: %s — %d/%d rounds, %d spawned, %d killed, %d alive\n",
            done ? "done" : "STALLED",
            __atomic_load_n(&g_ks_round, __ATOMIC_ACQUIRE), rounds,
            g_ks_spawned, g_ks_killed,
            __atomic_load_n(&g_ks_alive, __ATOMIC_RELAXED));
    if (!done) {
        /* The whole point of running the storm elsewhere: report what the
         * scheduler was holding when it stopped making progress. */
        console_write("killstorm: task table at the stall —\n");
        task_list();
        task_kill(drvpid);
    }
}

static void rq_audit_print(const char* tag, const struct rq_audit* a, int bad) {
    /* Structural verdict first, because that is the pass/fail one; the two
     * soft counters follow, labelled, so a stale estimate can never be
     * mistaken for a corrupted runqueue. */
    kprintf("%s: %d queued | struct: home %d orphan %d count %d ring %d -> %s"
            " | soft: lost %d load %d\n",
            tag, a->tasks_queued, a->bad_home, a->orphan_home, a->bad_count,
            a->broken_ring, bad ? "VIOLATIONS" : "consistent",
            a->lost, a->bad_load);
}

static void cmd_rqcheck(const char* args) {
    (void)args;
    struct rq_audit a;
    int bad = task_rq_audit(&a);
    rq_audit_print("rqcheck", &a, bad);
}

/* SEVERAL churners, not one.  The first version of this test drove affinity
 * and nice from a single task and reported `ok` even against the PRE-§M57
 * code — because the only thing it could race with was the periodic balancer,
 * which fires every LOAD_BALANCE_INTERVAL_MS, so a 240 ms run offered about
 * two chances to hit a window measured in instructions.  A test that cannot
 * fail is not evidence.
 *
 * Two tasks re-homing the SAME task is the same bug with a window orders of
 * magnitude wider: both read cpu_home, both decide, and the second acts on a
 * queue the first has already moved the task off — which is precisely what
 * "act on the queue an unlocked read named" means.  It is also the realistic
 * case: a shell, the Task Manager and a script can all call taskset. */
static void schedstorm_churner(void) {
    struct task* self = task_current();
    unsigned seed = (unsigned)(self ? self->pid : 1) * 2654435761u;
    uint32_t ncpu = (uint32_t)smp_ncpus();
    while (!__atomic_load_n(&g_ss_stop, __ATOMIC_ACQUIRE) && !task_should_stop()) {
        int n = __atomic_load_n(&g_ss_npids, __ATOMIC_ACQUIRE);
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            int pid = g_ss_pids[i];
            struct task* t = task_find(pid);
            if (!t) continue;
            uint32_t mask = ((seed >> 8) & ((1u << ncpu) - 1u));
            if (!mask) mask = 1u;
            task_set_affinity(t, mask);
            task_set_nice(pid, (int)((seed >> 16) % 21u) - 10);
            __atomic_add_fetch(&g_ss_churn, 2, __ATOMIC_RELAXED);
        }
        task_yield();
    }
    __atomic_sub_fetch(&g_ss_churners, 1, __ATOMIC_ACQ_REL);
}

/* A hog, so the balancer has something worth migrating.  A queue of sleepers
 * is never rebalanced (§M49: load is demand, not queue length), and an
 * unmigrated task cannot expose a migration bug. */
static void schedstorm_hog(void) {
    volatile unsigned x = 0;
    while (!task_should_stop()) { for (int i = 0; i < 20000; i++) x += i; task_yield(); }
}

static void schedstorm_driver(void) {
    int rounds = g_ss_rounds;
    int pids[12];
    int n = 0;
    for (int i = 0; i < 12; i++) {
        struct task* t = task_spawn("ss-hog", schedstorm_hog);
        if (!t) break;
        pids[n++] = t->pid;
        g_ss_pids[i] = t->pid;
    }
    __atomic_store_n(&g_ss_npids, n, __ATOMIC_RELEASE);

    __atomic_store_n(&g_ss_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ss_churners, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < SS_CHURNERS; i++) {
        if (task_spawn("ss-churn", schedstorm_churner))
            __atomic_add_fetch(&g_ss_churners, 1, __ATOMIC_ACQ_REL);
    }

    for (int r = 0; r < rounds; r++) {
        /* One transient task per round, so spawn/exit/reap runs alongside the
         * affinity churn — the reaper's sweep and the balancer's migration are
         * the two things that must not disagree about where a task is. */
        struct task* tmp = task_spawn("ss-tmp", schedstorm_hog);
        if (tmp) { int p = tmp->pid; task_msleep(2); task_kill(p); }

        struct rq_audit a;
        /* task_rq_audit returns the STRUCTURAL count; rules 5 and 6 are
         * reported in `a` and checked at rest below. */
        int hard = task_rq_audit(&a);
        if (hard > __atomic_load_n(&g_ss_worst, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_ss_worst, hard, __ATOMIC_RELAXED);
            /* Keep the WORST SNAPSHOT, not just its size.  "4 violations" does
             * not say which rule, and the rule is the entire diagnosis. */
            g_ss_worst_snap = a;
        }
        task_msleep(4);
    }

    /* Stop the churners and WAIT for them: an affinity call still in flight
     * while the audit is taken at rest would be the transient the "at rest"
     * reading exists to exclude. */
    __atomic_store_n(&g_ss_stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 200 && __atomic_load_n(&g_ss_churners, __ATOMIC_ACQUIRE); i++)
        task_msleep(10);

    for (int i = 0; i < n; i++) {
        struct task* t = task_find(pids[i]);
        if (t) task_set_affinity(t, 0xFFFFFFFFu);
        task_set_nice(pids[i], 0);
    }
    for (int i = 0; i < n; i++) task_kill(pids[i]);
    __atomic_store_n(&g_ss_done, 1, __ATOMIC_RELEASE);
}

static void cmd_schedstorm(const char* args) {
    int rounds = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) rounds = rounds * 10 + (*args - '0');
    if (rounds <= 0) rounds = 60;

    struct rq_audit before;
    int bad0 = task_rq_audit(&before);
    rq_audit_print("schedstorm: before", &before, bad0);

    g_ss_rounds = rounds; g_ss_done = 0; g_ss_worst = 0; g_ss_churn = 0;
    kprintf("schedstorm: %d rounds, %d concurrent churners, %d cpus\n",
            rounds, SS_CHURNERS, smp_ncpus());

    struct task* drv = task_spawn("ss-driver", schedstorm_driver);
    if (!drv) { console_write("schedstorm: cannot spawn driver\n"); return; }

    /* Same no-progress rule as killstorm: a slow box finishes late, a broken
     * one stops moving. */
    int last = -1, stuck = 0;
    while (!__atomic_load_n(&g_ss_done, __ATOMIC_ACQUIRE)) {
        task_msleep(10);
        int now = __atomic_load_n(&g_ss_churn, __ATOMIC_RELAXED);
        if (now == last) { if (++stuck > 800) break; }      /* ~8 s of silence */
        else             { last = now; stuck = 0; }
    }
    int done = __atomic_load_n(&g_ss_done, __ATOMIC_ACQUIRE);

    /* AT REST — every rule must hold now, rule 5 included.  Give the reaper a
     * moment first: a task that has just exited is legitimately not on a queue
     * and not yet freed. */
    task_msleep(200);
    struct rq_audit after;
    int bad1 = task_rq_audit(&after);
    rq_audit_print("schedstorm: after ", &after, bad1);

    int worst = __atomic_load_n(&g_ss_worst, __ATOMIC_RELAXED);
    if (worst) rq_audit_print("schedstorm: worst ", &g_ss_worst_snap, worst);
    kprintf("schedstorm: %s — %d churn ops, worst structural mid-churn %d\n",
            done ? "done" : "STALLED",
            __atomic_load_n(&g_ss_churn, __ATOMIC_RELAXED), worst);
    /* Pass = structurally perfect THROUGHOUT, and everything (rules 5 and 6
     * included) exact once the churn has stopped. */
    int pass = done && worst == 0 && bad1 == 0 && after.lost == 0 && after.bad_load == 0;
    kprintf("schedstorm: %s\n", pass ? "ok" : "FAIL");
}

static void loop_hog_main(void) {
    volatile uint32_t counter = 0;
    /* Deliberately no yield (that is the point of the preemption test),
     * but M22.3 adds the kthread contract: CPU-bound kernel threads
     * MUST poll task_should_stop() so `kill` / the task manager can
     * terminate them — same rule as Linux kthread_should_stop(). */
    while (!loop_stop_flag && !task_should_stop()) {
        counter++;
    }
}

/* `kill <pid>` — cooperative task termination (M22.3).  The victim
 * dies at its next yield point / task_should_stop() poll; reaping is
 * lazy (the GUI window teardown reaps its own shells; CLI kills stay
 * as DEAD entries in `ps` until something reaps them — good enough
 * for a teaching kernel, and visible state is a feature here). */
/* §M72 — a pid, or the NAME of exactly one live task.  A name matching two
 * tasks is refused rather than guessed: killing the wrong one of two is worse
 * than asking for the number. */
struct name_find { const char* name; int pid; int hits; };
static void name_find_cb(const struct task* t, int cur, void* c) {
    (void)cur;
    struct name_find* f = (struct name_find*)c;
    if (t->state == TASK_DEAD) return;
    int i = 0;
    while (f->name[i] && t->name[i] == f->name[i]) i++;
    if (f->name[i] == 0 && t->name[i] == 0) { f->pid = t->pid; f->hits++; }
}
static int resolve_pid(const char* args, const char* verb, int* out) {
    int pid = 0, any = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) { pid = pid * 10 + (*args - '0'); any = 1; }
    if (any) { *out = pid; return 0; }
    if (!*args) { kprintf("%s: usage: %s <pid|name>\n", verb, verb); return -1; }
    char nm[TASK_NAME_MAX + 1];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < TASK_NAME_MAX) { nm[i] = args[i]; i++; }
    nm[i] = 0;
    struct name_find f = { nm, -1, 0 };
    task_for_each(name_find_cb, &f);
    if (f.hits == 1) { *out = f.pid; return 0; }
    if (f.hits == 0) kprintf("%s: no task named '%s'\n", verb, nm);
    else             kprintf("%s: %d tasks are named '%s' - give the pid\n", verb, f.hits, nm);
    return -1;
}

static void cmd_kill(const char* args) {
    int pid = 0;
    if (resolve_pid(args, "kill", &pid) != 0) return;
    if (task_current() && task_current()->pid == pid) {
        console_write("kill: refusing to kill the calling shell\n");
        return;
    }
    int r = task_kill(pid);
    if (r == 0)       kprintf("kill: pid %d flagged (dies at next yield)\n", pid);
    else if (r == -2) kprintf("kill: pid %d belongs to somebody else\n", pid);
    else              kprintf("kill: pid %d not found or protected\n", pid);
}

/* §M46 — force-kill: reclaims a WEDGED ring-3 task (one spinning in userland
 * that never reaches a cooperative yield, so plain `kill` can't touch it).  It
 * dies at its next timer preemption in user mode. */
static void cmd_fkill(const char* args) {
    int pid = 0;
    if (resolve_pid(args, "fkill", &pid) != 0) return;
    if (task_current() && task_current()->pid == pid) {
        console_write("fkill: refusing to kill the calling shell\n");
        return;
    }
    int fr = task_force_kill(pid);
    if (fr == -2) { kprintf("fkill: pid %d belongs to somebody else\n", pid); return; }
    if (fr == 0) kprintf("fkill: pid %d force-killed\n", pid);
    else                          kprintf("fkill: pid %d not found or protected\n", pid);
}

/* §M46 — spawn the WEDGE test app: a ring-3 task that spins forever without ever
 * yielding.  `kill` (cooperative) cannot reclaim it; `fkill` (force) can. */
extern const unsigned char _binary_user_wedge_elf_start[]   __attribute__((weak));
extern const unsigned char _binary_user_wedge_elf_end[]     __attribute__((weak));
extern const unsigned char _binary_user_wedge_x86_64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wedge_x86_64_elf_end[]   __attribute__((weak));
static void cmd_wedge(void) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_wedge_elf_start)        { s = _binary_user_wedge_elf_start;   e = _binary_user_wedge_elf_end; }
    else if (_binary_user_wedge_x86_64_elf_start) { s = _binary_user_wedge_x86_64_elf_start; e = _binary_user_wedge_x86_64_elf_end; }
    if (!s || !e) { console_write("wedge: no wedge ELF embedded for this arch\n"); return; }
    int pid = proc_spawn("wedge", s, (size_t)(e - s));
    if (pid < 0) { console_write("wedge: spawn failed\n"); return; }
    /* §M46 — apply the runaway auto-fkill policy (off unless configured), so a
     * `package.auto_fkill_ms` setting makes the watchdog reclaim this hog on its
     * own; with no config it stays wedged for manual `fkill` testing. */
    long ms = config_get_long("package.wedge.auto_fkill_ms",
                              config_get_long("package.auto_fkill_ms", 0));
    if (ms > 0) task_set_auto_fkill(pid, (uint32_t)ms);
    kprintf("wedge: spawned a WEDGED ring-3 task (pid %d) — `kill %d` can't stop it, `fkill %d` can%s\n",
            pid, pid, pid, ms > 0 ? " (auto-fkill armed)" : "");
}

/* `loop [n]` — spawn n CPU hogs (default 1).  The count exists for §M49:
 * one hog cannot show a load-distribution problem, because there is
 * nothing to distribute.  `loop 8` on a 4-CPU box is the shape that makes
 * an imbalance visible under `sched`. */
static void cmd_loop(const char* args) {
    while (*args == ' ') args++;
    int count = 0;
    for (; *args >= '0' && *args <= '9'; args++) count = count * 10 + (*args - '0');
    if (count <= 0) count = 1;
    if (count > 32) count = 32;             /* a shell typo shouldn't OOM the box */
    loop_stop_flag = 0;
    int spawned = 0;
    for (int i = 0; i < count; i++) {
        struct task* t = task_spawn("cpu-hog", loop_hog_main);
        if (!t) break;
        spawned++;
    }
    if (!spawned) { console_write("loop: spawn failed (OOM?)\n"); return; }
    kprintf("loop: spawned %d cpu-hog(s) — should NOT freeze the shell; "
            "`sched` to see the spread, `loopstop` to stop them all\n", spawned);
}

/* `loopstop` — release every cpu-hog spawned by `loop`.  They poll the
 * flag, so they exit at their next iteration.  Without this, measuring
 * twice in one boot means measuring the first run's leftovers too. */
static void cmd_loopstop(void) {
    loop_stop_flag = 1;
    console_write("loopstop: all cpu-hogs asked to exit\n");
}

static void cmd_lscpu(void) {
    int n = smp_ncpus();
    int me = this_cpu_id();
    kprintf("CPU  APIC_ID  NODE  STATE   RQ\n");
    for (int i = 0; i < n; i++) {
        struct percpu* p = percpu_at(i);
        if (!p) continue;
        kprintf("%d    %u        %d     %s   %d%s\n",
                i, p->apic_id, p->numa_node,
                p->online ? "online " : "offline",
                p->rq_count,
                (i == me) ? " <this>" : "");
    }
}

static void sched_snap_one(const struct task* t, int is_current, void* ctx) {
    (void)is_current;   /* only true for the CALLING task — useless here */
    struct sched_snap* s = (struct sched_snap*)ctx;
    if (s->n >= SCHED_SNAP_MAX) return;
    /* Idle tasks would each report ~100% of their core and drown the real
     * tasks out; per-CPU BUSY% already says how idle a core is. */
    if (t->is_idle) return;
    struct sched_snap_task* e = &s->t[s->n++];
    e->pid      = t->pid;
    /* Same switch-boundary gap as the per-CPU busy counter: a task that is
     * on a CPU right now has not been credited for the slice it is in the
     * middle of.  Find out whether it is running anywhere and add it. */
    e->cpu_ms   = t->cpu_ms;
    for (int c = 0; c < smp_ncpus() && c < 32; c++) {
        struct percpu* p = percpu_at(c);
        if (p && p->current == t) {
            if (s->now > t->sched_in_ms) e->cpu_ms += s->now - t->sched_in_ms;
            break;
        }
    }
    e->cpu_home = t->cpu_home;
    e->demand   = t->demand;
    e->nice     = t->nice;
    int i = 0;
    for (; i < (int)sizeof(e->name) - 1 && t->name[i]; i++) e->name[i] = t->name[i];
    e->name[i] = '\0';
}

static void cmd_sched(const char* args) {
    while (*args == ' ') args++;
    uint32_t window = 0;
    for (; *args >= '0' && *args <= '9'; args++) window = window * 10 + (uint32_t)(*args - '0');
    if (window < 100)   window = 1000;      /* too short to measure anything */
    if (window > 10000) window = 10000;

    int n = smp_ncpus();
    /* Snapshot A. */
    static uint64_t busy0[32], sw0[32], mig0[32];
    /* `busy_ms` is only credited at a context switch, so a task that
     * monopolises a core without ever being switched out contributes
     * NOTHING to it — the first version of this command reported a core
     * running one busy task at 0%.  Add the in-flight slice at both
     * snapshots to close that gap. */
    #define BUSY_NOW(p, now) \
        ((p)->busy_ms + (((p)->current && !(p)->current->is_idle && \
                          (now) > (p)->current->sched_in_ms) \
                         ? (now) - (p)->current->sched_in_ms : 0))
    struct sched_snap* a = (struct sched_snap*)kmalloc(sizeof *a);
    struct sched_snap* b = (struct sched_snap*)kmalloc(sizeof *b);
    if (!a || !b) { console_write("sched: OOM\n"); kfree(a); kfree(b); return; }
    a->n = b->n = 0;
    uint64_t t0 = timer_ticks_ms();
    a->now = t0;
    for (int i = 0; i < n && i < 32; i++) {
        struct percpu* p = percpu_at(i);
        busy0[i] = p ? BUSY_NOW(p, t0)  : 0;
        sw0[i]   = p ? p->switches      : 0;
        mig0[i]  = p ? p->migrations    : 0;
    }
    task_for_each(sched_snap_one, a);

    kprintf("sched: sampling %u ms across %d CPU(s)...\n", window, n);
    task_msleep(window);

    uint64_t t1 = timer_ticks_ms();
    b->now = t1;
    uint64_t elapsed = t1 - t0;
    if (elapsed == 0) elapsed = 1;          /* never divide by a stopped clock */
    task_for_each(sched_snap_one, b);

    kprintf("CPU  RQ  LOAD  BUSY%%  SWITCH  MIGR  CURRENT\n");
    unsigned lo = 100, hi = 0;
    int rq_lo = 1 << 30, rq_hi = 0;
    int ld_lo = 1 << 30, ld_hi = 0;
    for (int i = 0; i < n && i < 32; i++) {
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) continue;
        uint64_t bnow = BUSY_NOW(p, t1);
        uint64_t db = (bnow > busy0[i]) ? bnow - busy0[i] : 0;
        if (db > elapsed) db = elapsed;     /* clamp a torn/racy sample */
        unsigned pct = (unsigned)((db * 100) / elapsed);
        if (pct < lo) lo = pct;
        if (pct > hi) hi = pct;
        if (p->rq_count < rq_lo) rq_lo = p->rq_count;
        if (p->rq_count > rq_hi) rq_hi = p->rq_count;
        if (p->rq_load < ld_lo) ld_lo = p->rq_load;
        if (p->rq_load > ld_hi) ld_hi = p->rq_load;
        kprintf("%d    %d   %d   %u      %u       %u     %s\n",
                i, p->rq_count, p->rq_load, pct,
                (unsigned)(p->switches   - sw0[i]),
                (unsigned)(p->migrations - mig0[i]),
                (p->current && p->current->name) ? p->current->name : "?");
    }
    if (rq_lo > rq_hi) rq_lo = rq_hi = 0;
    if (ld_lo > ld_hi) ld_lo = ld_hi = 0;
    /* LOAD spread is the number that matters: it is what the balancer
     * equalises, and unlike BUSY% it stays informative when every core is
     * saturated (four hogs and one hog are both 100% busy). */
    kprintf("spread: load %d..%d (delta %d), busy %u%%..%u%% (delta %u), rq %d..%d\n",
            ld_lo, ld_hi, ld_hi - ld_lo, lo, hi, hi - lo, rq_lo, rq_hi);

    /* Per-task CPU time consumed during the window, and where it ran.
     * This is the line that shows an imbalance as a human sees it: two
     * hogs at 99% and two at 1% is a scheduling failure no aggregate
     * per-CPU number makes obvious. */
    /* DEM is the balancer's own view of the task (how much CPU it WANTS);
     * CPU%% is what it actually got.  The two diverging is the signature
     * of contention: a hog reads DEM 100 and CPU%% 25 on a crowded core. */
    kprintf("PID  CPU  NI  DEM  CPU%%  NAME\n");
    for (int i = 0; i < b->n; i++) {
        uint64_t before = 0;
        int seen = 0;
        for (int j = 0; j < a->n; j++)
            if (a->t[j].pid == b->t[i].pid) { before = a->t[j].cpu_ms; seen = 1; break; }
        uint64_t used = seen ? (b->t[i].cpu_ms - before) : b->t[i].cpu_ms;
        if (used > elapsed) used = elapsed;
        unsigned pct = (unsigned)((used * 100) / elapsed);
        if (pct == 0) continue;             /* idle/blocked tasks: not the story */
        kprintf("%d    %d    %d   %u   %u     %s%s\n",
                b->t[i].pid, b->t[i].cpu_home, b->t[i].nice,
                b->t[i].demand, pct, b->t[i].name, seen ? "" : " (new)");
    }
    kfree(a);
    kfree(b);
    #undef BUSY_NOW
}

static void wqtest_fn(struct work* w) {
    struct wqtest_item* it = (struct wqtest_item*)w;   /* w is the first member */
    /* Busy for a few ms — long enough that concurrent items overlap, which
     * is what makes the CPU spread meaningful. */
    uint64_t end = timer_ticks_ms() + 5;
    while (timer_ticks_ms() < end) { /* spin */ }
    it->cpu = this_cpu_id();
    it->ran++;
}

static void cmd_wqtest(const char* args) {
    while (*args == ' ') args++;
    int n = 0;
    for (; *args >= '0' && *args <= '9'; args++) n = n * 10 + (*args - '0');
    if (n <= 0) n = 8;
    if (n > WQTEST_MAX) n = WQTEST_MAX;

    int workers = 0, pending = 0;
    uint64_t done0 = 0;
    workqueue_stats(&workers, &pending, &done0);
    if (workers == 0) { console_write("wqtest: no workers (pool not up)\n"); return; }

    for (int i = 0; i < n; i++) {
        wqt[i].idx = i;
        wqt[i].ran = 0;
        wqt[i].cpu = -1;
        work_init(&wqt[i].w, wqtest_fn);
    }

    kprintf("wqtest: submitting %d items to %d worker(s)...\n", n, workers);
    uint64_t t0 = timer_ticks_ms();
    for (int i = 0; i < n; i++) work_submit(&wqt[i].w);
    /* Re-submitting the same items immediately must NOT double them. */
    for (int i = 0; i < n; i++) work_submit(&wqt[i].w);
    work_flush();
    uint64_t elapsed = timer_ticks_ms() - t0;

    int ran = 0, extra = 0, runs = 0;
    int per_cpu[32];
    for (int i = 0; i < 32; i++) per_cpu[i] = 0;
    for (int i = 0; i < n; i++) {
        runs += wqt[i].ran;
        if (wqt[i].ran >= 1) ran++;
        if (wqt[i].ran >  1) extra++;      /* re-queued while running — legal */
        if (wqt[i].cpu >= 0 && wqt[i].cpu < 32) per_cpu[wqt[i].cpu]++;
    }
    int cpus_used = 0;
    for (int i = 0; i < 32; i++) if (per_cpu[i]) cpus_used++;

    kprintf("wqtest: %d/%d items ran in %u ms across %d CPU(s):", ran, n,
            (unsigned)elapsed, cpus_used);
    for (int i = 0; i < smp_ncpus() && i < 32; i++) kprintf(" cpu%d=%d", i, per_cpu[i]);
    kprintf("\n");

    uint64_t done1 = 0;
    workqueue_stats(&workers, &pending, &done1);
    /* Serial work would take n*5 ms; real overlap should beat that
     * noticeably once there is more than one worker. */
    kprintf("wqtest: completed counter +%u, still pending %d, serial would be ~%d ms\n",
            (unsigned)(done1 - done0), pending, n * 5);
    /* `extra` is NOT a failure.  The second submit loop collapses into the
     * first only for items still WAITING; an item a worker has already
     * picked up is legitimately queued again, which is exactly how a
     * driver says "more arrived while you were draining".  The first
     * version of this test asserted zero duplicates and failed against its
     * own documented contract — the assertion was wrong, not the queue.
     * What must hold: every item ran, nothing was left pending after the
     * flush, and the completion counter agrees with the runs observed. */
    /* The completion counter is GLOBAL, so it may exceed our own runs: the
     * xHCI event-ring drain submits work from the timer tick, and any
     * future consumer will too.  This check originally demanded exact
     * equality and started failing the moment the queue got a real
     * production user — the counter read +12 against 11 of our runs, the
     * difference being one USB drain.  `>=` is what was actually meant. */
    int ok = (ran == n) && (pending == 0) && (done1 - done0 >= (uint64_t)runs);
    if (ok)
        kprintf("wqtest: PASS (all %d ran, %d re-queued while running, "
                "flush drained everything)\n", n, extra);
    else
        kprintf("wqtest: FAIL (ran=%d/%d, runs=%d, counter=+%u, pending=%d)\n",
                ran, n, runs, (unsigned)(done1 - done0), pending);
    if (done1 - done0 > (uint64_t)runs)
        kprintf("wqtest: (+%u completions from other submitters — the xHCI "
                "drain runs on this pool)\n",
                (unsigned)(done1 - done0 - (uint64_t)runs));
}

/* `nice <pid> <value>` — scheduling priority, -20 (strongest) .. +19
 * (weakest), 0 default (§M49).  Unprivileged in both directions: d-os has
 * no user model yet (§M32), so there is nobody to protect the setting
 * from. */
static void cmd_nice(const char* args) {
    while (*args == ' ') args++;
    int pid = 0, any = 0;
    for (; *args >= '0' && *args <= '9'; args++) { pid = pid * 10 + (*args - '0'); any = 1; }
    if (!any) { console_write("nice: usage: nice <pid> <-20..19>\n"); return; }
    while (*args == ' ') args++;
    int neg = 0;
    if (*args == '-') { neg = 1; args++; }
    else if (*args == '+') args++;
    int val = 0, hasval = 0;
    for (; *args >= '0' && *args <= '9'; args++) { val = val * 10 + (*args - '0'); hasval = 1; }
    if (!hasval) { console_write("nice: missing value (-20..19)\n"); return; }
    if (neg) val = -val;
    /* §M32 — a refusal and a missing task are DIFFERENT answers.  This line
     * used to print "no task with pid" for every non-zero return, which was
     * harmless while the only failure was a bad pid and becomes a lie the
     * moment a permission check can fail: the user goes looking for the wrong
     * problem, which is the failure mode this milestone keeps closing. */
    int nr = task_set_nice(pid, val);
    if (nr == -1) { kprintf("nice: no task with pid %u\n", pid); return; }
    if (nr == -2) { console_write("nice: refused — raising a priority needs an "
                                  "administrator, and the task must be yours\n"); return; }
    struct task* t = task_find(pid);
    kprintf("nice: pid %d nice=%d weight=%u (%u%% of a default task's share)\n",
            pid, t ? t->nice : val, t ? t->weight : 0, t ? t->weight : 0);
}

static void cmd_taskset(const char* args) {
    if (!args || !*args) {
        console_write("taskset: usage: taskset <pid> <hex-mask>  (e.g. taskset 5 0x2)\n");
        return;
    }
    /* Parse pid (decimal), then mask (hex, may have 0x prefix). */
    char pid_buf[16];
    int pi = 0;
    while (*args && *args != ' ' && pi < (int)sizeof pid_buf - 1) {
        pid_buf[pi++] = *args++;
    }
    pid_buf[pi] = 0;
    while (*args == ' ') args++;
    if (!*args) {
        console_write("taskset: missing mask\n");
        return;
    }
    uint32_t pid;
    if (cmd_parse_uint(pid_buf, &pid) != 0) {
        kprintf("taskset: bad pid '%s'\n", pid_buf);
        return;
    }
    uint32_t mask;
    if (cmd_parse_hex(args, &mask) != 0) {
        kprintf("taskset: bad mask '%s'\n", args);
        return;
    }
    struct task* t = task_find((int)pid);
    if (!t) {
        kprintf("taskset: no task with pid %u\n", pid);
        return;
    }
    if (task_set_affinity(t, mask) != 0) {
        kprintf("taskset: rejected (mask=0 is not allowed)\n");
        return;
    }
    kprintf("taskset: pid %u mask now 0x%x (home cpu=%d)\n",
            pid, mask, t->cpu_home);
}

/* --- registrations --------------------------------------------------------- */

static void tk_ps       (const char* a) { (void)a; task_list();  }
static void tk_yield    (const char* a) { (void)a; task_yield(); }
static void tk_loopstop (const char* a) { (void)a; cmd_loopstop(); }
static void tk_spawn    (const char* a) { (void)a; cmd_spawn();  }
static void tk_wedge    (const char* a) { (void)a; cmd_wedge();  }
static void tk_lscpu    (const char* a) { (void)a; cmd_lscpu();  }

SHELL_CMD(ps)         = { "ps", "", "running tasks, as a process tree",
                          SHELL_G_TASK, tk_ps, SHELL_P_ANY };
SHELL_CMD(yield)      = { "yield", "", "give up the rest of this quantum",
                          SHELL_G_TASK, tk_yield, SHELL_P_ANY };
/* §M32 — kill and fkill are SHELL_P_ANY, and that is a CORRECTION of this
 * milestone's own first sweep, which marked them ADMIN for being dangerous.
 *
 * Their scope is not a rank, it is OWNERSHIP: `task_kill` refuses a task that
 * belongs to somebody else (may_signal in task.c), so a limited user may end
 * their own work and nobody else's.  Marking the verb ADMIN would have made
 * that check unreachable for exactly the people it was written for — a rule
 * derived from "is this dangerous" instead of "whose is it". */
SHELL_CMD(kill)       = { "kill", "<pid|name>", "ask a task to exit",
                          SHELL_G_TASK, cmd_kill, SHELL_P_ANY };
SHELL_CMD(fkill)      = { "fkill", "<pid|name>", "force-kill a wedged ring-3 task",
                          SHELL_G_TASK, cmd_fkill, SHELL_P_ANY };
/* §M32 — ANY, for the same reason as `kill`, and it is the third correction of
 * the same misjudgement: this verb's scope is the ARGUMENT's sign and the
 * task's owner, both of which task_set_nice checks.  Marking it ADMIN would
 * put the asymmetric rule (lowering is anybody's, raising is not) behind a
 * gate that already answered no. */
SHELL_CMD(nice)       = { "nice", "<pid> <-20..19>", "scheduling priority",
                          SHELL_G_TASK, cmd_nice, SHELL_P_ANY };
SHELL_CMD(taskset)    = { "taskset", "<pid> <hexmask>", "pin a task to a CPU set",
                          SHELL_G_TASK, cmd_taskset, SHELL_P_ADMIN };
SHELL_CMD(sched)      = { "sched", "[ms]", "how work is spread over the CPUs",
                          SHELL_G_TASK, cmd_sched, SHELL_P_ANY };
SHELL_CMD(lscpu)      = { "lscpu", "", "CPUs, topology and NUMA nodes",
                          SHELL_G_TASK, tk_lscpu, SHELL_P_ANY };
SHELL_CMD(spawn)      = { "spawn", "", "spawn a ticker task",
                          SHELL_G_TASK, tk_spawn, SHELL_P_ADMIN };
SHELL_CMD(loop)       = { "loop", "[n]", "spawn n CPU hogs",
                          SHELL_G_TASK, cmd_loop, SHELL_P_ADMIN };
SHELL_CMD(loopstop)   = { "loopstop", "", "stop the hogs `loop` started",
                          SHELL_G_TASK, tk_loopstop, SHELL_P_ADMIN };

/* Deliberately NOT hidden: these are the falsifiers, and a test nobody can
 * find is a test nobody runs. */
SHELL_CMD(killstorm)  = { "killstorm", "[rounds] [tasks]",
                          "kill blocked tasks under SMP, repeatedly",
                          SHELL_G_TEST, cmd_killstorm, SHELL_P_ADMIN };
SHELL_CMD(rqcheck)    = { "rqcheck", "", "check the runqueue invariant now",
                          SHELL_G_TEST, cmd_rqcheck, SHELL_P_ANY };
SHELL_CMD(schedstorm) = { "schedstorm", "[rounds]", "affinity + nice churn, audited",
                          SHELL_G_TEST, cmd_schedstorm, SHELL_P_ADMIN };
SHELL_CMD(wqtest)     = { "wqtest", "[n]", "deferred-work pool over every CPU",
                          SHELL_G_TEST, cmd_wqtest, SHELL_P_ANY };

/* `wedge` runs away in ring 3 on purpose so `fkill` has something to kill.
 * Not hidden — the point of it is to be reachable. */
SHELL_CMD(wedge)      = { "wedge", "", "start a runaway ring-3 task",
                          SHELL_G_TEST, tk_wedge, SHELL_P_ADMIN };
