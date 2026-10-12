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
    default:
        return;                                      /* m h l r t ... consumed */
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
            else if (c == 'D') o->put(x, '\n');      /* index: down, scrolling */
            else if (c == 'E') { o->put(x, '\r'); o->put(x, '\n'); }
            else if (c == 'M') { if (o->set_cursor && r > 0) o->set_cursor(x, r - 1, col); }
            else if (c == 'c') { o->put(x, '\f'); vt_init(v); }   /* reset: clear */
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
struct vscr { char cell[VR][VC]; int r, c; char reply[32]; int nreply; };
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
static const struct vt_ops vs_ops = { vs_put, vs_get, vs_set, vs_dims, vs_erase, vs_shift, vs_reply };

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
    vt_init(&v); s.nreply = 0; vs_put(&s, '\f');
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
    feed(&v, &s, "\x1b[31;1mR\x1b[0m\x1b]0;title\x07\x1b(BS");
    VT(row_is(&s, 0, "aRSbef...."), "colours, a window title and a charset are consumed, never printed");
    feed(&v, &s, "\x1b[2J");
    VT(row_is(&s, 0, "..........") && row_is(&s, 2, ".........."), "erase the screen (CSI 2J)");
    feed(&v, &s, "\x1b[99;99H");
    VT(s.r == VR - 1 && s.c == VC - 1, "a position past the edge is clamped");
    feed(&v, &s, "\x1b[1;1Hq\x1b[A\x1b[3Bw");
    VT(s.cell[3][1] == 'w', "up at the top stays, down moves");
    /* ("\x1b" "7", not "\x1b7": a hex escape takes every hex digit after it) */
    feed(&v, &s, "\x1b" "7" "\x1b[5;5H" "\x1b" "8" "x");
    VT(s.cell[3][2] == 'x', "save and restore the cursor (ESC 7 / 8)");
    kprintf("vt: %s\n", ok ? "ALL ok" : "FAIL");
    #undef VT
}
SHELL_CMD(vttest) = { "vttest", "", "the VT100 subset against a test screen", SHELL_G_TEST, cmd_vttest, SHELL_P_ANY };

/* `vtdemo` — sequences to LOOK at in a real terminal window (the parser is
 * proven by vttest; this proves a renderer): each line says what it should
 * read as. */
static void cmd_vtdemo(const char* args) {
    (void)args;
    console_write("1 want 'vtdemo: BB'    > vtdemo: AAAA\x1b[4DBB\x1b[K\n");
    console_write("2 want 'red, no codes' > \x1b[31;1mred\x1b[0m, no codes\n");
    console_write("3 want 'abcf'          > abcdef\x1b[3D\x1b[2P\n");
    console_write("4 want 'a  bc'         > abc\x1b[2D\x1b[2@\n");
    console_write("5 want 'title gone'    > \x1b]0;a window title\x07title gone\n");
}
SHELL_CMD(vtdemo) = { "vtdemo", "", "print VT100 sequences to look at", SHELL_G_TEST, cmd_vtdemo, SHELL_P_ANY };
