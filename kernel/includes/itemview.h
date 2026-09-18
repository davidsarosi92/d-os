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
    /* §M81 — THIS ROW STARTS A NEW GROUP: draw a rule above it.
     *
     * Appended, and zero is the old behaviour exactly, which is the rule since
     * §M58's scar (this struct is filled with positional initialisers inside
     * the views themselves).
     *
     * It exists because a MENU is a list with divisions in it, and the Start
     * menu had been drawing its own rule from its own row arithmetic — a
     * second copy of the geometry the view already owns, which is the shape
     * §4.79 paid for in the title buttons: the painter and the hit test
     * computing the same box separately.  A model that says WHERE a group
     * begins leaves the geometry in one place. */
    int         group_start;
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

    /* §M81 — HOW TALL A ROW OF MINE SHOULD BE, or 0 for the density's `row_h`.
     * Appended, like everything since §M58's scar.
     *
     * THE MODEL DECIDES, NOT THE VIEW — the same argument §M69 made for
     * `col_style`.  A file list and a MENU are both lists, and the design gives
     * them different heights: a table row is a hit target with a rhythm
     * (`row_h`), a menu item is text with padding.  A view that hard-coded one
     * would be right for the file manager and wrong for the Start menu, and a
     * view that took the height as a parameter would put the number back in
     * every caller — which is what this whole milestone is removing.
     *
     * It is not cosmetic.  With `SM_MAX_APPS` (12) plus the session tail, a
     * menu drawn at `row_h` comes to 1088 px at the 200 % density cap, i.e.
     * past both `PANEL_POPUP_MAX` and the screen — so the wrong answer here is
     * a menu whose top rows are silently clipped, which is exactly the §M32
     * defect this file's neighbours already paid for. */
    int  (*row_h)(void* ctx);

    /* HOW a column is set — appended, like everything since §M58's scar.
     *
     * The design's table is not a grid of identical text: the identity column
     * is in the accent colour, the numbers are RIGHT-ALIGNED and set in the
     * monospaced face so digits line up, the status column is muted.  Those are
     * per-column facts the MODEL knows and the view cannot guess — a view that
     * right-aligned anything numeric-looking would right-align a version
     * string, and one that guessed at colour would have to know what the
     * columns MEAN.
     *
     * One callback returning a bitmask rather than four callbacks: they are
     * always decided together, and a model that answers three of four is a
     * column styled half by its author and half by a default.
     *
     * NULL (or 0 for a column) is the old behaviour exactly — left-aligned,
     * proportional, ordinary text — so no existing model changes. */
    int  (*col_style)(void* ctx, int col);

    /* §M69 — THE EMPTY STATE (widget_specs.md §11).  A view with no items drew
     * NOTHING, so "this folder is empty", "the filter matched nothing" and
     * "the model failed to load" were one identical blank rectangle — *and the
     * blank is the one the user reads as broken.*
     *
     * DATA, NOT A CALLBACK, because there is nothing to compute: the message
     * is a property of the collection.  They hold CATALOGUE KEYS and the view
     * runs them through `lstr` — which also means a plain English literal
     * works unchanged (locale.h's fallback), and that a language change
     * updates the text without the model being rebuilt.
     *
     * `empty_action` is the design's `accent` call-to-action and is optional
     * on its own: a list with nothing to do about being empty should not
     * invent an instruction. */
    const char* empty_text;
    const char* empty_action;
};

/* Flags for item_model.col_style. */
#define ICOL_RIGHT   0x01   /* right-align the cell inside its column        */
#define ICOL_MONO    0x02   /* tabular face: a column of digits must line up */
#define ICOL_ACCENT  0x04   /* the record's identity, in the accent colour   */
#define ICOL_DIM     0x08   /* secondary information, in the muted colour    */

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

    /* §M69 — WHERE IS THIS VIEW'S SCROLLBAR, and what does it scroll?
     *
     * Appended, like every optional op since §M58's positional-initialiser
     * scar.  The VIEW knows where it drew the bar (only it knows about its
     * header band, its row height, its columns); the WIDGET owns `scroll` and
     * therefore owns the drag.  Splitting it this way is what stops the four
     * layouts each growing their own copy of press/drag/release — which is how
     * the tree ended up with four scrollbars and no way to grab any of them.
     *
     * Fills the bar's rect plus the CONTENT and VIEWPORT in the view's own
     * unit (rows for the list and the table), which is all `sb_metrics` needs.
     * Returns 0 when the view has no bar — either it does not scroll, or
     * everything already fits.  NULL means the layout has no scrollbar at all,
     * which a caller must be able to tell from "there is nothing to scroll". */
    int  (*scrollbar)(int w, int h, const struct item_model* m, int scroll,
                      int* bx, int* by, int* bw, int* bh,
                      int* content, int* viewport);

    /* §M75.2 — WHERE IS ONE CELL?  Appended, like every optional op since
     * §M58's scar.
     *
     * The content diff damages a changed ROW, which is precise while a window
     * is small and is not when it is maximized: at 1920 px a row is ~76 kpx,
     * so five moving rows are ~380 kpx of compositing a second — and the
     * compositor also draws the cursor, which is what *"maximize the Task
     * Manager and the mouse lags terribly"* is made of.  A changed `TIME` cell
     * is ~6 kpx.
     *
     * OPTIONAL BECAUSE ONLY A COLUMNAR VIEW HAS CELLS.  The list and the grid
     * leave it NULL and keep whole-row damage, which is correct for them —
     * their "row" IS one cell.  A caller must therefore treat NULL as "ask for
     * the row instead", never as an error.
     *
     * Returns 0 and fills the rect (relative to the box) on success. */
    int  (*cell_rect)(int i, int col, int w, int h, const struct item_model* m,
                      int scroll, int* ox, int* oy, int* ow, int* oh);

    /* §M81 — HOW TALL MUST THE BOX BE TO SHOW `n` ITEMS?  The honest inverse of
     * `page`, and appended for the reason everything since §M58 is.
     *
     * Every caller so far is GIVEN a box and asks how much of the model fits.
     * A MENU is the other way round: it sizes itself to its contents, so it has
     * to ask the view rather than keep its own copy of the row height.  The
     * Start menu kept one (`SM_ITEM_H`) and computed its rows in three separate
     * places — the painter, the hit test and the hover — which is exactly the
     * divergence this whole milestone is about.
     *
     * NULL means "this layout cannot size itself to its content", which a
     * caller must be able to tell from a height of zero. */
    int  (*height_for)(int w, int n, const struct item_model* m);
};

/* One cell's text, exactly as a view would draw it: from `cell` when the model
 * has columns, otherwise the entry's label — which is what makes a plain list
 * model render in the table.  Exported because the WIDGET needs the same answer
 * for its content diff, and two copies of "cell or label" are two chances for
 * the diff to compare something the painter never drew. */
void item_cell_text(const struct item_model* m, int index, int col,
                    char* out, int cap);

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
