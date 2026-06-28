/* =============================================================================
 * task.c — kernel task table + preemptive round-robin scheduler.
 *
 * Run-queue is a circular singly-linked list of `struct task`, rooted at
 * `current`.  `schedule()` walks `current->next` looking for the next
 * RUNNABLE task; if none other is found, it returns immediately.
 * Otherwise it calls `context_switch` (switch.s) to swap registers and
 * stack.
 *
 * Two entry points reach `schedule()`:
 *   - Cooperative:   task_yield() → schedule()
 *   - Preemptive:    pit_irq() → schedule_request() sets a deferred flag;
 *                    isr_handler, after pic_eoi, calls schedule_check()
 *                    which honors the flag (if preempt_count == 0).
 *
 * Why the deferred flag instead of context-switching directly from
 * pit_irq?  If we switched away mid-handler, pic_eoi would never fire
 * on that IRQ — the PIC would consider IRQ0 still in-service and stop
 * delivering further timer interrupts to the new task.  The flag lets
 * us complete the EOI on the OLD task's stack, then pivot.
 *
 * Synchronization (UP).  We rely on `cli` to keep the runqueue stable
 * against the timer IRQ (the only other thing that could mutate it).
 * Every runqueue mutator brackets its work in pushf/cli ... popf-equiv.
 * No spinlock_t is needed here — UP has no second CPU to spin against.
 * (`spinlock_t` from lock.h is still used by other subsystems that need
 * the API shape for the SMP future; the scheduler doesn't need that
 * shape because schedule() itself is the thing other code synchronizes
 * around, not the other way.)
 *
 * Bootstrap subtlety: pid 0 is the original kernel_main flow.  We
 * synthesize its `struct task` without allocating a stack (we already
 * have one) — its `esp` field is left zero and gets populated by the
 * very first `context_switch` that swaps away from it.
 *
 * Stack layout for a fresh task (so context_switch's pop+ret lands at
 * task_trampoline, which then `sti`s and jumps to the entry):
 *
 *   [low addr]
 *     ebx = entry      \   ebx-via-spawn carries the entry pointer into
 *     esi = 0           |  the trampoline (which moves it out before the
 *     edi = 0           |  call); the rest of the callee-saved regs are
 *     ebp = 0          /   zeroed to keep gdb traces tidy
 *     task_trampoline  <-- return addr; `ret` jumps here on first switch
 *     0xDEADBEEF       <-- guard if entry returns despite task_exit
 *   [high addr / stack top]
 *
 * `task->esp` is set to point at the `ebx` slot.
 *
 * IRQ-on / IRQ-off invariants across context_switch:
 *
 *   schedule() entered IF=1 (cooperative):
 *     cli → fl saved with IF=1 → switch out → ... → switch back in →
 *     schedule() restores IF=1.  Caller's view: nothing changed.
 *
 *   schedule() entered IF=0 (called from IRQ via schedule_check):
 *     cli (no-op) → fl saved with IF=0 → switch out → ... → switch back
 *     in → schedule() does NOT sti (fl had IF=0).  Returns to IRQ-exit
 *     path, which iret restores the pre-IRQ EFLAGS (and with it, the
 *     pre-IRQ IF).
 *
 *   brand-new task first run:
 *     reached via context_switch's `ret` → task_trampoline.  We never
 *     went through a schedule() to get here, so we explicitly `sti` in
 *     the trampoline to let interrupts start arriving at our entry.
 * ============================================================================= */

#include "task.h"
#include "kmalloc.h"
#include "printf.h"
#include "lock.h"
#include "hal_api.h"
#include "percpu.h"
#include <stddef.h>
#include <stdint.h>

/* The arch-specific context-switch routine.  On x86 see
 * kernel/hal/x86/switch.s; on x64/aarch64 the equivalent ships with
 * those ports.  Signature is intentionally `uintptr_t` so the same
 * declaration works on every 32/64-bit arch. */
extern void context_switch(uintptr_t* save_esp_to, uintptr_t new_esp);

/* Run-queue (M18): a single circular singly-linked list shared by
 * every CPU.  `head` is the diagnostic anchor; the per-CPU pointer
 * to the currently-running task lives in `this_cpu()->current`.
 *
 * A single global runqueue with a spinlock is the simplest correct
 * SMP design — fine until contention shows up.  Per-CPU runqueues +
 * a load-balancer is an M19/M18.5 follow-up (see PLAN.md).  Today's
 * lock contention is invisible on -smp 2 because both CPUs spend
 * most of their time in their idle tasks. */
static struct task* head      = NULL;
static int          next_pid  = 0;
static spinlock_t   runqueue_lock = SPINLOCK_INIT;


/* Deferred-reschedule flag.  Set by schedule_request() (from pit_irq),
 * consumed by schedule_check() (from isr_handler after pic_eoi).  Reader
 * and writer always run with IF=0 (writer is in IRQ context; reader is
 * in IRQ-exit), so no torn 32-bit reads. */
static volatile int need_resched = 0;

/* ------------------------------------------------------------------- */
/* IRQ-save / IRQ-restore — go through the HAL (M17).                   */
/*                                                                      */
/* Pre-M17 we inlined the pushf/cli pair here for "compiler-friendly    */
/* hot path" reasons; that's now a HAL call.  Same single instruction   */
/* generated on x86, and the same source compiles unchanged on x64 /    */
/* aarch64 once their HAL ships.                                         */
/* ------------------------------------------------------------------- */

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

/* Insert `t` into the circular list.  Caller must hold runqueue_lock
 * and have IRQs disabled.  If the queue is empty, `t` becomes the
 * sole occupant AND the BSP's current task — that's the bootstrap
 * path for pid 0. */
static void runqueue_insert_locked(struct task* t) {
    if (!head) {
        t->next = t;
        head    = t;
        /* Bootstrap: BSP picks up this first task as its current. */
        this_cpu()->current = t;
    } else {
        t->next    = head->next;
        head->next = t;
    }
}

/* Append `t` to the ring without touching any `current` pointer.
 * Used by AP bootstrap (the AP wants its own idle as `current` but
 * the global ring already exists, so the empty-queue branch above
 * would be wrong). */
static void runqueue_append_locked(struct task* t) {
    if (!head) {
        /* Same as empty-queue path of insert; shouldn't happen on
         * AP, but stay safe. */
        t->next = t;
        head    = t;
    } else {
        t->next    = head->next;
        head->next = t;
    }
}

/* ------------------------------------------------------------------- */
/* Init.                                                                */
/* ------------------------------------------------------------------- */

/* Generic per-CPU idle entry — halt forever until preempted away.
 * Used by the BSP idle thread synthesized at task_init time; APs use
 * their own ap_main loop body which serves the same role (idle = the
 * already-running context). */
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

    uint32_t fl = spin_lock_irqsave(&runqueue_lock);
    runqueue_insert_locked(t0);
    spin_unlock_irqrestore(&runqueue_lock, fl);

    /* Synthesize BSP idle (M18.5).  Distinct from kernel_main: when
     * the kernel_main flow eventually task_exit()s, the scheduler
     * needs a non-DEAD task to fall back to or BSP would halt
     * forever — and that halts PIT delivery, which freezes time on
     * every other CPU too.  Spawning idle here makes BSP a proper
     * scheduler citizen from the start. */
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

        uint32_t fl2 = spin_lock_irqsave(&runqueue_lock);
        runqueue_append_locked(bsp_idle);
        this_cpu()->idle = bsp_idle;
        spin_unlock_irqrestore(&runqueue_lock, fl2);
    }

    kprintf("task: pid 0 (kernel) installed\n");
}

/* AP-side idle-task bootstrap (M18.5).  Each AP calls this from its
 * C entry to (1) synthesize a task struct for its currently-running
 * context (the ap_main flow), (2) splice that task into the global
 * ring, and (3) stamp it as this CPU's current+idle pointers.
 *
 * Like `task_init`, we don't allocate a stack — the AP is already
 * running on the per-AP stack `smp_boot_aps` allocated for it.
 * `esp` is left zero; the first time the scheduler switches AWAY
 * from this idle task, `context_switch` populates it.
 *
 * After this returns, the AP is a full scheduler participant: when
 * its LAPIC timer fires the schedule_check path picks up any
 * RUNNABLE non-idle task in the ring. */
void task_install_ap_idle(void) {
    struct task* idle = (struct task*)kcalloc(1, sizeof(struct task));
    if (!idle) {
        kprintf("task: failed to allocate AP idle\n");
        return;
    }
    /* Name encodes CPU index so `ps` reads sensibly: "idle-N". */
    char buf[TASK_NAME_MAX + 1] = "idle-";
    int cpu = this_cpu_id();
    /* Tiny manual itoa, single digit covers up to CPU 9 — fine for
     * our ACPI_MAX_CPUS = 32 cap (CPUs 10+ get "idle-1"+digit). */
    int pos = 5;
    if (cpu >= 10) { buf[pos++] = '0' + (cpu / 10); cpu %= 10; }
    buf[pos++] = '0' + cpu;
    buf[pos]   = 0;
    str_copy_n(idle->name, buf, sizeof idle->name);

    idle->pid         = next_pid++;
    idle->state       = TASK_RUNNABLE;
    idle->esp         = 0;
    idle->kstack_base = NULL;     /* AP stack owned by smp.c, not us */
    idle->is_idle     = 1;

    uint32_t fl = spin_lock_irqsave(&runqueue_lock);
    runqueue_append_locked(idle);
    this_cpu()->current = idle;
    this_cpu()->idle    = idle;
    spin_unlock_irqrestore(&runqueue_lock, fl);
}

void task_become_idle(void) {
    uint32_t fl = spin_lock_irqsave(&runqueue_lock);
    struct task* me = this_cpu()->current;
    if (me) {
        me->is_idle = 1;
        this_cpu()->idle = me;
    }
    spin_unlock_irqrestore(&runqueue_lock, fl);
}

/* ------------------------------------------------------------------- */
/* Spawn.                                                               */
/*                                                                      */
/* The arch-specific stack pre-build (trampoline + register layout for  */
/* the first context_switch) lives in `kernel/hal/<arch>/task_arch.c`   */
/* behind `hal_task_init_stack` (M17).  This file only owns the          */
/* portable bits: kmalloc the stack, fill the `struct task`, link into  */
/* the runqueue.                                                        */
/* ------------------------------------------------------------------- */

struct task* task_spawn(const char* name, void (*entry)(void)) {
    struct task* t = (struct task*)kcalloc(1, sizeof(struct task));
    if (!t) return NULL;

    void* stack = kmalloc(TASK_KSTACK_SZ);
    if (!stack) { kfree(t); return NULL; }

    str_copy_n(t->name, name, sizeof t->name);
    t->pid         = next_pid++;
    t->state       = TASK_RUNNABLE;
    t->esp         = hal_task_init_stack((char*)stack + TASK_KSTACK_SZ, entry);
    t->kstack_base = stack;

    uint32_t fl = spin_lock_irqsave(&runqueue_lock);
    runqueue_insert_locked(t);
    spin_unlock_irqrestore(&runqueue_lock, fl);

    return t;
}

/* ------------------------------------------------------------------- */
/* Schedule + yield + exit.                                             */
/* ------------------------------------------------------------------- */

/* Core scheduler — picks the next RUNNABLE task for THIS CPU and
 * context-switches.  MUST be called with the runqueue lock held +
 * IRQs disabled.  Returns with both invariants intact; caller
 * releases the lock.
 *
 * SMP shape: we skip tasks that are RUNNABLE but currently `current`
 * on ANOTHER CPU.  Cheap to detect because every CPU stamps its
 * current pointer in this_cpu()->current and we can scan all
 * per-CPU slots in O(ncpus).  Tracked under PLAN §M18 follow-ups
 * (per-CPU runqueue would eliminate the scan entirely). */
static int task_running_elsewhere(struct task* t) {
    for (int i = 0; i < smp_ncpus(); i++) {
        struct percpu* p = percpu_at(i);
        if (p && p != this_cpu() && p->current == t) return 1;
    }
    return 0;
}

/* Find the next runnable non-idle task in the ring (round-robin
 * starting from prev->next).  Skip:
 *   - tasks that aren't RUNNABLE,
 *   - tasks currently `current` on another CPU,
 *   - idle tasks (those are pure fallbacks).
 * Returns NULL if no eligible task exists. */
static struct task* pick_next_locked(struct task* prev) {
    struct task* t = prev->next;
    while (t != prev) {
        if (t->state == TASK_RUNNABLE &&
            !t->is_idle &&
            !task_running_elsewhere(t)) {
            return t;
        }
        t = t->next;
    }
    /* The walk skipped prev itself.  Treat prev as a candidate too —
     * if we're already on a non-idle RUNNABLE task and nothing else
     * is eligible, staying put is the right answer. */
    return NULL;
}

static void schedule_locked(void) {
    struct percpu* me = this_cpu();
    struct task* prev = me->current;
    if (!prev) return;

    /* First preference: any RUNNABLE non-idle task in the ring. */
    struct task* next = pick_next_locked(prev);

    if (!next) {
        /* No real work found.  If prev is itself a working task and
         * still RUNNABLE, stay put — pointless to bounce into idle
         * and back on the next tick. */
        if (prev->state == TASK_RUNNABLE && !prev->is_idle) return;

        /* Otherwise fall back to this CPU's idle.  If we don't have
         * an idle (BSP before task_become_idle, e.g.) and prev is
         * unusable, there's literally nothing we can do — return
         * and let the caller (likely IRQ exit) iret back to whatever
         * was running.  Won't make progress but won't crash. */
        struct task* idle = me->idle;
        if (!idle || idle->state != TASK_RUNNABLE) return;
        if (idle == prev) return;
        next = idle;
    }

    if (next == prev) return;

    me->current = next;
    context_switch(&prev->esp, next->esp);
    /* When `prev` is later scheduled back in, control resumes here.
     * IRQs are still off; caller restores IF and releases the lock. */
}

void schedule(void) {
    uint32_t fl = spin_lock_irqsave(&runqueue_lock);
    schedule_locked();
    spin_unlock_irqrestore(&runqueue_lock, fl);
}

void task_yield(void) {
    schedule();
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

/* Lock-handoff finisher for brand-new tasks.  See task.h. */
void task_finish_first_switch(void) {
    spin_unlock(&runqueue_lock);
}

void task_exit(void) {
    /* Mark ourselves DEAD then call schedule.  No future selection will
     * pick a DEAD task, so schedule_locked won't context-switch back. */
    uint32_t fl = spin_lock_irqsave(&runqueue_lock);
    this_cpu()->current->state = TASK_DEAD;
    schedule_locked();
    spin_unlock_irqrestore(&runqueue_lock, fl);
    /* Unreachable if at least one other RUNNABLE task exists.  Halt
     * forever if somehow we get back — the system has no one to run. */
    for (;;) { hal_intr_disable(); hal_cpu_halt(); }
}

/* ------------------------------------------------------------------- */
/* Preemption hooks.                                                    */
/* ------------------------------------------------------------------- */

void schedule_request(void) {
    need_resched = 1;
}

void schedule_check(void) {
    if (!need_resched)        return;
    if (preempt_count() != 0) return;   /* hot path asked us to wait */

    need_resched = 0;
    schedule();
}

/* ------------------------------------------------------------------- */
/* Diagnostics.                                                         */
/* ------------------------------------------------------------------- */

void task_list(void) {
    if (!head) { kprintf("ps: no tasks\n"); return; }
    kprintf("PID  STATE  NAME\n");
    struct task* t = head;
    do {
        /* "running" if ANY CPU currently has us scheduled — useful on
         * SMP where it's not just BSP. */
        int running = 0;
        for (int i = 0; i < smp_ncpus(); i++) {
            if (percpu_at(i) && percpu_at(i)->current == t) { running = 1; break; }
        }
        kprintf("%d   %s    %s%s\n",
                t->pid, state_name(t->state), t->name,
                running ? " (running)" : "");
        t = t->next;
    } while (t != head);
}

void task_for_each(task_iter_fn fn, void* ctx) {
    if (!fn || !head) return;
    struct task* me = task_current();
    struct task* t = head;
    do {
        fn(t, t == me, ctx);
        t = t->next;
    } while (t != head);
}

int task_count(void) {
    if (!head) return 0;
    int n = 0;
    struct task* t = head;
    do { n++; t = t->next; } while (t != head);
    return n;
}
