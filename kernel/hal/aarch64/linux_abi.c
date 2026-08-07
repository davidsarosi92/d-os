/* =============================================================================
 * linux_abi.c — the Linux/arm64 personality (PLAN_AARCH64 A2).
 *
 * Compare this file's length with its x86 siblings: `hal/x86/linux_abi.c` is
 * 1211 lines and `hal/x86_64/linux_abi.c` is 1064.  The difference is not that
 * aarch64 supports less — it is that §M50 moved the translation into a shared
 * engine, so this file is only the part that is genuinely architecture-specific:
 *
 *     which registers carry the syscall number, its arguments and its result.
 *
 * That is the AArch64 Linux calling convention (x8 = number, x0..x5 = args,
 * result in x0), and it is six lines.  Everything else — what number 63 means,
 * how to do it — lives in kernel/core/abi_linux.c (as data) and
 * kernel/core/abi_engine.c (as shared handlers).
 *
 * If this file ever grows a large `switch`, something has gone wrong: the case
 * belongs in the engine, so that x86 gets it too.
 * ============================================================================= */

#include "abi.h"
#include "task.h"
#include "fd.h"
#include "syscall.h"
#include "printf.h"
#include <stdint.h>

/* Matches the trapframe laid down by vectors.S. */
struct trapframe {
    uint64_t x[31];
    uint64_t _pad;
    uint64_t elr;
    uint64_t spsr;
};

void aarch64_user_exit(void);          /* usermode.S — excursion teleport */

#define LNX_ENOSYS 38

/* Linux/arm64 numbers for exit and exit_group.  Deliberately NOT in the shared
 * number map: terminating a process is arch-coupled here, because a program run
 * as a synchronous EXCURSION from the kernel (proc_exec_elf — how every
 * self-test runs) must teleport back to the kernel stack it came from, and that
 * stack is saved per arch.  See kernel/core/abi_linux.c for the note. */
#define LNX_ARM64_exit        93
#define LNX_ARM64_exit_group  94

void linux_syscall_dispatch(struct trapframe* tf) {
    struct task* me = task_current();
    /* §M47.2 — arm the ring-3 pointer gate for the duration.  Both x86 layers
     * went two milestones with this left unset, which silently disabled the
     * first of §M46's three boundary layers for the ENTIRE musl userland and
     * failed in no visible way.  Armed here from the first line of the port. */
    int prev = me ? me->in_user_syscall : 0;
    if (me) me->in_user_syscall = 1;

    unsigned long nr = tf->x[8];

    if (nr == LNX_ARM64_exit || nr == LNX_ARM64_exit_group) {
        if (me && me->user_task) { fd_close_all(); task_exit_code((int)tf->x[0]); }
        aarch64_user_exit();                     /* excursion: teleport back */
        if (me) me->in_user_syscall = prev;      /* unreachable */
        return;
    }

    long r;
    if (abi_dispatch(&abi_map_linux_arm64, nr,
                     tf->x[0], tf->x[1], tf->x[2],
                     tf->x[3], tf->x[4], tf->x[5], &r)) {
        tf->x[0] = (uint64_t)r;
    } else {
        /* Name it loudly.  A silent -ENOSYS is how a musl program turns into a
         * mystery hang; the number printed here is exactly what to add to the
         * arm64 table (and, if the meaning is new, to the vocabulary). */
        kprintf("linux/arm64: unhandled syscall %lu\n", nr);
        tf->x[0] = (uint64_t)(-LNX_ENOSYS);
    }

    if (me) me->in_user_syscall = prev;
}
