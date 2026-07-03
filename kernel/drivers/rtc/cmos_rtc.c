/* =============================================================================
 * cmos_rtc.c — MC146818-compatible CMOS real-time clock (read-only).
 *
 * Access pattern: write the register index to port 0x70, read the value
 * from port 0x71.  Three gotchas the code below handles:
 *
 *   1. Update-in-progress: while the chip copies its internal counters
 *      to the user registers (~2 ms window each second), reads return
 *      garbage.  We wait for status-A bit 7 to clear, then read, then
 *      re-read and compare — if the two snapshots differ, an update
 *      sneaked in between and we retry.
 *   2. BCD vs binary: status-B bit 2 says which encoding the RTC uses.
 *      QEMU uses BCD by default; real firmware varies.
 *   3. 12h vs 24h: status-B bit 1.  In 12h mode, hour bit 7 = PM.
 *
 * The century register (FADT-declared, usually 0x32) is ignored — we
 * assume 20xx, which is safe until 2100 and this is a teaching kernel.
 * ============================================================================= */

#include "rtc.h"
#include "hal.h"
#include "module.h"
#include <stdint.h>

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71

static int rtc_present = 0;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

static void read_raw(uint8_t* v /* [6]: s m h d mo y */) {
    v[0] = cmos_read(0x00);
    v[1] = cmos_read(0x02);
    v[2] = cmos_read(0x04);
    v[3] = cmos_read(0x07);
    v[4] = cmos_read(0x08);
    v[5] = cmos_read(0x09);
}

static uint8_t bcd2bin(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }

int rtc_read(struct rtc_time* out) {
    if (!rtc_present || !out) return -1;

    uint8_t a[6], b[6];
    int guard = 0;
    do {
        while (update_in_progress()) { /* ~2 ms worst case */ }
        read_raw(a);
        read_raw(b);
        if (++guard > 8) break;                 /* never wedge the caller */
    } while (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]);

    uint8_t sb   = cmos_read(0x0B);
    int     bcd  = !(sb & 0x04);
    int     h12  = !(sb & 0x02);
    uint8_t hour = a[2];
    uint8_t pm   = (uint8_t)(hour & 0x80);      /* only meaningful in 12h mode */
    hour &= 0x7F;

    out->sec   = bcd ? bcd2bin(a[0]) : a[0];
    out->min   = bcd ? bcd2bin(a[1]) : a[1];
    out->hour  = bcd ? bcd2bin(hour) : hour;
    out->day   = bcd ? bcd2bin(a[3]) : a[3];
    out->month = bcd ? bcd2bin(a[4]) : a[4];
    out->year  = (uint16_t)(2000 + (bcd ? bcd2bin(a[5]) : a[5]));

    if (h12) {
        if (pm && out->hour != 12) out->hour = (uint8_t)(out->hour + 12);
        if (!pm && out->hour == 12) out->hour = 0;    /* 12 AM = 00 */
    }
    return 0;
}

static int rtc_module_init(void) {
    /* Sanity probe: a missing RTC floats the bus — month 0xFF etc.
     * One plausibility check is enough to gate the feature. */
    uint8_t m = cmos_read(0x08);
    rtc_present = (m != 0xFF);
    return 0;
}

MODULE("cmos-rtc", "clock", rtc_module_init);
