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
    uintptr_t         mmap_cursor;
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
    me->mmap_cursor = b->mmap_cursor;
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
    b->mmap_cursor = parent->mmap_cursor;
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

    /* Claim the reap so init leaves the child as a POSIX zombie for the
     * parent's waitpid() (task_wait). */
    task_set_reap_owned(child, 1);
    return child->pid;                       /* parent: fork() returns child pid */
}
