/* =============================================================================
 * layouts.c — built-in keyboard layouts (US, HU).
 *
 * Each layout is 4 × 256-entry char arrays indexed by USB HID Usage ID
 * (Page 0x07).  Cells outside the populated range are 0 ("no char").
 *
 * Special codes shared by every layout (independent of national
 * variants) — these always map to the same control character:
 *
 *   0x28 Enter      → '\n'
 *   0x29 Escape     → 27
 *   0x2A Backspace  → '\b'
 *   0x2B Tab        → '\t'
 *   0x2C Space      → ' '
 *
 * National layouts diverge from here mostly in:
 *   - Z ↔ Y on QWERTZ keyboards (HU, DE).
 *   - The number row's shift faces (HU: 1='  2="  3=+  4=!  ...).
 *   - Punctuation positions (HU: 0=ö, etc — but most ASCII fallbacks).
 *   - AltGr-accessed symbols (HU: AltGr+v=@, AltGr+q=\, AltGr+ě= ...).
 *
 * About the magyar layout below: it's the **ISO 102-key Hungarian
 * QWERTZ** mapping AS FAR AS 7-bit ASCII can express.  The Hungarian
 * accented vowels (á, é, í, ó, ú, ö, ő, ü, ű) need an extended font
 * (CP437 magyar / ISO-8859-2 / UTF-8) which the 8×8 ASCII glyph table
 * we ship doesn't carry.  We map those positions to 0 for now so the
 * layout doesn't lie about what it produces; a follow-up milestone
 * can replace the font and fill these cells in.  The non-accented
 * positions (digits, punctuation, AltGr symbols available in ASCII)
 * are populated.
 *
 * Reference: the wiki-page picture of the Hungarian 102-key layout
 * (search "Magyar billentyűzetkiosztás 102"), HID 1.11 §10 for the
 * usage-ID-to-physical-key mapping.
 * ============================================================================= */

#include "keymap.h"
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * US layout — straight copy of what ps2_keyboard.c and usb_hid.c used to
 * hardcode.  Single source of truth from M16 onward.
 * --------------------------------------------------------------------------- */

static const char us_base[KBD_KEYCODE_MAX] = {
    [KC_A]   = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09]   = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E]   = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13]   = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18]   = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [KC_Z]   = 'z',
    [KC_1]   = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23]   = '6', [0x24] = '7', [0x25] = '8', [KC_9] = '9', [KC_0] = '0',
    [KC_ENTER]     = '\n',
    [KC_ESC]       = 27,
    [KC_BACKSPACE] = '\b',
    [KC_TAB]       = '\t',
    [KC_SPACE]     = ' ',
    [0x2D] = '-',  [0x2E] = '=',  [0x2F] = '[',  [0x30] = ']',
    [0x31] = '\\',
    [0x33] = ';',  [0x34] = '\'',
    [0x35] = '`',
    [0x36] = ',',  [0x37] = '.',  [0x38] = '/',
};

static const char us_shift[KBD_KEYCODE_MAX] = {
    [KC_A]   = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09]   = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E]   = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13]   = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18]   = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [KC_Z]   = 'Z',
    [KC_1]   = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23]   = '^', [0x24] = '&', [0x25] = '*', [KC_9] = '(', [KC_0] = ')',
    [KC_ENTER]     = '\n',
    [KC_ESC]       = 27,
    [KC_BACKSPACE] = '\b',
    [KC_TAB]       = '\t',
    [KC_SPACE]     = ' ',
    [0x2D] = '_',  [0x2E] = '+',  [0x2F] = '{',  [0x30] = '}',
    [0x31] = '|',
    [0x33] = ':',  [0x34] = '"',
    [0x35] = '~',
    [0x36] = '<',  [0x37] = '>',  [0x38] = '?',
};

const struct kbd_layout kbd_layout_us = {
    .name = "us",
    .maps = { us_base, us_shift, NULL, NULL },
};

/* ---------------------------------------------------------------------------
 * HU layout — Magyar 102-key QWERTZ, 7-bit ASCII subset.
 *
 * Letters: identical to US except Z ↔ Y swap.  Accented vowels (á, é,
 * í, ó, ú, ö, ő, ü, ű) are LEFT AS 0 — see file header for why.
 *
 * Number row (BASE / SHIFT):
 *   1  '/'/ — base   "  → "" character (we emit '\"' since ASCII only)
 *   2  '"'/@ — base   '
 *   3  '+'/#
 *   4  '!'/$
 *   5  '%'/%
 *   6  '/'/&
 *   7  '='/*
 *   8  '('/(
 *   9  ')'/)
 *   0  '='/' — actually Hungarian: 0 BASE = ö (accented); ASCII fallback 0
 *
 * On the US-style table this means: at HU base the visible chars are
 * what a Magyar user expects when typing without AltGr, MINUS the
 * accented vowels.  AltGr maps the most common ASCII-only AltGr
 * symbols (\, |, @, $, {, }, [, ], &, etc.).
 *
 * This is the minimum viable layout — good enough to test the
 * abstraction (typing `z` produces 'z' under "us", produces 'y' under
 * "hu") and to ship.  Replacing the font and filling in the accented
 * cells is a clean follow-up.
 * --------------------------------------------------------------------------- */

static const char hu_base[KBD_KEYCODE_MAX] = {
    /* Letters: US a..z, but Z ↔ Y. */
    [KC_A] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'z',                  /* HID 0x1C ("Y" on US) → 'z' on HU */
    [KC_Z] = 'y',                  /* HID 0x1D ("Z" on US) → 'y' on HU */
    /* Number row: HU base — accented vowels omitted, ASCII rest filled. */
    [KC_1] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [KC_9] = '9', [KC_0] = '0',
    [KC_ENTER]     = '\n',
    [KC_ESC]       = 27,
    [KC_BACKSPACE] = '\b',
    [KC_TAB]       = '\t',
    [KC_SPACE]     = ' ',
    /* Punctuation row — base layer; accented-vowel keys (0x2F=ő, 0x30=ú,
     * 0x33=é, 0x34=á, 0x35=0, 0x2D=ü, 0x2E=ó) left as 0. */
    [0x36] = ',', [0x37] = '.', [0x38] = '-',     /* on HU, /? key is - and _ */
    [KC_NONUS_BSLASH] = '<',                       /* 102nd key */
};

static const char hu_shift[KBD_KEYCODE_MAX] = {
    [KC_A] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X',
    [0x1C] = 'Z', [KC_Z] = 'Y',
    /* Shifted number row — Magyar conventional. */
    [KC_1] = '\'', [0x1F] = '"', [0x20] = '+', [0x21] = '!', [0x22] = '%',
    [0x23] = '/',  [0x24] = '=', [0x25] = '(', [KC_9] = ')', [KC_0] = '=',
    [KC_ENTER]     = '\n',
    [KC_ESC]       = 27,
    [KC_BACKSPACE] = '\b',
    [KC_TAB]       = '\t',
    [KC_SPACE]     = ' ',
    [0x36] = '?', [0x37] = ':', [0x38] = '_',
    [KC_NONUS_BSLASH] = '>',
};

/* AltGr column — the symbols that need RAlt on a Hungarian keyboard
 * and that DO fit in 7-bit ASCII.  Brackets, braces, backslash, etc. */
static const char hu_altgr[KBD_KEYCODE_MAX] = {
    [0x14] = '\\',     /* AltGr+Q = \ */
    [0x1A] = '|',      /* AltGr+W = | */
    [0x1F] = '~',      /* AltGr+2 = ~ */
    [0x20] = '^',      /* AltGr+3 = ^ */
    [0x21] = '`',      /* AltGr+4 = ` */
    [0x22] = '\'',     /* AltGr+5 = ' (sometimes; mirrors Linux us-altgr) */
    [0x24] = '`',      /* AltGr+7 = ` */
    [0x16] = '$',      /* AltGr+S = $ (en-US AltGr position) */
    [0x09] = '[',      /* AltGr+F = [ */
    [0x0A] = ']',      /* AltGr+G = ] */
    [0x0B] = '{',      /* AltGr+H = { */
    [0x0C] = '}',      /* AltGr+I = } - actually I; in HU it's the {  key */
    [0x19] = '@',      /* AltGr+V = @ */
    [0x1E] = '~',      /* AltGr+1 = ~ */
    /* This list is intentionally short — we cover the symbols a user
     * is likely to need from a Hungarian layout that fit 7-bit ASCII.
     * Magyar typing real-world relies on the accented vowels too, but
     * those need a wider font (see file header). */
};

const struct kbd_layout kbd_layout_hu = {
    .name = "hu",
    .maps = { hu_base, hu_shift, hu_altgr, NULL },
};
