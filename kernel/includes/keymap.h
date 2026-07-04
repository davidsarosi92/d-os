/* =============================================================================
 * keymap.h — keyboard-layout abstraction shared by all input drivers.
 *
 * Two-stage translation, mirroring the shape Linux uses for HID:
 *
 *   1. Hardware → universal keycode + modifier-bitmask.
 *      Each input driver (ps2_keyboard, usb_hid) does this locally:
 *      PS/2 maps scancodes via its set-1 table; USB takes the HID
 *      Usage IDs straight out of the report.
 *
 *   2. (Keycode, modifiers) → ASCII character via the active
 *      `struct kbd_layout`.  Layouts ship as static tables in
 *      `kernel/core/layouts.c`; the active layout is selected at boot
 *      from `keyboard.layout` (config) and can be switched at runtime
 *      via `keymap_select(name)`.
 *
 * The universal keycode is, by design, the USB HID Usage ID (Page 0x07,
 * Keyboard/Keypad).  Picking that as the wire format means the USB
 * driver does zero translation work, and the PS/2 driver only has to
 * carry one small "scancode → HID usage" table.  Future driver classes
 * (serial console with escape sequences, virtual KB over RPC, …) just
 * need to produce the same keycode/modifier pair.
 *
 * Modifier bitmask layout mirrors the HID boot-keyboard report so
 * usb_hid.c can pass its `report->modifiers` byte through unchanged:
 *
 *   bit 0 = LCtrl    bit 4 = RCtrl
 *   bit 1 = LShift   bit 5 = RShift
 *   bit 2 = LAlt     bit 6 = RAlt   (= AltGr for international layouts)
 *   bit 3 = LGUI     bit 7 = RGUI
 *
 * Only Shift and RAlt (AltGr) influence the layout lookup — Ctrl/Alt/
 * GUI combinations are policy decisions left to the input driver
 * (e.g. PS/2 driver intercepts Alt+digit for `vc_focus_by_id` and
 * doesn't even call keymap_translate in that case).
 * ============================================================================= */

#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

/* Selected USB HID usage IDs (HID 1.11 §10).  We name only the ones a
 * call site might want to test for; the rest are used as raw integers.
 * Listed in usage-ID order so a glance at the file is enough to remind
 * yourself which keycode is which. */
#define KC_A             0x04
#define KC_C             0x06
#define KC_N             0x11
#define KC_O             0x12
#define KC_S             0x16
#define KC_V             0x19
#define KC_X             0x1B
#define KC_Z             0x1D
#define KC_1             0x1E
#define KC_9             0x26
#define KC_0             0x27
#define KC_ENTER         0x28
#define KC_ESC           0x29
#define KC_BACKSPACE     0x2A
#define KC_TAB           0x2B
#define KC_SPACE         0x2C
/* Navigation / editing cluster (M22.5 — flows to widgets as raw
 * keycode events; layouts map none of these to a character). */
#define KC_INSERT        0x49
#define KC_HOME          0x4A
#define KC_PGUP          0x4B
#define KC_DELETE        0x4C
#define KC_END           0x4D
#define KC_PGDN          0x4E
#define KC_RIGHT         0x4F
#define KC_LEFT          0x50
#define KC_DOWN          0x51
#define KC_UP            0x52
#define KC_NONUS_BSLASH  0x64    /* the 102-key "extra" key between LSHIFT and Z */

/* Modifier bitmask bits — match HID layout for cheap pass-through. */
#define KBD_MOD_LCTRL    0x01
#define KBD_MOD_LSHIFT   0x02
#define KBD_MOD_LALT     0x04
#define KBD_MOD_LGUI     0x08
#define KBD_MOD_RCTRL    0x10
#define KBD_MOD_RSHIFT   0x20
#define KBD_MOD_RALT     0x40    /* = AltGr for HU/DE/FR/etc layouts */
#define KBD_MOD_RGUI     0x80

#define KBD_MOD_SHIFT_MASK (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)
#define KBD_MOD_CTRL_MASK  (KBD_MOD_LCTRL  | KBD_MOD_RCTRL)
#define KBD_MOD_ALT_MASK   (KBD_MOD_LALT   | KBD_MOD_RALT)

/* Layout columns — 4 per layout (none, shift, altgr, shift+altgr). */
#define KBD_COL_BASE         0
#define KBD_COL_SHIFT        1
#define KBD_COL_ALTGR        2
#define KBD_COL_SHIFT_ALTGR  3
#define KBD_COL_COUNT        4

#define KBD_KEYCODE_MAX      256

struct kbd_layout {
    const char* name;                       /* "us", "hu", ... */
    /* 4 × 256 char arrays, indexed by (column, keycode).  Each cell:
     * the ASCII character to emit, or 0 if the keycode is unmapped in
     * this combo.  Control characters (\n, \b, \t, ESC) sit at fixed
     * keycodes (0x28/0x2A/0x2B/0x29) and every layout uses the same
     * value for them. */
    const char* maps[KBD_COL_COUNT];
};

/* ---------------------------------------------------------------------------
 * Public API.
 * --------------------------------------------------------------------------- */

/* Install a layout in the registry.  Layouts call this from their own
 * static initializer (no MODULE/DRIVER framework — the function table
 * holds them and `keymap_init` walks builtin layouts directly). */
void keymap_register(const struct kbd_layout* layout);

/* Initialize the registry with the built-in layouts (US, HU) and
 * activate the one named in `keyboard.layout` (default: "us"). */
void keymap_init(void);

/* Switch active layout by name.  Returns 0 on success, -1 if no such
 * layout was registered. */
int  keymap_select(const char* name);

/* Active layout's short name, e.g. "us".  Never NULL after keymap_init. */
const char* keymap_current(void);

/* Translate (keycode, modifier_mask) into an ASCII character.  Returns
 * 0 for unmapped combinations.  Input drivers call this on every fresh
 * key-down to produce the character to push into vc_kbd_push. */
char keymap_translate(uint8_t keycode, uint8_t modifiers);

/* Iterate registered layouts — used by `lslayout` shell command. */
typedef void (*keymap_iter_fn)(const struct kbd_layout*, void* ctx);
void keymap_for_each(keymap_iter_fn fn, void* ctx);

#endif
