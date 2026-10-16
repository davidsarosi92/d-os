/* =============================================================================
 * vt.c — the VT100/ANSI subset (§M90 rung 6).  What and why: vt.h.
 *
 * A byte at a time, because that is how output arrives (console_putchar):
 *   GROUND   printable and C0 controls go to the renderer; ESC starts a
 *            sequence
 *   ESC      the byte after ESC: '[' a CSI, ']' an OSC, '(' ')' a charset
 *            designation, or a two-byte command (7 8 c D E M)
 *   CSI      parameters (digits, ';', a leading '?' or '>'), then the final
 *            byte runs the command
 *   OSC      everything to BEL or ESC \ is consumed (window titles)
 *   SKIP1    one byte consumed (the charset name)
 * A malformed sequence never prints its bytes: it ends at the first byte that
 * cannot belong to it, and that byte is dropped — half an escape on screen is
 * the failure this exists to prevent.
 * ============================================================================= */

#include "vt.h"

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_SKIP1 };

void vt_init(struct vt* v) {
    v->state = ST_GROUND;
    v->np = 0; v->priv = 0;
    v->saved_row = v->saved_col = 0;
    for (int i = 0; i < 8; i++) v->params[i] = 0;
    v->fg = v->bg = VT_DEFAULT;
    v->bold = v->reverse = 0;
    v->top = 0; v->bot = -1;
    v->alt = 0; v->alt_row = v->alt_col = 0;
}

/* ---- the scroll region (2026-10-16) ------------------------------------------
 * `rg` resolves it against the screen as it is NOW: bot < 0, or a region that
 * no longer fits after a resize, means the whole screen. */
static void rg(const struct vt* v, int rows, int* t, int* b) {
    int tt = v->top, bb = v->bot;
    if (bb < 0 || bb >= rows || tt < 0 || tt >= bb) { tt = 0; bb = rows - 1; }
    *t = tt; *b = bb;
}
static int region_partial(const struct vt* v, int rows) {
    int t, b; rg(v, rows, &t, &b);
    return t != 0 || b != rows - 1;
}
/* Scroll the region by n (n > 0 up).  A renderer without the scroll op can
 * still do a whole-screen-to-the-bottom region with its line ops. */
static void do_scroll(const struct vt_ops* o, void* x, int t, int b, int n, int rows) {
    if (!n) return;
    if (o->scroll) { o->scroll(x, t, b, n); return; }
    if (o->lines && b == rows - 1) o->lines(x, t, -n);
}
/* A line feed's movement (ESC D, LF): at the region's bottom the region
 * scrolls; on the screen's last row outside the region nothing moves (VT100);
 * otherwise one row down.  `lf` = it came as '\n', which the renderers treat
 * as CR+LF (their contract), so the full-screen case still goes to put(). */
static void index_down(struct vt* v, const struct vt_ops* o, void* x, int lf) {
    int rows = 24, cols = 80, r = 0, c = 0;
    if (o->dims) o->dims(x, &rows, &cols);
    if (o->get_cursor) o->get_cursor(x, &r, &c);
    if (!region_partial(v, rows) || !o->set_cursor) {
        /* The whole screen: the renderer's own line feed, which is where the
         * scrollback is fed.  ESC D keeps the column (a '\n' returns it). */
        o->put(x, '\n');
        if (!lf && o->set_cursor) {
            int r2 = 0, c2 = 0;
            if (o->get_cursor) o->get_cursor(x, &r2, &c2);
            o->set_cursor(x, r2, c);
        }
        return;
    }
    int t, b; rg(v, rows, &t, &b);
    if (r == b) do_scroll(o, x, t, b, 1, rows);
    else if (r < rows - 1) r++;
    o->set_cursor(x, r, lf ? 0 : c);
}
/* ESC M: at the region's top the region scrolls DOWN (how `less` steps back
 * a line); elsewhere the cursor moves up, stopping at row 0. */
static void index_up(struct vt* v, const struct vt_ops* o, void* x) {
    int rows = 24, cols = 80, r = 0, c = 0;
    if (o->dims) o->dims(x, &rows, &cols);
    if (o->get_cursor) o->get_cursor(x, &r, &c);
    int t, b; rg(v, rows, &t, &b);
    if (r == t) do_scroll(o, x, t, b, -1, rows);
    else if (r > 0 && o->set_cursor) o->set_cursor(x, r - 1, c);
}
/* ?1049 / ?1047 / ?47 / ?1048 (h = set, l = reset). */
static void private_mode(struct vt* v, int set, const struct vt_ops* o, void* x) {
    int rows = 24, cols = 80, r = 0, c = 0;
    if (o->dims) o->dims(x, &rows, &cols);
    if (o->get_cursor) o->get_cursor(x, &r, &c);
    for (int i = 0; i < (v->np ? v->np : 1); i++) {
        int p = v->np ? v->params[i] : 0;
        if (p == 1048 || p == 1049) {
            if (set) { v->alt_row = r; v->alt_col = c; }
        }
        if (p == 1049 || p == 1047 || p == 47) {
            if (set && !v->alt) { v->alt = 1; if (o->altscreen) o->altscreen(x, 1); }
            else if (!set && v->alt) { v->alt = 0; if (o->altscreen) o->altscreen(x, 0); }
        }
        if ((p == 1048 || p == 1049) && !set && o->set_cursor) {
            int rr = v->alt_row < rows ? v->alt_row : rows - 1;
            int cc = v->alt_col < cols ? v->alt_col : cols - 1;
            o->set_cursor(x, rr, cc);
        }
        /* everything else (?25 cursor visibility, ?1 cursor keys, ?2004
         * bracketed paste, ...) is accepted and ignored */
    }
}

/* §M90 — xterm's palette: 0-7 the basic colours, 8-15 their bright forms,
 * 16-231 a 6x6x6 cube, 232-255 a grey ramp. */
unsigned vt_rgb(int idx) {
    static const unsigned base[16] = {
        0x000000, 0xCD0000, 0x00CD00, 0xCDCD00, 0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
        0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00, 0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF };
    if (idx < 0) return 0;
    if (idx < 16) return base[idx];
    if (idx < 232) {
        int i = idx - 16, r = i / 36, g = (i / 6) % 6, b = i % 6;
        int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return ((unsigned)lv[r] << 16) | ((unsigned)lv[g] << 8) | (unsigned)lv[b];
    }
    if (idx < 256) { unsigned g = 8 + (unsigned)(idx - 232) * 10; return (g << 16) | (g << 8) | g; }
    return 0;
}
/* The palette index nearest to an RGB colour (the 6x6x6 cube, or a grey). */
static int rgb_to_idx(int r, int g, int b) {
    int cr = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40;
    int cg = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40;
    int cb = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;
    if (cr > 5) cr = 5;
    if (cg > 5) cg = 5;
    if (cb > 5) cb = 5;
    if (cr == cg && cg == cb && r == g && g == b) {
        if (r < 8) return 16;
        if (r > 238) return 231;
        return 232 + (r - 8) / 10;
    }
    return 16 + 36 * cr + 6 * cg + cb;
}
static void sgr(struct vt* v, const struct vt_ops* o, void* x) {
    int n = v->np ? v->np : 1;
    for (int i = 0; i < n; i++) {
        int p = v->np ? v->params[i] : 0;
        if (p < 0) p = 0;
        if (p == 0)       { v->fg = v->bg = VT_DEFAULT; v->bold = v->reverse = 0; }
        else if (p == 1)  v->bold = 1;
        else if (p == 22) v->bold = 0;
        else if (p == 7)  v->reverse = 1;
        else if (p == 27) v->reverse = 0;
        else if (p >= 30 && p <= 37) v->fg = p - 30;
        else if (p >= 40 && p <= 47) v->bg = p - 40;
        else if (p >= 90 && p <= 97) v->fg = p - 90 + 8;
        else if (p >= 100 && p <= 107) v->bg = p - 100 + 8;
        else if (p == 39) v->fg = VT_DEFAULT;
        else if (p == 49) v->bg = VT_DEFAULT;
        else if (p == 38 || p == 48) {
            int c = -2;
            if (i + 2 < n && v->params[i + 1] == 5) { c = v->params[i + 2] & 255; i += 2; }
            else if (i + 4 < n && v->params[i + 1] == 2) {
                c = rgb_to_idx(v->params[i + 2] & 255, v->params[i + 3] & 255, v->params[i + 4] & 255);
                i += 4;
            }
            if (c >= 0) { if (p == 38) v->fg = c; else v->bg = c; }
        }
        /* 2-9 other renditions (dim, italic, underline, blink...): accepted, not drawn */
    }
    if (o->attr) o->attr(x, v->fg, v->bg, v->bold, v->reverse);
}

static int arg(const struct vt* v, int i, int def) {
    return (i < v->np && v->params[i] > 0) ? v->params[i] : def;
}
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

static void put_dec(char* b, int* n, int v) {
    char t[12]; int k = 0;
    if (v < 0) v = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[(*n)++] = t[--k];
}

static void csi(struct vt* v, char f, const struct vt_ops* o, void* x) {
    int rows = 24, cols = 80, r = 0, c = 0;
    if (o->dims) o->dims(x, &rows, &cols);
    if (o->get_cursor) o->get_cursor(x, &r, &c);
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    int n = arg(v, 0, 1);
    switch (f) {
    case 'A': r -= n; break;
    case 'B': case 'e': r += n; break;
    case 'C': case 'a': c += n; break;
    case 'D': c -= n; break;
    case 'E': r += n; c = 0; break;
    case 'F': r -= n; c = 0; break;
    case 'G': case '`': c = n - 1; break;
    case 'd': r = n - 1; break;
    case 'H': case 'f': r = arg(v, 0, 1) - 1; c = arg(v, 1, 1) - 1; break;
    case 'J': {
        int m = v->np ? v->params[0] : 0;
        if (o->erase) {
            if (m == 0) { o->erase(x, r, c, cols); for (int i = r + 1; i < rows; i++) o->erase(x, i, 0, cols); }
            else if (m == 1) { for (int i = 0; i < r; i++) o->erase(x, i, 0, cols); o->erase(x, r, 0, c + 1); }
            else for (int i = 0; i < rows; i++) o->erase(x, i, 0, cols);
        }
        return;
    }
    case 'K': {
        int m = v->np ? v->params[0] : 0;
        if (o->erase) {
            if (m == 0) o->erase(x, r, c, cols);
            else if (m == 1) o->erase(x, r, 0, c + 1);
            else o->erase(x, r, 0, cols);
        }
        return;
    }
    case 'X': if (o->erase) o->erase(x, r, c, clampi(c + n, c, cols)); return;
    case 'P': if (o->shift) o->shift(x, r, c, -n); return;
    case '@': if (o->shift) o->shift(x, r, c, n); return;
    case 's': v->saved_row = r; v->saved_col = c; return;
    case 'u': r = v->saved_row; c = v->saved_col; break;
    case 'n':
        if (v->priv) return;
        if (o->reply) {
            char b[32]; int k = 0;
            if (arg(v, 0, 0) == 6) {                 /* where is the cursor? */
                b[k++] = 0x1b; b[k++] = '[';
                put_dec(b, &k, r + 1); b[k++] = ';'; put_dec(b, &k, c + 1);
                b[k++] = 'R'; b[k] = 0;
                o->reply(x, b);
            } else if (arg(v, 0, 0) == 5) {          /* are you all right? */
                o->reply(x, "\x1b[0n");
            }
        }
        return;
    case 'c':                                        /* who are you: a VT102 */
        if (o->reply && !v->priv) o->reply(x, "\x1b[?6c");
        return;
    case 'm':                                        /* §M90 — select graphic rendition */
        if (!v->priv) sgr(v, o, x);
        return;
    case 'L': case 'M': {                            /* insert / delete lines, IN the region */
        if (v->priv) return;
        int t, b; rg(v, rows, &t, &b);
        if (r < t || r > b) return;                  /* outside the region: nothing */
        if (n > b - r + 1) n = b - r + 1;
        if (o->scroll) o->scroll(x, r, b, f == 'L' ? -n : n);
        else if (o->lines && b == rows - 1) o->lines(x, r, f == 'L' ? n : -n);
        if (o->set_cursor) o->set_cursor(x, r, 0);
        return;
    }
    case 'S': case 'T': {                            /* scroll the region up / down */
        if (v->priv) return;
        int t, b; rg(v, rows, &t, &b);
        do_scroll(o, x, t, b, f == 'S' ? n : -n, rows);
        return;
    }
    case 'r': {                                      /* DECSTBM: the scroll region */
        if (v->priv) return;
        int t = arg(v, 0, 1) - 1, b = arg(v, 1, rows) - 1;
        if (b >= rows) b = rows - 1;
        if (t < b) { v->top = t; v->bot = (t == 0 && b == rows - 1) ? -1 : b; }
        r = 0; c = 0;                                /* the cursor goes home */
        break;
    }
    case 'h': case 'l':
        if (v->priv) private_mode(v, f == 'h', o, x);
        return;                                      /* ANSI modes (IRM, ...): consumed */
    default:
        return;                                      /* t ... consumed */
    }
    if (o->set_cursor) o->set_cursor(x, clampi(r, 0, rows - 1), clampi(c, 0, cols - 1));
}

void vt_feed(struct vt* v, char ch, const struct vt_ops* o, void* x) {
    unsigned char c = (unsigned char)ch;
    switch (v->state) {
    case ST_GROUND:
        if (c == 0x1b) { v->state = ST_ESC; return; }
        if (c == 0x07) return;                       /* BEL */
        if (c == '\t') {                             /* to the next tab stop */
            int rows, cols, r, col;
            if (o->dims && o->get_cursor && o->set_cursor) {
                o->dims(x, &rows, &cols);
                o->get_cursor(x, &r, &col);
                o->set_cursor(x, r, clampi((col / 8 + 1) * 8, 0, cols - 1));
                return;
            }
        }
        if (c == 0x0e || c == 0x0f || c == 0) return;   /* SO / SI / NUL */
        /* LF and VT are line feeds.  FF is NOT treated as one: '\f' is the
         * renderers' "clear the screen" (vc_clear, `clear`), so it goes to
         * put() as it always has. */
        if (c == '\n' || c == 0x0b) { index_down(v, o, x, c == '\n'); return; }
        o->put(x, (char)c);
        return;
    case ST_ESC:
        v->state = ST_GROUND;
        if (c == '[') { v->state = ST_CSI; v->np = 0; v->priv = 0; for (int i = 0; i < 8; i++) v->params[i] = 0; return; }
        if (c == ']') { v->state = ST_OSC; return; }
        if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') { v->state = ST_SKIP1; return; }
        {
            int r = 0, col = 0, rows = 24, cols = 80;
            if (o->get_cursor) o->get_cursor(x, &r, &col);
            if (o->dims) o->dims(x, &rows, &cols);
            if (c == '7') { v->saved_row = r; v->saved_col = col; }
            else if (c == '8') { if (o->set_cursor) o->set_cursor(x, clampi(v->saved_row, 0, rows - 1), clampi(v->saved_col, 0, cols - 1)); }
            else if (c == 'D') index_down(v, o, x, 0);           /* index */
            else if (c == 'E') { o->put(x, '\r'); index_down(v, o, x, 0); }
            else if (c == 'M') index_up(v, o, x);                  /* reverse index */
            else if (c == 'c') {                                   /* reset: clear */
                if (v->alt && o->altscreen) o->altscreen(x, 0);
                o->put(x, '\f'); vt_init(v);
                if (o->attr) o->attr(x, VT_DEFAULT, VT_DEFAULT, 0, 0);
            }
        }
        return;
    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (v->np == 0) v->np = 1;
            int* p = &v->params[v->np - 1];
            if (*p < 100000) *p = *p * 10 + (c - '0');
            return;
        }
        if (c == ';') { if (v->np == 0) v->np = 1; if (v->np < 8) v->np++; return; }
        if (c == '?' || c == '>' || c == '=' || c == '!') { v->priv = 1; return; }
        if (c >= 0x20 && c <= 0x2f) return;          /* intermediates: ignored */
        v->state = ST_GROUND;
        if (c >= 0x40 && c <= 0x7e) csi(v, (char)c, o, x);
        return;                                      /* anything else ends it, dropped */
    case ST_OSC:
        if (c == 0x07) v->state = ST_GROUND;
        else if (c == 0x1b) v->state = ST_OSC_ESC;
        return;
    case ST_OSC_ESC:
        v->state = (c == '\\') ? ST_GROUND : ST_OSC;
        return;
    case ST_SKIP1:
        v->state = ST_GROUND;
        return;
    }
}

/* ---- `vttest` — the parser against a 5x10 screen it can be checked on ------------------ */
#include "shellcmd.h"
#include "printf.h"
#include "console.h"

#define VR 5
#define VC 10
struct vscr { char cell[VR][VC]; int r, c; char reply[32]; int nreply; int fg, bg, bold, rev, nattr;
               char kept[VR][VC]; int alt; };
static void vs_put(void* x, char ch) {
    struct vscr* s = x;
    if (ch == '\n') { s->c = 0; if (s->r < VR - 1) s->r++; return; }
    if (ch == '\r') { s->c = 0; return; }
    if (ch == '\b') { if (s->c) s->c--; return; }
    if (ch == '\f') { for (int i = 0; i < VR; i++) for (int j = 0; j < VC; j++) s->cell[i][j] = '.'; s->r = s->c = 0; return; }
    if ((unsigned char)ch < 0x20) return;
    s->cell[s->r][s->c] = ch;
    if (s->c < VC - 1) s->c++;
}
static void vs_get(void* x, int* r, int* c) { struct vscr* s = x; *r = s->r; *c = s->c; }
static void vs_set(void* x, int r, int c) { struct vscr* s = x; s->r = r; s->c = c; }
static void vs_dims(void* x, int* r, int* c) { (void)x; *r = VR; *c = VC; }
static void vs_erase(void* x, int r, int c0, int c1) {
    struct vscr* s = x;
    for (int c = c0; c < c1 && c < VC; c++) s->cell[r][c] = '.';
}
static void vs_shift(void* x, int r, int col, int n) {
    struct vscr* s = x;
    if (n > 0) for (int c = VC - 1; c >= col; c--) s->cell[r][c] = (c - n >= col) ? s->cell[r][c - n] : '.';
    else for (int c = col; c < VC; c++) s->cell[r][c] = (c - n < VC) ? s->cell[r][c - n] : '.';
}
static void vs_reply(void* x, const char* str) {
    struct vscr* s = x;
    while (*str && s->nreply < 31) s->reply[s->nreply++] = *str++;
    s->reply[s->nreply] = 0;
}
static void vs_attr(void* x, int fg, int bg, int bold, int rev) {
    struct vscr* s = x;
    s->fg = fg; s->bg = bg; s->bold = bold; s->rev = rev; s->nattr++;
}
static void vs_lines(void* x, int row, int n) {
    struct vscr* s = x;
    if (n > 0) { for (int r = VR - 1; r >= row; r--) for (int c = 0; c < VC; c++)
                     s->cell[r][c] = (r - n >= row) ? s->cell[r - n][c] : '.'; }
    else       { int k = -n; for (int r = row; r < VR; r++) for (int c = 0; c < VC; c++)
                     s->cell[r][c] = (r + k < VR) ? s->cell[r + k][c] : '.'; }
}
/* rows top..bot by n: n > 0 up, n < 0 down (the vt.h contract) */
static void vs_scroll(void* x, int top, int bot, int n) {
    struct vscr* s = x;
    if (n > 0) { for (int r = top; r <= bot; r++) for (int c = 0; c < VC; c++)
                     s->cell[r][c] = (r + n <= bot) ? s->cell[r + n][c] : '.'; }
    else       { int k = -n; for (int r = bot; r >= top; r--) for (int c = 0; c < VC; c++)
                     s->cell[r][c] = (r - k >= top) ? s->cell[r - k][c] : '.'; }
}
static void vs_alt(void* x, int on) {
    struct vscr* s = x;
    if (on) { for (int r = 0; r < VR; r++) for (int c = 0; c < VC; c++) { s->kept[r][c] = s->cell[r][c]; s->cell[r][c] = '.'; } }
    else    { for (int r = 0; r < VR; r++) for (int c = 0; c < VC; c++) s->cell[r][c] = s->kept[r][c]; }
    s->alt = on;
}
static const struct vt_ops vs_ops = { vs_put, vs_get, vs_set, vs_dims, vs_erase, vs_shift, vs_reply,
                                      vs_attr, vs_lines, vs_scroll, vs_alt };

static int row_is(struct vscr* s, int r, const char* want) {
    for (int c = 0; c < VC; c++) if (s->cell[r][c] != want[c]) return 0;
    return 1;
}
static void feed(struct vt* v, struct vscr* s, const char* str) { for (; *str; str++) vt_feed(v, *str, &vs_ops, s); }

static void cmd_vttest(const char* args) {
    (void)args;
    struct vt v; struct vscr s;
    int ok = 1;
    #define VT(cond, what) do { int _c = (cond); kprintf("vt: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    vt_init(&v); s.nreply = 0; s.nattr = 0; s.fg = s.bg = VT_DEFAULT; s.bold = s.rev = 0; vs_put(&s, '\f');
    feed(&v, &s, "hello\x1b[2Dxy");
    VT(row_is(&s, 0, "helxy....."), "cursor back (CSI D) then overwrite");
    feed(&v, &s, "\x1b[1;3H\x1b[K");
    VT(row_is(&s, 0, "he........") && s.r == 0 && s.c == 2, "cursor position (CSI H) and erase to end of line (CSI K)");
    feed(&v, &s, "\x1b[3;5HZ\x1b[6n");
    VT(s.cell[2][4] == 'Z' && s.nreply == 6 && s.reply[0] == 0x1b && s.reply[2] == '3' && s.reply[4] == '6',
       "row 3 col 5, and the cursor-position report says 3;6");
    feed(&v, &s, "\x1b[1;1Habcdef\x1b[1;3H\x1b[2P");
    VT(row_is(&s, 0, "abef......"), "delete two characters (CSI P)");
    feed(&v, &s, "\x1b[1;2H\x1b[2@");
    VT(row_is(&s, 0, "a..bef...."), "insert two blanks (CSI @)");
    feed(&v, &s, "\x1b[31;1mR");
    VT(s.fg == 1 && s.bg == VT_DEFAULT && s.bold, "SGR 31;1: red, bold, default background");
    feed(&v, &s, "\x1b[0m\x1b]0;title\x07\x1b(BS");
    VT(row_is(&s, 0, "aRSbef....") && s.fg == VT_DEFAULT && !s.bold,
       "SGR 0 resets; a window title and a charset are consumed, never printed");
    feed(&v, &s, "\x1b[38;5;200;48;2;0;0;255m");
    VT(s.fg == 200 && s.bg == 21, "256-colour foreground, direct-colour background (nearest: 21)");
    feed(&v, &s, "\x1b[97;7m");
    VT(s.fg == 15 && s.rev, "bright white (97) and reverse (7)");
    feed(&v, &s, "\x1b[39;49;27m");
    VT(s.fg == VT_DEFAULT && s.bg == VT_DEFAULT && !s.rev, "default colours (39, 49), reverse off (27)");
    feed(&v, &s, "\x1b[2J");
    VT(row_is(&s, 0, "..........") && row_is(&s, 2, ".........."), "erase the screen (CSI 2J)");
    feed(&v, &s, "\x1b[99;99H");
    VT(s.r == VR - 1 && s.c == VC - 1, "a position past the edge is clamped");
    feed(&v, &s, "\x1b[1;1Hq\x1b[A\x1b[3Bw");
    VT(s.cell[3][1] == 'w', "up at the top stays, down moves");
    /* ("\x1b" "7", not "\x1b7": a hex escape takes every hex digit after it) */
    feed(&v, &s, "\x1b" "7" "\x1b[5;5H" "\x1b" "8" "x");
    VT(s.cell[3][2] == 'x', "save and restore the cursor (ESC 7 / 8)");
    feed(&v, &s, "\x1b[2J\x1b[1;1H1\x1b[2;1H2\x1b[3;1H3\x1b[2;1H\x1b[L");
    VT(s.cell[0][0] == '1' && s.cell[1][0] == '.' && s.cell[2][0] == '2' && s.cell[3][0] == '3',
       "insert a line (CSI L): the rest moves down");
    feed(&v, &s, "\x1b[1;1H\x1b[2M");
    VT(s.cell[0][0] == '2' && s.cell[1][0] == '3' && s.cell[4][0] == '.', "delete two lines (CSI 2M): the rest moves up");
    /* 2026-10-16 — the scroll region and the alternate screen (vim, less). */
    feed(&v, &s, "\x1b[2J\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD\x1b[5;1HE");
    feed(&v, &s, "\x1b[2;4r");
    VT(s.r == 0 && s.c == 0, "setting a region (CSI 2;4 r) homes the cursor");
    feed(&v, &s, "\x1b[4;1H\n");
    VT(s.cell[0][0] == 'A' && s.cell[1][0] == 'C' && s.cell[2][0] == 'D' && s.cell[3][0] == '.' &&
       s.cell[4][0] == 'E' && s.r == 3, "a line feed on the region's bottom scrolls ONLY the region");
    feed(&v, &s, "\x1b[2;1H\x1b" "M");
    VT(s.cell[1][0] == '.' && s.cell[2][0] == 'C' && s.cell[3][0] == 'D' && s.cell[4][0] == 'E' &&
       s.cell[0][0] == 'A', "reverse index on the region's top scrolls it down (less, one line back)");
    feed(&v, &s, "\x1b[3;1H\x1b[L");
    VT(s.cell[2][0] == '.' && s.cell[3][0] == 'C' && s.cell[4][0] == 'E',
       "insert a line inside the region: the region's bottom line falls off, the line below it stays");
    feed(&v, &s, "\x1b[2;1HX\x1b[3;1HY\x1b[4;1HZ\x1b[S");
    VT(s.cell[1][0] == 'Y' && s.cell[2][0] == 'Z' && s.cell[3][0] == '.' && s.cell[4][0] == 'E',
       "scroll up (CSI S) moves the region");
    feed(&v, &s, "\x1b[r\x1b[5;1H\n");
    VT(s.r == 4 && s.cell[4][0] == 'E', "CSI r resets to the whole screen (the renderer's own line feed)");
    feed(&v, &s, "\x1b[2J\x1b[1;1Hshell$\x1b[1;7H\x1b[?1049h");
    VT(s.alt && row_is(&s, 0, ".........."), "?1049h: a blank alternate screen");
    feed(&v, &s, "\x1b[3;3Hvim");
    feed(&v, &s, "\x1b[?1049l");
    VT(!s.alt && row_is(&s, 0, "shell$....") && s.cell[2][2] == '.' && s.r == 0 && s.c == 6,
       "?1049l: the shell's screen and cursor come back, vim's text is gone");
    feed(&v, &s, "\x1b[?47h\x1b[1;1Hq\x1b[?47l");
    VT(!s.alt && row_is(&s, 0, "shell$...."), "?47 switches without touching the cursor");
    kprintf("vt: %s\n", ok ? "ALL ok" : "FAIL");
    #undef VT
}
SHELL_CMD(vttest) = { "vttest", "", "the VT100 subset against a test screen", SHELL_G_TEST, cmd_vttest, SHELL_P_ANY };

/* `vtdemo` — sequences to LOOK at in a real terminal window (the parser is
 * proven by vttest; this proves a renderer): each line says what it should
 * read as. */
static void cmd_vtdemo(const char* args) {
    /* 2026-10-16 — `vtdemo alt` enters the alternate screen and draws a
     * framed page with a scroll region inside it (a status line top and
     * bottom that must NOT move while the middle scrolls); `vtdemo back`
     * leaves, and the screen from before `vtdemo alt` must come back intact. */
    if (args && args[0] == 'a') {
        console_write("\x1b[?1049h\x1b[2J\x1b[1;1H\x1b[7m TOP STATUS - must stay \x1b[0m");
        console_write("\x1b[8;1H\x1b[7m BOTTOM STATUS - must stay \x1b[0m");
        console_write("\x1b[2;7r\x1b[2;1H");
        for (int i = 1; i <= 10; i++) {
            char b[40]; int k = 0;
            const char* t = "region line ";
            while (*t) b[k++] = *t++;
            if (i >= 10) b[k++] = (char)('0' + i / 10);
            b[k++] = (char)('0' + i % 10);
            b[k] = 0;
            console_write(b);
            if (i < 10) console_write("\r\n");
        }
        console_write("\x1b[2;1H\x1b" "M" "\x1b[2;1Hreverse-indexed in at the top");
        console_write("\x1b[r\x1b[10;1Hvtdemo: alternate screen\r\n");
        return;
    }
    if (args && args[0] == 'b') { console_write("\x1b[?1049l"); return; }
    console_write("1 want 'vtdemo: BB'    > vtdemo: AAAA\x1b[4DBB\x1b[K\n");
    console_write("2 want 'red' in red    > \x1b[31;1mred\x1b[0m, no codes\n");
    console_write("6 want colours         > \x1b[32mgreen \x1b[44myellow-on-blue\x1b[0m "
                  "\x1b[38;5;208morange(256)\x1b[0m \x1b[7mreverse\x1b[0m\n");
    console_write("3 want 'abcf'          > abcdef\x1b[3D\x1b[2P\n");
    console_write("4 want 'a  bc'         > abc\x1b[2D\x1b[2@\n");
    console_write("5 want 'title gone'    > \x1b]0;a window title\x07title gone\n");
}
SHELL_CMD(vtdemo) = { "vtdemo", "[alt|back]", "print VT100 sequences to look at", SHELL_G_TEST, cmd_vtdemo, SHELL_P_ANY };
