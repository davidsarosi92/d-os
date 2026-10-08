/* cgroupfs.h — the cgroup v2 filesystem, §M90.  See kernel/fs/cgroupfs.c. */
#ifndef CGROUPFS_H
#define CGROUPFS_H

#include <stddef.h>
#include <stdint.h>

struct task;

/* The cgroup path of `t` ("/" for the root), as /proc/<pid>/cgroup prints it
 * after "0::".  A task's node is `task->cgroup` (NULL = the root). */
void cgroup_path_of(const struct task* t, char* out, size_t cap);

/* ---- §M90 rung 3 — the limits ENFORCED ------------------------------------
 *
 * Each hook is a no-op returning "allowed" for a task in the root cgroup
 * (task->cgroup == NULL), which is every kernel thread and every program not
 * started by a container runtime — so the cost outside containers is one
 * pointer test.  Limits are HIERARCHICAL, as on Linux: a node's limit bounds
 * everything in its subtree, and the tightest ancestor wins. */

/* pids.max — may `t` create one more task (fork, clone, a thread)?  1 = yes;
 * 0 = a pids.max on t's node or an ancestor is reached (pids.events "max" is
 * counted).  The Linux-facing callers turn 0 into EAGAIN. */
int cgroup_fork_allowed(const struct task* t);

/* cpu.max — charge `t`'s CPU time up to `now_ms` to its cgroup chain.  Called
 * from the tick (the running task) and the context switch (the outgoing one),
 * with interrupts off.  A node whose quota for the current period is spent
 * becomes THROTTLED until the period ends. */
void cgroup_cpu_charge(struct task* t, uint64_t now_ms);

/* cpu.max — is `t` throttled right now (its node or an ancestor has spent its
 * quota for this period)?  The scheduler does not pick a throttled task.
 * Called with the runqueue lock held; takes only the cgroup's own lock. */
int cgroup_cpu_throttled(const struct task* t);

/* memory.max — may the CURRENT task take one more user frame?  Called by
 * pmm_alloc_frame_user*() before allocating.  1 = yes.  0 = the limit is
 * reached and the OOM killer chose the current process: the allocation must
 * fail, and the caller's fault path ends the process with SIGKILL (137) via
 * cgroup_oom_victim_self().  When the victim is ANOTHER process the call
 * answers 1 (the frames come back as the victim dies — Linux's charge
 * likewise succeeds once the kill is under way). */
int cgroup_mem_charge(void);

/* 1 when the current task's process was chosen by a cgroup OOM kill: a fault
 * path that could not get a frame must then exit with 137 rather than report
 * a SIGSEGV and a crash record (an OOM kill is a policy decision, not a
 * program error). */
int cgroup_oom_victim_self(void);

#endif
