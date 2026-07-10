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
 *   Load balance runs every LOAD_BALANCE_INTERVAL_MS on the local
 *   tick: if this CPU's rq is empty, scan peers for the longest queue
 *   and steal one task whose affinity allows running here.  Then send
 *   a reschedule IPI to ourselves only if the steal woke us up from
 *   idle and we want immediate pickup (the natural next local tick is
 *   usually fine and avoids self-IPI overhead).
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
#include "kmalloc.h"
#include "printf.h"
#include "lock.h"
#include "hal_api.h"
#include "percpu.h"
#include "smp.h"
#include "timer.h"          /* M22.3: per-task CPU-time accounting */
#include "vmm.h"            /* M25: per-process address-space switch */
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
}

/* Pick the lightest-loaded online CPU among those allowed by `mask`.
 * Tiebreak: prefer this_cpu_id so newly spawned tasks land here. */
static int pick_lightest_cpu(uint32_t mask) {
    int self = this_cpu_id();
    int best = -1;
    int best_load = 0x7FFFFFFF;
    int n = smp_ncpus();
    for (int i = 0; i < n; i++) {
        if ((mask & (1u << i)) == 0) continue;
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) continue;
        int load = p->rq_count;
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
                                 int ppid_override, void* arg) {
    struct task* t = (struct task*)kcalloc(1, sizeof(struct task));
    if (!t) return NULL;

    void* stack = kmalloc(TASK_KSTACK_SZ);
    if (!stack) { kfree(t); return NULL; }
    t->start_arg = arg;

    str_copy_n(t->name, name, sizeof t->name);
    t->pid         = next_pid++;
    /* M27 — parent: an explicit override (detached → init), else whoever
     * called (or pid 0 very early in boot, before there is a `current`). */
    if (ppid_override >= 0) {
        t->ppid    = ppid_override;
    } else {
        struct task* cur = task_current();
        t->ppid    = cur ? cur->pid : 0;
    }
    t->state       = TASK_RUNNABLE;
    t->esp         = hal_task_init_stack((char*)stack + TASK_KSTACK_SZ, entry);
    t->kstack_base = stack;
    t->cpu_mask    = 0xFFFFFFFFu;
    t->cpu_home    = -1;
    t->rq_next = t->rq_prev = NULL;

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
    return spawn_common(name, entry, -1, NULL);  /* parent = caller */
}

/* M22.7 — spawn with a start argument (read via task_start_arg in the
 * entry).  Parent = caller.  Used by the GUI to hand each app-host task
 * the app it should run. */
struct task* task_spawn_arg(const char* name, void (*entry)(void), void* arg) {
    return spawn_common(name, entry, -1, arg);
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
    return spawn_common(name, entry, g_init_pid, NULL);
}

/* M22.7 — spawn with an EXPLICIT parent pid (>= 0), or the caller when
 * ppid < 0.  Lets the GUI parent a launched terminal's shell to the desktop
 * (session) rather than to the transient launcher task that created it. */
struct task* task_spawn_under(const char* name, void (*entry)(void), int ppid) {
    return spawn_common(name, entry, ppid, NULL);
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
    struct task* t = head;
    do {
        if (t->state == TASK_RUNNABLE &&
            !t->is_idle &&
            (t->cpu_mask & (1u << self)) &&
            !task_running_elsewhere(t)) {
            return t;
        }
        t = t->rq_next;
    } while (t != head);
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
 * Returns the task with rq_lock dropped (caller re-acquires if
 * needed).  Caller does NOT hold any lock entering. */
static struct task* load_steal_one(struct percpu* victim) {
    int self = this_cpu_id();
    uint32_t fl = spin_lock_irqsave(&victim->rq_lock);
    struct task* head = victim->rq_head;
    struct task* found = NULL;
    if (head) {
        struct task* t = head;
        do {
            if (t->state == TASK_RUNNABLE &&
                !t->is_idle &&
                t != victim->current &&
                (t->cpu_mask & (1u << self))) {
                found = t;
                break;
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

/* Try to steal one task onto this_cpu's rq.  Returns 1 if we got
 * one, 0 otherwise.  Caller must NOT hold this CPU's rq_lock. */
static int load_balance_pull(void) {
    int self = this_cpu_id();
    int n = smp_ncpus();
    /* Find the busiest peer (snapshot read of rq_count without the
     * lock — races are fine: worst case we pick a slightly-less-busy
     * CPU, which still beats spinning idle). */
    int best = -1;
    int best_load = 0;
    for (int i = 0; i < n; i++) {
        if (i == self) continue;
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) continue;
        if (p->rq_count > best_load) {
            best = i;
            best_load = p->rq_count;
        }
    }
    if (best < 0 || best_load <= 0) return 0;

    struct task* stolen = load_steal_one(percpu_at(best));
    if (!stolen) return 0;

    struct percpu* mine = this_cpu();
    uint32_t fl = spin_lock_irqsave(&mine->rq_lock);
    rq_insert_tail_locked(mine, stolen);
    spin_unlock_irqrestore(&mine->rq_lock, fl);
    return 1;
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

    if (prev->state == TASK_RUNNABLE && !prev->is_idle &&
        prev->cpu_home == this_cpu_id()) {
        rq_rotate_to_tail_locked(me, prev);
    }

    struct task* next = pick_next_local_locked(me);

    if (!next) {
        /* No candidate on local rq.  Drop the lock briefly to try a
         * load-balance steal — can't hold rq_lock while taking
         * another CPU's lock (would risk deadlock). */
        spin_unlock(&me->rq_lock);
        int stole = load_balance_pull();
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
        uint64_t now = timer_ticks_ms();
        prev->cpu_ms += now - prev->sched_in_ms;
        next->sched_in_ms = now;
    }

    me->current = next;
    /* M25 — switch to next's address space before swapping stacks.  For a
     * kernel thread (mm == NULL) this targets the shared kernel directory,
     * and vmm_space_switch skips the CR3 reload when it's already loaded —
     * so kernel-thread → kernel-thread switches stay free (no TLB flush).
     * Both stacks live in the kernel identity map, which every space keeps
     * mapped, so doing this before context_switch is safe. */
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
    schedule();
}

int task_should_stop(void) {
    struct task* self = task_current();
    return self ? self->kill_pending : 0;
}

int task_kill(int pid) {
    if (pid == 0) return -1;                 /* pid 0 = kernel/BSP idle */
    struct task* t = task_find(pid);
    if (!t || t->is_idle || t->state == TASK_DEAD) return -1;
    t->kill_pending = 1;
    task_notify_change();                    /* M22.4 — liveness will change */
    return 0;
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
        /* Poll at the timer's pace: hlt until the next IRQ, then yield.
         * DEAD tasks are rare events, so a ~100 Hz sweep is effectively
         * event-driven while costing nothing when the system is quiet
         * (mirrors the compositor's idle loop). */
        hal_cpu_idle();
        task_yield();
    }
}

void task_start_init(void) {
    if (g_init_pid) return;                  /* singleton */
    struct task* t = task_spawn("init", init_entry);
    if (!t) { kprintf("task: FATAL — init spawn failed\n"); return; }
    g_init_pid = t->pid;
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

    /* Mark DEAD under master_lock so iterators see a consistent state. */
    uint32_t mfl = spin_lock_irqsave(&master_lock);
    self->exit_code = code;                  /* M27 — recorded for reap/ps */
    self->state = TASK_DEAD;
    spin_unlock_irqrestore(&master_lock, mfl);

    task_notify_change();                    /* M22.4 — went DEAD */

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

    /* Unreachable if any other task exists.  Halt-forever fallback
     * only if literally nothing else can run. */
    for (;;) { hal_intr_disable(); hal_cpu_halt(); }
}

/* ------------------------------------------------------------------- */
/* Preemption hooks.                                                    */
/* ------------------------------------------------------------------- */

void schedule_request(void) {
    /* Per-CPU flag (M18.6.1).  Timer IRQ on CPU N sets CPU N's bit;
     * IRQ exit on CPU N consumes it.  Cross-CPU preempt IPI (vector
     * 0x41) sets the receiver's bit before schedule_check runs. */
    this_cpu()->need_resched = 1;
}

void schedule_check(void) {
    struct percpu* me = this_cpu();
    if (!me->need_resched)    return;
    if (preempt_count() != 0) return;   /* hot path asked us to wait */

    me->need_resched = 0;
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

uint32_t task_get_affinity(const struct task* t) {
    return t ? t->cpu_mask : 0;
}
