/* =============================================================================
 * pit.c — Intel 8254 PIT driver, programmed for a 1000 Hz tick.
 *
 * The 8254 has a fixed input frequency of 1,193,182 Hz.  Channel 0 is
 * routed to IRQ0 via the master PIC.  We program it in mode 3 (square
 * wave generator) with a divisor of 1193, which lands at 1000.15 Hz —
 * close enough to "one millisecond per tick" that we treat each tick
 * as 1 ms in software.
 *
 * Register map:
 *   0x40 — channel 0 data port (read/write counter)
 *   0x41 — channel 1 data port (legacy DRAM refresh; don't touch)
 *   0x42 — channel 2 data port (PC speaker; don't touch)
 *   0x43 — mode/command register
 *
 * Mode/command byte for our program:
 *   bits 7..6 = 00  → channel 0
 *   bits 5..4 = 11  → access mode "lobyte/hibyte" (write low, then high)
 *   bits 3..1 = 011 → operating mode 3 (square wave)
 *   bit 0     = 0   → 16-bit binary counter (not BCD)
 *   ⇒ 0x36
 *
 * IRQ0 → vector 32 (after PIC remap, see kernel/hal/x86/idt.c).  The
 * handler bumps a 64-bit `ticks_ms` counter; `timer_ticks_ms` returns
 * that count, `timer_msleep` busy-waits on it.
 *
 * Reference: Intel 8254 datasheet, OSDev Wiki "Programmable Interval
 * Timer".
 * ============================================================================= */

#include "timer.h"
#include "ktimer.h"
#include "hal.h"
#include "hal_api.h"
#include "idt.h"
#include "module.h"
#include "task.h"
#include "usb.h"
#include "printf.h"
#include "lock.h"
#include <stdint.h>

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_FREQ  1193182u
#define PIT_HZ    1000u

/* Scheduler quantum in PIT ticks.  PIT runs at 1000 Hz, so 50 ticks =
 * 50 ms.  Long enough that short bursts of work never trigger a
 * context switch; short enough that two CPU-bound tasks feel
 * responsive.  Tunable later via /etc/d-os.conf. */
#define SCHED_QUANTUM_TICKS 50u

/* `volatile`: the IRQ writes, consumer code reads on the main thread.
 * Without volatile the compiler is allowed to cache the value across
 * the busy-wait loop of `timer_msleep`. */
static volatile uint64_t ticks_ms = 0;     /* PIT INTERRUPTS delivered — see below */

/* ---------------------------------------------------------------------------
 * THE MILLISECOND CLOCK COMES FROM A COUNTER, NOT FROM COUNTING INTERRUPTS.
 *
 * `ticks_ms` counts PIT interrupts, and an interrupt count is not a clock: an
 * interrupt that is late, coalesced by the emulator or held off by a
 * higher-priority one is simply never counted.  Measured on QEMU/TCG with the
 * guest's own `uptime` against the host's wall clock: 16.35 s of guest time for
 * 20.4 s of real time — the clock ran at 80 %.  Everything downstream inherited
 * it: every sleep and timeout 25 % long, `rec` reporting a correct capture as
 * "faster than real time" (803 ms for 1000), and both boot calibrations, which
 * measure against this clock, off by the same 1/0.8 — the LAPIC timer at 80 Hz
 * for a 100 Hz target (78362 ticks/ms measured against 62500 real), the TSC at
 * anything from 1.26 to 17.8 GHz from one boot to the next.  aarch64 never had
 * the problem because it reads CNTPCT, a counter.  (§M82 session, 2026-09-25.)
 *
 * So once ACPI names the PM timer — a free-running 3.579545 MHz counter every
 * PIIX4/ICH chipset has, and QEMU's too — the ms clock is DERIVED from it and the
 * interrupt only drives the scheduler.  The switch is continuous (it resumes
 * from the value the interrupt count had reached, so nothing sees time go
 * backwards), and without a PM timer nothing changes.
 *
 * The 24-bit counter wraps every 4.69 s, so it is EXTENDED to 64 bits on every
 * read: any caller may update the extension, under a TRYLOCK, and one that
 * finds it busy (a nested interrupt, an NMI) reads the published value through a
 * sequence count instead — bounded, never blocking, never a deadlock in a path
 * that cannot wait.  Every tick reads it, so a wrap cannot be missed while the
 * machine runs at all. */
#define PM_TIMER_HZ 3579545ull
static volatile uint16_t pm_port;           /* 0 = no PM timer: count interrupts */
static uint32_t          pm_mask;
static spinlock_t        pm_lock = SPINLOCK_INIT;
static volatile uint32_t pm_seq;
static volatile uint32_t pm_last_raw;
static volatile uint64_t pm_acc;            /* extended counter, PM ticks        */
static uint64_t          pm_acc0, pm_ms0;   /* the continuity offset at switch   */

static uint64_t pm_ms_from(uint64_t acc) {
    return pm_ms0 + ((acc - pm_acc0) * 1000ull) / PM_TIMER_HZ;
}

void timer_use_pm_timer(uint16_t port, int bits32) {
    if (!port || pm_port) return;
    uint32_t fl = spin_lock_irqsave(&pm_lock);
    pm_mask     = bits32 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    pm_last_raw = inl(port) & pm_mask;
    pm_acc      = 0;
    pm_acc0     = 0;
    pm_ms0      = ticks_ms;
    pm_port     = port;
    spin_unlock_irqrestore(&pm_lock, fl);
    kprintf("timer: ms clock now derived from the ACPI PM timer at io %x (%u-bit) "
            "instead of counting PIT interrupts\n", port, bits32 ? 32u : 24u);
}

uint64_t timer_ticks_ms(void) {
    if (!pm_port) return ticks_ms;
    if (spin_trylock(&pm_lock)) {
        uint32_t fl = hal_intr_save();
        __atomic_add_fetch(&pm_seq, 1, __ATOMIC_ACQ_REL);         /* odd: writing */
        uint32_t raw = inl(pm_port) & pm_mask;
        pm_acc += (uint32_t)((raw - pm_last_raw) & pm_mask);
        pm_last_raw = raw;
        uint64_t acc = pm_acc;
        __atomic_add_fetch(&pm_seq, 1, __ATOMIC_ACQ_REL);         /* even: stable */
        hal_intr_restore(fl);
        spin_unlock(&pm_lock);
        return pm_ms_from(acc);
    }
    /* Busy: read the published pair consistently, plus what has passed since. */
    for (int tries = 0; tries < 64; tries++) {
        uint32_t s0 = __atomic_load_n(&pm_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        uint64_t acc = pm_acc;
        uint32_t last = pm_last_raw;
        if (__atomic_load_n(&pm_seq, __ATOMIC_ACQUIRE) != s0) continue;
        uint32_t raw = inl(pm_port) & pm_mask;
        return pm_ms_from(acc + (uint32_t)((raw - last) & pm_mask));
    }
    return pm_ms_from(pm_acc);             /* the writer is this CPU, mid-update */
}

/* The raw INTERRUPT count, for the one thing that must measure delivery rather
 * than time: the IRQ0 starvation check in idt.c. */
uint64_t timer_pit_irqs(void) { return ticks_ms; }

void timer_msleep(uint32_t ms) {
    uint64_t deadline = timer_ticks_ms() + ms;
    while (timer_ticks_ms() < deadline) {
        /* Atomic enable+halt — sleep until the next interrupt arrives,
         * including our own IRQ0.  Cheap idle for now; could be
         * replaced by `task_yield()` for tighter scheduling. */
        hal_cpu_idle();
    }
}

/* IRQ0 handler.  Runs with interrupts disabled (we don't enable nested
 * interrupts), so the simple ++ on a 64-bit value is race-free relative
 * to readers that take a snapshot via `timer_ticks_ms`.
 *
 * On 32-bit x86 the 64-bit increment compiles to a 2-instruction
 * add+adc — readers can in principle observe a torn value if they
 * preempted us mid-increment.  Since we disable interrupts during IRQs
 * and have no preemption today, the window does not materialize.  When
 * SMP or preemptive kernel threads land we'll need an atomic variant.
 *
 * Preemption (M13): bump a per-tick counter; on every quantum boundary,
 * post a deferred reschedule request that isr_handler will honor AFTER
 * pic_eoi.  We never context-switch from inside the handler itself —
 * doing so before EOI would leave the PIC convinced IRQ0 is still
 * in-service and stop further timer ticks.  See task.c header for the
 * full rationale. */
static uint32_t quantum_count = 0;
static uint32_t usb_poll_count = 0;

/* Frequency at which we drain the xHCI Event Ring from the timer.
 * The HC posts a Transfer Event every time it DMA's a HID report into
 * our buffer; the periodic poll picks them up since we don't have
 * MSI/MSI-X wired in.  10 ms is more than enough for an 8 ms HID
 * polling interval. */
#define USB_POLL_TICKS 10u

static void pit_irq(struct int_frame* f) {
    (void)f;
    ticks_ms++;
    if (pm_port && !(ticks_ms & 255)) (void)timer_ticks_ms();   /* keep the 24-bit extension fresh */

    /* §M53 — fire due deadline timers HERE, on the real tick.
     *
     * The first version hooked this into schedule_check, which looked like the
     * same thing and is not: schedule_check runs at the QUANTUM rate (every
     * SCHED_QUANTUM_TICKS ticks, i.e. 100 Hz), so every timer was up to 10 ms
     * late no matter what deadline it asked for.  `ktimer` measured it — a
     * 500 us sleep taking 9.7 ms — which is exactly the kind of thing a
     * timer service with no accuracy measurement would have shipped with. */
    ktimer_expire();

    if (++quantum_count >= SCHED_QUANTUM_TICKS) {
        quantum_count = 0;
        schedule_request();
    }
    if (++usb_poll_count >= USB_POLL_TICKS) {
        usb_poll_count = 0;
        xhci_poll();
    }
}

/* -------------------------------------------------------------------------- */
/* Module init.                                                                */
/* -------------------------------------------------------------------------- */

static int pit_module_init(void) {
    /* Compute and program the divisor for our target frequency. */
    uint32_t divisor = PIT_FREQ / PIT_HZ;        /* 1193 → ~1000.15 Hz */
    if (divisor < 1)     divisor = 1;
    if (divisor > 65535) divisor = 65535;

    outb(PIT_CMD, 0x36);                         /* ch0, lo/hi, mode 3, binary */
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));    /* low byte first */
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    /* Hook IRQ0.  irq_install also unmasks the line on the PIC. */
    irq_install(0, pit_irq);

    kprintf("pit: %u Hz (divisor %u), preempt quantum=%u ticks\n",
            PIT_HZ, divisor, SCHED_QUANTUM_TICKS);
    return 0;
}

MODULE("8254-pit", "timer", pit_module_init);
