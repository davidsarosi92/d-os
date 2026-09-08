/* =============================================================================
 * gui_priv.h — the compositor's private definitions (§M70).
 *
 * NOT a public kernel header, and the distinction matters:
 *
 *   gui.h          — what an APP may use.  Windows are opaque there.
 *   gui_internal.h — what a DESKTOP SHELL may use (taskbar, launcher).
 *   gui_priv.h     — what the COMPOSITOR ITSELF is built out of.  Only the
 *                    files that together implement the compositor may include
 *                    it: gui.c, gterm.c, app_host.c, wm.c, gui_mode.c.
 *
 * WHY IT EXISTS.  gui.c was 5620 lines holding seven subsystems — window
 * lifecycle, damage tracking, two event queues, a complete terminal emulator,
 * the per-window app-host loop, the window manager and the compositor itself.
 * They were already SEPARATE THINGS; what held them in one file was that
 * `struct gui_window` and ninety-odd file-scope statics were private to it.
 * This header is the minimum needed to let those subsystems live in their own
 * files, and NOTHING MORE — every declaration here is one a split file
 * actually uses.
 *
 * THE THREADING CONTRACT IS UNCHANGED and is the thing to keep in mind while
 * reading across files (M22.7):
 *
 *   - the COMPOSITOR task owns compositing, `zorder`, focus and input ROUTING;
 *   - each WIN_APP window has its own APP-HOST task which owns that window's
 *     widgets, its `surf` contents and its event queue `aq`;
 *   - the mouse/keyboard IRQ only ENQUEUES — it never walks the widget tree.
 *
 * A function named `*_locked` requires `state_lock`.  §M22.7's rule still
 * holds across the split: a window's widgets belong to the task that hosts it,
 * and a cross-task write into them appears to do nothing, because damaging a
 * window is the host's job.
 * ============================================================================= */

#ifndef GUI_PRIV_H
#define GUI_PRIV_H

#include "gfx.h"
#include "lock.h"
#include "widget.h"
#include "vc.h"
#include "desktop.h"
#include "console_plate.h"
#include <stdint.h>

struct gui_input;
struct task;
struct desktop_shell;

#define GUI_MAX_WINDOWS 8

#define BORDER      CP_BORDER
#define MIN_W       160
#define MIN_H       96
#define TITLE_BTN_GAP 4
enum { TB_CLOSE = 0, TB_MAX = 1, TB_MIN = 2, TB_COUNT = 3 };
enum drag_mode { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

/* The drag in progress.  It lives with the WM because a drag is a WINDOW
 * operation, but the mouse path in gui.c starts and ends it — hence extern
 * rather than an accessor pair. */
extern enum drag_mode     drag;
extern struct gui_window* drag_win;
extern int grab_dx, grab_dy;

/* Chrome geometry and the themed colours a window's CONTENT is drawn in.
 * Read the LIVE theme on every use rather than caching: `gui.theme` can
 * change between two draws, and a value captured at construction is a copy
 * the live source can no longer reach — the defect §M69 found three times in
 * one milestone (a stored colour, a stored translation, a stored icon size). */
#define TITLE_H     cp_titlebar_h()
#define PAD         3
#define COL_WIN_BG (cp_current_theme()->surface)
#define COL_SEL_BG (cp_current_theme()->sel_bg)
#define COL_SEL_FG (cp_current_theme()->sel_fg)
#define COL_WIN_FG (cp_current_theme()->text)

enum win_kind { WIN_TERM, WIN_APP };

/* M22.7 — per-window input event (compositor produces, the window's app-host
 * task consumes).  Widget hit-testing + dispatch happens on the host, not the
 * compositor, so a slow app handler can no longer stall the whole GUI. */
enum ae_type { AE_MOUSE, AE_KEY, AE_KEYCODE, AE_BUTTON, AE_POINTER, AE_SCROLL,
               /* §M65 — a menu/combo popup was dismissed by picking item `x`
                * (-1 = dismissed without a choice).  Delivered to the window
                * that OPENED it, on its app-host task: choosing a menu item
                * runs app code, which must not happen in the mouse IRQ. */
               AE_POPUP,
               /* §M65's state row needed two states the toolkit could not
                * observe: HOVER and PRESSED.  The comment on the motion path
                * used to say widget windows get motion only on click, because
                * "a widget hit-test per mouse packet would be pointless work"
                * — true while nothing drew a hover.  The design's catalogue
                * draws five states per control, so the work has a point now.
                *
                * It is still not done in the IRQ: this only RECORDS a position
                * and the app-host task resolves which widget it lands on
                * (§M22.7's split).  A dropped hover event is harmless — the
                * next motion corrects it — which is why a full queue may
                * discard one without any repair path. */
               AE_HOVER };
struct app_event {
    uint8_t type;
    int16_t x, y;                       /* AE_MOUSE: content-relative     */
    uint8_t dbl;
    char    c;                          /* AE_KEY                         */
    uint8_t kc, mods;                   /* AE_KEYCODE                     */
    uint8_t btn, down;                  /* AE_BUTTON: 1=L 2=R 3=M, 1=press */
    uint8_t phase;                      /* AE_POINTER: WPTR_* (§M58)      */
};
#define AQ_SZ 32

struct gui_window {
    int  used;
    enum win_kind kind;
    int  x, y, w, h;                    /* outer rect (state_lock)        */
    char title[24];

    spinlock_t         lock;            /* content surface guard          */
    struct gfx_surface surf;

    /* WIN_TERM: grid cursor + char backing store + input VC. */
    int   cols, rows, ccol, crow;
    char* cells;
    struct vc* vc;

    /* §M58 — SCROLLBACK.  A ring of `sb_cap` rows, each gmax_cols wide; a row
     * evicted by a scroll is pushed here instead of being dropped.  `scrolled`
     * counts every line ever evicted, which makes it the ABSOLUTE line number
     * of the live grid's first row — and absolute line numbers are what the
     * rest of this feature is addressed in (see below). */
    char* sb;                           /* scrollback ring, or NULL       */
    int   sb_cap, sb_count, sb_head;    /* rows / valid / next write slot */
    int   scrolled;                     /* lines evicted so far = abs base */
    int   view_off;                     /* 0 = live; N = N lines back     */

    /* §M58 — text selection over the CELL GRID.  Anchored where the press
     * landed and extended by drag; -1 = no selection.
     *
     * The rows are ABSOLUTE LINE NUMBERS, not grid rows.  That is the whole
     * difference scrollback makes: a grid row is a position on the screen, and
     * one line of output arriving renumbers every one of them — so a selection
     * held in grid rows silently slides onto text the user never pointed at.
     * An absolute line names the same text forever. */
    int   sel_ar, sel_ac;               /* anchor: absolute line + column */
    int   sel_br, sel_bc;               /* current (drag) end             */
    int   sel_on;                       /* non-zero = a range exists      */

    /* WIN_APP: widgets + layout + lifetime hooks. */
    struct widget* widgets;
    struct widget* focusw;
    struct widget* grabw;               /* §M58 pointer grab (host task)  */
    void (*key_hook)(struct gui_window*, char);  /* §M61 window-level keys */
    void (*on_layout)(struct gui_window*);
    void (*on_close) (struct gui_window*);
    void* app_ctx;
    /* §M65 — the toolkit's per-window state (ui.c).  A pointer rather than a
     * side table keyed by window, so it cannot outlive the window it describes:
     * destroy_window frees it in the same place it frees app_ctx. */
    void* ui_state;

    /* §M26 — optional input sink: when set, window input is forwarded here
     * (instead of the widgets) — the Wayland server routes it to wl_seat. */
    void (*input_hook)(struct gui_window*, const struct gui_input*, void*);
    void* input_ctx;

    /* M22.7 — per-task app.  Every WIN_APP window is driven by its own
     * "app-host" task: it creates the widgets, drains this window's event
     * queue, runs on_tick/on_layout, and renders into `surf` — all off the
     * compositor.  The compositor only composites `surf` (under `lock`) and
     * routes input into `aq`.  Teardown: on want_close the host frees the
     * widgets + calls on_close + sets host_released; the compositor then
     * disposes the window struct (see apply_pending / destroy_window). */
    struct task* host_task;
    /* §M42/§M46 — a CLIENT-MANAGED window (dosgui bridge) has host_task == NULL
     * and instead records its ring-3 client's pid here, so the compositor can
     * dispose the window if that client dies WITHOUT a clean DOSGUI_DESTROY
     * (force-kill / crash).  0 for a normal app-host window. */
    int  client_pid;
    /* §M54 — "this window is gone" notification for whoever owns a handle to
     * it (the dosgui bridge).  Fired exactly once, from destroy_window, on
     * EVERY disposal route — that is the point: the bridge must not have to
     * infer the window's death from the route it happened to take. */
    void (*on_dispose)(struct gui_window*, void*);
    void*  dispose_ctx;
    struct app_event aq[AQ_SZ];
    volatile uint32_t aq_h, aq_t;
    volatile int tick_pending;          /* compositor asks host to on_tick */
    volatile int layout_pending;        /* compositor asks host to on_layout */
    volatile int host_released;         /* host cleaned up; compositor may free */

    /* M22.3 */
    int  minimized;                     /* skipped by compose + hit-test  */
    void (*on_tick)(struct gui_window*);/* APP: ~1 Hz on compositor task  */

    /* M22.5 — maximize/restore.  `maximized` windows fill the work
     * area (screen minus the shell's bottom reserve); the pre-maximize
     * outer rect is stashed for restore.  Move/resize are disabled
     * while maximized. */
    int  maximized;
    int  sav_x, sav_y, sav_w, sav_h;

    /* IRQ → compositor handoff (state_lock). */
    int  pending_w, pending_h;
    volatile int want_close;

    /* §M47.1 — closing a CLIENT-MANAGED window is a TWO-CLICK escalation:
     *   1st X click → want_close (a polite request the client should honour);
     *   2nd X click → close_force_now (the user has decided it is hung).
     * `close_deadline_ms` is only the unattended backstop, in case nobody is
     * there to click a second time.  0 = no close in flight. */
    uint64_t close_deadline_ms;
    volatile int close_force_now;
};

/* ---------------------------------------------------------------------------
 * Shared compositor state.  Everything here is defined in gui.c; the split
 * files reach it through these declarations rather than through accessors,
 * because an accessor per field would be ninety functions that do nothing and
 * would hide the LOCKING, which is the only part that is subtle.
 * ------------------------------------------------------------------------- */

extern struct gui_window windows[GUI_MAX_WINDOWS];
extern struct gui_window* zorder[GUI_MAX_WINDOWS];   /* bottom → top          */
extern int                zcount;
extern struct gui_window* focused_win;
extern spinlock_t         state_lock;                /* guards all of the above */

extern struct gfx_surface fbsurf, backsurf, wallsurf;
extern int  work_h;                    /* screen minus the shell's chrome     */
extern int  gmax_cols, gmax_rows;      /* terminal grid capacity, in cells    */
extern volatile int need_frame;
extern const struct desktop_shell* shell;

/* ---- damage (damage_lock, gui.c) ---------------------------------------- */
void gui_damage_win(struct gui_window* w);

/* ---- the terminal emulator (gterm.c) ------------------------------------ */

/* Absolute-line addressing is the whole of §M58: a grid row is a position on
 * the SCREEN and one line of output renumbers every one of them, so anything
 * that must keep naming the same text — the renderer, the hit test and the
 * copy alike — asks these rather than doing the arithmetic itself. */
const char* gterm_row(const struct gui_window* win, int abs);
int   gterm_screen_row(const struct gui_window* win, int abs);
void  gterm_emit(void* ctx, char c);
void  gterm_rerender_locked(struct gui_window* win);
void  gterm_sb_push(struct gui_window* win, const char* row);
int   gterm_view_scroll(struct gui_window* win, int dl);
void  gterm_cell_at(struct gui_window* win, int cx, int cy, int* row, int* col);
int   gterm_selection_text(struct gui_window* win, char* dst, int cap);

/* Deferred selection work.  The mouse IRQ only records a cell RANGE and sets a
 * flag; re-rendering a grid is thousands of glyph blits and `clipboard_set`
 * allocates, so the actual work runs on the compositor (§M22.7's split). */
void term_selection_service(void);
/* The window whose selection is being dragged.  WRITTEN by the pointer path
 * in gui.c and READ here — the split that keeps the IRQ side cheap. */
extern struct gui_window* term_sel_win;
extern struct gui_window* volatile term_sel_dirty;
extern struct gui_window* volatile term_sel_copy;
extern struct gui_window* volatile term_sel_copy_to_clip;
extern struct gui_window* volatile term_paste_win;

/* ---- the per-window app-host task (app_host.c) --------------------------- */

/* The entry point every WIN_APP window's own task runs.  `window_show` spawns
 * it; it owns that window's widgets and surface until `host_released`. */
void app_host_main(void);

/* Queue one event for a window's host.  Called from the input path on the
 * compositor; a position REPLACES a queued position, and a full ring
 * sacrifices the OLDEST hover rather than the newcomer — a burst of trackpad
 * motion must not be able to discard the CLICK behind it (§M69). */
void aq_push(struct gui_window* w, struct app_event e);

void app_widgets_free(struct gui_window* win);
void app_widgets_reset(struct gui_window* win);
int  app_dispatch_event(struct gui_window* win, const struct app_event* e);
void app_redraw(struct gui_window* win);

extern volatile unsigned aq_dropped, aq_coalesced;

/* ---- compositor services the host needs --------------------------------- */
void apply_pending(void);
int  gui_input_debug(void);

/* ---- runtime mode change (gui_mode.c) ------------------------------------
 * Both are QUEUED and applied by the compositor BETWEEN FRAMES: they
 * reallocate the backbuffer, and doing that while compose() is mid-blit frees
 * the buffer out from under it. */
void apply_mode_change(void);
void apply_mode_revert(void);

extern int gui_active;
extern volatile int panel_dirty;

/* ---- presentation (gui.c) -------------------------------------------------
 * The hardware page flip, when the display has room for a second scanout
 * buffer.  `flipbuf` ALIASES the two buffers; nothing here owns their pixels. */
extern int flip_ok;
extern struct gfx_surface flipbuf[2];
extern int flip_front;
extern int fb_flip_init(volatile uint32_t** buf0, volatile uint32_t** buf1);

/* ---- the desktop shell's panel strip (gui.c) ------------------------------
 * A BOTTOM STRIP and not a full-screen surface: the taskbar needs the bottom
 * rows and a full-screen second surface costs ~5 MiB for pixels nothing draws. */
#define PANEL_POPUP_MAX 480
extern struct gfx_surface panelsurf;
extern uint32_t*  panel_buf;
extern int        panel_strip_top;
extern int        panel_ready;
/* The desktop-shell task.  A window launched from the Start menu is parented
 * to it, so leaving the session closes what the session started. */
extern int        desktop_pid;
extern spinlock_t panel_lock;

#define COL_WALL_TOP (cp_current_theme()->bg)
#define COL_WALL_BOT (cp_current_theme()->bg)

/* Repaint the wallpaper into `wallsurf` at the current size (gui.c). */
int  paint_wallpaper(void);

/* ---------------------------------------------------------------------------
 * The window manager (wm.c) — "which window", as against gui.c's "which
 * pixels".  A `_locked` suffix means the caller already holds `state_lock`:
 * the mouse path does, because the IRQ took it before routing the click.
 * ------------------------------------------------------------------------- */

struct gui_window* window_alloc(const char* title, enum win_kind kind,
                                int x, int y, int w, int h);
void window_show(struct gui_window* win);
int  window_set_size(struct gui_window* win, int outer_w, int outer_h);
void destroy_window(struct gui_window* win);
int  raise_window(struct gui_window* win);

/* Create a terminal window (gui.c owns the VC binding it needs). */
struct gui_window* term_window_create(const char* title, int x, int y, int w, int h,
                                      const char* task_name, void (*entry)(void),
                                      int shell_ppid);

/* The topmost window at a point — or, while a modal is up, THE MODAL OR
 * NOTHING.  That one answer covers hover, the title-button highlight, the
 * right/middle press and the drag start, which is why modality needs only
 * four gates rather than one per input path. */
struct gui_window* topmost_at(int px, int py);
struct gui_window* top_visible_locked(void);
int  modal_hit(int px, int py);

/* Title buttons are numbered FROM THE RIGHT EDGE and their geometry comes from
 * ONE function, so the painter, the hit test and the click handler cannot
 * disagree about which boxes exist or where they are (§4.79). */
void title_btn_rect(int wx, int wy, int ww, int idx, int* bx, int* by, int* bw, int* bh);
int  title_btn_count(const struct gui_window* win);
int  title_btn_at_n(int wx, int wy, int ww, int px, int py, int count);

void toggle_maximize_locked(struct gui_window* w);

/* ---- popups (one slot: an open popup owns the next click, wherever it
 *       lands, and it must NOT also reach the window underneath) ----------- */
#define POPUP_MAX_ITEMS 16
#define POPUP_ITEM_LEN  28
#define POPUP_ROW_H     (cp_fh() + 10)

struct gui_popup_state {
    volatile int active;
    int x, y, w, h;
    char items[POPUP_MAX_ITEMS][POPUP_ITEM_LEN];
    int  count;
    int  hover;
    struct gui_window* owner;
    int  tag;
};
extern struct gui_popup_state popup;
int  popup_row_at(int sx, int sy);

/* ---- modality ------------------------------------------------------------ */
extern struct gui_window* modal_win;
extern struct gui_window* modal_prev_focus;
extern volatile unsigned modal_dbg_seen, modal_dbg_missing;

/* ---- small shared helper ------------------------------------------------- */
void str_copy(char* dst, const char* src, int cap);

#endif
