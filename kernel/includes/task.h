/* =============================================================================
 * task.h — kernel-mode task abstraction + scheduler.
 *
 * A task is one strand of execution that the kernel can switch between.
 * Each task carries:
 *   - a kernel stack (kmalloc'd, TASK_KSTACK_SZ)
 *   - saved ESP (the kernel-stack location to resume from)
 *   - state (RUNNABLE / SLEEPING / DEAD)
 *   - run-queue link (intrusive singly-linked circular list)
 *
 * As of M25 stage 1 a task MAY carry a private address space (`mm`, a
 * `struct vmm_space*`).  `mm == NULL` means "kernel thread": it runs in the
 * shared kernel page directory, exactly as every task did before M25.  A
 * user process sets `mm` to its own space; the scheduler loads that space's
 * CR3/TTBR0 on switch-in (and the kernel space on switch-out).
 *
 * Switching uses `context_switch` in switch.s (callee-saved push, swap
 * ESP, callee-saved pop, ret).  For a brand-new task, `task_spawn`
 * pre-builds the stack so the first `ret` lands in `task_trampoline`,
 * which `sti`s (so the new task is preemptible) and then calls the
 * caller-supplied entry.
 *
 * Scheduling shape (after §M13):
 *   - `task_yield()` is the cooperative way to give up the CPU.  It just
 *     calls `schedule()`.
 *   - `schedule()` picks the next RUNNABLE task and context-switches to
 *     it.  Holds the runqueue lock for the decision; lock is released by
 *     the now-current task on its way out of schedule().
 *   - PIT IRQ → `schedule_request()` sets a deferred flag.
 *   - IRQ-exit path → `schedule_check()` consults the flag (and
 *     preempt_count) and, if appropriate, calls schedule() from IRQ
 *     context.  Running schedule() only after pic_eoi guarantees the
 *     timer keeps firing on whoever runs next.
 * ============================================================================= */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <stddef.h>

/* Per-process address space (M25).  Opaque here — see vmm.h.  Forward
 * declared so `struct task` can hold one without pulling in the VMM. */
struct vmm_space;
/* Generic open-file object behind a descriptor (M25 stage 3/4) — see fd.h. */
struct ofile;

#define TASK_NAME_MAX  31
#define TASK_MAX_FDS   32       /* per-process file descriptors (0/1/2 = console) */
#define TASK_KSTACK_SZ 4096

enum task_state {
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_DEAD,
};

struct task {
    char     name[TASK_NAME_MAX + 1];
    int      pid;
    /* M27 — process model.  `ppid` is the parent's pid (a stable int, not
     * a pointer, so it never dangles when the parent is reaped).  On the
     * parent's reap its surviving children are re-parented to init
     * (task_reaper_pid()).  `exit_code` is recorded by task_exit_code();
     * `reap_owned` marks a task whose reap is owned by a subsystem (the
     * GUI reaps its window shells itself) — init's universal reaper skips
     * those so it never races the owner for the same struct. */
    int      ppid;
    int      exit_code;
    int      reap_owned;
    void*    start_arg;                 /* M22.7 — entry arg (task_start_arg) */
    enum task_state state;
    uintptr_t esp;                      /* saved kernel-stack pointer (HAL-typed) */
    void*    kstack_base;               /* kmalloc'd; freed at reap */
    /* M25 — private address space, or NULL for a kernel thread (shared
     * kernel page directory).  Zero-initialised by kcalloc at spawn, so
     * every task defaults to a kernel thread until a loader sets it. */
    struct vmm_space* mm;
    /* M25 stage 3/4 — per-process file-descriptor table.  Index → generic
     * open-file object (VFS file / shm / socket); fds 0/1/2 are the implicit
     * console (no object, handled in usyscall.c).  Zero-init by kcalloc. */
    struct ofile* fds[TASK_MAX_FDS];
    /* M25 stage 4 — bump cursor for anonymous / shm mmap VAs in this task's
     * user space.  Reset by the exec path; grows upward per mmap. */
    uintptr_t     mmap_cursor;
    /* Tier B — 1 for an INDEPENDENT user process (proc_spawn): it runs at
     * ring 3/EL0 as its own preemptible task, uses its own kernel stack for
     * privilege transitions (scheduler sets TSS.esp0 to its kstack top on x86),
     * and SYS_EXIT ends the task (task_exit) rather than teleporting back.  0
     * for kernel threads AND the excursion-model self-tests (proc_exec_elf),
     * which keep the fixed syscall stack + teleport-back. */
    int           user_task;
    /* M35 — thread: this task SHARES its `mm` with its creator (clone), so its
     * reap must NOT destroy the address space (the thread group still uses it).
     * 0 for a process that owns its mm; 1 for a cloned thread. */
    int           mm_shared;
    /* M35 TLS — user thread-local-storage pointer.  `has_tls` set once the
     * thread calls set_thread_area; `tls_base` is its %gs segment base (the
     * thread pointer).  The scheduler reloads this CPU's user-TLS descriptor
     * from `tls_base` on switch-in.  A TLS thread is pinned to its CPU. */
    int           has_tls;
    uintptr_t     tls_base;
    /* Master-list link (circular SLL of every alive task).  Walked by
     * ps / task_for_each / task_find.  Pre-M18.6.1 the scheduler
     * also walked this list; now per-CPU runqueues take that role and
     * `next` is purely diagnostic. */
    struct task* next;
    /* M14: optional per-task output binding.  When non-NULL, console.c
     * routes the running task's console_putchar bytes here instead of
     * (or in addition to) the global sinks.  Opaque `void*` so task.h
     * does not need to know about `struct vc`. */
    void*    out_console;
    /* M18.5: idle-task marker.  Idle tasks are skipped during normal
     * scheduling and only picked as a fallback when no other task is
     * runnable on this CPU.  Set via task_become_idle() (current task)
     * or by task_install_ap_idle() (AP bootstrap). */
    int      is_idle;
    /* M18.6.3 — CPU affinity mask.  Bit i set => task may run on CPU i.
     * Default 0xFFFFFFFF (any CPU; capped at ACPI_MAX_CPUS=32 today —
     * widening to a real cpuset_t is straightforward once we have
     * boards with >32 cores).  Scheduler skips tasks whose mask
     * excludes this_cpu_id; load balancer also skips them at steal
     * time so a pinned task never migrates off its allowed set. */
    uint32_t cpu_mask;
    /* M18.6.1 — per-CPU runqueue links.  When state==RUNNABLE and the
     * task is NOT the current of any CPU, it lives on cpu_home's
     * runqueue.  Idle tasks never live on a runqueue — they're picked
     * via percpu->idle directly.
     *   cpu_home   : which CPU's rq this task lives on (or -1).
     *   rq_next/rq_prev : doubly-linked list rooted at percpu->rq_head.
     */
    int      cpu_home;
    struct task* rq_next;
    struct task* rq_prev;
    /* Tier A.1 — wait-queue link.  When state==TASK_SLEEPING because the
     * task parked itself on a `struct waitq`, it hangs off that queue's
     * singly-linked list through `wq_next` (a task can be blocked on at
     * most one waitq at a time).  NULL otherwise.  See waitq.h. */
    struct task* wq_next;
    /* M22.3 — cooperative kill (the Linux kthread_stop contract: all
     * tasks are kernel threads today, so forced termination at an
     * arbitrary preemption point is unsafe — the victim might hold a
     * spinlock.  task_kill sets the flag; the task dies at its next
     * task_yield; CPU-bound workers poll task_should_stop()). */
    volatile int kill_pending;
    /* M22.3 — CPU time accounting: ms actually spent on a CPU.
     * `sched_in_ms` stamps switch-in; switch-out accumulates into
     * `cpu_ms`.  Feeds `ps` and the GUI task manager. */
    uint64_t cpu_ms;
    uint64_t sched_in_ms;
    /* M34 — POSIX signals.  `sig_pending` is a bitmask of posted signals;
     * `sig_handler[sig]` is SIG_DFL(0) / SIG_IGN(1) / a ring-3 handler address;
     * `sig_restorer` is the libc trampoline that issues SYS_SIGRETURN after a
     * handler returns.  Delivered on the return-to-user path (hal/x86/signal.c).
     * Zero-initialised by kcalloc → every task starts with default dispositions. */
    uint32_t  sig_pending;
    uintptr_t sig_handler[32];          /* NSIG (syscall.h) */
    uintptr_t sig_restorer;
};

/* Set up the scheduler and convert the current `kernel_main` context
 * into pid 0 (named "kernel").  Must be called once, after kmalloc is
 * up, before any `task_spawn`. */
void task_init(void);

/* AP-side bootstrap (M18.5).  Each AP calls this from its C entry to
 * synthesize an idle task for its current execution context, splice
 * it into the global ring, and stamp it as this CPU's current + idle.
 * After this returns, the AP is a full scheduler participant — its
 * LAPIC timer can drive normal preemption. */
void task_install_ap_idle(void);

/* Mark the currently-running task as this CPU's idle task (M18.5).
 * Called by kernel_main on the BSP right before it enters its
 * halt+yield loop, so the scheduler treats pid 0 as the BSP's
 * fallback rather than a competitor for the runqueue. */
void task_become_idle(void);

/* Create a new kernel-mode task.  `entry` runs on its own stack.  Args
 * are not passed (the entry function may consult globals / config).
 * Returns the new task (RUNNABLE in the queue) or NULL on OOM.  The new
 * task's parent (ppid) is the caller — it joins the caller's subtree. */
struct task* task_spawn(const char* name, void (*entry)(void));

/* M27 — like task_spawn, but the new task is INDEPENDENT: its parent is
 * init, not the caller.  Use it for daemons / background workers that must
 * outlive whoever started them and must NOT be swept up by a kill_tree on
 * the caller.  (The building block for M29 services.) */
struct task* task_spawn_detached(const char* name, void (*entry)(void));

/* M22.7 — spawn with a start argument, retrievable by the entry through
 * task_start_arg().  The arg is set before the task is enqueued, so it is
 * safe even if another CPU runs the task immediately.  The GUI uses it to
 * hand each per-app "host" task the window/app it should drive. */
struct task* task_spawn_arg(const char* name, void (*entry)(void), void* arg);
void* task_start_arg(void);

/* M22.7 — spawn with an explicit parent pid (>= 0), or the caller (< 0).
 * The GUI uses it to parent a launched terminal's shell to the desktop
 * session instead of the transient launcher task. */
struct task* task_spawn_under(const char* name, void (*entry)(void), int ppid);

/* Cooperative yield.  No-op if we're the only runnable task.  Returns
 * when this task is scheduled again. */
void task_yield(void);

/* Cooperative millisecond sleep — yields (hlt-until-IRQ + task_yield) until
 * `ms` of wall time has elapsed on the timer, so other tasks run and the CPU
 * idles low-power.  NOT a precise timer wakeup (it polls timer_ticks_ms at the
 * tick granularity) and returns early if task_should_stop() fires, so a long
 * sleep still honours the kthread kill contract.  Used by services (M29),
 * cron (M30) and the watchdog (M31); a true timed blocking-sleep can replace
 * the internals later without changing callers. */
void task_msleep(uint32_t ms);

/* The currently scheduled task (or NULL very early in boot, before
 * task_init has run).  Read-only view; do not retain across blocking
 * calls — the next yield may change `current`. */
struct task* task_current(void);

/* Bind / unbind an output console to `t`.  When set, console_putchar
 * (and thus kprintf) routes that task's output to this opaque pointer
 * via the per-task hook installed by vc_init.  Pass NULL to clear. */
void task_set_out_console(struct task* t, void* console);

/* Called by the arch-specific task trampoline on the first context
 * switch into a brand-new task.  Releases the runqueue lock that
 * the spawning schedule() acquired and never got to release (the
 * lock-handoff trick — see task.c header for the rationale).
 * Trampoline must call this BEFORE sti'ing and calling the entry. */
void task_finish_first_switch(void);

/* Mark the current task DEAD and never return — the scheduler picks the
 * next runnable task on the next yield cycle.  task_exit() is exit code 0;
 * task_exit_code() records a code first (shown by ps, logged by init). */
void task_exit(void) __attribute__((noreturn));
void task_exit_code(int code) __attribute__((noreturn));

/* M27 — process model.
 *
 * task_start_init() spawns the init task (the always-on reaper + orphan
 * adopter).  Call once from kernel_main after task_init + SMP are up; its
 * pid becomes task_reaper_pid().  init reaps every DEAD task that is not
 * reap_owned, so an exited kernel thread never leaks as a zombie the way
 * it could before (reaping used to depend on the Task Manager being open).
 *
 * task_kill_tree() cooperatively kills `pid` AND all its descendants (the
 * kthread contract still holds — each victim dies at its next yield).  Use
 * it when a subtree should go down together (e.g. closing a shell window
 * takes anything that shell spawned with it).
 *
 * task_set_reap_owned() lets a subsystem claim a task's reap so init keeps
 * its hands off it. */
void task_start_init(void);
int  task_reaper_pid(void);
int  task_kill_tree(int pid);
void task_set_reap_owned(struct task* t, int owned);

/* Tier A.2 — block until a child exits, POSIX waitpid-shaped.
 *
 *   pid > 0  : wait for that specific child (must be a direct child).
 *   pid <= 0 : wait for ANY direct child.
 *
 * Blocks (on the internal child-exit wait-queue) until a matching child is
 * DEAD, then records its exit code in *code (if non-NULL), reaps it, and
 * returns its pid.  Returns -1 immediately if the caller has no matching
 * live-or-dead child to wait for.  This is the "downward" answer to child
 * death — the building block M29's service supervisor uses to notice a
 * service crash and restart it.  Woken by task_exit_code. */
int task_wait(int pid, int* code);

/* Internal scheduler entry — pick next RUNNABLE task and context_switch
 * to it.  Both task_yield and the IRQ-driven preemption path call this.
 * Acquires the runqueue lock internally; safe to call with IRQs already
 * disabled. */
void schedule(void);

/* Called from IRQ handlers (e.g. pit_irq) to ask for a reschedule once
 * the IRQ has finished its EOI.  Cheap: just sets a flag. */
void schedule_request(void);

/* Called from the IRQ-exit path AFTER pic_eoi.  If a reschedule is
 * pending AND preemption is enabled (preempt_count==0) AND we have an
 * alternative task to run, this performs the context switch from IRQ
 * context. */
void schedule_check(void);

/* Diagnostic — dumps every task on the run-queue to the console.  Used
 * by the `ps` shell command. */
void task_list(void);

/* Number of tasks currently in the queue (RUNNABLE + SLEEPING). */
int  task_count(void);

/* Iterate every task in the run queue.  `is_current` is non-zero for the
 * currently scheduled task.  Used by procfs to render `/proc/tasks`. */
typedef void (*task_iter_fn)(const struct task* t, int is_current, void* ctx);
void task_for_each(task_iter_fn fn, void* ctx);

/* M18.6.3 — find a task by pid.  Returns NULL if no live task carries
 * that pid.  Used by `taskset` and a future `kill`.  Walks the global
 * task list under the master scheduler lock. */
struct task* task_find(int pid);

/* M22.3 — request cooperative termination of `pid`.  Returns 0 if the
 * flag was set, -1 if no such task or it is protected (pid 0, idle
 * tasks).  The task exits at its next voluntary yield point — tasks
 * parked in vc_getchar/keyboard_getchar die within one timer tick;
 * CPU-bound kernel threads must poll task_should_stop() (the kthread
 * rule).  Pair with task_reap() to reclaim struct + stack. */
int  task_kill(int pid);

/* For CPU-bound kernel threads: non-zero once task_kill was called on
 * the calling task — poll it in long-running loops and return/exit. */
int  task_should_stop(void);

/* Reclaim a DEAD task: unlink from the master ring, free kstack +
 * struct.  Returns 0 on success, -1 if the pid is missing, not DEAD
 * yet, or still current on some CPU (caller retries later). */
int  task_reap(int pid);

/* M22.4 — task-lifecycle change notification.  The hook fires (from
 * whatever context mutated the task set: spawn, kill, exit, reap —
 * possibly IRQ or the dying task itself) whenever the task list or a
 * task's liveness changed.  Keep it trivial: set a flag, wake a
 * consumer.  One consumer today: the GUI compositor uses it to refresh
 * the Task Manager within one frame of a program closing instead of
 * waiting for the 1 Hz tick.  Pass NULL to uninstall. */
void task_set_change_hook(void (*fn)(void));

/* M18.6.3 — set / get task affinity.  Mask of allowed CPU bits;
 * passing 0 is rejected (would mean "may run nowhere").  If the
 * caller restricts a task off its current home CPU, the scheduler
 * naturally re-homes it on the next pick (the load-balancer steal
 * path respects affinity). */
int      task_set_affinity(struct task* t, uint32_t mask);
uint32_t task_get_affinity(const struct task* t);

#endif
