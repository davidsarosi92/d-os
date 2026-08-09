/* =============================================================================
 * timer.h — wall-clock-ish time service.
 *
 * Backed today by the legacy 8254 PIT programmed at 1000 Hz on IRQ0.
 * The same interface will eventually multiplex onto HPET / ARM generic
 * timer / TSC deadline; consumers should never reach for a specific
 * timer device directly.
 *
 * `timer_ticks_ms` is monotonic, never decreases, never wraps in any
 * realistic uptime (64-bit ms = 584 million years).
 * ============================================================================= */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Milliseconds since boot.  Defined to be 0 until the timer driver
 * registers (very early in module init), so a caller before that point
 * just reads back 0 — useful for boot-time diagnostics that want
 * "elapsed since X" without crashing if X happens before the timer is
 * up. */
uint64_t timer_ticks_ms(void);

/* §M53 — the same clock, in nanoseconds, from whatever source the machine
 * actually has: the ARM system counter, an invariant x86 TSC, or (if neither is
 * usable) the millisecond tick scaled up.  Callers never learn which.
 *
 * MONOTONIC and never decreasing, including across a task migrating between
 * CPUs whose counters are slightly skewed — see the clamp in ktime.c.  Starts
 * near zero at boot.  `timer_res_ns` reports the granularity actually achieved,
 * which is what `clock_getres` should answer and what makes a coarse fallback
 * visible instead of silent. */
uint64_t    timer_now_ns(void);
uint64_t    timer_res_ns(void);
const char* timer_source_name(void);
uint64_t    timer_source_hz(void);      /* 0 when running on the tick */

/* Install the clock.  Must run after the tick source is live, because the x86
 * calibration measures the TSC against it. */
void ktime_init(void);

/* Busy-wait for at least `ms` milliseconds.  Cooperative sleeping
 * (yielding to the scheduler) lands when M7 brings tasks. */
void timer_msleep(uint32_t ms);

#endif
