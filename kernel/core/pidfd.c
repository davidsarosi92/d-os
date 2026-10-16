/* =============================================================================
 * pidfd.c — a process behind a descriptor (Linux pidfd, §M90).
 *
 * WHY: a pid is a NUMBER, and a number is recycled.  Anything that holds a pid
 * across time — a supervisor that will signal its child later, a waiter in
 * another thread — can end up talking to an unrelated process that was given
 * the same number after the first one was reaped.  A pidfd names the PROCESS:
 * it stays bound to that one process whatever happens to its number.  Go's
 * runtime uses one for every child it starts once the kernel offers them
 * (os.StartProcess with CLONE_PIDFD, Process.Wait through waitid(P_PIDFD),
 * Process.Signal through pidfd_send_signal), and containerd checks for them at
 * start ("failed to ensure the kernel supports pidfd" until now).
 *
 * WHAT IS IMPLEMENTED (Linux semantics):
 *   pidfd_open(pid, flags)                 a descriptor for the process; flags
 *                                          PIDFD_NONBLOCK (O_NONBLOCK) and
 *                                          PIDFD_THREAD (O_EXCL: a thread may
 *                                          be named); otherwise the pid must be
 *                                          a thread-group leader (EINVAL)
 *   clone(CLONE_PIDFD) / clone3            the parent receives one for the child
 *   poll/epoll                             POLLIN once the whole thread group has
 *                                          exited (before it is reaped); POLLHUP
 *                                          as well once it has been reaped
 *   pidfd_send_signal(fd, sig, info, 0)    kill(2) aimed at THAT process; ESRCH
 *                                          once it has exited (signal 0 probes)
 *   waitid(P_PIDFD, fd, ...)               wait for exactly that child (ECHILD
 *                                          if it is not the caller's child)
 *   read/write                             EINVAL, as on Linux
 *
 * IDENTITY: the descriptor stores the global pid AND the task's start time
 * (`start_ms`, the same value /proc/<pid>/stat reports): a task found under
 * that pid with a different start time is a different process, and the pidfd's
 * own process counts as gone.
 *
 * NOT here, said plainly: pidfd_getfd, the PIDFD_GET_INFO ioctl, siginfo
 * passed to pidfd_send_signal (the signal is sent as kill(2) would), and the
 * fdinfo "Pid:" line.
 * ============================================================================= */
#include "fd.h"
#include "task.h"
#include "nsproxy.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

#define PIDFD_NONBLOCK  0x800        /* O_NONBLOCK */
#define PIDFD_THREAD    0x80         /* O_EXCL */

struct ofile* ofile_alloc_kind(int kind);

/* The process this pidfd names, or NULL once that process is gone (reaped, or
 * its pid now belongs to somebody else). */
static struct task* pidfd_task(const struct ofile* o) {
    struct task* t = task_find(o->pidfd_pid);
    if (!t || t->start_ms != o->pidfd_start) return NULL;
    return t;
}

/* A pidfd for global pid `gpid` (a task that exists), or a negative errno. */
int pidfd_create(int gpid, unsigned flags) {
    if (flags & ~(unsigned)(PIDFD_NONBLOCK | PIDFD_THREAD)) return -22;   /* EINVAL */
    struct task* t = gpid > 0 ? task_find(gpid) : NULL;
    if (!t || t->is_idle) return -3;                                      /* ESRCH */
    if (!(flags & PIDFD_THREAD) && task_tgid(t) != t->pid) return -22;    /* not a leader */
    struct ofile* o = ofile_alloc_kind(FD_PIDFD);
    if (!o) return -12;
    o->pidfd_pid   = t->pid;
    o->pidfd_start = t->start_ms;
    if (flags & PIDFD_NONBLOCK) o->nonblock = 1;
    int fd = fd_install_k(o);
    if (fd < 0) { ofile_unref(o); return -24; }                           /* EMFILE */
    /* A pidfd is ALWAYS close-on-exec (Linux sets O_CLOEXEC unconditionally). */
    fd_set_cloexec(fd, 1);
    return fd;
}

/* POLLIN (0x001) once every thread of the process has exited; POLLHUP (0x010)
 * as well once it has been reaped (or its pid recycled). */
uint32_t pidfd_readiness(const struct ofile* o) {
    struct task* t = pidfd_task(o);
    if (!t) return 0x001 | 0x010;
    if (task_group_alive(task_tgid(t))) return 0;
    return 0x001;
}

/* The global pid behind a pidfd descriptor of the caller, ESRCH-free: the
 * number even after the process is gone (waitid wants it to report ECHILD for
 * a non-child, not EBADF).  -9 (EBADF) for a descriptor that is not a pidfd. */
int pidfd_pid_of(int fd, int* live) {
    struct ofile* o = fd_get(fd);
    if (!o) return -9;
    if (o->kind != FD_PIDFD) { ofile_unref(o); return -9; }
    int pid = o->pidfd_pid;
    if (live) *live = pidfd_task(o) != NULL;
    ofile_unref(o);
    return pid;
}

/* pidfd_send_signal's target: the live task, or NULL (ESRCH) / EBADF. */
struct task* pidfd_target(int fd, int* err) {
    struct ofile* o = fd_get(fd);
    if (!o) { *err = -9; return NULL; }
    if (o->kind != FD_PIDFD) { ofile_unref(o); *err = -9; return NULL; }
    struct task* t = pidfd_task(o);
    ofile_unref(o);
    if (!t || t->state == TASK_DEAD) { *err = -3; return NULL; }
    return t;
}
