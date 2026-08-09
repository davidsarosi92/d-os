/* =============================================================================
 * ktime.c — one monotonic nanosecond clock, whatever the hardware offers (§M53).
 *
 * Everything above this file asks the same question — "what time is it, in
 * nanoseconds since boot" — and never learns whether the answer came from a
 * 62.5 MHz ARM system counter, an invariant x86 TSC, or a 1000 Hz interrupt
 * counter on a machine that has neither.  That is the entire point: a timer
 * subsystem, a `clock_gettime`, and a sleep that ends at a DEADLINE rather than
 * at the next tick all need resolution finer than the tick, and none of them
 * should carry an arch branch to get it.
 *
 * WHY NOT JUST USE THE TICK.  `timer_ticks_ms` counts interrupts, so it moves
 * in 1 ms steps: two timestamps taken microseconds apart are equal, a duration
 * measured across it is quantised to ±1 ms, and a program that sleeps for
 * 100 µs sleeps for a millisecond.  For a scheduler deadline that is fine.  For
 * anything a program times it is the difference between a clock and a counter.
 *
 * THE MONOTONICITY CLAMP.  The hardware counter is read without a lock — that
 * is what makes it cheap — and on x86 it is PER-CPU, synchronised at reset by
 * every machine we run on but not guaranteed by the architecture.  A task that
 * migrates between two slightly skewed CPUs would otherwise see time step
 * BACKWARD, and time going backward breaks things in ways that are very hard to
 * trace back here (a deadline computed as now+d lands in the past; a duration
 * comes out negative and, unsigned, enormous).  So the clock remembers the last
 * value it handed out and never returns less than that.
 *
 * The clamp costs one compare-and-swap per read and buys the invariant every
 * caller already assumes.  It does NOT paper over a broken counter: skew still
 * shows up as time standing still on the lagging CPU, which is visible in
 * `ktime` rather than silent.
 * ============================================================================= */

#include "timer.h"
#include "hal_api.h"
#include <stdint.h>

/* Weak fallbacks so an architecture that has no high-resolution counter (or has
 * not wired one up yet) links and simply keeps the millisecond tick. */
int      hal_hires_init(void)  __attribute__((weak));
int      hal_hires_init(void)  { return 0; }
uint64_t hal_hires_ticks(void) __attribute__((weak));
uint64_t hal_hires_ticks(void) { return 0; }
uint64_t hal_hires_hz(void)    __attribute__((weak));
uint64_t hal_hires_hz(void)    { return 0; }

static uint64_t g_hz;              /* counter frequency; 0 = fall back to ticks */
static uint64_t g_base;            /* counter value at init, so the clock starts near 0 */
static volatile uint64_t g_last;   /* monotonicity clamp */

void ktime_init(void) {
    if (hal_hires_init() && hal_hires_hz()) {
        g_hz   = hal_hires_hz();
        g_base = hal_hires_ticks();
    } else {
        g_hz = 0;
    }
}

/* Convert a counter delta to nanoseconds WITHOUT overflowing 64 bits.
 *
 * The obvious `d * 1000000000 / hz` overflows after about nine seconds at
 * 2 GHz, which is exactly long enough to look correct during a boot test and
 * then wrap in front of a user.  Splitting into whole seconds plus a remainder
 * keeps both products small: the remainder is below `hz`, so `r * 1e9` is safe
 * for any counter slower than ~18 GHz. */
static inline uint64_t ticks_to_ns(uint64_t d, uint64_t hz) {
    uint64_t s = d / hz;
    uint64_t r = d % hz;
    return s * 1000000000ull + (r * 1000000000ull) / hz;
}

uint64_t timer_now_ns(void) {
    uint64_t ns;
    if (g_hz) ns = ticks_to_ns(hal_hires_ticks() - g_base, g_hz);
    else      ns = timer_ticks_ms() * 1000000ull;

    /* Never hand out less than we handed out before — see the header. */
    for (;;) {
        uint64_t last = __atomic_load_n(&g_last, __ATOMIC_RELAXED);
        if ((int64_t)(ns - last) <= 0) return last;
        if (__atomic_compare_exchange_n(&g_last, &last, ns, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return ns;
    }
}

uint64_t timer_res_ns(void) {
    if (!g_hz) return 1000000ull;                  /* the 1 ms tick */
    uint64_t r = 1000000000ull / g_hz;
    return r ? r : 1;                              /* sub-ns counters read as 1 */
}

const char* timer_source_name(void) {
    return g_hz ? "hires counter" : "tick (1 ms)";
}

uint64_t timer_source_hz(void) { return g_hz; }
