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
};

struct widget {
    int x, y, w, h;                     /* inside window content        */
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

/* ---- Generic helpers (used by gui.c) ----------------------------------------- */
void widget_draw_all(struct widget* head, struct gfx_surface* s);
struct widget* widget_at(struct widget* head, int lx, int ly);

#endif
