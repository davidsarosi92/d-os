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

#define COL_WIN_BG (cp_current_theme()->surface)
/* §M58 — selection wash.  Bright enough to be unambiguous over the terminal
 * background, and the text flips to dark so it stays legible. */
#define COL_SEL_BG (cp_current_theme()->sel_bg)
#define COL_SEL_FG (cp_current_theme()->sel_fg)
#define COL_WIN_FG (cp_current_theme()->text)
#define COL_TITLE_F_TOP (cp_current_theme()->raised)
#define COL_TITLE_F_BOT (cp_current_theme()->raised)
#define COL_TITLE_U_TOP (cp_current_theme()->tray)
#define COL_TITLE_U_BOT (cp_current_theme()->tray)
#define COL_BORDER_F (cp_current_theme()->accent)
#define COL_BORDER_U (cp_current_theme()->line)
#define COL_TITLE_TEXT (cp_current_theme()->text)
/* §M69 — the modal backdrop.  widget_specs.md §14 says black at 45 %, and
 * 45 % of 255 is 115 = 0x73; written as the arithmetic rather than as a
 * rounded-looking constant so the next person can check it against the spec
 * instead of trusting it. */
#define COL_MODAL_DIM   0x73000000u
#define COL_RUBBER (cp_current_theme()->accent)
#define COL_CLOSE_BG    0xFFC0392Bu
#define COL_CLOSE_FG    0xFFF8ECEAu

/* Title-button hover, and the panel's published popup extent.  Both are
 * WRITTEN by the input path and READ by the painter — which is why they are
 * shared state and not accessors.  §M23's lesson about the extent: a SECOND
 * publisher is a second thing that can forget to clear it, and a stale extent
 * swallows clicks over a window. */
extern struct gui_window* volatile tb_hover_win;
extern volatile int tb_hover_idx;
extern volatile int pnl_pop_on, pnl_pop_x, pnl_pop_y, pnl_pop_w, pnl_pop_h;

/* Pointer position and the resize rubber band: the input path writes them,
 * the painter reads them. */
extern int mx, my;
extern int rubber_w, rubber_h;

/* How many composites during a drag took the COPY path and how many fell
 * back to the painter.  BOTH numbers matter: all-fast would mean the
 * fallback is never exercised and therefore never tested, and all-slow would
 * mean the optimisation is not running while the timing improved for some
 * other reason (§4.61).  A real drag gives both — measured 37 copied, 3
 * repainted over the self-refreshing Task Manager. */
extern uint32_t drag_fast, drag_slow;

/* Cursor sprite is 11x17 px; ±1 px margin, matching draw position. */
#define CUR_DMG_X(cx)  ((cx) - 1)
#define CUR_DMG_Y(cy)  ((cy) - 1)
#define CUR_DMG_W      14
#define CUR_DMG_H      20

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

/* ---------------------------------------------------------------------------
 * Damage and compositing (compose.c) — "which pixels", as against wm.c's
 * "which window".
 *
 * Damage is a LIST OF DISJOINT RECTS, not a bounding box: compose() paints and
 * presents each separately, so a table refresh and a far-away cursor stay two
 * small blits instead of one large union (§4.61).
 * ------------------------------------------------------------------------- */
/* §perf — per-drag accounting, printed when the drag ends (gui.drag_stats=1).
 *
 * It has to be a REPORT rather than a command, because by the time anyone
 * could type `gui stats` the drag is over and its cost has been averaged into
 * everything else.  The figures are chosen to separate the two candidate
 * explanations of "dragging lags": if `moved` is far below `motions` the
 * window is being throttled, and if the compositor's own milliseconds fill the
 * elapsed time the blit is the bottleneck. */
/* §perf — THE MOVE HINT: "this window went from here to there, and nothing
 * else changed".
 *
 * A dragged window's pixels do not change — only its position does — so the
 * composited image can be COPIED from the old place to the new one instead of
 * being rebuilt out of wallpaper, shadow, chrome and content.  Measured, that
 * rebuild was 36 ms per frame for a 921x721 window against a 30 ms frame
 * budget, which is precisely what "the window trails the pointer" is made of.
 *
 * The hint is passed OUT OF BAND rather than as damage, and that is what makes
 * the optimisation safe: compose takes the fast path only when the damage list
 * is otherwise EMPTY.  Anything else that changed this frame — an app
 * repainting, a window raising, the panel — puts a rect in that list and the
 * whole thing falls back to the ordinary painter.  The alternative (inspecting
 * merged damage rects to guess whether they are "only the drag") cannot
 * distinguish a window that moved from one that moved AND redrew, and the
 * failure mode of guessing wrong is a stale image nobody can explain. */
struct move_hint {
    int  active;
    struct gui_window* win;
    int  ox, oy, nx, ny, w, h;
};
/* One-frame snapshot of the WM state, shared by every damage rect's draw. */
struct scene_snapshot {
    struct gui_window* zsnap[GUI_MAX_WINDOWS];
    int   wx[GUI_MAX_WINDOWS], wy[GUI_MAX_WINDOWS],
          ww[GUI_MAX_WINDOWS], wh[GUI_MAX_WINDOWS];
    int   zn;
    int   cx, cy;                       /* cursor */
    enum  drag_mode dsnap;
    struct gui_window* dwin;
    int   rw, rh, rrx, rry;             /* resize rubber band */
    struct gui_window* fsnap;
    /* §M69 — index in zsnap[] of the modal window, or -1.  An INDEX and not a
     * pointer because the backdrop has to be painted at a precise point in
     * the stack — over everything below the modal and under the modal itself
     * — and the paint loop is indexed. */
    int   modal_idx;
};

#define COL_SHADOW      0x48000000u
#define COL_POP_BG (cp_current_theme()->raised)
#define COL_POP_EDGE (cp_current_theme()->line)
#define COL_POP_HOVER (cp_current_theme()->hover)
#define COL_POP_SEP (cp_current_theme()->line_soft)
#define COL_POP_TEXT (cp_current_theme()->text)

/* M22.7 — a damage rectangle (used by both the damage list and the page
 * flip's previous-frame list). */
struct rect { int x0, y0, x1, y1; };
#define DMG_MAX 16

/* `mv_hint` is how a MOVE reaches the compositor: out of band, never as
 * damage.  Merged damage rects cannot tell a window that MOVED from one that
 * moved AND redrew, and the failure mode of guessing is a stale image nobody
 * can explain — so the copy path runs only when the damage list is otherwise
 * empty, and anything else falls back to the painter. */
extern struct move_hint mv_hint;          /* guarded by state_lock */
extern volatile int panel_gen;            /* bumped on WM changes  */

void gui_damage_win(struct gui_window* w);
void compose(void);
extern spinlock_t damage_lock;

/* Frame accounting.  These are READ by the instruments (gui_diag.c) and
 * written here — `gui stats` reports AREA as well as time, because a slow
 * frame is either big or fixed-cost and those want opposite fixes; without the
 * pixel count the two are indistinguishable (§M69). */
extern uint32_t frames_full, frames_partial;
extern uint64_t total_compose_ns, total_blit_px;
extern uint32_t occluded_rects, painted_rects;
extern int      g_occlude;

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
extern void fb_flip_to(int idx);

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

/* ---------------------------------------------------------------------------
 * The input router (input.c).  M22.7's rule: the IRQ only ENQUEUES — the
 * widget hit test, the dispatch and anything that allocates run on the
 * window's own app-host task.
 * ------------------------------------------------------------------------- */

/* The device-facing hooks the session installs (and clears FIRST on teardown:
 * an event delivered into a compositor being torn down is the classic
 * teardown crash). */
void gui_mouse(int nx, int ny, unsigned buttons);
void gui_wheel(int dz);
int  gui_raw_key(uint8_t keycode, uint8_t mods);
int  gui_kbd_hook(char c);

/* Drained by the compositor once per frame. */
void dispatch_events(void);
void dispatch_keys(void);
void dispatch_keycodes(void);
void pump_hostless_input(void);

/* §M46 Ctrl+Alt+X — close, then force, the top application. */
extern volatile int sak_close_req;
void sak_close_top_app(void);

#define GRIP        14
/* §perf — DRAG_MOVE recompose throttle.  Opaque window move re-blits the whole
 * (possibly large) window every mouse packet; a fast drag of a big window (e.g.
 * NetSurf) then floods the single CPU with multi-MB blits and starves everything
 * else (cursor, cron, the app itself) — the "drag froze the system" the user hit.
 * Cap the WINDOW move+damage to ~33 fps; skipped motions still move the cursor
 * (cheap), so the pointer stays smooth while the window follows at a sane rate. */
#define DRAG_FRAME_MS 30
#define PEV_CLICK  0

/* Does the DESKTOP hold keyboard focus?  §M64's gate is load-bearing: the GUI
 * suppresses the console but keys still reach its VC, which is how a command
 * is typed with the desktop up — and how this project's own harness drives
 * every GUI build.  Consuming Enter unconditionally would have made the test
 * that proves the feature its first casualty. */
extern volatile int desk_focus;

/* Panel input queue (compositor/IRQ produces, panel task consumes). */
struct pev { uint8_t type; int16_t x, y; };

#define PEV_MOTION 1
/* §M64 — a click on the desktop background (no window, no chrome). */
#define PEV_DESK_CLICK 2
#define PEV_DESK_DBL   3
/* §M64 tail — the desktop's pointer PHASES, carrying WPTR_* in the queue so
 * the shell sees §M58's vocabulary and not a second one. */
#define PEV_DESK_PRESS   4
#define PEV_DESK_DRAG    5
#define PEV_DESK_RELEASE 6
/* §M64 tail — a keycode nothing else claimed; x carries the code, y the
 * modifiers.  The queue's two fields are a point for the pointer events and a
 * key here: one more event type, not one more queue. */
#define PEV_DESK_KEY     7


/* Queue a desktop-shell pointer event (taskbar, launcher, wallpaper). */
/* The desktop shell's pointer queue: input.c PRODUCES, the desktop task
 * CONSUMES.  A pop accessor rather than a shared ring, so the single-producer
 * invariant that makes it lock-free stays enforceable in one file — §M69's own
 * first eviction attempt broke exactly that by walking the ring from the
 * consumer's index inside the mouse IRQ. */
void pevq_push(uint8_t type, int x, int y);
int  pevq_pop(struct pev* out);          /* 0 = queue empty */

/* Ring statistics, reported by `gui stats`.  A dropped-event line is what
 * makes the next occurrence of a silent drop name itself. */
extern volatile unsigned evq_dropped, evq_coalesced;

/* ---- the instruments (gui_diag.c) ----------------------------------------
 * Drained once per frame by the compositor, for the reason in that file: a
 * report printed on the REQUESTING task goes to the suppressed console. */
void gui_diag_service(void);

/* Desktop-task counters.  "The taskbar is not updating" has THREE causes that
 * look identical from outside — the loop is not running, it never marks itself
 * dirty, or its damage never reaches the compositor — so `gui stats` reports
 * all three separately (§4.67.1). */
extern volatile uint32_t desk_iters, desk_draws, desk_events;
extern volatile uint32_t desk_ticks, desk_tick_dirty, desk_now_ms;

#endif
