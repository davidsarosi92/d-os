/* =============================================================================
 * basic.c — Tiny-BASIC interpreter (M22.5).  See basic.h for the
 * dialect, the threading contract and the design notes.
 *
 * Structure:
 *   - a hand-rolled recursive-descent expression parser (rel → sum →
 *     term → factor), everything 32-bit signed;
 *   - one statement executor working on a bare text pointer — the RUN
 *     loop, IF...THEN recursion and the REPL's immediate mode all call
 *     the same function;
 *   - the program store is a sorted array of numbered lines; GOTO/
 *     GOSUB do a linear scan (256 lines max — not worth a search
 *     structure).
 *
 * Error handling: the parser sets b->err and the run loop reports
 * "?ERROR IN <line>" and stops — the classic BASIC experience, no
 * kernel panic paths anywhere near user input.
 * ============================================================================= */

#include "basic.h"
#include "vc.h"
#include "task.h"
#include "timer.h"
#include "vfs.h"
#include "printf.h"
#include "kmalloc.h"
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Small helpers (freestanding — no libc).                                     */
/* -------------------------------------------------------------------------- */

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int  is_digit(char c) { return c >= '0' && c <= '9'; }
static int  is_alpha(char c) { char u = up(c); return u >= 'A' && u <= 'Z'; }

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Case-insensitive keyword match; must NOT be followed by an
 * identifier character (so "PRINTER" doesn't match PRINT). */
static const char* kw(const char* p, const char* word) {
    p = skip_ws(p);
    const char* q = p;
    while (*word) {
        if (up(*q) != *word) return NULL;
        q++; word++;
    }
    if (is_alpha(*q)) return NULL;
    return q;
}

/* -------------------------------------------------------------------------- */
/* Expression parser.                                                          */
/* -------------------------------------------------------------------------- */

static int32_t bas_rel(struct basic* b, const char** pp);

static int32_t bas_factor(struct basic* b, const char** pp) {
    const char* p = skip_ws(*pp);

    if (*p == '-') { p++; *pp = p; int32_t v = bas_factor(b, pp); return -v; }

    if (*p == '(') {
        p++; *pp = p;
        int32_t v = bas_rel(b, pp);
        p = skip_ws(*pp);
        if (*p != ')') { b->err = 1; return 0; }
        *pp = p + 1;
        return v;
    }

    if (is_digit(*p)) {
        int32_t v = 0;
        while (is_digit(*p)) { v = v * 10 + (*p - '0'); p++; }
        *pp = p;
        return v;
    }

    const char* q;
    if ((q = kw(p, "RND")) != NULL) {           /* RND(n) → 0..n-1 */
        q = skip_ws(q);
        if (*q != '(') { b->err = 1; return 0; }
        q++; *pp = q;
        int32_t n = bas_rel(b, pp);
        q = skip_ws(*pp);
        if (*q != ')') { b->err = 1; return 0; }
        *pp = q + 1;
        if (n <= 0) return 0;
        /* Park–Miller-ish LCG, seeded from the timer on first use. */
        if (b->rnd == 0) b->rnd = (uint32_t)timer_ticks_ms() | 1;
        b->rnd = b->rnd * 1103515245u + 12345u;
        return (int32_t)((b->rnd >> 16) % (uint32_t)n);
    }
    if ((q = kw(p, "ABS")) != NULL) {           /* ABS(n) */
        q = skip_ws(q);
        if (*q != '(') { b->err = 1; return 0; }
        q++; *pp = q;
        int32_t n = bas_rel(b, pp);
        q = skip_ws(*pp);
        if (*q != ')') { b->err = 1; return 0; }
        *pp = q + 1;
        return n < 0 ? -n : n;
    }

    if (is_alpha(*p) && !is_alpha(p[1])) {      /* single-letter variable */
        int32_t v = b->vars[up(*p) - 'A'];
        *pp = p + 1;
        return v;
    }

    b->err = 1;
    return 0;
}

static int32_t bas_term(struct basic* b, const char** pp) {
    int32_t v = bas_factor(b, pp);
    for (;;) {
        const char* p = skip_ws(*pp);
        if (*p == '*') { *pp = p + 1; v *= bas_factor(b, pp); }
        else if (*p == '/') {
            *pp = p + 1;
            int32_t d = bas_factor(b, pp);
            if (d == 0) { b->err = 1; return 0; }   /* division by zero */
            v /= d;
        }
        else if (*p == '%') {
            *pp = p + 1;
            int32_t d = bas_factor(b, pp);
            if (d == 0) { b->err = 1; return 0; }
            v %= d;
        }
        else return v;
    }
}

static int32_t bas_sum(struct basic* b, const char** pp) {
    int32_t v = bas_term(b, pp);
    for (;;) {
        const char* p = skip_ws(*pp);
        if      (*p == '+') { *pp = p + 1; v += bas_term(b, pp); }
        else if (*p == '-') { *pp = p + 1; v -= bas_term(b, pp); }
        else return v;
    }
}

static int32_t bas_rel(struct basic* b, const char** pp) {
    int32_t v = bas_sum(b, pp);
    const char* p = skip_ws(*pp);
    if (*p == '<') {
        if (p[1] == '>') { *pp = p + 2; return v != bas_sum(b, pp); }
        if (p[1] == '=') { *pp = p + 2; return v <= bas_sum(b, pp); }
        *pp = p + 1; return v < bas_sum(b, pp);
    }
    if (*p == '>') {
        if (p[1] == '=') { *pp = p + 2; return v >= bas_sum(b, pp); }
        *pp = p + 1; return v > bas_sum(b, pp);
    }
    if (*p == '=') { *pp = p + 1; return v == bas_sum(b, pp); }
    return v;
}

/* -------------------------------------------------------------------------- */
/* Program store.                                                              */
/* -------------------------------------------------------------------------- */

static int find_line_idx(struct basic* b, int num) {   /* first idx with num >= */
    for (int i = 0; i < b->nlines; i++)
        if (b->prog[i].num >= num) return i;
    return b->nlines;
}

/* Insert / replace / delete (empty text) a numbered line. */
static void store_line(struct basic* b, int num, const char* text) {
    int i = find_line_idx(b, num);
    int exists = (i < b->nlines && b->prog[i].num == num);
    const char* t = skip_ws(text);

    if (!*t) {                                  /* bare number = delete */
        if (exists) {
            for (int j = i; j < b->nlines - 1; j++) b->prog[j] = b->prog[j + 1];
            b->nlines--;
        }
        return;
    }
    if (!exists) {
        if (b->nlines >= BAS_MAX_LINES) { kprintf("?PROGRAM FULL\n"); return; }
        for (int j = b->nlines; j > i; j--) b->prog[j] = b->prog[j - 1];
        b->nlines++;
        b->prog[i].num = num;
    }
    int k = 0;
    for (; t[k] && k < BAS_TEXT_LEN - 1; k++) b->prog[i].text[k] = t[k];
    b->prog[i].text[k] = 0;
}

/* GOTO/GOSUB target: exact line number → index. */
static int goto_idx(struct basic* b, int num) {
    int i = find_line_idx(b, num);
    if (i < b->nlines && b->prog[i].num == num) return i;
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Console input (INPUT + REPL) — vc_getchar with echo, like the shell.        */
/* -------------------------------------------------------------------------- */

static void bas_read_line(struct basic* b, char* buf, int cap) {
    int len = 0;
    for (;;) {
        char c = vc_getchar(b->vc);
        if (c == '\n') { vc_putchar(b->vc, '\n'); buf[len] = 0; return; }
        if (c == '\b') {
            if (len > 0) { len--; vc_putchar(b->vc, '\b'); }
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = c;
            vc_putchar(b->vc, c);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Statement executor.                                                         */
/*                                                                             */
/* Return codes: 0 = fall through to next line, 1 = control transfer           */
/* (b->pc already set), 2 = END, -1 = error.                                   */
/* -------------------------------------------------------------------------- */

static int exec_stmt(struct basic* b, const char* s) {
    const char* p;
    b->err = 0;

    if ((p = kw(s, "REM")) != NULL) return 0;

    if ((p = kw(s, "PRINT")) != NULL) {
        int newline = 1;
        p = skip_ws(p);
        while (*p) {
            newline = 1;
            if (*p == '"') {                    /* string literal */
                p++;
                while (*p && *p != '"') { kprintf("%c", *p); p++; }
                if (*p != '"') return -1;       /* unterminated */
                p++;
            } else {                            /* expression */
                int32_t v = bas_rel(b, &p);
                if (b->err) return -1;
                kprintf("%d", (int)v);
            }
            p = skip_ws(p);
            if (*p == ';') { p = skip_ws(p + 1); newline = 0; continue; }
            if (*p == ',') { kprintf("    ");   p = skip_ws(p + 1); newline = 0; continue; }
            break;
        }
        if (*skip_ws(p)) return -1;             /* trailing junk */
        if (newline) kprintf("\n");
        return 0;
    }

    if ((p = kw(s, "INPUT")) != NULL) {
        p = skip_ws(p);
        if (!is_alpha(*p) || is_alpha(p[1])) return -1;
        int vi = up(*p) - 'A';
        kprintf("? ");
        char line[32];
        bas_read_line(b, line, (int)sizeof line);
        if (task_should_stop()) return 2;       /* killed while waiting */
        const char* q = skip_ws(line);
        int neg = 0;
        if (*q == '-') { neg = 1; q++; }
        int32_t v = 0;
        while (is_digit(*q)) { v = v * 10 + (*q - '0'); q++; }
        b->vars[vi] = neg ? -v : v;
        return 0;
    }

    if ((p = kw(s, "IF")) != NULL) {
        int32_t c = bas_rel(b, &p);
        if (b->err) return -1;
        const char* q = kw(p, "THEN");
        if (!q) return -1;
        if (!c) return 0;                       /* condition false */
        q = skip_ws(q);
        if (is_digit(*q)) {                     /* THEN <line> = GOTO */
            int32_t n = bas_rel(b, &q);
            if (b->err) return -1;
            int idx = goto_idx(b, (int)n);
            if (idx < 0) return -1;
            b->pc = idx;
            return 1;
        }
        return exec_stmt(b, q);                 /* THEN <statement> */
    }

    if ((p = kw(s, "GOTO")) != NULL) {
        int32_t n = bas_rel(b, &p);
        if (b->err) return -1;
        int idx = goto_idx(b, (int)n);
        if (idx < 0) return -1;
        b->pc = idx;
        return 1;
    }

    if ((p = kw(s, "GOSUB")) != NULL) {
        int32_t n = bas_rel(b, &p);
        if (b->err) return -1;
        int idx = goto_idx(b, (int)n);
        if (idx < 0 || b->gsp >= BAS_STACK) return -1;
        b->gosub[b->gsp++] = b->pc + 1;         /* resume after this line */
        b->pc = idx;
        return 1;
    }

    if ((p = kw(s, "RETURN")) != NULL) {
        if (b->gsp <= 0) return -1;
        b->pc = b->gosub[--b->gsp];
        return 1;
    }

    if ((p = kw(s, "FOR")) != NULL) {
        p = skip_ws(p);
        if (!is_alpha(*p) || is_alpha(p[1])) return -1;
        int vi = up(*p) - 'A';
        p = skip_ws(p + 1);
        if (*p != '=') return -1;
        p++;
        int32_t start = bas_rel(b, &p);
        if (b->err) return -1;
        const char* q = kw(p, "TO");
        if (!q) return -1;
        int32_t limit = bas_rel(b, &q);
        if (b->err) return -1;
        int32_t step = 1;
        const char* r = kw(q, "STEP");
        if (r) {
            step = bas_rel(b, &r);
            if (b->err || step == 0) return -1;
            q = r;
        }
        if (*skip_ws(q)) return -1;
        if (b->forsp >= BAS_STACK) return -1;
        b->vars[vi] = start;
        b->forstk[b->forsp].var      = vi;
        b->forstk[b->forsp].limit    = limit;
        b->forstk[b->forsp].step     = step;
        b->forstk[b->forsp].line_idx = b->pc;
        b->forsp++;
        return 0;
    }

    if ((p = kw(s, "NEXT")) != NULL) {
        p = skip_ws(p);
        int vi = -1;
        if (is_alpha(*p) && !is_alpha(p[1])) vi = up(*p) - 'A';
        if (b->forsp <= 0) return -1;
        /* NEXT with a variable unwinds to the matching FOR (allows
         * breaking out of an inner loop via GOTO past its NEXT). */
        if (vi >= 0) {
            while (b->forsp > 0 && b->forstk[b->forsp - 1].var != vi)
                b->forsp--;
            if (b->forsp <= 0) return -1;
        }
        int top = b->forsp - 1;
        int v = b->forstk[top].var;
        b->vars[v] += b->forstk[top].step;
        int cont = b->forstk[top].step > 0
                 ? b->vars[v] <= b->forstk[top].limit
                 : b->vars[v] >= b->forstk[top].limit;
        if (cont) {
            b->pc = b->forstk[top].line_idx + 1;    /* first body line */
            return 1;
        }
        b->forsp--;
        return 0;
    }

    if ((p = kw(s, "CLS")) != NULL) { kprintf("\f"); return 0; }

    if ((p = kw(s, "END")) != NULL || (p = kw(s, "STOP")) != NULL) return 2;

    /* LET v = expr — with LET optional. */
    p = kw(s, "LET");
    if (!p) p = s;
    p = skip_ws(p);
    if (is_alpha(*p) && !is_alpha(p[1])) {
        int vi = up(*p) - 'A';
        const char* q = skip_ws(p + 1);
        if (*q == '=') {
            q++;
            int32_t v = bas_rel(b, &q);
            if (b->err || *skip_ws(q)) return -1;
            b->vars[vi] = v;
            return 0;
        }
    }
    return -1;                                  /* unrecognized statement */
}

/* -------------------------------------------------------------------------- */
/* Run loop — the kthread-contract heart (see basic.h).                        */
/* -------------------------------------------------------------------------- */

void basic_run(struct basic* b) {
    b->pc = 0;
    b->forsp = 0;
    b->gsp = 0;
    int steps = 0;

    while (b->pc < b->nlines) {
        /* Kill point: poll FIRST so a stop request between yields is
         * honored before more statements run, then yield so a tight
         * GOTO loop can't starve the system (the yield itself is also
         * the cooperative-kill landing site). */
        if (task_should_stop()) return;
        if (++steps >= 64) { steps = 0; task_yield(); }

        int cur = b->pc;
        int r = exec_stmt(b, b->prog[cur].text);
        if (r < 0) {
            kprintf("?ERROR IN %d: %s\n", b->prog[cur].num, b->prog[cur].text);
            return;
        }
        if (r == 2) return;                     /* END / STOP / killed */
        if (r == 0) b->pc = cur + 1;            /* 1 = transferred already */
    }
}

/* -------------------------------------------------------------------------- */
/* LOAD / SAVE.                                                                */
/* -------------------------------------------------------------------------- */

void basic_init(struct basic* b, struct vc* vc) {
    /* No memset in freestanding core — clear field by field. */
    b->nlines = 0;
    for (int i = 0; i < 26; i++) b->vars[i] = 0;
    b->vc = vc;
    b->pc = 0; b->err = 0; b->forsp = 0; b->gsp = 0;
    b->rnd = 0;
}

int basic_load(struct basic* b, const char* path) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;

    b->nlines = 0;
    char line[BAS_TEXT_LEN + 16];
    int  li = 0;
    char chunk[256];
    ssize_t n;
    int done = 0;

    while (!done && (n = vfs_read(f, chunk, sizeof chunk)) >= 0) {
        if (n == 0) { done = 1; chunk[0] = '\n'; n = 1; }   /* flush tail */
        for (ssize_t i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (li < (int)sizeof(line) - 1) line[li++] = c;
                continue;
            }
            line[li] = 0;
            li = 0;
            const char* q = skip_ws(line);
            if (!*q) continue;                  /* blank line */
            if (!is_digit(*q)) { vfs_close(f); return -1; } /* unnumbered */
            int num = 0;
            while (is_digit(*q)) { num = num * 10 + (*q - '0'); q++; }
            store_line(b, num, q);
        }
    }
    vfs_close(f);
    return 0;
}

int basic_save(struct basic* b, const char* path) {
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;
    for (int i = 0; i < b->nlines; i++) {
        char out[BAS_TEXT_LEN + 16];
        int p = 0;
        int num = b->prog[i].num, digs = 0;
        char tmp[12];
        do { tmp[digs++] = (char)('0' + num % 10); num /= 10; } while (num);
        while (digs) out[p++] = tmp[--digs];
        out[p++] = ' ';
        for (int k = 0; b->prog[i].text[k]; k++) out[p++] = b->prog[i].text[k];
        out[p++] = '\n';
        if (vfs_write(f, out, (size_t)p) != p) { vfs_close(f); return -1; }
    }
    vfs_close(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* REPL.                                                                       */
/* -------------------------------------------------------------------------- */

static void bas_list(struct basic* b) {
    for (int i = 0; i < b->nlines; i++)
        kprintf("%d %s\n", b->prog[i].num, b->prog[i].text);
}

/* Extract a path argument: bare word or "quoted". */
static const char* arg_path(const char* p, char* buf, int cap) {
    p = skip_ws(p);
    int i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < cap - 1) buf[i++] = *p++;
    } else {
        while (*p && *p != ' ' && i < cap - 1) buf[i++] = *p++;
    }
    buf[i] = 0;
    return i ? buf : NULL;
}

void basic_repl(struct basic* b, const char* autoload) {
    kprintf("d-os Tiny-BASIC (%d lines max). Type HELP for a refresher.\n",
            BAS_MAX_LINES);

    if (autoload && *autoload) {
        if (basic_load(b, autoload) == 0) {
            kprintf("LOADED %s\n", autoload);
            basic_run(b);
        } else {
            kprintf("?CANNOT LOAD %s\n", autoload);
        }
    }
    kprintf("READY\n");

    char line[BAS_TEXT_LEN + 16];
    char path[128];
    for (;;) {
        kprintf("> ");
        bas_read_line(b, line, (int)sizeof line);
        if (task_should_stop()) return;
        const char* p = skip_ws(line);
        if (!*p) continue;

        if (is_digit(*p)) {                     /* stored program line */
            int num = 0;
            while (is_digit(*p)) { num = num * 10 + (*p - '0'); p++; }
            store_line(b, num, p);
            continue;
        }

        const char* q;
        if ((q = kw(p, "RUN")) != NULL)  { basic_run(b);  continue; }
        if ((q = kw(p, "LIST")) != NULL) { bas_list(b);   continue; }
        if ((q = kw(p, "NEW")) != NULL)  { basic_init(b, b->vc); continue; }
        if ((q = kw(p, "BYE")) != NULL)  { kprintf("BYE\n"); return; }
        if ((q = kw(p, "HELP")) != NULL) {
            kprintf("lines: 10 PRINT \"HI\"  |  bare number deletes\n"
                    "stmts: PRINT INPUT LET IF..THEN GOTO GOSUB RETURN\n"
                    "       FOR..TO..STEP NEXT REM CLS END STOP\n"
                    "expr:  + - * / %% ( ) = <> < > <= >= RND(n) ABS(n)\n"
                    "repl:  RUN LIST NEW LOAD <p> SAVE <p> BYE\n");
            continue;
        }
        if ((q = kw(p, "LOAD")) != NULL) {
            const char* pa = arg_path(q, path, (int)sizeof path);
            if (!pa)                       kprintf("?LOAD <path>\n");
            else if (basic_load(b, pa) == 0) kprintf("LOADED %s\n", pa);
            else                           kprintf("?CANNOT LOAD %s\n", pa);
            continue;
        }
        if ((q = kw(p, "SAVE")) != NULL) {
            const char* pa = arg_path(q, path, (int)sizeof path);
            if (!pa)                       kprintf("?SAVE <path>\n");
            else if (basic_save(b, pa) == 0) kprintf("SAVED %s\n", pa);
            else                           kprintf("?CANNOT SAVE %s\n", pa);
            continue;
        }

        /* Anything else: immediate statement. */
        int r = exec_stmt(b, p);
        if (r < 0) kprintf("?ERROR\n");
    }
}
