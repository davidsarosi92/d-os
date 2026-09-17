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
     * DELIBERATELY LAST in the struct.  (§M75.2: every table in the tree is a
     * NAMED initialiser now, so a field in the middle no longer re-binds them
     * — but appending stays the rule, because a name only protects the tables
     * that have been converted and the next author will copy an old one.)
     * The hazard, for the record: a positional field inserted in the middle
     * silently re-binds each table by one slot (the compiler warns about the type
     * mismatches — and would NOT warn where two neighbours happen to share a
     * signature).  New optional ops go at the end.
     * NULL = the widget does not want the stream. */
    void (*pointer)(struct widget* w, int lx, int ly, int phase);
    /* §M61 follow-up — mouse WHEEL over this widget.  `dz` is positive for
     * wheel-up.  Also at the end, for the reason above.
     *
     * §M69 — RETURNS WHETHER IT CONSUMED THE NOTCH.  It used to return void
     * and the router treated "this widget has a scroll op" as "this widget
     * took it", so a list already at its end SWALLOWED the wheel and the page
     * it sits on never moved.  Reported from use: *"scrolling on the content
     * is no good, but on the scrollbar it is perfect."*  Non-zero = consumed;
     * 0 = pass it on to whatever contains me. */
    int  (*scroll)(struct widget* w, int dz);

    /* §M75.2 — DOES HOVER CHANGE HOW I LOOK?  Non-zero = "no, or I damaged
     * exactly what changed": the host then damages NOTHING for the hover.
     * NULL, or 0, means the host repaints the whole widget, which is what
     * every widget got before and is right for a button.
     *
     * Why it exists: `app_hover_to` damages TWO WIDGETS, which §M69 correctly
     * called precise — and it is, WHILE A WIDGET IS SMALL.  The item view
     * filling a maximized window is ~1.9 Mpx, and it renders no hover state at
     * all, so every pointer movement across it repainted the screen to set a
     * flag nothing draws.  Reported from use as *"maximize the Task Manager and
     * the mouse lags terribly"*, and the compositor draws the cursor, so each
     * of those frames is a packet the pointer waits for.
     *
     * *Precision measured in WIDGETS stops being precision when a widget is
     * the size of the screen.*
     *
     * At the end of the struct, for the reason stated above. */
    int  (*hover)(struct widget* w, int entering);
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

    /* §M65's state row had four of the design's five: rest, hover, pressed and
     * focused existed; DISABLED did not.  A control that cannot be turned off
     * forces every panel to fake it — by hiding the control, which moves the
     * layout, or by leaving it live and ignoring the result.  One flag, honoured
     * in the two places that matter (draw and hit-test), rather than in each
     * widget's own code where four of nine would forget.
     *
     * §M32 — AND IT MEANS NO INPUT AT ALL, not merely a dimmer drawing.  It was
     * honoured in the painter and in ONE of app_host.c's four dispatch points,
     * so a greyed control looked dead and ACTED — worse than either, because a
     * user who has been told a control is off does not expect it to fire.  All
     * four check it now (pointer, mouse, key, keycode), and a fifth dispatch
     * path must too. */
    int disabled;
    /* Pointer-over, resolved on the app-host task from an AE_HOVER position.
     * Not set for a disabled widget: the design's state row is exclusive, and
     * a control that is off must not light up under the cursor. */
    int hovered;
    /* Held down.  The design's fifth state, and the one that makes a click
     * feel connected to the screen: without it a button looks identical the
     * whole time the mouse is down on it. */
    int pressed;
};

/* ---- Label ---------------------------------------------------------------- */
struct w_label {
    struct widget base;
    char     text[96];
    /* The RIGHT-HAND half of a caption band.  The design's table footer is two
     * sided — a count on the left and a total on the right — and two labels in
     * one box would each paint their own `tray` band over the other's.  Empty
     * by default, which is the old one-sided behaviour exactly. */
    char     text2[48];
    /* §M69 — 0 means "ask the theme", which is the default and what makes a
     * live theme switch reach an existing label.
     *
     * Reported from use: *"if I change the theme the colours update, but the
     * labels in the Appearance panel stay wrong until I close and reopen it."*
     * The constructor did `l->color = WCOL_TEXT` — a macro that READS the
     * theme, evaluated ONCE — so every label held a copy of the colour that
     * was current when it was built.  The same defect as a widget storing a
     * translated string instead of a key, and the same fix: resolve at DRAW.
     *
     * A raw colour still wins when one is set, because a few callers really do
     * want a specific value; `role` is the theme-following way to be
     * secondary, and is what the status lines use. */
    uint32_t color;                     /* 0 = use `role` against the theme  */
    int      role;                      /* WLBL_* below                      */
    int caption;   /* 0 = body, 1 = header (rule below), 2 = footer (rule above) */
};
/* Switch a label to the design's caption style (UPPERCASE, tracked, `muted`,
 * on a `tray` band).  A flag rather than a second widget class: it is the same
 * text in the same box, drawn to say "this names the thing below it".
 * 1 = header, 2 = footer — the value decides which side the rule goes on. */
/* Theme ROLES for a label, resolved at draw so a theme switch reaches it. */
#define WLBL_TEXT   0
#define WLBL_MUTED  1
#define WLBL_ACCENT 2

void w_label_set_caption(struct w_label* l, int on);
void w_label_set_trailing(struct w_label* l, const char* text);

struct w_label* w_label_create(struct gui_window* win, int x, int y, int w,
                               const char* text);
void w_label_set(struct w_label* l, const char* text);

/* ---- Button ---------------------------------------------------------------- */
/* The design's four button emphases (catalogue §02).  They are not four
 * shades of the same control: PRIMARY is the action the dialog exists for and
 * carries an accent fill; SECONDARY is every other action and is a plate with
 * a border; GHOST has no box at all until you touch it, which is what lets a
 * row of tertiary actions sit in a toolbar without becoming a wall of buttons;
 * ICON is secondary in a square.  Drawing them all alike — which is what this
 * toolkit did — throws away the only thing that says which button matters. */
enum cp_btn_emphasis {
    CP_BTN_SECONDARY = 0,       /* the default: plate + 1 px border          */
    CP_BTN_PRIMARY,             /* accent fill, `on_accent` text             */
    CP_BTN_GHOST,               /* text only until hovered or focused        */
    CP_BTN_ICON,                /* square, secondary look                    */
};

struct w_button {
    struct widget base;
    char text[24];
    int  emphasis;              /* enum cp_btn_emphasis                      */
    void (*on_click)(struct w_button* b, void* ctx);
};

/* Emphasis is set after creation so the constructor keeps one signature — the
 * common case is SECONDARY and stays a plain create. */
void w_button_set_emphasis(struct w_button* b, int emphasis);

/* §M69 — move a button to (x, y) and resize it to what console_plate.h's
 * control convention says it should be: `cp_text_w(label)` plus the design's
 * horizontal padding, `cp_ctrl_h()` tall.  Returns the width taken, so a
 * hand-placed row can advance a cursor across it.
 *
 * For the apps that predate the layout engine and still position their button
 * rows by hand.  Those rows were measured for the 8x8 font, so at §M69's
 * runtime type their frames cut through their own labels.  Use `ui_build` with
 * a UI_ROW for anything new — this exists so the older windows can obey the
 * rule without being rewritten first, not as a second way to lay out. */
int w_button_autosize(struct w_button* b, int x, int y);
struct w_button* w_button_create(struct gui_window* win, int x, int y,
                                 int w, int h, const char* text,
                                 void (*on_click)(struct w_button*, void*),
                                 void* ctx);

/* ---- List view -------------------------------------------------------------- */
#define WLIST_MAX_ITEMS 96
#define WLIST_ITEM_LEN  72
/* DERIVED, NOT A CONSTANT.  This was 14 — the 8x8 glyph plus padding — and it
 * silently became wrong the moment `gui.font_scale` could move: at 2x the text
 * is 16 px in a 14 px row, so every row overlaps the next.  A metric that
 * encodes an assumed text size has to be recomputed when the text size becomes
 * a setting, and the Task Manager's list is where that showed. */
int cp_fh(void);
struct cp_density_fwd;
/* cp_row_h() is declared in console_plate.h — a density fact, not a toolkit
 * one.  Every user of this header already includes that one. */
/* THE ROW HEIGHT IS THE DENSITY'S, NOT THE FONT'S.
 *
 * It was `cp_fh() + 6` — the glyph plus padding — which is how a terminal sizes
 * a line, not how the design sizes a table.  widget_specs.md gives `row_h` its
 * own value per density (40 px comfort / 32 px compact) precisely because a row
 * is a hit target and a rhythm, not a piece of text with margins: at 8 px text
 * the font-derived version produced 14 px rows, about half the design's, and
 * the table read as a dense log rather than a list you can point at. */
#define WLIST_ROW_H     cp_row_h()

struct w_listview {
    struct widget base;
    char items[WLIST_MAX_ITEMS][WLIST_ITEM_LEN];
    uint8_t tags[WLIST_MAX_ITEMS];      /* opaque per-item tag (fs uses type) */
    int  count;
    int  sel;                           /* -1 = none                     */
    int  scroll;                        /* first visible row             */
    void (*on_activate)(struct w_listview* lv, int idx, void* ctx);  /* dbl-click */
    void (*on_select)  (struct w_listview* lv, int idx, void* ctx);  /* click     */
    /* TABULAR: draw the rows in the monospaced face.
     *
     * A listview row here is ONE space-padded string, and space padding only
     * lines up under a fixed advance — with the proportional body face the
     * columns wander, which is exactly what the Task Manager showed the moment
     * the real typeface arrived.  The design's own answer is the same one: it
     * sets tabular data in IBM Plex Mono precisely so digits align.
     *
     * A flag rather than a fix to the model because they are different jobs:
     * real columns with per-column alignment are the table view's, and a list
     * of pre-formatted lines is what this widget IS.  Making that legible is
     * worth one boolean; making it a table is a different widget. */
    int  mono;
    /* §M69 — SCROLLBAR DRAG STATE.  A grab has to remember where inside the
     * thumb the press landed, or the first motion event snaps the thumb's top
     * to the pointer and the list lurches the moment you touch it.  `sb_part`
     * also keeps the pressed arrow emphasised for as long as it is held, which
     * is the only feedback that a step actually registered. */
    int  sb_part;                       /* enum sb_part, 0 = not dragging  */
    int  sb_grab_dy;                    /* press y minus the thumb's top   */
};
struct w_listview* w_listview_create(struct gui_window* win, int x, int y,
                                     int w, int h, void* ctx);
void w_listview_clear(struct w_listview* lv);
int  w_listview_add(struct w_listview* lv, const char* text, uint8_t tag);

/* THE PER-ROW CONTENT DIFF USED TO LIVE HERE, AND IT IS GONE ON PURPOSE.
 *
 * `w_listview_dirty_run` + a 6.9 KiB `prev[96][72]` shadow copy per listview
 * existed for exactly one caller, the Task Manager, which is now a table view;
 * `w_itemview_refresh` is the same idea generalised, and it serves every model
 * and every layout instead of this one widget.  Removed rather than left
 * available: an untested mechanism that LOOKS ready is how somebody builds on
 * a path nothing has ever exercised (§M52), and this one had the extra sting of
 * costing 6.9 KiB in each of the listviews that never used it.
 *
 * The measurements that justified it are worth keeping, because they are what
 * a future diff has to beat: on a maximized Task Manager an idle desktop
 * composited in 17.6 ms and the same desktop with the table in it took
 * 38-45 ms.  Its own conclusion turned out to be wrong — the cost was the app
 * host repainting the whole window on every tick, one layer above (gui.h), not
 * the rows — which is the other reason not to leave the code standing as
 * though it had been the answer. */

/* ---- Single-line text input -------------------------------------------------- */
struct w_textinput {
    struct widget base;
    char buf[64];
    int  len;
    void (*on_submit)(struct w_textinput* t, void* ctx);   /* Enter */
    /* §M32 stage 10 — a SECRET field draws nothing of what was typed.
     *
     * Not one bullet per character: that publishes the LENGTH to anybody
     * looking at the screen, which is the same reason the shell's
     * `shell_read_secret` echoes nothing at all.  A single fixed marker says
     * "something is here" without saying how much.
     *
     * Appended at the END of the struct (§M58's rule for optional fields), and
     * zero is the ordinary visible field, so every existing text input is
     * unchanged by construction. */
    int  secret;
};
struct w_textinput* w_textinput_create(struct gui_window* win, int x, int y,
                                       int w, void* ctx);
void w_textinput_set(struct w_textinput* t, const char* text);
/* Make this field a password field.  See `secret` above for why it does not
 * draw one mark per character. */
void w_textinput_set_secret(struct w_textinput* t, int on);

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

/* The content diff's cache.  Bounded by the VIEWPORT, not by the model — a
 * 4000-entry directory costs exactly what a 12-entry one does. */
#define IV_SLOTS    64
#define IV_SIG_LEN  96

struct w_itemview {
    struct widget base;
    const struct item_model* model;
    const struct item_view*  view;
    int  sel;
    int  scroll;
    void (*on_select)(struct w_itemview* iv, int idx, void* ctx);

    /* What is CURRENTLY DRAWN in each screen slot, so a refresh can repaint the
     * slots that differ instead of the pane.  See w_itemview_refresh. */
    char rowsig[IV_SLOTS][IV_SIG_LEN];
    int  sig_slots;
    int  sig_scroll;
    int  sig_valid;

    /* §M69 — scrollbar drag state; the same three the listview keeps, plus the
     * content/viewport the VIEW reported, cached so the drag and the wheel do
     * not each have to ask again mid-gesture. */
    int  sb_part;                       /* enum sb_part, 0 = not dragging   */
    int  sb_grab_dy;                    /* press y minus the thumb's top    */
    int  sb_content, sb_viewport;
    /* §M76.2 — the column geometry at the last refresh, so a content-driven
     * re-layout can be noticed.  See iv_cols_moved in w_itemview.c. */
    int colx[8], colw[8];
};
struct w_itemview* w_itemview_create(struct gui_window* win, int x, int y,
                                     int w, int h,
                                     const struct item_model* model,
                                     const char* view_name, void* ctx);

/* Re-read the model and repaint ONLY what changed.
 *
 * For an owner whose data moves under it (the Task Manager's task list, a
 * directory being written to).  The widget cannot be told what changed — the
 * model is live — so it remembers what it drew and compares.
 *
 * THE SELECTION IS PART OF THE SIGNATURE, deliberately.  A row's text does not
 * change when it becomes the selected row, only its colours do, and a diff that
 * compared text alone would leave the highlight where it was — the exact defect
 * §M69 wrote down for the listview.  Folding it in means one mechanism serves
 * both, instead of a second path somebody has to remember to call. */
void w_itemview_refresh(struct w_itemview* iv);

/* ---- Generic helpers (used by gui.c) ----------------------------------------- */
/* Initialise a widget's base and add it to the window.  Every constructor goes
 * through this — see the note in widget.c for what happened to the one that
 * did not. */
void widget_init(struct widget* w, struct gui_window* win,
                 int x, int y, int ww, int hh,
                 const struct widget_ops* ops, void* ctx, int focusable);

void widget_draw_all(struct widget* head, struct gfx_surface* s);
struct widget* widget_at(struct widget* head, int lx, int ly);


/* §M75 — a chart's data changes without the widget being told, so its OWNER
 * damages it on its tick.  gui.h's contract since §M69 is that a tick damages
 * what it changed; this damages exactly the chart's box and never the window.
 * See kernel/gui/w_chart.c. */
void w_chart_refresh(struct widget* w);

#endif
