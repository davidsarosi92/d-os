/* =============================================================================
 * timer.c — ARM architected generic timer for the AArch64 port (M21 Phase B).
 *
 * The generic timer is ARM's per-CPU periodic-tick source — the replacement
 * for the x86 PIT / LAPIC timer.  It is a set of system registers (no MMIO):
 *
 *   CNTFRQ_EL0     — the fixed counter frequency in Hz (QEMU `virt`: 62.5 MHz).
 *   CNTPCT_EL0     — the always-running 64-bit physical counter (a monotonic
 *                    tick source, the ARM analogue of the TSC).
 *   CNTP_TVAL_EL0  — a 32-bit down-counter; the timer fires when it reaches 0.
 *                    Writing it arms "fire N counter-ticks from now".
 *   CNTP_CTL_EL0   — control: bit0 ENABLE, bit1 IMASK, bit2 ISTATUS.
 *
 * We use the *non-secure EL1 physical* timer, whose interrupt on the QEMU
 * `virt` board is PPI 14 → GIC INTID 30.  EL1 access to the physical timer
 * was granted in boot.S (CNTHCTL_EL2.EL1PCEN/EL1PCTEN) before the drop to EL1.
 *
 * Each interrupt we re-arm TVAL for the next interval — a one-shot rearmed
 * periodically, which is the standard architected-timer tick pattern (there
 * is no auto-reload register).  A monotonically increasing `tick_count` gives
 * the scheduler (Phase C) its quantum source and gives timer_ticks_ms() a
 * wall-clock-ish base.
 * ============================================================================= */

#include "ktimer.h"
#include <stdint.h>

void uart_early_puts(const char* s);
void gic_register_handler(uint32_t intid, void (*fn)(uint32_t));
void gic_enable_irq(uint32_t intid);

/* Scheduler preemption hook (task.c, M21 Phase C).  Weak so the Phase A/B
 * builds — which do not link the core scheduler — still assemble; when the
 * core is linked, the timer sets the per-CPU need_resched flag each tick and
 * the IRQ-exit path (gic.c) acts on it. */
void schedule_request(void) __attribute__((weak));

/* USB xHCI event/HID-report poll (xhci.c) — the ARM equivalent of the x86 PIT
 * tick calling xhci_poll().  Weak so builds without the USB stack still link.
 * Driven on the BSP only (see timer_isr) so the two CPUs' ticks don't race on
 * the controller's rings. */
void xhci_poll(void) __attribute__((weak));
void xhci_poll(void) { }
int  this_cpu_id(void);                 /* percpu.c */

/* GIC INTID of the non-secure EL1 physical timer on QEMU `virt` (PPI 14). */
#define TIMER_INTID 30

/* CNTP_CTL_EL0 bits. */
#define CNTP_CTL_ENABLE (1u << 0)
#define CNTP_CTL_IMASK  (1u << 1)

static volatile uint64_t tick_count;   /* incremented once per interrupt      */
static uint32_t          interval;     /* counter-ticks between interrupts    */
static uint32_t          hz;           /* configured tick rate                */
static uint64_t          cnt_base;     /* CNTPCT at timer_init — the clock origin */

/* ---- system-register accessors --------------------------------------------- */
static inline uint64_t read_cntfrq(void) {
    uint64_t v; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}
static inline void write_cntp_tval(uint32_t v) {
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"((uint64_t)v));
}
static inline void write_cntp_ctl(uint32_t v) {
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"((uint64_t)v));
}
static inline uint64_t read_cntpct(void) {
    uint64_t v; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(v)); return v;
}

/* Interrupt handler (called from the GIC dispatcher).  Re-arm + count. */
static void timer_isr(uint32_t intid) {
    (void)intid;
    tick_count++;
    write_cntp_tval(interval);          /* schedule the next interrupt        */

    /* §M53 — due deadline timers fire on the tick (see the x86 twin for why
     * this must NOT live in schedule_check).  This arch ticks at 100 Hz, so
     * 10 ms is its accuracy floor until the generic timer is driven as a
     * one-shot deadline instead of a periodic source; `ktimer` reports the
     * lateness actually observed rather than leaving it to be assumed. */
    ktimer_expire();

    if (this_cpu_id() == 0) xhci_poll();   /* service USB (BSP only)          */

    /* Drive preemption: flag a reschedule on this CPU.  gic.c runs the
     * actual schedule_check() after EOI (so the timer keeps firing on
     * whoever runs next). */
    if (schedule_request) schedule_request();
}

/* Program the timer for `tick_hz` interrupts per second and start it. */
void timer_init(uint32_t tick_hz) {
    hz       = tick_hz;
    uint64_t freq = read_cntfrq();
    /* Zero point for timer_ticks_ms.  Set by whichever CPU initialises first;
     * the others re-arm their own timer but must NOT move the origin, or the
     * clock would jump backward as each AP comes up. */
    if (!cnt_base) cnt_base = read_cntpct();
    interval = (uint32_t)(freq / tick_hz);

    gic_register_handler(TIMER_INTID, timer_isr);
    gic_enable_irq(TIMER_INTID);

    write_cntp_tval(interval);
    write_cntp_ctl(CNTP_CTL_ENABLE);    /* enable, unmasked (IMASK = 0)       */

    uart_early_puts("aarch64: generic timer armed (CNTP, INTID 30)\n");
}

/* Monotonic tick count since timer_init — Phase C's scheduler quantum base. */
uint64_t timer_ticks(void) {
    return tick_count;
}

/* Elapsed milliseconds since timer_init.
 *
 * §M53 — READ FROM THE COUNTER, not from the interrupt count.  This used to be
 * `tick_count * 1000 / hz`, and `tick_count` is a single global that EVERY
 * CPU's timer ISR increments — while `hz` is the per-CPU rate.  So on an
 * N-CPU machine the millisecond clock advanced N TIMES TOO FAST: with -smp 2 a
 * 100 ms sleep really lasted about 50 ms, with -smp 4 about 25.  Every timeout,
 * watchdog deadline and `sleep` on this architecture was wrong by the CPU
 * count, silently, because nothing had a second opinion to check it against.
 *
 * The nanosecond clock is that second opinion, and it found this within
 * minutes of existing: `ktime` reported a 100 ms sleep measuring 57 ms.
 *
 * CNTPCT is the right source anyway — it is exact, needs no coordination
 * between CPUs, and does not care how many of them are taking interrupts. */
uint64_t timer_ticks_ms(void) {
    uint64_t f = read_cntfrq();
    if (!f || !cnt_base) return 0;      /* before timer_init — see the header */
    return ((read_cntpct() - cnt_base) * 1000ULL) / f;
}

/* Raw 64-bit counter — high-resolution monotonic source (TSC analogue). */
uint64_t timer_raw_count(void) {
    return read_cntpct();
}

/* =============================================================================
 * §M53 — the high-resolution clock source.
 *
 * AArch64 needs no calibration and no invariance check: the architecture
 * DEFINES CNTPCT_EL0 as a fixed-frequency counter and CNTFRQ_EL0 as its rate in
 * Hz, both readable from EL1 with one instruction.  That is the whole driver —
 * which is a fair illustration of why the x86 twin (tsc.c) is eighty lines of
 * checking and calibrating to establish the same two facts.
 * ============================================================================= */
int hal_hires_init(void) {
    uint64_t f = read_cntfrq();
    return (f >= 1000000ull) ? 1 : 0;   /* under 1 MHz is not worth the trouble */
}
uint64_t hal_hires_ticks(void) {
    uint64_t v;
    /* isb first: CNTPCT reads may be speculated ahead of surrounding code, and
     * a clock that can be sampled early is a clock that can go backward across
     * two reads.  Architecturally required for an ordered read. */
    __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
uint64_t hal_hires_hz(void) { return read_cntfrq(); }
