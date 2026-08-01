/* =============================================================================
 * ib700.c — iBASE 700 ISA hardware watchdog driver (§M31 L3).
 *
 * This is the hard-lockup safety net the earlier watchdog layers can't provide:
 * L1 (heartbeat) and L2 (softlockup) both run on a scheduler TASK, so a HARD
 * lockup — the kernel spinning or `hlt`-ing with IRQs OFF on the only CPU (UP) —
 * stops them dead too, and the box is silently frozen forever.  A HARDWARE
 * watchdog keeps counting regardless of what the CPU is doing; if the kernel
 * stops petting it (because it's wedged), the device fires — and we route that
 * to an NMI (non-maskable, so it's delivered even with IRQs disabled), giving
 * the kernel a chance to log where it's stuck and recover (see the NMI lockup
 * handler in idt.c).
 *
 * The IB700 is trivial to drive: writing a 4-bit timeout code to the ENABLE port
 * (re)arms/reloads the countdown; writing anything to the DISABLE port stops it.
 * QEMU maps the code → seconds via a fixed table (code 0 = 30s … code 15 = 0s);
 * we use ~4s, comfortably above the ~500 ms pet interval.  With no `-device
 * ib700` present these port writes go nowhere (harmless), so petting is safe to
 * compile in unconditionally.
 *
 * Enable on the QEMU side with:  -device ib700 -action watchdog=inject-nmi
 * ============================================================================= */

#include "hal.h"
#include "watchdog.h"
#include <stdint.h>

#define IB700_PORT_ENABLE   0x443   /* write a timeout code here → (re)arm/reload */
#define IB700_PORT_DISABLE  0x441   /* write anything here        → stop          */
/* QEMU time_map[code] seconds = {30,28,26,24,22,20,18,16,14,12,10,8,6,4,2,0};
 * code 13 ≈ 4 s — 8× the pet interval, so normal operation never trips it. */
#define IB700_TIMEOUT_CODE  13

static int hw_wd_armed = 0;

void hw_watchdog_init(void) {
    /* Arm + start counting.  From here the watchdog TASK must pet within the
     * timeout or the device fires an NMI. */
    outb(IB700_PORT_ENABLE, IB700_TIMEOUT_CODE);
    hw_wd_armed = 1;
}

void hw_watchdog_pet(void) {
    if (!hw_wd_armed) return;
    outb(IB700_PORT_ENABLE, IB700_TIMEOUT_CODE);   /* reload the countdown */
}

void hw_watchdog_disable(void) {
    outb(IB700_PORT_DISABLE, 0);
    hw_wd_armed = 0;
}
