/* =============================================================================
 * fork.c — POSIX fork() orchestration (M34 slice B), i386.
 *
 * fork() duplicates the calling user process: a cloned address space (eager
 * page copy — COW is a later optimisation), a duplicated fd table (ref-bumped
 * ofile objects), and a child task that resumes in ring 3 at the parent's
 * exact post-`int 0x80` point with eax = 0 (so the child sees fork() == 0,
 * the parent sees the child's pid).
 *
 * The mechanism is arch-specific (it restores a full user register set via
 * iret — see enter_user_mode_regs in usermode.s), so this lives under
 * kernel/hal/x86/ and is i386-only today.  Generalising to x86_64/aarch64 is
 * "move the orchestration to proc.c + provide each arch's enter_user_mode_regs".
 *
 * The child is a normal preemptible task (user_task=1): its parent is the
 * caller (task_spawn_arg parents to the current task), so the caller can
 * waitpid() on it (task_wait), and init/waitpid reaps it — freeing the cloned
 * address space (task_reap → vmm_space_destroy).
 * ============================================================================= */

#include "proc.h"
#include "task.h"
#include "vmm.h"
#include "usermode.h"
#include "fd.h"
#include "kmalloc.h"
#include "hal_api.h"
#include "syscall.h"
#include "gdt.h"          /* gdt_tls_selector — child's ring-3 %gs */
#include "percpu.h"       /* this_cpu_id                          */
#include <stdint.h>
#include <stddef.h>

/* The ring-3 %gs selector enter_user_mode_regs loads (usermode.s).  Default is
 * the plain user-data selector (0x23); fork_child_bootstrap raises it to the
 * per-CPU TLS selector when the child has TLS, so musl's post-fork thread-local
 * accesses resolve.  (Global → UP-correct; SMP concurrent forks want this
 * per-CPU / passed-in — a follow-up.) */
extern uint16_t g_entry_gs;

/* Handed to the child task's bootstrap (heap-allocated, freed by the child). */
struct fork_boot {
    struct vmm_space* space;
    struct user_regs  regs;
    uintptr_t         mmap_cursor;
    struct ofile*     fds[TASK_MAX_FDS];    /* parent fd snapshot, refs bumped */
};

/* First thing the child task runs (in kernel mode): adopt the cloned space +
 * fd table, point the CPU's ring-3→ring-0 stack at its own kernel stack, then
 * resume ring 3 with the parent's registers (eax already 0). */
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

    /* Re-establish TLS for the child: pin to this CPU (its %gs selector is
     * per-CPU), load this CPU's TLS descriptor base, and tell the resume path
     * to enter ring 3 with the TLS selector rather than plain user data. */
    if (me->has_tls) {
        task_set_affinity(me, 1u << this_cpu_id());
        hal_set_tls_base(me->tls_base);
        g_entry_gs = gdt_tls_selector();
    } else {
        g_entry_gs = 0x23;
    }

    enter_user_mode_regs(&regs);            /* → ring 3 at the fork point; no return */
}

int proc_fork(struct user_regs* parent_regs) {
    struct task* parent = task_current();
    if (!parent || !parent->mm) return -1;  /* only a user process can fork    */

    struct vmm_space* child_space = vmm_space_clone(parent->mm);
    if (!child_space) return -1;

    struct fork_boot* b = (struct fork_boot*)kmalloc(sizeof *b);
    if (!b) { vmm_space_destroy(child_space); return -1; }
    b->space       = child_space;
    b->regs        = *parent_regs;
    b->regs.eax    = 0;                      /* child: fork() returns 0         */
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

    /* Inherit the ABI personality: a fork()ing Linux/musl process (a shell)
     * must produce Linux-personality children so its execve'd programs are
     * serviced by linux_abi.c too. */
    child->linux_abi = parent->linux_abi;

    /* Inherit TLS: musl touches thread-local state (errno, the pthread self
     * pointer) immediately after fork, so the child needs the same %gs base.
     * The child's %gs selector is (re)established in fork_child_bootstrap. */
    child->has_tls  = parent->has_tls;
    child->tls_base = parent->tls_base;

    /* Claim the reap so the M27 universal reaper (init) keeps its hands off:
     * the child becomes a POSIX zombie held for the parent's waitpid()
     * (task_wait), which reaps it and collects the exit code.  Without this,
     * init would reap the child first and waitpid() would find no child.
     * (Caveat: a child never waited-for now leaks as a zombie — the POSIX
     * semantics; reaping orphaned zombies on parent exit is a follow-up.) */
    task_set_reap_owned(child, 1);
    return child->pid;                       /* parent: fork() returns child pid */
}
