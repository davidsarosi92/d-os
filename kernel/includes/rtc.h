/* =============================================================================
 * rtc.h — wall-clock time source (M22.1 taskbar clock).
 *
 * Minimal read-only interface over the CMOS RTC.  No alarms, no
 * periodic interrupt — the GUI polls at its own pace and the PIT/LAPIC
 * remain the only tick sources.  On QEMU pass `-rtc base=localtime`
 * (run_qemu.sh does) so the values match the host clock.
 * ============================================================================= */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_time {
    uint8_t  sec, min, hour;        /* 0-59 / 0-59 / 0-23 */
    uint8_t  day, month;            /* 1-31 / 1-12        */
    uint16_t year;                  /* e.g. 2026          */
};

/* Read the current time.  Returns 0 on success, -1 if no RTC driver
 * registered (non-PC arch) — callers should then hide the clock. */
int rtc_read(struct rtc_time* out);

#endif
