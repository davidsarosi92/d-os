/* =============================================================================
 * gterm.c — the terminal emulator behind a WIN_TERM window (§M70; §M58).
 *
 * Extracted from gui.c, which had grown to 5620 lines holding seven separate
 * subsystems.  This is the most clearly separate of them: it knows about a
 * character grid, a scrollback ring and a selection, and NOTHING about
 * compositing, z-order or focus.  It reaches the rest of the compositor
 * through exactly two calls — `gui_damage_win` and `gui_window_raise`.
 *
 * THE ONE IDEA TO CARRY AWAY: everything here is addressed in ABSOLUTE LINE
 * NUMBERS, not grid rows.  A grid row is a position on the SCREEN, and one
 * line of output renumbers every one of them — so a selection held in grid
 * rows silently slides onto text the user never pointed at.  `gterm_row(abs)`
 * answers "where does that line live now" for the renderer, the hit test and
 * the copy alike, so the three cannot disagree; a line that has aged out of
 * the ring yields NOTHING rather than the wrong text, because copying whatever
 * happens to occupy that slot today is worse than a short copy.
 *
 * THREADING (M22.7): the mouse IRQ only RECORDS a cell range and sets a flag.
 * A grid re-render is thousands of glyph blits and `clipboard_set` allocates,
 * so the work happens in `term_selection_service` on the compositor task.
 * =========================================================================== */

#include "gui_priv.h"
#include "gui.h"
#include "gfx.h"
#include "console_plate.h"
#include "clipboard.h"
#include "printf.h"
#include "kmalloc.h"
#include "config.h"
#include "vc.h"
#include "task.h"
#include <stdint.h>
#include <stddef.h>

/* §M58 — the terminal window currently being selected in, and one that needs a
 * re-render because its selection changed.  Both are set in the mouse IRQ under
 * state_lock and consumed by the compositor: re-rendering a whole terminal grid
 * is far too much work for an interrupt. */
struct gui_window* term_sel_win   = NULL;
struct gui_window* volatile term_sel_dirty = NULL;
struct gui_window* volatile term_sel_copy  = NULL;
struct gui_window* volatile term_paste_win = NULL;
/* §M59 — Ctrl+Shift+C: the selection goes to the EXPLICIT clipboard (the drag
 * alone only fills the primary slot). */
struct gui_window* volatile term_sel_copy_to_clip = NULL;

/* §M58 — is cell (row,col) inside the selection?  The range is LINEAR in
 * reading order, not a rectangle: selecting from the middle of one line to the
 * middle of the next must take the end of the first line and the start of the
 * second, which is what a person means by "from here to there".  A rectangular
 * selection is a different (also useful) feature and would need its own
 * modifier — it is not this one wearing the wrong maths. */
/* The row holding ABSOLUTE line `abs`, or NULL if it has scrolled out of the
 * kept history (or is below the live grid).  ONE lookup for every reader —
 * renderer, selection and copy all go through it, so "where does this line
 * live" is answered in a single place rather than three that can disagree. */
const char* gterm_row(const struct gui_window* win, int abs) {
    int rel = abs - win->scrolled;
    if (rel >= 0)
        return (rel < win->rows && win->cells)
               ? win->cells + (size_t)rel * gmax_cols : NULL;
    int back = -rel;                            /* 1 = most recently evicted */
    if (!win->sb || win->sb_cap <= 0 || back > win->sb_count) return NULL;
    int i = (win->sb_head - back) % win->sb_cap;
    if (i < 0) i += win->sb_cap;
    return win->sb + (size_t)i * gmax_cols;
}
/* Screen row currently showing absolute line `abs`, or -1 if it is off view. */
int gterm_screen_row(const struct gui_window* win, int abs) {
    int v = abs - (win->scrolled - win->view_off);
    return (v >= 0 && v < win->rows) ? v : -1;
}
/* Push the live grid's top row into the ring (called just before a scroll
 * discards it).  Silently a no-op without scrollback, which is what makes
 * `gui.scrollback = 0` a supported configuration rather than a broken one. */
void gterm_sb_push(struct gui_window* win, const char* row) {
    if (!win->sb || win->sb_cap <= 0) return;
    char* d = win->sb + (size_t)win->sb_head * gmax_cols;
    for (int c = 0; c < gmax_cols; c++) d[c] = row[c];
    win->sb_head = (win->sb_head + 1) % win->sb_cap;
    if (win->sb_count < win->sb_cap) win->sb_count++;
}
static int gterm_cell_selected(const struct gui_window* win, int row, int col) {
    if (!win->sel_on) return 0;
    int ar = win->sel_ar, ac = win->sel_ac, br = win->sel_br, bc = win->sel_bc;
    if (br < ar || (br == ar && bc < ac)) {          /* dragged backwards */
        int tr = ar, tc = ac; ar = br; ac = bc; br = tr; bc = tc;
    }
    if (row < ar || row > br) return 0;
    if (row == ar && col < ac) return 0;
    if (row == br && col >= bc) return 0;            /* end is EXCLUSIVE */
    return 1;
}
/* `row` is an ABSOLUTE line number.  A cell whose line is not on screen right
 * now (the user has scrolled back) is not drawn at all — without this check the
 * live shell would keep painting its output over the history being read, which
 * is the one thing scrollback exists to prevent. */
/* §M90 — a cell's rendition (SGR), packed: foreground 0 = default or palette
 * index + 1 (9 bits), background likewise, bold, reverse.  Kept for the LIVE
 * grid only: a line that scrolls into the history keeps its text and is shown
 * in the default colours (said in vt.h's spirit — not pretended). */
#define GA_FG(a)    ((int)((a) & 0x1FF) - 1)
#define GA_BG(a)    ((int)(((a) >> 9) & 0x1FF) - 1)
#define GA_BOLD     (1u << 18)
#define GA_REV      (1u << 19)
#define GA_BGMASK   (0x1FFu << 9)
static uint32_t ga_pack(int fg, int bg, int bold, int rev) {
    return (uint32_t)(fg + 1) | ((uint32_t)(bg + 1) << 9) | (bold ? GA_BOLD : 0) | (rev ? GA_REV : 0);
}
static uint32_t gterm_cell_attr(const struct gui_window* win, int col, int abs) {
    int rel = abs - win->scrolled;
    if (!win->cattr || rel < 0 || rel >= win->rows || col < 0 || col >= gmax_cols) return 0;
    return win->cattr[(size_t)rel * gmax_cols + col];
}
static void gterm_draw_cell(struct gui_window* win, int col, int row, char c) {
    int v = gterm_screen_row(win, row);
    if (v < 0) return;
    int px = PAD + col * cp_cell_w();
    int py = PAD + v * cp_cell_h();
    char s[2] = { c, 0 };
    int sel = gterm_cell_selected(win, row, col);
    uint32_t fg = COL_WIN_FG, bg = COL_WIN_BG;
    uint32_t a = gterm_cell_attr(win, col, row);
    if (a) {
        int f = GA_FG(a), b = GA_BG(a);
        if ((a & GA_BOLD) && f >= 0 && f < 8) f += 8;      /* bold = bright */
        if (f >= 0) fg = 0xFF000000u | vt_rgb(f);
        if (b >= 0) bg = 0xFF000000u | vt_rgb(b);
        if (a & GA_REV) { uint32_t t = fg; fg = bg; bg = t; }
    }
    gfx_fill(&win->surf, px, py, cp_cell_w(), cp_cell_h(), sel ? COL_SEL_BG : bg);
    /* §M69 — the MONO face at the runtime size, not the 8x8 bitmap. */
    if (c > 0x20) cp_mono_text(&win->surf, px, py, s, sel ? COL_SEL_FG : fg);
}
/* Move the attribute rows with the character rows (scroll / line edits). */
static void ga_row_copy(struct gui_window* w, int dst, int src) {
    if (!w->cattr) return;
    uint32_t* d = w->cattr + (size_t)dst * gmax_cols;
    if (src < 0) { for (int c = 0; c < gmax_cols; c++) d[c] = 0; return; }
    const uint32_t* s = w->cattr + (size_t)src * gmax_cols;
    for (int c = 0; c < gmax_cols; c++) d[c] = s[c];
}
static void gterm_region_scroll_locked(struct gui_window* w, int top, int bot, int n);
static void gterm_scroll(struct gui_window* win) {
    /* 2026-10-16 — on the alternate screen a scroll moves the grid and nothing
     * else: no history, no absolute line numbers advance. */
    if (win->alt) { gterm_region_scroll_locked(win, 0, win->rows - 1, 1); return; }
    /* §M58 — the line about to be discarded goes into the history first. */
    gterm_sb_push(win, win->cells);
    win->scrolled++;

    /* A view that is scrolled BACK must not move: the user is reading fixed
     * text while new output arrives underneath it.  view_off is measured from
     * the live bottom, so following the same content means growing it by one —
     * up to the depth actually kept, past which the text really is gone. */
    if (win->view_off > 0) {
        win->view_off++;
        if (win->view_off > win->sb_count) win->view_off = win->sb_count;
        /* The screen is showing history; the pixel scroll below would slide it.
         * Only the MODEL moves here — the compositor re-renders the view. */
        for (int r = 0; r < win->rows - 1; r++) {
            char* d = win->cells + (size_t)r * gmax_cols;
            for (int c = 0; c < gmax_cols; c++) d[c] = d[c + gmax_cols];
            ga_row_copy(win, r, r + 1);
        }
        char* last = win->cells + (size_t)(win->rows - 1) * gmax_cols;
        for (int c = 0; c < gmax_cols; c++) last[c] = 0;
        ga_row_copy(win, win->rows - 1, -1);
        return;
    }

    struct gfx_surface* s = &win->surf;
    int top    = PAD;
    int bottom = PAD + win->rows * cp_cell_h();
    int lift   = cp_cell_h() * s->stride;
    for (int y = top; y < bottom - cp_cell_h(); y++) {
        uint32_t* row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = row[x + lift];
    }
    gfx_fill(s, 0, bottom - cp_cell_h(), s->w, cp_cell_h(), COL_WIN_BG);

    for (int r = 0; r < win->rows - 1; r++) {
        char* d = win->cells + (size_t)r * gmax_cols;
        for (int c = 0; c < gmax_cols; c++) d[c] = d[c + gmax_cols];
        ga_row_copy(win, r, r + 1);
    }
    char* lastrow = win->cells + (size_t)(win->rows - 1) * gmax_cols;
    for (int c = 0; c < gmax_cols; c++) lastrow[c] = 0;
    ga_row_copy(win, win->rows - 1, -1);
}
void gterm_emit(void* ctx, char c) {
    struct gui_window* win = (struct gui_window*)ctx;
    spin_lock(&win->lock);

    if (c == '\f') {
        gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
        for (int i = 0; i < gmax_cols * gmax_rows; i++) win->cells[i] = 0;
        if (win->cattr) for (int i = 0; i < gmax_cols * gmax_rows; i++) win->cattr[i] = 0;
        win->ccol = win->crow = 0;
    } else if (c == '\n') {
        win->ccol = 0;
        if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
    } else if (c == '\r') {
        win->ccol = 0;
    } else if (c == '\b') {
        if (win->ccol > 0) {
            win->ccol--;
            win->cells[(size_t)win->crow * gmax_cols + win->ccol] = 0;
            if (win->cattr) win->cattr[(size_t)win->crow * gmax_cols + win->ccol] = win->cur_attr & GA_BGMASK;
            gterm_draw_cell(win, win->ccol, win->scrolled + win->crow, ' ');
        }
    } else {
        win->cells[(size_t)win->crow * gmax_cols + win->ccol] = c;
        if (win->cattr) win->cattr[(size_t)win->crow * gmax_cols + win->ccol] = win->cur_attr;
        gterm_draw_cell(win, win->ccol, win->scrolled + win->crow, c);
        if (++win->ccol >= win->cols) {
            win->ccol = 0;
            if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
        }
    }

    spin_unlock(&win->lock);
    gui_damage_win(win);
}
/* §M90 rung 6 — the GUI terminal's side of the VT100 subset (vt.h): the
 * cursor, erasing and shifting cells in the backing store, and the answer to
 * a cursor-position query typed back into the window's own input.  Each op
 * takes the window lock as gterm_emit does, and damages what it changed. */
static void gv_put(void* x, char c) { gterm_emit(x, c); }
static void gv_get(void* x, int* r, int* c) {
    struct gui_window* w = x;
    spin_lock(&w->lock); *r = w->crow; *c = w->ccol; spin_unlock(&w->lock);
}
static void gv_set(void* x, int r, int c) {
    struct gui_window* w = x;
    spin_lock(&w->lock);
    w->crow = r < w->rows ? r : w->rows - 1;
    w->ccol = c < w->cols ? c : w->cols - 1;
    spin_unlock(&w->lock);
}
static void gv_dims(void* x, int* r, int* c) {
    struct gui_window* w = x;
    *r = w->rows > 0 ? w->rows : 24;
    *c = w->cols > 0 ? w->cols : 80;
}
static void gv_redraw_row(struct gui_window* w, int row, int c0, int c1) {
    for (int c = c0; c < c1; c++) {
        char ch = w->cells[(size_t)row * gmax_cols + c];
        gterm_draw_cell(w, c, w->scrolled + row, ch ? ch : ' ');
    }
}
static void gv_erase(void* x, int row, int c0, int c1) {
    struct gui_window* w = x;
    spin_lock(&w->lock);
    if (row >= 0 && row < w->rows) {
        if (c1 > w->cols) c1 = w->cols;
        for (int c = c0 < 0 ? 0 : c0; c < c1; c++) {
            w->cells[(size_t)row * gmax_cols + c] = 0;
            /* the erased cells take the CURRENT background (xterm's BCE) */
            if (w->cattr) w->cattr[(size_t)row * gmax_cols + c] = w->cur_attr & GA_BGMASK;
        }
        gv_redraw_row(w, row, c0 < 0 ? 0 : c0, c1);
    }
    spin_unlock(&w->lock);
    gui_damage_win(w);
}
static void gv_shift(void* x, int row, int col, int n) {
    struct gui_window* w = x;
    spin_lock(&w->lock);
    if (row >= 0 && row < w->rows && col >= 0 && col < w->cols && n) {
        char* r = w->cells + (size_t)row * gmax_cols;
        uint32_t* ra = w->cattr ? w->cattr + (size_t)row * gmax_cols : NULL;
        if (n > 0) {                                   /* insert n blanks at col */
            for (int c = w->cols - 1; c >= col; c--) {
                r[c] = (c - n >= col) ? r[c - n] : 0;
                if (ra) ra[c] = (c - n >= col) ? ra[c - n] : 0;
            }
        } else {                                       /* delete -n characters */
            int k = -n;
            for (int c = col; c < w->cols; c++) {
                r[c] = (c + k < w->cols) ? r[c + k] : 0;
                if (ra) ra[c] = (c + k < w->cols) ? ra[c + k] : 0;
            }
        }
        gv_redraw_row(w, row, col, w->cols);
    }
    spin_unlock(&w->lock);
    gui_damage_win(w);
}
static void gv_reply(void* x, const char* s) {
    struct gui_window* w = x;
    for (; *s; s++) vc_kbd_push_to(w->vc, *s);
}
/* §M90 — SGR: the rendition the next output gets. */
static void gv_attr(void* x, int fg, int bg, int bold, int rev) {
    struct gui_window* w = x;
    spin_lock(&w->lock);
    w->cur_attr = (fg == VT_DEFAULT && bg == VT_DEFAULT && !bold && !rev) ? 0 : ga_pack(fg, bg, bold, rev);
    spin_unlock(&w->lock);
}
/* §M90 — insert (n > 0) / delete (n < 0) whole lines at `row` (CSI L / M):
 * the rows below move, the bottom fills blank, all of it redrawn. */
static void gv_lines(void* x, int row, int n) {
    struct gui_window* w = x;
    spin_lock(&w->lock);
    if (row >= 0 && row < w->rows && n) {
        int k = n > 0 ? n : -n;
        if (k > w->rows - row) k = w->rows - row;
        if (n > 0) {
            for (int r = w->rows - 1; r >= row; r--) {
                char* d = w->cells + (size_t)r * gmax_cols;
                if (r - k >= row) {
                    const char* s2 = w->cells + (size_t)(r - k) * gmax_cols;
                    for (int c = 0; c < gmax_cols; c++) d[c] = s2[c];
                    ga_row_copy(w, r, r - k);
                } else {
                    for (int c = 0; c < gmax_cols; c++) d[c] = 0;
                    ga_row_copy(w, r, -1);
                }
            }
        } else {
            for (int r = row; r < w->rows; r++) {
                char* d = w->cells + (size_t)r * gmax_cols;
                if (r + k < w->rows) {
                    const char* s2 = w->cells + (size_t)(r + k) * gmax_cols;
                    for (int c = 0; c < gmax_cols; c++) d[c] = s2[c];
                    ga_row_copy(w, r, r + k);
                } else {
                    for (int c = 0; c < gmax_cols; c++) d[c] = 0;
                    ga_row_copy(w, r, -1);
                }
            }
        }
        for (int r = row; r < w->rows; r++) gv_redraw_row(w, r, 0, w->cols);
    }
    spin_unlock(&w->lock);
    gui_damage_win(w);
}
/* 2026-10-16 — rows top..bot by n (n > 0 up), blanks in at the freed edge,
 * those rows redrawn.  Window lock held. */
static void gterm_region_scroll_locked(struct gui_window* w, int top, int bot, int n) {
    if (top < 0) top = 0;
    if (bot >= w->rows) bot = w->rows - 1;
    if (top > bot || !n) return;
    int k = n > 0 ? n : -n, span = bot - top + 1;
    if (k > span) k = span;
    if (n > 0) {
        for (int r = top; r <= bot; r++) {
            char* d = w->cells + (size_t)r * gmax_cols;
            if (r + k <= bot) {
                const char* s2 = w->cells + (size_t)(r + k) * gmax_cols;
                for (int c = 0; c < gmax_cols; c++) d[c] = s2[c];
                ga_row_copy(w, r, r + k);
            } else { for (int c = 0; c < gmax_cols; c++) d[c] = 0; ga_row_copy(w, r, -1); }
        }
    } else {
        for (int r = bot; r >= top; r--) {
            char* d = w->cells + (size_t)r * gmax_cols;
            if (r - k >= top) {
                const char* s2 = w->cells + (size_t)(r - k) * gmax_cols;
                for (int c = 0; c < gmax_cols; c++) d[c] = s2[c];
                ga_row_copy(w, r, r - k);
            } else { for (int c = 0; c < gmax_cols; c++) d[c] = 0; ga_row_copy(w, r, -1); }
        }
    }
    /* the blank rows take the current background (BCE), as erase does */
    if (w->cattr) {
        int b0 = n > 0 ? bot - k + 1 : top, b1 = n > 0 ? bot : top + k - 1;
        for (int r = b0; r <= b1; r++)
            for (int c = 0; c < gmax_cols; c++) w->cattr[(size_t)r * gmax_cols + c] = w->cur_attr & GA_BGMASK;
    }
    for (int r = top; r <= bot; r++) {
        /* a whole row repainted: fill it first so no stale glyph survives */
        int v = gterm_screen_row(w, w->scrolled + r);
        if (v >= 0) gfx_fill(&w->surf, PAD, PAD + v * cp_cell_h(), w->cols * cp_cell_w(), cp_cell_h(), COL_WIN_BG);
        gv_redraw_row(w, r, 0, w->cols);
    }
}
static void gv_scroll(void* x, int top, int bot, int n) {
    struct gui_window* w = x;
    spin_lock(&w->lock);
    gterm_region_scroll_locked(w, top, bot, n);
    spin_unlock(&w->lock);
    gui_damage_win(w);
}
/* 2026-10-16 — the alternate screen: keep the grid aside (cells and
 * renditions), show a blank one; leaving puts it back and re-renders.  The
 * view snaps to the live screen and any selection is dropped — it was made
 * on the other screen. */
static void gv_alt(void* x, int on) {
    struct gui_window* w = x;
    size_t n = (size_t)gmax_cols * gmax_rows;
    spin_lock(&w->lock);
    if (on && !w->alt) {
        if (!w->alt_cells) w->alt_cells = (char*)kmalloc(n);
        if (w->cattr && !w->alt_cattr) w->alt_cattr = (uint32_t*)kmalloc(n * sizeof(uint32_t));
        if (w->alt_cells && (!w->cattr || w->alt_cattr)) {
            for (size_t i = 0; i < n; i++) { w->alt_cells[i] = w->cells[i]; w->cells[i] = 0; }
            if (w->cattr) for (size_t i = 0; i < n; i++) { w->alt_cattr[i] = w->cattr[i]; w->cattr[i] = 0; }
            w->alt = 1;
            w->view_off = 0;
            w->sel_on = 0;
            gterm_rerender_locked(w);
        }
    } else if (!on && w->alt) {
        for (size_t i = 0; i < n; i++) w->cells[i] = w->alt_cells[i];
        if (w->cattr && w->alt_cattr) for (size_t i = 0; i < n; i++) w->cattr[i] = w->alt_cattr[i];
        kfree(w->alt_cells); w->alt_cells = NULL;
        if (w->alt_cattr) { kfree(w->alt_cattr); w->alt_cattr = NULL; }
        w->alt = 0;
        w->sel_on = 0;
        gterm_rerender_locked(w);
    }
    spin_unlock(&w->lock);
    gui_damage_win(w);
}
const struct vt_ops gterm_vt_ops = {
    .put = gv_put, .get_cursor = gv_get, .set_cursor = gv_set, .dims = gv_dims,
    .erase = gv_erase, .shift = gv_shift, .reply = gv_reply, .attr = gv_attr,
    .lines = gv_lines, .scroll = gv_scroll, .altscreen = gv_alt,
};

/* §M58 — copy the selected cells out of the backing store into `dst`.
 * Trailing blanks on each line are dropped (a terminal pads its rows with
 * spaces, and pasting that padding is never what was meant), and a newline is
 * inserted between rows.  Returns the number of bytes produced. */
int gterm_selection_text(struct gui_window* win, char* dst, int cap) {
    if (!win->sel_on || !win->cells || cap <= 0) return 0;
    int ar = win->sel_ar, ac = win->sel_ac, br = win->sel_br, bc = win->sel_bc;
    if (br < ar || (br == ar && bc < ac)) {
        int tr = ar, tc = ac; ar = br; ac = bc; br = tr; bc = tc;
    }
    int n = 0;
    for (int r = ar; r <= br; r++) {
        /* ABSOLUTE line → wherever it lives now (live grid or history).  A line
         * that has aged out of the ring yields nothing rather than the wrong
         * text: the alternative is silently copying whatever occupies that slot
         * today, which is worse than a short copy. */
        const char* src = gterm_row(win, r);
        if (!src) { if (r != br && n < cap - 1) dst[n++] = '\n'; continue; }
        int c0 = (r == ar) ? ac : 0;
        int c1 = (r == br) ? bc : win->cols;
        if (c1 > win->cols) c1 = win->cols;
        /* Trim the row's trailing blanks/NULs. */
        int end = c1;
        while (end > c0) {
            char ch = src[end - 1];
            if (ch != 0 && ch != ' ') break;
            end--;
        }
        for (int c = c0; c < end && n < cap - 1; c++)
            dst[n++] = src[c] ? src[c] : ' ';
        if (r != br && n < cap - 1) dst[n++] = '\n';
    }
    dst[n] = '\0';
    return n;
}
void term_selection_service(void) {
    struct gui_window* d = term_sel_dirty;
    if (d) {
        term_sel_dirty = NULL;
        if (d->used && d->kind == WIN_TERM && d->cells) {
            spin_lock(&d->lock);
            gterm_rerender_locked(d);
            spin_unlock(&d->lock);
            gui_damage_win(d);
        }
    }

    struct gui_window* p = term_paste_win;
    if (p) {
        term_paste_win = NULL;
        if (p->used && p->kind == WIN_TERM && p->vc) {
            /* Prefer the EXPLICIT clipboard and fall back to the selection:
             * a paste with nothing deliberately copied should still do the
             * obvious thing rather than nothing at all. */
            int use_clip = clipboard_len() > 0;
            int n = use_clip ? clipboard_len() : clipboard_primary_len();
            if (n > 0) {
                char* buf = (char*)kmalloc((size_t)n + 1);
                if (buf) {
                    n = use_clip ? clipboard_get(buf, n + 1)
                                 : clipboard_get_primary(buf, n + 1);
                    /* Focus first: a paste goes to the window that was CLICKED,
                     * and vc_kbd_push feeds the FOCUSED VC — without this the
                     * text would land in whichever terminal happened to have
                     * focus, which is the kind of bug that looks like data
                     * loss. */
                    gui_window_raise(p);
                    for (int i = 0; i < n; i++) {
                        /* A newline in the middle of a pasted selection is a
                         * command SUBMISSION here, exactly as if it had been
                         * typed — that is what pasting into a shell means, and
                         * silently dropping it would make multi-line pastes
                         * concatenate into one wrong command. */
                        vc_kbd_push(buf[i]);
                    }
                    kfree(buf);
                }
            }
        }
    }

    struct gui_window* k = term_sel_copy_to_clip;
    if (k) {
        term_sel_copy_to_clip = NULL;
        if (k->used && k->kind == WIN_TERM && k->cells && k->sel_on) {
            enum { SEL_MAX = 16 * 1024 };
            char* buf = (char*)kmalloc(SEL_MAX);
            if (buf) {
                spin_lock(&k->lock);
                int n = gterm_selection_text(k, buf, SEL_MAX);
                spin_unlock(&k->lock);
                if (n > 0) {
                    clipboard_set(buf, n);
                    kprintf("gui: copied %d byte(s) to the clipboard\n", n);
                }
                kfree(buf);
            }
        } else if (k->used) {
            kprintf("gui: nothing selected - drag across the text first\n");
        }
    }

    struct gui_window* c = term_sel_copy;
    if (c) {
        term_sel_copy = NULL;
        if (c->used && c->kind == WIN_TERM && c->cells && c->sel_on) {
            /* Bounded: a selection is at most the visible grid, and a cap keeps
             * a future scrollback selection from defining the buffer size. */
            enum { SEL_MAX = 16 * 1024 };
            char* buf = (char*)kmalloc(SEL_MAX);
            if (buf) {
                spin_lock(&c->lock);
                int n = gterm_selection_text(c, buf, SEL_MAX);
                spin_unlock(&c->lock);
                if (n > 0) {
                    clipboard_set_primary(buf, n);
                    kprintf("gui: selected %d byte(s) - Ctrl+Shift+C to copy, "
                            "Ctrl+Shift+V or middle-click to paste\n", n);
                }
                kfree(buf);
            }
        }
    }
}
/* Repaint the VIEW: screen row v shows absolute line `base + v`, which may live
 * in the live grid or in the history ring — gterm_row knows which, and nothing
 * here needs to. */
void gterm_rerender_locked(struct gui_window* win) {
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    int base = win->scrolled - win->view_off;
    for (int v = 0; v < win->rows; v++) {
        const char* src = gterm_row(win, base + v);
        if (!src) continue;
        for (int c = 0; c < win->cols; c++)      /* §M90 — a coloured blank is drawn too */
            if (src[c] || gterm_cell_attr(win, c, base + v)) gterm_draw_cell(win, c, base + v, src[c] ? src[c] : ' ');
    }

    /* A scrolled-back view says so, in the corner it cannot be confused with
     * output: a terminal that silently stops showing new text is indisting-
     * uishable from one that has hung. */
    if (win->view_off > 0) {
        char tag[24];
        int n = 0;
        tag[n++] = '['; 
        int v = win->view_off, div = 10000, seen = 0;
        while (div > 0) {
            int d = (v / div) % 10;
            if (d || seen || div == 1) { tag[n++] = (char)('0' + d); seen = 1; }
            div /= 10;
        }
        const char* suffix = " lines back]";
        for (const char* p2 = suffix; *p2 && n < (int)sizeof tag - 1; p2++) tag[n++] = *p2;
        tag[n] = 0;
        int tw = n * cp_cell_w();
        int tx = win->surf.w - PAD - tw, ty = PAD;
        if (tx < 0) tx = 0;
        gfx_fill(&win->surf, tx, ty, tw, cp_cell_h(), COL_SEL_BG);
        gfx_text(&win->surf, tx, ty, tag, COL_SEL_FG);
    }
}
/* Move the view by `dl` lines (positive = back into history) and clamp it.
 * Returns non-zero if the view actually moved — the caller only repaints then,
 * so holding the wheel at the top of the history costs nothing. */
int gterm_view_scroll(struct gui_window* win, int dl) {
    if (!win->cells) return 0;
    int want = win->view_off + dl;
    if (want < 0) want = 0;
    if (want > win->sb_count) want = win->sb_count;
    if (want == win->view_off) return 0;
    win->view_off = want;
    return 1;
}
/* ==========================================================================
 * `termcheck` — the falsification for scrollback (§M58).
 *
 * A screenshot can show that a window LOOKS scrolled; it cannot show that the
 * selection still names the text the user pointed at.  This asks the model
 * instead: it writes numbered lines until they have demonstrably scrolled off,
 * then selects one BY ABSOLUTE LINE NUMBER and prints what the copy path
 * returns.  If the addressing were still grid-relative — the bug this feature
 * exists to remove — the answer would be a line that is currently on screen,
 * and the printed text would say so.
 *
 * Runs inside a GUI terminal window (it needs one to inspect); on the text
 * console it says so rather than pretending.
 * ========================================================================== */
void gui_term_check(void) {
    struct task* self = task_current();
    struct gui_window* win = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].kind == WIN_TERM && windows[i].cells &&
            self && windows[i].vc == self->out_console) { win = &windows[i]; break; }
    if (!win) {
        kprintf("termcheck: not running in a GUI terminal window "
                "(open one from Start > New Shell)\n");
        return;
    }

    kprintf("termcheck: grid %dx%d, scrollback %d/%d lines, view_off %d\n",
            win->cols, win->rows, win->sb_count, win->sb_cap, win->view_off);
    if (win->sb_cap <= 0) {
        kprintf("termcheck: no scrollback configured (gui.scrollback = 0)\n");
        return;
    }

    /* Where the next line will land, recorded BEFORE writing any: the absolute
     * number is the only handle that survives the scrolling we are about to
     * cause. */
    int base = win->scrolled + win->crow;
    int n    = win->rows * 2;                   /* enough to scroll off twice */
    for (int i = 0; i < n; i++) kprintf("SBLINE %d\n", i);

    /* Line 3 was printed long ago and is certainly off screen now. */
    int target = base + 3;
    int onscreen = gterm_screen_row(win, target);
    const char* row = gterm_row(win, target);
    kprintf("termcheck: line abs %d - on screen: %s, in history: %s\n",
            target, onscreen >= 0 ? "yes" : "no", row ? "yes" : "no");

    /* Select that whole line THROUGH THE SAME PATH the mouse uses, and copy. */
    char buf[128];
    int  got = 0;
    spin_lock(&win->lock);
    int save_ar = win->sel_ar, save_ac = win->sel_ac;
    int save_br = win->sel_br, save_bc = win->sel_bc, save_on = win->sel_on;
    win->sel_ar = target; win->sel_ac = 0;
    win->sel_br = target; win->sel_bc = win->cols;
    win->sel_on = 1;
    got = gterm_selection_text(win, buf, (int)sizeof buf);
    win->sel_ar = save_ar; win->sel_ac = save_ac;
    win->sel_br = save_br; win->sel_bc = save_bc; win->sel_on = save_on;
    spin_unlock(&win->lock);

    kprintf("termcheck: copied %d byte(s) from that line: \"%s\"\n", got, buf);
    kprintf("termcheck: expected \"SBLINE 3\" - %s\n",
            (buf[0] == 'S' && buf[1] == 'B' && buf[7] == '3' && got == 8)
            ? "PASS (absolute addressing reaches history)"
            : "FAIL (the selection is not naming the line it was given)");

    /* And the view: scroll back, confirm the offset took, come back. */
    int moved = gterm_view_scroll(win, 10);
    kprintf("termcheck: view scrolled back 10 -> view_off %d (%s)\n",
            win->view_off, moved ? "moved" : "clamped at the top of history");
    gterm_view_scroll(win, -win->view_off);
    term_sel_dirty = win;
    need_frame = 1;
}

/* Pixel (content-relative) → cell, clamped into the grid.  `col` is allowed to
 * reach `cols` so a drag past the end of a line selects the whole line. */
void gterm_cell_at(struct gui_window* win, int cx, int cy,
                          int* row, int* col) {
    int r = (cy - PAD) / cp_cell_h();
    int c = (cx - PAD + cp_cell_w() / 2) / cp_cell_w();
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if (r >= win->rows) r = win->rows - 1;
    if (c > win->cols)  c = win->cols;
    /* ABSOLUTE line, not the screen row: what the caller means by "this text"
     * must keep meaning it after the next line of output arrives. */
    *row = win->scrolled - win->view_off + r;
    *col = c;
}
