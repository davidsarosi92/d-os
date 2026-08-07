/* =============================================================================
 * task.c — kernel task table + per-CPU preemptive round-robin scheduler.
 *
 * Two data structures.
 *
 *   1. Master task list — circular singly-linked list of every alive
 *      (non-DEAD) task, rooted at `master_head` and threaded via
 *      `task->next`.  Used by ps / task_for_each / task_find — i.e.
 *      iteration paths that don't care which CPU a task is on.  The
 *      master list is protected by `master_lock`.
 *
 *   2. Per-CPU runqueues — one intrusive doubly-linked list per CPU,
 *      rooted at `percpu->rq_head` and threaded via
 *      `task->rq_next/rq_prev`.  ONLY RUNNABLE non-idle tasks live on
 *      a runqueue, and a task is on AT MOST one runqueue at a time
 *      (which CPU is recorded in task->cpu_home).  Idle tasks are
 *      not on any runqueue — each CPU's idle is reached via
 *      percpu->idle.  Each per-CPU runqueue is protected by its
 *      own `percpu->rq_lock`.
 *
 * Lock ordering (when both are needed):  master_lock outer, rq_lock
 * inner.  Most fast-paths only need rq_lock for this_cpu().
 *
 * ---------------------------------------------------------------------
 * Scheduling sketch (per-CPU, M18.6.1)
 * ---------------------------------------------------------------------
 *
 *   schedule() runs in the context of THIS CPU only.  It looks at the
 *   local rq head, picks the first task that:
 *     - is RUNNABLE
 *     - has this_cpu_id in its cpu_mask (M18.6.3 affinity)
 *     - is not currently `current` on some other CPU (safety net for
 *       a corner case where the load balancer just stole the task)
 *   and rotates it to the tail (round-robin).  Falls back to the
 *   per-CPU idle if no candidate exists.  Worst case is O(rq_len) per
 *   pick instead of O(ntasks * ncpus) under the global lock — and the
 *   common case is O(1) when the head matches.
 *
 *   Load balance has TWO triggers (§M49):
 *     - this CPU's rq ran empty: steal any runnable task from the
 *       longest peer queue rather than idle;
 *     - every LOAD_BALANCE_INTERVAL_MS on the local tick: pull from a
 *       peer that is at least LOAD_BALANCE_MIN_DELTA tasks busier.
 *   Only the first existed before §M49, and it is a work-stealing rule,
 *   not a load-distribution one — with every queue non-empty an
 *   arbitrarily bad split never corrected itself.  See
 *   load_balance_periodic for the measurement and the constants.
 *
 *   (This comment previously described the periodic pass as if it were
 *   implemented, naming a LOAD_BALANCE_INTERVAL_MS that did not exist
 *   anywhere in the tree.  It does now.)
 *
 * ---------------------------------------------------------------------
 * Brand-new task wiring (unchanged from M18)
 * ---------------------------------------------------------------------
 *
 *   task_spawn pre-builds the kernel stack so the first context_switch
 *   `ret` lands at task_trampoline (arch-specific), which releases the
 *   runqueue lock (held by the spawning schedule()) and then sti's
 *   before calling the entry.  See hal/<arch>/task_arch.c.
 *
 *   On the per-CPU rq world, the lock that the trampoline releases is
 *   this_cpu()'s rq_lock — the same lock the schedule() that picked
 *   the brand-new task was holding.
 *
 * ---------------------------------------------------------------------
 * Deferred reschedule
 * ---------------------------------------------------------------------
 *
 *   `need_resched` is one bit per CPU (lives in percpu).  Timer IRQ
 *   sets the local flag; IRQ exit consults it.  This makes a remote
 *   schedule_request via lapic IPI (vector 0x41) safe — it'll set the
 *   right CPU's flag.
 *
 * Bootstrap: pid 0 = the original kernel_main flow.  We synthesize its
 * `struct task` without allocating a stack; esp is set by the first
 * context_switch that swaps away from it.
 * ============================================================================= */

#include "task.h"
#include "syscall.h"   /* sys_futex — CLONE_CHILD_CLEARTID wake */
#include "kmalloc.h"
#include "printf.h"
#include "lock.h"
#include "hal_api.h"
#include "percpu.h"
#include "smp.h"
#include "timer.h"          /* M22.3: per-task CPU-time accounting */
#include "vmm.h"            /* M25: per-process address-space switch */
#include "waitq.h"
#include "crash.h"      /* §M47 — record a forced kill */          /* Tier A.1: block/wake wait-queue primitive */
#include <stddef.h>
#include <stdint.h>

extern void context_switch(uintptr_t* save_esp_to, uintptr_t new_esp);

/* ------------------------------------------------------------------- */
/* Master list state.                                                   */
/* ------------------------------------------------------------------- */

static struct task* master_head = NULL;   /* circular SLL of all alive tasks */
static int          next_pid    = 0;
static spinlock_t   master_lock = SPINLOCK_INIT;

/* M27 — pid of the init/reaper task (0 until task_start_init runs).
 * Orphans re-parent here; the reaper loop runs under this pid. */
static int          g_init_pid  = 0;

/* Tier A.2 — child-exit wait-queue.  A task blocked in task_wait() parks
 * here; task_exit_code() wakes every waiter after marking itself DEAD, so a
 * parent notices a child's death without polling.  One global queue (not
 * per-parent) keeps it simple — a waiter just re-scans its own children on
 * wake, which is cheap (task counts are small) and correct (a spurious wake
 * from an unrelated exit just re-blocks). */
static struct waitq child_exit_wq = WAITQ_INIT;

/* ------------------------------------------------------------------- */
/* Helpers.                                                             */
/* ------------------------------------------------------------------- */

static void str_copy_n(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static const char* state_name(enum task_state s) {
    switch (s) {
        case TASK_RUNNABLE: return "RUN";
        case TASK_SLEEPING: return "SLP";
        case TASK_DEAD:     return "DEAD";
    }
    return "?";
}

/* Master-list insertion — caller holds master_lock. */
static void master_insert_locked(struct task* t) {
    if (!master_head) {
        t->next     = t;
        master_head = t;
    } else {
        t->next            = master_head->next;
        master_head->next  = t;
    }
}

/* ------------------------------------------------------------------- */
/* Per-CPU runqueue plumbing.                                           */
/* ------------------------------------------------------------------- */

/* §M49 — load-balancer tuning, kept in one block so the policy can be
 * read without hunting through the code that applies it.  Each constant
 * is justified where it is used (load_balance_periodic, task_refresh_demand). */
#define LOAD_BALANCE_INTERVAL_MS 100   /* how often a CPU re-examines the spread */
#define LOAD_BALANCE_MIN_DELTA    25   /* demand units of imbalance worth a migration */
#define LOAD_BALANCE_MAX_STEAL     4   /* migrations per pass, bounding IRQ-off time */
#define LOAD_SAMPLE_MIN_MS        50   /* shortest window that yields a usable demand */
#define SLEEP_SWEEP_BATCH         16   /* timed sleepers woken per tick sweep */

/* §M49 — nice → weight.  A coarse table rather than a formula: eight
 * steps across the range are plenty for a kernel whose whole priority
 * requirement today is "the desktop should outrank a batch job", and a
 * table makes the ratios inspectable instead of implied.  Each step is
 * roughly 1.5x, so nice -20 gets about ten times the CPU of nice 0 and
 * nice +19 about a tenth. */
uint32_t task_nice_to_weight(int nice) {
    static const uint32_t tbl[] = {
        /* nice -20..-15 */ 1000, 1000, 1000, 700, 700, 700,
        /* nice -14.. -9 */  500,  500,  500, 350, 350, 350,
        /* nice  -8.. -3 */  250,  250,  250, 175, 175, 175,
        /* nice  -2..  0 */  150,  120, TASK_WEIGHT_BASE,
        /* nice   1..  6 */   80,   65,   55,  45,  36,  30,
        /* nice   7.. 12 */   25,   20,   17,  14,  12,  10,
        /* nice  13.. 19 */    9,    8,    7,   6,   5,   4, 3,
    };
    if (nice < TASK_NICE_MIN) nice = TASK_NICE_MIN;
    if (nice > TASK_NICE_MAX) nice = TASK_NICE_MAX;
    return tbl[nice - TASK_NICE_MIN];
}

/* §M49 — timed-sleep bookkeeping (see task_msleep / sleep_sweep).  The
 * counter exists purely so the per-tick sweep can return immediately when
 * nothing is sleeping on a clock. */
static volatile int g_timed_sleepers = 0;
static uint64_t     g_sleep_sweep_ms = 0;

/* Weighted load contribution of one task — defined with the demand code
 * below, but needed by the runqueue insert/remove helpers above it. */
static int task_load_contrib(const struct task* t);
static void task_refresh_demand(struct task* t, uint64_t now);

/* §M49 — scheduler-visible defaults for a freshly built task.
 *
 * This exists because FOUR places construct a `struct task` and only one
 * of them is `spawn_common`: pid 0, the BSP idle task and each AP's idle
 * task are all synthesised by hand.  Initialising the new weight fields
 * in spawn_common alone left the other three at kcalloc's zero, and a
 * weight of zero is a DIVIDE BY ZERO in the round-robin replenish — the
 * kernel took a #DE before the scheduler had run once.  One initialiser
 * that every construction site calls is the fix; the alternative is
 * remembering four places forever, and the next field would break it
 * again the same way. */
static void task_sched_defaults(struct task* t) {
    t->nice       = 0;
    t->weight     = TASK_WEIGHT_BASE;
    t->deficit    = (int)TASK_WEIGHT_BASE;
    t->skips_left = 0;
    t->demand           = TASK_DEMAND_MAX;
    t->demand_sample_ms = timer_ticks_ms();
}

/* Insert at tail of rq.  Caller holds rq->rq_lock; task->cpu_home is
 * the caller-set destination CPU index.  Idempotent: re-enqueueing
 * an already-on-this-rq task is a no-op (defensive — shouldn't
 * happen if state transitions are right). */
static void rq_insert_tail_locked(struct percpu* rq, struct task* t) {
    if (t->rq_next || t->rq_prev || rq->rq_head == t) return;
    if (!rq->rq_head) {
        rq->rq_head = t;
        t->rq_next  = t;
        t->rq_prev  = t;
    } else {
        struct task* head = rq->rq_head;
        struct task* tail = head->rq_prev;
        t->rq_next     = head;
        t->rq_prev     = tail;
        tail->rq_next  = t;
        head->rq_prev  = t;
    }
    rq->rq_count++;
    /* §M49 — the task starts accruing runnable time here (running or
     * merely waiting: both are "wants CPU"), and its demand joins this
     * queue's load. */
    t->runnable_since = timer_ticks_ms();
    rq->rq_load += task_load_contrib(t);
}

/* Remove a task from its current rq.  Caller holds the OWNING rq's
 * lock (= percpu_at(t->cpu_home)->rq_lock).  After return, t's
 * rq_next/rq_prev are NULL and t is no longer on any runqueue. */
static void rq_remove_locked(struct percpu* rq, struct task* t) {
    if (!t->rq_next && !t->rq_prev && rq->rq_head != t) return;
    if (rq->rq_head == t) {
        if (t->rq_next == t) {
            /* Single-element rq. */
            rq->rq_head = NULL;
        } else {
            rq->rq_head = t->rq_next;
        }
    }
    t->rq_prev->rq_next = t->rq_next;
    t->rq_next->rq_prev = t->rq_prev;
    t->rq_next = NULL;
    t->rq_prev = NULL;
    if (rq->rq_count > 0) rq->rq_count--;
    /* §M49 — close the runnable-time stay and withdraw the demand we
     * contributed.  Subtracting the SAME value we added is what keeps
     * rq_load consistent: `demand` only ever changes in the refresh
     * pass, which runs under this same lock while the task is queued. */
    {
        uint64_t now = timer_ticks_ms();
        if (now > t->runnable_since) t->runnable_acc += now - t->runnable_since;
        t->runnable_since = now;
    }
    rq->rq_load -= task_load_contrib(t);
    if (rq->rq_load < 0) rq->rq_load = 0;
    /* §M49 — take a demand sample on the way OUT too.  The periodic
     * refresh only walks tasks that are currently queued, so a task that
     * blocks quickly and often — every interactive task, now that the
     * console read really blocks — would never be re-measured and would
     * keep whatever demand it last had (for a fresh task, the maximum).
     * Sampling here is what lets an interactive task's demand actually
     * fall.  Ordering matters: the load above was withdrawn using the OLD
     * demand, which is the value that was added on insert. */
    task_refresh_demand(t, timer_ticks_ms());
}

/* §M49 — recompute one task's `demand` from the runnable time it has
 * accrued since its last sample.  Caller holds the owning rq_lock.
 *
 * The EWMA (three parts history, one part new sample) is deliberately
 * sluggish: at a 100 ms balance interval it settles in roughly half a
 * second, which is fast enough to follow a task changing behaviour and
 * slow enough that one unlucky window cannot trigger a migration. */
static void task_refresh_demand(struct task* t, uint64_t now) {
    uint64_t total = t->runnable_acc;
    if (t->rq_next || t->rq_prev)                 /* currently queued */
        if (now > t->runnable_since) total += now - t->runnable_since;

    uint64_t dt = now - t->demand_sample_ms;
    if (dt < LOAD_SAMPLE_MIN_MS) return;          /* too short to mean anything */

    uint64_t used = (total > t->demand_acc_at_sample)
                  ? total - t->demand_acc_at_sample : 0;
    uint32_t pct = (uint32_t)((used * TASK_DEMAND_MAX) / dt);
    if (pct > TASK_DEMAND_MAX) pct = TASK_DEMAND_MAX;

    t->demand              = (t->demand * 3 + pct) / 4;
    t->demand_sample_ms    = now;
    t->demand_acc_at_sample = total;
}

/* §M49 — a task's contribution to its runqueue's load: demand scaled by
 * weight.  A nice-19 hog wants a full CPU just as much as a nice-0 hog
 * does, but it will not get one, so counting them equally would keep a
 * core reserved for work the scheduler has already decided to starve.
 * Placement and balancing should pack cheap tasks densely. */
static int task_load_contrib(const struct task* t) {
    uint64_t c = (uint64_t)t->demand * t->weight / TASK_WEIGHT_BASE;
    return (int)c;
}

/* §M49 — refresh every demand figure on THIS CPU's runqueue and republish
 * the summed load.  Own lock only: each CPU maintains its own number and
 * peers read it locklessly, so balancing never needs two rq_locks. */
static void rq_refresh_local_load(struct percpu* me) {
    uint64_t now = timer_ticks_ms();
    uint32_t fl  = spin_lock_irqsave(&me->rq_lock);
    int sum = 0;
    struct task* head = me->rq_head;
    if (head) {
        struct task* t = head;
        do {
            task_refresh_demand(t, now);
            sum += task_load_contrib(t);
            t = t->rq_next;
        } while (t != head);
    }
    me->rq_load = sum;
    spin_unlock_irqrestore(&me->rq_lock, fl);
}

/* Pick the lightest-loaded online CPU among those allowed by `mask`.
 * Tiebreak: prefer this_cpu_id so newly spawned tasks land here.
 *
 * §M49 — "lightest" is summed demand, not queue length.  Placing a new
 * CPU-bound task on a core that holds three sleepers is not the same
 * decision as placing it on a core that holds three hogs, and rq_count
 * scores those identically. */
static int pick_lightest_cpu(uint32_t mask) {
    int self = this_cpu_id();
    int best = -1;
    int best_load = 0x7FFFFFFF;
    int n = smp_ncpus();
    for (int i = 0; i < n; i++) {
        if ((mask & (1u << i)) == 0) continue;
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) continue;
        int load = p->rq_load;
        if (load < best_load || (load == best_load && i == self)) {
            best = i;
            best_load = load;
        }
    }
    /* If no online CPU is in the mask, fall back to self even though
     * the mask doesn't include it — caller is expected to validate
     * masks before calling, but be defensive. */
    if (best < 0) best = self;
    return best;
}

/* Enqueue `t` onto a runqueue.  Picks a destination CPU based on
 * affinity + current load.  Caller must NOT hold any rq_lock; we
 * acquire here.  Caller is responsible for setting t->state = RUNNABLE
 * before calling. */
static void task_enqueue(struct task* t) {
    if (t->is_idle) return;                       /* idle is not on any rq */

    int cpu = pick_lightest_cpu(t->cpu_mask ? t->cpu_mask : 0xFFFFFFFFu);
    struct percpu* rq = percpu_at(cpu);
    if (!rq) return;

    uint32_t fl = spin_lock_irqsave(&rq->rq_lock);
    t->cpu_home = cpu;
    rq_insert_tail_locked(rq, t);
    spin_unlock_irqrestore(&rq->rq_lock, fl);

    /* If we enqueued onto a different CPU, kick it so it picks up
     * the work without waiting up to a quantum for its own tick. */
    if (cpu != this_cpu_id()) smp_send_reschedule(cpu);
}

/* ------------------------------------------------------------------- */
/* Init.                                                                */
/* ------------------------------------------------------------------- */

/* Generic per-CPU idle entry — halt forever until preempted away. */
static void cpu_idle_entry(void) {
    for (;;) {
        hal_intr_enable();
        hal_cpu_halt();
        task_yield();
    }
}

void task_init(void) {
    /* Synthesize task 0 = the running kernel_main context. */
    struct task* t0 = (struct task*)kcalloc(1, sizeof(struct task));
    if (!t0) {
        kprintf("task: failed to allocate pid 0\n");
        return;
    }
    str_copy_n(t0->name, "kernel", sizeof t0->name);
    t0->pid         = next_pid++;
    t0->state       = TASK_RUNNABLE;
    t0->esp         = 0;                /* set by first context_switch */
    t0->kstack_base = NULL;             /* we don't own boot's stack */
    t0->cpu_mask    = 0xFFFFFFFFu;
    t0->cpu_home    = 0;                /* BSP */
    t0->rq_next = t0->rq_prev = NULL;
    task_sched_defaults(t0);
    hal_fpu_init_state(t0->fpu_state);  /* valid image before the first restore */

    /* Insert into master list. */
    uint32_t fl = spin_lock_irqsave(&master_lock);
    master_insert_locked(t0);
    spin_unlock_irqrestore(&master_lock, fl);

    /* Stamp BSP's current pointer.  At this point percpu_init_bsp has
     * NOT run yet (acpi+lapic come later in kernel_main); this_cpu()
     * still returns slot 0 since the lapic_id() readback is 0 before
     * the LAPIC is enabled.  That's the right slot. */
    this_cpu()->current = t0;

    /* Enqueue t0 onto its home rq so scheduler's rotate-to-tail and
     * round-robin walk see it like any other RUNNABLE non-idle task.
     * rq_lock is zero-initialized (= unlocked) before percpu_init_bsp
     * runs, which is the same state spin_lock_init leaves it in. */
    uint32_t fl_rq = spin_lock_irqsave(&this_cpu()->rq_lock);
    rq_insert_tail_locked(this_cpu(), t0);
    spin_unlock_irqrestore(&this_cpu()->rq_lock, fl_rq);

    /* Synthesize BSP idle (M18.5).  Distinct from kernel_main: when
     * kernel_main eventually task_exits, the scheduler falls back to
     * BSP idle rather than halt forever. */
    void* idle_stack = kmalloc(TASK_KSTACK_SZ);
    struct task* bsp_idle = (struct task*)kcalloc(1, sizeof(struct task));
    if (bsp_idle && idle_stack) {
        str_copy_n(bsp_idle->name, "idle-0", sizeof bsp_idle->name);
        bsp_idle->pid         = next_pid++;
        bsp_idle->state       = TASK_RUNNABLE;
        bsp_idle->esp         = hal_task_init_stack(
                                    (char*)idle_stack + TASK_KSTACK_SZ,
                                    cpu_idle_entry);
        bsp_idle->kstack_base = idle_stack;
        bsp_idle->is_idle     = 1;
        bsp_idle->cpu_mask    = 1u << 0;        /* pinned to BSP */
        bsp_idle->cpu_home    = 0;
        bsp_idle->rq_next = bsp_idle->rq_prev = NULL;
        task_sched_defaults(bsp_idle);
        hal_fpu_init_state(bsp_idle->fpu_state);

        uint32_t fl2 = spin_lock_irqsave(&master_lock);
        master_insert_locked(bsp_idle);
        spin_unlock_irqrestore(&master_lock, fl2);

        this_cpu()->idle = bsp_idle;
    }

    kprintf("task: pid 0 (kernel) installed\n");
}

/* AP-side idle-task bootstrap (M18.5).  Each AP calls this from its
 * C entry to synthesize a task struct for its current context, splice
 * it into the master list, and stamp it as this CPU's current+idle. */
void task_install_ap_idle(void) {
    struct task* idle = (struct task*)kcalloc(1, sizeof(struct task));
    if (!idle) {
        kprintf("task: failed to allocate AP idle\n");
        return;
    }
    int cpu = this_cpu_id();
    char buf[TASK_NAME_MAX + 1] = "idle-";
    int pos = 5;
    int c = cpu;
    if (c >= 10) { buf[pos++] = '0' + (c / 10); c %= 10; }
    buf[pos++] = '0' + c;
    buf[pos]   = 0;
    str_copy_n(idle->name, buf, sizeof idle->name);

    idle->pid         = next_pid++;
    idle->state       = TASK_RUNNABLE;
    idle->esp         = 0;
    idle->kstack_base = NULL;       /* AP stack owned by smp.c */
    idle->is_idle     = 1;
    idle->cpu_mask    = 1u << cpu;  /* pinned to this AP */
    idle->cpu_home    = cpu;
    idle->rq_next = idle->rq_prev = NULL;
    task_sched_defaults(idle);
    hal_fpu_init_state(idle->fpu_state);

    uint32_t fl = spin_lock_irqsave(&master_lock);
    master_insert_locked(idle);
    spin_unlock_irqrestore(&master_lock, fl);

    this_cpu()->current = idle;
    this_cpu()->idle    = idle;
}

void task_become_idle(void) {
    /* Mark the CURRENT task as the per-CPU idle.  No master/rq lock
     * needed — we're modifying our own task's idle flag and the
     * per-CPU `idle` pointer; both are read only by us. */
    struct task* me = this_cpu()->current;
    if (me) {
        me->is_idle = 1;
        this_cpu()->idle = me;
    }
}

/* ------------------------------------------------------------------- */
/* Task-lifecycle change hook (M22.4 — see task.h).                     */
/* ------------------------------------------------------------------- */

/* Single consumer slot; the write is atomic on both target archs
 * (aligned pointer).  Fired on spawn / kill / exit / reap. */
static void (*task_change_hook)(void) = NULL;

void task_set_change_hook(void (*fn)(void)) {
    task_change_hook = fn;
}

static void task_notify_change(void) {
    void (*fn)(void) = task_change_hook;
    if (fn) fn();
}

/* ------------------------------------------------------------------- */
/* Spawn.                                                               */
/* ------------------------------------------------------------------- */

/* M27 — shared spawn body.  `ppid_override >= 0` forces the parent pid
 * (task_spawn_detached uses init); < 0 means "the caller is the parent".
 * M22.7 — `arg` is stashed in t->start_arg BEFORE the task is enqueued
 * (so it is visible even if another CPU picks the task up immediately),
 * readable by the entry via task_start_arg(). */
static struct task* spawn_common(const char* name, void (*entry)(void),
                                 int ppid_override, void* arg, void* console) {
    struct task* t = (struct task*)kcalloc(1, sizeof(struct task));
    if (!t) return NULL;

    void* stack = kmalloc(TASK_KSTACK_SZ);
    if (!stack) { kfree(t); return NULL; }
    t->start_arg = arg;
    /* §M49 — bind the console HERE, before the task can be enqueued.
     * Doing it from the caller after spawn returns is an SMP race:
     * the task may already be running on another core.  Same reason
     * start_arg is set here. */
    t->out_console = console;

    str_copy_n(t->name, name, sizeof t->name);
    t->pid         = next_pid++;
    if (next_pid <= 0) next_pid = 1;        /* §4.6 — never wrap to a negative pid */
    /* M27 — parent: an explicit override (detached → init), else whoever
     * called (or pid 0 very early in boot, before there is a `current`). */
    if (ppid_override >= 0) {
        t->ppid    = ppid_override;
        /* Explicitly parented to init = a detached daemon (task_spawn_detached,
         * or a "Detached Shell" via task_spawn_under(...,init)): it outlives its
         * launcher, so it must NOT be taken down as part of a parent's subtree. */
        if (g_init_pid > 0 && ppid_override == g_init_pid) t->survives_parent = 1;
    } else {
        struct task* cur = task_current();
        t->ppid    = cur ? cur->pid : 0;
    }
    t->state       = TASK_RUNNABLE;
    t->last_yield_ms = timer_ticks_ms();     /* §M46 runaway detector baseline */
    t->esp         = hal_task_init_stack((char*)stack + TASK_KSTACK_SZ, entry);
    t->kstack_base = stack;
    t->cpu_mask    = 0xFFFFFFFFu;
    t->cpu_home    = -1;
    t->rq_next = t->rq_prev = NULL;
    /* §M49 — nice 0 and "assumed to want a whole CPU" until measured.
     * Starting demand at zero would be worse than having no metric: a
     * burst of fresh tasks would each look free and pick_lightest_cpu
     * would stack the lot onto one core before the first measurement
     * window closed.  Guessing high is self-correcting. */
    task_sched_defaults(t);
    /* A fresh, VALID FPU/SIMD image.  kcalloc's zeroing is not enough — an
     * all-zero x86 FXSAVE image restores MXCSR = 0, i.e. every SIMD exception
     * unmasked, and the task would take #XF on its first FP instruction. */
    hal_fpu_init_state(t->fpu_state);

    uint32_t fl = spin_lock_irqsave(&master_lock);
    master_insert_locked(t);
    spin_unlock_irqrestore(&master_lock, fl);

    /* Pick a CPU and enqueue.  task_enqueue does the affinity-respecting
     * lightest-load selection. */
    task_enqueue(t);
    task_notify_change();                    /* M22.4 — new task appeared */
    return t;
}

struct task* task_spawn(const char* name, void (*entry)(void)) {
    return spawn_common(name, entry, -1, NULL, NULL);  /* parent = caller */
}

/* M22.7 — spawn with a start argument (read via task_start_arg in the
 * entry).  Parent = caller.  Used by the GUI to hand each app-host task
 * the app it should run. */
struct task* task_spawn_arg(const char* name, void (*entry)(void), void* arg) {
    return spawn_common(name, entry, -1, arg, NULL);
}

/* Like task_spawn_arg, but with an EXPLICIT parent pid (>= 0), or the caller
 * when ppid < 0.  Lets a GUI launcher parent a spawned package to the long-lived
 * desktop/session task instead of the transient app-host that created it (which
 * exits immediately → M27 would otherwise re-parent the package to init). */
struct task* task_spawn_arg_under(const char* name, void (*entry)(void),
                                  void* arg, int ppid) {
    return spawn_common(name, entry, ppid, arg, NULL);
}

void* task_start_arg(void) {
    struct task* self = task_current();
    return self ? self->start_arg : NULL;
}

/* M27 — spawn an INDEPENDENT task: its parent is init, not the caller, so
 * it is not part of the caller's subtree, survives the caller's death, and
 * is never taken down by a kill_tree on the caller (the daemon pattern).
 * Before init exists (g_init_pid == 0) it falls back to pid 0 (kernel),
 * which is also a permanent root — still detached from the caller. */
struct task* task_spawn_detached(const char* name, void (*entry)(void)) {
    return spawn_common(name, entry, g_init_pid, NULL, NULL);
}

/* M22.7 — spawn with an EXPLICIT parent pid (>= 0), or the caller when
 * ppid < 0.  Lets the GUI parent a launched terminal's shell to the desktop
 * (session) rather than to the transient launcher task that created it. */
struct task* task_spawn_under(const char* name, void (*entry)(void), int ppid) {
    return spawn_common(name, entry, ppid, NULL, NULL);
}

/* §M49 — see task.h: the console must be bound before the task can be
 * picked up by any CPU, so it goes through spawn_common like start_arg. */
struct task* task_spawn_console(const char* name, void (*entry)(void),
                                int ppid, void* console) {
    return spawn_common(name, entry, ppid, NULL, console);
}

/* ------------------------------------------------------------------- */
/* Scheduler.                                                           */
/* ------------------------------------------------------------------- */

/* Defensive helper: is `t` currently `current` on some other CPU?
 * Used as a last-line safety net — the load balancer should never
 * steal a `current` task, but if a race ever sneaks through this
 * guards against double-running. */
static int task_running_elsewhere(struct task* t) {
    int self = this_cpu_id();
    int n = smp_ncpus();
    for (int i = 0; i < n; i++) {
        if (i == self) continue;
        struct percpu* p = percpu_at(i);
        if (p && p->current == t) return 1;
    }
    return 0;
}

/* Walk this CPU's runqueue and return the first RUNNABLE task whose
 * affinity includes us and which isn't running elsewhere.  Caller
 * holds rq->rq_lock.  Returns NULL if no candidate. */
static struct task* pick_next_local_locked(struct percpu* rq) {
    int self = this_cpu_id();
    struct task* head = rq->rq_head;
    if (!head) return NULL;

    /* §M49 — two passes.  The first honours `skips_left`, which is how a
     * below-baseline nice value gives up whole turns (a task at weight 50
     * sits out every other round).  If that pass finds nothing — every
     * eligible task owes a skip — the second pass ignores skips entirely,
     * so a runqueue of nothing but niced-down tasks still runs rather
     * than falling through to idle.  At the default weight no task ever
     * carries a skip and the first pass always wins on its first
     * iteration, exactly as before. */
    for (int pass = 0; pass < 2; pass++) {
        struct task* t = head;
        do {
            if (t->state == TASK_RUNNABLE &&
                !t->is_idle &&
                (t->cpu_mask & (1u << self)) &&
                !task_running_elsewhere(t)) {
                if (pass == 0 && t->skips_left > 0) {
                    t->skips_left--;            /* passed over, pays a turn */
                } else {
                    return t;
                }
            }
            t = t->rq_next;
        } while (t != head);
    }
    return NULL;
}

/* Rotate `t` to the tail of its current rq (round-robin).  Caller
 * holds rq->rq_lock.  No-op if `t` isn't actually on this rq
 * (defensive — covers e.g. brand-new t0 before its first enqueue
 * and any state transition that detaches it while still being
 * `current`). */
static void rq_rotate_to_tail_locked(struct percpu* rq, struct task* t) {
    if (!t || !rq->rq_head) return;
    /* Detect "not on this rq" cheaply: a queued task has both rq_next
     * and rq_prev non-NULL (circular list).  Or it's head with NULL
     * links — but then rq->rq_head == t is impossible without proper
     * links, so an unlinked t never matches head either. */
    if (!t->rq_next || !t->rq_prev) return;
    if (rq->rq_head == t && t->rq_next == t) return;   /* singleton */
    if (rq->rq_head == t) rq->rq_head = t->rq_next;
    t->rq_prev->rq_next = t->rq_next;
    t->rq_next->rq_prev = t->rq_prev;
    struct task* head = rq->rq_head;
    struct task* tail = head->rq_prev;
    t->rq_next    = head;
    t->rq_prev    = tail;
    tail->rq_next = t;
    head->rq_prev = t;
}

/* ---------------------------------------------------------------------
 * Load balancer.
 *
 * Cheap, opportunistic.  Runs ONLY when this CPU's rq is empty (we're
 * about to fall back to idle).  Scans peer rqs for the heaviest one
 * and steals a single non-current task whose affinity allows running
 * on us.  Returns the stolen task or NULL.
 *
 * Why "only when empty": that's the cheapest right time to balance
 * (we'd otherwise idle uselessly), avoids cross-CPU lock contention
 * on the hot path of every tick, and converges quickly under common
 * workloads (a task becoming runnable on a loaded CPU will get stolen
 * the next time some other CPU goes empty).
 * --------------------------------------------------------------------- */

/* Find a task on `victim`'s rq that:
 *   - is RUNNABLE non-idle
 *   - isn't `current` on the victim CPU
 *   - has us in its affinity mask
 *   - has demand <= `max_demand`
 * and, among those, whose demand is CLOSEST to `want_demand`.
 *
 * §M49 — WHICH task moves matters as much as whether one does.  Moving a
 * sleeper off an overloaded CPU changes the queue length and none of the
 * load, so the imbalance survives a migration that looked like progress.
 * The caller therefore asks for a specific size:
 *
 *   - the periodic balancer wants half the imbalance, because a move
 *     shifts the difference by twice the task's demand — take exactly
 *     half and the two CPUs end up level.  It also caps demand below the
 *     full imbalance, so a move can never overshoot into a mirror-image
 *     imbalance and start a ping-pong.
 *   - the idle path passes want=max=TASK_DEMAND_MAX, i.e. "the heaviest
 *     thing you have": we have nothing to run, so overshoot is
 *     impossible and the biggest task is the most useful to take.
 *
 * Returns the task already removed from the victim's queue and re-homed
 * to us.  Caller does NOT hold any lock entering. */
static struct task* load_steal_one(struct percpu* victim,
                                   uint32_t want_demand, uint32_t max_demand) {
    int self = this_cpu_id();
    uint32_t fl = spin_lock_irqsave(&victim->rq_lock);
    struct task* head = victim->rq_head;
    struct task* found = NULL;
    uint32_t best_dist = 0xFFFFFFFFu;
    if (head) {
        struct task* t = head;
        do {
            if (t->state == TASK_RUNNABLE &&
                !t->is_idle &&
                t != victim->current &&
                (t->cpu_mask & (1u << self))) {
                /* Compare in the SAME units the imbalance is expressed
                 * in — weighted contribution, not raw demand. */
                uint32_t c = (uint32_t)task_load_contrib(t);
                if (c > max_demand) { t = t->rq_next; continue; }
                uint32_t dist = (c > want_demand)
                              ? c - want_demand : want_demand - c;
                if (dist < best_dist) { best_dist = dist; found = t; }
            }
            t = t->rq_next;
        } while (t != head);
    }
    if (found) {
        rq_remove_locked(victim, found);
        found->cpu_home = self;
    }
    spin_unlock_irqrestore(&victim->rq_lock, fl);
    return found;
}

/* Try to steal one task onto this_cpu's rq.  Returns 1 if we got one,
 * 0 otherwise.  Caller must NOT hold this CPU's rq_lock.
 *
 * `min_delta` is how much busier the victim must be than us before a
 * steal is worth it.  The idle caller passes 1 (our queue is empty, so
 * anything at all beats idling).  The periodic caller passes 2 — see
 * load_balance_periodic for why 1 would oscillate. */
static int load_balance_pull(int min_delta) {
    int self = this_cpu_id();
    int n = smp_ncpus();
    struct percpu* mine = this_cpu();
    if (!mine) return 0;
    /* Two different questions, so two different metrics.
     *
     * An EMPTY queue is asking "is there any work at all?", and there the
     * right measure is the plain task count.  Scoring by demand would be
     * a regression: a runnable task that has been blocking a lot carries
     * demand near zero, so a demand-only rule would leave real work
     * queued on a peer while this CPU sat in the idle loop.  Nothing can
     * overshoot when we hold nothing, so take the heaviest available.
     *
     * A NON-EMPTY queue is asking "is the work spread fairly?", and there
     * only demand answers it — that is the entire §M49 finding.
     * `min_delta` applies to this case alone. */
    int idle_pull = (mine->rq_count == 0);
    int my_load   = idle_pull ? 0 : mine->rq_load;

    int best = -1;
    int best_load = 0;
    for (int i = 0; i < n; i++) {
        if (i == self) continue;
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) continue;
        int load = idle_pull ? p->rq_count : p->rq_load;
        if (load > best_load) {
            best = i;
            best_load = load;
        }
    }
    if (best < 0 || best_load <= 0) return 0;

    uint32_t want, cap;
    if (idle_pull) {
        /* No ceiling: a weighted contribution can far exceed
         * TASK_DEMAND_MAX (a nice -20 task counts ten times a nice-0
         * one), and capping at 100 here would make exactly the most
         * important tasks unstealable by an idle CPU. */
        want = cap = 0x7FFFFFFFu;
    } else {
        int delta = best_load - my_load;
        if (delta < min_delta) return 0;
        want = (uint32_t)(delta / 2);
        cap  = (uint32_t)(delta - 1);
    }
    struct task* stolen = load_steal_one(percpu_at(best), want, cap);
    if (!stolen) return 0;

    uint32_t fl = spin_lock_irqsave(&mine->rq_lock);
    rq_insert_tail_locked(mine, stolen);
    spin_unlock_irqrestore(&mine->rq_lock, fl);
    mine->migrations++;
    return 1;
}

/* ---------------------------------------------------------------------
 * Periodic load balance (§M49).
 *
 * Until §M49 the ONLY balancing trigger was "this CPU's runqueue ran
 * empty".  That is a work-stealing rule, not a load-distribution rule,
 * and the difference is measurable: pin five CPU hogs onto CPU0 while
 * CPU1..3 keep one each, and nothing ever moves — no queue is empty, so
 * no steal is attempted.  Measured on a 4-CPU i386 guest, the five tasks
 * on CPU0 got 15-20% of a core each while the singletons got 66%, a 3.3x
 * unfairness between identical tasks that persisted indefinitely.
 *
 * Note what does NOT reveal this: every CPU was 100% busy the whole
 * time.  Aggregate utilisation is blind to it, which is why `sched`
 * reports queue depth and per-task share as well.
 *
 * So: every LOAD_BALANCE_INTERVAL_MS each CPU also checks whether some
 * peer is meaningfully busier than it is, and pulls if so.
 *
 * Two constants carry the design:
 *
 *   MIN_DELTA = 2.  A move takes one task off the victim and puts it on
 *   us, so it swings the difference by TWO.  With a threshold of 1, a
 *   3-vs-2 split would move a task to make it 2-vs-3, and the next
 *   window would move it straight back — a permanent ping-pong that
 *   costs cache locality and buys nothing.  At 2 the post-move state is
 *   never worse than the pre-move state.
 *
 *   MAX_STEAL = 4.  Each pull re-reads the queue depths and re-checks
 *   the threshold, so a large imbalance is corrected within a single
 *   pass (5-vs-1 becomes 4-vs-2 becomes 3-vs-3, then stops on the
 *   threshold) instead of trickling one task per 100 ms window.  The cap
 *   just bounds the worst-case time spent here with interrupts off.
 *
 * Concurrency: two CPUs may pull from the same victim at once.  Each
 * takes the victim's rq_lock so the queue itself is safe, and the
 * threshold is re-evaluated on every pull, so the worst case is one
 * extra migration — not a runaway.
 * --------------------------------------------------------------------- */

/* Called from schedule_check (timer-IRQ exit), holding NO rq_lock. */
static void load_balance_tick(struct percpu* me) {
    if (smp_ncpus() < 2) return;                 /* nothing to balance against */
    uint64_t now = timer_ticks_ms();
    if (now - me->last_balance_ms < LOAD_BALANCE_INTERVAL_MS) return;
    me->last_balance_ms = now;
    /* Republish our own load first — peers read this number locklessly to
     * decide whether we are worth stealing from, so a CPU that never
     * refreshed would advertise a permanently stale figure. */
    rq_refresh_local_load(me);
    for (int i = 0; i < LOAD_BALANCE_MAX_STEAL; i++)
        if (!load_balance_pull(LOAD_BALANCE_MIN_DELTA)) break;
}

/* schedule_locked — pick + context_switch.  NEVER releases the lock:
 * after context_switch, the LOCK OWNERSHIP transfers to whoever resumes
 * us next.  This is the Linux pattern (finish_task_switch).  Reasoning:
 *
 *   Each schedule() invocation acquires this_cpu()->rq_lock at entry.
 *   If schedule_locked context_switches OUT, the current schedule()
 *   frame is suspended; the rq_lock we acquired stays held until SOME
 *   future moment when we (this task) get scheduled BACK IN, returning
 *   to the schedule_locked frame that's been paused on our stack.
 *   At that moment the lock we should release is THIS CPU's rq_lock
 *   — i.e. the CPU we're now running on, which may differ from the
 *   one we yielded on (if the load balancer migrated us).  We can't
 *   pass the old CPU's lock identity through the stack — it's no
 *   longer the right thing to release.  Instead the resumer re-reads
 *   this_cpu() at unlock time.
 *
 *   The matching invariant: every CPU's schedule() pairs its rq_lock
 *   acquire with EXACTLY ONE rq_lock release somewhere — that release
 *   may happen on a different CPU's stack (the resumed task's), but
 *   it WILL release the right lock (the one the resuming CPU's
 *   schedule_locked just took).
 *
 * Brand-new task path: task_finish_first_switch performs the unlock
 * for the brand-new task's "first schedule that never was."  See
 * hal/<arch>/task_arch.c task_trampoline. */
static void schedule_locked(struct percpu* me) {
    struct task* prev = me->current;
    if (!prev) return;

    /* §M49 — weighted round robin.  `deficit` is a quantum budget spent
     * by the tick handler (schedule_check) while this task is current;
     * only when it runs out does the task go to the back of the queue and
     * get a fresh budget of `weight`.  So weight 300 runs three quanta per
     * round and weight 100 runs one — identical to the unweighted
     * behaviour, which is why the default costs nothing.
     *
     * Weights BELOW the base cannot buy fewer than one quantum, so they
     * buy fewer TURNS instead: the replenish converts the leftover debt
     * into `skips_left`, which pick_next_local_locked honours.
     *
     * A task that yields voluntarily has its deficit zeroed by
     * task_yield, so cooperative yielding still hands the CPU over
     * immediately rather than being ignored until the quantum expires. */
    if (prev->state == TASK_RUNNABLE && !prev->is_idle &&
        prev->cpu_home == this_cpu_id() &&
        prev->deficit <= 0) {
        int debt = -prev->deficit;              /* how far past the budget */
        /* Never trust the weight to be non-zero here.  A zero weight is a
         * #DE, and this runs in the scheduler on every tick — the one
         * place a kernel cannot afford to fault.  See task_sched_defaults
         * for how a zero got here in the first place. */
        uint32_t w = prev->weight ? prev->weight : TASK_WEIGHT_BASE;
        prev->deficit = (int)w;
        prev->skips_left = (w < TASK_WEIGHT_BASE)
                         ? (int)((TASK_WEIGHT_BASE + debt) / w) - 1
                         : 0;
        if (prev->skips_left < 0) prev->skips_left = 0;
        rq_rotate_to_tail_locked(me, prev);
    }

    struct task* next = pick_next_local_locked(me);

    if (!next) {
        /* No candidate on local rq.  Drop the lock briefly to try a
         * load-balance steal — can't hold rq_lock while taking
         * another CPU's lock (would risk deadlock). */
        spin_unlock(&me->rq_lock);
        int stole = load_balance_pull(1);   /* we're empty: take anything */
        spin_lock(&me->rq_lock);
        if (stole) next = pick_next_local_locked(me);
    }

    if (!next) {
        if (prev->state == TASK_RUNNABLE && !prev->is_idle) return;
        struct task* idle = me->idle;
        if (!idle || idle->state != TASK_RUNNABLE) return;
        if (idle == prev) return;
        next = idle;
    }

    if (next == prev) return;

    /* M22.3 — CPU-time accounting at the switch boundary.  Cheap: one
     * timer read + two u64 ops per context switch. */
    {
        uint64_t now   = timer_ticks_ms();
        uint64_t slice = now - prev->sched_in_ms;
        prev->cpu_ms += slice;
        next->sched_in_ms = now;
        /* §M49 — per-CPU busy time.  Everything that is not the idle task
         * counts as work, which is what makes "is this core actually loaded"
         * answerable at all; rq_count alone cannot tell a saturated CPU from
         * an idle one. */
        if (!prev->is_idle) me->busy_ms += slice;
        me->switches++;
    }

    me->current = next;
    /* Tier B — set the per-task ring-3→ring-0 kernel stack (TSS.esp0 on x86).
     * An independent user task uses its own kstack top; anything else restores
     * the arch default (fixed syscall stack / excursion model).  No-op on
     * aarch64 (SP_EL1 tracks via context_switch).  Must happen before the CR3
     * switch + context_switch so a ring-3 entry after this uses the right
     * stack. */
    hal_set_kernel_stack((next->user_task && next->kstack_base)
                         ? (uintptr_t)next->kstack_base + TASK_KSTACK_SZ
                         : 0);
    /* M35 TLS — point this CPU's user-TLS segment base at the incoming
     * thread's thread-pointer, so its %gs-relative __thread accesses resolve
     * to its own block.  No-op if the task set no TLS.  (TLS threads are
     * pinned to their CPU, so `this_cpu` here is always their home CPU.) */
    if (next->has_tls) hal_set_tls_base(next->tls_base);
    /* M25 — switch to next's address space before swapping stacks.  For a
     * kernel thread (mm == NULL) this targets the shared kernel directory,
     * and vmm_space_switch skips the CR3 reload when it's already loaded —
     * so kernel-thread → kernel-thread switches stay free (no TLB flush).
     * Both stacks live in the kernel identity map, which every space keeps
     * mapped, so doing this before context_switch is safe. */
    /* Per-task FPU/SIMD register file.  context_switch swaps the INTEGER
     * context only, so the FP/vector registers have to be moved explicitly —
     * otherwise `next` inherits whatever `prev` left in them.  Two FP-using
     * ring-3 tasks would silently corrupt each other's arithmetic (no fault,
     * no log, just wrong numbers), and on SMP a task migrated to another core
     * would resume on THAT core's register file.  Eager save/restore on every
     * switch: at a 100 Hz tick the cost is noise, and lazy (CR0.TS + #NM)
     * switching is a classic source of subtle cross-task state leaks.  Arches
     * with no reachable FP unit implement these as no-ops (see aarch64). */
    hal_fpu_save(prev->fpu_state);
    hal_fpu_restore(next->fpu_state);
    vmm_space_switch(next->mm);
    context_switch(&prev->esp, next->esp);
    /* Resumes here when `prev` is scheduled back in by SOME CPU. */
}

void schedule(void) {
    uint32_t fl = hal_intr_save();
    spin_lock(&this_cpu()->rq_lock);
    schedule_locked(this_cpu());
    /* Re-read this_cpu() at unlock time — see schedule_locked header.
     * The lock we're releasing is whichever CPU we are NOW (which may
     * differ from entry CPU if context_switch resumed us on another
     * core after a load-balance migration). */
    spin_unlock(&this_cpu()->rq_lock);
    hal_intr_restore(fl);
}

void task_yield(void) {
    /* M22.3 — cooperative kill lands here: every voluntary yield point
     * (vc_getchar, keyboard_getchar, ...) funnels through task_yield,
     * and by convention no spinlocks are held across a yield, so
     * exiting is safe.  The IRQ preemption path (schedule_check) must
     * NOT do this — a preempted task may hold a window/surface lock. */
    struct task* self = task_current();
    if (self && self->kill_pending && !self->is_idle) task_exit();
    if (self) {                              /* §M46 — voluntary yield: not a runaway */
        self->last_yield_ms   = timer_ticks_ms();
        self->cpu_ms_at_yield = self->cpu_ms;
        /* §M49 — giving up the CPU on purpose forfeits the rest of the
         * quantum budget, so the rotate in schedule_locked actually
         * happens.  Without this a high-weight task's voluntary yield
         * would be silently ignored until its quanta ran out. */
        self->deficit = 0;
    }
    schedule();
}

int task_should_stop(void) {
    struct task* self = task_current();
    return self ? self->kill_pending : 0;
}

/* §M49 — a REAL timed sleep: the caller leaves every runqueue until its
 * deadline, and the tick sweep puts it back.
 *
 * The previous implementation was a spin-yield loop — hlt, yield, re-read
 * the clock — which left the task RUNNABLE and queued for the whole
 * "sleep".  That cost twice over.  It burned a scheduling slot on every
 * round through the queue, and the `hlt` HALTED THE CPU while other
 * runnable tasks sat on that same runqueue waiting: a sleeping cron job
 * could idle a core that had work queued behind it.
 *
 * It also made the §M49 load metric meaningless, which is how it was
 * found.  Demand is measured as time spent runnable, so a task that
 * never leaves the queue measures as wanting a full CPU.  Every service
 * task in the system — cron, watchdog, heartbeat — reported demand 100,
 * identical to a genuine CPU hog, and the balancer had no signal to work
 * with.  A metric is only as honest as the state it observes.
 *
 * Early boot (no `current` yet, or the idle task itself) keeps the old
 * busy-wait: there is nothing to switch to and nothing to sweep us. */
void task_msleep(uint32_t ms) {
    uint64_t end = timer_ticks_ms() + (uint64_t)ms;
    struct task* self = task_current();
    if (!self || self->is_idle) {
        while (timer_ticks_ms() < end) hal_cpu_idle();
        return;
    }

    while (timer_ticks_ms() < end) {
        if (task_should_stop()) return;          /* let a kill land promptly */

        uint32_t fl = hal_intr_save();
        /* Re-check under IRQs off: the sweep runs from the timer IRQ, so
         * without this a tick landing here could pass our deadline after
         * we tested it and before we parked — and nothing would wake us. */
        if (timer_ticks_ms() >= end) { hal_intr_restore(fl); return; }

        self->sleep_until_ms  = end;
        self->state           = TASK_SLEEPING;
        self->last_yield_ms   = timer_ticks_ms();  /* §M46 — blocking = responsive */
        self->cpu_ms_at_yield = self->cpu_ms;
        g_timed_sleepers++;

        struct percpu* me = this_cpu();
        spin_lock(&me->rq_lock);
        if (self->cpu_home == this_cpu_id()) rq_remove_locked(me, self);
        spin_unlock(&me->rq_lock);

        hal_intr_restore(fl);
        schedule();                              /* off-queue until swept */
    }
    self->sleep_until_ms = 0;
}

/* §M49 — wake every timed sleeper whose deadline has passed.  Runs from
 * the timer-IRQ exit path on whichever CPU ticks first in a given
 * millisecond.
 *
 * The counter fast-path matters: with no timed sleepers this is a single
 * load and return, so the common case costs nothing.  Waking is done
 * AFTER dropping master_lock — task_enqueue takes an rq_lock, and taking
 * an rq_lock underneath master_lock would introduce a second lock order
 * into a kernel that otherwise only ever nests the other way. */
static void sleep_sweep(void) {
    if (g_timed_sleepers <= 0) return;
    uint64_t now = timer_ticks_ms();
    if (now == g_sleep_sweep_ms) return;         /* at most one sweep per ms */
    g_sleep_sweep_ms = now;

    struct task* wake[SLEEP_SWEEP_BATCH];
    int n = 0;
    uint32_t fl = spin_lock_irqsave(&master_lock);
    struct task* t = master_head;
    if (t) {
        do {
            if (t->state == TASK_SLEEPING && t->sleep_until_ms &&
                t->sleep_until_ms <= now) {
                t->sleep_until_ms = 0;
                if (g_timed_sleepers > 0) g_timed_sleepers--;
                wake[n++] = t;
                if (n == SLEEP_SWEEP_BATCH) break;   /* rest wait one more tick */
            }
            t = t->next;
        } while (t != master_head);
    }
    spin_unlock_irqrestore(&master_lock, fl);

    for (int i = 0; i < n; i++) {
        if (wake[i]->state != TASK_SLEEPING) continue;   /* someone beat us to it */
        wake[i]->state = TASK_RUNNABLE;
        task_enqueue(wake[i]);
    }
}

/* §M49 — drag a timed sleeper back onto a runqueue ahead of its deadline.
 *
 * Needed because task_msleep now really blocks.  While it was a spin-yield
 * loop, a kill landed within a tick: the sleeper was RUNNABLE the whole
 * time and re-tested task_should_stop() on every pass.  A blocked task
 * tests nothing, so without this a `kill` on a service sleeping one second
 * would sit unacknowledged for up to that second — and the GUI window
 * teardown and the §M46 kill-tree both wait on tasks reaching a kill
 * point.  Cheap to do right: a timed sleeper is on no waitq, so there is
 * no queue to unlink it from — only its deadline to cancel.
 *
 * Callers hold no rq_lock. */
static void wake_timed_sleeper(struct task* t) {
    if (!t || t->state != TASK_SLEEPING || !t->sleep_until_ms) return;
    t->sleep_until_ms = 0;
    if (g_timed_sleepers > 0) g_timed_sleepers--;
    t->state = TASK_RUNNABLE;
    task_enqueue(t);
}

/* §M49 — drag a task parked on a WAITQ back onto a runqueue.
 *
 * The counterpart of wake_timed_sleeper for the other way a task can be
 * asleep.  Needed once blocking spread from `task_msleep` to the console
 * read: a shell parked in `vc_getchar` waiting for a keystroke has to be
 * killable without one arriving, because both the GUI window teardown and
 * §M46's kill-tree wait for tasks to reach a kill point.
 *
 * This is a SPURIOUS wake by design — the condition the task blocked on
 * is still false.  That is safe precisely because waitq's contract makes
 * callers loop on their condition (see waitq.h); the woken task re-tests,
 * finds nothing, and its loop is expected to check `task_should_stop()`
 * before parking again.
 *
 * Lock order is wq->lock then rq_lock (via task_enqueue), matching
 * waitq_block — the only other place that holds both. */
static void wake_waitq_sleeper(struct task* t) {
    if (!t) return;
    struct waitq* wq = t->wq;
    if (!wq) return;

    uint32_t fl = spin_lock_irqsave(&wq->lock);
    /* Unlink from the singly-linked parked list.  It may already be gone
     * — a real waker could have raced us between the read of t->wq and
     * this lock — in which case there is nothing left to do. */
    struct task** pp = &wq->head;
    while (*pp) {
        if (*pp == t) { *pp = t->wq_next; t->wq_next = NULL; break; }
        pp = &(*pp)->wq_next;
    }
    t->wq = NULL;
    int woke = (t->state == TASK_SLEEPING);
    if (woke) t->state = TASK_RUNNABLE;
    spin_unlock_irqrestore(&wq->lock, fl);

    if (woke) task_enqueue(t);
}

/* Both flavours of sleep, for a caller that just wants the task awake. */
static void wake_blocked_task(struct task* t) {
    wake_timed_sleeper(t);
    wake_waitq_sleeper(t);
}

int task_kill(int pid) {
    if (pid == 0) return -1;                 /* pid 0 = kernel/BSP idle */
    struct task* t = task_find(pid);
    if (!t || t->is_idle || t->state == TASK_DEAD) return -1;
    t->kill_pending = 1;
    wake_blocked_task(t);                    /* so it notices now, not at its deadline */
    task_notify_change();                    /* M22.4 — liveness will change */
    return 0;
}

/* §M46 — opt a user task into runaway auto-force-kill (ms > 0), or disable it
 * (ms == 0).  A launcher sets this per package right after proc_spawn. */
int task_set_auto_fkill(int pid, uint32_t ms) {
    struct task* t = task_find(pid);
    if (!t) return -1;
    t->auto_fkill_ms  = ms;
    t->last_yield_ms  = timer_ticks_ms();    /* fresh baseline from now */
    t->cpu_ms_at_yield = t->cpu_ms;
    return 0;
}

/* §M46 — force-kill: cooperative flag PLUS the forced flag so a wedged ring-3
 * task is reclaimed at its next timer preemption in user mode (see
 * task_force_kill_point).  Same protections as task_kill (no pid 0 / idle). */
int task_force_kill(int pid) {
    if (pid == 0) return -1;
    struct task* t = task_find(pid);
    if (!t || t->is_idle || t->state == TASK_DEAD) return -1;
    t->kill_pending = 1;
    t->kill_forced  = 1;
    wake_blocked_task(t);                    /* §M49 — see task_kill */
    task_notify_change();
    return 0;
}

/* §M46 — the force-kill safe point, called from the arch timer ISR at the
 * preemption boundary with `from_user` = the interrupted context was in ring 3.
 * A ring-3 task holds no kernel locks, so tearing it down here is safe — this is
 * the point a busy-looping user task (frozen browser) can never reach through
 * the cooperative task_yield path.  A task force-killed while in a syscall is
 * left for its return-to-user / next cooperative yield instead. */
void task_force_kill_point(int from_user) {
    if (!from_user) return;
    struct task* self = task_current();
    if (self && self->kill_forced && !self->is_idle && self->state != TASK_DEAD) {
        /* §M47 — a task reclaimed by force is a failure worth reporting: from
         * the user's side the app "stopped responding", and they should be able
         * to see that afterwards rather than just watching it vanish. */
        crash_report(CRASH_FORCED_KILL, self->pid, self->name, 0, 0, 137,
                     "unresponsive task reclaimed by force");
        task_exit_code(137);                 /* 128 + SIGKILL, conventional */
    }
}

int task_reap(int pid) {
    if (!master_head) return -1;

    /* Refuse while the victim is still current anywhere — DEAD is set
     * just before its final context_switch, so there is a short window
     * where the stack is still in use.  Caller retries. */
    struct task* t = task_find(pid);
    if (!t || t->state != TASK_DEAD) return -1;
    int n = smp_ncpus();
    for (int i = 0; i < n; i++)
        if (percpu_at(i) && percpu_at(i)->current == t) return -1;

    /* Unlink from the master ring (circular SLL — walk for the prev). */
    uint32_t fl = spin_lock_irqsave(&master_lock);

    /* M27 — re-parent any surviving children to init before this pid
     * disappears, so their ppid never dangles on a freed/re-used pid.
     * (Walk the whole ring; task counts are small.) */
    if (master_head) {
        struct task* c = master_head;
        do {
            if (c->ppid == t->pid && c != t) c->ppid = g_init_pid;
            c = c->next;
        } while (c != master_head);
    }

    struct task* prev = master_head;
    while (prev->next != t && prev->next != master_head) prev = prev->next;
    if (prev->next != t) {                   /* raced away — bail */
        spin_unlock_irqrestore(&master_lock, fl);
        return -1;
    }
    if (t->next == t) master_head = NULL;    /* last task (can't happen) */
    else {
        prev->next = t->next;
        if (master_head == t) master_head = t->next;
    }
    spin_unlock_irqrestore(&master_lock, fl);

    /* Tier B — free an independent user task's address space now.  Safe here:
     * the task is DEAD and confirmed not current on any CPU (checked above),
     * so its vmm_space is loaded nowhere and can be torn down.  (Its fds were
     * closed at SYS_EXIT while it was still current.)  A kernel thread's mm is
     * NULL / borrowed, so only user_task owns one to free. */
    if (t->user_task && t->mm && !t->mm_shared) {
        vmm_space_destroy(t->mm);
        t->mm = NULL;
    }

    if (t->kstack_base) kfree(t->kstack_base);
    kfree(t);
    task_notify_change();                    /* M22.4 — task disappeared */
    return 0;
}

/* ------------------------------------------------------------------- */
/* M27 — process model: init/reaper, kill-tree, parentage helpers.     */
/* ------------------------------------------------------------------- */

int task_reaper_pid(void) { return g_init_pid; }

void task_set_reap_owned(struct task* t, int owned) {
    if (t) t->reap_owned = owned ? 1 : 0;
}

/* Cooperatively kill `pid` and every descendant.  Two phases so we never
 * call task_kill (which takes master_lock) while holding it: collect the
 * subtree's pids under the lock, then flag them after releasing it.  The
 * subtree is grown to a fixpoint — each pass adopts tasks whose parent is
 * already marked — which handles arbitrary depth in a few cheap passes. */
#define KILLTREE_MAX 64
int task_kill_tree(int pid) {
    if (pid <= 0) return -1;
    int ids[KILLTREE_MAX];
    int n = 0;
    ids[n++] = pid;

    uint32_t fl = spin_lock_irqsave(&master_lock);
    int changed = 1;
    while (changed && n < KILLTREE_MAX) {
        changed = 0;
        if (!master_head) break;
        struct task* t = master_head;
        do {
            /* Already collected?  Skip. */
            int seen = 0;
            for (int i = 0; i < n; i++) if (ids[i] == t->pid) { seen = 1; break; }
            if (!seen) {
                for (int i = 0; i < n; i++) {
                    if (t->ppid == ids[i]) {
                        if (n < KILLTREE_MAX) { ids[n++] = t->pid; changed = 1; }
                        break;
                    }
                }
            }
            t = t->next;
        } while (t != master_head);
    }
    spin_unlock_irqrestore(&master_lock, fl);

    int killed = 0;
    for (int i = 0; i < n; i++)
        if (task_kill(ids[i]) == 0) killed++;
    return killed;
}

/* ---- init / reaper task --------------------------------------------- */

/* Scan for DEAD tasks that are NOT reap_owned (a subsystem that owns its
 * own reap — e.g. the GUI window teardown) and NOT still current on some
 * CPU, then reap them.  Two phases for the same lock reason as kill-tree:
 * task_for_each holds master_lock, task_reap takes it. */
struct reap_scan { int pids[KILLTREE_MAX]; int n; };

static void reap_collect(const struct task* t, int is_current, void* ctx) {
    struct reap_scan* s = (struct reap_scan*)ctx;
    if (is_current || t->state != TASK_DEAD || t->reap_owned) return;
    /* Never reap pid 0 (the boot/"swapper" thread — task_exit'd after boot
     * but kept as the conventional permanent root) or init itself. */
    if (t->pid == 0 || t->pid == g_init_pid) return;
    if (s->n < KILLTREE_MAX) s->pids[s->n++] = t->pid;
}

static void reaper_pass(void) {
    struct reap_scan s = { .n = 0 };
    task_for_each(reap_collect, &s);
    for (int i = 0; i < s.n; i++) {
        struct task* t = task_find(s.pids[i]);
        if (!t) continue;
        char nm[TASK_NAME_MAX + 1];
        str_copy_n(nm, t->name, sizeof nm);
        int ppid = t->ppid, code = t->exit_code;
        if (task_reap(s.pids[i]) == 0)
            kprintf("init: reaped '%s' (pid %d, ppid %d, code %d)\n",
                    nm, s.pids[i], ppid, code);
    }
}

static void init_entry(void) {
    kprintf("init: up as pid %d — universal reaper + orphan adopter\n",
            task_current() ? task_current()->pid : -1);
    for (;;) {
        reaper_pass();
        /* Sweep at roughly the timer's pace.  This used to be
         * `hal_cpu_idle(); task_yield();` with a comment claiming it
         * "cost nothing when the system is quiet" — §M49 measured the
         * opposite: the loop stayed RUNNABLE, so init held a runqueue
         * slot forever, halted its core on every turn, and reported
         * demand 100 to the load balancer, indistinguishable from a CPU
         * hog.  On an otherwise idle machine init alone pegged a core.
         * `task_msleep` really blocks now, so the same sweep rate costs
         * an off-queue task instead of a busy one. */
        task_msleep(10);
    }
}

void task_start_init(void) {
    if (g_init_pid) return;                  /* singleton */
    struct task* t = task_spawn("init", init_entry);
    if (!t) { kprintf("task: FATAL — init spawn failed\n"); return; }
    g_init_pid = t->pid;
}

/* ---- task_wait (Tier A.2) ------------------------------------------- */

/* Scan the master list for a child of `parent_pid` matching `want` (>0 a
 * specific pid, <=0 any child).  Caller holds child_exit_wq's lock (so the
 * scan is serialized against a concurrent exit's wake — see task_wait).
 * Reports, via out-params: whether any matching child is still alive, and
 * the pid+exit_code of the first DEAD match found (dead_pid = -1 if none).
 *
 * Contract note: a parent that intends to task_wait a child should claim its
 * reap with task_set_reap_owned(child, 1) at spawn time, so init's universal
 * reaper leaves the DEAD struct in place for task_wait to harvest (otherwise
 * init may reap-and-free it first, and this scan would report "no such child").
 * task_wait IS that owning reaper, so — unlike the GUI's own-reap case — it
 * deliberately harvests reap_owned children here.  Runs under master_lock
 * (nested inside the queue lock; safe because task_exit_code never holds both
 * at once — it releases master_lock before taking the queue lock). */
static void wait_scan_locked(int parent_pid, int want,
                             int* any_alive, int* dead_pid, int* dead_code) {
    *any_alive = 0;
    *dead_pid  = -1;
    *dead_code = 0;
    uint32_t fl = spin_lock_irqsave(&master_lock);
    if (master_head) {
        struct task* c = master_head;
        do {
            if (c->ppid == parent_pid && (want <= 0 || c->pid == want)) {
                if (c->state == TASK_DEAD) {
                    if (*dead_pid < 0) { *dead_pid = c->pid; *dead_code = c->exit_code; }
                } else {
                    *any_alive = 1;
                }
            }
            c = c->next;
        } while (c != master_head);
    }
    spin_unlock_irqrestore(&master_lock, fl);
}

int task_wait(int pid, int* code) {
    struct task* self = task_current();
    if (!self) return -1;
    int me = self->pid;

    uint32_t f = waitq_lock(&child_exit_wq);
    for (;;) {
        int any_alive, dead_pid, dead_code;
        wait_scan_locked(me, pid, &any_alive, &dead_pid, &dead_code);

        if (dead_pid >= 0) {
            /* Found a dead child.  Capture the code now (under the queue
             * lock, before anyone can reap the struct), then release the
             * lock and reap outside it.  init's universal reaper may race
             * us for the struct — whoever loses gets task_reap() == -1,
             * but we already hold the exit code, so we still return it. */
            waitq_unlock(&child_exit_wq, f);
            if (code) *code = dead_code;
            task_reap(dead_pid);
            return dead_pid;
        }
        if (!any_alive) {
            /* No matching child, alive or dead — nothing to wait for. */
            waitq_unlock(&child_exit_wq, f);
            return -1;
        }
        /* A matching child is alive but not yet dead — park until some
         * task exits and wakes us, then re-scan. */
        waitq_block(&child_exit_wq);
    }
}

/* ------------------------------------------------------------------- */
/* Per-task accessors (M14).                                            */
/* ------------------------------------------------------------------- */

struct task* task_current(void) {
    return this_cpu() ? this_cpu()->current : NULL;
}

void task_set_out_console(struct task* t, void* console) {
    if (!t) return;
    t->out_console = console;
}

/* Lock-handoff finisher for brand-new tasks.  See task.h.  Releases
 * the rq_lock the spawning schedule() acquired and never had a chance
 * to release on the new task's first switch.  The "rq" here is the
 * one schedule() was looking at — i.e. THIS CPU's rq.  Safe to call
 * because the new task is running on the CPU that did schedule(). */
void task_finish_first_switch(void) {
    spin_unlock(&this_cpu()->rq_lock);
}

void task_exit(void) { task_exit_code(0); }

void task_exit_code(int code) {
    struct percpu* me = this_cpu();
    struct task* self = me->current;

    /* §M40 — CLONE_CHILD_CLEARTID.  musl's pthread_join parks on a futex at
     * this address, and the contract is that the KERNEL zeroes it and wakes the
     * waiters when the thread dies.  Do it BEFORE the task is marked DEAD, while
     * the shared address space is still current: joining a finished thread
     * otherwise blocks forever, which is how "the program printed all its output
     * and then hung" looks. */
    if (self->clear_tid) {
        int* p = self->clear_tid;
        self->clear_tid = NULL;
        *p = 0;
        sys_futex(p, 1 /* FUTEX_WAKE */, 0x7fffffff);
    }

    /* Mark DEAD under master_lock so iterators see a consistent state. */
    uint32_t mfl = spin_lock_irqsave(&master_lock);
    self->exit_code = code;                  /* M27 — recorded for reap/ps */
    self->state = TASK_DEAD;
    spin_unlock_irqrestore(&master_lock, mfl);

    task_notify_change();                    /* M22.4 — went DEAD */

    /* Tier A.2 — wake any parent parked in task_wait().  DEAD is already
     * set (and master_lock released), so a waiter that acquires the queue
     * lock after us is guaranteed to see this task dead on its re-scan —
     * that ordering (set-condition BEFORE signal-lock) is what makes the
     * wait/exit handshake lost-wakeup-free.  We wake ALL waiters and let
     * each re-scan; only the real parent will match. */
    {
        uint32_t wf = waitq_lock(&child_exit_wq);
        waitq_wake_all(&child_exit_wq);
        waitq_unlock(&child_exit_wq, wf);
    }

    /* Process model — a dying task takes its subtree DOWN with it (the user's
     * rule: "parent dies, its children go too").  Every direct child that did
     * NOT opt to survive (survives_parent — a detached daemon / shell) is
     * killed; survivors are re-parented to init so they outlive us.  Each killed
     * child runs this same pass when it exits, so the whole subtree unwinds —
     * e.g. the desktop crashing takes the compositor + every launched app
     * (NetSurf) with it, instead of leaving a windowless orphan under init.
     *
     * A ring-3 child (user_task) may be wedged (a frozen browser) and never
     * honour the cooperative kill, so force it; kernel threads take the
     * cooperative kill (they may hold locks — never force those).  Pids are
     * collected under master_lock, then killed after releasing it (task_kill /
     * task_force_kill re-acquire master_lock). */
    /* pid 0 is the permanent boot/"swapper" root: kernel_main task_exit()s it at
     * the end of boot, but it is NOT really dying — it becomes the BSP idle task,
     * and init + the early system daemons are parented to it.  So pid 0's "exit"
     * must NOT take its subtree down (that would kill init and stop all reaping).
     * Likewise never let the idle task's teardown run.  Every other task applies
     * the "parent dies → subtree dies" rule. */
    if (self->pid != 0 && !self->is_idle) {
        int kids[KILLTREE_MAX]; char kforce[KILLTREE_MAX]; int nk = 0;
        uint32_t f2 = spin_lock_irqsave(&master_lock);
        if (master_head) {
            struct task* c = master_head;
            do {
                if (c->ppid == self->pid && c != self && c->state != TASK_DEAD) {
                    if (c->survives_parent) {
                        c->ppid = g_init_pid;              /* detached: outlives us */
                    } else if (nk < KILLTREE_MAX) {
                        kforce[nk] = (char)(c->user_task ? 1 : 0);
                        kids[nk++] = c->pid;
                    }
                }
                c = c->next;
            } while (c != master_head);
        }
        spin_unlock_irqrestore(&master_lock, f2);
        for (int i = 0; i < nk; i++) {
            if (kforce[i]) task_force_kill(kids[i]);
            else           task_kill(kids[i]);
        }
    }

    /* Remove from this CPU's rq so the next pick doesn't keep
     * tripping over a DEAD entry.  (M18.6.1 — pre-refactor, DEAD
     * tasks were left in the ring and skipped by the state check;
     * with per-CPU rqs we have a clean remove path.)  Then schedule
     * away.  The DEAD task is not on any rq and not RUNNABLE — won't
     * be picked again. */
    uint32_t rfl = spin_lock_irqsave(&me->rq_lock);
    if (self->cpu_home == this_cpu_id()) {
        rq_remove_locked(me, self);
    }
    schedule_locked(me);
    spin_unlock_irqrestore(&me->rq_lock, rfl);

    /* Should be unreachable: idle is always runnable, so schedule_locked above
     * never returns for a DEAD task.  If we DO get here, do NOT freeze the box
     * with IRQs off (a silent 0%-CPU hang — impossible to diagnose).  Report it
     * to the serial port (lock-free) and idle with IRQs ENABLED (sti;hlt), re-
     * running the scheduler on each wake so a task that becomes runnable (a
     * device IRQ waking a blocked one) can still be picked up. */
    {
        extern void serial_write(const char* s);
        serial_write("\n!! task_exit_code: no runnable task after exit "
                     "(idle missing?) — idling with IRQs on, NOT a hard freeze\n");
    }
    for (;;) { hal_cpu_idle(); schedule(); }
}

/* ------------------------------------------------------------------- */
/* Wait-queue: race-free block / wake (Tier A.1 — see waitq.h).         */
/*                                                                       */
/* Lives here (not in its own .c) because parking a task reaches deep    */
/* into scheduler internals: this_cpu(), the per-CPU runqueue remove     */
/* path, and schedule().  The public shape is in waitq.h.                */
/* ------------------------------------------------------------------- */

void waitq_init(struct waitq* wq) {
    if (!wq) return;
    spin_lock_init(&wq->lock);
    wq->head = NULL;
}

uint32_t waitq_lock(struct waitq* wq) {
    return spin_lock_irqsave(&wq->lock);
}

void waitq_unlock(struct waitq* wq, uint32_t flags) {
    spin_unlock_irqrestore(&wq->lock, flags);
}

/* Park the current task on `wq`.  Called with wq->lock held + IRQs off
 * (the flags saved by waitq_lock).  Mirrors task_exit's discipline —
 * detach from the runqueue, then hand the CPU off — but with SLEEPING
 * (resumable) instead of DEAD.
 *
 * Ordering that makes this lost-wakeup-free: we register on the queue
 * AND flip to SLEEPING while STILL holding wq->lock, so any waker (which
 * must also take wq->lock, by the condvar convention) either runs before
 * us and finds nothing to check-then-block, or runs after us and finds a
 * fully-parked task.  We drop wq->lock only once committed to sleeping.
 *
 * The unlock->schedule window: a remote waker there pops us, sets
 * RUNNABLE and re-enqueues us on some CPU's rq while we are still
 * `current` here.  That is not a lost wakeup — we are RUNNABLE on a rq;
 * schedule() below either keeps running us (next==prev, we fall through
 * and recheck) or switches away and we get picked again.  IRQs are off,
 * so a same-CPU timer cannot preempt the window. */
void waitq_block(struct waitq* wq) {
    struct task* self = task_current();
    if (!self || self->is_idle) {            /* defensive: never park idle/pid-less */
        return;
    }

    /* Register + mark SLEEPING under wq->lock. */
    self->wq_next = wq->head;
    wq->head      = self;
    self->wq      = wq;                         /* §M49 — so a kill can find us */
    self->state   = TASK_SLEEPING;
    self->last_yield_ms   = timer_ticks_ms();   /* §M46 — blocking = responsive */
    self->cpu_ms_at_yield = self->cpu_ms;

    /* Detach from this CPU's runqueue so schedule() won't pick us. */
    struct percpu* me = this_cpu();
    spin_lock(&me->rq_lock);
    if (self->cpu_home == this_cpu_id()) {
        rq_remove_locked(me, self);
    }
    spin_unlock(&me->rq_lock);

    /* Commit to sleeping: release the condition lock (a waker may run
     * now) and switch away.  We are SLEEPING + off every rq, so no CPU
     * will pick us until a waker makes us RUNNABLE again. */
    spin_unlock(&wq->lock);
    schedule();

    /* Woken.  Re-acquire the condition lock for the caller's recheck
     * loop.  IRQs are still off (schedule restored them to entry state).*/
    spin_lock(&wq->lock);
    self->wq = NULL;                            /* §M49 — no longer parked */
}

/* Wake helper — caller holds wq->lock.  `all` drains the whole queue;
 * otherwise just the head is released. */
static void wq_wake_locked(struct waitq* wq, int all) {
    struct task* t = wq->head;
    if (!t) return;

    if (all) {
        wq->head = NULL;
    } else {
        wq->head    = t->wq_next;
        t->wq_next  = NULL;
    }

    for (;;) {
        struct task* nxt = all ? t->wq_next : NULL;
        t->wq_next = NULL;
        t->wq      = NULL;                      /* §M49 */
        if (t->state == TASK_SLEEPING) {
            t->state = TASK_RUNNABLE;
            /* task_enqueue picks an affinity-legal CPU, inserts on its rq,
             * and sends a reschedule IPI if that CPU isn't us — so a wake
             * from another core lands the task promptly. */
            task_enqueue(t);
        }
        if (!all || !nxt) break;
        t = nxt;
    }
}

void waitq_wake_one(struct waitq* wq) { wq_wake_locked(wq, 0); }
void waitq_wake_all(struct waitq* wq) { wq_wake_locked(wq, 1); }

/* ------------------------------------------------------------------- */
/* Preemption hooks.                                                    */
/* ------------------------------------------------------------------- */

void schedule_request(void) {
    /* Per-CPU flag (M18.6.1).  Timer IRQ on CPU N sets CPU N's bit;
     * IRQ exit on CPU N consumes it.  Cross-CPU preempt IPI (vector
     * 0x41) sets the receiver's bit before schedule_check runs. */
    struct percpu* me = this_cpu();
    me->need_resched = 1;
    /* M31 — per-CPU liveness heartbeat for the softlockup detector.  This
     * runs from the timer IRQ, so it advances once per tick on every CPU
     * that is still taking ticks; a CPU wedged with IRQs off (spinlock
     * deadlock, IRQ storm) stops advancing it and the watchdog sweep on a
     * healthy CPU notices.  Just a monotonic counter — no lock needed. */
    me->ticks++;
}

void schedule_check(void) {
    struct percpu* me = this_cpu();
    if (!me->need_resched)    return;
    if (preempt_count() != 0) return;   /* hot path asked us to wait */

    me->need_resched = 0;
    /* §M49 — spend one quantum of the running task's budget.  This is the
     * only place time is charged, and it runs once per tick, so a
     * "quantum" is exactly one timer tick regardless of how often
     * schedule() is called for other reasons. */
    {
        struct task* cur = me->current;
        if (cur && !cur->is_idle) cur->deficit -= (int)TASK_WEIGHT_BASE;
    }
    /* §M49 — wake expired timed sleepers first (they may be the most
     * deserving thing to run), then balance, so a task pulled in this
     * pass is eligible for the very next pick instead of waiting a
     * whole tick. */
    sleep_sweep();
    load_balance_tick(me);
    schedule();
}

/* ------------------------------------------------------------------- */
/* Diagnostics.                                                         */
/* ------------------------------------------------------------------- */

void task_list(void) {
    if (!master_head) { kprintf("ps: no tasks\n"); return; }
    kprintf("PID  PPID  STATE  CPU  CPUMS  NAME\n");
    uint32_t fl = spin_lock_irqsave(&master_lock);
    struct task* t = master_head;
    do {
        int running = 0;
        int n = smp_ncpus();
        for (int i = 0; i < n; i++) {
            if (percpu_at(i) && percpu_at(i)->current == t) { running = 1; break; }
        }
        kprintf("%d   %d   %s    %d   %u   %s%s\n",
                t->pid, t->ppid, state_name(t->state), t->cpu_home,
                (unsigned)t->cpu_ms,        /* truncates past ~49 days — fine */
                t->name, running ? " (running)" : "");
        t = t->next;
    } while (t != master_head);
    spin_unlock_irqrestore(&master_lock, fl);
}

void task_for_each(task_iter_fn fn, void* ctx) {
    if (!fn || !master_head) return;
    /* Snapshot of "current at the moment of iteration."  Doesn't
     * promise the task is still current by the time fn() reads it
     * — that's a fundamentally racy notion on SMP.  Good enough for
     * /proc rendering. */
    struct task* me = task_current();
    uint32_t fl = spin_lock_irqsave(&master_lock);
    struct task* t = master_head;
    do {
        fn(t, t == me, ctx);
        t = t->next;
    } while (t != master_head);
    spin_unlock_irqrestore(&master_lock, fl);
}

int task_count(void) {
    if (!master_head) return 0;
    int n = 0;
    uint32_t fl = spin_lock_irqsave(&master_lock);
    struct task* t = master_head;
    do { n++; t = t->next; } while (t != master_head);
    spin_unlock_irqrestore(&master_lock, fl);
    return n;
}

struct task* task_find(int pid) {
    if (!master_head) return NULL;
    struct task* found = NULL;
    uint32_t fl = spin_lock_irqsave(&master_lock);
    struct task* t = master_head;
    do {
        if (t->pid == pid) { found = t; break; }
        t = t->next;
    } while (t != master_head);
    spin_unlock_irqrestore(&master_lock, fl);
    return found;
}

/* ------------------------------------------------------------------- */
/* Affinity (M18.6.3).                                                  */
/* ------------------------------------------------------------------- */

int task_set_affinity(struct task* t, uint32_t mask) {
    if (!t || mask == 0) return -1;
    /* If the task is currently on a rq, and we're restricting it
     * off that rq's CPU, move it to a CPU that's allowed.
     *
     * Strategy: remove from old rq (if cpu_home is no longer in the
     * mask), update mask, then re-enqueue via the regular pick path.
     * The task itself may be currently running — in that case the
     * mask only takes effect on the next schedule() that visits it,
     * which is fine. */
    int old_cpu = t->cpu_home;
    int needs_migration =
        (old_cpu >= 0) && ((mask & (1u << old_cpu)) == 0);

    if (needs_migration && t->state == TASK_RUNNABLE && !t->is_idle) {
        struct percpu* old_rq = percpu_at(old_cpu);
        if (old_rq) {
            uint32_t fl = spin_lock_irqsave(&old_rq->rq_lock);
            rq_remove_locked(old_rq, t);
            spin_unlock_irqrestore(&old_rq->rq_lock, fl);
        }
        t->cpu_mask = mask;
        t->cpu_home = -1;
        task_enqueue(t);
        return 0;
    }

    t->cpu_mask = mask;
    return 0;
}

/* §M49 — set scheduling priority.  Recomputing `deficit` from the new
 * weight immediately (rather than letting the old budget drain) makes the
 * change take effect on the current round, which is what someone typing
 * `nice` at a stuttering desktop expects. */
int task_set_nice(int pid, int nice) {
    struct task* t = task_find(pid);
    if (!t) return -1;
    if (nice < TASK_NICE_MIN) nice = TASK_NICE_MIN;
    if (nice > TASK_NICE_MAX) nice = TASK_NICE_MAX;

    /* The task's load contribution changes with its weight, so its
     * runqueue's published total has to be corrected under that queue's
     * lock — otherwise the balancer works from a figure that no longer
     * matches the queue's contents. */
    struct percpu* rq = (t->cpu_home >= 0) ? percpu_at(t->cpu_home) : NULL;
    uint32_t fl = 0;
    if (rq) fl = spin_lock_irqsave(&rq->rq_lock);
    int queued = (t->rq_next || t->rq_prev) ? 1 : 0;
    if (rq && queued) rq->rq_load -= task_load_contrib(t);
    t->nice       = nice;
    t->weight     = task_nice_to_weight(nice);
    t->deficit    = (int)t->weight;
    t->skips_left = 0;
    if (rq && queued) rq->rq_load += task_load_contrib(t);
    if (rq) spin_unlock_irqrestore(&rq->rq_lock, fl);

    task_notify_change();
    return 0;
}

uint32_t task_get_affinity(const struct task* t) {
    return t ? t->cpu_mask : 0;
}
