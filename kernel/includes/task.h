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
#include "hal_api.h"       /* HAL_FPU_STATE_SIZE — per-task FPU/SIMD blob */

/* Per-process address space (M25).  Opaque here — see vmm.h.  Forward
 * declared so `struct task` can hold one without pulling in the VMM. */
struct vmm_space;
struct waitq;      /* §M49 — task.wq back-pointer; defined in waitq.h */
/* Generic open-file object behind a descriptor (M25 stage 3/4) — see fd.h. */
struct ofile;

#define TASK_NAME_MAX  31
#define TASK_MAX_FDS   32       /* per-process file descriptors (0/1/2 = console) */
/* Per-task kernel stack.  4 KiB was too small once ring-3 programs make
 * deep-call syscalls: a NetSurf launched from the Start menu runs on a spawned
 * task, and its DOSGUI_PRESENT → gui_window_blit → gfx_blit (a full-window
 * composite) + the ELF/ld.so exec path overflowed a 4 KiB stack and corrupted
 * memory (the desktop froze).  16 KiB matches the headroom the boot task's
 * stack has (where the same browser ran fine). */
#define TASK_KSTACK_SZ 16384

/* §M49 — full scale of `task.demand`: one task that wants an entire CPU.
 * Expressed as a percentage so a runqueue's summed load reads directly as
 * "how many cores' worth of work is queued here" (250 = two and a half). */
#define TASK_DEMAND_MAX 100

/* §M49 — scheduling weight of a nice-0 task.  A weight of 2*BASE means
 * twice the CPU quanta per round, BASE/2 means half.  Kept at 100 so a
 * weight reads directly as a percentage of the default share. */
#define TASK_WEIGHT_BASE 100
#define TASK_NICE_MIN   (-20)
#define TASK_NICE_MAX     19

/* Map a nice value to a scheduling weight.  Returns TASK_WEIGHT_BASE for
 * nice 0. */
uint32_t task_nice_to_weight(int nice);

/* §M49 — set a task's nice value (clamped to the legal range) and derive
 * its weight.  Returns 0 on success, -1 if no such task.  Lowering nice
 * (more CPU) is unprivileged here: d-os has no multi-user model yet
 * (§M32), so there is nothing to protect it from. */
int task_set_nice(int pid, int nice);

enum task_state {
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_DEAD,
};

struct task {
    char     name[TASK_NAME_MAX + 1];

    /* §M40 — environment variables handed to the NEXT exec on this task
     * ("KEY=VALUE" each, empty slot = none).  Per-launch data that cannot live
     * in the static default env: a Wayland client needs WAYLAND_SOCKET=<fd>,
     * and a Mesa client also needs LIBGL_DRIVERS_PATH — which is why this is a
     * small array rather than the single slot it started as.  Set with
     * proc_set_exec_env(); build_initial_stack consumes and clears them, so
     * they can never leak into an unrelated later exec. */
#define TASK_EXEC_ENV_MAX 4
    char exec_extra_env[TASK_EXEC_ENV_MAX][96];

    /* §M40 — CLONE_CHILD_CLEARTID.  musl's pthread_join blocks on a futex at
     * this address and relies on the KERNEL zeroing it + waking the waiters
     * when the thread dies; without that, joining a finished thread hangs
     * forever.  NULL for anything not created by clone(). */
    int* clear_tid;
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
    /* Process-model policy: when a task dies it takes its subtree down (its
     * children are killed), UNLESS the child set survives_parent — a daemon /
     * detached task (task_spawn_detached) that outlives its launcher and is
     * instead re-parented to init.  This is why a GUI-launched app dies with the
     * desktop, while a "Detached Shell" keeps running.  Default 0 (mortal). */
    int      survives_parent;
    /* §M46 — per-package RUNAWAY auto-kill.  auto_fkill_ms > 0 opts a user task
     * in: if it hogs the CPU for that long WITHOUT ever voluntarily yielding /
     * blocking (a frozen browser stuck in a ring-3 loop does no syscalls, so it
     * never yields), the watchdog auto-force-kills it.  0 = disabled (default;
     * the chrome/Task-Manager manual force-kill still applies).  last_yield_ms /
     * cpu_ms_at_yield are stamped at every voluntary yield/block and let the
     * detector tell a real hog (CPU actually grew) from a merely-starved task. */
    uint32_t auto_fkill_ms;
    uint64_t last_yield_ms;
    uint64_t cpu_ms_at_yield;
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
    /* Tier B — 1 for an INDEPENDENT user process (proc_spawn): it runs at
     * ring 3/EL0 as its own preemptible task, uses its own kernel stack for
     * privilege transitions (scheduler sets TSS.esp0 to its kstack top on x86),
     * and SYS_EXIT ends the task (task_exit) rather than teleporting back.  0
     * for kernel threads AND the excursion-model self-tests (proc_exec_elf),
     * which keep the fixed syscall stack + teleport-back. */
    int           user_task;
    /* §M33 Tier 1 — this task's ring-3 port grant, or NULL for "no ports",
     * which is every task that is not a user-mode driver.  Installed into the
     * TSS on context switch.  A POINTER, not an inline array: the bitmap is
     * 128 bytes and only a driver process ever has one, so paying for it in
     * every struct task would be the cost of a feature almost nothing uses. */
    void*         io_bitmap;
    /* §1.1 — set while this task is INSIDE a syscall entered from ring 3, i.e.
     * while its pointer arguments are USER pointers.  The sys_* handlers are
     * dual-use (an arch dispatcher calls them with ring-3 pointers, in-kernel
     * code — the shell self-tests, drivers — calls the same functions with
     * KERNEL buffers), and only the dispatcher knows which it is: it sets this
     * on entry and clears it on the way out.  usyscall.c validates a pointer
     * argument only while it is set, so a bad ring-3 pointer returns an error
     * instead of faulting the kernel, and an in-kernel caller is not rejected.
     * (An excursion — proc_exec_* on a kernel task — is covered too: its
     * syscalls arrive through the same dispatcher; proc.c clears the flag when
     * the excursion returns.) */
    int           in_user_syscall;
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
    /* M36 — execution personality.  0 = d-os-native syscall ABI; 1 = Linux i386
     * ABI (the process traps `int 0x80` with LINUX syscall numbers + struct
     * layouts).  Selected at exec time so an unmodified Linux/musl binary runs
     * alongside d-os-native programs.  The kernel routes syscall dispatch by
     * this flag (kernel/hal/x86/linux_abi.c).  Inherited across fork/clone. */
    int           linux_abi;
    /* §M43 — stdout capture: when cap_buf is set, sys_write(1/2) appends the
     * bytes here (bounded by cap_cap, NUL-terminated) in addition to the
     * console, so the Editor's "Compile & Run" can show a program's output.
     * Per-task (the run is a synchronous excursion on this task). */
    char*         cap_buf;
    int           cap_len;
    int           cap_cap;
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
     *
     * §M57 — cpu_home is a FACT, not a hint, and the sentence above is now
     * enforced rather than intended.  It is written in exactly two places —
     * rq_insert_tail_locked (claim) and rq_remove_locked (release) — each
     * holding the lock of the queue it names, so it is -1 precisely when the
     * task is on no runqueue.  A reader that holds queue N's lock and finds
     * cpu_home == N may therefore act on that queue; a reader holding no lock
     * may use it only to CHOOSE a candidate and must re-check it under that
     * candidate's lock (rq_detach_anywhere is the pattern).
     *
     * Do NOT assign it from a call site.  Every hand-assignment this file used
     * to have was a chance to name a queue that did not hold the task, and each
     * one produced the same failure at a distance: a ring spliced from a CPU
     * that did not own its lock, surfacing as a ring-repair report, a hung
     * task, or reap's "STILL QUEUED" — never near the code that caused it.
     */
    int      cpu_home;
    struct task* rq_next;
    struct task* rq_prev;
    /* §M54 — "a CPU is still standing on this task's kernel stack".
     *
     * `current` is NOT that answer.  The scheduler publishes the incoming task
     * as `current` BEFORE the stack swap, so between those two points the
     * outgoing task is current nowhere while the CPU is still executing on its
     * stack (the FPU save/restore and the address-space switch all run there,
     * and context_switch itself writes back into it).  A reaper that only
     * checked `current` therefore freed the stack out from under a live CPU,
     * which reappears later as a return to a garbage address — the machine
     * jumping to 0x3 with nothing to say why.
     *
     * Set when a task is about to be switched TO, cleared by whichever task
     * the CPU switches to next, once the swap is genuinely complete.  Reaping
     * waits for it. */
    volatile int on_cpu;
    /* §M54 — "this task owns a live timed sleep", and the TOKEN that says who
     * gets to end it.
     *
     * A timed sleep is ended by two independent parties: the tick sweep when
     * the deadline passes, and a kill that does not want to wait for it.  Both
     * used to test `sleep_until_ms` without a lock between them, so both could
     * decide the sleep was theirs to end — and both then decremented the global
     * sleeper count for ONE sleep.  A count that drifts low is not a slow
     * kernel, it is a stopped one: the sweep skips itself entirely when the
     * count reads zero, so a real sleeper is never woken and the task blocks
     * forever with no fault, no log and no way in.
     *
     * Claimed with an atomic exchange: whoever swaps this 1 → 0 owns the wake,
     * exactly once, and is the only one that adjusts the count. */
    volatile int timed_sleep;
    /* Tier A.1 — wait-queue link.  When state==TASK_SLEEPING because the
     * task parked itself on a `struct waitq`, it hangs off that queue's
     * singly-linked list through `wq_next` (a task can be blocked on at
     * most one waitq at a time).  NULL otherwise.  See waitq.h. */
    struct task* wq_next;
    /* §M49 — WHICH waitq the task is parked on, or NULL.  The `wq_next`
     * link alone says "somewhere in some queue", which is not enough to
     * get a task out again: a kill has to lock the owning queue and
     * unlink the task from it, and there is no way back to that queue
     * from the task without this pointer.  Set by waitq_block, cleared
     * by whoever wakes the task. */
    struct waitq* wq;
    /* M22.3 — cooperative kill (the Linux kthread_stop contract: all
     * tasks are kernel threads today, so forced termination at an
     * arbitrary preemption point is unsafe — the victim might hold a
     * spinlock.  task_kill sets the flag; the task dies at its next
     * task_yield; CPU-bound workers poll task_should_stop()). */
    volatile int kill_pending;
    /* §M46 — FORCED kill: unlike the cooperative kthread contract above, a
     * force-killed task is torn down without its cooperation.  Safe ONLY when
     * the task is caught in RING 3 (userland holds no kernel locks): the IRQ
     * force-kill point (task_force_kill_point, called from the timer ISR with
     * "was interrupted in user mode") task_exit()s it there.  A task force-killed
     * while in a syscall dies at its next return-to-user / cooperative yield
     * instead.  Used to reclaim a wedged ring-3 app (e.g. a frozen browser). */
    volatile int kill_forced;
    /* M22.3 — CPU time accounting: ms actually spent on a CPU.
     * `sched_in_ms` stamps switch-in; switch-out accumulates into
     * `cpu_ms`.  Feeds `ps` and the GUI task manager. */
    uint64_t cpu_ms;
    uint64_t sched_in_ms;
    /* §M49 — RUNNABLE-time tracking, the load balancer's demand signal.
     *
     * `cpu_ms` above answers "how much CPU did this task GET", which on a
     * saturated core is a property of the competition, not of the task:
     * four hogs sharing a CPU each show 25% and look modest.  What the
     * balancer needs is how much CPU the task WANTS, and that is the time
     * it spends RUNNABLE — running plus waiting in a runqueue.  A CPU hog
     * is runnable ~100% of the time no matter how little it is given; a
     * task that blocks on I/O is runnable a few percent.  Queue length
     * cannot tell those apart, which is the whole reason this exists:
     * with queue depths balanced at 4/3/3/3 the actual shares were still
     * 25% vs 49% for identical tasks, because three of those queue slots
     * were near-idle tasks.
     *
     * `runnable_since` is stamped on runqueue insert, `runnable_acc`
     * accumulates on removal; the sample pair lets the balancer turn the
     * total into a percentage over its own window.  `demand` is that
     * percentage, EWMA-smoothed, 0..TASK_DEMAND_MAX.  It starts at
     * maximum: an unmeasured task must be assumed to want a full core,
     * or a burst of fresh tasks would all look free and pile onto one
     * CPU before any of them had a measurement. */
    uint64_t runnable_since;        /* ms stamp of the current runqueue stay */
    uint64_t runnable_acc;          /* total runnable ms in completed stays */
    uint64_t demand_sample_ms;      /* when `demand` was last recomputed */
    uint64_t demand_acc_at_sample;  /* total runnable ms at that moment */
    uint32_t demand;                /* 0..TASK_DEMAND_MAX — smoothed CPU appetite */
    /* §M49 — deadline for a timed sleep (task_msleep), 0 when not sleeping
     * on a clock.  The task is TASK_SLEEPING and off every runqueue until
     * the tick sweep passes this stamp. */
    uint64_t sleep_until_ms;
    /* §M49 — scheduling priority.  Until now every task was equal, which
     * is fine until the machine is busy: eight CPU hogs and the desktop
     * compositor competed on identical terms, so heavy background work
     * made the UI stutter with nothing able to say otherwise.
     *
     * `nice` is the user-facing knob (-20 strongest .. +19 weakest, 0
     * default) and `weight` is its scheduling meaning: quanta per round,
     * relative to TASK_WEIGHT_BASE.  Weight also scales the task's
     * contribution to runqueue load, so placement packs cheap tasks more
     * densely than expensive ones.
     *
     * `deficit` is the running quantum budget (see schedule_locked) and
     * `skips_left` starves a below-baseline task of whole turns —
     * together they cover both halves of the nice range.  At the default
     * weight both degenerate to exactly the pre-§M49 behaviour: rotate
     * once per tick, never skip. */
    int      nice;
    uint32_t weight;
    int      deficit;
    int      skips_left;
    /* M34 — POSIX signals.  `sig_pending` is a bitmask of posted signals;
     * `sig_handler[sig]` is SIG_DFL(0) / SIG_IGN(1) / a ring-3 handler address;
     * `sig_restorer` is the libc trampoline that issues SYS_SIGRETURN after a
     * handler returns.  Delivered on the return-to-user path (hal/x86/signal.c).
     * Zero-initialised by kcalloc → every task starts with default dispositions. */
    uint32_t  sig_pending;
    /* §M57 — signals the task has asked NOT to receive (sigprocmask).  A
     * blocked signal stays PENDING and is delivered when it is unblocked;
     * dropping it instead would make sigprocmask a way to lose signals rather
     * than to defer them, which is the opposite of what it is for. */
    uint32_t  sig_blocked;
    /* §M57 — recursion depth while evaluating epoll readiness.  An epoll set
     * is itself pollable, so set A may watch set B; nothing stops B watching A,
     * and the readiness walk would then recurse until the kernel stack ran out
     * — with a lock held on every frame.  A depth counter is cheaper and more
     * honest than trying to detect the cycle. */
    int       epoll_depth;
    uintptr_t sig_handler[32];          /* NSIG (syscall.h) */
    uintptr_t sig_restorer;
    /* Per-task FPU / SIMD register file (2026-08-01).  The integer context
     * switch does not touch the FP/vector registers, so without this two tasks
     * doing FP work overwrite each other's registers — and on SMP a migrated
     * task resumes on a DIFFERENT core's register file.  Opaque to the core:
     * only hal_fpu_{init_state,save,restore} interpret it, and they align
     * inside the blob so this needs no special alignment here (struct task is
     * kcalloc'd, whose alignment we do not want to depend on).  See hal_api.h. */
    uint8_t   fpu_state[HAL_FPU_STATE_SIZE];
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

/* task_spawn_arg + an explicit parent pid (>= 0), or the caller (< 0).  Lets a
 * GUI launcher parent a spawned package to the long-lived desktop task rather
 * than the transient app-host that ran the launch fn. */
struct task* task_spawn_arg_under(const char* name, void (*entry)(void),
                                  void* arg, int ppid);

/* M22.7 — spawn with an explicit parent pid (>= 0), or the caller (< 0).
 * The GUI uses it to parent a launched terminal's shell to the desktop
 * session instead of the transient launcher task. */
struct task* task_spawn_under(const char* name, void (*entry)(void), int ppid);

/* §M49 — spawn with the output console ALREADY bound.  `ppid` >= 0 forces
 * the parent, < 0 means "the caller".
 *
 * Binding the console after the spawn call returns is a race on SMP, and
 * `preempt_disable()` does not close it: since §M18.6.2 that counter is
 * PER-CPU, while `task_enqueue` places a new task on the least-loaded
 * core and IPIs it — so another CPU can be running the task's entry point
 * before the binding lands, and a shell that finds no console exits
 * immediately.  It survived because the window is short and the everyday
 * run was uniprocessor (see §M49); the boot shell died the first time the
 * scheduler's timing shifted.
 *
 * `start_arg` already had exactly this treatment (see spawn_common) — the
 * console just never got it. */
struct task* task_spawn_console(const char* name, void (*entry)(void),
                                int ppid, void* console);

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

/* §M53 — sleep until an ABSOLUTE deadline on the timer_now_ns() timeline.
 * Unlike task_msleep this arms a real timer instead of relying on the per-tick
 * sweep, so the deadline is kept in nanoseconds rather than rounded to a tick.
 * Returns 0 if the deadline was reached, -1 if the task was asked to stop —
 * a kill must not wait for a long sleep to finish. */
int task_sleep_until_ns(uint64_t deadline_ns);

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

/* §M46 — FORCE-kill `pid`: cooperative kill_pending PLUS the kill_forced flag,
 * so a wedged RING-3 task (which never reaches a cooperative yield) is torn down
 * at its next timer preemption if it is caught in user mode.  Returns 0 / -1 as
 * task_kill.  Meant for a frozen user app (browser) — do NOT force-kill kernel
 * threads (they may hold locks); those still use the cooperative task_kill. */
int  task_force_kill(int pid);

/* §M46 — opt a user task (pid) into runaway auto-force-kill after `ms` of CPU
 * hogging with no voluntary yield (0 disables).  Set by a launcher per package. */
int  task_set_auto_fkill(int pid, uint32_t ms);

/* §M46 — the arch IRQ layer calls this at the timer preemption point with
 * `from_user` = "the interrupted context was in ring 3".  If the current task is
 * force-killed AND was in userland (holds no kernel locks), it task_exit()s here
 * — the safe point the cooperative path could never reach for a busy-looping
 * ring-3 task. */
void task_force_kill_point(int from_user);

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

/* =============================================================================
 * §M57 — RUNQUEUE CONSISTENCY AUDIT.
 *
 * The §M54/§M57 defects all have the same shape: a task's queue MEMBERSHIP and
 * the kernel's BELIEF about it (cpu_home, rq_count, rq_load) drift apart, and
 * nothing notices until something faults somewhere else entirely — a ring walk
 * in the scheduler, a hung shell, a "STILL QUEUED" at reap time.  Each of those
 * is a symptom observed minutes and megabytes away from its cause.
 *
 * So state the invariant in code and check it directly.  It is short:
 *
 *   1. a task linked in CPU N's ring has cpu_home == N;
 *   2. a task linked in a ring never has cpu_home == -1;
 *   3. rq_count equals what the ring actually contains;
 *   4. every ring is circular and no longer than the queue claims;
 *   5. no RUNNABLE non-idle task sits on NO queue while not running;
 *   6. rq_load equals the summed contribution of the ring's members.
 *
 * RULES 1-4 ARE STRUCTURAL AND HOLD AT EVERY INSTANT.  Rules 5 and 6 are not,
 * and saying so is the difference between a checker people trust and one they
 * learn to ignore:
 *
 *   - Rule 5 has a legitimate transient — a task really is on no queue for a
 *     moment while it is re-homed — so it must be read AT REST.
 *   - Rule 6 measures a published ESTIMATE.  rq_load is read locklessly by
 *     peers by design ("a stale read costs one suboptimal steal decision"),
 *     and a weight change on an unqueued task can race its own enqueue.  The
 *     figure self-heals at the next balance tick, so a transient disagreement
 *     is the design, not a defect; a PERSISTENT one is a real bug and is what
 *     rule 6 is worth checking for, at rest.
 *
 * Reporting all six together and demanding zero would have made the honest
 * answer ("structurally perfect, one estimate briefly stale") indistinguishable
 * from a corrupted runqueue.  They are counted separately for that reason.
 *
 * Rule 1 subsumes "a task is never in two rings": two rings can only share a
 * task by having been spliced together, and then one walk reaches members whose
 * cpu_home names the other queue.  Rule 5 is the one that catches a HANG
 * directly — a task that is ready, on no queue, and current nowhere is a task
 * that will never be picked again, which is what "the shell just stopped"
 * looked like from the outside.  It is the only rule with legitimate
 * transients (a task is briefly off-queue while being re-homed), so it must be
 * read at REST, not during churn.
 *
 * `task_rq_audit` walks every queue under its own lock and fills in how many
 * times each rule was broken.  A violation is a FACT about the scheduler, not
 * an opinion, which is what makes this able to FALSIFY a fix rather than merely
 * fail to contradict it — the §M56.2 lesson, applied one milestone later.
 *
 * Cheap enough to call in a loop from a stress test (one uncontended lock per
 * CPU plus a bounded walk), and deliberately read-only: it REPORTS, it does not
 * repair.  A checker that quietly fixes what it finds destroys the evidence.
 * ============================================================================= */
struct rq_audit {
    int tasks_queued;       /* total tasks found linked in some runqueue     */
    int bad_home;           /* rule 1 — linked in N but cpu_home says other  */
    int orphan_home;        /* rule 2 — cpu_home == -1 yet linked in a ring  */
    int bad_count;          /* rule 3 — rq_count disagrees with the ring     */
    int broken_ring;        /* rule 4 — not circular within the bound        */
    int lost;               /* rule 5 — ready, unqueued, running nowhere     */
    int bad_load;           /* rule 6 — published rq_load is stale           */
};

/* Returns the number of STRUCTURAL violations — rules 1-4 only, the ones that
 * must hold at every instant.  `lost` and `bad_load` are reported in `out` and
 * deliberately excluded from the return value: a caller checking mid-churn
 * wants "is the runqueue corrupt", and folding a legitimate transient into
 * that answer is how a checker gets ignored. */
int task_rq_audit(struct rq_audit* out);

#endif
