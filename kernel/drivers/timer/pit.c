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
#include "hal.h"
#include "hal_api.h"
#include "idt.h"
#include "module.h"
#include "task.h"
#include "usb.h"
#include "printf.h"
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
static volatile uint64_t ticks_ms = 0;

uint64_t timer_ticks_ms(void) {
    return ticks_ms;
}

void timer_msleep(uint32_t ms) {
    uint64_t deadline = ticks_ms + ms;
    while (ticks_ms < deadline) {
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
