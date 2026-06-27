/* =============================================================================
 * keymap.c — keyboard-layout registry + (keycode, mods) → ASCII translator.
 *
 * See keymap.h for the design rationale.  This file holds the runtime
 * state (the registered layout array + the active-layout pointer) and
 * the two-line translation lookup.
 *
 * Storage: a small static array of `struct kbd_layout*`.  Layouts are
 * compile-time-static (`layouts.c` defines them) so we never alloc.
 *
 * Concurrency: the active-layout pointer is updated only from the
 * shell-task (via `keymap_select` from `setconf` / `lslayout`); IRQ
 * handlers read it via `keymap_translate`.  On x86, a pointer-sized
 * write is atomic and the rare race ("you switched layouts mid-keystroke
 * and the resulting char is from the new layout") is harmless.
 * ============================================================================= */

#include "keymap.h"
#include "config.h"
#include "printf.h"
#include <stddef.h>

#define KBD_MAX_LAYOUTS 8

static const struct kbd_layout* layouts[KBD_MAX_LAYOUTS];
static int                       layouts_n = 0;
static const struct kbd_layout*  active    = NULL;

/* Forward declarations of the built-in layouts (defined in layouts.c). */
extern const struct kbd_layout kbd_layout_us;
extern const struct kbd_layout kbd_layout_hu;

/* String equality without libc. */
static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void keymap_register(const struct kbd_layout* layout) {
    if (!layout || !layout->name) return;
    if (layouts_n >= KBD_MAX_LAYOUTS) {
        kprintf("keymap: layout table full, dropping '%s'\n", layout->name);
        return;
    }
    /* Idempotent: don't register the same layout twice. */
    for (int i = 0; i < layouts_n; i++) {
        if (layouts[i] == layout) return;
    }
    layouts[layouts_n++] = layout;
}

void keymap_init(void) {
    keymap_register(&kbd_layout_us);
    keymap_register(&kbd_layout_hu);

    /* Activate whichever layout `keyboard.layout` selects; fall back to
     * "us" if the configured layout is unknown.  config_init has
     * already run by the time we get here (kernel.c boot order). */
    const char* want = config_get("keyboard.layout", "us");
    if (keymap_select(want) != 0) {
        kprintf("keymap: unknown layout '%s', falling back to 'us'\n", want);
        keymap_select("us");
    }
    kprintf("keymap: active layout '%s' (%d available)\n",
            keymap_current(), layouts_n);
}

int keymap_select(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < layouts_n; i++) {
        if (streq(layouts[i]->name, name)) {
            active = layouts[i];
            return 0;
        }
    }
    return -1;
}

const char* keymap_current(void) {
    return active ? active->name : "?";
}

char keymap_translate(uint8_t keycode, uint8_t mods) {
    if (!active) return 0;
    int col = 0;
    if (mods & KBD_MOD_SHIFT_MASK) col |= KBD_COL_SHIFT;
    if (mods & KBD_MOD_RALT)       col |= KBD_COL_ALTGR;
    const char* map = active->maps[col];
    if (!map) {
        /* Layout omitted this column — fall through to BASE so e.g. an
         * AltGr keypress on a layout without an AltGr map still emits
         * the base character. */
        map = active->maps[KBD_COL_BASE];
    }
    return map ? map[keycode] : 0;
}

void keymap_for_each(keymap_iter_fn fn, void* ctx) {
    if (!fn) return;
    for (int i = 0; i < layouts_n; i++) fn(layouts[i], ctx);
}
