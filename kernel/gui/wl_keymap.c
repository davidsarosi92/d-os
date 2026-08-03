/* =============================================================================
 * wl_keymap.c — generate an XKB keymap from d-os's active keyboard layout (§M40).
 *
 * WHY THIS EXISTS
 * ---------------
 * `wl_keyboard.key` carries a raw keycode and nothing else.  A toolkit turns
 * that into a character by feeding it to xkbcommon together with the keymap the
 * COMPOSITOR handed it over `wl_keyboard.keymap` — so without a keymap a client
 * receives keystrokes it cannot interpret at all.  SDL, GTK and Qt all require
 * one before they will report a single character.
 *
 * WHY IT IS GENERATED RATHER THAN EMBEDDED
 * ----------------------------------------
 * d-os already has keyboard layouts (`kernel/core/layouts.c`, us + hu, selected
 * by `keyboard.layout` and shown in the taskbar).  Shipping a fixed xkb file
 * would mean two independent sources of truth that silently disagree the moment
 * someone switches layout.  Deriving the keymap from the live layout means a
 * Wayland client sees exactly the keyboard the rest of the system is using.
 *
 * THE KEYCODE CONVENTION
 * ----------------------
 * On Linux the number in `wl_keyboard.key` is an evdev code and the xkb keycode
 * is that plus 8.  d-os forwards its own scancodes, so rather than build an
 * evdev translation table we emit a keymap whose keycodes ARE our scancodes + 8.
 * That is entirely legitimate — the compositor defines the keymap it hands out,
 * and the two only have to agree with each other — and it keeps one table
 * instead of two.
 *
 * The generated keymap is deliberately self-contained: no `include` directives,
 * because those make xkbcommon look for files on disk and d-os has no xkb data
 * directory to point it at.
 * ============================================================================= */

#include "wayland.h"
#include "keymap.h"
#include "fd.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * ASCII → xkb keysym name.  Letters and digits are their own names; everything
 * else needs the X11 spelling.  A character with no name here is simply left
 * out of the keymap (the key still delivers a keycode, it just has no symbol).
 * ------------------------------------------------------------------------- */
static const char* keysym_name(char ch, char* scratch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9')) {
        scratch[0] = ch; scratch[1] = '\0';
        return scratch;
    }
    switch (ch) {
        case ' ':  return "space";
        case '!':  return "exclam";
        case '"':  return "quotedbl";
        case '#':  return "numbersign";
        case '$':  return "dollar";
        case '%':  return "percent";
        case '&':  return "ampersand";
        case '\'': return "apostrophe";
        case '(':  return "parenleft";
        case ')':  return "parenright";
        case '*':  return "asterisk";
        case '+':  return "plus";
        case ',':  return "comma";
        case '-':  return "minus";
        case '.':  return "period";
        case '/':  return "slash";
        case ':':  return "colon";
        case ';':  return "semicolon";
        case '<':  return "less";
        case '=':  return "equal";
        case '>':  return "greater";
        case '?':  return "question";
        case '@':  return "at";
        case '[':  return "bracketleft";
        case '\\': return "backslash";
        case ']':  return "bracketright";
        case '^':  return "asciicircum";
        case '_':  return "underscore";
        case '`':  return "grave";
        case '{':  return "braceleft";
        case '|':  return "bar";
        case '}':  return "braceright";
        case '~':  return "asciitilde";
        case '\n': return "Return";
        case '\b': return "BackSpace";
        case '\t': return "Tab";
        case 0x1B: return "Escape";
        default:   return NULL;
    }
}

/* Tiny append-with-bounds helper: everything below builds one big text blob. */
struct sbuf { char* p; uint32_t len, cap; };

static void sb_puts(struct sbuf* b, const char* s) {
    while (s && *s && b->len < b->cap - 1) b->p[b->len++] = *s++;
    b->p[b->len] = '\0';
}
static void sb_putu(struct sbuf* b, unsigned v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n && b->len < b->cap - 1) b->p[b->len++] = t[--n];
    b->p[b->len] = '\0';
}

/* Emit "<Kn>" for scancode `kc` (xkb keycode = kc + 8, see the header note). */
static void sb_keyname(struct sbuf* b, unsigned kc) {
    sb_puts(b, "<K"); sb_putu(b, kc + 8); sb_puts(b, ">");
}

/* ---------------------------------------------------------------------------
 * Build the keymap text.  Returns the length written (excluding the NUL that
 * xkbcommon requires to be INCLUDED in the size it is given — see the caller).
 * ------------------------------------------------------------------------- */
static uint32_t build_keymap(char* out, uint32_t cap) {
    struct sbuf b = { out, 0, cap };
    const struct kbd_layout* lay = keymap_active();
    const char* base  = lay ? lay->maps[KBD_COL_BASE]  : NULL;
    const char* shift = lay ? lay->maps[KBD_COL_SHIFT] : NULL;

    sb_puts(&b, "xkb_keymap {\n");

    /* keycodes: name every scancode that produces something. */
    sb_puts(&b, "xkb_keycodes \"d-os\" {\n  minimum = 8;\n  maximum = 264;\n");
    for (unsigned kc = 0; kc < KBD_KEYCODE_MAX; kc++) {
        char c = base ? base[kc] : 0;
        char sc[2];
        if (!c || !keysym_name(c, sc)) continue;
        sb_puts(&b, "  "); sb_keyname(&b, kc);
        sb_puts(&b, " = "); sb_putu(&b, kc + 8); sb_puts(&b, ";\n");
    }
    sb_puts(&b, "};\n");

    /* types: one- and two-level is all an ASCII layout needs. */
    sb_puts(&b,
        "xkb_types \"d-os\" {\n"
        "  virtual_modifiers NumLock;\n"
        "  type \"ONE_LEVEL\" {\n"
        "    modifiers = none;\n"
        "    map[none] = Level1;\n"
        "    level_name[Level1] = \"Any\";\n"
        "  };\n"
        "  type \"TWO_LEVEL\" {\n"
        "    modifiers = Shift;\n"
        "    map[none] = Level1;\n"
        "    map[Shift] = Level2;\n"
        "    level_name[Level1] = \"Base\";\n"
        "    level_name[Level2] = \"Shift\";\n"
        "  };\n"
        "};\n");

    /* compat: the standard "a modifier key sets its modifier" rule. */
    sb_puts(&b,
        "xkb_compatibility \"d-os\" {\n"
        "  interpret Any+AnyOf(all) {\n"
        "    action = SetMods(modifiers = modMapMods, clearLocks);\n"
        "  };\n"
        "};\n");

    /* symbols: base + shift level per key. */
    sb_puts(&b, "xkb_symbols \"d-os\" {\n  name[Group1] = \"d-os\";\n");
    for (unsigned kc = 0; kc < KBD_KEYCODE_MAX; kc++) {
        char cb = base ? base[kc] : 0;
        char s1[2], s2[2];
        const char* nb = cb ? keysym_name(cb, s1) : NULL;
        if (!nb) continue;
        char cs = shift ? shift[kc] : 0;
        const char* ns = cs ? keysym_name(cs, s2) : NULL;
        sb_puts(&b, "  key "); sb_keyname(&b, kc);
        if (ns) {
            sb_puts(&b, " { type = \"TWO_LEVEL\", [ ");
            sb_puts(&b, nb); sb_puts(&b, ", "); sb_puts(&b, ns);
            sb_puts(&b, " ] };\n");
        } else {
            sb_puts(&b, " { type = \"ONE_LEVEL\", [ ");
            sb_puts(&b, nb); sb_puts(&b, " ] };\n");
        }
    }
    sb_puts(&b, "};\n};\n");
    return b.len;
}

/* ---------------------------------------------------------------------------
 * Produce the keymap as a shared-memory object the client can mmap.
 *
 * `wl_keyboard.keymap` passes a FILE DESCRIPTOR, not bytes — the client maps it
 * read-only and hands the text to xkbcommon.  d-os has no files to pass, but it
 * does have memfds, and those are exactly a descriptor over anonymous memory.
 *
 * The size reported to the client INCLUDES the terminating NUL: xkbcommon's
 * from_string path expects the mapping to be NUL-terminated, and a keymap that
 * is one byte short of that is rejected with a parse error at the last line.
 * Returns the ofile (caller owns the reference) or NULL, with *size_out set.
 * ------------------------------------------------------------------------- */
struct ofile* wl_keymap_make(uint32_t* size_out) {
    const uint32_t CAP = 16384;
    struct shm* s = shm_create(CAP);
    if (!s) return NULL;

    /* The frames are contiguous only within one page, so build into the first
     * frame and copy across page boundaries as we go. */
    static char text[16384];
    uint32_t len = build_keymap(text, sizeof text);
    uint32_t total = len + 1;                    /* include the NUL */

    for (uint32_t off = 0; off < total; off++) {
        uint32_t fi = off / 4096, fo = off % 4096;
        if ((int)fi >= s->nframes) break;
        *(volatile uint8_t*)(uintptr_t)(s->frames[fi] + fo) = (uint8_t)text[off];
    }

    struct ofile* o = ofile_from_shm(s);
    shm_unref(s);                                /* the ofile owns it now */
    if (!o) return NULL;
    if (size_out) *size_out = total;
    kprintf("wayland: xkb keymap generated from layout '%s' (%u bytes)\n",
            keymap_current(), total);
    return o;
}
