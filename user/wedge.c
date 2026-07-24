/* =============================================================================
 * wedge.c — a deliberately WEDGED ring-3 program, the §M46 test artifact.
 *
 * It spins forever in userland and NEVER issues a syscall, so it never reaches
 * a cooperative yield point: plain `kill` (the kthread_stop contract) cannot
 * reclaim it.  `fkill` (force-kill) can — the timer preempts this loop in ring 3
 * (where it holds no kernel locks), and the kernel's force-kill safe point tears
 * it down there.  Stand-in for a frozen GUI app (e.g. a hung browser).
 *
 * Links against the in-tree libc, but calls nothing from it after _start.
 * ============================================================================= */

#include "libc.h"

int main(void) {
    /* Never yield, never syscall — just burn ring-3 cycles forever. */
    for (;;) {
        for (volatile long j = 0; j < 1000000; j++) { }
    }
    return 0;
}
