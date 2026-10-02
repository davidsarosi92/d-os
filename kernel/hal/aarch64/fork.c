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

#include "lnx_signal.h"   /* §M89 */
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
    uint64_t          tpidr;                /* the parent's LIVE thread pointer */
    struct ofile*     fds[TASK_MAX_FDS];    /* parent fd snapshot, refs bumped */
    uint32_t          cloexec;              /* §M90 — the parent's close-on-exec bits */
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
    fd_cloexec_restore(me, b->cloexec);

    struct user_regs  regs  = b->regs;      /* copy out before freeing b */
    struct vmm_space* space = b->space;
    uint64_t          tpidr = b->tpidr;
    kfree(b);

    vmm_space_switch(space);

    /* Install the parent's thread pointer.  `has_tls`/`tls_base` are NOT enough
     * on this arch: EL0 can write TPIDR_EL0 itself (musl's aarch64
     * __set_thread_area is one `msr`, no syscall), so the kernel was never told
     * and has_tls is 0 — while the register very much holds a live pointer.
     *
     * Missing this is not a subtle degradation.  musl's fork path computes its
     * `struct pthread` as TP - 0xc8 and immediately stores through it, so a
     * child starting with TP = 0 faults at address -0xa8 on its first
     * instruction after the syscall.  That was the actual symptom. */
    __asm__ volatile ("msr tpidr_el0, %0" :: "r"(tpidr));
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
    /* Read it LIVE rather than from the task struct — see the bootstrap. */
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(b->tpidr));
    for (int i = 0; i < TASK_MAX_FDS; i++)
        b->fds[i] = parent->fds[i] ? ofile_ref(parent->fds[i]) : NULL;
    b->cloexec = fd_cloexec_mask(parent);

    struct task* child = task_spawn_arg_held("forked", fork_child_bootstrap, b);
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
    lnx_sig_fork(parent, child);            /* §M89 — Linux dispositions, copied */

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
    task_release(child);                /* §4.96: built completely, now it may run */
    return child->pid;                       /* parent: fork() returns child pid */
}

/* ---------------------------------------------------------------------------
 * §M89 rung 3 — clone(CLONE_VM): a THREAD, the aarch64 twin of the x86
 * proc_clone_thread.  The child shares the address space, the descriptor
 * table (CLONE_FILES, fdtable_share) and the signal dispositions
 * (CLONE_SIGHAND), and resumes at the parent's post-`svc` point with x0 = 0 on
 * the stack the caller supplied.
 *
 * TPIDR_EL0 is the whole of TLS on this arch, so CLONE_SETTLS is one `msr` in
 * the child; without it the child inherits the parent's LIVE pointer (read
 * with mrs, for the reason the fork bootstrap gives).
 * ------------------------------------------------------------------------- */
struct thread_boot {
    struct vmm_space* space;
    struct user_regs  regs;
    uint64_t          tpidr;
    struct fdtable*   fdt;
};

static void thread_child_bootstrap(void) {
    struct thread_boot* b = (struct thread_boot*)task_start_arg();
    struct task* me = task_current();
    me->mm        = b->space;              /* SHARED with the creator */
    me->mm_shared = 1;
    me->user_task = 1;
    fdtable_adopt(me, b->fdt);
    struct user_regs  regs  = b->regs;
    struct vmm_space* space = b->space;
    uint64_t          tpidr = b->tpidr;
    kfree(b);
    vmm_space_switch(space);
    __asm__ volatile ("msr tpidr_el0, %0" :: "r"(tpidr));
    me->tls_base = (uintptr_t)tpidr;
    me->has_tls  = 1;
    enter_user_mode_regs(&regs);            /* → EL0, x0 = 0; no return */
}

int proc_clone_thread(struct user_regs* parent_regs, uintptr_t child_stack,
                      uintptr_t tls, int* ctid_kaddr) {
    struct task* parent = task_current();
    if (!parent || !parent->mm || !child_stack) return -1;
    struct thread_boot* b = (struct thread_boot*)kmalloc(sizeof *b);
    if (!b) return -1;
    b->space   = parent->mm;
    b->regs    = *parent_regs;
    b->regs.x[0] = 0;
    b->regs.user_sp = child_stack;
    if (tls) b->tpidr = tls;
    else     __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(b->tpidr));
    b->fdt = fdtable_share(parent);
    if (!b->fdt) { kfree(b); return -1; }

    struct task* child = task_spawn_arg_held("thread", thread_child_bootstrap, b);
    if (!child) { fdtable_put(b->fdt); kfree(b); return -1; }
    child->mm_shared = 1;                   /* set early: it may run at once */
    child->linux_abi = parent->linux_abi;
    child->clear_tid = ctid_kaddr;          /* CLONE_CHILD_CLEARTID (task_exit) */
    lnx_sig_thread(parent, child);          /* CLONE_SIGHAND: shared */
    hal_fpu_save(parent->fpu_state);
    for (unsigned i = 0; i < HAL_FPU_STATE_SIZE; i++)
        child->fpu_state[i] = parent->fpu_state[i];
    /* §M89 — a Linux thread is joined through its CLEARTID futex, never by
     * waitpid: nobody collects it, so the reaper must (it used to be
     * reap_owned, and every JVM thread stayed a DEAD zombie).  A native
     * in-tree-libc thread is joined with waitpid and stays owned. */
    task_set_reap_owned(child, parent->linux_abi ? 0 : 1);
    task_release(child);
    return child->pid;
}
