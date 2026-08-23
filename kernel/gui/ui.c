/* =============================================================================
 * ui.c — the toolkit spine: class registry, spec builder, layout (§M65).
 *
 * See ui.h for WHY this exists.  The short version: the old toolkit could only
 * be told where to put things in pixels, so every panel computed its own
 * geometry by hand and none of them survived a resolution change.
 *
 * THE LAYOUT IS TWO PASSES, and that is the whole algorithm:
 *
 *   measure  — bottom-up.  Every node reports (min_w, pref_w, pref_h) for the
 *              width it is being offered.  A container sums its children along
 *              its axis and takes the maximum across it.
 *   arrange  — top-down.  A container hands each child its preferred size,
 *              then shares whatever is left over in proportion to `weight`.
 *
 * There is no constraint solver and no second pass over the same node: a
 * layout that needs iteration to settle is a layout whose result nobody can
 * predict, and this one runs on the app-host task with a window's worth of
 * widgets, not a document's.
 *
 * THREADING: everything here runs on the owning app-host task (window build,
 * resize, event dispatch).  Nothing in this file may be called from an IRQ —
 * it allocates.
 * ============================================================================= */

#include "ui.h"
#include "widget.h"
#include "gui.h"
#include "gui_internal.h"   /* gui_wm_focused — "this panel" means the focused one */
#include "gfx.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include "config.h"
#include <stddef.h>

/* Registry bounds — see linker-<arch>.ld for the section. */
extern const struct widget_class* const __start_ui_classes[];
extern const struct widget_class* const __stop_ui_classes[];

int ui_class_count(void) {
    return (int)(__stop_ui_classes - __start_ui_classes);
}

const struct widget_class* ui_class_at(int index) {
    if (index < 0 || index >= ui_class_count()) return NULL;
    return __start_ui_classes[index];
}

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const struct widget_class* ui_class_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < ui_class_count(); i++) {
        const struct widget_class* c = ui_class_at(i);
        if (c && streq(c->name, name)) return c;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Per-window state.
 * ------------------------------------------------------------------------- */

#define UI_MAX_NODES 64                 /* per window; a panel, not a document */
#define UI_PAD        8                 /* window edge inset, px               */
#define UI_GAP        6                 /* between siblings, px                */

struct ui_node {
    struct widget* w;                   /* NULL for a pure container           */
    int scroll;                         /* UI_SCROLL: offset into the content  */
    int content_h;                      /* UI_SCROLL: measured child height    */
    const struct widget_class* cls;
    int id, parent;
    int weight, flags;
    int min_w, pref_w, pref_h;          /* filled by measure                   */
    int x, y, cw, ch;                   /* filled by arrange                   */
    int hidden;                         /* dropped by a size-class rule        */
};

struct ui_state {
    struct ui_node n[UI_MAX_NODES];
    int  count;
    ui_event_fn on_event;
    void* ctx;
    struct widget* popup_src;           /* who opened the popup, if any      */
    /* The clip in force while ARRANGING a subtree — a scrolling container sets
     * it around its children so every widget BENEATH it inherits the viewport.
     * The first version set the clip only on the container's direct children,
     * and a grid inside a viewport has none: its labels scrolled straight out
     * over the panel's title.  A clip that does not descend is not a clip. */
    int clip_x, clip_y, clip_w, clip_h;
};

static struct ui_state* state_of(struct gui_window* win) {
    return (struct ui_state*)gui_window_ui(win);
}

int ui_scroll_by(struct gui_window* win, int id, int dl) {
    struct ui_state* st = state_of(win);
    if (!st) return 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].id != id || !(st->n[i].flags & UI_SCROLL)) continue;
        int before = st->n[i].scroll;
        st->n[i].scroll += dl;
        if (st->n[i].scroll < 0) st->n[i].scroll = 0;
        ui_layout(win);                 /* clamps against the current viewport */
        return st->n[i].scroll != before;
    }
    return 0;
}

/* The wheel: find the scrolling container under (x,y) and move it.  A
 * container is not a widget, so this cannot ride the widget scroll op — and
 * routing by POSITION rather than by focus is what every toolkit does and what
 * the hand expects (§M61's wheel lesson). */
int ui_scroll_at(struct gui_window* win, int x, int y, int dz) {
    struct ui_state* st = state_of(win);
    if (!st) return 0;
    for (int i = 0; i < st->count; i++) {
        struct ui_node* nd = &st->n[i];
        if (!(nd->flags & UI_SCROLL)) continue;
        if (x < nd->x || x >= nd->x + nd->cw) continue;
        if (y < nd->y || y >= nd->y + nd->ch) continue;
        return ui_scroll_by(win, nd->id, dz > 0 ? -48 : 48);
    }
    return 0;
}

int ui_node_count(struct gui_window* win) {
    struct ui_state* st = state_of(win);
    return st ? st->count : 0;
}

struct widget* ui_by_id(struct gui_window* win, int id) {
    struct ui_state* st = state_of(win);
    if (!st || id == 0) return NULL;
    for (int i = 0; i < st->count; i++)
        if (st->n[i].id == id) return st->n[i].w;
    return NULL;
}

void ui_emit(struct widget* w, int type, int value) {
    if (!w || !w->win) return;
    struct ui_state* st = state_of(w->win);
    if (!st || !st->on_event) return;
    /* Find the id this widget was built with.  A linear walk over at most 64
     * entries, on a user-visible event — the alternative is a back-pointer in
     * `struct widget`, which is a field every old constructor would have to
     * learn to initialise. */
    for (int i = 0; i < st->count; i++)
        if (st->n[i].w == w) { st->on_event(w->win, st->n[i].id, type, value, st->ctx); return; }
}

void ui_popup_from(struct widget* w, int tag) {
    (void)tag;
    if (!w || !w->win) return;
    struct ui_state* st = state_of(w->win);
    if (st) st->popup_src = w;
}

void ui_dispatch_popup(struct gui_window* win, int row, int tag) {
    struct ui_state* st = state_of(win);
    if (!st || !st->popup_src) return;
    struct widget* w = st->popup_src;
    st->popup_src = NULL;               /* one answer per opening */
    for (int i = 0; i < st->count; i++)
        if (st->n[i].w == w) {
            if (st->n[i].cls && st->n[i].cls->popup_pick)
                st->n[i].cls->popup_pick(w, tag, row);
            return;
        }
}

/* ---------------------------------------------------------------------------
 * Size classes — in CELLS, see ui.h.
 * ------------------------------------------------------------------------- */

int ui_size_class_for(int content_px_w) {
    int cells = content_px_w / GFX_GLYPH_W;
    if (cells < 60)  return UI_SIZE_COMPACT;
    if (cells < 120) return UI_SIZE_REGULAR;
    return UI_SIZE_WIDE;
}

int ui_size_class(const struct gui_window* win) {
    int w = 0, h = 0;
    gui_window_content_size((struct gui_window*)win, &w, &h);
    return ui_size_class_for(w);
}

/* ---------------------------------------------------------------------------
 * Measure.
 * ------------------------------------------------------------------------- */

static int is_container(const struct ui_node* nd) { return nd->w == NULL; }

static void measure_node(struct ui_state* st, int idx, int avail_w, int size_class);

/* Width of a two-column grid's FIRST column: the widest label, clamped so a
 * long key cannot squeeze the controls out of existence. */
static int grid_col0(struct ui_state* st, struct ui_node* nd, int avail_w,
                     int size_class) {
    int col0 = 0, i = 0, seen = 0;
    for (i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        if ((seen++ & 1) == 0) {                 /* even child = label column */
            measure_node(st, i, avail_w, size_class);
            if (st->n[i].pref_w > col0) col0 = st->n[i].pref_w;
        }
    }
    int cap = avail_w * 45 / 100;
    if (col0 > cap) col0 = cap;
    if (col0 < GFX_GLYPH_W * 6) col0 = GFX_GLYPH_W * 6;
    return col0;
}

static void measure_grid(struct ui_state* st, int idx, int avail_w,
                         int size_class) {
    struct ui_node* nd = &st->n[idx];
    int stacked = (size_class == UI_SIZE_COMPACT);
    int col0 = stacked ? 0 : grid_col0(st, nd, avail_w, size_class);
    int rest = stacked ? avail_w : avail_w - col0 - UI_GAP;
    if (rest < GFX_GLYPH_W * 6) rest = GFX_GLYPH_W * 6;

    int total_h = 0, seen = 0, row_h = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id) continue;
        struct ui_node* c = &st->n[i];
        c->hidden = (c->flags & UI_HIDE_COMPACT) && size_class == UI_SIZE_COMPACT;
        if (c->hidden) continue;
        int even = (seen & 1) == 0;
        measure_node(st, i, even ? (stacked ? avail_w : col0) : rest, size_class);
        if (stacked) {
            total_h += c->pref_h + (even ? 2 : UI_GAP);
        } else {
            if (c->pref_h > row_h) row_h = c->pref_h;
            if (!even) { total_h += row_h + UI_GAP; row_h = 0; }
        }
        seen++;
    }
    if (!stacked && (seen & 1)) total_h += row_h + UI_GAP;   /* dangling label */
    if (total_h > 0) total_h -= UI_GAP;

    nd->pref_w = avail_w;
    nd->pref_h = total_h;
    nd->min_w  = avail_w;
}

static void measure_children(struct ui_state* st, int idx, int avail_w,
                             int size_class) {
    struct ui_node* nd = &st->n[idx];
    if (nd->flags & UI_SCROLL) {
        /* A viewport asks for whatever it is given and keeps its content's
         * height separately: reporting the CONTENT height would make the
         * parent grow to fit it, which is the opposite of scrolling. */
        int total = 0, nvis = 0;
        for (int i = 0; i < st->count; i++) {
            if (st->n[i].parent != nd->id) continue;
            struct ui_node* c = &st->n[i];
            c->hidden = (c->flags & UI_HIDE_COMPACT) && size_class == UI_SIZE_COMPACT;
            if (c->hidden) continue;
            measure_node(st, i, avail_w, size_class);
            total += c->pref_h;
            nvis++;
        }
        if (nvis > 1) total += UI_GAP * (nvis - 1);
        nd->content_h = total;
        nd->pref_w = avail_w;
        nd->pref_h = GFX_GLYPH_H * 4;      /* a floor; weight gives it the rest */
        nd->min_w  = avail_w;
        return;
    }
    if (nd->flags & UI_GRID) { measure_grid(st, idx, avail_w, size_class); return; }
    int row = (nd->flags & UI_ROW) != 0;
    /* A row that was told to wrap becomes a column when the window is narrow —
     * this single rule is most of what "responsive" means here. */
    if (row && (nd->flags & UI_WRAP_COMPACT) && size_class == UI_SIZE_COMPACT)
        row = 0;

    int main = 0, cross = 0, nvis = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id) continue;
        struct ui_node* c = &st->n[i];
        c->hidden = (c->flags & UI_HIDE_COMPACT) && size_class == UI_SIZE_COMPACT;
        if (c->hidden) continue;
        int child_avail = row ? avail_w : avail_w;   /* refined during arrange */
        measure_node(st, i, child_avail, size_class);
        nvis++;
        if (row) {
            main  += c->pref_w;
            if (c->pref_h > cross) cross = c->pref_h;
        } else {
            main  += c->pref_h;
            if (c->pref_w > cross) cross = c->pref_w;
        }
    }
    if (nvis > 1) main += UI_GAP * (nvis - 1);

    nd->pref_w = row ? main  : cross;
    nd->pref_h = row ? cross : main;
    nd->min_w  = nd->pref_w;
}

static void measure_node(struct ui_state* st, int idx, int avail_w,
                         int size_class) {
    struct ui_node* nd = &st->n[idx];
    if (is_container(nd)) { measure_children(st, idx, avail_w, size_class); return; }

    if (nd->cls && nd->cls->measure) {
        nd->cls->measure(nd->w, avail_w, &nd->min_w, &nd->pref_w, &nd->pref_h);
    } else {
        /* No measure op: the widget keeps whatever size its constructor gave
         * it.  That is what lets the M22 controls take part in a laid-out
         * window without being rewritten. */
        nd->min_w = nd->pref_w = nd->w->w;
        nd->pref_h = nd->w->h;
    }
    if (nd->pref_w < 1) nd->pref_w = 1;
    if (nd->pref_h < 1) nd->pref_h = 1;
}

/* ---------------------------------------------------------------------------
 * Arrange.
 * ------------------------------------------------------------------------- */

static void arrange_node(struct ui_state* st, int idx, int x, int y, int w, int h,
                         int size_class);

static void place_widget(struct ui_state* st, struct ui_node* nd,
                         int x, int y, int w, int h) {
    nd->x = x; nd->y = y; nd->cw = w; nd->ch = h;
    if (!nd->w) return;
    nd->w->clip_x = st->clip_x; nd->w->clip_y = st->clip_y;
    nd->w->clip_w = st->clip_w; nd->w->clip_h = st->clip_h;
    if (nd->hidden) {
        /* Off-surface rather than zero-sized: a zero-width widget still draws
         * its text (the M22 controls do not clip themselves), and a hit test
         * on a zero rect is fine but a stray glyph is not. */
        nd->w->x = -10000; nd->w->y = -10000;
        nd->w->w = 1; nd->w->h = 1;
        return;
    }
    nd->w->x = x; nd->w->y = y; nd->w->w = w; nd->w->h = h;
}

static void arrange_grid(struct ui_state* st, int idx, int x, int y,
                         int w, int h, int size_class) {
    (void)h;
    struct ui_node* nd = &st->n[idx];
    int stacked = (size_class == UI_SIZE_COMPACT);
    int col0 = stacked ? 0 : grid_col0(st, nd, w, size_class);
    int rest = stacked ? w : w - col0 - UI_GAP;

    int cur = y, seen = 0, row_h = 0, label_idx = -1;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        struct ui_node* c = &st->n[i];
        int even = (seen & 1) == 0;
        if (stacked) {
            arrange_node(st, i, x, cur, even ? w : w, c->pref_h, size_class);
            cur += c->pref_h + (even ? 2 : UI_GAP);
        } else if (even) {
            label_idx = i;
            row_h = c->pref_h;
        } else {
            if (c->pref_h > row_h) row_h = c->pref_h;
            /* The label is centred against a possibly taller control: a
             * caption sitting at the top of a three-row radio group reads as
             * belonging to the row above it. */
            if (label_idx >= 0) {
                struct ui_node* l = &st->n[label_idx];
                int ly = cur + (row_h - l->pref_h) / 2;
                arrange_node(st, label_idx, x, ly, col0, l->pref_h, size_class);
            }
            arrange_node(st, i, x + col0 + UI_GAP, cur, rest, c->pref_h, size_class);
            cur += row_h + UI_GAP;
            label_idx = -1;
        }
        seen++;
    }
    if (!stacked && label_idx >= 0) {
        struct ui_node* l = &st->n[label_idx];
        arrange_node(st, label_idx, x, cur, col0, l->pref_h, size_class);
    }
}

/* Lay a scrolling container's children out as a column, offset by its scroll
 * position, and clip every one of them to the VIEWPORT (not to its own box):
 * a child straddling the edge must be cut there. */
static void arrange_scroll(struct ui_state* st, int idx, int x, int y,
                           int w, int h, int size_class) {
    struct ui_node* nd = &st->n[idx];

    int content = 0, nvis = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        content += st->n[i].pref_h;
        nvis++;
    }
    if (nvis > 1) content += UI_GAP * (nvis - 1);
    nd->content_h = content;

    int max_scroll = content - h;
    if (max_scroll < 0) max_scroll = 0;
    if (nd->scroll > max_scroll) nd->scroll = max_scroll;
    if (nd->scroll < 0) nd->scroll = 0;

    /* The viewport is in force for the WHOLE subtree, not just the direct
     * children — see the note on ui_state.clip_*. */
    int sx = st->clip_x, sy = st->clip_y, sw = st->clip_w, sh = st->clip_h;
    st->clip_x = x; st->clip_y = y; st->clip_w = w; st->clip_h = h;

    int cur = y - nd->scroll;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        struct ui_node* c = &st->n[i];
        arrange_node(st, i, x, cur, w, c->pref_h, size_class);
        cur += c->pref_h + UI_GAP;
    }

    st->clip_x = sx; st->clip_y = sy; st->clip_w = sw; st->clip_h = sh;
}

static void arrange_children(struct ui_state* st, int idx, int x, int y,
                             int w, int h, int size_class) {
    struct ui_node* nd = &st->n[idx];
    if (nd->flags & UI_SCROLL) { arrange_scroll(st, idx, x, y, w, h, size_class); return; }
    if (nd->flags & UI_GRID) { arrange_grid(st, idx, x, y, w, h, size_class); return; }
    int row = (nd->flags & UI_ROW) != 0;
    if (row && (nd->flags & UI_WRAP_COMPACT) && size_class == UI_SIZE_COMPACT)
        row = 0;

    int nvis = 0, fixed = 0, weight_sum = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        nvis++;
        fixed += row ? st->n[i].pref_w : st->n[i].pref_h;
        weight_sum += st->n[i].weight;
    }
    if (nvis == 0) return;

    int gaps  = UI_GAP * (nvis - 1);
    int space = (row ? w : h) - fixed - gaps;
    if (space < 0) space = 0;

    int cur = row ? x : y;
    int used_extra = 0, seen = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        struct ui_node* c = &st->n[i];
        seen++;
        int extra = 0;
        if (weight_sum > 0 && c->weight > 0) {
            extra = space * c->weight / weight_sum;
            /* The LAST weighted child absorbs the rounding remainder, so a
             * three-way split never leaves a one-pixel gap at the edge. */
            if (seen == nvis) extra = space - used_extra;
            used_extra += extra;
        }
        int cwid, chei, cx, cy;
        if (row) {
            cwid = c->pref_w + extra;
            chei = (c->flags & UI_FILL_W) ? h : (c->pref_h < h ? c->pref_h : h);
            cx = cur; cy = y;
            cur += cwid + UI_GAP;
        } else {
            chei = c->pref_h + extra;
            cwid = (c->flags & UI_FILL_W) ? w : (c->pref_w < w ? c->pref_w : w);
            cx = x; cy = cur;
            cur += chei + UI_GAP;
        }
        arrange_node(st, i, cx, cy, cwid, chei, size_class);
    }
}

static void arrange_node(struct ui_state* st, int idx, int x, int y, int w, int h,
                         int size_class) {
    place_widget(st, &st->n[idx], x, y, w, h);
    if (is_container(&st->n[idx]))
        arrange_children(st, idx, x, y, w, h, size_class);
}

void ui_layout(struct gui_window* win) {
    struct ui_state* st = state_of(win);
    if (!st || st->count == 0) return;

    int cw = 0, ch = 0;
    gui_window_content_size(win, &cw, &ch);
    if (cw <= 2 * UI_PAD || ch <= 2 * UI_PAD) return;

    st->clip_x = st->clip_y = st->clip_w = st->clip_h = 0;   /* no clip at the root */
    int sc = ui_size_class_for(cw);
    int inner_w = cw - 2 * UI_PAD, inner_h = ch - 2 * UI_PAD;

    /* Root nodes are the ones whose parent id names no node — measured and
     * arranged as if they were children of one implicit column. */
    for (int i = 0; i < st->count; i++) st->n[i].hidden = 0;

    /* The implicit root is node -1: emulate it by measuring/arranging every
     * top-level node in a column.  Keeping it implicit means an app does not
     * have to declare a container it never thinks about. */
    int main = 0, nvis = 0;
    for (int i = 0; i < st->count; i++) {
        if (ui_by_id(win, st->n[i].parent) || st->n[i].parent != 0) continue;
        struct ui_node* c = &st->n[i];
        c->hidden = (c->flags & UI_HIDE_COMPACT) && sc == UI_SIZE_COMPACT;
        if (c->hidden) continue;
        measure_node(st, i, inner_w, sc);
        main += c->pref_h;
        nvis++;
    }
    if (nvis > 1) main += UI_GAP * (nvis - 1);

    int space = inner_h - main;
    if (space < 0) space = 0;
    int weight_sum = 0;
    for (int i = 0; i < st->count; i++)
        if (st->n[i].parent == 0 && !st->n[i].hidden) weight_sum += st->n[i].weight;

    int cur = UI_PAD, used = 0, seen = 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != 0 || st->n[i].hidden) continue;
        struct ui_node* c = &st->n[i];
        seen++;
        int extra = 0;
        if (weight_sum > 0 && c->weight > 0) {
            extra = space * c->weight / weight_sum;
            if (seen == nvis) extra = space - used;
            used += extra;
        }
        int hgt = c->pref_h + extra;
        arrange_node(st, i, UI_PAD, cur, inner_w, hgt, sc);
        cur += hgt + UI_GAP;
    }
}

/* ---------------------------------------------------------------------------
 * Build.
 * ------------------------------------------------------------------------- */

int ui_build(struct gui_window* win, const struct ui_spec* specs, int n,
             ui_event_fn on_event, void* ctx) {
    if (!win || !specs || n <= 0) return 0;

    struct ui_state* st = state_of(win);
    if (!st) {
        st = (struct ui_state*)kcalloc(1, sizeof *st);
        if (!st) return 0;
        gui_window_set_ui(win, st);
    }
    st->on_event = on_event;
    st->ctx = ctx;

    int built = 0;
    for (int i = 0; i < n && st->count < UI_MAX_NODES; i++) {
        const struct ui_spec* sp = &specs[i];
        struct ui_node* nd = &st->n[st->count];
        nd->id     = sp->id;
        nd->parent = sp->parent;
        nd->weight = sp->weight;
        nd->flags  = sp->flags;
        nd->hidden = 0;

        if (streq(sp->cls, "box")) {
            /* A container is a NODE with no widget: it draws nothing, cannot
             * be hit and costs no allocation.  Making it a widget would mean a
             * transparent widget on every hit test for no benefit. */
            nd->w = NULL; nd->cls = NULL;
            st->count++; built++;
            continue;
        }

        const struct widget_class* cls = ui_class_find(sp->cls);
        if (!cls || !cls->create) {
            /* Report and SKIP.  A panel missing one control is more useful
             * than a window that refuses to open, and the name is printed so
             * the typo is findable. */
            kprintf("ui: unknown widget class '%s' (id %d) — skipped\n",
                    sp->cls ? sp->cls : "(null)", sp->id);
            continue;
        }
        struct widget* w = cls->create(win, sp);
        if (!w) { kprintf("ui: class '%s' failed to build id %d\n", sp->cls, sp->id); continue; }
        nd->w = w; nd->cls = cls;
        st->count++; built++;
    }

    ui_layout(win);

    /* Say what was built, once, in one line.  A panel's geometry is the thing
     * a screenshot cannot settle — whether the content is TALLER than its
     * viewport is a number, and without it "the page looks cut off" and "the
     * page scrolls" are the same picture. */
    {
        struct ui_state* s2 = state_of(win);
        for (int i = 0; s2 && i < s2->count; i++) {
            if (!(s2->n[i].flags & UI_SCROLL)) continue;
            klog(KLOG_INFO, "ui", "%d widget(s); viewport id %d: %d px of "
                 "content in %d px%s\n", built, s2->n[i].id,
                 s2->n[i].content_h, s2->n[i].ch,
                 s2->n[i].content_h > s2->n[i].ch ? " (scrolls)" : "");
            break;
        }
    }

    /* `gui.ui_debug = 1` dumps the laid-out tree.  Kept because the alternative
     * is squinting at a screenshot: every geometry question this milestone
     * raised ("is that label inside the viewport?") is one line of numbers. */
    if (config_get_long("gui.ui_debug", 0)) ui_dump(win);

    /* Ask for a paint.  On an app-host window the host does it; on a hostless
     * one (a ring-3 client's) the compositor does — either way the request is
     * the same flag, which is what keeps ui_build indifferent to which kind of
     * window it was handed. */
    gui_window_request_layout(win);
    return built;
}

void ui_text_clipped(struct gfx_surface* s, const struct widget* w,
                     int x, int y, const char* text, uint32_t colour) {
    (void)w;
    if (!s || !text) return;
    /* THE CLIP IS ALREADY IN FORCE.  widget_draw_all sets it around every
     * widget's draw — to the widget's own rect, or to the VIEWPORT when the
     * widget is inside a scrolling container.
     *
     * This function used to set it here as well, to the widget's box, and that
     * is not a harmless duplicate: gfx_set_clip REPLACES, so the inner one
     * threw the viewport away and a label scrolled out of view was drawn over
     * the panel's title.  Two mechanisms for one invariant, and the narrower
     * one lost.  Kept as a named call because the intent reads well at the
     * call sites — but the clipping happens in exactly one place. */
    gfx_text(s, x, y, text, colour);
}

/* ---------------------------------------------------------------------------
 * Diagnostic.
 * ------------------------------------------------------------------------- */

static const char* size_name(int sc) {
    return sc == UI_SIZE_COMPACT ? "compact"
         : sc == UI_SIZE_REGULAR ? "regular" : "wide";
}

/* §M65 — drive the toolkit from the shell, so the things a screenshot cannot
 * settle (did the viewport actually move?  by how much?  what is the content
 * height?) have an answer that is a line of text.  `win` NULL = the focused
 * window, which is what a person means by "this panel". */
/* Any window that HAS a toolkit tree — the focused one first, because that is
 * what a person means by "this panel", but not only it: a command that answers
 * "no toolkit window focused" when one is plainly on screen is a command that
 * makes the user hunt for the focus rather than for the answer. */
static struct gui_window* ui_any_window(void) {
    struct gui_window* f = gui_wm_focused();
    if (f && gui_window_ui(f)) return f;
    struct gui_window* list[16];
    int n = gui_wm_windows(list, 16);
    for (int i = n - 1; i >= 0; i--)
        if (gui_window_ui(list[i])) return list[i];
    return NULL;
}

void ui_cmd(const char* args) {
    while (args && *args == ' ') args++;
    struct gui_window* win = ui_any_window();

    if (args && args[0] == 's') {                    /* "scroll [delta]" */
        int delta = 0, neg = 0;
        const char* p = args;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') delta = delta * 10 + (*p++ - '0');
        if (!delta) delta = 60;
        if (neg) delta = -delta;

        struct ui_state* st = win ? state_of(win) : NULL;
        if (!st) { kprintf("ui: no toolkit window focused\n"); return; }
        for (int i = 0; i < st->count; i++) {
            if (!(st->n[i].flags & UI_SCROLL)) continue;
            int before = st->n[i].scroll;
            int moved  = ui_scroll_by(win, st->n[i].id, delta);
            kprintf("ui: viewport id %d — content %d px in %d px, scroll %d -> %d (%s)\n",
                    st->n[i].id, st->n[i].content_h, st->n[i].ch,
                    before, st->n[i].scroll, moved ? "moved" : "clamped");
            gui_window_request_redraw(win);
            return;
        }
        kprintf("ui: this window has no scrolling container\n");
        return;
    }

    ui_dump(win);
}

void ui_dump(struct gui_window* win) {
    kprintf("ui: %d class(es) registered:", ui_class_count());
    for (int i = 0; i < ui_class_count(); i++)
        kprintf(" %s", ui_class_at(i)->name);
    kprintf("\n");

    struct ui_state* st = win ? state_of(win) : NULL;
    if (!st) { kprintf("ui: this window has no toolkit tree\n"); return; }

    int cw = 0, ch = 0;
    gui_window_content_size(win, &cw, &ch);
    kprintf("ui: content %dx%d px = %d cells -> %s, %d node(s)\n",
            cw, ch, cw / GFX_GLYPH_W, size_name(ui_size_class_for(cw)), st->count);
    for (int i = 0; i < st->count; i++) {
        struct ui_node* nd = &st->n[i];
        /* Plain %d/%s: this kernel's printf has no width or precision
         * specifiers, and "%-3d" is printed literally — which turned the first
         * version of this dump into garbage exactly when it was needed. */
        kprintf("  id %d parent %d %s at %d,%d %dx%d%s%s\n",
                nd->id, nd->parent,
                nd->w ? (nd->cls ? nd->cls->name : "?") : "box",
                nd->x, nd->y, nd->cw, nd->ch,
                nd->hidden ? "  (hidden)" : "",
                (nd->flags & UI_SCROLL) ? "  [viewport]" : "");
        if (nd->w && nd->w->clip_w > 0)
            kprintf("        clipped to %d,%d %dx%d\n", nd->w->clip_x,
                    nd->w->clip_y, nd->w->clip_w, nd->w->clip_h);
        if (nd->flags & UI_SCROLL)
            kprintf("        content %d px, scroll %d\n", nd->content_h, nd->scroll);
    }
}
