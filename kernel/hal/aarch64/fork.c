/* =============================================================================
 * fork.c — POSIX fork() orchestration, AArch64 (§A1).
 *
 * The ARM sibling of kernel/hal/x86/fork.c and kernel/hal/x86_64/fork.c.  Same
 * shape: clone the address space, duplicate the fd table (ref-bumped), start a
 * child task that resumes at EL0 at the parent's post-`svc` point with x0 = 0.
 *
 * Three arch differences worth stating, because each one is a place the x86
 * code cannot simply be transliterated:
 *
 *   1. The register snapshot comes from the trapframe vectors.S already builds
 *      (x0..x30 + ELR + SPSR) — but that frame does NOT contain SP_EL0.  Taking
 *      an exception from EL0 switches the CPU to SP_EL1 and leaves SP_EL0
 *      banked, so the handler never needed it.  A child resuming in its own
 *      address space does, so proc_fork reads it with `mrs` at fork time.
 *
 *   2. TLS is TPIDR_EL0 — a single system register, with none of i386's
 *      per-CPU GDT descriptor dance and none of x86_64's FS.base MSR.  The
 *      child just reloads it via hal_set_tls_base.
 *
 *   3. The clone is COPY-ON-WRITE (vmm_space_clone marks both sides read-only
 *      and vmm_cow_fault privatises on the first write), so a fault on a
 *      write-protected user page is a normal event here — exceptions.c routes
 *      permission faults from EL0 through vmm_cow_fault before treating them
 *      as a real fault.
 * ============================================================================= */

#include "proc.h"
#include "task.h"
#include "vmm.h"
#include "usermode.h"
#include "fd.h"
#include "kmalloc.h"
#include "hal_api.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

/* Handed to the child task's bootstrap (heap-allocated, freed by the child). */
struct fork_boot {
    struct vmm_space* space;
    struct user_regs  regs;
    struct ofile*     fds[TASK_MAX_FDS];    /* parent fd snapshot, refs bumped */
};

/* First thing the child runs, in kernel mode: adopt the cloned space + fd
 * table, re-establish TLS, then resume EL0 with the parent's registers (x0
 * already 0).
 *
 * No hal_set_kernel_stack equivalent is needed: on AArch64 the CPU selects
 * SP_EL1 automatically when it takes an exception from EL0, and context_switch
 * already tracks each task's SP_EL1 — the whole TSS.esp0 problem does not
 * exist here. */
static void fork_child_bootstrap(void) {
    struct fork_boot* b = (struct fork_boot*)task_start_arg();
    struct task* me = task_current();

    me->mm        = b->space;
    me->user_task = 1;
    for (int i = 0; i < TASK_MAX_FDS; i++) me->fds[i] = b->fds[i];

    struct user_regs  regs  = b->regs;      /* copy out before freeing b */
    struct vmm_space* space = b->space;
    kfree(b);

    vmm_space_switch(space);
    if (me->has_tls) hal_set_tls_base(me->tls_base);

    enter_user_mode_regs(&regs);            /* → EL0 at the fork point; no return */
}

int proc_fork(struct user_regs* parent_regs) {
    struct task* parent = task_current();
    if (!parent || !parent->mm) return -1;   /* only a user process can fork */

    struct vmm_space* child_space = vmm_space_clone(parent->mm);
    if (!child_space) return -1;

    struct fork_boot* b = (struct fork_boot*)kmalloc(sizeof *b);
    if (!b) { vmm_space_destroy(child_space); return -1; }
    b->space      = child_space;
    b->regs       = *parent_regs;
    b->regs.x[0]  = 0;                       /* child: fork() returns 0 */
    for (int i = 0; i < TASK_MAX_FDS; i++)
        b->fds[i] = parent->fds[i] ? ofile_ref(parent->fds[i]) : NULL;

    struct task* child = task_spawn_arg("forked", fork_child_bootstrap, b);
    if (!child) {
        for (int i = 0; i < TASK_MAX_FDS; i++)
            if (b->fds[i]) ofile_unref(b->fds[i]);
        kfree(b);
        vmm_space_destroy(child_space);
        return -1;
    }
    /* Inherit the parent's signal dispositions (POSIX: fork keeps handlers). */
    for (int i = 0; i < NSIG; i++) child->sig_handler[i] = parent->sig_handler[i];
    child->sig_restorer = parent->sig_restorer;

    child->linux_abi = parent->linux_abi;
    child->has_tls   = parent->has_tls;
    child->tls_base  = parent->tls_base;

    /* Inherit the FP/SIMD register file.  The parent's LIVE state is in the CPU
     * right now — its blob only holds what it had at its last switch-out — so
     * snapshot it before copying, or the child resumes with stale registers. */
    hal_fpu_save(parent->fpu_state);
    for (unsigned i = 0; i < HAL_FPU_STATE_SIZE; i++)
        child->fpu_state[i] = parent->fpu_state[i];

    /* Claim the reap so init leaves the child as a POSIX zombie for the
     * parent's waitpid() (task_wait). */
    task_set_reap_owned(child, 1);
    return child->pid;                       /* parent: fork() returns child pid */
}
