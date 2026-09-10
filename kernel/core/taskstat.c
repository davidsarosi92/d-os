/* =============================================================================
 * taskstat.c — what a task is costing right now (§M75).
 *
 * `ps` and the GUI task manager both want to answer "how much CPU is this
 * using" and "how much memory is this holding".  Two consumers is exactly the
 * number at which this tree writes the answer down ONCE: four scrollbars and
 * two shells were each a second copy of something that looked too small to
 * share (§4.85.9, §M70).
 *
 * Both numbers have a trap that a naive read of `struct task` walks straight
 * into, which is the other reason they live here rather than in a caller.
 * ============================================================================= */

#include "task.h"
#include "vmm.h"
#include "timer.h"

/* ---------------------------------------------------------------------------
 * CPU time, including the slice in flight.
 *
 * **`cpu_ms` IS ONLY CREDITED AT A CONTEXT SWITCH** — switch-in stamps
 * `sched_in_ms`, switch-out accumulates the difference.  So a task that is on
 * a CPU *at this instant* is under-reported by however long it has been there,
 * and a task that monopolises a core without being switched out contributes
 * NOTHING.
 *
 * That is not hypothetical: §M49's `sched` command shipped with exactly this
 * bug one layer down — its first version reported a core running one busy task
 * at 0 % — and the fix there was the same addition made here.  Preemption
 * makes the error small (a task cannot hold a CPU past its quantum), but
 * "small" is the wrong property for the number a CPU% column is a DELTA of:
 * two samples that each omit a different in-flight slice produce a percentage
 * that is wrong by the difference, not by the slice.
 *
 * Callers must hold whatever keeps `t` alive (`task_for_each`'s master lock).
 * ------------------------------------------------------------------------- */
uint64_t task_cpu_ms_now(const struct task* t) {
    if (!t) return 0;
    /* §M75.2 — MINUS the time spent halted waiting for work.  A poll loop is
     * SCHEDULED while it waits, so without this the compositor and the desktop
     * each read ~25 % of a 4-CPU box while measurement put their real
     * composing cost at 1-2 %.  A CPU column that cannot tell waiting from
     * working is the one column nobody can act on. */
    uint64_t ms = t->cpu_ms;
    /* `on_cpu` spans the WHOLE context switch (§M54), so it is the honest test
     * for "executing somewhere" — `current` on some CPU is published before
     * the stack swap and would count a task that is not running yet. */
    if (t->on_cpu && !t->is_idle) {
        uint64_t now = timer_ticks_ms();
        if (now > t->sched_in_ms) ms += now - t->sched_in_ms;
    }

    /* §M75.2 — SUBTRACT THE HALT LAST, from the TOTAL, and the order is the
     * whole bug the first version had.
     *
     * `cpu_ms` is credited at SWITCH-OUT — and a poll loop that owns its core
     * is never switched out, so the compositor's `cpu_ms` measured **ZERO**
     * after twenty seconds of uptime while all of its time sat in the
     * in-flight term above.  Subtracting the halt from `cpu_ms` alone
     * therefore subtracted it from zero, clamped, and changed nothing — a
     * correction that compiled, ran, and was inert, which looks exactly like a
     * wrong theory until you print both numbers side by side.
     *
     * It also explains a much older oddity: `ps` showed the compositor and the
     * desktop with IDENTICAL `cpu_ms` to the millisecond, run after run.  They
     * were both reporting `now - sched_in_ms` — the uptime — because neither
     * had ever been switched out. */
    uint64_t halt_ms = t->halt_ns / 1000000ull;
    return ms > halt_ms ? ms - halt_ms : 0;
}

/* ---------------------------------------------------------------------------
 * Memory.
 *
 * The measurement itself is `vmm_space_resident` (see vmm.h for why the policy
 * is portable and the walk is not).  What this adds is the ONE rule that keeps
 * the column summable:
 *
 * **A CLONED THREAD REPORTS ZERO, because it does not own its address space.**
 * `mm_shared` marks a task that shares its creator's `mm` — its reap must not
 * destroy the space, and by exactly the same reasoning its row must not claim
 * the space's memory.  Without this a four-thread process reports its heap
 * four times, and a machine with three such processes reports more memory in
 * use than it has: the kind of number that discredits every other figure in
 * the window.
 *
 * The caller can tell "a thread" from "a kernel task" from "a process holding
 * nothing" through `owns_mm`, which is why it is reported rather than folded
 * into a zero.
 * ------------------------------------------------------------------------- */
void task_mem_bytes(const struct task* t, uint64_t* out_private,
                    uint64_t* out_shared, int* out_owns_mm) {
    uint64_t priv = 0, shared = 0;
    int owns = 0;

    if (t && t->mm && !t->mm_shared) {
        owns = 1;
        vmm_space_resident(t->mm, &priv, &shared);
    }

    if (out_private) *out_private = priv;
    if (out_shared)  *out_shared  = shared;
    if (out_owns_mm) *out_owns_mm = owns;
}

/* ---------------------------------------------------------------------------
 * CPU% between two samples.
 *
 * Kept here rather than in each caller because the SCALING is the part that is
 * easy to get differently wrong twice: a task pinning one core of four is
 * using 25 % of the machine, and a task manager that reports 100 % for it
 * cannot also report a meaningful total.
 *
 * `elapsed_ms` MUST come from a clock, never from a count of refresh ticks.
 * §M61 paid for that distinction already — its mode-change countdown showed
 * how many timer events had fired rather than how much time had passed, and
 * under emulation a missed or doubled tick lands straight in what the user
 * reads.  *A counter counts events; a clock measures time.*
 *
 * Returns tenths of a percent (0..1000), so a single integer carries one
 * decimal place without this kernel's printf needing a float it does not have.
 * ------------------------------------------------------------------------- */
uint32_t task_cpu_permille(uint64_t cpu_ms_delta, uint64_t elapsed_ms, int ncpus) {
    if (elapsed_ms == 0 || ncpus <= 0) return 0;
    /* A task cannot have used more CPU than the machine had to give; a delta
     * that says otherwise means the two samples are not comparable (a wrapped
     * clock, a task that was reaped and its pid reused).  Clamp rather than
     * print an impossible number — but clamp at the CEILING, so the display
     * saturates visibly instead of quietly reporting a plausible smaller
     * value. */
    uint64_t capacity = elapsed_ms * (uint64_t)ncpus;
    if (cpu_ms_delta > capacity) cpu_ms_delta = capacity;
    return (uint32_t)((cpu_ms_delta * 1000u) / capacity);
}
