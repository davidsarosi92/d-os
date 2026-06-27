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
 * All tasks share the kernel page directory today — they're "kernel
 * threads", not full processes.  When per-process VMM contexts arrive,
 * `struct task` gains a `struct vmm_space*`.
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

#define TASK_NAME_MAX  31
#define TASK_KSTACK_SZ 4096

enum task_state {
    TASK_RUNNABLE,
    TASK_SLEEPING,
    TASK_DEAD,
};

struct task {
    char     name[TASK_NAME_MAX + 1];
    int      pid;
    enum task_state state;
    uint32_t esp;                       /* saved kernel-stack pointer */
    void*    kstack_base;               /* kmalloc'd; freed at reap */
    struct task* next;                  /* circular run-queue link */
    /* M14: optional per-task output binding.  When non-NULL, console.c
     * routes the running task's console_putchar bytes here instead of
     * (or in addition to) the global sinks.  Opaque `void*` so task.h
     * does not need to know about `struct vc`. */
    void*    out_console;
};

/* Set up the scheduler and convert the current `kernel_main` context
 * into pid 0 (named "kernel").  Must be called once, after kmalloc is
 * up, before any `task_spawn`. */
void task_init(void);

/* Create a new kernel-mode task.  `entry` runs on its own stack.  Args
 * are not passed (the entry function may consult globals / config).
 * Returns the new task (RUNNABLE in the queue) or NULL on OOM. */
struct task* task_spawn(const char* name, void (*entry)(void));

/* Cooperative yield.  No-op if we're the only runnable task.  Returns
 * when this task is scheduled again. */
void task_yield(void);

/* The currently scheduled task (or NULL very early in boot, before
 * task_init has run).  Read-only view; do not retain across blocking
 * calls — the next yield may change `current`. */
struct task* task_current(void);

/* Bind / unbind an output console to `t`.  When set, console_putchar
 * (and thus kprintf) routes that task's output to this opaque pointer
 * via the per-task hook installed by vc_init.  Pass NULL to clear. */
void task_set_out_console(struct task* t, void* console);

/* Mark the current task DEAD and never return — the scheduler picks the
 * next runnable task on the next yield cycle. */
void task_exit(void) __attribute__((noreturn));

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

#endif
