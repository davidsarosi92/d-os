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
#include "console_plate.h"
#include "locale.h"
#include "scrollbar.h"
#include "gfx.h"
#include <stddef.h>

/* §M69 — THE THEME, READ LIVE.  These were five literals, and they were
 * light-on-dark: `LBL_FG` is very nearly white, so under the LIGHT theme the
 * labels were white text on a light pane — reported from use as *"in the light
 * theme the labels disappear."*  A palette that assumes one theme is not a
 * palette, it is a second theme nobody can switch away from.
 *
 * Read per use rather than cached, for w_controls.c's reason: `gui.theme` can
 * change between two draws and a cached copy is a rule somebody has to
 * remember to refresh. */
#define SEL_EDGE    (cp_current_theme()->accent)
#define LBL_FG      (cp_current_theme()->text)
#define LBL_DIM     (cp_current_theme()->muted)
#define SUB_FG      (cp_current_theme()->muted)
/* The grid's selection is a translucent WASH rather than a solid row fill —
 * an icon sits on top of it and a solid block would swallow the artwork.  So
 * the theme's selection colour with the design's alpha forced on, which keeps
 * the intent and drops the hardcoded hue. */
#define SEL_FILL    ((cp_current_theme()->sel_bg & 0x00FFFFFFu) | 0x60000000u)

/* §M69 — THE EMPTY STATE, drawn once for every layout (widget_specs.md §11).
 *
 * In ONE place rather than in each of the three views, because it is the same
 * picture in all of them and three copies would be three chances for one to
 * drift — which is exactly what happened to the scrollbar, four times over,
 * one file down.
 *
 * Returns non-zero when it painted, so a view can `return` immediately: an
 * empty collection has no rows, no header worth showing and no scrollbar, and
 * drawing chrome around nothing is what made a failed load and an empty folder
 * look alike. */
static int draw_empty(struct gfx_surface* s, int x, int y, int w, int h,
                      const struct item_model* m) {
    if (m && m->count && m->count(m->ctx) > 0) return 0;
    /* THE OWNER DECIDES WHETHER EMPTINESS NEEDS EXPLAINING, and a model that
     * says nothing gets nothing — exactly the behaviour before this existed.
     *
     * Reported from use within minutes of shipping: *"what is this dashed
     * thing on the desktop?"*  The desktop is a GRID model too, so an empty
     * icon field grew a placeholder box across the wallpaper.  It was wrong
     * twice over: an empty desktop is a NORMAL state that needs no
     * explanation, and the placeholder's whole visual argument — a dashed
     * frame reading as "a container waiting for content" — depends on there
     * being a container, which a field painted straight onto the wallpaper is
     * not.  *A default that applies everywhere is a decision made for surfaces
     * nobody looked at.* */
    if (!m || !m->empty_text) return 0;
    const cp_theme* t = cp_current_theme();
    /* The design's inset, so the placeholder reads as sitting INSIDE the pane
     * rather than as the pane's own border having gone dashed. */
    int pad = cp_px(14);
    int bx = x + pad, by = y + pad, bw = w - 2 * pad, bh = h - 2 * pad;
    if (bw < cp_px(60) || bh < cp_row_h()) return 1;   /* too small to say so */
    cp_border_dashed(s, bx, by, bw, bh, t->line);

    const char* msg = lstr(m->empty_text);
    const char* act = (m && m->empty_action) ? lstr(m->empty_action) : NULL;
    int th = cp_fh();
    int total = act ? th * 2 + cp_px(6) : th;
    int ty = by + (bh - total) / 2;
    cp_text(s, bx + (bw - cp_text_w(msg)) / 2, ty, msg, t->muted);
    if (act)
        cp_text(s, bx + (bw - cp_text_w(act)) / 2, ty + th + cp_px(6), act,
                t->accent);
    return 1;
}

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
    cp_text(s, x, y, buf, col);
}

/* Centre a string of `len` glyphs inside `w`. */
static int centre_x(int w, int len) {
    int px = len * cp_fw();
    return px >= w ? 0 : (w - px) / 2;
}
static int str_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

/* ===================================================================== */
/* GRID — icons in rows, label under each.  The desktop's default.       */
/* ===================================================================== */

/* §M69 — the grid cell tracks the icon and the type, not the 8x8 font.
 *
 * These were four flat pixel counts, and the label was the one that showed:
 * a 96 px cell fits twelve 8 px characters, so "Appearance" and "Region and
 * language" truncated to `Appeara~` in the Control Panel — the cell had been
 * sized for a font the system stopped using in §M69.  Deriving the width from
 * the ICON plus a margin keeps the cell square-ish at any density, and the
 * label still truncates when it must, which is the honest behaviour for a
 * grid: the alternative is cells of different widths, and then nothing lines
 * up in either direction. */
#define G_PAD       cp_px(8)
#define G_GAP       cp_px(6)            /* icon to label                     */
/* THE CELL IS SIZED FROM THE ICON THAT IS ACTUALLY DRAWN.
 *
 * Reported from use: *"the selection box is about twice the meaningful content
 * (icon + title)."*  It was — and for a reason worth keeping: the cell was
 * built around a constant 48 px icon while `grid_draw` draws `cp_icon_size()`,
 * a §M63 SETTING whose default is 24.  So on a default desktop every cell
 * reserved room for an icon twice the size of the one in it, and the selection
 * wash — which fills the cell — advertised that empty space as part of the
 * item.  *A constant and a setting describing the same thing is a mismatch
 * waiting for somebody to change the setting.*
 *
 * Height is now exactly what the painter lays down: pad, icon, gap, one line
 * of label, pad.  Width keeps a floor, because a cell narrower than its labels
 * truncates every one of them — and the floor is the honest trade a grid makes
 * (cells of different widths line up in neither direction). */
#define G_CELL_H    (2 * G_PAD + cp_icon_size() + G_GAP + cp_fh())
#define G_CELL_W    (cp_icon_size() + 4 * G_PAD > cp_px(96) \
                     ? cp_icon_size() + 4 * G_PAD : cp_px(96))

/* Ceiling on the free-slot walk in g_place().  A layout pass must terminate
 * even if the model reports nonsense (every slot claimed, a `pos` that answers
 * differently on two calls): past this the item is placed wherever the walk
 * stopped, which is wrong on screen and cannot hang the compositor.  Sized
 * well above SHORTCUT_MAX so it is unreachable in normal use. */
#define SLOT_SCAN_MAX  4096

static int g_cols(int w) {
    int c = w / G_CELL_W;
    return c < 1 ? 1 : c;
}

/* Does any PLACED item sit in linear slot `s`?  (§M64 tail.)  Bounded by the
 * item count, and on a desktop where nothing has been dragged yet there is
 * nothing to scan — `m->pos` answers "not placed" for every item. */
static int slot_taken(const struct item_model* m, int cols, int s, int except) {
    int n = m->count ? m->count(m->ctx) : 0;
    for (int j = 0; j < n; j++) {
        if (j == except) continue;
        int c, r;
        if (m->pos(m->ctx, j, &c, &r) != 0) continue;
        if (c < 0) c = 0;
        if (c >= cols) c = cols - 1;
        if (r * cols + c == s) return 1;
    }
    return 0;
}

/* Where does an item go?  Its own slot if it has one, otherwise the first slot
 * no PLACED item has claimed, counting in model order.
 *
 * The skip is what stops a dragged icon and an auto-placed one from landing on
 * top of each other: flow order knows nothing about the slots somebody has
 * dragged things into, and two icons in one cell is not a layout, it is a lost
 * shortcut. */
static int g_place(const struct item_model* m, int i, int cols, int* oc, int* or_) {
    if (m && m->pos && m->pos(m->ctx, i, oc, or_) == 0) {
        /* Clamped rather than dropped: a column that no longer exists (a
         * narrower mode) must still be reachable — an icon hidden off the
         * right edge is indistinguishable from one that was deleted. */
        if (*oc < 0)      *oc = 0;
        if (*oc >= cols)  *oc = cols - 1;
        if (*or_ < 0)     *or_ = 0;
        return 0;
    }
    if (!m || !m->pos) {                       /* no positioning at all */
        *oc = i % cols; *or_ = i / cols;
        return 0;
    }
    /* Unplaced: rank among the other unplaced items, then walk the free slots. */
    int rank = 0;
    for (int j = 0; j < i; j++) {
        int c, r;
        if (m->pos(m->ctx, j, &c, &r) != 0) rank++;
    }
    int s = 0, seen = 0;
    for (;;) {
        if (!slot_taken(m, cols, s, i)) {
            if (seen == rank) break;
            seen++;
        }
        s++;
        if (s > SLOT_SCAN_MAX) break;          /* bounded — see below */
    }
    *oc = s % cols; *or_ = s / cols;
    return 0;
}

static int grid_rect(int i, int w, int h, const struct item_model* m, int scroll,
                     int* ox, int* oy, int* ow, int* oh) {
    int cols = g_cols(w);
    int c, r;
    if (m && m->pos) {
        g_place(m, i, cols, &c, &r);
        r -= scroll;
    } else {
        int idx = i - scroll;
        if (idx < 0) return -1;
        r = idx / cols; c = idx % cols;
    }
    if (r < 0) return -1;
    int y = r * G_CELL_H;
    if (y >= h) return -1;
    *ox = c * G_CELL_W; *oy = y; *ow = G_CELL_W; *oh = G_CELL_H;
    return 0;
}

/* §M69 — THE GRID SCROLLS IN ROWS, AND NOTHING KNEW THAT.
 *
 * Reported from use: *"the Control Panel scrolls although everything fits, and
 * scrolling makes most icons vanish — as if the bottom row jumped to the
 * top."*  Both halves are one bug.  `grid_rect` subtracts `scroll` from the
 * ROW index, while `w_itemview`'s wheel clamped to `count - 1` in ITEMS — so
 * one notch (three units) scrolled three ROWS off a two-row grid, and the
 * clamp allowed it because there were six items.  *A number is not a unit, and
 * two layers disagreeing about which one it is looks like a rendering bug.*
 *
 * Reporting the range here fixes both: the widget clamps in the view's own
 * unit, and when the content fits the answer is "no bar, nothing to scroll",
 * so the wheel does nothing at all. */
static int grid_scrollbar(int w, int h, const struct item_model* m, int scroll,
                          int* bx, int* by, int* bw, int* bh,
                          int* content, int* viewport) {
    (void)scroll;
    if (!m || !m->count) return 0;
    int n = m->count(m->ctx);
    int cols = g_cols(w);
    int rows_total = (n + cols - 1) / cols;
    int rows_fit = h / G_CELL_H;
    if (rows_fit < 1) rows_fit = 1;
    if (rows_total <= rows_fit) return 0;        /* everything fits */
    *bw = cp_scrollbar_w();
    *bx = w - *bw;
    *by = 0;
    *bh = h;
    *content = rows_total;
    *viewport = rows_fit;
    return 1;
}

static int grid_page(int w, int h) {
    int rows = h / G_CELL_H; if (rows < 1) rows = 1;
    return rows * g_cols(w);
}

static void grid_draw(struct gfx_surface* s, int x, int y, int w, int h,
                      const struct item_model* m, int sel, int scroll) {
    if (!m || !m->count || !m->get) return;
    if (draw_empty(s, x, y, w, h, m)) return;
    int n = m->count(m->ctx);
    /* With slots the model's order and the screen's order are different
     * things, so every item is offered to `grid_rect` and IT decides what is
     * off the top — starting at `scroll` would skip a placed item whose index
     * is low and whose row is high. */
    for (int i = m->pos ? 0 : scroll; i < n; i++) {
        int cx, cy, cw, ch;
        if (grid_rect(i, w, h, m, scroll, &cx, &cy, &cw, &ch) != 0) continue;
        struct item_entry e = { .icon = ICON_APP };
        if (m->get(m->ctx, i, &e) != 0) continue;

        cx += x; cy += y;
        if (i == sel) {
            gfx_blend_fill(s, cx + 2, cy + 2, cw - 4, ch - 4, SEL_FILL);
            gfx_line(s, cx + 2, cy + 2, cx + cw - 3, cy + 2, SEL_EDGE);
            gfx_line(s, cx + 2, cy + ch - 3, cx + cw - 3, cy + ch - 3, SEL_EDGE);
            gfx_line(s, cx + 2, cy + 2, cx + 2, cy + ch - 3, SEL_EDGE);
            gfx_line(s, cx + cw - 3, cy + 2, cx + cw - 3, cy + ch - 3, SEL_EDGE);
        }
        /* §M63 gui.icon_size.  Read per draw rather than cached: the setting
         * can change from the Appearance panel while the desktop is up, and a
         * cached copy would need its own invalidation path for no gain. */
        const int gi = cp_icon_size();
        icon_draw(s, cx + (cw - gi) / 2, cy + G_PAD, gi, e.icon);

        int maxch = (cw - 6) / cp_fw();
        int len   = str_len(e.label);
        if (len > maxch) len = maxch;
        /* THE SAME ARITHMETIC AS G_CELL_H, not a second guess at it: the
         * label used a literal 6 where the cell used G_GAP, so the two drifted
         * the moment either changed. */
        text_clipped(s, cx + centre_x(cw, len), cy + G_PAD + gi + G_GAP,
                     e.label, maxch, e.dim ? LBL_DIM : LBL_FG);
    }
}

static int grid_hit(int px, int py, int w, int h,
                    const struct item_model* m, int scroll) {
    if (!m || !m->count) return -1;
    if (px < 0 || py < 0 || px >= w || py >= h) return -1;

    /* With explicit slots the arithmetic below no longer holds — slot (2,0)
     * may belong to item 5 — so ask the same function that DREW them.  One
     * source of truth for "where is item i": a hit test that computes the
     * answer a second way is how a view ends up drawing correctly and
     * selecting the wrong thing (§M64 shipped `shortcut check` for exactly
     * this failure). */
    if (m->pos) {
        int n = m->count(m->ctx);
        for (int i = 0; i < n; i++) {
            int ox, oy, ow, oh;
            if (grid_rect(i, w, h, m, scroll, &ox, &oy, &ow, &oh) != 0) continue;
            if (px >= ox && px < ox + ow && py >= oy && py < oy + oh) return i;
        }
        return -1;
    }

    int cols = g_cols(w);
    int c = px / G_CELL_W, r = py / G_CELL_H;
    if (c >= cols) return -1;
    int idx = scroll + r * cols + c;
    return idx < m->count(m->ctx) ? idx : -1;
}

static int grid_slot_at(int px, int py, int w, int h, int* col, int* row) {
    if (px < 0 || py < 0 || px >= w || py >= h) return -1;
    int c = px / G_CELL_W, r = py / G_CELL_H;
    if (c >= g_cols(w)) return -1;
    *col = c; *row = r;
    return 0;
}

ITEM_VIEW(itemview_grid) = {
    .name = "grid",
    .draw = grid_draw,
    .hit  = grid_hit,
    .rect = grid_rect,
    .page = grid_page,
    .slot_at = grid_slot_at,        /* §M64 tail — the grid can be arranged */
    .scrollbar = grid_scrollbar,
};

/* ===================================================================== */
/* LIST — one item per row, small icon, label + optional sub-label.      */
/* ===================================================================== */

/* §M81 — THE ROW HEIGHT IS THE DENSITY'S, not a literal.
 *
 * It was a flat `40`, which is the design's comfort-density row in DESIGN
 * pixels — so at §M69's measured 137 % the list drew 40 px rows while every
 * other row in the tree was 44, and one wheel notch (`cp_row_h()` per line)
 * stopped a fraction of a row short each time.  The same class of defect §M69
 * swept out of nine windows, still standing in the shared view because nothing
 * had put a list next to a panel and looked. */
#define L_ICON      cp_px(28)
#define L_PAD       cp_px(8)

/* §M81 — THE ROW HEIGHT COMES FROM THE MODEL, falling back to the density's.
 *
 * It was a flat `40` — the design's comfort row in DESIGN pixels — so at
 * §M69's measured 137 % the list drew 40 px rows while every other row in the
 * tree was 44.  The same density-blind literal §M69 swept out of nine windows,
 * still standing in the shared view because nothing had put a list next to a
 * panel and looked.
 *
 * And it is the MODEL's answer rather than a constant because a menu and a file
 * list are both lists with different rows — see item_model.row_h. */
static int list_row_h(const struct item_model* m) {
    int h = (m && m->row_h) ? m->row_h(m->ctx) : 0;
    if (h > 0) return h;                /* the model asked for a height */

    /* §M81 — THE DEFAULT MUST FIT WHAT THIS VIEW DRAWS, and it did not.
     *
     * `list_draw` puts a SECOND LINE under the label when the model supplies
     * `sub`, which needs `2 * cp_fh() + 3` — about 47 px at §M69's measured
     * 137 % — while the row was a flat 40.  So every two-line row overran its
     * box and its sub-label was painted across the NEXT row's label: found by
     * PICTURE in the Control Panel's list mode, where "Theme, density, text and
     * icon size" sat on top of "System".
     *
     * *A view that draws more than its own row height is the layout bug a
     * measure exists to prevent* — and it survived because the only caller that
     * supplies `sub` is the Control Panel, whose default view is the grid.
     *
     * The floor applies to the DEFAULT only: a model that names its own height
     * gets it, because the Start menu's rows are one line and must stay short
     * enough for the whole menu to fit the panel strip. */
    int two = 2 * cp_fh() + cp_px(6);
    int row = cp_row_h();
    return row > two ? row : two;
}

static int list_rect(int i, int w, int h, const struct item_model* m, int scroll,
                     int* ox, int* oy, int* ow, int* oh) {
    int rh = list_row_h(m);
    int idx = i - scroll;
    if (idx < 0) return -1;
    int y = idx * rh;
    if (y >= h) return -1;
    *ox = 0; *oy = y; *ow = w; *oh = rh;
    return 0;
}

/* `page` has no model to ask, so it answers for the density's row — which is
 * what every caller of it draws with.  A model with its own row height sizes
 * itself through `height_for` instead. */
static int list_page(int w, int h) {
    (void)w;
    int rows = h / cp_row_h();
    return rows < 1 ? 1 : rows;
}

static void list_draw(struct gfx_surface* s, int x, int y, int w, int h,
                      const struct item_model* m, int sel, int scroll) {
    if (!m || !m->count || !m->get) return;
    if (draw_empty(s, x, y, w, h, m)) return;
    int n = m->count(m->ctx);
    for (int i = scroll; i < n; i++) {
        int cx, cy, cw, ch;
        if (list_rect(i, w, h, m, scroll, &cx, &cy, &cw, &ch) != 0) continue;
        struct item_entry e = { .icon = ICON_APP };
        if (m->get(m->ctx, i, &e) != 0) continue;

        cx += x; cy += y;
        /* §M81 — the group rule, from the MODEL's `group_start`.  Drawn on the
         * row's own top edge, so the divider and the row it divides cannot end
         * up computed from two different numbers.  Never above the first
         * visible row: a rule with nothing above it reads as the box's border. */
        if (e.group_start && i > scroll)
            gfx_fill(s, cx + L_PAD, cy, cw - 2 * L_PAD, 1,
                     cp_current_theme()->line);
        if (i == sel)
            gfx_blend_fill(s, cx + 1, cy + 1, cw - 2, ch - 2, SEL_FILL);

        /* Bounded by the row: the setting is a CEILING here, not a value.  A
         * 48 px icon in a 40 px row would paint over its neighbours. */
        int li = cp_icon_size();
        if (li > L_ICON) li = L_ICON;
        icon_draw(s, cx + L_PAD, cy + (ch - li) / 2, li, e.icon);

        int tx = cx + L_PAD + li + L_PAD;
        int maxch = (cw - (tx - cx) - L_PAD) / cp_fw();
        if (e.sub) {
            text_clipped(s, tx, cy + ch / 2 - cp_fh() - 1, e.label, maxch,
                         e.dim ? LBL_DIM : LBL_FG);
            text_clipped(s, tx, cy + ch / 2 + 2, e.sub, maxch, SUB_FG);
        } else {
            text_clipped(s, tx, cy + (ch - cp_fh()) / 2, e.label, maxch,
                         e.dim ? LBL_DIM : LBL_FG);
        }
    }
}

static int list_hit(int px, int py, int w, int h,
                    const struct item_model* m, int scroll) {
    if (!m || !m->count) return -1;
    if (px < 0 || py < 0 || px >= w || py >= h) return -1;
    int idx = scroll + py / list_row_h(m);
    return idx < m->count(m->ctx) ? idx : -1;
}

/* The list scrolls in ROWS too — same reasoning as the grid's above. */
static int list_scrollbar(int w, int h, const struct item_model* m, int scroll,
                          int* bx, int* by, int* bw, int* bh,
                          int* content, int* viewport) {
    (void)scroll;
    if (!m || !m->count) return 0;
    int n = m->count(m->ctx);
    int fit = h / list_row_h(m);
    if (fit < 1) fit = 1;
    if (n <= fit) return 0;
    *bw = cp_scrollbar_w();
    *bx = w - *bw;
    *by = 0;
    *bh = h;
    *content = n;
    *viewport = fit;
    return 1;
}

/* §M81 — the inverse of `page`: the box height that shows exactly `n` rows. */
static int list_height_for(int w, int n, const struct item_model* m) {
    (void)w;
    return n > 0 ? n * list_row_h(m) : 0;
}

ITEM_VIEW(itemview_list) = {
    .scrollbar = list_scrollbar,
    .height_for = list_height_for,
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

/* THE ROW HEIGHT IS THE DENSITY'S AND THE COLOURS ARE THE THEME'S.
 *
 * These were 18 and 20 with five literal colours next to them — 2007 pixels and
 * a palette belonging to no theme, which is the same defect §M69 fixed in the
 * listview: at the design's type sizes an 18 px row is about half of what the
 * spec gives (`row 40px` comfort / 32 compact), and a hardcoded header fill is
 * a dark bar across the light theme.  The table was simply the last widget
 * nobody had looked at. */
#define T_ROW_H   cp_row_h()
#define T_HEAD_H  (cp_fh() + cp_px(10))
#define T_PAD     cp_px(6)
#define T_MIN_COL (8 * cp_fw())     /* below this a column is useless */

static int t_style(const struct item_model* m, int c) {
    return m->col_style ? m->col_style(m->ctx, c) : 0;
}

/* A cell's width in the face it will actually be DRAWN in.  Measuring a
 * proportional string as `strlen * cp_fw()` is the habit the 8x8 font taught
 * (console_plate.h says so in capitals); getting it wrong here does not merely
 * misjudge a column, it puts a right-aligned number in the wrong place. */
static int t_text_w(const char* s, int style) {
    if (!(style & ICOL_MONO)) return cp_text_w(s);
    int n = 0;
    while (s[n]) n++;
    return n * cp_mono_cell_w();
}

/* §M69 — MARK A CUT CELL, the same as the header above and as the grid and
 * list have done since M22.  The table clipped hard, so a truncated value
 * ("no driver neede") read as data that was itself wrong rather than as data
 * that did not fit — the distinction the reader needs and cannot make from a
 * clipped glyph. */
static void t_draw_cell(struct gfx_surface* s, int x, int y, const char* str,
                        cp_color col, int style, int maxw) {
    char buf[64];
    int k = 0;
    while (str[k] && k < (int)sizeof buf - 1) { buf[k] = str[k]; k++; }
    buf[k] = 0;
    if (maxw > 0)
        while (k > 1 && t_text_w(buf, style) > maxw) { buf[--k - 1] = '~'; buf[k] = 0; }
    (style & ICOL_MONO ? cp_mono_text : cp_text)(s, x, y, buf, col);
}

static int t_rows_fit(int h) {
    int r = (h - T_HEAD_H) / T_ROW_H;
    return r < 1 ? 1 : r;
}

/* THE TABLE HAD NO SCROLLBAR AT ALL — the listview has had one since M22, and
 * the table simply never grew one, so a table with more rows than fit gave no
 * sign that there were any.  *A view that silently shows a prefix of its data
 * is worse than one that shows none, because nothing looks wrong.*  Returns the
 * width to reserve, 0 when everything is visible. */
static int t_scrollbar(const struct item_model* m, int h) {
    int total = m->count ? m->count(m->ctx) : 0;
    return (total > t_rows_fit(h)) ? cp_scrollbar_w() + 2 : 0;
}

/* §M69 — the bar's rect and what it scrolls, in the view's own coordinates.
 * Below the header band on purpose: a header that scrolled away with the rows
 * would take the column names with it, and then the numbers under them mean
 * nothing. */
static int table_scrollbar(int w, int h, const struct item_model* m, int scroll,
                           int* bx, int* by, int* bw, int* bh,
                           int* content, int* viewport) {
    (void)scroll;
    if (!m || !m->count) return 0;
    int total = m->count(m->ctx);
    int fit = t_rows_fit(h);
    if (total <= fit) return 0;
    *bw = cp_scrollbar_w();
    *bx = w - *bw;
    *by = T_HEAD_H;
    *bh = h - T_HEAD_H;
    *content = total;
    *viewport = fit;
    return 1;
}

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
#define T_GAP  (2 * cp_fw())        /* never let two columns touch */

/* §M76.5 — NARROW THE CLIP AND PUT IT BACK; NEVER CLEAR IT.
 *
 * `gfx_clear_clip` resets to the WHOLE SURFACE, so a view that clears after
 * clipping one cell has thrown away whatever its CALLER established — and the
 * caller is `widget_draw_all`, which had narrowed the surface to the damaged
 * rect.  From the second cell onward this table was painting **unclipped over
 * the entire window**, including rows whose background nobody had cleared.
 *
 * Text drawn on top of text is what that looks like: the same value rendered
 * BOLDER than its neighbours, in rows that change from one refresh to the next.
 * Reported from use as *"they flicker completely at random, I am doing
 * nothing"* — and it survived four attempts aimed at the damage path, all of
 * which changed WHICH rects were damaged while this went on painting all of
 * them regardless.
 *
 * §M65 wrote this rule down after `ui_text_clipped` did the same thing to a
 * scrolling container's viewport, and §M76.1 found the third instance in
 * `widget_draw_all` this same day.  *A clip is a stack discipline, and there is
 * no API here that enforces it* — so the shape is spelled out at each site. */
struct clip_save { int x0, y0, x1, y1; };
static struct clip_save clip_push(struct gfx_surface* s, int x, int y, int w, int h) {
    struct clip_save c = { s->clip_x0, s->clip_y0, s->clip_x1, s->clip_y1 };
    /* INTERSECT, because gfx_set_clip replaces: a cell must never be allowed to
     * paint outside the damage rect its caller narrowed the surface to. */
    int nx0 = x   > c.x0 ? x   : c.x0;
    int ny0 = y   > c.y0 ? y   : c.y0;
    int nx1 = x+w < c.x1 ? x+w : c.x1;
    int ny1 = y+h < c.y1 ? y+h : c.y1;
    if (nx1 < nx0) nx1 = nx0;
    if (ny1 < ny0) ny1 = ny0;
    gfx_set_clip(s, nx0, ny0, nx1 - nx0, ny1 - ny0);
    return c;
}
static void clip_pop(struct gfx_surface* s, struct clip_save c) {
    gfx_set_clip(s, c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0);
}

/* §M79.2 — a temporary two-ended probe.  The damage side and the paint side
 * each print their own numbers, because the whole question is whether they
 * agree — and four attempts at this artifact were made by reasoning about one
 * of them at a time. */
int  iv_probe_on(void);
void iv_probe(const char* fmt, ...);
extern unsigned iv_probe_seq;   /* bumped per refresh; pairs the two ends */

/* §M79.4 — THE COLUMN LAYOUT AS THE PAINTER SEES IT, and there is exactly one
 * way to ask for it.
 *
 * `table_draw` lays the columns out over `w - sb` — the width the SCROLLBAR does
 * not take — and `table_cell_rect` called `t_layout(m, w, ...)` directly,
 * missing the subtraction.  Same model, same nominal width, different column
 * geometry: the damage rect was computed for a layout the painter never used,
 * so a right-aligned cell that shrank left a strip of its old glyph outside
 * every rect anybody computed.
 *
 * §4.79's shape for the third time in this file's history — a painter and a
 * hit test computing one rectangle differently — and the reason the fix is a
 * SHARED FUNCTION rather than a copied `- sb`: the two cannot drift if there is
 * only one of them.
 *
 * It cost five wrong theories, and every one of them was possible because the
 * two ends were compared by reading rather than by printing the SAME expression
 * from both.  The probe that found it printed the function's PARAMETER `w`
 * instead of the ARGUMENT handed to `t_layout`, which is why they looked
 * identical for two more rounds — *an instrument that reports a neighbouring
 * value is worse than none, because it is believed.* */
static int t_layout(const struct item_model* m, int w, int* xs, int* ws);
static int t_scrollbar(const struct item_model* m, int h);

static int t_columns(const struct item_model* m, int w, int h, int* xs, int* ws) {
    return t_layout(m, w - t_scrollbar(m, h), xs, ws);
}

static int t_layout(const struct item_model* m, int w, int* xs, int* ws) {
    int n = t_cols(m);
    int avail = w - 2 * T_PAD;
    int nat[8];

    int rows = m->count ? m->count(m->ctx) : 0;
    if (rows > T_SCAN) rows = T_SCAN;
    for (int c = 0; c < n; c++) {
        int style = t_style(m, c);
        /* §M87 — a column TITLE is always an interface word (a cell may be
         * user data and is the model's to translate; a heading never is), so
         * the view translates it — measured and drawn from the same lookup. */
        const char* t = m->col_title ? lstr(m->col_title(m->ctx, c)) : "";
        /* The header is drawn in the ordinary face whatever the column is, so
         * it is measured in that one — a mono column with a proportional title
         * measured as mono comes out narrower than its own heading. */
        int longest = t ? cp_text_w(t) : 0;
        for (int i = 0; i < rows; i++) {
            char buf[64];
            item_cell_text(m, i, c, buf, (int)sizeof buf);
            int k = t_text_w(buf, style);
            if (k > longest) longest = k;
        }
        /* §M76.2 — QUANTISING A MONO COLUMN'S WIDTH WAS TRIED HERE AND
         * REVERTED, and the reason is worth more than the attempt.
         *
         * The problem it aimed at is real and measured: a `TIME` value crossing
         * 9999 -> 10000 widens its column, shifts every column after it and
         * forces a full-table repaint — **twice in 25 seconds** on an idle
         * desktop, because twenty rows all growing cross digit boundaries
         * constantly.  Rounding the width up to whole 4-character steps did
         * remove them completely (`colmove 0` over the same 25 s).
         *
         * NB: it was attempted while chasing a reported flicker and IS NOT its
         * cause — that was §M76.5's cleared clip.  The re-layout churn is a
         * separate, genuine cost that this would address.
         *
         * IT ALSO MADE THE TABLE WORSE, WHICH ONLY THE PICTURE SHOWED.  Wider
         * columns pushed the total past the available width, so `t_layout`'s
         * squeeze-and-drop logic took over and the TIME column began
         * TRUNCATING: `29821 ~` where `29821 ms` belonged.  *A column that is
         * stable and unreadable is not an improvement on one that moves.*
         *
         * The honest fix is not here: a column's width should be decided ONCE
         * and kept, which is per-instance STATE, and this view is deliberately
         * stateless (§M64 — selection and scroll live in the viewer so views
         * stay stateless).  Giving the widget a sticky per-column width is the
         * change, and it is a change to the item-view contract rather than to
         * this function. */
        nat[c] = longest + T_GAP;
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

    /* §M77 — SPEND SLACK ON MAKING MONO COLUMNS STOP MOVING.
     *
     * A column sized from its content resizes as its content grows, and every
     * resize shifts every column after it and forces a full-table repaint —
     * **measured at twice in 25 seconds** on an idle desktop, because twenty
     * rows of milliseconds cross digit boundaries constantly.  That is a
     * visible flash, and it is the last measured cost in this table.
     *
     * Rounding mono columns up to whole character cells removes it.  Doing so
     * UNCONDITIONALLY was tried and reverted: it pushed the total past the
     * available width, the squeeze-and-drop logic took over, and the TIME
     * column began truncating (`29821 ~`).  *A column that is stable and
     * unreadable is not an improvement on one that moves.*
     *
     * SO IT IS SPENT OUT OF SLACK THAT ALREADY EXISTS, and never borrowed:
     * each mono column is widened to the next 4-character boundary only while
     * the remaining slack covers it, and a table with no room to spare keeps
     * exactly the layout it had before.  The stability is best where there is
     * room for it and costs nothing where there is not — which is the right way
     * round, because a cramped table's own width is already changing for
     * bigger reasons.
     *
     * Four characters rather than one: a number crossing a digit boundary is
     * the common case, and a one-character step would still move for every one
     * of them. */
    if (total_nat < avail) {
        int cell = cp_cell_w();
        int slack = avail - total_nat;
        for (int c = 0; c < n && cell > 0 && slack > 0; c++) {
            if (!(t_style(m, c) & ICOL_MONO)) continue;
            int chars   = (nat[c] - T_GAP + cell - 1) / cell;
            int rounded = ((chars + 3) / 4) * 4 * cell + T_GAP;
            int extra   = rounded - nat[c];
            if (extra <= 0 || extra > slack) continue;   /* never borrow */
            nat[c] = rounded;
            slack -= extra;
            total_nat += extra;
        }
    }

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
void item_cell_text(const struct item_model* m, int i, int c, char* out, int cap) {
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
    if (draw_empty(s, x, y, w, h, m)) return;
    const cp_theme* t = cp_current_theme();
    int xs[8], ws[8];
    /* THE COLUMNS GET THE WIDTH THE SCROLLBAR DOES NOT.  Laying them out over
     * the full width and then painting a bar on top clips the last column by
     * exactly the bar — which is invisible until a value happens to be long. */
    int n = t_columns(m, w, h, xs, ws);

    /* Header — drawn from the model, not from a caller's padded string.  The
     * file manager used to fake this with spaces in a label, which is exactly
     * the kind of thing that stops being aligned the moment a name is long. */
    gfx_fill(s, x, y, w, T_HEAD_H, t->tray);
    for (int c = 0; c < n; c++) {
        const char* ct = m->col_title ? lstr(m->col_title(m->ctx, c)) : "";
        if (!ct) continue;
        int style = t_style(m, c);
        int cw = ws[c] - T_GAP;
        /* THE HEADING SITS OVER ITS OWN COLUMN, INCLUDING ITS ALIGNMENT.  A
         * right-aligned number column with a left-aligned heading reads as two
         * different columns, which is exactly how the old padded-string header
         * looked once the real typeface arrived. */
        /* §M69 — A CUT HEADING IS MARKED, not merely clipped.  With more
         * columns than fit, the widths are scaled down and a heading can end
         * up narrower than its own word — and a hard clip mid-glyph produced
         * `Isolatior`, which reads as a MISSPELLING rather than as a
         * truncation.  *The reader has to be able to tell a short label from a
         * wrong one.*  The cells have marked their cuts with `~` since M22;
         * the header simply never did. */
        char hbuf[32];
        {
            int k = 0;
            while (ct[k] && k < (int)sizeof hbuf - 1) { hbuf[k] = ct[k]; k++; }
            hbuf[k] = 0;
            while (k > 1 && cp_text_w(hbuf) > cw) { hbuf[--k - 1] = '~'; hbuf[k] = 0; }
        }
        int hx = x + xs[c];
        if (style & ICOL_RIGHT) hx += cw - cp_text_w(hbuf);
        struct clip_save hc = clip_push(s, x + xs[c], y, cw, T_HEAD_H);
        cp_text(s, hx, y + (T_HEAD_H - cp_fh()) / 2, hbuf, t->muted);
        clip_pop(s, hc);
    }
    gfx_fill(s, x, y + T_HEAD_H - 1, w, 1, t->line);

    int total = m->count(m->ctx);
    int rows  = (h - T_HEAD_H) / T_ROW_H;
    for (int r = 0; r < rows; r++) {
        int i = scroll + r;
        if (i >= total) break;
        int ry = y + T_HEAD_H + r * T_ROW_H;
        /* A HAIRLINE BETWEEN ROWS, NOT ALTERNATING FILLS — the design's answer,
         * and the same one the listview got: banding would compete with the
         * selection for the one visual job the selection has.  No rule above
         * the first row; the header's own rule is already there. */
        if (i == sel)   gfx_fill(s, x, ry, w, T_ROW_H, t->sel_bg);
        else if (r)     gfx_fill(s, x, ry, w, 1, t->line_soft);

        for (int c = 0; c < n; c++) {
            char buf[64];
            item_cell_text(m, i, c, buf, (int)sizeof buf);
            if (!buf[0]) continue;
            int style = t_style(m, c);
            int cw = ws[c] - T_GAP;

            /* THE SELECTION OVERRIDES THE COLUMN'S ROLE COLOUR.  An accent PID
             * drawn on the accent selection fill is invisible — the one row the
             * user has chosen would be the one row that cannot be read. */
            cp_color col = t->text;
            if (i == sel)              col = t->sel_fg;
            else if (style & ICOL_ACCENT) col = t->accent;
            else if (style & ICOL_DIM)    col = t->muted;

            int tx = x + xs[c];
            if (style & ICOL_RIGHT) {
                int tw = t_text_w(buf, style);
                if (tw < cw) tx += cw - tw;   /* never push it out to the left */
            }
            /* Clip each cell to its own column: a long name must not run into
             * the size column, which is the failure a padded string cannot
             * even detect. */
            /* §M79.2 — TEMPORARY PROBE: the paint's own numbers, printed only
             * when a right-aligned cell's width CHANGES, which is the one
             * moment the artifact appears. */
            /* §M79.4 — PRINT EVERY TIME, for one column only.  The previous
             * version printed only when the width CHANGED, which meant the
             * damage line and the paint line could come from different refresh
             * cycles — and comparing two unpaired samples is what produced the
             * §M79.2 reading that the two ends disagree.  *A filter on one end
             * of a two-ended probe makes the pair meaningless.* */
            if (c == 3 && i == scroll && iv_probe_on())
                iv_probe("[%u] paint col%d W=%d n=%d xs=%d ws=%d tx=%d tw=%d\n",
                         iv_probe_seq, c, w, n, xs[c], ws[c], tx - x,
                         t_text_w(buf, style));
            struct clip_save cc = clip_push(s, x + xs[c], ry, cw, T_ROW_H);
            t_draw_cell(s, tx, ry + (T_ROW_H - cp_fh()) / 2, buf, col, style, cw);
            clip_pop(s, cc);
        }
    }

    /* §M69 — THE BAR IS NOT PAINTED HERE ANY MORE.  A view is stateless by
     * construction (itemview.h), and a scrollbar has state the moment it can be
     * used: which part is held, where inside the thumb the press landed.  So
     * the view reserves the strip and REPORTS it (`.scrollbar`), and the widget
     * — which owns `scroll` and the grab — paints it through scrollbar.c.
     * The old copy here drew a thumb that nothing could touch. */
    /* the strip is reserved inside t_columns now; the widget paints the bar */
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

/* §M75.2 — one CELL's box, so the content diff can damage a changed number
 * instead of the whole row it sits in.  See itemview.h for why.
 *
 * The geometry is `table_rect`'s row band intersected with `t_layout`'s column
 * — the SAME `t_layout` the painter calls, which is what stops the damaged box
 * and the painted box from drifting apart (§4.79's title buttons, where a
 * painter and a hit test computed the same rectangle differently and the top
 * edge of every button was dead).
 *
 * The band is widened by a pixel each side: a proportional glyph may overhang
 * its measured advance, and a cell damaged exactly to its computed width can
 * leave a column of stale pixels down the edge — visible as a faint seam that
 * nothing ever repaints. */
static int table_cell_rect(int i, int col, int w, int h, const struct item_model* m,
                           int scroll, int* ox, int* oy, int* ow, int* oh) {
    int rx, ry, rw, rh;
    if (table_rect(i, w, h, m, scroll, &rx, &ry, &rw, &rh) != 0) return -1;

    int xs[8], ws[8];
    int n = t_columns(m, w, h, xs, ws);      /* §M79.4 — the PAINTER's layout */
    if (col < 0 || col >= n) return -1;
    if (iv_probe_on())
        iv_probe("[%u] rect  col%d W=%d n=%d xs=%d ws=%d\n", iv_probe_seq, col, w, n, xs[col], ws[col]);

    int x0 = xs[col] - 1;
    int x1 = xs[col] + ws[col] + 1;
    if (x0 < rx) x0 = rx;
    if (x1 > rx + rw) x1 = rx + rw;
    if (x1 <= x0) return -1;

    *ox = x0; *oy = ry; *ow = x1 - x0; *oh = rh;
    return 0;
}

static int table_page(int w, int h) {
    (void)w;
    int rows = (h - T_HEAD_H) / T_ROW_H;
    return rows < 1 ? 1 : rows;
}

ITEM_VIEW(itemview_table) = {
    .scrollbar = table_scrollbar,
    .name = "table",
    .draw = table_draw,
    .hit  = table_hit,
    .rect = table_rect,
    .page = table_page,
    .cell_rect = table_cell_rect,      /* §M75.2 — per-cell damage */
};
