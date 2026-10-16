/* vt.h — the VT100/ANSI subset a terminal on this machine understands
 * (§M90 rung 6, kernel/core/vt.c).
 *
 * WHY.  The console became a terminal (tty.c), and programs that find one
 * behave like terminal programs: a shell edits its line with cursor movement
 * and "erase to end of line", asks where the cursor is (ESC [ 6 n) to learn
 * the width, `docker pull` draws its progress bars with "cursor up" and
 * "erase line".  Without a parser each of those prints as "[1A[2K" garbage.
 *
 * ONE parser, two renderers: the framebuffer panes (vc.c) and the GUI
 * terminal windows (gterm.c) supply the callbacks; the parser owns only the
 * escape-sequence state machine and the arithmetic.  Coordinates are 0-based
 * cells.
 *
 * UNDERSTOOD: CSI A B C D E F G H f d J K P @ X s u m n (5 and 6) c r h l
 * (private modes accepted and ignored; L and M — insert/delete lines — are
 * consumed, not performed), ESC 7 / 8 / c / D / E / M, charset
 * designations (consumed), OSC strings (consumed to BEL or ST), BEL, TAB
 * (to the next multiple of 8).  SGR (m) — §M90, 2026-10-15 — IS rendered:
 * the 16 colours, the bright ones (90-97, 100-107), the 256-colour palette
 * (38;5;n / 48;5;n), direct colour (38;2;r;g;b, kept as the nearest of the
 * 256), bold (shown as the bright variant of a basic colour), reverse, and
 * the defaults (39, 49, 0).  Underline/italic/blink are accepted and not
 * drawn.  Insert/delete lines (L, M) are performed by the GUI terminal (a
 * framebuffer pane keeps no cell store and consumes them).  Scroll regions (r)
 * are accepted and ignored: scrolling is always the whole screen.  An
 * alternate screen (?1049) is not kept; a full-screen program draws over the
 * scrollback. */
#ifndef VT_H
#define VT_H

struct vt_ops {
    void (*put)(void* ctx, char c);                 /* printable, \n \r \b, \f       */
    void (*get_cursor)(void* ctx, int* row, int* col);
    void (*set_cursor)(void* ctx, int row, int col); /* already clamped              */
    void (*dims)(void* ctx, int* rows, int* cols);
    /* erase cells [col0, col1) of `row` (to spaces) */
    void (*erase)(void* ctx, int row, int col0, int col1);
    /* shift the rest of `row` from `col`: n > 0 inserts n blanks, n < 0
     * deletes -n characters (the tail fills with blanks) */
    void (*shift)(void* ctx, int row, int col, int n);
    void (*reply)(void* ctx, const char* s);        /* bytes into the input (DSR)    */
    /* §M90 — APPENDED.  The current rendition changed (SGR): colours are
     * VT_DEFAULT or a 256-colour palette index (vt_rgb turns one into RGB). */
    void (*attr)(void* ctx, int fg, int bg, int bold, int reverse);
    /* §M90 — APPENDED.  Insert (n > 0) or delete (n < 0) |n| whole lines at
     * `row`; the lines below move down/up, the screen bottom fills blank. */
    void (*lines)(void* ctx, int row, int n);
};
#define VT_DEFAULT (-1)
/* xterm's 256-colour palette as 0xRRGGBB. */
unsigned vt_rgb(int idx);

struct vt {
    int state;
    int params[8], np, priv;
    int saved_row, saved_col;
    int fg, bg, bold, reverse;          /* §M90 — the current rendition (SGR) */
};

void vt_init(struct vt* v);
/* One output byte through the parser. */
void vt_feed(struct vt* v, char c, const struct vt_ops* ops, void* ctx);

#endif
