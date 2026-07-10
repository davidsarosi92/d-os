/* =============================================================================
 * pl031_rtc.c — ARM PL031 real-time clock (M21 Phase K — GUI taskbar clock).
 *
 * The x86 taskbar clock reads the CMOS RTC (cmos_rtc.c, I/O ports 0x70/0x71).
 * QEMU's `virt` board has no CMOS; it exposes an ARM PL031 RTC at MMIO
 * 0x0901_0000 whose data register (DR) reads a 32-bit count of seconds since
 * the Unix epoch.  This driver provides the same `rtc_read()` interface
 * (rtc.h) the desktop shell polls, so shell_vista.c's clock works unchanged on
 * ARM.  Pass `-rtc base=localtime` to QEMU for host-local wall-clock values.
 *
 * The epoch-seconds → civil (Y/M/D) conversion is Howard Hinnant's public-
 * domain days_from_civil inverse (civil_from_days) — branch-light and correct
 * for the proleptic Gregorian calendar.
 * ============================================================================= */

#include "rtc.h"
#include <stdint.h>

#define PL031_BASE  0x09010000UL
#define PL031_DR    0x000               /* data register: seconds since epoch */

int rtc_read(struct rtc_time* out) {
    if (!out) return -1;

    uint32_t secs = *(volatile uint32_t*)(PL031_BASE + PL031_DR);
    uint32_t days = secs / 86400u;
    uint32_t rem  = secs % 86400u;

    out->hour = (uint8_t)(rem / 3600u);
    out->min  = (uint8_t)((rem % 3600u) / 60u);
    out->sec  = (uint8_t)(rem % 60u);

    /* days since 1970-01-01 → (year, month, day), Gregorian. */
    int64_t  z   = (int64_t)days + 719468;                  /* shift epoch to 0000-03-01 */
    int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);            /* [0, 146096] */
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  y   = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365] */
    uint64_t mp  = (5 * doy + 2) / 153;                     /* [0, 11] (Mar=0) */
    uint64_t d   = doy - (153 * mp + 2) / 5 + 1;            /* [1, 31] */
    uint64_t m   = mp < 10 ? mp + 3 : mp - 9;               /* [1, 12] */
    y += (m <= 2);

    out->year  = (uint16_t)y;
    out->month = (uint8_t)m;
    out->day   = (uint8_t)d;
    return 0;
}
