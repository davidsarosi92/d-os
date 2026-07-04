/* =============================================================================
 * w_editor.c — multiline text editor widget (M22.5, PLAN §M22.5 stage 2).
 *
 * The biggest widget in the toolkit, so it gets its own file.  Model:
 *
 *   - One contiguous kmalloc'd byte buffer (grow-by-doubling, kept
 *     NUL-terminated).  Insert/delete are memmove-style shifts —
 *     O(len), which is fine at the file sizes a teaching kernel edits
 *     (a gap buffer is the classic upgrade if this ever feels slow).
 *   - Lines are IMPLICIT ('\n' separated) — all line math scans the
 *     buffer.  Again O(len) per keystroke, again fine at our sizes.
 *   - Cursor + optional selection anchor are byte offsets.  Shift +
 *     movement extends the selection, unshifted movement drops it
 *     (the standard editing convention).
 *   - Viewport (scroll_line/scroll_col) follows the cursor; there is
 *     no line wrapping — long lines scroll horizontally.
 *
 * Keyboard: printable chars / '\n' / '\b' arrive via the char path
 * (widget_ops.key); arrows, Home/End, PgUp/PgDn, Delete and the
 * Ctrl shortcuts arrive via the M22.5 raw-keycode path
 * (widget_ops.keycode).  Ctrl+C/X/V talk to the kernel clipboard;
 * Ctrl+A selects all; other Ctrl+letters are forwarded to the app
 * through on_shortcut (the editor app binds Ctrl+S = save).
 *
 * Everything runs on the compositor task — no locking needed beyond
 * what gui.c already does around widget dispatch.
 * ============================================================================= */

#include "widget.h"
#include "gui.h"
#include "gfx.h"
#include "keymap.h"
#include "clipboard.h"
#include "kmalloc.h"
#include <stddef.h>

/* Palette — matches widget.c's boxes. */
#define ED_BG        0xFF0B1220u
#define ED_EDGE      0xFF3A4A5Eu
#define ED_FOCUS     0xFF3D7BD8u
#define ED_TEXT      0xFFE0E0E0u
#define ED_SEL_BG    0xFF2C5B9Eu
#define ED_CARET     0xFFF2F5FAu

#define ED_PAD_X 5
#define ED_PAD_Y 2

/* -------------------------------------------------------------------------- */
/* Buffer primitives.                                                          */
/* -------------------------------------------------------------------------- */

static int ed_grow(struct w_editor* e, int need) {
    if (need + 1 <= e->cap) return 0;           /* +1 for the NUL */
    int ncap = e->cap ? e->cap : 256;
    while (ncap < need + 1) ncap *= 2;
    char* nb = (char*)kmalloc((size_t)ncap);
    if (!nb) return -1;
    for (int i = 0; i < e->len; i++) nb[i] = e->buf[i];
    if (e->buf) kfree(e->buf);
    e->buf = nb;
    e->cap = ncap;
    return 0;
}

static int ed_insert(struct w_editor* e, int off, const char* s, int n) {
    if (n <= 0) return 0;
    if (ed_grow(e, e->len + n) != 0) return -1;
    for (int i = e->len - 1; i >= off; i--) e->buf[i + n] = e->buf[i];
    for (int i = 0; i < n; i++) e->buf[off + i] = s[i];
    e->len += n;
    e->buf[e->len] = 0;
    e->modified = 1;
    return 0;
}

static void ed_delete(struct w_editor* e, int a, int b) {  /* [a,b) */
    if (a < 0) a = 0;
    if (b > e->len) b = e->len;
    if (a >= b) return;
    for (int i = b; i < e->len; i++) e->buf[i - (b - a)] = e->buf[i];
    e->len -= b - a;
    e->buf[e->len] = 0;
    e->modified = 1;
}

/* -------------------------------------------------------------------------- */
/* Line math (all O(len) scans — see header comment).                          */
/* -------------------------------------------------------------------------- */

static int ed_line_start(const struct w_editor* e, int off) {
    while (off > 0 && e->buf[off - 1] != '\n') off--;
    return off;
}

static int ed_line_end(const struct w_editor* e, int off) {
    while (off < e->len && e->buf[off] != '\n') off++;
    return off;
}

static int ed_line_of(const struct w_editor* e, int off) {
    int line = 0;
    for (int i = 0; i < off; i++) if (e->buf[i] == '\n') line++;
    return line;
}

static int ed_offset_of_line(const struct w_editor* e, int line) {
    int off = 0;
    while (line > 0 && off < e->len) {
        if (e->buf[off] == '\n') line--;
        off++;
    }
    return off;
}

static int ed_line_count(const struct w_editor* e) {
    return ed_line_of(e, e->len) + 1;
}

/* -------------------------------------------------------------------------- */
/* Selection + viewport helpers.                                               */
/* -------------------------------------------------------------------------- */

static int ed_sel_min(const struct w_editor* e) {
    return e->anchor < e->cursor ? e->anchor : e->cursor;
}
static int ed_sel_max(const struct w_editor* e) {
    return e->anchor > e->cursor ? e->anchor : e->cursor;
}
static int ed_has_sel(const struct w_editor* e) {
    return e->anchor >= 0 && e->anchor != e->cursor;
}

/* Delete the selection (if any) and land the cursor at its start. */
static void ed_kill_sel(struct w_editor* e) {
    if (!ed_has_sel(e)) { e->anchor = -1; return; }
    int a = ed_sel_min(e), b = ed_sel_max(e);
    ed_delete(e, a, b);
    e->cursor = a;
    e->anchor = -1;
}

static int ed_vis_rows(const struct w_editor* e) {
    int r = (e->base.h - 2 * ED_PAD_Y) / WED_ROW_H;
    return r > 0 ? r : 1;
}

static int ed_vis_cols(const struct w_editor* e) {
    int c = (e->base.w - 2 * ED_PAD_X) / GFX_GLYPH_W;
    return c > 0 ? c : 1;
}

static void ed_ensure_visible(struct w_editor* e) {
    int line = ed_line_of(e, e->cursor);
    int col  = e->cursor - ed_line_start(e, e->cursor);
    int rows = ed_vis_rows(e), cols = ed_vis_cols(e);

    if (line < e->scroll_line)            e->scroll_line = line;
    if (line >= e->scroll_line + rows)    e->scroll_line = line - rows + 1;
    if (col  < e->scroll_col)             e->scroll_col  = col;
    if (col  >= e->scroll_col + cols)     e->scroll_col  = col - cols + 1;
    if (e->scroll_line < 0) e->scroll_line = 0;
    if (e->scroll_col  < 0) e->scroll_col  = 0;
}

/* Cursor motion helper: handles the shift-extends-selection rule. */
static void ed_move_to(struct w_editor* e, int off, int shift) {
    if (off < 0)      off = 0;
    if (off > e->len) off = e->len;
    if (shift) {
        if (e->anchor < 0) e->anchor = e->cursor;
    } else {
        e->anchor = -1;
    }
    e->cursor = off;
    ed_ensure_visible(e);
}

/* Vertical motion keeps the "preferred column" so cursoring through a
 * short line doesn't lose the horizontal position. */
static void ed_move_lines(struct w_editor* e, int delta, int shift) {
    int line = ed_line_of(e, e->cursor);
    int col  = e->cursor - ed_line_start(e, e->cursor);
    if (e->pref_col < 0) e->pref_col = col;

    int nline = line + delta;
    int lcount = ed_line_count(e);
    if (nline < 0) nline = 0;
    if (nline >= lcount) nline = lcount - 1;

    int ls = ed_offset_of_line(e, nline);
    int le = ed_line_end(e, ls);
    int ncol = e->pref_col;
    if (ncol > le - ls) ncol = le - ls;
    ed_move_to(e, ls + ncol, shift);
}

/* -------------------------------------------------------------------------- */
/* Drawing.                                                                    */
/* -------------------------------------------------------------------------- */

static void editor_draw(struct widget* w, struct gfx_surface* s) {
    struct w_editor* e = (struct w_editor*)w;
    int focused = gui_widget_focused(w);

    gfx_fill(s, w->x, w->y, w->w, w->h, ED_BG);
    gfx_fill(s, w->x, w->y, w->w, 1, focused ? ED_FOCUS : ED_EDGE);
    gfx_fill(s, w->x, w->y + w->h - 1, w->w, 1, focused ? ED_FOCUS : ED_EDGE);
    gfx_fill(s, w->x, w->y, 1, w->h, focused ? ED_FOCUS : ED_EDGE);
    gfx_fill(s, w->x + w->w - 1, w->y, 1, w->h, focused ? ED_FOCUS : ED_EDGE);

    int rows = ed_vis_rows(e), cols = ed_vis_cols(e);
    int selmin = ed_has_sel(e) ? ed_sel_min(e) : -1;
    int selmax = ed_has_sel(e) ? ed_sel_max(e) : -1;

    int off = ed_offset_of_line(e, e->scroll_line);
    for (int r = 0; r < rows && off <= e->len; r++) {
        int le = ed_line_end(e, off);
        int py = w->y + ED_PAD_Y + r * WED_ROW_H;

        for (int c = 0; c < cols; c++) {
            int o = off + e->scroll_col + c;
            if (o > le) break;
            int px = w->x + ED_PAD_X + c * GFX_GLYPH_W;

            /* Selection highlight covers the newline cell too (one
             * trailing cell), matching how editors show selected EOLs. */
            if (o >= selmin && o < selmax)
                gfx_fill(s, px, py, GFX_GLYPH_W, WED_ROW_H, ED_SEL_BG);

            if (o < le) {
                char ch = e->buf[o];
                if (ch > 0x20) {
                    char str[2] = { ch, 0 };
                    gfx_text(s, px, py + 1, str, ED_TEXT);
                }
            }

            /* Caret. */
            if (focused && o == e->cursor)
                gfx_fill(s, px, py, 1, WED_ROW_H, ED_CARET);
        }

        if (le >= e->len) break;                /* last line rendered */
        off = le + 1;                           /* skip the '\n' */
    }
}

/* -------------------------------------------------------------------------- */
/* Input.                                                                      */
/* -------------------------------------------------------------------------- */

static void editor_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)kind;
    struct w_editor* e = (struct w_editor*)w;
    gui_window_focus_widget(w->win, w);

    int line = e->scroll_line + (ly - ED_PAD_Y) / WED_ROW_H;
    int col  = e->scroll_col  + (lx - ED_PAD_X + GFX_GLYPH_W / 2) / GFX_GLYPH_W;
    if (line < 0) line = 0;
    if (col  < 0) col  = 0;
    int lcount = ed_line_count(e);
    if (line >= lcount) line = lcount - 1;

    int ls = ed_offset_of_line(e, line);
    int le = ed_line_end(e, ls);
    if (col > le - ls) col = le - ls;
    e->pref_col = -1;
    ed_move_to(e, ls + col, 0);
}

static void editor_key(struct widget* w, char c) {
    struct w_editor* e = (struct w_editor*)w;
    e->pref_col = -1;

    if (c == '\b') {
        if (ed_has_sel(e)) {
            ed_kill_sel(e);
        } else if (e->cursor > 0) {
            ed_delete(e, e->cursor - 1, e->cursor);
            e->cursor--;
        }
        ed_ensure_visible(e);
        return;
    }
    if (c == '\t') {                            /* tab types as 4 spaces */
        ed_kill_sel(e);
        if (ed_insert(e, e->cursor, "    ", 4) == 0) e->cursor += 4;
        ed_ensure_visible(e);
        return;
    }
    if (c == '\n' || (c >= 0x20 && c <= 0x7E)) {
        ed_kill_sel(e);
        if (ed_insert(e, e->cursor, &c, 1) == 0) e->cursor++;
        ed_ensure_visible(e);
    }
}

static void editor_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    struct w_editor* e = (struct w_editor*)w;
    int shift = (mods & KBD_MOD_SHIFT_MASK) != 0;
    int ctrl  = (mods & KBD_MOD_CTRL_MASK)  != 0;

    if (ctrl) {
        switch (kc) {
        case KC_A:                              /* select all */
            e->anchor = 0;
            e->cursor = e->len;
            ed_ensure_visible(e);
            return;
        case KC_C:
        case KC_X: {
            int a, b;
            if (ed_has_sel(e)) {
                a = ed_sel_min(e); b = ed_sel_max(e);
            } else {                            /* no selection: whole line */
                a = ed_line_start(e, e->cursor);
                b = ed_line_end(e, e->cursor);
                if (b < e->len) b++;            /* include the newline */
            }
            clipboard_set(e->buf + a, b - a);
            if (kc == KC_X) {
                ed_delete(e, a, b);
                e->cursor = a;
                e->anchor = -1;
                ed_ensure_visible(e);
            }
            return;
        }
        case KC_V: {
            int n = clipboard_len();
            if (n <= 0) return;
            char* tmp = (char*)kmalloc((size_t)n + 1);
            if (!tmp) return;
            n = clipboard_get(tmp, n + 1);
            ed_kill_sel(e);
            if (ed_insert(e, e->cursor, tmp, n) == 0) e->cursor += n;
            kfree(tmp);
            ed_ensure_visible(e);
            return;
        }
        default:
            if (e->on_shortcut) e->on_shortcut(e, kc, w->ctx);
            return;
        }
    }

    switch (kc) {
    case KC_LEFT:
        e->pref_col = -1;
        /* Collapsing a selection with a bare arrow lands on its edge. */
        if (!shift && ed_has_sel(e)) ed_move_to(e, ed_sel_min(e), 0);
        else                         ed_move_to(e, e->cursor - 1, shift);
        break;
    case KC_RIGHT:
        e->pref_col = -1;
        if (!shift && ed_has_sel(e)) ed_move_to(e, ed_sel_max(e), 0);
        else                         ed_move_to(e, e->cursor + 1, shift);
        break;
    case KC_UP:    ed_move_lines(e, -1, shift); break;
    case KC_DOWN:  ed_move_lines(e, +1, shift); break;
    case KC_PGUP:  ed_move_lines(e, -ed_vis_rows(e), shift); break;
    case KC_PGDN:  ed_move_lines(e, +ed_vis_rows(e), shift); break;
    case KC_HOME:
        e->pref_col = -1;
        ed_move_to(e, ed_line_start(e, e->cursor), shift);
        break;
    case KC_END:
        e->pref_col = -1;
        ed_move_to(e, ed_line_end(e, e->cursor), shift);
        break;
    case KC_DELETE:
        e->pref_col = -1;
        if (ed_has_sel(e))            ed_kill_sel(e);
        else if (e->cursor < e->len)  ed_delete(e, e->cursor, e->cursor + 1);
        ed_ensure_visible(e);
        break;
    default:
        break;
    }
}

static void editor_destroy(struct widget* w) {
    struct w_editor* e = (struct w_editor*)w;
    if (e->buf) { kfree(e->buf); e->buf = NULL; }
}

static const struct widget_ops editor_ops =
    { editor_draw, editor_mouse, editor_key, editor_keycode, editor_destroy };

/* -------------------------------------------------------------------------- */
/* Public API.                                                                 */
/* -------------------------------------------------------------------------- */

struct w_editor* w_editor_create(struct gui_window* win, int x, int y,
                                 int w, int h, void* ctx) {
    struct w_editor* e = (struct w_editor*)kcalloc(1, sizeof(*e));
    if (!e) return NULL;

    e->base.x = x; e->base.y = y; e->base.w = w; e->base.h = h;
    e->base.ops = &editor_ops;
    e->base.win = win;
    e->base.ctx = ctx;
    e->base.focusable = 1;
    e->anchor  = -1;
    e->pref_col = -1;

    if (ed_grow(e, 0) != 0) { kfree(e); return NULL; }
    e->buf[0] = 0;

    gui_window_add_widget(win, &e->base);
    return e;
}

int w_editor_set_text(struct w_editor* e, const char* text, int len) {
    if (!e) return -1;
    if (len < 0) { len = 0; if (text) while (text[len]) len++; }
    if (ed_grow(e, len) != 0) return -1;
    for (int i = 0; i < len; i++) e->buf[i] = text[i];
    e->len = len;
    e->buf[len] = 0;
    e->cursor = 0;
    e->anchor = -1;
    e->scroll_line = e->scroll_col = 0;
    e->pref_col = -1;
    e->modified = 0;
    return 0;
}

const char* w_editor_text(struct w_editor* e, int* out_len) {
    if (!e) return NULL;
    if (out_len) *out_len = e->len;
    return e->buf;
}
