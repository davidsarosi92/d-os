/* =============================================================================
 * ui.h — the widget toolkit's spine: classes, specs, layout (§M65).
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT JUST "MORE WIDGETS".
 *
 * The old toolkit (widget.h, M22) has five controls, and every one of them is
 * created with ABSOLUTE PIXEL COORDINATES.  That is why the settings panel
 * renders a bool as a text box with a "Cycle" button next to it: there is no
 * checkbox, and there is nowhere to put one — a panel that wants two columns
 * has to compute both of them by hand, and re-compute them when §M61 changes
 * the resolution underneath it.
 *
 * So the missing piece is not the control list.  It is:
 *
 *   1. LAYOUT — widgets that say what size they want, containers that hand out
 *      the space.  Two-phase (measure bottom-up, arrange top-down); no
 *      constraint solver, no cascade, no stylesheet language.
 *   2. IDENTITY BY NAME — a widget class registers itself (WIDGET_CLASS) and is
 *      instantiated by NAME, the same shape ITEM_VIEW()/DESKTOP_SHELL() already
 *      use here.  A name can cross a process boundary; a function pointer
 *      cannot.
 *   3. A DESCRIPTION THAT IS DATA — `struct ui_spec` is ints and strings.  A
 *      kernel-side app fills an array of them; a ring-3 client can send the
 *      same array over the dosgui bridge, and BOTH get the same toolkit.  That
 *      is the whole reason the API looks like this instead of like a set of
 *      `w_checkbox_create(win, x, y, ...)` calls.
 *   4. ONE EVENT SINK PER WINDOW — (id, type, value), not a callback pointer
 *      per widget, for exactly the same reason.
 *
 * WHAT IS DELIBERATELY ABSENT: percentages, margins-as-a-box-model, selectors,
 * a style cascade, DPI scaling.  This kernel already learned what a general
 * parser in ring 0 costs (§M62's SVG went to build time).  The responsive part
 * is three size classes and one container that changes axis.
 *
 * NEW CAPABILITIES GO ON THE CLASS, NOT INTO `widget_ops`.  Every widget_ops in
 * the tree is a positional initialiser, and §M58 already paid for inserting a
 * field in the middle of one.  `struct widget_class` is new, so it can carry
 * measure/build/value without touching a single existing table.
 * ============================================================================= */

#ifndef UI_H
#define UI_H

#include <stdint.h>

struct widget;
struct gui_window;
struct gfx_surface;

/* ---------------------------------------------------------------------------
 * The spec: a widget described as DATA.
 * ------------------------------------------------------------------------- */

/* Container direction + per-child hints.  Flags rather than separate fields so
 * the struct stays small and a marshalled spec stays fixed-size. */
#define UI_ROW          0x0001  /* container: lay children out horizontally  */
#define UI_COL          0x0002  /* container: vertically (the default)       */
#define UI_FILL_W       0x0004  /* child: take all the cross-axis width      */
#define UI_HIDE_COMPACT 0x0008  /* child: drop me on a narrow screen         */
#define UI_WRAP_COMPACT 0x0010  /* container: become a column when narrow    */
#define UI_FOCUSABLE    0x0020  /* child: may take keyboard focus            */
/* Container: TWO ALIGNED COLUMNS.  Children are taken in pairs — (label,
 * control), (label, control) … — and the first column is ONE width for every
 * row: the widest label, not each row's own.
 *
 * That difference is the whole reason the flag exists.  A row-per-setting
 * layout gives every row its natural label width, so the controls start at a
 * different x on every line and nothing lines up: reported from use as "it all
 * runs together, it is hard to read".  Alignment is not decoration in a
 * key/value list — it is what makes it a list of pairs instead of a paragraph.
 *
 * On a COMPACT screen it stacks (label above control), like UI_WRAP_COMPACT. */
#define UI_GRID         0x0040
/* Container: SCROLLING VIEWPORT.  Children are laid out at their natural
 * height and the whole column is offset by the container's scroll position;
 * anything outside the viewport is clipped (widget.c enforces that), and the
 * wheel over the container moves it.
 *
 * Scrolling by MOVING the children — rather than by drawing into an offscreen
 * surface and blitting a window of it — because the widgets already carry
 * their own coordinates and the layout already places them: the alternative
 * would be a second surface per panel and a second set of hit-test maths. */
#define UI_SCROLL       0x0080

struct ui_spec {
    int         id;         /* app-assigned, unique in the window; 0 = none  */
    int         parent;     /* container id; 0 = the window root             */
    const char* cls;        /* class name — "box", "label", "checkbox", ...  */
    const char* text;       /* label / caption / initial content             */
    int         value;      /* checkbox state, slider position, selection    */
    int         min, max;   /* numeric range where the class uses one        */
    int         weight;     /* share of the leftover space (0 = natural)     */
    int         flags;      /* UI_* above                                    */
};

/* ---------------------------------------------------------------------------
 * Events: what comes back.  (id, type, value) — nothing that needs a pointer.
 * ------------------------------------------------------------------------- */
#define UI_EV_CLICK    1        /* button pressed                            */
#define UI_EV_TOGGLE   2        /* checkbox flipped; value = 0/1             */
#define UI_EV_CHANGE   3        /* slider / selection moved; value = new     */
#define UI_EV_SUBMIT   4        /* text entry accepted (Enter)               */
#define UI_EV_ACTIVATE 5        /* list/table row double-clicked             */

typedef void (*ui_event_fn)(struct gui_window* win, int id, int type,
                            int value, void* ctx);

/* ---------------------------------------------------------------------------
 * The class registry.
 * ------------------------------------------------------------------------- */

struct widget_class {
    const char* name;

    /* Build one instance from the spec.  The class adds it to the window
     * itself (widget.c's base_init does that today) and returns it. */
    struct widget* (*create)(struct gui_window* win, const struct ui_spec* sp);

    /* What size does this widget want?  `avail_w` is what the parent can
     * offer, so a label can report the height its text needs at that width.
     * A NULL measure means "whatever the spec asked for", which is what makes
     * the old fixed-size controls work unchanged. */
    void (*measure)(struct widget* w, int avail_w, int* min_w, int* pref_w,
                    int* pref_h);

    /* Optional: read/write the widget's value without knowing its type —
     * this is what lets a generic panel (and later a ring-3 client) drive a
     * control it has never heard of. */
    int  (*get_value)(struct widget* w);
    void (*set_value)(struct widget* w, int v);
    void (*set_text) (struct widget* w, const char* text);
    int  (*get_text) (struct widget* w, char* out, int cap);

    /* The window popup this widget opened has closed: `row` is the item picked
     * (-1 = dismissed), `tag` is whatever the widget passed to gui_popup_open.
     * A menu turns that into a command id, a combo turns it into its value —
     * same transport, different meaning, which is exactly why this is one op
     * on the class and not two mechanisms. */
    void (*popup_pick)(struct widget* w, int tag, int row);
};

#define WIDGET_CLASS(_var)                                               \
    static const struct widget_class _var;                               \
    static const struct widget_class* const _var##_ptr                   \
        __attribute__((used, section("ui_classes"))) = &_var;             \
    static const struct widget_class _var

int  ui_class_count(void);
const struct widget_class* ui_class_at(int index);
const struct widget_class* ui_class_find(const char* name);

/* ---------------------------------------------------------------------------
 * Building and laying out.
 * ------------------------------------------------------------------------- */

/* Instantiate `n` specs into `win` (order matters: a parent must appear before
 * its children).  Returns the number built; unknown class names are reported
 * and SKIPPED rather than aborting the whole window — a panel missing one
 * control is more useful than a panel that refuses to open.
 *
 * BUILD ONCE, LAYOUT MANY.  `on_layout` fires on every resize, and the app-host
 * does NOT free the widgets before calling it — the convention every app here
 * follows is "create in the open fn, reposition in on_layout".  Calling
 * ui_build again would therefore add a SECOND copy of every control (and the
 * first attempt did exactly that: after a resize the panel came back empty
 * because the node table had filled up with duplicates).  Use ui_node_count()
 * to tell the two calls apart and call ui_layout() for the second. */
int  ui_build(struct gui_window* win, const struct ui_spec* specs, int n,
              ui_event_fn on_event, void* ctx);

/* Re-run layout for the window's current content size.  Called by the app
 * host on `on_layout` (i.e. after a resize) and by ui_build. */
void ui_layout(struct gui_window* win);

/* Scroll a UI_SCROLL container by `dl` pixels (positive = further down its
 * content).  Clamped to the content; returns non-zero if it actually moved,
 * so a caller only repaints when something changed. */
int  ui_scroll_by(struct gui_window* win, int id, int dl);

/* Wheel routing: scroll whichever UI_SCROLL container is under (x,y) — content
 * coordinates.  Returns non-zero if something moved. */
int  ui_scroll_at(struct gui_window* win, int x, int y, int dz);

/* How many nodes this window's tree already has — 0 means "not built yet". */
int  ui_node_count(struct gui_window* win);

/* Remember which widget opened the popup, so its class hears the answer.
 * Called by a widget immediately before gui_popup_open. */
void ui_popup_from(struct widget* w, int tag);

/* Deliver a popup result — called by the app host when the compositor reports
 * one (AE_POPUP), never from an IRQ. */
void ui_dispatch_popup(struct gui_window* win, int row, int tag);

/* Look a built widget up by its spec id (0 = not found). */
struct widget* ui_by_id(struct gui_window* win, int id);

/* Raise an event to the window's sink — used by the controls. */
void ui_emit(struct widget* w, int type, int value);

/* Draw `text` clipped to a widget's own rectangle.
 *
 * A widget that draws past its bounds is a bug the layout cannot fix: the
 * measure said "this much", and the draw ignoring it is how a settings page
 * ends up with help text running out through the window frame.  Every control
 * that draws text should go through this rather than gfx_text. */
void ui_text_clipped(struct gfx_surface* s, const struct widget* w,
                     int x, int y, const char* text, uint32_t colour);

/* ---------------------------------------------------------------------------
 * Menus (§M65 stage 2).
 *
 * A menu is DECLARED as flat triples — (menu title, item label, command id) —
 * and the app gets the id back through the same event sink everything else
 * uses.  Flat, because a tree of pointers is not something a ring-3 client can
 * send; and ids rather than row numbers, because a row number describes the
 * menu's current shape while an id describes the command.
 *
 * An item of "-" is a separator.  The drop-down is gui.c's window popup, the
 * same overlay the combo box uses.
 * ------------------------------------------------------------------------- */
struct ui_menu_def {
    const char* menu;       /* top-level title, e.g. "File"                  */
    const char* item;       /* label, or "-" for a separator                 */
    int         id;         /* the command id delivered on UI_EV_CLICK       */
};

/* Longest popup text blob a menu can build (items joined by '\n'). */
#define POPUP_TEXT_MAX 512

/* Attach the model to a "menubar" widget built by ui_build.  `defs` is
 * BORROWED — apps declare it static, which also keeps it out of the heap. */
void ui_menubar_set(struct gui_window* win, int id,
                    const struct ui_menu_def* defs, int n);

/* A picked item arrives as UI_EV_CLICK on the MENU BAR's own id, carrying the
 * declared command id as the value — so an app has ONE place that handles
 * commands, whether they came from a menu or from a button. */
void ui_menubar_closed(struct gui_window* win, int id);

/* ---------------------------------------------------------------------------
 * Responsive: THREE size classes, measured in CHARACTER CELLS.
 *
 * Cells, not pixels, because this system has one fixed 8x8 font and no notion
 * of DPI: what decides whether two columns fit is how many characters fit, and
 * that is the number the layout actually reasons about.  Three classes,
 * because a 12-column grid with six breakpoints is an answer to a browser's
 * problem, not to ours.
 * ------------------------------------------------------------------------- */
#define UI_SIZE_COMPACT 0       /* < 60 cells (~480 px): one column          */
#define UI_SIZE_REGULAR 1       /* < 120 cells (~960 px)                     */
#define UI_SIZE_WIDE    2

int ui_size_class(const struct gui_window* win);
int ui_size_class_for(int content_px_w);

/* Diagnostic: print the registry and the window's laid-out tree.  `ui check`
 * uses it — a layout that draws correctly and measures wrongly is invisible in
 * a screenshot (§M64's lesson, applied to geometry). */
void ui_dump(struct gui_window* win);

/* The `ui` shell command: `ui` dumps the focused window's tree, `ui scroll [n]`
 * moves its viewport and prints what happened.  A layout that draws correctly
 * and measures wrongly is invisible in a screenshot (§M64's lesson). */
void ui_cmd(const char* args);

#endif
