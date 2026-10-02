/* cgroupfs.h — the cgroup v2 filesystem, §M90.  See kernel/fs/cgroupfs.c. */
#ifndef CGROUPFS_H
#define CGROUPFS_H

#include <stddef.h>

struct task;

/* The cgroup path of `t` ("/" for the root), as /proc/<pid>/cgroup prints it
 * after "0::".  A task's node is `task->cgroup` (NULL = the root). */
void cgroup_path_of(const struct task* t, char* out, size_t cap);

#endif
