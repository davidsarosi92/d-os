/* =============================================================================
 * tsc.c — the high-resolution counter on x86, shared by i386 and x86_64 (§M53).
 *
 * The tick is not a clock.  `timer_ticks_ms` counts PIT interrupts at 1000 Hz,
 * so every timestamp it produces is a multiple of one millisecond and every
 * duration measured with it carries ±1 ms of quantisation.  That is fine for
 * "has the watchdog deadline passed" and useless for anything a program times:
 * a profiler, a frame budget, a `clock_gettime(CLOCK_MONOTONIC)` that a libc
 * subtracts from itself, or a sleep that is supposed to end at a deadline
 * rather than at the next tick.
 *
 * x86's answer is the TSC — a per-CPU counter incremented at a fixed rate, read
 * with one instruction and no bus traffic.  Two properties have to be TRUE
 * before it can back a clock, and both are checked rather than assumed:
 *
 *   1. INVARIANT.  Early TSCs counted core clock cycles, so they slowed down
 *      with frequency scaling and stopped in deep C-states.  CPUID leaf
 *      0x80000007 EDX bit 8 is the architectural promise that the TSC runs at
 *      a constant rate regardless of power state.  Without it the counter is
 *      still monotonic but its RATE is a lie, which is worse than a coarse
 *      clock — a program would see time speed up and slow down.
 *
 *   2. SYNCHRONISED ACROSS CPUS — assumed, not verified, and the reason
 *      `timer_now_ns` is documented as monotonic per-CPU rather than globally.
 *      Every machine this runs on (QEMU, and any single-socket modern x86)
 *      synchronises the TSC at reset; multi-socket hardware historically did
 *      not.  A cross-CPU skew shows up as time appearing to step BACKWARD when
 *      a task migrates.  The clock guards against that with a monotonicity
 *      clamp in ktime.c rather than pretending the problem cannot happen.
 *
 * If either check fails the kernel keeps the millisecond tick: a coarse clock
 * that is right beats a fine one that is wrong.
 *
 * CALIBRATION is against the PIT, the same way lapic_timer_calibrate does it —
 * sample, wait a known number of PIT milliseconds, sample again.  The PIT is
 * crystal-derived and does not scale, which is exactly what makes it the right
 * reference even though it is the thing we are trying to stop using.
 * ============================================================================= */

#include "hal_api.h"
#include "timer.h"
#include "printf.h"
#include <stdint.h>

static uint64_t g_tsc_hz;          /* 0 = unusable, keep the millisecond tick */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    /* `rdtsc` is not a serialising instruction, so the value can be sampled a
     * few instructions out of program order.  For a clock read that is noise
     * far below the calibration error; a serialising `cpuid` before it would
     * cost more than the imprecision it removes. */
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b,
                         uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

static int tsc_is_invariant(void) {
    uint32_t a, b, c, d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000007u) return 0;             /* leaf not implemented */
    cpuid(0x80000007u, &a, &b, &c, &d);
    return (d & (1u << 8)) != 0;               /* EDX.8 = InvariantTSC */
}

/* CPUID.1:ECX[31] — "a hypervisor is present".  Architecturally reserved-zero
 * on bare metal, which is what makes it a reliable signal.
 *
 * Why it counts here: a hypervisor virtualises the TSC.  The guest reads a
 * scaled, offset view of the host counter, so it does NOT track a guest core's
 * frequency scaling or C-states — the very failure modes the invariant bit
 * exists to warn about.  QEMU's default CPU models do not set the invariant
 * bit (they model an older part), and refusing the TSC there would leave every
 * development machine on the millisecond tick, which is precisely the thing
 * this milestone exists to stop doing.
 *
 * Not a blanket excuse: this only widens the rule to "constant-rate by
 * construction", and the fallback stays for anything that satisfies neither. */
static int hypervisor_present(void) {
    uint32_t a, b, c, d;
    cpuid(0u, &a, &b, &c, &d);
    if (a < 1u) return 0;
    cpuid(1u, &a, &b, &c, &d);
    return (c & (1u << 31)) != 0;
}

/* Called once on the BSP, after the PIT is ticking. */
int hal_hires_init(void) {
    const char* why;
    if (tsc_is_invariant())        why = "invariant";
    else if (hypervisor_present()) why = "virtualised (constant-rate)";
    else {
        kprintf("tsc: neither invariant nor virtualised — clock stays on the "
                "1 ms tick\n");
        g_tsc_hz = 0;
        return 0;
    }

    /* 50 ms is long enough that the ±1 ms edge error is ~2%, and short enough
     * that nobody notices it during boot.  Two samples bracket the wait so the
     * measured interval is exactly the counted milliseconds. */
    uint64_t t0 = rdtsc();
    timer_msleep(50);
    uint64_t t1 = rdtsc();

    uint64_t hz = (t1 - t0) * 20u;             /* 50 ms → ×20 = per second */
    /* Sanity: anything outside 1 MHz..100 GHz means the PIT was not running
     * yet and we measured nothing.  Refuse rather than install a clock whose
     * rate is fiction. */
    if (hz < 1000000ull || hz > 100000000000ull) {
        kprintf("tsc: calibration out of range (%u kHz) — keeping the tick\n",
                (unsigned)(hz / 1000));
        g_tsc_hz = 0;
        return 0;
    }
    g_tsc_hz = hz;
    /* No width specifiers: this kernel's printf has none (see printf.c), and a
     * "%03u" comes out as the literal text — which is how the first version of
     * this line reported a frequency of "2.%03u MHz". */
    kprintf("tsc: %s, %u kHz — clock resolution %u ns\n",
            why, (unsigned)(hz / 1000u),
            (unsigned)((1000000000ull / hz) ? (1000000000ull / hz) : 1));
    return 1;
}

uint64_t hal_hires_ticks(void) { return rdtsc(); }
uint64_t hal_hires_hz(void)    { return g_tsc_hz; }
