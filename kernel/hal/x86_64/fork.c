/* =============================================================================
 * fork.c — POSIX fork() orchestration (M34 slice B), x86_64.
 *
 * x86_64 sibling of kernel/hal/x86/fork.c.  Same shape — clone the address
 * space, duplicate the fd table (ref-bumped), start a child task that resumes
 * in ring 3 at the parent's post-`syscall` point with rax = 0 — with two
 * arch differences:
 *
 *   1. The register snapshot is a full x86_64 GPR set (struct user_regs), and
 *      the resume path is enter_user_mode_regs in kernel/hal/x86_64/usermode.s.
 *   2. TLS is the FS.base MSR (musl uses %fs on x86_64), not i386's %gs GDT
 *      descriptor — so the child re-establishes it with hal_set_tls_base and
 *      there is no per-CPU TLS selector / g_entry_gs dance.
 *
 * The address-space clone is EAGER (vmm_space_clone deep-copies user pages) —
 * x86_64 has no COW #PF path yet (a follow-up), so no vmm_cow_fault here.
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

/* First thing the child task runs (in kernel mode): adopt the cloned space +
 * fd table, point the CPU's ring-3→ring-0 stack at its own kernel stack,
 * re-establish TLS (FS.base), then resume ring 3 with the parent's registers
 * (rax already 0). */
static void fork_child_bootstrap(void) {
    struct fork_boot* b = (struct fork_boot*)task_start_arg();
    struct task* me = task_current();

    me->mm          = b->space;
    me->user_task   = 1;
    for (int i = 0; i < TASK_MAX_FDS; i++) me->fds[i] = b->fds[i];

    struct user_regs  regs  = b->regs;      /* copy out before freeing b       */
    struct vmm_space* space = b->space;
    kfree(b);

    vmm_space_switch(space);
    if (me->kstack_base)
        hal_set_kernel_stack((uintptr_t)me->kstack_base + TASK_KSTACK_SZ);

    /* Re-establish the child's TLS: on x86_64 that's just reloading FS.base
     * (musl touches thread-local state — errno, the pthread self pointer —
     * immediately after fork). */
    if (me->has_tls) hal_set_tls_base(me->tls_base);

    enter_user_mode_regs(&regs);            /* → ring 3 at the fork point; no return */
}

int proc_fork(struct user_regs* parent_regs) {
    struct task* parent = task_current();
    if (!parent || !parent->mm) return -1;   /* only a user process can fork    */

    struct vmm_space* child_space = vmm_space_clone(parent->mm);
    if (!child_space) return -1;

    struct fork_boot* b = (struct fork_boot*)kmalloc(sizeof *b);
    if (!b) { vmm_space_destroy(child_space); return -1; }
    b->space       = child_space;
    b->regs        = *parent_regs;
    b->regs.rax    = 0;                      /* child: fork() returns 0         */
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

    /* Inherit the ABI personality so a musl shell's children are serviced by
     * linux_abi.c too, and the child's TLS base. */
    child->linux_abi = parent->linux_abi;
    child->has_tls   = parent->has_tls;
    child->tls_base  = parent->tls_base;

    /* Inherit the FP/SIMD register file (POSIX: the child continues with the
     * parent's floating-point state).  The parent's LIVE state is in the CPU
     * right now — its blob only holds what it had at its last switch-out — so
     * snapshot it before copying, or the child would resume with stale
     * registers the parent has since overwritten.  Matters more here than on
     * i386: SSE2 is baseline on x86_64, so every musl child is an FP user. */
    hal_fpu_save(parent->fpu_state);
    for (unsigned i = 0; i < HAL_FPU_STATE_SIZE; i++)
        child->fpu_state[i] = parent->fpu_state[i];

    /* Claim the reap so init leaves the child as a POSIX zombie for the
     * parent's waitpid() (task_wait). */
    task_set_reap_owned(child, 1);
    return child->pid;                       /* parent: fork() returns child pid */
}

/* ---------------------------------------------------------------------------
 * §M40 — clone() as a THREAD (musl's pthread_create).
 *
 * Same shape as fork above, with the one difference that matters: the child
 * SHARES the address space instead of getting a copy, resumes on a stack the
 * caller supplies, and installs the caller-supplied thread pointer.  Linux's
 * clone resumes the child at the SAME instruction with the return value 0 —
 * musl's __clone relies on exactly that, having pre-laid the start function and
 * its argument on the new stack — which is why this reuses the fork register
 * snapshot rather than taking an entry point like the native proc_clone.
 *
 * The fd "table" is per-task here, so the child gets ref-bumped copies of the
 * parent's descriptors.  Sharing the OBJECTS is what POSIX threads actually
 * need for I/O; a descriptor opened later by one thread is not yet visible to
 * the others (a real shared table is a follow-up).
 * ------------------------------------------------------------------------- */
struct clone_boot_t {
    struct vmm_space* space;
    struct user_regs  regs;
    uintptr_t         tls;
    struct ofile*     fds[TASK_MAX_FDS];
};

static void clone_thread_bootstrap(void) {
    struct clone_boot_t* b = (struct clone_boot_t*)task_start_arg();
    struct task* me = task_current();

    me->mm        = b->space;               /* SHARED with the creator */
    me->mm_shared = 1;
    me->user_task = 1;
    for (int i = 0; i < TASK_MAX_FDS; i++) me->fds[i] = b->fds[i];

    struct user_regs  regs  = b->regs;
    struct vmm_space* space = b->space;
    uintptr_t         tls   = b->tls;
    kfree(b);

    vmm_space_switch(space);
    if (me->kstack_base)
        hal_set_kernel_stack((uintptr_t)me->kstack_base + TASK_KSTACK_SZ);
    if (tls) { me->tls_base = tls; me->has_tls = 1; hal_set_tls_base(tls); }

    enter_user_mode_regs(&regs);            /* → ring 3, rax = 0 */
}

int proc_clone_thread(struct user_regs* parent_regs, uintptr_t child_stack,
                      uintptr_t tls, int* ctid_kaddr) {
    struct task* parent = task_current();
    if (!parent || !parent->mm || !child_stack) return -1;

    struct clone_boot_t* b = (struct clone_boot_t*)kmalloc(sizeof *b);
    if (!b) return -1;
    b->space   = parent->mm;                /* share, do not clone */
    b->regs    = *parent_regs;
    b->regs.rax = 0;                    /* the child sees clone() == 0 */
    b->regs.user_sp = child_stack;
    b->tls     = tls;
    for (int i = 0; i < TASK_MAX_FDS; i++)
        b->fds[i] = parent->fds[i] ? ofile_ref(parent->fds[i]) : NULL;

    struct task* child = task_spawn_arg("thread", clone_thread_bootstrap, b);
    if (!child) {
        for (int i = 0; i < TASK_MAX_FDS; i++)
            if (b->fds[i]) ofile_unref(b->fds[i]);
        kfree(b);
        return -1;
    }
    child->mm_shared   = 1;                 /* set early: it may run at once */
    child->linux_abi   = parent->linux_abi;
    child->clear_tid   = ctid_kaddr;        /* CLONE_CHILD_CLEARTID (see task_exit) */
    hal_fpu_save(parent->fpu_state);
    for (unsigned i = 0; i < HAL_FPU_STATE_SIZE; i++)
        child->fpu_state[i] = parent->fpu_state[i];
    task_set_reap_owned(child, 1);          /* the creator joins it */
    return child->pid;
}
