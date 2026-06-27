/* =============================================================================
 * usb_hid.c — USB HID boot-keyboard class driver.
 *
 * Scope: decode the fixed 8-byte boot-protocol keyboard report
 * (HID 1.11 §B.1), diff successive reports for fresh key-down events,
 * translate the USB HID usage IDs to ASCII (with shift modifier
 * handling), and push the characters into the focused VC's ring via
 * vc_kbd_push — the same routing path the PS/2 driver uses.
 *
 * Why we don't parse the device's full report descriptor: in boot mode
 * the device contractually emits this exact 8-byte report regardless
 * of what its full descriptor says.  That's the whole point of the
 * Boot Interface subclass (HID 1.11 §4.3).  We get USB keyboards for
 * "free" without dragging in HID's variable-length item parser.
 *
 * Modifiers: only Shift is handled today (translates lowercase/upper
 * lookup tables, matching the PS/2 driver's behavior).  Alt for VC
 * pane switching is intentionally NOT handled here — we'd need
 * device-side modifier tracking to mirror what the PS/2 driver does,
 * but Alt-N from a USB keyboard works fine because:
 *   - Alt produces a separate HID event with modifier bit set
 *   - The digit produces an event whose keys[] contains the digit
 *   - Our diff logic catches the digit press while modifiers!=0
 * We translate "Alt + digit row" into a `vc_focus_by_id` call exactly
 * like the PS/2 driver.
 * ============================================================================= */

#include "usb.h"
#include "vc.h"
#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * HID usage ID → ASCII tables.
 *
 * USB HID Usage Page 0x07 (Keyboard/Keypad).  Codes from HID 1.11
 * §10 (Keyboard/Keypad Page), constrained to the printable subset we
 * care about (0x04 'a' .. 0x38 '/').  Codes outside the table map to
 * 0 ("no character" — consumed without emitting anything).
 *
 * Special codes:
 *   0x28 = Enter        → '\n'
 *   0x29 = Escape       → 27
 *   0x2A = Backspace    → '\b'
 *   0x2B = Tab          → '\t'
 *   0x2C = Space        → ' '
 * --------------------------------------------------------------------------- */

const char usb_hid_kbd_lower[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = '\n',
    [0x29] = 27,
    [0x2A] = '\b',
    [0x2B] = '\t',
    [0x2C] = ' ',
    [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']',
    [0x31] = '\\',
    [0x33] = ';', [0x34] = '\'',
    [0x35] = '`',
    [0x36] = ',', [0x37] = '.', [0x38] = '/',
};

const char usb_hid_kbd_upper[256] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z',
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x28] = '\n',
    [0x29] = 27,
    [0x2A] = '\b',
    [0x2B] = '\t',
    [0x2C] = ' ',
    [0x2D] = '_', [0x2E] = '+', [0x2F] = '{', [0x30] = '}',
    [0x31] = '|',
    [0x33] = ':', [0x34] = '"',
    [0x35] = '~',
    [0x36] = '<', [0x37] = '>', [0x38] = '?',
};

/* ---------------------------------------------------------------------------
 * Report state — last 6 active keys, to compute new-down deltas.
 * --------------------------------------------------------------------------- */
static uint8_t prev_keys[6];

static int contains(const uint8_t* arr, int n, uint8_t k) {
    for (int i = 0; i < n; i++) if (arr[i] == k) return 1;
    return 0;
}

void usb_hid_kbd_handle_report(const uint8_t* report) {
    const struct hid_boot_kbd_report* r = (const struct hid_boot_kbd_report*)report;
    int shift = (r->modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
    int alt   = (r->modifiers & (HID_MOD_LALT   | HID_MOD_RALT))   != 0;

    /* For every key in the new report not in the old one, it's a fresh
     * press — translate and route.  Skip ErrorRollOver (0x01) and
     * other phantom-key reports (HID 1.11 §B.1.3). */
    for (int i = 0; i < 6; i++) {
        uint8_t k = r->keys[i];
        if (k == 0 || k == 0x01 || k == 0x02 || k == 0x03) continue;
        if (contains(prev_keys, 6, k)) continue;

        /* Alt + digit row (USB HID 0x1E='1' .. 0x26='9') → focus switch. */
        if (alt && k >= 0x1E && k <= 0x26) {
            int n = (k - 0x1E) + 1;
            vc_focus_by_id(n);
            continue;
        }

        char c = shift ? usb_hid_kbd_upper[k] : usb_hid_kbd_lower[k];
        if (c) vc_kbd_push(c);
    }

    /* Snapshot the new keys as our previous-state for the next report. */
    for (int i = 0; i < 6; i++) prev_keys[i] = r->keys[i];
}
