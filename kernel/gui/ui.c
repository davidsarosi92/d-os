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
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "widget.h"
#include "console_plate.h"
#include "scrollbar.h"
#include "gui.h"
#include "gui_internal.h"   /* gui_wm_focused — "this panel" means the focused one */
#include "gfx.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include "config.h"
#include "settings.h"
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
    /* §M69 — the container scrollbar's drag.  ONE latch per window, not per
     * node, because a pointer grab is singular: while it is held every phase
     * belongs to the node that took the press, whatever the pointer wanders
     * over (a child widget, the wallpaper).  Without the latch a drag that
     * strayed off the twelve-pixel bar would start clicking the controls it
     * passed over — which is most of a settings panel. */
    int sb_node;                        /* node id being dragged, 0 = none   */
    int sb_part;                        /* enum sb_part                      */
    int sb_grab_dy;
};

static struct ui_state* state_of(struct gui_window* win) {
    return (struct ui_state*)gui_window_ui(win);
}

/* §M69 — A SCROLL DAMAGES ITS VIEWPORT, NOT THE WINDOW.
 *
 * Everything that scrolls a container used to leave the repaint to the caller,
 * and every caller asked for the WHOLE window: on the Appearance panel that is
 * 431 kpx and ~50 ms of compositing for a 273 kpx viewport, with the header,
 * the status line and the Save button — none of which moved — repainted every
 * time.  The bar lives inside the viewport rect (arrange_scroll reserves the
 * strip from the children's width), so one rect covers both. */
static void ui_damage_node(struct gui_window* win, const struct ui_node* nd) {
    gui_window_request_redraw_rect(win, nd->x, nd->y, nd->cw, nd->ch);
}

int ui_scroll_by(struct gui_window* win, int id, int dl) {
    struct ui_state* st = state_of(win);
    if (!st) return 0;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].id != id || !(st->n[i].flags & UI_SCROLL)) continue;
        int before = st->n[i].scroll;
        st->n[i].scroll += dl;
        if (st->n[i].scroll < 0) st->n[i].scroll = 0;
        ui_reflow(win);                 /* clamps against the current viewport */
        if (st->n[i].scroll == before) return 0;
        ui_damage_node(win, &st->n[i]);
        return 1;
    }
    return 0;
}

/* The wheel: find the scrolling container under (x,y) and move it.  A
 * container is not a widget, so this cannot ride the widget scroll op — and
 * routing by POSITION rather than by focus is what every toolkit does and what
 * the hand expects (§M61's wheel lesson). */
/* §M69 — HOW FAR ONE NOTCH GOES, in ROWS, from one config key that the list
 * widgets read too.
 *
 * It was a literal 48 PIXELS here: density-blind (so it meant a different
 * amount of content at every resolution) and not a multiple of anything, so
 * every notch stopped mid-control and the panel showed half a row at the top
 * and half at the bottom.  Reported from use as *"the scroll is chaotic"*, and
 * that is what chaotic looks like — nothing lands where the eye expects.
 *
 * A KEY rather than a constant because the right value depends on the DEVICE:
 * a mouse wheel sends one notch per click and wants three lines, a trackpad
 * sends a stream of them and three lines each is a panel that flies past.
 * Nobody can pick one number for both, which is the same argument
 * `gui.scroll_invert` was added under. */
int ui_wheel_lines(void) {
    /* Cached for the same reason: this is read per wheel notch, and a trackpad
     * sends them in bursts.  `gui.scroll_lines` changing is a settings action,
     * not something that has to be seen within one gesture. */
    static int cached = -1;
    if (cached >= 0) return cached;
    long n = config_get_long("gui.scroll_lines", 3);
    if (n < 1) n = 1;
    if (n > 20) n = 20;
    cached = (int)n;
    return cached;
}

int ui_wheel_step(void) { return ui_wheel_lines() * cp_row_h(); }

CONFIG_KEY(ck_scroll_lines) = {
    .key = "gui.scroll_lines", .group = "Appearance", .type = CFG_INT,
    .min = 1, .max = 10, .def = "3",
    .help = "rows moved by one wheel notch",
};

int ui_scroll_at(struct gui_window* win, int x, int y, int dz) {
    struct ui_state* st = state_of(win);
    if (!st) return 0;
    int only = -1, nscroll = 0;
    for (int i = 0; i < st->count; i++) {
        struct ui_node* nd = &st->n[i];
        if (!(nd->flags & UI_SCROLL)) continue;
        nscroll++;
        only = i;
        if (x < nd->x || x >= nd->x + nd->cw) continue;
        if (y < nd->y || y >= nd->y + nd->ch) continue;
        return ui_scroll_by(win, nd->id, dz > 0 ? -ui_wheel_step() : ui_wheel_step());
    }
    /* §M69 — NOTHING UNDER THE POINTER, BUT THE PANEL HAS EXACTLY ONE SCROLL
     * AREA: scroll that one.
     *
     * Reported from use as *"the scroll works only sometimes in Appearance, as
     * if it were intermittent."*  It was not intermittent, it was POSITIONAL:
     * a settings panel's group heading at the top and its status line and Save
     * button at the bottom sit OUTSIDE the viewport, so a notch aimed at any
     * of them matched no container and did nothing.  *From a chair, behaviour
     * that depends on a few pixels of pointer position is indistinguishable
     * from behaviour that is unreliable.*
     *
     * ONLY when there is exactly one, which is what keeps this from guessing:
     * with two scroll areas the pointer is the only thing that says which is
     * meant, and choosing for the user would be worse than doing nothing. */
    if (nscroll == 1 && only >= 0)
        return ui_scroll_by(win, st->n[only].id,
                            dz > 0 ? -ui_wheel_step() : ui_wheel_step());
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
    int cells = content_px_w / cp_fw();
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
    if (col0 < cp_fw() * 6) col0 = cp_fw() * 6;
    return col0;
}

static void measure_grid(struct ui_state* st, int idx, int avail_w,
                         int size_class) {
    struct ui_node* nd = &st->n[idx];
    int stacked = (size_class == UI_SIZE_COMPACT);
    int col0 = stacked ? 0 : grid_col0(st, nd, avail_w, size_class);
    int rest = stacked ? avail_w : avail_w - col0 - UI_GAP;
    if (rest < cp_fw() * 6) rest = cp_fw() * 6;

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
        nd->pref_h = cp_fh() * 4;      /* a floor; weight gives it the rest */
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
            /* Rule 2 again, in the branch a narrow screen takes: the LABEL
             * spans (it is the row's heading), the CONTROL does not unless it
             * asked to.  `even ? w : w` was the tell — both arms the same,
             * which is how a line says it was meant to distinguish two cases
             * and never did. */
            int cwid = even || (c->flags & UI_FILL_W)
                           ? w
                           : (c->pref_w < w ? c->pref_w : w);
            arrange_node(st, i, x, cur, cwid, c->pref_h, size_class);
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
            /* RULE 2 (console_plate.h): the control gets its OWN width, not the
             * column's, unless it asked to fill.  This passed `rest`
             * unconditionally, which is why a Save button came out as wide as
             * the panel and a three-item segmented control stretched across a
             * window — nobody chose either; it was simply what a grid's second
             * column handed out.  A slider, a text box and a progress bar do
             * want the width, and they say so with UI_FILL_W: a track that
             * stops halfway says nothing about the range it represents. */
            int cwid = (c->flags & UI_FILL_W)
                           ? rest
                           : (c->pref_w < rest ? c->pref_w : rest);
            arrange_node(st, i, x + col0 + UI_GAP, cur, cwid, c->pref_h,
                         size_class);
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

    /* THE BAR'S WIDTH COMES OFF THE CONTENT, NOT OUT OF IT.  Laying the
     * children out across the full viewport and then painting a bar on top
     * covers whatever is under it — which is invisible until one child happens
     * to be wide enough to reach the edge.  The table view learned the same
     * thing today, one layer down. */
    int inner = w - (content > h ? cp_scrollbar_w() + 2 : 0);
    if (inner < cp_fw() * 4) inner = w;          /* too narrow to bother */

    int cur = y - nd->scroll;
    for (int i = 0; i < st->count; i++) {
        if (st->n[i].parent != nd->id || st->n[i].hidden) continue;
        struct ui_node* c = &st->n[i];
        int cwid = (c->flags & UI_FILL_W)
                       ? inner
                       : (c->pref_w < inner ? c->pref_w : inner);
        arrange_node(st, i, x, cur, cwid, c->pref_h, size_class);
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
    /* §M69 — UI_ALIGN_END: start the run `space` further along, so the group
     * finishes flush with the container's far edge.  Only when nothing is
     * weighted: a weight has already consumed the slack, and pushing by it as
     * well would double-count the same pixels. */
    if ((nd->flags & UI_ALIGN_END) && weight_sum == 0) cur += space;
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

/* §M69 — MEASURE ONCE, ARRANGE MANY, and this is a latency fix rather than
 * tidiness.
 *
 * `ui_layout` MEASURES every node before arranging it, and measuring a label
 * means a catalogue lookup plus a per-glyph text width.  Scrolling calls it —
 * `ui_scroll_by` per notch, and the scrollbar DRAG per motion packet — so
 * dragging the thumb across a settings panel re-measured every widget in it
 * dozens of times a second.  Reported from use as *"during a drag it froze for
 * a few seconds and then became usable again"*: not a stuck state, a starved
 * one.
 *
 * A scroll changes no SIZE, only positions, so the measurements from the last
 * real layout are still true.  `ui_reflow` re-arranges with them. */
static int ui_layout_pass(struct gui_window* win, int do_measure);

void ui_layout(struct gui_window* win) { ui_layout_pass(win, 1); }
void ui_reflow(struct gui_window* win) { ui_layout_pass(win, 0); }

static int ui_layout_pass(struct gui_window* win, int do_measure) {
    struct ui_state* st = state_of(win);
    if (!st || st->count == 0) return 0;

    int cw = 0, ch = 0;
    gui_window_content_size(win, &cw, &ch);
    if (cw <= 2 * UI_PAD || ch <= 2 * UI_PAD) return 0;

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
        if (do_measure) measure_node(st, i, inner_w, sc);
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
        /* RULE 2 AT THE ROOT — the third copy of this defect, and the one that
         * produced the reported symptom: the Control Panel's Save button asked
         * for 78 px and was placed at 544, because this line handed every
         * top-level child `inner_w` without ever consulting UI_FILL_W.  The
         * grid had the same bug twice (its stacked and unstacked branches), so
         * the layout engine stretched controls on all three of its paths and
         * no caller could opt out anywhere. */
        int wid = (c->flags & UI_FILL_W)
                      ? inner_w
                      : (c->pref_w < inner_w ? c->pref_w : inner_w);
        arrange_node(st, i, UI_PAD, cur, wid, hgt, sc);
        cur += hgt + UI_GAP;
    }
    return 1;
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
    cp_text(s, x, y, text, colour);
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

/* THE SCROLL INDICATOR — drawn here rather than by a widget, because a
 * container is a NODE and nodes have no draw op.
 *
 * WHY IT MUST EXIST.  `ui_dump` measured the settings panel's viewport at
 * 242 px holding 1004 px of content: two thirds of every long settings group
 * was unreachable with nothing on screen saying so, which is how a radio group
 * with three options came to be reported as showing one.  *A view that silently
 * shows a prefix of its data is worse than one that shows none, because nothing
 * looks wrong.*  The table view got the same treatment today for the same
 * reason, and the shape is deliberately identical: a `tray` trough, a
 * proportional `muted` thumb, no end arrows (widget_specs.md §10).
 *
 * Painted AFTER the widgets and with no clip of its own: the children are
 * clipped to the viewport, and the bar sits in the strip arrange_scroll kept
 * clear for it. */
static void ui_sb_metrics(const struct ui_node* nd, struct sb_metrics* m);

void ui_draw_overlay(struct gui_window* win, struct gfx_surface* s) {
    struct ui_state* st = win ? state_of(win) : NULL;
    if (!st || !s) return;
    const cp_theme* t = cp_current_theme();

    for (int i = 0; i < st->count; i++) {
        struct ui_node* nd = &st->n[i];
        if (!(nd->flags & UI_SCROLL) || nd->hidden) continue;
        if (nd->content_h <= nd->ch || nd->ch <= 0) continue;

        /* §M69 — through the shared scrollbar, which is what gives the
         * container's bar arrows and a thumb that can be grabbed.  The old
         * copy here drew an indicator; nothing could touch it. */
        struct sb_metrics m;
        ui_sb_metrics(nd, &m);
        sb_draw(s, &m, st->sb_node == nd->id ? st->sb_part : SB_NONE);
    }
    (void)t;
}

/* The container bar's metrics, in the same window coordinates its children
 * live in.  One definition for the painter and the hit test. */
static void ui_sb_metrics(const struct ui_node* nd, struct sb_metrics* m) {
    int bw = cp_scrollbar_w();
    sb_metrics(m, nd->x + nd->cw - bw, nd->y, bw, nd->ch,
               nd->content_h, nd->ch, nd->scroll);
}

static struct ui_node* ui_node_by_id(struct ui_state* st, int id) {
    for (int i = 0; i < st->count; i++)
        if (st->n[i].id == id) return &st->n[i];
    return NULL;
}

/* Damage the BAR alone — a strip about sixteen pixels wide, against the
 * viewport's quarter of a megapixel.  What changes when a press latches or a
 * release lets go is which part is drawn held, and nothing else on the panel
 * moves; the two are separated because they differ by a factor of thirty. */
static void ui_damage_bar(struct gui_window* win, const struct ui_node* nd) {
    struct sb_metrics m;
    ui_sb_metrics(nd, &m);
    gui_window_request_redraw_rect(win, m.x, m.y, m.w, m.h);
}

/* §M69 — PRESS / DRAG / RELEASE on a scrolling container's bar.
 *
 * A container is a NODE and nodes have no widget, so nothing in the window's
 * widget list sits under that strip and gui.c's ordinary pointer routing found
 * nobody — the settings panels drew a scrollbar that could not be used at all.
 * gui.c asks here BEFORE resolving a widget, so the latch above wins over
 * whatever the drag passes over.
 *
 * Returns non-zero when the toolkit consumed the event. */
int ui_pointer_at(struct gui_window* win, int x, int y, int phase) {
    struct ui_state* st = state_of(win);
    /* THE ENTRY HALF of the probe.  Without it, "no press was logged" cannot
     * distinguish *the event never arrived* from *it arrived and matched no
     * container* — the two causes of "I cannot grab the bar", and they live in
     * different files. */
    if (phase == WPTR_PRESS && gui_input_debug()) {
        int nsc = 0;
        if (st) for (int i = 0; i < st->count; i++) {
            struct ui_node* n = &st->n[i];
            if (!(n->flags & UI_SCROLL)) continue;
            nsc++;
            kprintf("ui_pointer_at: phase=%d at %d,%d  node %d rect %d,%d %dx%d "
                    "content=%d scroll=%d\n", phase, x, y, n->id,
                    n->x, n->y, n->cw, n->ch, n->content_h, n->scroll);
        }
        if (!nsc) kprintf("ui_pointer_at: phase=%d at %d,%d  no scroll node\n",
                          phase, x, y);
    }
    if (!st) return 0;

    /* §M69 — A LATCH THAT SURVIVES INTO THE NEXT PRESS IS STALE BY DEFINITION.
     *
     * Reported from use: *"I click and nothing happens, I do not know what it
     * depends on, then suddenly it works"* and *"during a drag it froze in a
     * lighter colour, as if it were active."*  Both are one state: the grab
     * latched on a press and the RELEASE never arrived, after which this
     * function consumed EVERY later event — including every press — and the
     * window looked alive and answered nothing.
     *
     * The release can go missing for reasons this file cannot prevent (a full
     * queue, a window closing mid-gesture), so the latch must be
     * SELF-HEALING rather than merely correct on the happy path: a press is by
     * definition the start of a new gesture, so anything still held when one
     * arrives belongs to a gesture that ended without telling us.  *A recovery
     * that depends on the event that went missing is not a recovery.* */
    if (st->sb_node && phase == WPTR_PRESS) {
        struct ui_node* old = ui_node_by_id(st, st->sb_node);
        st->sb_node = st->sb_part = 0;
        if (old) ui_damage_bar(win, old);       /* the held highlight, no more */
    }

    /* Mid-drag: the latched node owns every phase until the release. */
    if (st->sb_node) {
        struct ui_node* nd = ui_node_by_id(st, st->sb_node);
        if (!nd) { st->sb_node = st->sb_part = 0; return 0; }
        if (phase == WPTR_RELEASE) {
            st->sb_node = st->sb_part = 0;
            /* ONLY THE BAR CHANGED.  A release ends the highlight and moves
             * nothing, so repainting the window here was the second of the
             * four full-window repaints a single click used to cost. */
            ui_damage_bar(win, nd);
            return 1;
        }
        if (phase == WPTR_DRAG && st->sb_part == SB_THUMB) {
            struct sb_metrics m;
            ui_sb_metrics(nd, &m);
            int ns = sb_scroll_from_thumb(&m, nd->content_h, nd->ch,
                                          y - st->sb_grab_dy);
            if (ns != nd->scroll) {
                nd->scroll = ns;
                ui_reflow(win);          /* positions only — see ui_layout_pass */
                ui_damage_node(win, nd);
            }
        }
        return 1;                        /* consumed either way while latched */
    }

    if (phase != WPTR_PRESS) return 0;

    for (int i = 0; i < st->count; i++) {
        struct ui_node* nd = &st->n[i];
        if (!(nd->flags & UI_SCROLL) || nd->hidden) continue;
        if (nd->content_h <= nd->ch || nd->ch <= 0) continue;
        struct sb_metrics m;
        ui_sb_metrics(nd, &m);
        int part = sb_hit(&m, x, y);
        if (gui_input_debug())
            kprintf("ui: press at %d,%d over node %d bar %d,%d %dx%d part=%d\n",
                    x, y, nd->id, m.x, m.y, m.w, m.h, part);
        if (part == SB_NONE) continue;

        st->sb_node = nd->id;
        st->sb_part = part;
        st->sb_grab_dy = y - m.thumb_y;
        int step = cp_row_h();
        int was = nd->scroll;
        switch (part) {
        case SB_UP:          nd->scroll -= step; break;
        case SB_DOWN:        nd->scroll += step; break;
        case SB_TROUGH_UP:   nd->scroll -= sb_page(nd->ch); break;
        case SB_TROUGH_DOWN: nd->scroll += sb_page(nd->ch); break;
        default: break;
        }
        int max = nd->content_h - nd->ch;
        if (nd->scroll < 0)   nd->scroll = 0;
        if (nd->scroll > max) nd->scroll = max;
        /* The viewport when the content moved; otherwise only the bar, whose
         * pressed part is now drawn held — an arrow at the end of its travel
         * has to answer the click without repainting a thing under it. */
        if (nd->scroll != was) { ui_reflow(win); ui_damage_node(win, nd); }
        else                   ui_damage_bar(win, nd);
        return 1;
    }
    return 0;
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
            cw, ch, cw / cp_fw(), size_name(ui_size_class_for(cw)), st->count);
    for (int i = 0; i < st->count; i++) {
        struct ui_node* nd = &st->n[i];
        /* Plain %d/%s: this kernel's printf has no width or precision
         * specifiers, and "%-3d" is printed literally — which turned the first
         * version of this dump into garbage exactly when it was needed. */
        /* THE PREFERRED SIZE IS PRINTED NEXT TO THE PLACED ONE, because the
         * only question this dump is ever opened for is "did it get what it
         * asked for" — and with only the placed size, a control stretched by
         * its container and one that genuinely wants the width look identical.
         * `fill` marks the ones that asked to span, so a surprise is visible as
         * a wide box WITHOUT that word. */
        kprintf("  id %d parent %d %s at %d,%d %dx%d (want %dx%d)%s%s%s\n",
                nd->id, nd->parent,
                nd->w ? (nd->cls ? nd->cls->name : "?") : "box",
                nd->x, nd->y, nd->cw, nd->ch,
                nd->pref_w, nd->pref_h,
                (nd->flags & UI_FILL_W) ? "  fill" : "",
                nd->hidden ? "  (hidden)" : "",
                (nd->flags & UI_SCROLL) ? "  [viewport]" : "");
        if (nd->w && nd->w->clip_w > 0)
            kprintf("        clipped to %d,%d %dx%d\n", nd->w->clip_x,
                    nd->w->clip_y, nd->w->clip_w, nd->w->clip_h);
        if (nd->flags & UI_SCROLL)
            kprintf("        content %d px, scroll %d\n", nd->content_h, nd->scroll);
    }
}

/* --- §M70 shell registration ----------------------------------------------- */
SHELL_CMD(ui) = { "ui", "[dump|classes]", "the widget-class registry and layout dumps",
                  SHELL_G_GUI, ui_cmd, SHELL_P_ANY };
