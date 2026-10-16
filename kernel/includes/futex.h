/* =============================================================================
 * futex.h — the futex primitive behind every threading library (M35, §M89).
 *
 * futex_wait parks the caller iff *uaddr still equals `val` (checked under the
 * bucket lock, which is what closes the lost-wakeup window) until a wake, an
 * absolute deadline on the timer_now_ns() timeline (0 = none) or a signal.
 * futex_wake wakes waiters on `uaddr`.  Both return 0 / a count, or a
 * NEGATIVE Linux errno — the ABI layer passes that straight through.
 * ============================================================================= */
#ifndef DOS_FUTEX_H
#define DOS_FUTEX_H

#include <stdint.h>

/* The Linux op word: the command in the low bits, two modifier flags. */
#define FUTEX_CMD_MASK        0x7F
#define FUTEX_PRIVATE_FLAG    0x80
#define FUTEX_CLOCK_REALTIME  0x100
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10

#define FUTEX_EAGAIN     11
#define FUTEX_EINTR       4
#define FUTEX_EFAULT     14
#define FUTEX_EINVAL     22
#define FUTEX_ETIMEDOUT 110

long futex_wait(uintptr_t uaddr, uint32_t val, uint64_t deadline_ns);
long futex_wake(uintptr_t uaddr, int count);

/* NEXT #14 — a record of the last few CLONE_CHILD_CLEARTID clears made at
 * thread exit (pid, word, whether the store happened, whether anybody was
 * parked in the bucket).  Lock-free and allocation-free so the exit path can
 * call it; `futex` prints it.  Read beside `ps`'s "futex <addr> want <v>". */
void futex_note_clear(int pid, uintptr_t addr, int stored, int woke);

#endif
