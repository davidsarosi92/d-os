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

/* Busy-wait for at least `ms` milliseconds.  Cooperative sleeping
 * (yielding to the scheduler) lands when M7 brings tasks. */
void timer_msleep(uint32_t ms);

#endif
