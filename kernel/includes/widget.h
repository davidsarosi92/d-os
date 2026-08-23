/* =============================================================================
 * widget.h — minimal immediate-ish widget toolkit for GUI app windows
 * (M22 stage 6).
 *
 * Model: a flat list of widgets per window (no nesting/containers yet).
 * Each widget is a struct with `struct widget` as its FIRST member, so
 * the generic code can walk/draw/hit-test without knowing the concrete
 * type.  All coordinates are relative to the window's CONTENT surface.
 *
 * Threading: widget callbacks (on_click / on_activate / on_submit /
 * key handling) run on the COMPOSITOR task — the mouse IRQ only
 * enqueues events.  Callbacks may therefore call the VFS, kmalloc,
 * create windows, etc.  After dispatching events the window is redrawn
 * wholesale (widget count is tiny; no per-widget damage).
 *
 * Widgets are kmalloc'd by their constructors and freed by
 * gui_window_destroy — apps never free them individually.
 * ============================================================================= */

#ifndef WIDGET_H
#define WIDGET_H

#include <stdint.h>

struct gfx_surface;
struct gui_window;
struct widget;

struct widget_ops {
    void (*draw) (struct widget* w, struct gfx_surface* s);
    /* (lx,ly) relative to the widget; kind: 0 = click, 1 = double. */
    void (*mouse)(struct widget* w, int lx, int ly, int kind);
    void (*key)  (struct widget* w, char c);
    /* M22.5 — raw keycode event (KC_* from keymap.h + modifier mask).
     * Delivered to the FOCUSED widget for keys that produce no
     * character (arrows, Home/End, Delete, PgUp/PgDn) and for
     * Ctrl+letter shortcuts (clipboard, save).  NULL = ignored. */
    void (*keycode)(struct widget* w, uint8_t kc, uint8_t mods);
    /* M22.5 — optional destructor for widget-owned heap objects (the
     * editor's text buffer).  Runs on the compositor task during
     * window teardown, BEFORE the widget struct itself is kfree'd. */
    void (*destroy)(struct widget* w);
    /* §M58 — the pointer stream a DRAG needs: press, motion-while-held,
     * release.  `mouse` above is the click EVENT and stays what most widgets
     * want; this is the phase stream, and a widget that implements it is
     * automatically pointer-GRABBED between press and release (gui.c), so the
     * motion keeps arriving after the pointer has left the widget — without
     * which a selection would stop exactly where a user drags to.
     *
     * Before this the toolkit could express a click and a double click and
     * NOTHING ELSE, which is why nothing in this system could be selected with
     * a mouse: a drag had no transport, whatever a widget did.
     *
     * DELIBERATELY LAST in the struct: every existing `widget_ops` in the tree
     * is a POSITIONAL initialiser, so a field inserted in the middle silently
     * re-binds each of them by one slot (the compiler warns about the type
     * mismatches — and would NOT warn where two neighbours happen to share a
     * signature).  New optional ops go at the end.
     * NULL = the widget does not want the stream. */
    void (*pointer)(struct widget* w, int lx, int ly, int phase);
    /* §M61 follow-up — mouse WHEEL over this widget.  `dz` is positive for
     * wheel-up.  Also at the end, for the reason above. */
    void (*scroll)(struct widget* w, int dz);
};

/* Phases for widget_ops.pointer. */
#define WPTR_PRESS    0
#define WPTR_DRAG     1
#define WPTR_RELEASE  2

struct widget {
    int x, y, w, h;                     /* inside window content        */
    /* §M65 — the rectangle this widget may draw in, when something above it
     * restricts that: a scrolling container's viewport.  `clip_w == 0` means
     * "no restriction", which is every widget that is not inside one.
     *
     * It lives here rather than in the toolkit's node table because the DRAW
     * loop walks the window's flat widget list and has nothing else to consult
     * — and a scrolled child that draws its whole box is exactly the artefact
     * a viewport exists to prevent. */
    int clip_x, clip_y, clip_w, clip_h;
    const struct widget_ops* ops;
    struct widget*     next;            /* window's widget list         */
    struct gui_window* win;
    void*              ctx;             /* owner cookie for callbacks   */
    int                focusable;       /* can receive keyboard focus   */
};

/* ---- Label ---------------------------------------------------------------- */
struct w_label {
    struct widget base;
    char     text[96];
    uint32_t color;
};
struct w_label* w_label_create(struct gui_window* win, int x, int y, int w,
                               const char* text);
void w_label_set(struct w_label* l, const char* text);

/* ---- Button ---------------------------------------------------------------- */
struct w_button {
    struct widget base;
    char text[24];
    void (*on_click)(struct w_button* b, void* ctx);
};
struct w_button* w_button_create(struct gui_window* win, int x, int y,
                                 int w, int h, const char* text,
                                 void (*on_click)(struct w_button*, void*),
                                 void* ctx);

/* ---- List view -------------------------------------------------------------- */
#define WLIST_MAX_ITEMS 96
#define WLIST_ITEM_LEN  72
#define WLIST_ROW_H     14

struct w_listview {
    struct widget base;
    char items[WLIST_MAX_ITEMS][WLIST_ITEM_LEN];
    uint8_t tags[WLIST_MAX_ITEMS];      /* opaque per-item tag (fs uses type) */
    int  count;
    int  sel;                           /* -1 = none                     */
    int  scroll;                        /* first visible row             */
    void (*on_activate)(struct w_listview* lv, int idx, void* ctx);  /* dbl-click */
    void (*on_select)  (struct w_listview* lv, int idx, void* ctx);  /* click     */
};
struct w_listview* w_listview_create(struct gui_window* win, int x, int y,
                                     int w, int h, void* ctx);
void w_listview_clear(struct w_listview* lv);
int  w_listview_add(struct w_listview* lv, const char* text, uint8_t tag);

/* ---- Single-line text input -------------------------------------------------- */
struct w_textinput {
    struct widget base;
    char buf[64];
    int  len;
    void (*on_submit)(struct w_textinput* t, void* ctx);   /* Enter */
};
struct w_textinput* w_textinput_create(struct gui_window* win, int x, int y,
                                       int w, void* ctx);
void w_textinput_set(struct w_textinput* t, const char* text);

/* ---- Multiline text editor (M22.5, w_editor.c) -------------------------------
 * Scrollable text buffer with cursor, selection (Shift+arrows),
 * clipboard (Ctrl+C/X/V), viewport tracking.  The buffer is
 * kmalloc'd and grows on demand; `len` is authoritative (the buffer
 * is kept NUL-terminated as a convenience for w_editor_text). */
#define WED_ROW_H  10                   /* 8 px glyph + 2 px leading */

struct w_editor {
    struct widget base;
    char* buf;                          /* cap bytes, buf[len] == 0     */
    int   cap, len;
    int   cursor;                       /* byte offset, 0..len          */
    int   anchor;                       /* selection anchor, -1 = none  */
    int   scroll_line, scroll_col;      /* viewport origin (line, col)  */
    int   pref_col;                     /* sticky column for up/down    */
    int   modified;                     /* dirty flag (apps clear it)   */
    /* Ctrl+letter combos the widget itself doesn't consume (C/X/V/A
     * are handled internally) are forwarded here — the editor app
     * binds Ctrl+S to save through this. */
    void (*on_shortcut)(struct w_editor* e, uint8_t kc, void* ctx);
};
struct w_editor* w_editor_create(struct gui_window* win, int x, int y,
                                 int w, int h, void* ctx);
/* Replace the whole content (len < 0 → strlen).  Returns 0 / -1 (OOM). */
int  w_editor_set_text(struct w_editor* e, const char* text, int len);
/* NUL-terminated view of the content; *out_len = e->len if non-NULL. */
const char* w_editor_text(struct w_editor* e, int* out_len);

/* ---- Item view (§M63/§M64, w_itemview.c) --------------------------------------
 * The window-side half of itemview.h: it owns the SELECTION and the SCROLL
 * (which the stateless views deliberately do not) and forwards mouse events to
 * the chosen layout's hit-test.  The desktop uses the same models and views
 * without a widget, because it paints onto the compositor's back buffer — that
 * is why the view API takes a surface and an origin rather than a window. */
struct item_model;
struct item_view;

struct w_itemview {
    struct widget base;
    const struct item_model* model;
    const struct item_view*  view;
    int  sel;
    int  scroll;
    void (*on_select)(struct w_itemview* iv, int idx, void* ctx);
};
struct w_itemview* w_itemview_create(struct gui_window* win, int x, int y,
                                     int w, int h,
                                     const struct item_model* model,
                                     const char* view_name, void* ctx);

/* ---- Generic helpers (used by gui.c) ----------------------------------------- */
/* Initialise a widget's base and add it to the window.  Every constructor goes
 * through this — see the note in widget.c for what happened to the one that
 * did not. */
void widget_init(struct widget* w, struct gui_window* win,
                 int x, int y, int ww, int hh,
                 const struct widget_ops* ops, void* ctx, int focusable);

void widget_draw_all(struct widget* head, struct gfx_surface* s);
struct widget* widget_at(struct widget* head, int lx, int ly);

#endif
