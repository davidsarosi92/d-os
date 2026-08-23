/* =============================================================================
 * itemview.h — a collection of items, and a SWAPPABLE way of laying them out
 * (§M63 / §M64).
 *
 * The desktop's shortcuts, the Control Panel's categories and the file
 * manager's listing are three views of one idea: *a list of things with a
 * label, an icon and an action*.  Written three times they become three
 * layouts, three hit-tests, three keyboard-navigation implementations and
 * three sets of off-by-one bugs — and "I would like a list view instead of
 * icons" becomes a rewrite in each.
 *
 * So the layout is not the widget.  There are two halves:
 *
 *   MODEL — what the items ARE.  Supplied by the owner (shortcut files, the
 *           settings registry, a directory listing).  Pure data + an activate
 *           callback; it knows nothing about pixels.
 *   VIEW  — how they are ARRANGED.  A stateless painter + hit-tester,
 *           registered with ITEM_VIEW() into a linker section, chosen BY NAME
 *           from config (`desktop.view`, `controlpanel.view`).  Adding
 *           "details" later is a new file, not an edit to any consumer.
 *
 * Selection and scroll position are NOT in either half — they belong to the
 * thing being looked at (a window keeps its own, the desktop keeps its own),
 * and passing them in keeps the views free of state and therefore free of
 * lifetime questions.
 *
 * The draw call takes a TARGET SURFACE and an ORIGIN rather than a window,
 * because §M64's desktop paints onto the compositor's back buffer under the
 * windows, not into a window of its own.  That is a five-minute decision now
 * and a refactor later.
 *
 * Threading: `draw`/`hit` are pure and may run anywhere.  `activate` runs on
 * whatever task dispatched the click — the desktop defers it to the desktop
 * task, because the mouse IRQ holds the WM lock (see desktop.h).
 * ============================================================================= */

#ifndef ITEMVIEW_H
#define ITEMVIEW_H

#include <stdint.h>

struct gfx_surface;

/* One item, filled in by the model on demand.  Strings are borrowed for the
 * duration of the call — a model may point them at its own storage. */
struct item_entry {
    const char* label;
    const char* sub;            /* optional second line; NULL = none      */
    int         icon;           /* enum icon_id                           */
    int         dim;            /* non-zero = draw as unavailable         */
};

struct item_model {
    int  (*count)(void* ctx);
    /* Fill `out` for `index`.  Return 0 on success, non-zero to skip. */
    int  (*get)(void* ctx, int index, struct item_entry* out);
    /* Double-click / Enter.  May be NULL. */
    void (*activate)(void* ctx, int index);
    void* ctx;

    /* §M65 stage 3 — COLUMNS, appended (never inserted: this struct is filled
     * with positional initialisers in places, the lesson §M58 paid for).
     *
     * A table is not a new kind of thing; it is this model asked a second
     * question: not "what is item i" but "what is item i's column c".  A model
     * that leaves these NULL still renders in every view — the table simply
     * shows one column, built from `get`, which is what a list already is.
     *
     * `col_title` names the header; `col_weight` shares the width the way the
     * layout engine does; `cell` fills one cell's text. */
    int  (*columns)(void* ctx);
    const char* (*col_title)(void* ctx, int col);
    int  (*col_weight)(void* ctx, int col);
    int  (*cell)(void* ctx, int index, int col, char* out, int cap);

    /* §M64 tail — where the owner has PUT this item, if anywhere.  Appended
     * for the same reason the columns were: a model that leaves it NULL lays
     * out in flow order exactly as it did before, so the Control Panel and the
     * file manager are untouched by this existing.
     *
     * THE UNIT IS A GRID SLOT (column, row), NOT A PIXEL, and that is the
     * whole decision.  §M61 made the resolution a runtime choice: a position
     * stored in pixels puts an icon off the screen the moment somebody picks
     * a smaller mode — silently, because nothing draws outside the box, so it
     * reads as "my shortcut was deleted".  A slot survives the mode change,
     * cannot half-overlap its neighbour, and is what the .lnk file has always
     * stored (`x`/`y`, with -1 meaning "not placed").
     *
     * Return 0 and fill the two outputs when the item is placed; non-zero when
     * it is not, and the view puts it in flow order. */
    int  (*pos)(void* ctx, int index, int* col, int* row);
};

/* A layout.  Stateless: everything it needs arrives as arguments. */
struct item_view {
    const char* name;                   /* "grid", "list", …              */

    /* Paint items into `s` inside the box (x,y,w,h).  `sel` is the selected
     * index or -1.  `scroll` is a view-defined first-item offset. */
    void (*draw)(struct gfx_surface* s, int x, int y, int w, int h,
                 const struct item_model* m, int sel, int scroll);

    /* Index at (px,py), given RELATIVE to the box.  -1 = nothing there. */
    int  (*hit)(int px, int py, int w, int h,
                const struct item_model* m, int scroll);

    /* Bounding box of item `i` relative to the box — used for damage rects,
     * so moving or selecting one item does not repaint the screen.
     * Returns 0 on success. */
    int  (*rect)(int i, int w, int h, const struct item_model* m, int scroll,
                 int* ox, int* oy, int* ow, int* oh);

    /* How many items fit at once — the scroll step and the page size. */
    int  (*page)(int w, int h);

    /* §M64 tail — which SLOT does a point fall in?  The other half of
     * `item_model.pos`, and what makes drag-to-move possible: the drop lands
     * at a point, and somebody has to turn that into the thing a position is
     * stored as.
     *
     * OPTIONAL ON PURPOSE — a layout decides whether its items can be
     * arranged at all.  The list and the table say NO by leaving this NULL,
     * because their order IS the model's order and dropping row 3 onto row 7
     * means REORDER, which is a different feature with different persistence.
     * A caller can therefore tell "this view cannot be arranged" from "the
     * drop missed the field", instead of a silent no-op that reads as a bug.
     *
     * Returns 0 and fills the two outputs on success. */
    int  (*slot_at)(int px, int py, int w, int h, int* col, int* row);
};

extern struct item_view __start_item_views[];
extern struct item_view __stop_item_views[];

#define ITEM_VIEW(_var)                                                  \
    static const struct item_view                                        \
    __attribute__((used, section("item_views"), aligned(4)))             \
    _var##_registration

/* Look a view up by name.  Never returns NULL: an unknown name falls back to
 * the first registered view, because a mistyped config key should give a
 * usable desktop with the wrong layout rather than an empty screen. */
const struct item_view* item_view_by_name(const char* name);
int  item_view_count(void);
const struct item_view* item_view_at(int i);

#endif
