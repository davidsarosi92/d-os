/* =============================================================================
 * usb_hid.c — USB HID boot-keyboard class driver.
 *
 * Scope: decode the fixed 8-byte boot-protocol keyboard report
 * (HID 1.11 §B.1), diff successive reports for fresh key-down events,
 * and route the resulting (keycode, modifiers) pair through
 * `keymap_translate` to get an ASCII char that we push into the
 * focused VC's input ring.
 *
 * Boot protocol — fixed 8-byte layout:
 *   byte 0   : modifier mask (HID_MOD_*, identical to KBD_MOD_*)
 *   byte 1   : reserved
 *   bytes 2-7: up to 6 currently-active HID Usage IDs
 *
 * Because the universal keycode the keymap layer expects IS the USB
 * HID Usage ID (see keymap.h), this driver does zero scancode
 * translation — we pass `r->keys[i]` through verbatim.  And because
 * KBD_MOD_* mirrors HID_MOD_* bit-for-bit, the modifier byte goes
 * through unchanged too.
 *
 * Boot-mode trade-off: a boot keyboard contractually emits this exact
 * layout regardless of its full report descriptor (HID 1.11 §4.3).
 * That means we skip writing a report-descriptor parser today —
 * deferred to a future milestone when a non-boot device shows up.
 * ============================================================================= */

#include "usb.h"
#include "vc.h"
#include "keymap.h"
#include <stdint.h>
#include <stddef.h>

/* The old `usb_hid_kbd_lower` / `usb_hid_kbd_upper` tables from M15
 * are gone: their content lives in `kbd_layout_us` (layouts.c) now,
 * accessed via keymap_translate.  Removed from usb.h too. */

/* Last 6 active keys, to compute new-down deltas. */
static uint8_t prev_keys[6];

static int contains(const uint8_t* arr, int n, uint8_t k) {
    for (int i = 0; i < n; i++) if (arr[i] == k) return 1;
    return 0;
}

void usb_hid_kbd_handle_report(const uint8_t* report) {
    const struct hid_boot_kbd_report* r = (const struct hid_boot_kbd_report*)report;
    uint8_t mods = r->modifiers;

    /* For every key in the new report not in the old one, it's a fresh
     * press — translate and route.  Skip ErrorRollOver (0x01) and the
     * other phantom-key reports (HID 1.11 §B.1.3). */
    for (int i = 0; i < 6; i++) {
        uint8_t k = r->keys[i];
        if (k == 0 || k == 0x01 || k == 0x02 || k == 0x03) continue;
        if (contains(prev_keys, 6, k)) continue;

        /* LAlt + digit row → focus switch.  Use LAlt specifically: RAlt
         * is AltGr and should produce layout symbols (e.g. AltGr+1 = ~
         * on HU) rather than pane-switch. */
        if ((mods & HID_MOD_LALT) && k >= 0x1E && k <= 0x26) {
            int n = (k - 0x1E) + 1;
            vc_focus_by_id(n);
            continue;
        }

        /* Universal keycode = HID usage; pass through unchanged. */
        char c = keymap_translate(k, mods);
        if (c) vc_kbd_push(c);
    }

    /* Snapshot the new keys as our previous-state for the next report. */
    for (int i = 0; i < 6; i++) prev_keys[i] = r->keys[i];
}
