/* =============================================================================
 * charset.c — ISO-8859-2 <-> UTF-8 (§M59, 2026-09-27).
 *
 * d-os text is ISO-8859-2 bytes end to end: the font is indexed by byte, the
 * keymap emits Latin-2 codes, and the clipboard stores what was typed or
 * selected (§4.66 explains why Latin-2 and not Latin-1: ő and ű do not exist
 * in Latin-1).  The rest of the world — every Wayland client, every musl
 * program — speaks UTF-8.  The two meet at the clipboard bridge, which offers
 * `text/plain;charset=utf-8` and must therefore mean it: handing a toolkit
 * Latin-2 bytes under a UTF-8 label produces invalid sequences that it will
 * either reject or render as replacement characters.
 *
 * WHAT DOES NOT MAP is replaced by '?', never dropped: a paste that silently
 * loses characters looks like a shorter paste, and one that shows a '?' says
 * exactly where the loss was.
 * ============================================================================= */

#include "charset.h"
#include <stdint.h>

/* ISO-8859-2 0xA0..0xFF → Unicode.  (0x80..0x9F are the C1 controls, which
 * map to themselves.)  From the standard table. */
static const uint16_t l2_hi[96] = {
    0x00A0,0x0104,0x02D8,0x0141,0x00A4,0x013D,0x015A,0x00A7,0x00A8,0x0160,0x015E,0x0164,0x0179,0x00AD,0x017D,0x017B,
    0x00B0,0x0105,0x02DB,0x0142,0x00B4,0x013E,0x015B,0x02C7,0x00B8,0x0161,0x015F,0x0165,0x017A,0x02DD,0x017E,0x017C,
    0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
    0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
    0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
    0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9,
};

static uint32_t l2_to_ucs(uint8_t b) {
    return b < 0xA0 ? b : l2_hi[b - 0xA0];
}

static int ucs_to_l2(uint32_t u) {
    if (u < 0xA0) return (int)u;
    for (int i = 0; i < 96; i++) if (l2_hi[i] == u) return 0xA0 + i;
    return -1;
}

int charset_latin2_to_utf8(const char* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t u = l2_to_ucs((uint8_t)in[i]);
        if (u < 0x80) {
            if (o + 1 > cap) break;
            out[o++] = (char)u;
        } else if (u < 0x800) {
            if (o + 2 > cap) break;
            out[o++] = (char)(0xC0 | (u >> 6));
            out[o++] = (char)(0x80 | (u & 0x3F));
        } else {
            if (o + 3 > cap) break;
            out[o++] = (char)(0xE0 | (u >> 12));
            out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (u & 0x3F));
        }
    }
    return o;
}

/* Is this well-formed UTF-8 carrying at least one multi-byte character?  A
 * Latin-2 byte string with accents is almost never valid UTF-8 (an accented
 * letter is a lone byte >= 0x80, which UTF-8 forbids outside a sequence), so
 * this tells the two apart well enough to decide whether to convert. */
int charset_is_utf8_text(const char* s, int n) {
    int multi = 0;
    for (int i = 0; i < n; ) {
        uint8_t b = (uint8_t)s[i];
        int len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 :
                  (b & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n) return 0;
        for (int k = 1; k < len; k++)
            if (((uint8_t)s[i + k] & 0xC0) != 0x80) return 0;
        if (len > 1) multi = 1;
        i += len;
    }
    return multi;
}

int charset_utf8_to_latin2(const char* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap; ) {
        uint8_t b = (uint8_t)in[i];
        uint32_t u; int len;
        if (b < 0x80)                { u = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0) { u = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { u = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { u = b & 0x07; len = 4; }
        else { out[o++] = '?'; i++; continue; }          /* stray continuation */
        /* Truncated sequence: mark it and go on with the NEXT byte — the first
         * version stopped here, and a stray lead byte near the end took every
         * character after it with it. */
        if (i + len > n) { out[o++] = '?'; i++; continue; }
        int ok = 1;
        for (int k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = 0; break; }
            u = (u << 6) | (c & 0x3F);
        }
        if (!ok) { out[o++] = '?'; i++; continue; }
        int l2 = ucs_to_l2(u);
        out[o++] = (char)(l2 < 0 ? '?' : l2);
        i += len;
    }
    return o;
}
