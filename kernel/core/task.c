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
#include <stddef.h>
#include <stdint.h>

/* The arch-specific context-switch routine.  On x86 see
 * kernel/hal/x86/switch.s; on x64/aarch64 the equivalent ships with
 * those ports.  Signature is intentionally `uintptr_t` so the same
 * declaration works on every 32/64-bit arch. */
extern void context_switch(uintptr_t* save_esp_to, uintptr_t new_esp);

/* Run-queue is a circular singly-linked list rooted at `current`.
 * `head` exists for diagnostic listing only. */
static struct task* current = NULL;
static struct task* head    = NULL;
static int          next_pid = 0;

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

/* Insert `t` into the circular list right after `current`.  Caller must
 * have IRQs disabled. */
static void runqueue_insert_locked(struct task* t) {
    if (!current) {
        /* First task — circle of one. */
        t->next = t;
        head    = t;
        current = t;
    } else {
        t->next       = current->next;
        current->next = t;
    }
}

/* ------------------------------------------------------------------- */
/* Init.                                                                */
/* ------------------------------------------------------------------- */

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

    uint32_t fl = hal_intr_save();
    runqueue_insert_locked(t0);
    hal_intr_restore(fl);

    kprintf("task: pid 0 (kernel) installed\n");
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

    uint32_t fl = hal_intr_save();
    runqueue_insert_locked(t);
    hal_intr_restore(fl);

    return t;
}

/* ------------------------------------------------------------------- */
/* Schedule + yield + exit.                                             */
/* ------------------------------------------------------------------- */

/* Core scheduler — picks the next RUNNABLE task and context-switches.
 * MUST be called with IRQs already disabled.  Returns with IRQs still
 * disabled; caller is responsible for restoring IF. */
static void schedule_locked(void) {
    if (!current) return;

    struct task* prev = current;
    struct task* next = prev->next;
    while (next != prev && next->state != TASK_RUNNABLE) next = next->next;
    if (next == prev) return;           /* no one else to run */

    current = next;
    context_switch(&prev->esp, next->esp);
    /* When `prev` is later scheduled back in, control resumes here.
     * IF is still 0 (the resume path inherited cli from the schedule()
     * that put us out).  Our caller will restore IF based on its own
     * captured `fl`. */
}

void schedule(void) {
    uint32_t fl = hal_intr_save();
    schedule_locked();
    hal_intr_restore(fl);
}

void task_yield(void) {
    schedule();
}

/* ------------------------------------------------------------------- */
/* Per-task accessors (M14).                                            */
/* ------------------------------------------------------------------- */

struct task* task_current(void) {
    return current;
}

void task_set_out_console(struct task* t, void* console) {
    if (!t) return;
    t->out_console = console;
}

void task_exit(void) {
    /* Mark ourselves DEAD then call schedule.  No future selection will
     * pick a DEAD task, so schedule_locked won't context-switch back. */
    hal_intr_disable();
    current->state = TASK_DEAD;
    schedule_locked();
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
        kprintf("%d   %s    %s%s\n",
                t->pid, state_name(t->state), t->name,
                (t == current) ? " (running)" : "");
        t = t->next;
    } while (t != head);
}

void task_for_each(task_iter_fn fn, void* ctx) {
    if (!fn || !head) return;
    struct task* t = head;
    do {
        fn(t, t == current, ctx);
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
