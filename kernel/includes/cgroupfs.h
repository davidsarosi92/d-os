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

/* memory.max — THE CHARGE MODEL (cgroupfs.c).  pmm_alloc_frame_user*() call
 * cgroup_mem_charge before allocating: `as` < 0 charges the current task's
 * cgroup, `as` >= 0 that charge id (a memfd's page cache, charged to the
 * memfd's creator).  1 = go ahead, *out_id = the id to record against the
 * frame (0 = unaccounted); 0 = refused (the current process was OOM-killed).
 * On a failed allocation the caller gives the charge back with
 * cgroup_mem_uncharge(*out_id); on success it records the owner with
 * cgroup_mem_set_owner, and pmm_free_frame uncharges through
 * cgroup_mem_free_frame. */
#include "pmm.h"
int  cgroup_mem_charge(int as, int* out_id);
void cgroup_mem_uncharge(int id);
void cgroup_mem_set_owner(pmm_phys_t frame, int id);
void cgroup_mem_free_frame(pmm_phys_t frame);
/* The current task's charge id, held (a memfd remembers its creator's), and
 * its release.  0 = the root (nothing held). */
int  cgroup_memcg_hold_current(void);
void cgroup_memcg_put(int id);

/* 1 when the current task's process was chosen by a cgroup OOM kill: a fault
 * path that could not get a frame must then exit with 137 rather than report
 * a SIGSEGV and a crash record (an OOM kill is a policy decision, not a
 * program error). */
int cgroup_oom_victim_self(void);

#endif
