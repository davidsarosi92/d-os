/* =============================================================================
 * itemview.c — the two layouts that ship today (grid, list) plus the registry.
 * See itemview.h for why the layout is separate from the collection.
 *
 * Both views are written against the same three numbers — cell size, columns,
 * first visible item — so a third layout ("details", "tiles") is a copy of the
 * shorter one with different geometry, and nothing outside this file changes.
 *
 * Labels are drawn with the 8×8 font and CLIPPED BY CHARACTER COUNT rather
 * than by pixel: gfx_text has no width limit, so a long shortcut name would
 * happily paint across its neighbours.  Truncation is with an ellipsis
 * character ('~' — the font is ASCII) so a cut label is visibly cut.
 * ============================================================================= */

#include "itemview.h"
#include "icons.h"
#include "gfx.h"
#include <stddef.h>

#define SEL_FILL    0x603D6FB8u         /* translucent selection wash     */
#define SEL_EDGE    0xFF6E9BE0u
#define LBL_FG      0xFFF2F5FAu
#define LBL_DIM     0xFF8B94A6u
#define SUB_FG      0xFFA9B4C8u

/* Draw `text` at (x,y) truncated to `maxch` characters. */
static void text_clipped(struct gfx_surface* s, int x, int y,
                         const char* text, int maxch, uint32_t col) {
    if (!text || maxch <= 0) return;
    char buf[64];
    if (maxch > (int)sizeof buf - 1) maxch = (int)sizeof buf - 1;
    int n = 0;
    while (text[n] && n < maxch) { buf[n] = text[n]; n++; }
    if (text[n]) {                       /* did not fit → mark the cut */
        if (n > 0) buf[n - 1] = '~';
    }
    buf[n] = '\0';
    gfx_text(s, x, y, buf, col);
}

/* Centre a string of `len` glyphs inside `w`. */
static int centre_x(int w, int len) {
    int px = len * GFX_GLYPH_W;
    return px >= w ? 0 : (w - px) / 2;
}
static int str_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

/* ===================================================================== */
/* GRID — icons in rows, label under each.  The desktop's default.       */
/* ===================================================================== */

#define G_CELL_W    96
#define G_CELL_H    96
#define G_ICON      48
#define G_PAD       8

static int g_cols(int w) {
    int c = w / G_CELL_W;
    return c < 1 ? 1 : c;
}

static int grid_rect(int i, int w, int h, const struct item_model* m, int scroll,
                     int* ox, int* oy, int* ow, int* oh) {
    (void)m;
    int cols = g_cols(w);
    int idx  = i - scroll;
    if (idx < 0) return -1;
    int r = idx / cols, c = idx % cols;
    int y = r * G_CELL_H;
    if (y >= h) return -1;
    *ox = c * G_CELL_W; *oy = y; *ow = G_CELL_W; *oh = G_CELL_H;
    return 0;
}

static int grid_page(int w, int h) {
    int rows = h / G_CELL_H; if (rows < 1) rows = 1;
    return rows * g_cols(w);
}

static void grid_draw(struct gfx_surface* s, int x, int y, int w, int h,
                      const struct item_model* m, int sel, int scroll) {
    if (!m || !m->count || !m->get) return;
    int n = m->count(m->ctx);
    for (int i = scroll; i < n; i++) {
        int cx, cy, cw, ch;
        if (grid_rect(i, w, h, m, scroll, &cx, &cy, &cw, &ch) != 0) continue;
        struct item_entry e = { 0, 0, ICON_APP, 0 };
        if (m->get(m->ctx, i, &e) != 0) continue;

        cx += x; cy += y;
        if (i == sel) {
            gfx_blend_fill(s, cx + 2, cy + 2, cw - 4, ch - 4, SEL_FILL);
            gfx_line(s, cx + 2, cy + 2, cx + cw - 3, cy + 2, SEL_EDGE);
            gfx_line(s, cx + 2, cy + ch - 3, cx + cw - 3, cy + ch - 3, SEL_EDGE);
            gfx_line(s, cx + 2, cy + 2, cx + 2, cy + ch - 3, SEL_EDGE);
            gfx_line(s, cx + cw - 3, cy + 2, cx + cw - 3, cy + ch - 3, SEL_EDGE);
        }
        icon_draw(s, cx + (cw - G_ICON) / 2, cy + G_PAD, G_ICON, e.icon);

        int maxch = (cw - 6) / GFX_GLYPH_W;
        int len   = str_len(e.label);
        if (len > maxch) len = maxch;
        text_clipped(s, cx + centre_x(cw, len), cy + G_PAD + G_ICON + 6,
                     e.label, maxch, e.dim ? LBL_DIM : LBL_FG);
    }
}

static int grid_hit(int px, int py, int w, int h,
                    const struct item_model* m, int scroll) {
    if (!m || !m->count) return -1;
    if (px < 0 || py < 0 || px >= w || py >= h) return -1;
    int cols = g_cols(w);
    int c = px / G_CELL_W, r = py / G_CELL_H;
    if (c >= cols) return -1;
    int idx = scroll + r * cols + c;
    return idx < m->count(m->ctx) ? idx : -1;
}

ITEM_VIEW(itemview_grid) = {
    .name = "grid",
    .draw = grid_draw,
    .hit  = grid_hit,
    .rect = grid_rect,
    .page = grid_page,
};

/* ===================================================================== */
/* LIST — one item per row, small icon, label + optional sub-label.      */
/* ===================================================================== */

#define L_ROW_H     40
#define L_ICON      28
#define L_PAD       8

static int list_rect(int i, int w, int h, const struct item_model* m, int scroll,
                     int* ox, int* oy, int* ow, int* oh) {
    (void)m;
    int idx = i - scroll;
    if (idx < 0) return -1;
    int y = idx * L_ROW_H;
    if (y >= h) return -1;
    *ox = 0; *oy = y; *ow = w; *oh = L_ROW_H;
    return 0;
}

static int list_page(int w, int h) {
    (void)w;
    int rows = h / L_ROW_H;
    return rows < 1 ? 1 : rows;
}

static void list_draw(struct gfx_surface* s, int x, int y, int w, int h,
                      const struct item_model* m, int sel, int scroll) {
    if (!m || !m->count || !m->get) return;
    int n = m->count(m->ctx);
    for (int i = scroll; i < n; i++) {
        int cx, cy, cw, ch;
        if (list_rect(i, w, h, m, scroll, &cx, &cy, &cw, &ch) != 0) continue;
        struct item_entry e = { 0, 0, ICON_APP, 0 };
        if (m->get(m->ctx, i, &e) != 0) continue;

        cx += x; cy += y;
        if (i == sel)
            gfx_blend_fill(s, cx + 1, cy + 1, cw - 2, ch - 2, SEL_FILL);

        icon_draw(s, cx + L_PAD, cy + (ch - L_ICON) / 2, L_ICON, e.icon);

        int tx = cx + L_PAD + L_ICON + L_PAD;
        int maxch = (cw - (tx - cx) - L_PAD) / GFX_GLYPH_W;
        if (e.sub) {
            text_clipped(s, tx, cy + ch / 2 - GFX_GLYPH_H - 1, e.label, maxch,
                         e.dim ? LBL_DIM : LBL_FG);
            text_clipped(s, tx, cy + ch / 2 + 2, e.sub, maxch, SUB_FG);
        } else {
            text_clipped(s, tx, cy + (ch - GFX_GLYPH_H) / 2, e.label, maxch,
                         e.dim ? LBL_DIM : LBL_FG);
        }
    }
}

static int list_hit(int px, int py, int w, int h,
                    const struct item_model* m, int scroll) {
    if (!m || !m->count) return -1;
    if (px < 0 || py < 0 || px >= w || py >= h) return -1;
    int idx = scroll + py / L_ROW_H;
    return idx < m->count(m->ctx) ? idx : -1;
}

ITEM_VIEW(itemview_list) = {
    .name = "list",
    .draw = list_draw,
    .hit  = list_hit,
    .rect = list_rect,
    .page = list_page,
};

/* ===================================================================== */
/* Registry.                                                             */
/* ===================================================================== */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int item_view_count(void) {
    return (int)(__stop_item_views - __start_item_views);
}
const struct item_view* item_view_at(int i) {
    if (i < 0 || i >= item_view_count()) return NULL;
    return &__start_item_views[i];
}

const struct item_view* item_view_by_name(const char* name) {
    int n = item_view_count();
    if (n == 0) return NULL;                    /* nothing linked — caller checks */
    if (name && *name) {
        for (int i = 0; i < n; i++)
            if (__start_item_views[i].name && streq_(__start_item_views[i].name, name))
                return &__start_item_views[i];
    }
    /* Unknown name → the first registered view.  A mistyped `desktop.view`
     * should give a usable desktop with the wrong layout, not an empty one. */
    return &__start_item_views[0];
}

/* ===================================================================== */
/* §M65 stage 3 — TABLE: the same model, asked about columns.            */
/*                                                                        */
/* This is deliberately a VIEW and not a new widget.  The file manager,   */
/* the task manager and a settings list are one idea — rows of records —  */
/* and the difference between them is presentation.  Writing a third      */
/* widget would have made "show it as a table instead" a rewrite in each. */
/*                                                                        */
/* RESPONSIVE, in the one way that matters for a table: when the box is   */
/* too narrow for every column, the ones on the right are DROPPED rather  */
/* than squeezed into illegibility.  Column 0 is never dropped — it is    */
/* the record's identity, and a table with no identity column is a grid   */
/* of numbers.                                                            */
/* ===================================================================== */

#define T_ROW_H   18
#define T_HEAD_H  20
#define T_PAD     6
#define T_MIN_COL (8 * GFX_GLYPH_W)     /* below this a column is useless */

#define TCOL_HEAD   0xFF1B2434u
#define TCOL_TEXT   0xFFDDE4EEu
#define TCOL_DIM    0xFF93A1B4u
#define TCOL_SEL    0xFF2C5B9Eu
#define TCOL_RULE   0xFF2A3547u

static void t_cell(const struct item_model* m, int i, int c, char* out, int cap);

static int t_cols(const struct item_model* m) {
    int n = (m->columns && m->cell) ? m->columns(m->ctx) : 1;
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return n;
}

/* How many columns actually fit, and where each starts.  Returns the count
 * kept; `xs`/`ws` receive their geometry.
 *
 * SIZED FROM CONTENT, not from weights alone.  The first version divided the
 * width by the declared weights, and a column whose text was longer than its
 * share simply ran into the next one — reported from use as "it all runs
 * together".  Weights are a preference; the longest cell is a fact, and a
 * table that ignores it is a table nobody can read.
 *
 * The scan is BOUNDED (T_SCAN rows): the column widths must not become a
 * function of how many rows a directory happens to have. */
#define T_SCAN 64
#define T_GAP  (2 * GFX_GLYPH_W)        /* never let two columns touch */

static int t_layout(const struct item_model* m, int w, int* xs, int* ws) {
    int n = t_cols(m);
    int avail = w - 2 * T_PAD;
    int nat[8];

    int rows = m->count ? m->count(m->ctx) : 0;
    if (rows > T_SCAN) rows = T_SCAN;
    for (int c = 0; c < n; c++) {
        const char* t = m->col_title ? m->col_title(m->ctx, c) : "";
        int longest = 0;
        while (t && t[longest]) longest++;
        for (int i = 0; i < rows; i++) {
            char buf[64];
            t_cell(m, i, c, buf, (int)sizeof buf);
            int k = 0;
            while (buf[k]) k++;
            if (k > longest) longest = k;
        }
        nat[c] = longest * GFX_GLYPH_W + T_GAP;
    }

    /* Drop from the RIGHT while what is left cannot be read.  Column 0 is
     * never dropped: it is the record's identity, and a table without one is a
     * grid of numbers. */
    while (n > 1) {
        int need = 0;
        for (int c = 0; c < n; c++) need += (nat[c] < T_MIN_COL ? T_MIN_COL : nat[c]);
        if (need <= avail) break;
        /* Before giving a column up, try shrinking the wide ones to the floor. */
        int floor_need = n * T_MIN_COL;
        if (floor_need <= avail) break;
        n--;
    }

    int total_nat = 0;
    for (int c = 0; c < n; c++) total_nat += nat[c];

    int x = T_PAD;
    for (int c = 0; c < n; c++) {
        int cw;
        if (total_nat <= avail) {
            /* Everything fits: natural width, and the LAST column absorbs the
             * slack so the table fills its box instead of ending mid-air. */
            cw = (c == n - 1) ? avail - (x - T_PAD) : nat[c];
        } else {
            cw = avail * nat[c] / (total_nat ? total_nat : 1);
            if (cw < T_MIN_COL) cw = T_MIN_COL;
        }
        xs[c] = x; ws[c] = cw;
        x += cw;
    }
    return n;
}

/* One cell's text: from the model's `cell` when it has one, otherwise the
 * entry's label — which is what makes an old single-column model render. */
static void t_cell(const struct item_model* m, int i, int c, char* out, int cap) {
    out[0] = 0;
    if (m->cell && m->columns) { m->cell(m->ctx, i, c, out, cap); return; }
    if (c != 0) return;
    struct item_entry e = {0};
    if (m->get && m->get(m->ctx, i, &e) == 0 && e.label) {
        int k = 0;
        for (; e.label[k] && k < cap - 1; k++) out[k] = e.label[k];
        out[k] = 0;
    }
}

static void table_draw(struct gfx_surface* s, int x, int y, int w, int h,
                       const struct item_model* m, int sel, int scroll) {
    if (!m || !m->count) return;
    int xs[8], ws[8];
    int n = t_layout(m, w, xs, ws);

    /* Header — drawn from the model, not from a caller's padded string.  The
     * file manager used to fake this with spaces in a label, which is exactly
     * the kind of thing that stops being aligned the moment a name is long. */
    gfx_fill(s, x, y, w, T_HEAD_H, TCOL_HEAD);
    for (int c = 0; c < n; c++) {
        const char* t = m->col_title ? m->col_title(m->ctx, c) : "";
        if (!t) continue;
        gfx_set_clip(s, x + xs[c], y, ws[c] - T_GAP, T_HEAD_H);
        gfx_text(s, x + xs[c], y + (T_HEAD_H - GFX_GLYPH_H) / 2, t, TCOL_DIM);
        gfx_clear_clip(s);
    }
    gfx_fill(s, x, y + T_HEAD_H - 1, w, 1, TCOL_RULE);

    int total = m->count(m->ctx);
    int rows  = (h - T_HEAD_H) / T_ROW_H;
    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= total) break;
        int ry = y + T_HEAD_H + r * T_ROW_H;
        if (i == sel) gfx_fill(s, x, ry, w, T_ROW_H, TCOL_SEL);
        for (int c = 0; c < n; c++) {
            char buf[64];
            t_cell(m, i, c, buf, (int)sizeof buf);
            if (!buf[0]) continue;
            /* Clip each cell to its own column: a long name must not run into
             * the size column, which is the failure a padded string cannot
             * even detect. */
            gfx_set_clip(s, x + xs[c], ry, ws[c] - T_GAP, T_ROW_H);
            gfx_text(s, x + xs[c], ry + (T_ROW_H - GFX_GLYPH_H) / 2, buf, TCOL_TEXT);
            gfx_clear_clip(s);
        }
    }
}

static int table_hit(int px, int py, int w, int h, const struct item_model* m,
                     int scroll) {
    (void)w;
    if (!m || !m->count) return -1;
    if (py < T_HEAD_H) return -1;                  /* the header is not a row */
    int r = (py - T_HEAD_H) / T_ROW_H;
    if (r < 0 || py >= h) return -1;
    int i = scroll + r;
    return (i < m->count(m->ctx)) ? i : -1;
}

static int table_rect(int i, int w, int h, const struct item_model* m, int scroll,
                      int* ox, int* oy, int* ow, int* oh) {
    (void)m;
    int r = i - scroll;
    if (r < 0) return -1;
    int y = T_HEAD_H + r * T_ROW_H;
    if (y >= h) return -1;
    *ox = 0; *oy = y; *ow = w; *oh = T_ROW_H;
    return 0;
}

static int table_page(int w, int h) {
    (void)w;
    int rows = (h - T_HEAD_H) / T_ROW_H;
    return rows < 1 ? 1 : rows;
}

ITEM_VIEW(itemview_table) = {
    .name = "table",
    .draw = table_draw,
    .hit  = table_hit,
    .rect = table_rect,
    .page = table_page,
};
