/* =============================================================================
 * gui.c — compositor + window manager CORE (M22, M22.1, M22.2).
 *
 * After the M22.2 modularity cut this file owns ONLY:
 *   - surfaces, backbuffer, wallpaper, cursor, composition
 *   - windows (terminal + app kinds), z-order, focus, drag/resize
 *   - input routing (mouse IRQ, keyboard hook) + IRQ→task queues
 *   - the app/desktop-shell REGISTRIES (walk helpers + launch queue)
 *
 * Everything that looks like a desktop — taskbar, launcher menu,
 * clock — lives behind `struct desktop_shell` (desktop.h) and is
 * picked at gui_start by the `gui.shell` config key.  Apps register
 * with GUI_APP() (gui_app.h); this file references no app by symbol.
 *
 * Threading model (three actors, three lock scopes):
 *
 *   shell task(s)   — gterm_emit renders shell output into the window's
 *                     content surface.       Holds:  win->lock.
 *   mouse/kbd IRQ   — updates cursor, focus, z-order, drag state,
 *                     pending resize/close; forwards chrome events to
 *                     the desktop shell; ENQUEUES app events / keys /
 *                     launches.              Holds:  state_lock.
 *   compositor task — drains the queues (widget callbacks + app
 *                     launches run HERE), applies pending resizes +
 *                     closes, composes, calls shell->draw, blits.
 *
 * Damage model: `need_frame` global flag; the shell's second_tick
 * (clock) may also raise it.  Terminal windows keep a char backing
 * store so resize re-renders content (M22.1).
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "desktop.h"
#include "gui_internal.h"
#include "gfx.h"
#include "fb_present.h"                 /* fb_present_flush — virtio-gpu scanout push (M21) */
#include "widget.h"
#include "ui.h"
#include "vc.h"
#include "task.h"
#include "lock.h"
#include "mouse.h"
#include "timer.h"
#include "config.h"
#include "wallpaper.h"          /* §M60: the desktop background source */
#include "clipboard.h"          /* §M58/§M59: selection → primary */
#include "klog.h"               /* §M61: a refused gui.mode is a warning */
#include "keymap.h"          /* M22.3: Alt-Tab raw keycodes */
#include "version.h"         /* DOS_LABEL — the desktop milestone label */
#include "kmalloc.h"
#include "printf.h"
#include "hal.h"
#include "hal_api.h"
#include <stddef.h>
#include <stdint.h>

/* S.1: terminal windows spawn the ACTIVE shell provider — no direct
 * symbol reference to any particular shell implementation. */
#include "shell_provider.h"

/* -------------------------------------------------------------------------- */
/* Metrics + palette (window chrome only — desktop chrome is the shell's).    */
/* -------------------------------------------------------------------------- */

#define GUI_MAX_WINDOWS 8

#define BORDER      2
#define TITLE_H     18
#define GRIP        14
#define PAD         3
#define MIN_W       160
#define MIN_H       96
#define CLOSE_W     14
#define CLOSE_H     11

#define COL_WALL_TOP    0xFF10243Eu
#define COL_WALL_BOT    0xFF1B5E63u
#define COL_WIN_BG      0xFF101828u
/* §M58 — selection wash.  Bright enough to be unambiguous over the terminal
 * background, and the text flips to dark so it stays legible. */
#define COL_SEL_BG      0xFF3D6FB8u
#define COL_SEL_FG      0xFFFFFFFFu
#define COL_WIN_FG      0xFFE0E0E0u
#define COL_TITLE_F_TOP 0xFF3D7BD8u
#define COL_TITLE_F_BOT 0xFF29579Eu
#define COL_TITLE_U_TOP 0xFF4A5568u
#define COL_TITLE_U_BOT 0xFF353D49u
#define COL_BORDER_F    0xFF3D7BD8u
#define COL_BORDER_U    0xFF3A424Eu
#define COL_TITLE_TEXT  0xFFF2F5FAu
#define COL_SHADOW      0x48000000u
#define COL_RUBBER      0xFFE8C25Au
#define COL_CLOSE_BG    0xFFC0392Bu
#define COL_CLOSE_FG    0xFFF8ECEAu
/* §M65 popup palette — deliberately the same values the Start menu uses in
 * shell_vista.c: two menus that look different are two menus, and a shared
 * header for four colours would be a header for four colours. */
#define COL_POP_BG      0xFF1B2434u
#define COL_POP_EDGE    0xFF44536Bu
#define COL_POP_HOVER   0xFF2C5B9Eu
#define COL_POP_SEP     0xFF3A465Cu
#define COL_POP_TEXT    0xFFE6ECF5u

/* -------------------------------------------------------------------------- */
/* Window object.                                                              */
/* -------------------------------------------------------------------------- */

enum win_kind { WIN_TERM, WIN_APP };

/* M22.7 — per-window input event (compositor produces, the window's app-host
 * task consumes).  Widget hit-testing + dispatch happens on the host, not the
 * compositor, so a slow app handler can no longer stall the whole GUI. */
enum ae_type { AE_MOUSE, AE_KEY, AE_KEYCODE, AE_BUTTON, AE_POINTER, AE_SCROLL,
               /* §M65 — a menu/combo popup was dismissed by picking item `x`
                * (-1 = dismissed without a choice).  Delivered to the window
                * that OPENED it, on its app-host task: choosing a menu item
                * runs app code, which must not happen in the mouse IRQ. */
               AE_POPUP };
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

static struct gui_window windows[GUI_MAX_WINDOWS];

/* Z-order, bottom → top (state_lock). */
static struct gui_window* zorder[GUI_MAX_WINDOWS];
static int                zcount = 0;
static struct gui_window* focused_win = NULL;

/* WM / pointer state (state_lock; IRQ writer). */
static spinlock_t state_lock;
static int mx, my;
static unsigned btn_prev = 0;
enum drag_mode { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
static enum drag_mode      drag = DRAG_NONE;
static struct gui_window*  drag_win = NULL;
static int grab_dx, grab_dy;
static int rubber_w, rubber_h;
/* §perf — DRAG_MOVE recompose throttle.  Opaque window move re-blits the whole
 * (possibly large) window every mouse packet; a fast drag of a big window (e.g.
 * NetSurf) then floods the single CPU with multi-MB blits and starves everything
 * else (cursor, cron, the app itself) — the "drag froze the system" the user hit.
 * Cap the WINDOW move+damage to ~33 fps; skipped motions still move the cursor
 * (cheap), so the pointer stays smooth while the window follows at a sane rate. */
#define DRAG_FRAME_MS 30
static uint64_t last_drag_frame_ms = 0;

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
static struct move_hint mv_hint;               /* guarded by state_lock      */

static uint64_t drag_t0_ms, drag_compose0_ns;
static uint32_t drag_motions, drag_frames, drag_f0;
/* How many composites took the copy path and how many fell back.  Both
 * numbers matter: all-fast would mean the fallback is never exercised (and so
 * never tested), and all-slow would mean the optimisation is not running at
 * all while the timing appears to improve for some other reason. */
static uint32_t drag_fast, drag_slow;
static uint64_t drag_px0;

/* Double-click tracking (IRQ only). */
static uint64_t lastclick_ms = 0;
static int lastclick_x = -100, lastclick_y = -100;
static struct gui_window* lastclick_win = NULL;

/* Scene. */
static struct gfx_surface fbsurf, backsurf, wallsurf;
static int work_h = 0;                  /* screen minus shell chrome     */
static int gmax_cols = 0, gmax_rows = 0;
/* §M46 — see gui_start: X on a package window force-kills a wedged client.
 * §M47.1 — but that is the FALLBACK, not the close path.  Killing on the first
 * compositor pass meant the X never gave the client a chance to notice the close
 * event and quit by itself, so an ordinary "close the browser" was reported as a
 * forced kill — a crash record, and (since §M47 stage 2) a Crash Reports window
 * popping up as though something had gone wrong.
 *
 * The escalation is now the USER's, which is the familiar desktop contract:
 *   1st X click → ask the client to close;
 *   2nd X click → it clearly is not going to, so force it, immediately.
 * `close_grace_ms` remains only as an UNATTENDED backstop for the case where
 * nobody is there to click again, so it is deliberately generous — long enough
 * that it never pre-empts a client that is merely slow to shut down. */
static int close_forces_kill = 1;
static unsigned close_grace_ms = 10000;

/* M22.6 — tear-free presentation via a Bochs-VBE double buffer (see
 * fb_terminal.c).  When `flip_ok`, compose() copies the dirty region from
 * backsurf into the currently HIDDEN scanout buffer and pans to it, instead
 * of blitting straight into the live scanout.  QEMU then never reads a
 * half-updated frame — no mid-scanout shear.  flip_ok==0 keeps the legacy
 * single-buffer direct blit (real hardware / non-Bochs display). */
extern int  fb_flip_init(volatile uint32_t** buf0, volatile uint32_t** buf1);
extern void fb_flip_to(int idx);

/* M22.7 — a damage rectangle (used by both the damage list and the page
 * flip's previous-frame list). */
struct rect { int x0, y0, x1, y1; };
#define DMG_MAX 16

static int flip_ok = 0;
static struct gfx_surface flipbuf[2];   /* alias the two scanout buffers  */
static int flip_front = 0;              /* buffer index currently visible */
/* Last present's dirty rects.  A page flip has buffer-age 2: the hidden
 * buffer is stale outside the regions touched in the last TWO presents, so
 * each present copies this frame's rects ∪ last frame's rects. */
static struct rect prev_dmg[DMG_MAX + 2];       /* +2 for the cursor rects   */
static int         prev_dmg_n = 0;

static volatile int need_frame = 0;
static int gui_active = 0;

/* M22.7-B — the desktop shell (taskbar/launcher/clock) runs on its OWN
 * "desktop" task and renders into a full-screen `panelsurf` at screen
 * coordinates (so the shell's draw code is unchanged).  The compositor
 * composites only the OPAQUE parts of it — the taskbar strip (always) and
 * the launcher popup rect (when open) — on top of the windows, so the rest
 * of panelsurf never occludes anything.  Input in those regions is routed
 * to the panel task's queue; the shell's click/motion run there (under
 * state_lock, which they assume held) instead of in the mouse IRQ. */
/* panelsurf is addressed in SCREEN coordinates (so the shell's draw code is
 * unchanged) but only the bottom `strip` is actually backed by memory: the
 * taskbar reserve + PANEL_POPUP_MAX for the launcher.  `px` points
 * `panel_strip_top` rows "before" the real allocation so screen-row Y lands
 * on backed row Y-strip_top; the clip keeps draws inside the strip.  Saves
 * ~5 MiB versus a full-screen panel at 1920×1200. */
#define PANEL_POPUP_MAX 480
static struct gfx_surface panelsurf;
static uint32_t*    panel_buf = NULL;           /* real allocation base      */
static int          panel_strip_top = 0;        /* first backed screen row   */
static int          panel_ready = 0;
static spinlock_t   panel_lock;
/* pid of the desktop task (0 until spawned).  Launched session terminals
 * are parented here so they belong to the desktop session (M22.7). */
static int          desktop_pid = 0;
static volatile int panel_dirty = 1;            /* shell needs a redraw     */
static volatile int panel_gen = 0;              /* bumped on WM changes     */
/* ===========================================================================
 * §M65 — THE WINDOW POPUP: one overlay above every window.
 *
 * The system had exactly one popup before this — the Start menu — and it
 * belongs to the PANEL (gui_panel_set_popup, composited out of panelsurf).  A
 * window menu and a combo box need the same thing and could not have it.
 *
 * ONE slot, not a stack, because one is the truth: a popup is modal by nature
 * (the next click either picks from it or dismisses it), and a second one open
 * at the same time would have no way to say which owns the pointer.
 *
 * It has its OWN small surface rather than drawing into the window beneath it:
 * a menu that is clipped to its window is not a menu, and painting onto the
 * back buffer directly would be erased by the next compose of anything under
 * it.
 * ========================================================================= */
#define POPUP_MAX_ITEMS 16
#define POPUP_ITEM_LEN  28
#define POPUP_ROW_H     18

static struct {
    volatile int active;
    int x, y, w, h;                     /* screen rect                       */
    char items[POPUP_MAX_ITEMS][POPUP_ITEM_LEN];
    int  count;
    int  hover;                         /* -1 = none                         */
    struct gui_window* owner;           /* who gets the AE_POPUP event       */
    int  tag;                           /* echoed back, so one handler can
                                         * tell WHICH menu was open          */
} popup;

/* Published launcher-popup extent (set by the shell via gui_panel_set_popup)
 * — read by the compositor (what to composite) and input routing. */
static volatile int pnl_pop_on = 0;
static volatile int pnl_pop_x = 0, pnl_pop_y = 0, pnl_pop_w = 0, pnl_pop_h = 0;

/* Panel input queue (compositor/IRQ produces, panel task consumes). */
struct pev { uint8_t type; int16_t x, y; };
#define PEV_CLICK  0
#define PEV_MOTION 1
/* §M64 — a click on the desktop background (no window, no chrome). */
#define PEV_DESK_CLICK 2
#define PEV_DESK_DBL   3
#define PEVQ_SZ 32
static struct pev        pevq[PEVQ_SZ];
static volatile uint32_t pevq_h = 0, pevq_t = 0;

/* M22.4 — set by the task-lifecycle hook (any context) and consumed by
 * the compositor loop: run every window's on_tick NOW instead of at
 * the next 1 Hz beat, so a closed/killed program vanishes from the
 * Task Manager within one frame. */
static volatile int tasks_changed = 0;

static void gui_task_change_hook(void) {
    tasks_changed = 1;
    need_frame = 1;
}

/* M22.7 — damage tracking as a LIST of disjoint rects (was a single
 * bounding box).  A single box merged far-apart damages — a Task Manager
 * refresh in one corner and the cursor in another — into their bounding
 * box, so the compositor re-blitted a huge diagonal region every refresh
 * and the cursor visibly stuttered.  A list composites each small rect on
 * its own, so two disjoint updates stay two small blits.  Rects accumulate
 * under damage_lock (nested inside state_lock on the mouse path — never the
 * other way).  full/partial counters back the `gui stats` command. */
static spinlock_t damage_lock;
static struct rect dmg_list[DMG_MAX];
static int         dmg_n = 0;
static uint32_t frames_full = 0, frames_partial = 0;
static uint64_t total_blit_px = 0;              /* M22.7 — avg damage/frame */
/* §perf — how much TIME the compositor spends, not just how many pixels it
 * moves.  "The desktop lags when I drag a big window" is a statement about
 * duration, and pixels are only a proxy for it: the same rectangle costs a
 * different number of milliseconds on a 4 GHz core and under emulation. */
static uint64_t total_compose_ns = 0;

static int rects_overlap(const struct rect* r, int x0, int y0, int x1, int y1) {
    return !(x0 >= r->x1 || x1 <= r->x0 || y0 >= r->y1 || y1 <= r->y0);
}
static void rect_grow(struct rect* r, int x0, int y0, int x1, int y1) {
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}
static long rect_area(const struct rect* r) {
    return (long)(r->x1 - r->x0) * (r->y1 - r->y0);
}

/* Add a damage rect: merge into an OVERLAPPING existing rect (so we never
 * composite the same pixels twice), else append; if the list is full, fold
 * it into the rect whose area grows least (bounded degradation). */
static void damage_add_locked(int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    for (int i = 0; i < dmg_n; i++)
        if (rects_overlap(&dmg_list[i], x0, y0, x1, y1)) {
            rect_grow(&dmg_list[i], x0, y0, x1, y1);
            return;
        }
    if (dmg_n < DMG_MAX) {
        dmg_list[dmg_n++] = (struct rect){ x0, y0, x1, y1 };
        return;
    }
    int best = 0; long best_cost = -1;
    for (int i = 0; i < dmg_n; i++) {
        struct rect g = dmg_list[i];
        rect_grow(&g, x0, y0, x1, y1);
        long cost = rect_area(&g) - rect_area(&dmg_list[i]);
        if (best_cost < 0 || cost < best_cost) { best_cost = cost; best = i; }
    }
    rect_grow(&dmg_list[best], x0, y0, x1, y1);
}

void gui_damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    uint32_t fl = spin_lock_irqsave(&damage_lock);
    damage_add_locked(x, y, x + w, y + h);
    spin_unlock_irqrestore(&damage_lock, fl);
    need_frame = 1;
}

void gui_damage_all(void) {
    uint32_t fl = spin_lock_irqsave(&damage_lock);
    dmg_n = 0;                                  /* collapse to one full rect */
    damage_add_locked(0, 0, fbsurf.w, fbsurf.h);
    spin_unlock_irqrestore(&damage_lock, fl);
    need_frame = 1;
    panel_gen++;            /* M22.7-B — WM-ish change; nudge the taskbar */
}

/* Window rect + margin for border/shadow (+5 shadow, +2 safety). */
static void gui_damage_win(struct gui_window* w) {
    gui_damage(w->x - 2, w->y - 2, w->w + 9, w->h + 9);
}

void gui_get_stats(uint32_t* full, uint32_t* partial, uint32_t* avg_kb) {
    if (full)    *full    = frames_full;
    if (partial) *partial = frames_partial;
    if (avg_kb) {
        uint32_t frames = frames_full + frames_partial;
        *avg_kb = frames ? (uint32_t)(total_blit_px * 4 / 1024 / frames) : 0;
    }
}

/* Active desktop shell (chosen once at gui_start). */
static const struct desktop_shell* shell = NULL;

/* §M58 — POINTER GRAB.  From a press on a widget that wants the pointer
 * stream until the release, motion goes to THAT widget even when the pointer
 * has left it (or the window).  Without a grab a selection stops at the
 * widget's edge, which is precisely where a user drags to.
 *
 * Written by the mouse IRQ under state_lock, read there too; the compositor
 * only ever sees the events it produces. */
static struct gui_window* grab_win = NULL;
/* §M58 — the terminal window currently being selected in, and one that needs a
 * re-render because its selection changed.  Both are set in the mouse IRQ under
 * state_lock and consumed by the compositor: re-rendering a whole terminal grid
 * is far too much work for an interrupt. */
static struct gui_window* term_sel_win   = NULL;
static struct gui_window* volatile term_sel_dirty = NULL;
static struct gui_window* volatile term_sel_copy  = NULL;
static struct gui_window* volatile term_paste_win = NULL;
/* §M59 — Ctrl+Shift+C: the selection goes to the EXPLICIT clipboard (the drag
 * alone only fills the primary slot). */
static struct gui_window* volatile term_sel_copy_to_clip = NULL;

/* ---- IRQ → compositor queues (SPSC: IRQ produces, compositor consumes) ---- */

struct gev {
    struct gui_window* win;
    int16_t x, y;                       /* content-relative              */
    uint8_t dbl;
    uint8_t btn, down;                  /* 0 = motion; else button + edge */
    uint8_t ptr;                        /* §M58: 0 = none, else WPTR_*+1  */
    int8_t  dz;                         /* §M61: wheel delta, 0 = none    */
};
#define EVQ_SZ 32
static struct gev        evq[EVQ_SZ];
static volatile uint32_t evq_h = 0, evq_t = 0;

#define KEYQ_SZ 32
static volatile char     keyq[KEYQ_SZ];
static volatile uint32_t keyq_h = 0, keyq_t = 0;

/* M22.5 — raw keycode queue (nav/editing keys + Ctrl shortcuts for the
 * focused APP window).  Entry: kc | mods << 8.  Same SPSC shape as
 * keyq: keyboard IRQ produces, compositor consumes. */
#define KCQ_SZ 32
static volatile uint16_t kcq[KCQ_SZ];
static volatile uint32_t kcq_h = 0, kcq_t = 0;

/* App-launch queue + power request (shell chrome → compositor task). */
#define LQ_SZ 8
static const struct gui_app_def* volatile launchq[LQ_SZ];
static volatile uint32_t lq_h = 0, lq_t = 0;
/* §M61 — the same queue for an ANONYMOUS opener: a window that is not an app
 * in the launcher (the confirm-or-revert dialog) still has to be built on its
 * own APP-HOST task, because `gui_app_window_create` binds the window to
 * `task_current()` and only an app-host loop runs `on_layout` / `on_tick`.
 * Creating one from the compositor or a shell produced a window that never
 * laid out and never ticked — an empty box with a live countdown behind it. */
static void (* volatile openq[LQ_SZ])(void);
static volatile uint32_t oq_h = 0, oq_t = 0;
static volatile int power_req = 0;      /* 0 none / 1 reboot / 2 shutdown */
static volatile int exit_req  = 0;      /* Start → Exit GUI: end the session   */
static void gui_stop_main(void);        /* teardown task; defined by gui_stop  */
static volatile int sak_close_req = 0;  /* §M46 Ctrl+Alt+X — close/force top app */

/* `btn` 0 = plain motion; otherwise the button index, with `down` saying
 * press or release. */
static void evq_push(struct gui_window* w, int cx, int cy, int dbl,
                     int btn, int down) {
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) return;
    evq[evq_h].win = w;
    evq[evq_h].x = (int16_t)cx;
    evq[evq_h].y = (int16_t)cy;
    evq[evq_h].dbl = (uint8_t)dbl;
    evq[evq_h].btn = (uint8_t)btn;
    evq[evq_h].down = (uint8_t)down;
    evq[evq_h].ptr = 0;
    evq[evq_h].dz = 0;
    evq_h = n;
}

/* §M58 — push a pointer PHASE event (press / drag / release) for a widget
 * window.  Separate from evq_push because the two carry different meanings
 * through the same ring and conflating them is how a click becomes a drag. */
static void evq_push_ptr(struct gui_window* w, int cx, int cy, int phase) {
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) return;
    evq[evq_h].win = w;
    evq[evq_h].x = (int16_t)cx;
    evq[evq_h].y = (int16_t)cy;
    evq[evq_h].dbl = 0;
    evq[evq_h].btn = 0;
    evq[evq_h].down = 0;
    evq[evq_h].ptr = (uint8_t)(phase + 1);
    evq[evq_h].dz = 0;
    evq_h = n;
}

/* §M61 follow-up — a wheel event for the window under the pointer. */
static void evq_push_wheel(struct gui_window* w, int cx, int cy, int dz) {
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) return;
    evq[evq_h].win = w;
    evq[evq_h].x = (int16_t)cx;
    evq[evq_h].y = (int16_t)cy;
    evq[evq_h].dbl = 0;
    evq[evq_h].btn = 0;
    evq[evq_h].down = 0;
    evq[evq_h].ptr = 0;
    evq[evq_h].dz = (int8_t)dz;
    evq_h = n;
}

static struct gui_window* topmost_at(int px, int py);

/* Mouse-wheel listener (IRQ).  Finds the window under the cursor and queues;
 * the widget hit-test happens on the app-host like every other input. */
/* Defined further down with the rest of the terminal-grid code; needed here by
 * the wheel listener, which is the IRQ half of the same feature. */
static int gterm_view_scroll(struct gui_window* win, int dl);

static void gui_wheel(int dz) {
    if (!gui_active || !dz) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    struct gui_window* win = topmost_at(mx, my);
    if (win && win->kind == WIN_APP && !win->minimized && my >= win->y + TITLE_H)
        evq_push_wheel(win, mx - win->x - BORDER, my - win->y - TITLE_H, dz);
    else if (win && win->kind == WIN_TERM && !win->minimized &&
             my >= win->y + TITLE_H) {
        /* §M58 — the wheel over a terminal moves its SCROLLBACK.  Three lines
         * per notch is the convention everywhere else and the reason is that a
         * notch is a coarse gesture: one line per notch makes reading a page of
         * history a wrist exercise.
         *
         * The IRQ only moves the offset; the re-render (thousands of glyph
         * blits) runs on the compositor through the same flag the selection
         * uses (§M22.7's split — the interrupt records, the compositor works). */
        if (gterm_view_scroll(win, dz > 0 ? 3 : -3)) term_sel_dirty = win;
    }
    spin_unlock_irqrestore(&state_lock, fl);
    need_frame = 1;
}

/* -------------------------------------------------------------------------- */
/* App registry walk helpers (gui_app.h).                                      */
/* -------------------------------------------------------------------------- */

/* §M65 — the toolkit's per-window slot.  Accessors rather than a public field
 * so ui.c does not need gui.c's private window struct. */
/* Which widget has the keyboard right now — controls draw their focus ring
 * from it.  Read-only; focus is CHANGED through gui_window_focus_widget. */
struct widget* gui_window_focused_widget(struct gui_window* win) {
    return win ? win->focusw : NULL;
}

/* §M65 — where this window's CONTENT starts on screen.  A widget knows its
 * position inside the content; the popup is a compositor overlay in screen
 * coordinates, and something has to bridge the two.  Exposed rather than
 * exporting BORDER/TITLE_H, so the chrome's geometry stays gui.c's business. */
void gui_window_content_origin(struct gui_window* win, int* sx, int* sy) {
    if (sx) *sx = win ? win->x + BORDER : 0;
    if (sy) *sy = win ? win->y + TITLE_H : 0;
}

/* §M65 — ask for this window's widgets to be laid out and repainted.  The
 * app-host does it for a hosted window, the compositor for a hostless one. */
/* Move the keyboard focus to the next (or previous) FOCUSABLE widget, wrapping.
 * The window's widget list is in creation order, which is the order the layout
 * placed them and therefore the order a person reads them in. */
void gui_window_focus_cycle(struct gui_window* win, int backwards) {
    if (!win || !win->used) return;
    struct widget* first = NULL;
    struct widget* prev  = NULL;
    struct widget* pick  = NULL;
    int take_next = (win->focusw == NULL);

    for (struct widget* w = win->widgets; w; w = w->next) {
        if (!w->focusable) continue;
        if (!first) first = w;
        if (backwards) {
            if (w == win->focusw) { pick = prev; break; }
            prev = w;
        } else {
            if (take_next) { pick = w; break; }
            if (w == win->focusw) take_next = 1;
        }
    }
    if (!pick) {
        /* Wrapped: forwards lands on the first, backwards on the last. */
        if (backwards) { for (struct widget* w = win->widgets; w; w = w->next)
                             if (w->focusable) pick = w; }
        else pick = first;
    }
    if (!pick) return;                      /* nothing focusable in this window */
    gui_window_focus_widget(win, pick);
    gui_window_request_redraw(win);
}

void gui_window_request_layout(struct gui_window* win) {
    if (!win || !win->used) return;
    win->layout_pending = 1;
    need_frame = 1;
}

void* gui_window_ui(struct gui_window* win)            { return win ? win->ui_state : NULL; }
void  gui_window_set_ui(struct gui_window* win, void* p) { if (win) win->ui_state = p; }

int gui_app_count(void) {
    return (int)(__stop_gui_apps - __start_gui_apps);
}

const struct gui_app_def* gui_app_at(int idx) {
    if (idx < 0 || idx >= gui_app_count()) return NULL;
    return &__start_gui_apps[idx];
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

const struct gui_app_def* gui_app_find(const char* name) {
    if (!name || !*name) return NULL;
    /* Exact (case-insensitive) first, then unique-enough prefix. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < gui_app_count(); i++) {
            const char* a = __start_gui_apps[i].name;
            const char* b = name;
            while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
            if (*b == 0 && (pass == 1 || *a == 0))
                return &__start_gui_apps[i];
        }
    }
    return NULL;
}

/* M22.5 — extension → app association (see gui_app.h). */
const struct gui_app_def* gui_app_for_path(const char* path) {
    if (!path) return NULL;
    const char* ext = NULL;                     /* after the last '.' */
    for (const char* p = path; *p; p++) {
        if (*p == '.')      ext = p + 1;
        else if (*p == '/') ext = NULL;         /* dot belonged to a dir */
    }
    if (!ext || !*ext) return NULL;

    for (int i = 0; i < gui_app_count(); i++) {
        const char* list = __start_gui_apps[i].extensions;
        if (!list || !__start_gui_apps[i].open_path) continue;
        const char* p = list;
        while (*p) {
            while (*p == ' ') p++;
            const char* a = p;                  /* one list entry */
            const char* b = ext;
            while (*a && *a != ' ' && *b && lower(*a) == lower(*b)) { a++; b++; }
            if ((*a == 0 || *a == ' ') && *b == 0)
                return &__start_gui_apps[i];
            while (*p && *p != ' ') p++;
        }
    }
    return NULL;
}

/* ---- gui_internal.h services ---------------------------------------------- */

int gui_wm_windows_locked(struct gui_window** out, int max) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS && n < max; i++)
        if (windows[i].used) out[n++] = &windows[i];
    return n;
}

int gui_wm_windows(struct gui_window** out, int max) {
    uint32_t fl = spin_lock_irqsave(&state_lock);
    int n = gui_wm_windows_locked(out, max);
    spin_unlock_irqrestore(&state_lock, fl);
    return n;
}

struct gui_window* gui_wm_focused(void) { return focused_win; }

const char* gui_window_title(struct gui_window* w) {
    return w ? w->title : "";
}

void gui_queue_launch(const struct gui_app_def* app) {
    if (!app) return;
    uint32_t n = (lq_h + 1) % LQ_SZ;
    if (n == lq_t) return;
    launchq[lq_h] = app;
    lq_h = n;
    need_frame = 1;
}

/* §M46 — Ctrl+Alt+X: request the compositor close the top-most app window.  Set
 * from the keyboard IRQ (secure-attention key), acted on by the compositor task
 * (never the possibly-frozen app), so the combo works even when an app is wedged.
 * IRQ-safe: a single volatile store, no lock/alloc. */
void gui_request_close_last(void) {
    sak_close_req = 1;
    need_frame = 1;
}

void gui_queue_power(int reboot) {
    power_req = reboot ? 1 : 2;
    need_frame = 1;
}

/* End the GUI session and go back to the text console.  Only a flag is set
 * here: this is called from the chrome (a click, dispatched on the compositor)
 * and possibly from an IRQ, and the teardown KILLS the compositor.  See
 * gui_stop_main for the task that actually does it. */
void gui_queue_exit(void) {
    exit_req = 1;
    need_frame = 1;
}

void gui_request_frame(void) { gui_damage_all(); }

int gui_screen_w(void) { return fbsurf.w; }
int gui_screen_h(void) { return fbsurf.h; }

/* -------------------------------------------------------------------------- */
/* Small utils.                                                                */
/* -------------------------------------------------------------------------- */

static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void draw_rect_outline(struct gfx_surface* s, int x, int y, int w, int h,
                              int t, uint32_t c) {
    gfx_fill(s, x,         y,         w, t, c);
    gfx_fill(s, x,         y + h - t, w, t, c);
    gfx_fill(s, x,         y,         t, h, c);
    gfx_fill(s, x + w - t, y,         t, h, c);
}

/* -------------------------------------------------------------------------- */
/* Terminal-in-a-window ("gterm").                                             */
/* -------------------------------------------------------------------------- */

/* §M58 — is cell (row,col) inside the selection?  The range is LINEAR in
 * reading order, not a rectangle: selecting from the middle of one line to the
 * middle of the next must take the end of the first line and the start of the
 * second, which is what a person means by "from here to there".  A rectangular
 * selection is a different (also useful) feature and would need its own
 * modifier — it is not this one wearing the wrong maths. */
/* The row holding ABSOLUTE line `abs`, or NULL if it has scrolled out of the
 * kept history (or is below the live grid).  ONE lookup for every reader —
 * renderer, selection and copy all go through it, so "where does this line
 * live" is answered in a single place rather than three that can disagree. */
static const char* gterm_row(const struct gui_window* win, int abs) {
    int rel = abs - win->scrolled;
    if (rel >= 0)
        return (rel < win->rows && win->cells)
               ? win->cells + (size_t)rel * gmax_cols : NULL;
    int back = -rel;                            /* 1 = most recently evicted */
    if (!win->sb || win->sb_cap <= 0 || back > win->sb_count) return NULL;
    int i = (win->sb_head - back) % win->sb_cap;
    if (i < 0) i += win->sb_cap;
    return win->sb + (size_t)i * gmax_cols;
}

/* Screen row currently showing absolute line `abs`, or -1 if it is off view. */
static int gterm_screen_row(const struct gui_window* win, int abs) {
    int v = abs - (win->scrolled - win->view_off);
    return (v >= 0 && v < win->rows) ? v : -1;
}

/* Push the live grid's top row into the ring (called just before a scroll
 * discards it).  Silently a no-op without scrollback, which is what makes
 * `gui.scrollback = 0` a supported configuration rather than a broken one. */
static void gterm_sb_push(struct gui_window* win, const char* row) {
    if (!win->sb || win->sb_cap <= 0) return;
    char* d = win->sb + (size_t)win->sb_head * gmax_cols;
    for (int c = 0; c < gmax_cols; c++) d[c] = row[c];
    win->sb_head = (win->sb_head + 1) % win->sb_cap;
    if (win->sb_count < win->sb_cap) win->sb_count++;
}

static int gterm_cell_selected(const struct gui_window* win, int row, int col) {
    if (!win->sel_on) return 0;
    int ar = win->sel_ar, ac = win->sel_ac, br = win->sel_br, bc = win->sel_bc;
    if (br < ar || (br == ar && bc < ac)) {          /* dragged backwards */
        int tr = ar, tc = ac; ar = br; ac = bc; br = tr; bc = tc;
    }
    if (row < ar || row > br) return 0;
    if (row == ar && col < ac) return 0;
    if (row == br && col >= bc) return 0;            /* end is EXCLUSIVE */
    return 1;
}

/* `row` is an ABSOLUTE line number.  A cell whose line is not on screen right
 * now (the user has scrolled back) is not drawn at all — without this check the
 * live shell would keep painting its output over the history being read, which
 * is the one thing scrollback exists to prevent. */
static void gterm_draw_cell(struct gui_window* win, int col, int row, char c) {
    int v = gterm_screen_row(win, row);
    if (v < 0) return;
    int px = PAD + col * GFX_GLYPH_W;
    int py = PAD + v * GFX_GLYPH_H;
    char s[2] = { c, 0 };
    int sel = gterm_cell_selected(win, row, col);
    gfx_fill(&win->surf, px, py, GFX_GLYPH_W, GFX_GLYPH_H,
             sel ? COL_SEL_BG : COL_WIN_BG);
    if (c > 0x20) gfx_text(&win->surf, px, py, s, sel ? COL_SEL_FG : COL_WIN_FG);
}

static void gterm_scroll(struct gui_window* win) {
    /* §M58 — the line about to be discarded goes into the history first. */
    gterm_sb_push(win, win->cells);
    win->scrolled++;

    /* A view that is scrolled BACK must not move: the user is reading fixed
     * text while new output arrives underneath it.  view_off is measured from
     * the live bottom, so following the same content means growing it by one —
     * up to the depth actually kept, past which the text really is gone. */
    if (win->view_off > 0) {
        win->view_off++;
        if (win->view_off > win->sb_count) win->view_off = win->sb_count;
        /* The screen is showing history; the pixel scroll below would slide it.
         * Only the MODEL moves here — the compositor re-renders the view. */
        for (int r = 0; r < win->rows - 1; r++) {
            char* d = win->cells + (size_t)r * gmax_cols;
            for (int c = 0; c < gmax_cols; c++) d[c] = d[c + gmax_cols];
        }
        char* last = win->cells + (size_t)(win->rows - 1) * gmax_cols;
        for (int c = 0; c < gmax_cols; c++) last[c] = 0;
        return;
    }

    struct gfx_surface* s = &win->surf;
    int top    = PAD;
    int bottom = PAD + win->rows * GFX_GLYPH_H;
    int lift   = GFX_GLYPH_H * s->stride;
    for (int y = top; y < bottom - GFX_GLYPH_H; y++) {
        uint32_t* row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = row[x + lift];
    }
    gfx_fill(s, 0, bottom - GFX_GLYPH_H, s->w, GFX_GLYPH_H, COL_WIN_BG);

    for (int r = 0; r < win->rows - 1; r++) {
        char* d = win->cells + (size_t)r * gmax_cols;
        for (int c = 0; c < gmax_cols; c++) d[c] = d[c + gmax_cols];
    }
    char* lastrow = win->cells + (size_t)(win->rows - 1) * gmax_cols;
    for (int c = 0; c < gmax_cols; c++) lastrow[c] = 0;
}

static void gterm_emit(void* ctx, char c) {
    struct gui_window* win = (struct gui_window*)ctx;
    spin_lock(&win->lock);

    if (c == '\f') {
        gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
        for (int i = 0; i < gmax_cols * gmax_rows; i++) win->cells[i] = 0;
        win->ccol = win->crow = 0;
    } else if (c == '\n') {
        win->ccol = 0;
        if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
    } else if (c == '\r') {
        win->ccol = 0;
    } else if (c == '\b') {
        if (win->ccol > 0) {
            win->ccol--;
            win->cells[(size_t)win->crow * gmax_cols + win->ccol] = 0;
            gterm_draw_cell(win, win->ccol, win->scrolled + win->crow, ' ');
        }
    } else {
        win->cells[(size_t)win->crow * gmax_cols + win->ccol] = c;
        gterm_draw_cell(win, win->ccol, win->scrolled + win->crow, c);
        if (++win->ccol >= win->cols) {
            win->ccol = 0;
            if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
        }
    }

    spin_unlock(&win->lock);
    gui_damage_win(win);
}

/* §M58 — copy the selected cells out of the backing store into `dst`.
 * Trailing blanks on each line are dropped (a terminal pads its rows with
 * spaces, and pasting that padding is never what was meant), and a newline is
 * inserted between rows.  Returns the number of bytes produced. */
static int gterm_selection_text(struct gui_window* win, char* dst, int cap) {
    if (!win->sel_on || !win->cells || cap <= 0) return 0;
    int ar = win->sel_ar, ac = win->sel_ac, br = win->sel_br, bc = win->sel_bc;
    if (br < ar || (br == ar && bc < ac)) {
        int tr = ar, tc = ac; ar = br; ac = bc; br = tr; bc = tc;
    }
    int n = 0;
    for (int r = ar; r <= br; r++) {
        /* ABSOLUTE line → wherever it lives now (live grid or history).  A line
         * that has aged out of the ring yields nothing rather than the wrong
         * text: the alternative is silently copying whatever occupies that slot
         * today, which is worse than a short copy. */
        const char* src = gterm_row(win, r);
        if (!src) { if (r != br && n < cap - 1) dst[n++] = '\n'; continue; }
        int c0 = (r == ar) ? ac : 0;
        int c1 = (r == br) ? bc : win->cols;
        if (c1 > win->cols) c1 = win->cols;
        /* Trim the row's trailing blanks/NULs. */
        int end = c1;
        while (end > c0) {
            char ch = src[end - 1];
            if (ch != 0 && ch != ' ') break;
            end--;
        }
        for (int c = c0; c < end && n < cap - 1; c++)
            dst[n++] = src[c] ? src[c] : ' ';
        if (r != br && n < cap - 1) dst[n++] = '\n';
    }
    dst[n] = '\0';
    return n;
}

/* Pixel (content-relative) → cell, clamped into the grid.  `col` is allowed to
 * reach `cols` so a drag past the end of a line selects the whole line. */
static void gterm_cell_at(struct gui_window* win, int cx, int cy,
                          int* row, int* col) {
    int r = (cy - PAD) / GFX_GLYPH_H;
    int c = (cx - PAD + GFX_GLYPH_W / 2) / GFX_GLYPH_W;
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if (r >= win->rows) r = win->rows - 1;
    if (c > win->cols)  c = win->cols;
    /* ABSOLUTE line, not the screen row: what the caller means by "this text"
     * must keep meaning it after the next line of output arrives. */
    *row = win->scrolled - win->view_off + r;
    *col = c;
}

static void gterm_rerender_locked(struct gui_window* win);

/* §M58 — the compositor half of terminal selection.  The mouse IRQ only
 * RECORDS what changed (a cell range, a request to copy); everything that
 * costs time or allocates happens here:
 *
 *   - re-rendering the grid is thousands of glyph blits — not IRQ work;
 *   - `clipboard_set` allocates — not IRQ work either.
 *
 * Same split as every other input path in this file (§M22.7), and the reason
 * the selection is a MODEL range rather than painted pixels: the IRQ can move
 * it for free and the repaint happens once per frame no matter how many mouse
 * packets arrived.  */
static void apply_mode_change(void);
static void apply_mode_revert(void);

static void term_selection_service(void) {
    struct gui_window* d = term_sel_dirty;
    if (d) {
        term_sel_dirty = NULL;
        if (d->used && d->kind == WIN_TERM && d->cells) {
            spin_lock(&d->lock);
            gterm_rerender_locked(d);
            spin_unlock(&d->lock);
            gui_damage_win(d);
        }
    }

    struct gui_window* p = term_paste_win;
    if (p) {
        term_paste_win = NULL;
        if (p->used && p->kind == WIN_TERM && p->vc) {
            /* Prefer the EXPLICIT clipboard and fall back to the selection:
             * a paste with nothing deliberately copied should still do the
             * obvious thing rather than nothing at all. */
            int use_clip = clipboard_len() > 0;
            int n = use_clip ? clipboard_len() : clipboard_primary_len();
            if (n > 0) {
                char* buf = (char*)kmalloc((size_t)n + 1);
                if (buf) {
                    n = use_clip ? clipboard_get(buf, n + 1)
                                 : clipboard_get_primary(buf, n + 1);
                    /* Focus first: a paste goes to the window that was CLICKED,
                     * and vc_kbd_push feeds the FOCUSED VC — without this the
                     * text would land in whichever terminal happened to have
                     * focus, which is the kind of bug that looks like data
                     * loss. */
                    gui_window_raise(p);
                    for (int i = 0; i < n; i++) {
                        /* A newline in the middle of a pasted selection is a
                         * command SUBMISSION here, exactly as if it had been
                         * typed — that is what pasting into a shell means, and
                         * silently dropping it would make multi-line pastes
                         * concatenate into one wrong command. */
                        vc_kbd_push(buf[i]);
                    }
                    kfree(buf);
                }
            }
        }
    }

    struct gui_window* k = term_sel_copy_to_clip;
    if (k) {
        term_sel_copy_to_clip = NULL;
        if (k->used && k->kind == WIN_TERM && k->cells && k->sel_on) {
            enum { SEL_MAX = 16 * 1024 };
            char* buf = (char*)kmalloc(SEL_MAX);
            if (buf) {
                spin_lock(&k->lock);
                int n = gterm_selection_text(k, buf, SEL_MAX);
                spin_unlock(&k->lock);
                if (n > 0) {
                    clipboard_set(buf, n);
                    kprintf("gui: copied %d byte(s) to the clipboard\n", n);
                }
                kfree(buf);
            }
        } else if (k->used) {
            kprintf("gui: nothing selected — drag across the text first\n");
        }
    }

    struct gui_window* c = term_sel_copy;
    if (c) {
        term_sel_copy = NULL;
        if (c->used && c->kind == WIN_TERM && c->cells && c->sel_on) {
            /* Bounded: a selection is at most the visible grid, and a cap keeps
             * a future scrollback selection from defining the buffer size. */
            enum { SEL_MAX = 16 * 1024 };
            char* buf = (char*)kmalloc(SEL_MAX);
            if (buf) {
                spin_lock(&c->lock);
                int n = gterm_selection_text(c, buf, SEL_MAX);
                spin_unlock(&c->lock);
                if (n > 0) {
                    clipboard_set_primary(buf, n);
                    kprintf("gui: selected %d byte(s) — Ctrl+Shift+C to copy, "
                            "Ctrl+Shift+V or middle-click to paste\n", n);
                }
                kfree(buf);
            }
        }
    }
}

/* Repaint the VIEW: screen row v shows absolute line `base + v`, which may live
 * in the live grid or in the history ring — gterm_row knows which, and nothing
 * here needs to. */
static void gterm_rerender_locked(struct gui_window* win) {
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    int base = win->scrolled - win->view_off;
    for (int v = 0; v < win->rows; v++) {
        const char* src = gterm_row(win, base + v);
        if (!src) continue;
        for (int c = 0; c < win->cols; c++)
            if (src[c]) gterm_draw_cell(win, c, base + v, src[c]);
    }

    /* A scrolled-back view says so, in the corner it cannot be confused with
     * output: a terminal that silently stops showing new text is indisting-
     * uishable from one that has hung. */
    if (win->view_off > 0) {
        char tag[24];
        int n = 0;
        tag[n++] = '['; 
        int v = win->view_off, div = 10000, seen = 0;
        while (div > 0) {
            int d = (v / div) % 10;
            if (d || seen || div == 1) { tag[n++] = (char)('0' + d); seen = 1; }
            div /= 10;
        }
        const char* suffix = " lines back]";
        for (const char* p2 = suffix; *p2 && n < (int)sizeof tag - 1; p2++) tag[n++] = *p2;
        tag[n] = 0;
        int tw = n * GFX_GLYPH_W;
        int tx = win->surf.w - PAD - tw, ty = PAD;
        if (tx < 0) tx = 0;
        gfx_fill(&win->surf, tx, ty, tw, GFX_GLYPH_H, COL_SEL_BG);
        gfx_text(&win->surf, tx, ty, tag, COL_SEL_FG);
    }
}

/* Move the view by `dl` lines (positive = back into history) and clamp it.
 * Returns non-zero if the view actually moved — the caller only repaints then,
 * so holding the wheel at the top of the history costs nothing. */
static int gterm_view_scroll(struct gui_window* win, int dl) {
    if (!win->cells) return 0;
    int want = win->view_off + dl;
    if (want < 0) want = 0;
    if (want > win->sb_count) want = win->sb_count;
    if (want == win->view_off) return 0;
    win->view_off = want;
    return 1;
}

/* ==========================================================================
 * `termcheck` — the falsification for scrollback (§M58).
 *
 * A screenshot can show that a window LOOKS scrolled; it cannot show that the
 * selection still names the text the user pointed at.  This asks the model
 * instead: it writes numbered lines until they have demonstrably scrolled off,
 * then selects one BY ABSOLUTE LINE NUMBER and prints what the copy path
 * returns.  If the addressing were still grid-relative — the bug this feature
 * exists to remove — the answer would be a line that is currently on screen,
 * and the printed text would say so.
 *
 * Runs inside a GUI terminal window (it needs one to inspect); on the text
 * console it says so rather than pretending.
 * ========================================================================== */
void gui_term_check(void) {
    struct task* self = task_current();
    struct gui_window* win = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].kind == WIN_TERM && windows[i].cells &&
            self && windows[i].vc == self->out_console) { win = &windows[i]; break; }
    if (!win) {
        kprintf("termcheck: not running in a GUI terminal window "
                "(open one from Start > New Shell)\n");
        return;
    }

    kprintf("termcheck: grid %dx%d, scrollback %d/%d lines, view_off %d\n",
            win->cols, win->rows, win->sb_count, win->sb_cap, win->view_off);
    if (win->sb_cap <= 0) {
        kprintf("termcheck: no scrollback configured (gui.scrollback = 0)\n");
        return;
    }

    /* Where the next line will land, recorded BEFORE writing any: the absolute
     * number is the only handle that survives the scrolling we are about to
     * cause. */
    int base = win->scrolled + win->crow;
    int n    = win->rows * 2;                   /* enough to scroll off twice */
    for (int i = 0; i < n; i++) kprintf("SBLINE %d\n", i);

    /* Line 3 was printed long ago and is certainly off screen now. */
    int target = base + 3;
    int onscreen = gterm_screen_row(win, target);
    const char* row = gterm_row(win, target);
    kprintf("termcheck: line abs %d — on screen: %s, in history: %s\n",
            target, onscreen >= 0 ? "yes" : "no", row ? "yes" : "no");

    /* Select that whole line THROUGH THE SAME PATH the mouse uses, and copy. */
    char buf[128];
    int  got = 0;
    spin_lock(&win->lock);
    int save_ar = win->sel_ar, save_ac = win->sel_ac;
    int save_br = win->sel_br, save_bc = win->sel_bc, save_on = win->sel_on;
    win->sel_ar = target; win->sel_ac = 0;
    win->sel_br = target; win->sel_bc = win->cols;
    win->sel_on = 1;
    got = gterm_selection_text(win, buf, (int)sizeof buf);
    win->sel_ar = save_ar; win->sel_ac = save_ac;
    win->sel_br = save_br; win->sel_bc = save_bc; win->sel_on = save_on;
    spin_unlock(&win->lock);

    kprintf("termcheck: copied %d byte(s) from that line: \"%s\"\n", got, buf);
    kprintf("termcheck: expected \"SBLINE 3\" — %s\n",
            (buf[0] == 'S' && buf[1] == 'B' && buf[7] == '3' && got == 8)
            ? "PASS (absolute addressing reaches history)"
            : "FAIL (the selection is not naming the line it was given)");

    /* And the view: scroll back, confirm the offset took, come back. */
    int moved = gterm_view_scroll(win, 10);
    kprintf("termcheck: view scrolled back 10 -> view_off %d (%s)\n",
            win->view_off, moved ? "moved" : "clamped at the top of history");
    gterm_view_scroll(win, -win->view_off);
    term_sel_dirty = win;
    need_frame = 1;
}

/* -------------------------------------------------------------------------- */
/* App-window redraw + resize plumbing.                                        */
/* -------------------------------------------------------------------------- */

static void app_redraw(struct gui_window* win) {
    spin_lock(&win->lock);
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    widget_draw_all(win->widgets, &win->surf);
    spin_unlock(&win->lock);
    gui_damage_win(win);
}

/* -------------------------------------------------------------------------- */
/* M22.7 — per-task app host.  Each WIN_APP window runs on its own task; the   */
/* compositor routes input into win->aq and this loop consumes it, so a slow   */
/* app handler never stalls compositing.                                       */
/* -------------------------------------------------------------------------- */

/* Compositor/IRQ → host handoff (SPSC; the host is the sole consumer). */
static void aq_push(struct gui_window* w, struct app_event e) {
    if (!w) return;
    uint32_t n = (w->aq_h + 1) % AQ_SZ;
    if (n == w->aq_t) return;                   /* full — drop (input flood) */
    w->aq[w->aq_h] = e;
    w->aq_h = n;
}

/* Free a WIN_APP window's widget list + app_ctx.  Runs on the owning host
 * (teardown) — the compositor never touches widgets once a host exists. */
static void app_widgets_free(struct gui_window* win) {
    struct widget* w = win->widgets;
    while (w) {
        struct widget* nx = w->next;
        if (w->ops && w->ops->destroy) w->ops->destroy(w);
        kfree(w);
        w = nx;
    }
    win->widgets = NULL;
    win->focusw  = NULL;
    if (win->app_ctx) { kfree(win->app_ctx); win->app_ctx = NULL; }
    if (win->ui_state) { kfree(win->ui_state); win->ui_state = NULL; }
}

static void app_dispatch_event(struct gui_window* win, const struct app_event* e) {
    /* §M26 — a Wayland-backed window forwards input to its client instead of
     * to widgets.
     *
     * §M65 — UNLESS IT HAS TOOLKIT WIDGETS.  A client that called ui_build
     * asked the kernel to run its interface; forwarding the raw pointer stream
     * as well would mean the click reaches the client and the checkbox under
     * it never moves — which is exactly what happened the first time a ring-3
     * program built widgets: they drew, and nothing was clickable.  A window
     * with widgets is driven by the toolkit; one without keeps the raw stream,
     * which is every existing client. */
    if (win->input_hook && !win->widgets) {
        struct gui_input gi = {0};
        if (e->type == AE_MOUSE)        { gi.type = GUI_INPUT_MOTION; gi.x = e->x; gi.y = e->y; }
        else if (e->type == AE_BUTTON)  { gi.type = GUI_INPUT_BUTTON; gi.x = e->x; gi.y = e->y;
                                          gi.keycode = e->btn; gi.pressed = e->down; }
        else if (e->type == AE_KEYCODE) { gi.type = GUI_INPUT_KEY; gi.keycode = e->kc; gi.pressed = 1; }
        /* AE_KEY carries the keymap's OUTPUT.  It used to be dropped here, on
         * the grounds that a client gets the scancode — but a client with no
         * keymap of its own cannot turn a scancode into a letter, so every
         * typed character arrived as noise. */
        else if (e->type == AE_KEY)     { gi.type = GUI_INPUT_KEY; gi.ch = (unsigned char)e->c;
                                          gi.pressed = 1; }
        else return;
        win->input_hook(win, &gi, win->input_ctx);
        return;
    }
    if (e->type == AE_POPUP) {
        /* §M65 — the popup's answer, on the app host: choosing a menu item
         * runs app code (open a dialog, delete a file), which is why the IRQ
         * only queued it. */
        ui_dispatch_popup(win, e->x, e->y);
        return;
    }
    if (e->type == AE_SCROLL) {
        /* §M61 follow-up — the wheel goes to the widget UNDER THE POINTER, not
         * to the focused one: that is what every toolkit does and what the
         * hand expects, and it means a list can be scrolled without clicking
         * into it first. */
        struct widget* w = widget_at(win->widgets, e->x, e->y);
        int dz = (int)(int8_t)e->phase;
        if (w && w->ops && w->ops->scroll) { w->ops->scroll(w, dz); return; }
        /* §M65 — nothing under the pointer wanted it?  Ask the toolkit: a
         * SCROLLING CONTAINER is a node, not a widget, so it cannot have a
         * widget's scroll op of its own. */
        if (ui_scroll_at(win, e->x, e->y, dz)) gui_window_request_redraw(win);
        return;
    }
    if (e->type == AE_POINTER) {
        /* §M58 — the phase stream.  The GRABBED widget is resolved HERE, on
         * the host task that owns the widget list, and never carried through
         * the queue: a widget pointer travelling through an IRQ-filled ring
         * would be a lifetime bug waiting for the first window teardown
         * mid-drag (§M54's defect class).  The press picks the widget, the
         * drag and release go to whatever the press picked. */
        if (e->phase == WPTR_PRESS) {
            win->grabw = widget_at(win->widgets, e->x, e->y);
            if (win->grabw && win->grabw->ops && !win->grabw->ops->pointer)
                win->grabw = NULL;      /* widget does not want the stream */
        }
        struct widget* w = win->grabw;
        if (w && w->ops && w->ops->pointer)
            w->ops->pointer(w, e->x - w->x, e->y - w->y, e->phase);
        if (e->phase == WPTR_RELEASE) win->grabw = NULL;
        return;
    }
    if (e->type == AE_MOUSE) {
        struct widget* w = widget_at(win->widgets, e->x, e->y);
        if (w && w->ops && w->ops->mouse)
            w->ops->mouse(w, e->x - w->x, e->y - w->y, e->dbl);
    } else if (e->type == AE_KEY) {
        /* §M61 — a window-level key hook, consulted BEFORE the focused widget.
         * The confirm-or-revert dialog needs Enter/Esc to work whether or not
         * anything is focused: at a mode the display cannot show, the keyboard
         * is the only input the user can aim. */
        if (win->key_hook) { win->key_hook(win, e->c); return; }
        struct widget* w = win->focusw;
        if (w && w->ops && w->ops->key) w->ops->key(w, e->c);
    } else if (e->type == AE_KEYCODE) {
        /* §M65 — TAB CYCLES FOCUS, at the WINDOW level, before the focused
         * widget sees it.  It has to be here rather than in a widget: no
         * control can know what comes after it, and a toolkit where the only
         * way to reach the third field is the mouse is a toolkit half the
         * people cannot use.  Shift+Tab goes backwards, and the cycle wraps —
         * a focus ring with an end is a trap at both ends. */
        if (e->kc == KC_TAB) {
            int back = (e->mods & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
            gui_window_focus_cycle(win, back);
            return;
        }
        struct widget* w = win->focusw;
        if (w && w->ops && w->ops->keycode) w->ops->keycode(w, e->kc, e->mods);
    }
}

/* The app-host task entry.  start_arg is the app's launch (open) function;
 * it runs HERE (creating the window(s) + widgets on this task), then this
 * loop services every window the app owns until they all close. */
static void app_host_main(void) {
    void (*open_fn)(void) = (void (*)(void))task_start_arg();
    struct task* self = task_current();
    kprintf("gui: app-host '%s' up (pid %d)\n",
            self ? self->name : "?", self ? self->pid : -1);
    if (open_fn) open_fn();                     /* creates windows on this task */

    for (;;) {
        int live = 0, busy = 0;
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            struct gui_window* win = &windows[i];
            if (!win->used || win->kind != WIN_APP || win->host_task != self)
                continue;
            if (win->host_released) continue;   /* handed to the compositor */

            if (win->want_close) {              /* graceful, on the host */
                if (win->on_close) win->on_close(win);
                app_widgets_free(win);
                win->host_released = 1;         /* compositor disposes the struct */
                need_frame = 1;
                busy = 1;
                continue;                       /* not live anymore */
            }
            live++;

            int worked = 0;
            while (win->aq_t != win->aq_h) {
                struct app_event e = win->aq[win->aq_t];
                win->aq_t = (win->aq_t + 1) % AQ_SZ;
                app_dispatch_event(win, &e);
                worked = 1;
            }
            if (win->layout_pending) {
                win->layout_pending = 0;
                if (win->on_layout) win->on_layout(win);
                worked = 1;
            }
            if (win->tick_pending) {
                win->tick_pending = 0;
                if (win->on_tick) win->on_tick(win);
                worked = 1;               /* on_tick usually requests its own redraw */
            }
            if (worked) { app_redraw(win); busy = 1; }
        }
        if (live == 0) break;             /* all my windows closed → exit */
        if (!busy) hal_cpu_idle();        /* M22.7 — halt only when idle */
        task_yield();
    }
    /* Host exits; init reaps it (not reap_owned).  Any windows it released
     * are disposed by the compositor's apply_pending. */
}

static int window_set_size(struct gui_window* win, int outer_w, int outer_h) {
    int cw = outer_w - 2 * BORDER;
    int ch = outer_h - TITLE_H - BORDER;
    struct gfx_surface ns;
    if (gfx_surface_init(&ns, cw, ch) != 0) return -1;
    gfx_fill(&ns, 0, 0, cw, ch, COL_WIN_BG);

    spin_lock(&win->lock);
    struct gfx_surface old = win->surf;
    win->surf = ns;
    if (win->kind == WIN_TERM) {
        int ncols = (cw - 2 * PAD) / GFX_GLYPH_W;
        int nrows = (ch - 2 * PAD) / GFX_GLYPH_H;
        if (ncols > gmax_cols) ncols = gmax_cols;
        if (nrows > gmax_rows) nrows = gmax_rows;
        if (win->cells) {
            int excess = win->crow - (nrows - 1);
            if (excess > 0) {
                /* §M58 — a shrink evicts rows off the top exactly as a scroll
                 * does, so they belong in the history for the same reason. */
                for (int r = 0; r < excess; r++) {
                    gterm_sb_push(win, win->cells + (size_t)r * gmax_cols);
                    win->scrolled++;
                }
                for (int r = 0; r < gmax_rows - excess; r++) {
                    char* d = win->cells + (size_t)r * gmax_cols;
                    const char* srow = d + (size_t)excess * gmax_cols;
                    for (int c = 0; c < gmax_cols; c++) d[c] = srow[c];
                }
                for (int r = gmax_rows - excess; r < gmax_rows; r++) {
                    char* d = win->cells + (size_t)r * gmax_cols;
                    for (int c = 0; c < gmax_cols; c++) d[c] = 0;
                }
                win->crow = nrows - 1;
            }
            win->cols = ncols;
            win->rows = nrows;
            if (win->ccol >= ncols) win->ccol = ncols - 1;
            gterm_rerender_locked(win);
        } else {
            win->cols = ncols;
            win->rows = nrows;
        }
    }
    spin_unlock(&win->lock);

    gfx_surface_free(&old);

    /* M22.7 — the app-host owns widget layout + drawing; ask it to re-layout
     * (it runs on_layout + app_redraw next loop).  Set even at creation time:
     * the host processes it once the open fn has created the widgets. */
    if (win->kind == WIN_APP)
        win->layout_pending = 1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Widget plumbing (gui.h API used by widget.c + apps).                        */
/* -------------------------------------------------------------------------- */

void gui_window_add_widget(struct gui_window* win, struct widget* w) {
    if (!win || win->kind != WIN_APP || !w) return;
    struct widget** p = &win->widgets;
    while (*p) p = &(*p)->next;
    *p = w;
}

void gui_window_focus_widget(struct gui_window* win, struct widget* w) {
    if (!win || win->kind != WIN_APP) return;
    win->focusw = w;
}

int gui_widget_focused(struct widget* w) {
    return w && w->win && w->win->focusw == w;
}

void gui_window_outer_for_content(int cw, int ch, int* ow, int* oh) {
    if (ow) *ow = cw + 2 * BORDER;
    if (oh) *oh = ch + TITLE_H + BORDER;
}

int gui_window_content_size(struct gui_window* win, int* w, int* h) {
    if (!win) return -1;
    if (w) *w = win->surf.w;
    if (h) *h = win->surf.h;
    return 0;
}

void* gui_window_ctx(struct gui_window* win) {
    return win ? win->app_ctx : NULL;
}

void gui_window_request_redraw(struct gui_window* win) {
    if (win && win->used && win->kind == WIN_APP) app_redraw(win);
}

/* M22.7 — redraw + damage only a CONTENT sub-rect (widget-local coords), not
 * the whole window.  A frequently-refreshing app (the Task Manager) uses it
 * to repaint just its listview each second instead of the entire window
 * chrome — the widget clip confines the draw, and only that screen rect is
 * damaged. */
void gui_window_request_redraw_rect(struct gui_window* win,
                                    int cx, int cy, int cw, int ch) {
    if (!win || !win->used || win->kind != WIN_APP) return;
    if (cw <= 0 || ch <= 0) return;
    spin_lock(&win->lock);
    gfx_set_clip(&win->surf, cx, cy, cw, ch);
    gfx_fill(&win->surf, cx, cy, cw, ch, COL_WIN_BG);
    widget_draw_all(win->widgets, &win->surf);  /* clip keeps it to the rect */
    gfx_clear_clip(&win->surf);
    spin_unlock(&win->lock);
    gui_damage(win->x + BORDER + cx, win->y + TITLE_H + cy, cw, ch);
}

/* §M26 — paint a raw pixel block into a window's content surface + composite it
 * (the Wayland compositor bridge: a wl_surface's committed buffer becomes a real
 * window's contents).  Coords are content-relative (exclude the chrome). */
void gui_window_blit(struct gui_window* win, int x, int y,
                     const uint32_t* px, int w, int h, int stride) {
    if (!win || !win->used || win->kind != WIN_APP || !px || w <= 0 || h <= 0) return;
    struct gfx_surface src;
    src.w = w; src.h = h; src.stride = stride; src.px = (uint32_t*)px; src.owns_px = 0;
    gfx_clear_clip(&src);
    spin_lock(&win->lock);
    gfx_blit(&win->surf, x, y, &src, 0, 0, w, h);
    spin_unlock(&win->lock);
    gui_damage(win->x + BORDER + x, win->y + TITLE_H + y, w, h);
}

/* Read a content-surface pixel back (for self-tests). */
uint32_t gui_window_pixel(struct gui_window* win, int x, int y) {
    if (!win || !win->used || x < 0 || y < 0 ||
        x >= win->surf.w || y >= win->surf.h) return 0;
    return win->surf.px[y * win->surf.stride + x];
}

/* §M26 — set the input sink (see gui.h). */
void gui_window_set_input_hook(struct gui_window* win,
        void (*fn)(struct gui_window*, const struct gui_input*, void*), void* ctx) {
    if (!win) return;
    win->input_hook = fn;
    win->input_ctx  = ctx;
}

void gui_window_set_on_close(struct gui_window* win,
                             void (*fn)(struct gui_window*)) {
    if (win) win->on_close = fn;
}

void gui_window_close(struct gui_window* win) {
    if (win && win->used) {                 /* M22.3: TERM windows too */
        win->want_close = 1;
        need_frame = 1;
    }
}

/* §M42 — a CLIENT-MANAGED WIN_APP window (the dosgui bridge for a ring-3 client
 * like NetSurf).  Sever the host_task binding: the client is a DETACHED task
 * reaped by init, not a compositor-owned app-host, so the compositor must NOT
 * read host_task->state or reap it (that races init → task-table corruption →
 * GUI wedge on the next open).  With host_task == NULL, apply_pending's WIN_APP
 * teardown never observes the task's death — disposal is driven only by the
 * client's explicit release below. */
void gui_window_set_client_managed(struct gui_window* win, int client_pid) {
    if (win) { win->host_task = NULL; win->client_pid = client_pid; }
}

/* §M54 — see gui.h.  One slot, set at creation by the bridge that owns the
 * handle; the compositor calls it exactly once when the struct is disposed. */
void gui_window_set_dispose_cb(struct gui_window* win,
                               void (*cb)(struct gui_window*, void*), void* ctx) {
    if (win) { win->on_dispose = cb; win->dispose_ctx = ctx; }
}

/* §M42 — the client (dosgui_destroy, from dos_finalise) says it is finished with
 * the window and will not touch it again.  Mark it disposable: want_close makes
 * apply_pending pick it up; host_released makes it skip the host-coordination /
 * host_task->state read and dispose immediately (reap_gui_host(NULL) is a
 * no-op), so no init-owned task struct is ever touched. */
void gui_window_client_release(struct gui_window* win) {
    if (win && win->used) {
        win->host_released = 1;
        win->want_close    = 1;
        need_frame         = 1;
    }
}

void gui_window_set_tick(struct gui_window* win,
                         void (*fn)(struct gui_window*)) {
    if (win) win->on_tick = fn;
}

int gui_window_minimized(struct gui_window* w) {
    return w ? w->minimized : 0;
}

/* §M42 — has the window been asked to close (its X button was clicked)?  A
 * WIN_APP that drives itself (NetSurf, via the dosgui bridge) isn't running the
 * app-host loop that would normally see want_close, so it polls this and quits
 * on its own; the compositor then disposes the window when the task dies. */
int gui_window_want_close(struct gui_window* w) {
    return (w && w->used) ? w->want_close : 0;
}

static void raise_window(struct gui_window* win);

void gui_window_raise(struct gui_window* win) {
    if (!win || !win->used) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    raise_window(win);
    focused_win = win;
    spin_unlock_irqrestore(&state_lock, fl);
    if (win->kind == WIN_TERM) vc_focus(win->vc);
    gui_damage_all();
}

void gui_window_set_title(struct gui_window* win, const char* title) {
    if (!win || !win->used || !title) return;
    str_copy(win->title, title, (int)sizeof(win->title));
    gui_damage_win(win);                    /* repaint chrome (and taskbar
                                             * on the next full frame) */
}

void gui_wm_focus_raise_locked(struct gui_window* w) {
    if (!w || !w->used) return;
    w->minimized = 0;                       /* activating always restores */
    raise_window(w);
    focused_win = w;
    if (w->kind == WIN_TERM) vc_focus(w->vc);
}

/* Topmost non-minimized window — focus fallback. */
static struct gui_window* top_visible_locked(void) {
    for (int i = zcount - 1; i >= 0; i--)
        if (!zorder[i]->minimized) return zorder[i];
    return NULL;
}

/* M22.5 — maximize/restore toggle.  WM lock held (mouse IRQ).  The
 * geometry change goes through the pending-resize handoff so the
 * surface realloc happens on the compositor task, exactly like a
 * grip-resize release. */
static void toggle_maximize_locked(struct gui_window* w) {
    if (!w || !w->used) return;
    if (!w->maximized) {
        w->sav_x = w->x;  w->sav_y = w->y;
        w->sav_w = w->w;  w->sav_h = w->h;
        w->x = 0;  w->y = 0;
        w->pending_w = fbsurf.w;                /* work-area aware: height */
        w->pending_h = work_h;                  /* stops above the taskbar */
        w->maximized = 1;
    } else {
        w->x = w->sav_x;  w->y = w->sav_y;
        w->pending_w = w->sav_w;
        w->pending_h = w->sav_h;
        w->maximized = 0;
    }
    need_frame = 1;
}

/* Taskbar-button semantics (Windows-style): minimized → restore +
 * focus; focused → minimize; else → focus + raise.  WM lock held. */
void gui_wm_taskbar_activate_locked(struct gui_window* w) {
    if (!w || !w->used) return;
    if (w->minimized) {
        gui_wm_focus_raise_locked(w);
    } else if (focused_win == w) {
        w->minimized = 1;
        struct gui_window* nf = top_visible_locked();
        focused_win = nf;
        if (nf && nf->kind == WIN_TERM) vc_focus(nf->vc);
    } else {
        gui_wm_focus_raise_locked(w);
    }
}

/* -------------------------------------------------------------------------- */
/* Cursor sprite.                                                              */
/* -------------------------------------------------------------------------- */

static const char* const cursor_rows[17] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X........X ",
    "X.....XXXXX",
    "X..X..X    ",
    "X.X X..X   ",
    "XX  X..X   ",
    "X    X..X  ",
    "     X..X  ",
    "      XX   ",
};

/* Open the popup.  `items` is ONE string with '\n' between entries — flat, so
 * the same call survives being marshalled from ring 3 later, and "-" is a
 * separator.  Called on the owner's app-host task. */
void gui_popup_open(struct gui_window* owner, int sx, int sy,
                    const char* items, int tag) {
    if (!items) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    popup.count = 0;
    popup.hover = -1;
    int widest = 0;
    const char* p = items;
    while (*p && popup.count < POPUP_MAX_ITEMS) {
        int n = 0;
        while (*p && *p != '\n' && n < POPUP_ITEM_LEN - 1)
            popup.items[popup.count][n++] = *p++;
        popup.items[popup.count][n] = 0;
        while (*p && *p != '\n') p++;               /* drop an over-long tail */
        if (*p == '\n') p++;
        if (n > widest) widest = n;
        popup.count++;
    }
    popup.w = widest * GFX_GLYPH_W + 24;
    popup.h = popup.count * POPUP_ROW_H + 6;
    popup.x = sx;
    popup.y = sy;
    /* Keep it on screen: a menu opened near the right edge belongs to the LEFT
     * of the pointer, which is what every toolkit does and what stops the last
     * entry from being unreachable. */
    if (popup.x + popup.w > fbsurf.w) popup.x = fbsurf.w - popup.w;
    if (popup.y + popup.h > fbsurf.h) popup.y = fbsurf.h - popup.h;
    if (popup.x < 0) popup.x = 0;
    if (popup.y < 0) popup.y = 0;
    popup.owner = owner;
    popup.tag = tag;
    popup.active = 1;
    spin_unlock_irqrestore(&state_lock, fl);
    gui_damage_all();
}

void gui_popup_close(void) {
    if (!popup.active) return;
    popup.active = 0;
    gui_damage_all();
}

int gui_popup_active(void) { return popup.active; }

/* Which row is (sx,sy) over?  -1 = outside, or a separator (which is not a
 * choice and must not behave like one). */
static int popup_row_at(int sx, int sy) {
    if (!popup.active) return -1;
    if (sx < popup.x || sx >= popup.x + popup.w) return -1;
    if (sy < popup.y + 3 || sy >= popup.y + 3 + popup.count * POPUP_ROW_H) return -1;
    int i = (sy - popup.y - 3) / POPUP_ROW_H;
    if (i < 0 || i >= popup.count) return -1;
    if (popup.items[i][0] == '-' && !popup.items[i][1]) return -1;
    return i;
}

static void draw_popup(struct gfx_surface* dst) {
    if (!popup.active) return;
    gfx_blend_fill(dst, popup.x + 4, popup.y + 4, popup.w, popup.h, COL_SHADOW);
    gfx_fill(dst, popup.x, popup.y, popup.w, popup.h, COL_POP_BG);
    draw_rect_outline(dst, popup.x, popup.y, popup.w, popup.h, 1, COL_POP_EDGE);
    for (int i = 0; i < popup.count; i++) {
        int y = popup.y + 3 + i * POPUP_ROW_H;
        if (popup.items[i][0] == '-' && !popup.items[i][1]) {
            gfx_fill(dst, popup.x + 6, y + POPUP_ROW_H / 2, popup.w - 12, 1, COL_POP_SEP);
            continue;
        }
        if (i == popup.hover)
            gfx_fill(dst, popup.x + 2, y, popup.w - 4, POPUP_ROW_H, COL_POP_HOVER);
        gfx_text(dst, popup.x + 10, y + (POPUP_ROW_H - GFX_GLYPH_H) / 2,
                 popup.items[i], COL_POP_TEXT);
    }
}

static void draw_cursor(struct gfx_surface* s, int cx, int cy) {
    for (int j = 0; j < 17; j++) {
        for (int i = 0; cursor_rows[j][i]; i++) {
            char p = cursor_rows[j][i];
            if (p == ' ') continue;
            int x = cx + i, y = cy + j;
            if (x < 0 || x >= s->w || y < 0 || y >= s->h) continue;
            s->px[(size_t)y * s->stride + x] = (p == 'X') ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Composition.                                                                */
/* -------------------------------------------------------------------------- */

/* M22.4 — compositor-side cursor bookkeeping.  Where the cursor was
 * LAST DRAWN, updated only by compose().  Lesson learned (2026-07-04):
 * compose() snapshots the damage rect BEFORE the WM state, so an
 * IRQ-supplied cursor rect can describe an OLDER position than the
 * (cx,cy) we end up drawing — the cursor got erased at its old spot
 * but clipped away at its new one for that frame (visible flicker /
 * ghosting when gliding over contrasting chrome).  The fix: the mouse
 * IRQ never submits cursor rects at all (a glide is a bare need_frame
 * wake); compose() itself unions the previously-drawn and the freshly
 * snapshotted cursor rects into the clip region, so erase + redraw
 * always happen in the same frame with one consistent position. */
static int last_cur_x = -100, last_cur_y = -100;

/* Cursor sprite is 11x17 px; ±1 px margin, matching draw position. */
#define CUR_DMG_X(cx)  ((cx) - 1)
#define CUR_DMG_Y(cy)  ((cy) - 1)
#define CUR_DMG_W      14
#define CUR_DMG_H      20

/* -------------------------------------------------------------------------- */
/* M22.7-B — desktop shell / panel task.                                       */
/* -------------------------------------------------------------------------- */

static void pevq_push(uint8_t type, int x, int y) {
    uint32_t n = (pevq_h + 1) % PEVQ_SZ;
    if (n == pevq_t) return;
    pevq[pevq_h].type = type;
    pevq[pevq_h].x = (int16_t)x;
    pevq[pevq_h].y = (int16_t)y;
    pevq_h = n;
    need_frame = 1;
}

/* The active shell publishes its launcher-popup extent here (0 = closed).
 * The compositor composites this rect on top of the windows while open, and
 * the mouse IRQ routes clicks inside it to the panel. */
void gui_panel_set_popup(int on, int x, int y, int w, int h) {
    pnl_pop_x = x; pnl_pop_y = y; pnl_pop_w = w; pnl_pop_h = h;
    pnl_pop_on = on ? 1 : 0;
    panel_dirty = 1;
    need_frame = 1;
    /* The actual repaint (including the OLD extent when the menu closes) is
     * issued by desktop_main's panel-repaint block, OUTSIDE state_lock — this
     * setter can be called from vista_click with state_lock held, so it must
     * not touch the damage list itself. */
}

/* M22.7 — the shell asks for a chrome-only repaint (taskbar + open popup),
 * NOT a full-screen recompose.  vista_motion uses this for menu-hover
 * changes so gliding over the open menu doesn't repaint the whole 1920×1200
 * screen per motion event (the old gui_request_frame path — the menu lag). */
void gui_panel_dirty(void) {
    panel_dirty = 1;
    need_frame = 1;
}

/* Is (x,y) over the shell's chrome — the taskbar strip or the open popup? */
static int in_panel_region(int x, int y) {
    if (y >= work_h) return 1;                  /* taskbar strip (bottom_reserve) */
    if (pnl_pop_on && x >= pnl_pop_x && x < pnl_pop_x + pnl_pop_w &&
        y >= pnl_pop_y && y < pnl_pop_y + pnl_pop_h) return 1;
    return 0;
}

/* The desktop-shell task: renders the chrome into panelsurf and services
 * its input off the compositor.  shell->click/motion assume the WM lock is
 * held (their old IRQ contract), so we hold state_lock across them. */
static void dispatch_launches(void);            /* defined below; run by desktop */

/* Desktop-loop counters.  `gui stats` prints them, because "the taskbar is not
 * updating" has exactly three causes — the loop is not running, it is running
 * but never marks itself dirty, or it draws and the damage never reaches the
 * compositor — and from outside the guest they look identical. */
static volatile uint32_t desk_iters = 0, desk_draws = 0, desk_events = 0;
static volatile uint32_t desk_ticks = 0, desk_tick_dirty = 0;
static volatile uint32_t desk_now_ms = 0;

void gui_get_desktop_stats(struct gui_desktop_stats* out) {
    if (!out) return;
    out->iters      = desk_iters;
    out->draws      = desk_draws;
    out->events     = desk_events;
    out->ticks      = desk_ticks;
    out->tick_dirty = desk_tick_dirty;
    out->clock_ms   = desk_now_ms;
}

static void desktop_main(void) {
    kprintf("gui: desktop shell up on pid %d (shell '%s')\n",
            task_current() ? task_current()->pid : -1, shell ? shell->name : "none");
    uint64_t last_tick = 0;
    int gen_seen = -1;
    /* Previous launcher-popup extent — so when the menu closes we can repaint
     * the pixels it used to cover (compose only repaints the damage list). */
    int last_pop_on = 0, last_pop_x = 0, last_pop_y = 0, last_pop_w = 0, last_pop_h = 0;
    for (;;) {
        int busy = 0;
        desk_iters++;

        /* M22.7 — launches run HERE now: an app spawned from the taskbar (or
         * the `launch` command's queue) becomes a child of the desktop/
         * session, not of the display server (compositor). */
        dispatch_launches();

        while (pevq_t != pevq_h) {
            struct pev e = pevq[pevq_t];
            pevq_t = (pevq_t + 1) % PEVQ_SZ;
            if (e.type == PEV_DESK_CLICK || e.type == PEV_DESK_DBL) {
                /* §M64 — dispatched WITHOUT the WM lock, unlike the chrome
                 * events below.  A desktop click touches only the shell's own
                 * icon state, and activating a shortcut opens files and spawns
                 * an app-host task — work that must not run with state_lock
                 * held (§M49's "a metric is only as honest as the state it
                 * observes" had the same root: doing real work under a lock
                 * taken for something else). */
                if (shell && shell->desktop_click)
                    shell->desktop_click(e.x, e.y, e.type == PEV_DESK_DBL);
            } else {
                uint32_t fl = spin_lock_irqsave(&state_lock);
                if (e.type == PEV_CLICK) { if (shell && shell->click)  shell->click(e.x, e.y); }
                else                     { if (shell && shell->motion) shell->motion(e.x, e.y); }
                spin_unlock_irqrestore(&state_lock, fl);
            }
            /* Clicks change chrome state (menu, focus) → always repaint.
             * Motion is frequent; let the shell request a repaint itself
             * (vista only does so when the hover row changes) so a mouse
             * drag across the open menu doesn't repaint the chrome on every
             * event. */
            /* A desktop click changes only the icon layer, and that layer is
             * damaged precisely by the shell — repainting the whole panel for
             * it would undo §4.61's damage discipline on every click. */
            if (e.type == PEV_CLICK) panel_dirty = 1;
            desk_events++;
            busy = 1;
        }

        uint64_t now = timer_ticks_ms();
        desk_now_ms = (uint32_t)now;
        if (now - last_tick >= 500) {
            last_tick = now;
            desk_ticks++;
            if (shell && shell->second_tick && shell->second_tick()) {
                desk_tick_dirty++;
                panel_dirty = 1;
            }
        }
        if (panel_gen != gen_seen) { gen_seen = panel_gen; panel_dirty = 1; }

        if (panel_dirty) {
            panel_dirty = 0;
            desk_draws++;
            busy = 1;
            spin_lock(&panel_lock);
            if (shell && shell->draw) shell->draw(&panelsurf);
            spin_unlock(&panel_lock);
            gui_damage(0, work_h, fbsurf.w, fbsurf.h - work_h);   /* taskbar */
            /* Repaint the popup's CURRENT extent (if open) AND the extent it
             * had LAST frame (if it just closed or moved).  Without the "last"
             * rect a launcher menu that closes via the app-launch path — which
             * doesn't otherwise damage the screen — leaves its stale pixels on
             * screen ("the menu won't disappear").  This runs OUTSIDE state_lock
             * (gui_damage takes damage_lock), so no lock nesting. */
            if (pnl_pop_on)
                gui_damage(pnl_pop_x, pnl_pop_y, pnl_pop_w, pnl_pop_h);
            if (last_pop_on &&
                (!pnl_pop_on || last_pop_x != pnl_pop_x || last_pop_y != pnl_pop_y ||
                 last_pop_w != pnl_pop_w || last_pop_h != pnl_pop_h))
                gui_damage(last_pop_x, last_pop_y, last_pop_w, last_pop_h);
            last_pop_on = pnl_pop_on;
            last_pop_x = pnl_pop_x; last_pop_y = pnl_pop_y;
            last_pop_w = pnl_pop_w; last_pop_h = pnl_pop_h;
        }
        if (!busy) hal_cpu_idle();       /* halt only when idle (see compositor) */
        task_yield();
    }
}

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
};

/* Paint the whole scene (wallpaper → windows → rubber → panel → cursor)
 * into backsurf, clipped to one damage rect.  Called once per rect. */
static void draw_scene_rect(const struct scene_snapshot* s,
                            int rx0, int ry0, int rx1, int ry1) {
    gfx_set_clip(&backsurf, rx0, ry0, rx1 - rx0, ry1 - ry0);

    gfx_blit(&backsurf, 0, 0, &wallsurf, 0, 0, wallsurf.w, wallsurf.h);

    /* §M64 — the background LAYER: desktop icons sit on the wallpaper and
     * under every window.  Painted per damage rect like everything else here,
     * and clipped by the same clip box, so a shortcut only costs pixels when
     * its rectangle is actually dirty. */
    if (shell && shell->draw_under) shell->draw_under(&backsurf);

    for (int i = 0; i < s->zn; i++) {
        struct gui_window* win = s->zsnap[i];
        int x = s->wx[i], y = s->wy[i], w = s->ww[i], h = s->wh[i];
        int focused = (win == s->fsnap);

        gfx_blend_fill(&backsurf, x + 5, y + 5, w, h, COL_SHADOW);
        draw_rect_outline(&backsurf, x, y, w, h, BORDER,
                          focused ? COL_BORDER_F : COL_BORDER_U);
        gfx_vgradient(&backsurf, x + BORDER, y + BORDER,
                      w - 2 * BORDER, TITLE_H - BORDER,
                      focused ? COL_TITLE_F_TOP : COL_TITLE_U_TOP,
                      focused ? COL_TITLE_F_BOT : COL_TITLE_U_BOT);
        gfx_text(&backsurf, x + 8, y + (TITLE_H - GFX_GLYPH_H + BORDER) / 2,
                 win->title, COL_TITLE_TEXT);

        {
            int bx = x + w - BORDER - CLOSE_W - 3;
            int by = y + 4;
            gfx_fill(&backsurf, bx, by, CLOSE_W, CLOSE_H, COL_CLOSE_BG);
            gfx_text(&backsurf, bx + (CLOSE_W - GFX_GLYPH_W) / 2, by + 2, "x",
                     COL_CLOSE_FG);
            int xx = bx - CLOSE_W - 3;          /* maximize / restore */
            gfx_fill(&backsurf, xx, by, CLOSE_W, CLOSE_H, 0xFF3A4A5Eu);
            if (win->maximized) {
                draw_rect_outline(&backsurf, xx + 5, by + 2, CLOSE_W - 8,
                                  CLOSE_H - 6, 1, COL_TITLE_TEXT);
                draw_rect_outline(&backsurf, xx + 3, by + 4, CLOSE_W - 8,
                                  CLOSE_H - 6, 1, COL_TITLE_TEXT);
            } else {
                draw_rect_outline(&backsurf, xx + 3, by + 2, CLOSE_W - 6,
                                  CLOSE_H - 4, 1, COL_TITLE_TEXT);
            }
            int mx2 = xx - CLOSE_W - 3;
            gfx_fill(&backsurf, mx2, by, CLOSE_W, CLOSE_H, 0xFF3A4A5Eu);
            gfx_fill(&backsurf, mx2 + 3, by + CLOSE_H - 4, CLOSE_W - 6, 2,
                     COL_TITLE_TEXT);
        }

        spin_lock(&win->lock);
        gfx_blit(&backsurf, x + BORDER, y + TITLE_H,
                 &win->surf, 0, 0, win->surf.w, win->surf.h);
        spin_unlock(&win->lock);

        uint32_t gc = focused ? COL_BORDER_F : COL_BORDER_U;
        for (int t = 0; t < 3; t++) {
            int o = 4 + t * 4;
            gfx_line(&backsurf, x + w - 3 - o, y + h - 4,
                                x + w - 4,     y + h - 3 - o, gc);
        }
    }

    if (s->dsnap == DRAG_RESIZE && s->dwin)
        draw_rect_outline(&backsurf, s->rrx, s->rry, s->rw, s->rh, 2, COL_RUBBER);

    /* M22.7-B — desktop chrome (taskbar always + open popup) from panelsurf. */
    if (panel_ready) {
        spin_lock(&panel_lock);
        gfx_blit(&backsurf, 0, work_h, &panelsurf, 0, work_h,
                 fbsurf.w, fbsurf.h - work_h);
        if (pnl_pop_on) {
            int py = pnl_pop_y, ph = pnl_pop_h;
            if (py < panel_strip_top) { ph -= panel_strip_top - py; py = panel_strip_top; }
            if (ph > 0)
                gfx_blit(&backsurf, pnl_pop_x, py, &panelsurf,
                         pnl_pop_x, py, pnl_pop_w, ph);
        }
        spin_unlock(&panel_lock);
    }

    /* §M65 — the window popup sits above every window (and above the panel
     * chrome, so a menu near the bottom edge is not eaten by the taskbar) and
     * below the cursor, which is always last. */
    draw_popup(&backsurf);

    draw_cursor(&backsurf, s->cx, s->cy);
}

static void compose(void) {
    uint64_t compose_t0 = timer_now_ns();
    /* 1. Snapshot + clear the damage LIST: anything damaged while we paint
     *    lands in the next frame. */
    struct rect rl[DMG_MAX + 2];
    int rn;
    {
        uint32_t dfl = spin_lock_irqsave(&damage_lock);
        rn = dmg_n;
        for (int i = 0; i < rn; i++) rl[i] = dmg_list[i];
        dmg_n = 0;
        spin_unlock_irqrestore(&damage_lock, dfl);
    }

    /* 2. Snapshot the WM state (shared across all rects). */
    struct scene_snapshot s;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    s.zn = 0;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i]->minimized) continue;     /* M22.3 */
        s.zsnap[s.zn] = zorder[i];
        s.wx[s.zn] = zorder[i]->x;  s.wy[s.zn] = zorder[i]->y;
        s.ww[s.zn] = zorder[i]->w;  s.wh[s.zn] = zorder[i]->h;
        s.zn++;
    }
    s.cx = mx; s.cy = my;
    s.dsnap = drag; s.dwin = drag_win; s.rw = rubber_w; s.rh = rubber_h;
    s.rrx = s.rry = 0;
    if (s.dwin) { s.rrx = s.dwin->x; s.rry = s.dwin->y; }
    s.fsnap = focused_win;
    struct move_hint mh = mv_hint;
    mv_hint.active = 0;                          /* consumed exactly once     */
    spin_unlock_irqrestore(&state_lock, fl);

    /* ---- can this frame be a COPY instead of a repaint? ------------------
     *
     * Every condition below is a way the copy could produce a wrong image, so
     * each one is a fall-back to the ordinary painter rather than a fix-up:
     *
     *  - OTHER DAMAGE (rn != 0).  Something else changed and the merged rects
     *    can no longer be attributed; repaint everything.
     *  - NOT TOPMOST.  The source rectangle is only the dragged window's own
     *    pixels if nothing is drawn over it.  Anything above would be dragged
     *    along with it — a window that never moved, smearing across the screen.
     *  - MINIMISED / GONE / not the window being dragged now.
     *  - A ZERO move, which the copy would spend its whole cost on. */
    int fast = 0;
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;      /* the union (draw + present) */
    if (mh.active && mh.win && mh.win == s.dwin && s.zn > 0 &&
        s.zsnap[s.zn - 1] == mh.win &&           /* topmost visible           */
        rn == 0 &&                               /* nothing else changed      */
        (mh.nx != mh.ox || mh.ny != mh.oy)) {
        fast = 1;
        bx0 = (mh.ox < mh.nx ? mh.ox : mh.nx) - 2;
        by0 = (mh.oy < mh.ny ? mh.oy : mh.ny) - 2;
        bx1 = (mh.ox > mh.nx ? mh.ox : mh.nx) + mh.w + 7;   /* +5 shadow, +2  */
        by1 = (mh.oy > mh.ny ? mh.oy : mh.ny) + mh.h + 7;
    } else if (mh.active && mh.win) {
        /* The slow path still has to REPAINT the move, so the rects the drag
         * did not add now go in. */
        int ax0 = (mh.ox < mh.nx ? mh.ox : mh.nx) - 2;
        int ay0 = (mh.oy < mh.ny ? mh.oy : mh.ny) - 2;
        int ax1 = (mh.ox > mh.nx ? mh.ox : mh.nx) + mh.w + 7;
        int ay1 = (mh.oy > mh.ny ? mh.oy : mh.ny) + mh.h + 7;
        if (rn < DMG_MAX) rl[rn++] = (struct rect){ ax0, ay0, ax1, ay1 };
    }

    /* 3. Cursor damage from HERE (M22.4): erase the last-drawn sprite and
     *    draw the fresh one — two SEPARATE small rects, appended to the
     *    list (not unioned with far-away window damage). */
    int cur_moved = (s.cx != last_cur_x || s.cy != last_cur_y);
    int prev_cur_x = last_cur_x, prev_cur_y = last_cur_y;
    if (rn == 0 && !cur_moved && !fast) return; /* spurious wake */
    if (cur_moved && rn < DMG_MAX + 2)
        rl[rn++] = (struct rect){ CUR_DMG_X(last_cur_x), CUR_DMG_Y(last_cur_y),
                                  CUR_DMG_X(last_cur_x) + CUR_DMG_W,
                                  CUR_DMG_Y(last_cur_y) + CUR_DMG_H };
    if (cur_moved && rn < DMG_MAX + 2)
        rl[rn++] = (struct rect){ CUR_DMG_X(s.cx), CUR_DMG_Y(s.cy),
                                  CUR_DMG_X(s.cx) + CUR_DMG_W,
                                  CUR_DMG_Y(s.cy) + CUR_DMG_H };
    last_cur_x = s.cx;  last_cur_y = s.cy;

    /* 3b. THE COPY, and the small list of things that still have to be
     *     painted around it.
     *
     * Order is load-bearing and there is only one correct one: COPY FIRST.
     * The regions that need painting — the strip the window vacated, the
     * cursor's old footprint — lie INSIDE the source rectangle, and the
     * painter draws the scene as it is NOW (window already at its new
     * position).  Painting any of them before the copy would feed the copy
     * pixels that belong to the new frame, and the window would carry a band
     * of wallpaper across the screen with it. */
    int copy_x = 0, copy_y = 0, copy_w = 0, copy_h = 0;   /* what got filled  */
    if (fast) {
        int shx = mh.nx - mh.ox, shy = mh.ny - mh.oy;
        /* The valid source window: on screen at BOTH ends, and above the
         * panel — the taskbar is composited over the windows, so a source row
         * inside the panel strip holds panel pixels, not the window's. */
        int l = mh.ox, r = mh.ox + mh.w, t = mh.oy, b = mh.oy + mh.h;
        if (l < 0) l = 0;
        if (l < -shx) l = -shx;
        if (t < 0) t = 0;
        if (t < -shy) t = -shy;
        if (r > fbsurf.w) r = fbsurf.w;
        if (r > fbsurf.w - shx) r = fbsurf.w - shx;
        if (b > work_h) b = work_h;
        if (b > work_h - shy) b = work_h - shy;

        if (r - l > 0 && b - t > 0) {
            gfx_move_within(&backsurf, l, t, l + shx, t + shy, r - l, b - t);
            copy_x = l + shx; copy_y = t + shy;
            copy_w = r - l;   copy_h = b - t;
        } else {
            fast = 0;                            /* nothing worth copying     */
        }
    }

    if (fast) drag_fast++;
    if (mh.active && !fast) drag_slow++;

    if (fast) {
        /* Everything in the union EXCEPT what the copy just filled, as up to
         * four rectangles: the vacated strips, the new shadow band, and any
         * part of the destination the clipping above could not supply. */
        struct rect keep = { copy_x, copy_y, copy_x + copy_w, copy_y + copy_h };
        if (by0 < keep.y0 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ bx0, by0, bx1, keep.y0 };
        if (keep.y1 < by1 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ bx0, keep.y1, bx1, by1 };
        if (bx0 < keep.x0 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ bx0, keep.y0, keep.x0, keep.y1 };
        if (keep.x1 < bx1 && rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ keep.x1, keep.y0, bx1, keep.y1 };

        /* THE CURSOR CAME ALONG FOR THE RIDE.  draw_cursor paints the sprite
         * INTO the back buffer, so the source rectangle had a cursor burned
         * into it and the copy has just deposited a second one at
         * (old cursor + the move).  It is a handful of pixels and it is inside
         * the window, so repainting that one small rectangle removes it. */
        int shx = mh.nx - mh.ox, shy = mh.ny - mh.oy;
        if (rn < DMG_MAX + 2)
            rl[rn++] = (struct rect){ CUR_DMG_X(prev_cur_x) + shx,
                                      CUR_DMG_Y(prev_cur_y) + shy,
                                      CUR_DMG_X(prev_cur_x) + shx + CUR_DMG_W,
                                      CUR_DMG_Y(prev_cur_y) + shy + CUR_DMG_H };
    }

    /* 4. Clamp each rect to the screen; drop empties. */
    struct rect fr[DMG_MAX + 2];
    int fn = 0, any_full = 0;
    for (int i = 0; i < rn; i++) {
        int x0 = rl[i].x0, y0 = rl[i].y0, x1 = rl[i].x1, y1 = rl[i].y1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fbsurf.w) x1 = fbsurf.w;
        if (y1 > fbsurf.h) y1 = fbsurf.h;
        if (x1 <= x0 || y1 <= y0) continue;
        fr[fn++] = (struct rect){ x0, y0, x1, y1 };
        if (x0 == 0 && y0 == 0 && x1 == fbsurf.w && y1 == fbsurf.h) any_full = 1;
    }
    /* The copied rectangle is not painted — but it MUST be presented, or the
     * work stays in the back buffer and the screen shows the window still at
     * its old place.  It goes in as a present-only entry, remembered by index
     * so the draw pass below can step over exactly it. */
    int skip_draw = -1;
    if (fast && copy_w > 0 && copy_h > 0 && fn < DMG_MAX + 2) {
        fr[fn] = (struct rect){ copy_x, copy_y, copy_x + copy_w, copy_y + copy_h };
        skip_draw = fn++;
    }

    if (fn == 0) return;
    if (any_full) frames_full++; else frames_partial++;
    for (int k = 0; k < fn; k++)                 /* actual damage this frame */
        total_blit_px += (uint64_t)(fr[k].x1 - fr[k].x0) * (fr[k].y1 - fr[k].y0);

    /* 5. Draw pass — paint each rect into backsurf.  The copied rectangle is
     *    skipped: its pixels are already correct, and repainting them is the
     *    entire cost this path exists to avoid. */
    for (int k = 0; k < fn; k++) {
        if (k == skip_draw) continue;
        draw_scene_rect(&s, fr[k].x0, fr[k].y0, fr[k].x1, fr[k].y1);
    }
    gfx_clear_clip(&backsurf);

    /* 6. Present — blit each rect (M22.6 flip has buffer-age 2, so it also
     *    replays LAST frame's rects into the hidden buffer to complete it). */
    if (flip_ok) {
        int hidden = flip_front ^ 1;
        for (int k = 0; k < fn; k++)
            gfx_blit(&flipbuf[hidden], fr[k].x0, fr[k].y0, &backsurf,
                     fr[k].x0, fr[k].y0, fr[k].x1 - fr[k].x0, fr[k].y1 - fr[k].y0);
        for (int k = 0; k < prev_dmg_n; k++)
            gfx_blit(&flipbuf[hidden], prev_dmg[k].x0, prev_dmg[k].y0, &backsurf,
                     prev_dmg[k].x0, prev_dmg[k].y0,
                     prev_dmg[k].x1 - prev_dmg[k].x0,
                     prev_dmg[k].y1 - prev_dmg[k].y0);
        fb_flip_to(hidden);
        flip_front = hidden;
        prev_dmg_n = fn;
        for (int k = 0; k < fn; k++) prev_dmg[k] = fr[k];
    } else {
        for (int k = 0; k < fn; k++) {
            gfx_blit(&fbsurf, fr[k].x0, fr[k].y0, &backsurf,
                     fr[k].x0, fr[k].y0, fr[k].x1 - fr[k].x0, fr[k].y1 - fr[k].y0);
            /* Push the freshly-blitted rect to the scanout.  No-op on x86 (the
             * linear FB is the scanout); on aarch64 this is the virtio-gpu
             * transfer+flush that makes the compositor visible. */
            fb_present_flush(fr[k].x0, fr[k].y0,
                             fr[k].x1 - fr[k].x0, fr[k].y1 - fr[k].y0);
        }
    }

    /* At the END, after the draw pass AND the present.  The first version of
     * this accumulated before them and reported 15 microseconds a frame for
     * megabytes of blitting — it was timing the bookkeeping, which is the
     * cheapest thing in the function.  A measurement placed on the wrong side
     * of the work does not merely understate it; it says the work is free. */
    total_compose_ns += timer_now_ns() - compose_t0;
}

/* -------------------------------------------------------------------------- */
/* Window teardown (compositor task only).                                     */
/* -------------------------------------------------------------------------- */

static void destroy_window(struct gui_window* win) {
    /* M22.7 — a released WIN_APP already ran on_close + freed its widgets on
     * its host task; don't repeat it here.  WIN_TERM keeps the old path. */
    if (win->on_close && !win->host_released) win->on_close(win);

    /* §M54 — tell the handle owner the window is going away.  Unconditional and
     * BEFORE any teardown, because the whole point is that it must not depend
     * on which route got us here: on_close above is skipped for a released
     * window, and the crash route sets host_released, which is exactly the
     * combination that used to leave the dosgui bridge holding a handle to a
     * window that no longer exists. */
    if (win->on_dispose) {
        void (*cb)(struct gui_window*, void*) = win->on_dispose;
        void* ctx = win->dispose_ctx;
        win->on_dispose = NULL;                 /* fire once, never re-enter */
        win->dispose_ctx = NULL;
        cb(win, ctx);
    }

    uint32_t fl = spin_lock_irqsave(&state_lock);
    int i;
    for (i = 0; i < zcount && zorder[i] != win; i++) ;
    if (i < zcount) {
        for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
        zcount--;
    }
    if (drag_win == win) { drag = DRAG_NONE; drag_win = NULL; }
    struct gui_window* newfocus =
        (focused_win == win) ? top_visible_locked() : focused_win;
    focused_win = newfocus;
    spin_unlock_irqrestore(&state_lock, fl);

    if (newfocus && newfocus->kind == WIN_TERM) vc_focus(newfocus->vc);

    struct widget* w = win->widgets;
    while (w) {
        struct widget* nx = w->next;
        if (w->ops && w->ops->destroy) w->ops->destroy(w);  /* M22.5 */
        kfree(w);
        w = nx;
    }
    win->widgets = NULL;
    win->focusw  = NULL;
    gfx_surface_free(&win->surf);
    if (win->cells)   { kfree(win->cells); win->cells = NULL; }
    if (win->sb)      { kfree(win->sb);    win->sb = NULL; win->sb_cap = 0; }
    if (win->app_ctx) { kfree(win->app_ctx); win->app_ctx = NULL; }
    if (win->ui_state) { kfree(win->ui_state); win->ui_state = NULL; }
    win->used = 0;
    gui_damage_all();
}

/* -------------------------------------------------------------------------- */
/* Queue dispatch — runs on the compositor task.                               */
/* -------------------------------------------------------------------------- */

/* §M46 Ctrl+Alt+X — close the top-most user app window.  A client-managed
 * (package) window's want_close makes apply_pending force-kill the client, so it
 * works even when the app is frozen; a normal app-host window gets a graceful
 * want_close.  Runs on the compositor task, so taking state_lock is safe. */
static void sak_close_top_app(void) {
    struct gui_window* target = NULL;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    for (int i = zcount - 1; i >= 0; i--) {
        struct gui_window* w = zorder[i];
        if (w && w->used && w->kind == WIN_APP && !w->minimized) { target = w; break; }
    }
    if (target) target->want_close = 1;
    spin_unlock_irqrestore(&state_lock, fl);
    if (target) kprintf("gui: Ctrl+Alt+X — closing top app '%s'\n", target->title);
    else        kprintf("gui: Ctrl+Alt+X — no app window to close\n");
}

void gui_queue_open(void (*open_fn)(void)) {
    if (!open_fn) return;
    uint32_t n = (oq_h + 1) % LQ_SZ;
    if (n == oq_t) return;
    openq[oq_h] = open_fn;
    oq_h = n;
}

static void dispatch_launches(void) {
    if (sak_close_req) { sak_close_req = 0; sak_close_top_app(); }
    while (oq_t != oq_h) {
        void (*fn)(void) = openq[oq_t];
        oq_t = (oq_t + 1) % LQ_SZ;
        if (!fn) continue;
        struct task* host = task_spawn_arg("app:dialog", app_host_main,
                                           (void*)(uintptr_t)fn);
        if (host) task_set_reap_owned(host, 1);
    }
    while (lq_t != lq_h) {
        const struct gui_app_def* app = launchq[lq_t];
        lq_t = (lq_t + 1) % LQ_SZ;
        if (!app || !app->launch) continue;
        /* M22.7 — each app runs on its OWN task.  Spawn an app-host and hand
         * it the launch fn via start_arg; the host runs it (creating the
         * window(s) + widgets on that task) then services them.  A singleton
         * app whose open fn just raises an existing window creates nothing,
         * so its host exits immediately (init reaps it). */
        char nm[TASK_NAME_MAX + 1] = "app:";
        int p = 4;
        for (const char* s = app->name; s && *s && p < TASK_NAME_MAX; s++)
            nm[p++] = *s;
        nm[p] = 0;
        struct task* host =
            task_spawn_arg(nm, app_host_main, (void*)(uintptr_t)app->launch);
        if (!host) { kprintf("gui: app-host spawn failed for '%s'\n", app->name); continue; }
        /* The compositor owns the host's reap (window-teardown ordering) —
         * keep init off it, same contract as WIN_TERM shells. */
        task_set_reap_owned(host, 1);
    }
    if (power_req == 1) hal_reboot();
    if (power_req == 2) hal_shutdown();
    if (exit_req) {
        /* Not here: this is the compositor, and the teardown kills it.  Hand
         * the job to a task outside the session (see gui_teardown). */
        exit_req = 0;
        if (!task_spawn_detached("gui-stop", gui_stop_main))
            kprintf("gui: cannot spawn the teardown task - session stays up\n");
    }
}

/* M22.7 — the compositor no longer touches widgets: it drains the IRQ-fed
 * global queues and re-routes each event into the target window's per-window
 * queue (aq).  The owning app-host does the widget hit-test + dispatch +
 * redraw off the compositor. */
static void dispatch_events(void) {
    while (evq_t != evq_h) {
        struct gev e = evq[evq_t];
        evq_t = (evq_t + 1) % EVQ_SZ;
        struct gui_window* win = e.win;
        if (!win || !win->used || win->kind != WIN_APP) continue;
        struct app_event ae = { .type = e.dz ? AE_SCROLL
                                     : (e.ptr ? AE_POINTER
                                              : (e.btn ? AE_BUTTON : AE_MOUSE)),
                                .x = e.x, .y = e.y, .dbl = e.dbl,
                                .btn = e.btn, .down = e.down,
                                .phase = (uint8_t)(e.dz ? (uint8_t)e.dz
                                                        : (e.ptr ? e.ptr - 1 : 0)) };
        aq_push(win, ae);
    }
}

static void dispatch_keys(void) {
    while (keyq_t != keyq_h) {
        char c = keyq[keyq_t];
        keyq_t = (keyq_t + 1) % KEYQ_SZ;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        struct gui_window* win = focused_win;
        spin_unlock_irqrestore(&state_lock, fl);
        if (!win || !win->used || win->kind != WIN_APP) continue;
        struct app_event ae = { .type = AE_KEY, .c = c };
        aq_push(win, ae);
    }
}

/* M22.5 — raw keycode events to the focused widget (see widget.h). */
static void dispatch_keycodes(void) {
    while (kcq_t != kcq_h) {
        uint16_t e = kcq[kcq_t];
        kcq_t = (kcq_t + 1) % KCQ_SZ;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        struct gui_window* win = focused_win;
        spin_unlock_irqrestore(&state_lock, fl);
        if (!win || !win->used || win->kind != WIN_APP) continue;
        struct app_event ae = { .type = AE_KEYCODE, .kc = (uint8_t)(e & 0xFF),
                                .mods = (uint8_t)(e >> 8) };
        aq_push(win, ae);
    }
}

/* M22.7 — is `t` still referenced by any live window (app-host or the
 * terminal-shell task)?  Used to decide when an app-host can be reaped. */
static int gui_task_referenced(struct task* t) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* w = &windows[i];
        if (!w->used) continue;
        if (w->host_task == t) return 1;
        if (w->kind == WIN_TERM && w->vc && w->vc->task == t) return 1;
    }
    return 0;
}

/* Reap an app-host once it is DEAD and owns no more windows.  The host is
 * reap_owned (init won't touch it), so the compositor is its sole reaper. */
static void reap_gui_host(struct task* host) {
    if (!host || host->state != TASK_DEAD) return;
    if (gui_task_referenced(host)) return;
    task_reap(host->pid);
}

/* Sweep for DEAD reap_owned tasks no window references — catches a
 * singleton app whose open fn only raised an existing window (its host
 * created nothing and exited immediately).  Run on task-set changes.
 * A terminal shell mid-teardown is still referenced (win->vc->task), so
 * this never races the WIN_TERM reap path. */
struct gui_host_scan { int pids[GUI_MAX_WINDOWS * 2]; int n; };
static void gui_host_scan_cb(const struct task* t, int is_current, void* ctx) {
    struct gui_host_scan* s = (struct gui_host_scan*)ctx;
    if (is_current || t->state != TASK_DEAD || !t->reap_owned) return;
    if (s->n < (int)(sizeof s->pids / sizeof s->pids[0])) s->pids[s->n++] = t->pid;
}
static void reap_dead_gui_hosts(void) {
    struct gui_host_scan s = { .n = 0 };
    task_for_each(gui_host_scan_cb, &s);
    for (int i = 0; i < s.n; i++) {
        struct task* t = task_find(s.pids[i]);
        if (t && !gui_task_referenced(t)) task_reap(t->pid);
    }
}

static void apply_pending(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used) continue;

        /* M22.6 — auto-close a terminal window whose hosted task has died
         * by ANY route: the window's own X button (want_close, below),
         * the Task Manager's "End task", a CLI `kill`, or the task simply
         * returning from its entry.  Without this, an externally-killed
         * shell left its (now inert, un-typeable) window on screen.
         *
         * The trigger is TASK_DEAD — the task has ACTUALLY stopped.  A
         * task merely FLAGGED to stop (task_kill sets kill_pending; a
         * kthread only dies at its next yield / task_should_stop poll) is
         * still RUNNABLE, so its window stays until it truly terminates —
         * that is the "instruction to stop" vs "has stopped" distinction.
         *
         * Safe pointer: a VC-bound DEAD task is reaped ONLY by the
         * want_close path below (which nulls win->vc->task); the Task
         * Manager's reap pass skips vc_task_bound tasks.  So win->vc->task
         * stays valid here until we tear it down. */
        if (!win->want_close && win->kind == WIN_TERM &&
            win->vc && win->vc->task &&
            win->vc->task->state == TASK_DEAD) {
            kprintf("gui: window '%s' auto-closing (hosted pid %d died)\n",
                    win->title, win->vc->task->pid);
            win->want_close = 1;
        }

        /* M22.7 — same for a WIN_APP whose host task died (e.g. End task /
         * CLI kill of the app-host).  host_task is reap_owned, so it stays
         * valid until WE reap it below — the ->state read is safe. */
        if (!win->want_close && win->kind == WIN_APP &&
            win->host_task && win->host_task->state == TASK_DEAD &&
            !win->host_released) {
            win->want_close = 1;
        }

        /* §M46 — a CLIENT-MANAGED window (dosgui bridge, host_task == NULL) whose
         * ring-3 client died WITHOUT a clean DOSGUI_DESTROY (force-kill / crash):
         * the client can no longer release the window, so the compositor does it.
         * task_find(NULL/DEAD) = the client is gone → mark disposable.  (init is
         * the client's reaper; we only dispose the window.) */
        if (!win->want_close && win->kind == WIN_APP && !win->host_released &&
            win->host_task == NULL && win->client_pid > 0) {
            struct task* ct = task_find(win->client_pid);
            if (!ct || ct->state == TASK_DEAD) {
                kprintf("gui: client-managed window '%s' orphaned (pid %d gone) — disposing\n",
                        win->title, win->client_pid);
                win->host_released = 1;   /* client gone; nothing to coordinate */
                win->want_close    = 1;   /* teardown below disposes it */
            }
        }

        /* §M46 — a client-managed (package) window whose X button was clicked
         * (want_close).  A dosgui client is expected to poll the close event and
         * quit itself, but a WEDGED client (frozen browser) never will, so its
         * window could otherwise never close — "the chrome doesn't work when the
         * app is frozen".  Handle it on the compositor task (outside state_lock —
         * task_find/task_force_kill take the scheduler lock, which must not nest
         * under state_lock):
         *   - client already gone → mark host_released so the teardown disposes;
         *   - client still alive + close_forces_kill → force-kill it (M46), then
         *     wait for it to die (the next passes fall into the "gone" branch).
         * The X button thus ALWAYS closes the window.  With close_forces_kill off
         * the window waits for a cooperative quit instead (classic behaviour). */
        if (win->want_close && win->kind == WIN_APP && !win->host_released &&
            win->host_task == NULL && win->client_pid > 0) {
            struct task* ct = task_find(win->client_pid);
            if (!ct || ct->state == TASK_DEAD) {
                win->host_released = 1;              /* nothing left to coordinate */
            } else if (close_forces_kill && !ct->kill_forced) {
                uint64_t now = timer_ticks_ms();
                if (win->close_force_now) {
                    kprintf("gui: second close click on '%s' → force-killing "
                            "client pid %d\n", win->title, win->client_pid);
                    task_force_kill(win->client_pid);
                } else if (!win->close_deadline_ms) {
                    /* First pass: start the backstop and let the client quit. */
                    win->close_deadline_ms = now + close_grace_ms;
                } else if (now >= win->close_deadline_ms) {
                    kprintf("gui: '%s' did not close within %ums → force-killing "
                            "client pid %d\n", win->title, close_grace_ms,
                            win->client_pid);
                    task_force_kill(win->client_pid);
                }
            }
        }

        /* M22.7 — WIN_APP teardown is a two-actor dance.  Normally the host
         * sees want_close, runs on_close + frees its widgets, and sets
         * host_released; we then dispose the struct.  If the host died
         * WITHOUT releasing (it was killed), we do that cleanup here since
         * the host can no longer touch the widgets. */
        if (win->want_close && win->kind == WIN_APP) {
            if (!win->host_released) {
                int host_dead = win->host_task &&
                                win->host_task->state == TASK_DEAD;
                if (!host_dead) continue;       /* host still cleaning up */
                if (win->on_close) win->on_close(win);
                app_widgets_free(win);
                win->host_released = 1;
            }
            struct task* host = win->host_task;
            kprintf("gui: app window '%s' closed (host pid %d)\n",
                    win->title, host ? host->pid : -1);
            win->want_close = 0;
    win->close_deadline_ms = 0;
    win->close_force_now = 0;
            destroy_window(win);
            reap_gui_host(host);                /* reap once its last window is gone */
            continue;
        }

        if (win->want_close) {
            if (win->kind == WIN_TERM && win->vc && win->vc->task) {
                /* Kill the hosted shell first (cooperative — it dies at
                 * its next vc_getchar yield), then reap; retry on the
                 * next compositor pass until the reap succeeds.  Only
                 * then is it safe to free the VC and the surface.
                 *
                 * M27 — kill the whole SUBTREE: anything the shell spawned
                 * (e.g. `spawn`) dies with the window instead of orphaning.
                 * The shell itself is reap_owned, so WE reap it here; its
                 * (non-owned) children are reaped by init once they die. */
                struct task* t = win->vc->task;
                task_kill_tree(t->pid);
                if (task_reap(t->pid) != 0) continue;   /* not DEAD yet */
                win->vc->task = NULL;
            }
            if (win->kind == WIN_TERM && win->vc) {
                vc_destroy(win->vc);
                win->vc = NULL;
            }
            win->want_close = 0;
    win->close_deadline_ms = 0;
    win->close_force_now = 0;
            destroy_window(win);
            continue;
        }

        int nw = 0, nh = 0;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        if (win->pending_w) {
            nw = win->pending_w;  nh = win->pending_h;
            win->pending_w = win->pending_h = 0;
            win->w = nw;  win->h = nh;
        }
        spin_unlock_irqrestore(&state_lock, fl);

        if (nw && window_set_size(win, nw, nh) != 0)
            kprintf("gui: resize OOM (%dx%d), window keeps stale surface\n", nw, nh);
    }
}

/* §M40 — deliver queued input for a WIN_APP window that has an INPUT HOOK but
 * NO app-host task.
 *
 * A hook-backed window has no app-host that drains its queue: a Wayland window
 * is created by the server task, which then blocks reading its client's socket,
 * and a dosgui window (NetSurf) is client-managed with host_task cleared
 * outright.  Either way app_host_main never runs for it, so queued events
 * simply piled up and the hook was never called.  The §M26 demo hid this by
 * synthesising input directly, so it only surfaced once a REAL client asked for
 * a wl_seat.
 *
 * The compositor is the right owner: it is an ordinary task (the hook does a
 * socket send, which must not happen in the mouse IRQ) and a hook-backed window
 * has no widgets for anyone else to dispatch to. */
static void pump_hostless_input(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used || win->kind != WIN_APP) continue;
        if (!win->input_hook) continue;
        while (win->aq_t != win->aq_h) {
            struct app_event e = win->aq[win->aq_t];
            win->aq_t = (win->aq_t + 1) % AQ_SZ;
            app_dispatch_event(win, &e);
        }
    }
}

/* §M65 — DRAW the widgets of a window that has no host task.
 *
 * §M40 taught the compositor to PUMP INPUT for such a window (a Wayland or
 * dosgui client's window has `host_task` cleared by design, §M54) — and the
 * same hole existed on the drawing side, invisibly, until a ring-3 program
 * built toolkit widgets in its window and got an empty rectangle: the widgets
 * existed, the layout ran, and nothing ever painted them.
 *
 * A window in this mode uses the toolkit INSTEAD of blitting its own pixels;
 * if a client does both, the last writer wins, which is the honest outcome of
 * asking for both. */
static void pump_hostless_redraw(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used || win->kind != WIN_APP) continue;
        if (win->host_task || !win->widgets) continue;
        if (!win->layout_pending) continue;
        win->layout_pending = 0;
        ui_layout(win);
        app_redraw(win);
    }
}

static void gui_compositor_main(void) {
    kprintf("gui: compositor up on pid %d (shell '%s')\n",
            task_current() ? task_current()->pid : -1,
            shell ? shell->name : "none");
    uint64_t last_tick = 0;
    for (;;) {
        /* M22.7 — app launches moved to the desktop task (so launched apps
         * are children of the desktop/session, not the display server). */
        dispatch_events();
        dispatch_keys();
        dispatch_keycodes();
        pump_hostless_input();
        pump_hostless_redraw();          /* §M65 — …and paint them */
        apply_pending();
        term_selection_service();       /* §M58 — repaint + copy */
        apply_mode_change();            /* §M61 — resolution, between frames */
        apply_mode_revert();            /* §M61 — …and the undo, same rule */

        /* ~1 Hz per-window housekeeping.  (The shell clock moved to the
         * desktop task in M22.7-B; the compositor no longer runs it.) */
        uint64_t now = timer_ticks_ms();
        if (now - last_tick >= 500) {
            last_tick = now;
            /* M22.3/M22.7: per-window ~1 Hz ticks (e.g. task manager
             * refresh) — signal the owning app-host, which runs on_tick on
             * its own task rather than blocking the compositor here. */
            for (int i = 0; i < GUI_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick)
                    windows[i].tick_pending = 1;
        }

        /* M22.4: task set changed (spawn/kill/exit/reap) → nudge the tick
         * refreshes immediately, don't wait for the 1 Hz beat.  Ticks are
         * idempotent refreshes, so an early one is safe. */
        if (tasks_changed) {
            tasks_changed = 0;
            for (int i = 0; i < GUI_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick)
                    windows[i].tick_pending = 1;
            reap_dead_gui_hosts();          /* M22.7 — sweep exited app-hosts */
        }

        /* M22.7 — latency fix: halt the CPU only when there is NOTHING to
         * compose.  The old unconditional hal_cpu_idle() slept a whole timer
         * tick every iteration, so with several always-runnable tasks
         * (compositor + desktop + app-hosts) the compositor's turn came
         * around only every N ticks — visible cursor lag with the menu or
         * Task Manager open.  Under load need_frame stays set, so we spin
         * through the scheduler (fast); when truly idle we hlt (power save). */
        if (need_frame) {
            need_frame = 0;
            compose();
        } else {
            hal_cpu_idle();
        }
        task_yield();
    }
}

/* -------------------------------------------------------------------------- */
/* Pointer handling — IRQ context.                                             */
/* -------------------------------------------------------------------------- */

static struct gui_window* topmost_at(int px, int py) {
    for (int i = zcount - 1; i >= 0; i--) {
        struct gui_window* w = zorder[i];
        if (w->minimized) continue;             /* M22.3 */
        if (px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h)
            return w;
    }
    return NULL;
}

static void raise_window(struct gui_window* win) {
    int i;
    for (i = 0; i < zcount && zorder[i] != win; i++) ;
    if (i >= zcount) return;
    for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = win;
}

/* Read the gate ONCE.  This runs on the mouse path, and a string lookup in the
 * config store per drag is a cost the feature is supposed to be measuring. */
static int drag_report_on(void) {
    static int cached = -1;
    if (cached < 0) cached = (int)config_get_long("gui.drag_stats", 0);
    return cached;
}

static void gui_mouse(int dx, int dy, unsigned buttons) {
    /* M22.4 — per-motion drag damage bookkeeping (filled under the
     * lock, consumed after unlock so gui_damage isn't nested deeper
     * than it has to be). */
    int drag_moved = 0;
    /* Only the ORIGIN is still needed here: where the window came from, for
     * the move hint.  The rest of the old bookkeeping (size, destination)
     * moved into the hint itself when the drag stopped raising damage rects. */
    int drag_old_x = 0, drag_old_y = 0;
    /* M22.7 — precise structural damage: the windows whose look changed
     * (focus highlight, z-order raise, minimize) instead of the whole
     * screen.  Captured under the lock, damaged after unlock. */
    struct gui_window* clicked = NULL;
    int force_full = 0;                          /* geometry change → full damage */

    spin_lock(&state_lock);
    struct gui_window* old_focus = focused_win;

    mx += dx;  my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= fbsurf.w) mx = fbsurf.w - 1;
    if (my >= fbsurf.h) my = fbsurf.h - 1;

    unsigned pressed  =  buttons & ~btn_prev;
    unsigned released = ~buttons &  btn_prev;
    btn_prev = buttons;

    /* §M65 — window popup hover.  A plain integer store in the IRQ; the
     * repaint is a damage request, not a draw (§M22.7's split).
     *
     * THE RECT IS THE POINT.  The first version set `need_frame` and nothing
     * else — and a frame with an EMPTY damage list repaints only the cursor's
     * own rectangle (§4.61: a pure glide is a bare wake).  So the highlight was
     * painted into the cursor's footprint and never anywhere else: dragging
     * down the menu left a trail of cursor-sized coloured patches instead of
     * moving one highlighted row.  Reported from use, and exactly the symptom
     * you would predict from "asked for a frame, claimed no area". */
    int popup_hover_moved = 0;
    if (popup.active) {
        int r = popup_row_at(mx, my);
        if (r != popup.hover) { popup.hover = r; popup_hover_moved = 1; }
    }

    /* M22.7-B — chrome hover (launcher highlight): only meaningful while the
     * popup is open; route it to the desktop task instead of running the
     * shell in the IRQ. */
    if (pnl_pop_on) pevq_push(PEV_MOTION, mx, my);

    /* §M40 — a window that forwards its input to a CLIENT (Wayland) wants the
     * whole pointer stream, not just clicks: `wl_pointer.motion` is how an
     * application tracks the cursor at all.  Widget windows deliberately get
     * motion only on click (a widget hit-test per mouse packet would be
     * pointless work), so this is gated on the hook rather than made general. */
    {
        struct gui_window* hw = topmost_at(mx, my);
        if (hw && hw->kind == WIN_APP && hw->input_hook && !hw->minimized)
            evq_push(hw, mx - hw->x - BORDER, my - hw->y - TITLE_H, 0, 0, 0);
    }

    /* §M58 — motion while a TERMINAL selection is in progress: extend the range
     * and ask the compositor to re-render.  The comparison is what stops a
     * motionless drag from re-rendering the grid on every mouse packet. */
    if (term_sel_win && term_sel_win->used && (dx || dy)) {
        struct gui_window* tw = term_sel_win;
        int r, c;
        gterm_cell_at(tw, mx - tw->x - BORDER, my - tw->y - TITLE_H, &r, &c);
        if (r != tw->sel_br || c != tw->sel_bc || !tw->sel_on) {
            tw->sel_br = r; tw->sel_bc = c;
            tw->sel_on = (r != tw->sel_ar || c != tw->sel_ac);
            term_sel_dirty = tw;
        }
    }

    /* §M58 — motion while a widget holds the pointer grab.  Sent to the
     * GRABBING window regardless of what is under the cursor now: a selection
     * that stops at the widget's edge is not a selection. */
    if (grab_win && grab_win->used && !grab_win->minimized && (dx || dy))
        evq_push_ptr(grab_win, mx - grab_win->x - BORDER,
                     my - grab_win->y - TITLE_H, WPTR_DRAG);

    /* §M58/§M59 — MIDDLE-CLICK PASTE of the primary selection into a terminal.
     * This is the entire reason the primary selection is a separate slot: you
     * select with the left button and paste with the middle one, without either
     * touching what you deliberately copied with Ctrl+C.  Recorded here, done
     * on the compositor (it reads the clipboard and pushes characters into a
     * VC — neither is IRQ work). */
    if (pressed & MOUSE_BTN_MIDDLE) {
        struct gui_window* pw = topmost_at(mx, my);
        if (pw && pw->kind == WIN_TERM && pw->vc && !pw->minimized &&
            my >= pw->y + TITLE_H)
            term_paste_win = pw;
    }

    /* Right press goes only to a client window, and only over its content —
     * the desktop chrome has no right-click behaviour to compete with. */
    if (pressed & MOUSE_BTN_RIGHT) {
        struct gui_window* rw = topmost_at(mx, my);
        if (rw && rw->kind == WIN_APP && rw->input_hook && !rw->minimized &&
            my >= rw->y + TITLE_H)
            evq_push(rw, mx - rw->x - BORDER, my - rw->y - TITLE_H, 0, 2, 1);
    }

    if (pressed & MOUSE_BTN_LEFT) {
        /* Desktop chrome gets first refusal.  A click over the taskbar or
         * the open popup is consumed and handed to the desktop task; a click
         * elsewhere while the popup is open also goes there (to dismiss the
         * menu) but still falls through to the windows below. */
        if (in_panel_region(mx, my)) {
            pevq_push(PEV_CLICK, mx, my);
            goto drag_update;
        }
        /* §M65 — AN OPEN POPUP OWNS THE NEXT CLICK, wherever it lands.  Inside
         * it is a choice; outside it is a dismissal — and in BOTH cases the
         * click must not also reach the window underneath, or dismissing a
         * menu would activate whatever happened to be behind it. */
        if (popup.active) {
            int row = popup_row_at(mx, my);
            struct gui_window* ow = popup.owner;
            int tag = popup.tag;
            popup.active = 0;
            popup.hover = -1;
            if (ow && ow->used) {
                struct app_event e = {0};
                e.type = AE_POPUP;
                e.x = (int16_t)row;         /* -1 = dismissed without a choice */
                e.y = (int16_t)tag;
                aq_push(ow, e);
            }
            spin_unlock(&state_lock);
            gui_damage_all();
            return;
        }

        if (pnl_pop_on) pevq_push(PEV_CLICK, mx, my);   /* dismiss, then windows */

        struct gui_window* win = topmost_at(mx, my);
        clicked = win;                          /* for precise structural damage */
        if (win) {
            gui_wm_focus_raise_locked(win);

            int bx1 = win->x + win->w - BORDER - 3;          /* close right edge */
            int in_btn_row = (my >= win->y + 4 && my < win->y + 4 + CLOSE_H);
            int in_close = (in_btn_row &&
                            mx >= bx1 - CLOSE_W && mx < bx1);
            int in_max   = (in_btn_row &&                    /* M22.5 */
                            mx >= bx1 - 2 * CLOSE_W - 3 &&
                            mx <  bx1 - CLOSE_W - 3);
            int in_min   = (in_btn_row &&
                            mx >= bx1 - 3 * CLOSE_W - 6 &&
                            mx <  bx1 - 2 * CLOSE_W - 6);
            if (in_close) {
                /* Second click on an already-requested close = "force it".
                 * Runs in the mouse IRQ, so this is a plain volatile store; the
                 * compositor acts on it (task_force_kill takes locks we must
                 * not take here). */
                if (win->want_close) win->close_force_now = 1;
                win->want_close = 1;
            } else if (in_max) {
                toggle_maximize_locked(win);                 /* M22.5 */
                force_full = 1;                              /* geometry change */
            } else if (in_min) {
                win->minimized = 1;
                struct gui_window* nf = top_visible_locked();
                focused_win = nf;
                if (nf && nf->kind == WIN_TERM) vc_focus(nf->vc);
            } else if (my < win->y + TITLE_H) {
                /* M22.5 — double-click on the title bar toggles
                 * maximize; a single click starts a drag (disabled
                 * while maximized). */
                uint64_t now = timer_ticks_ms();
                int dbl = (win == lastclick_win &&
                           now - lastclick_ms < 400 &&
                           mx - lastclick_x < 6 && lastclick_x - mx < 6 &&
                           my - lastclick_y < 6 && lastclick_y - my < 6);
                lastclick_ms = now;
                lastclick_x = mx; lastclick_y = my;
                lastclick_win = win;
                if (dbl) {
                    toggle_maximize_locked(win);
                    force_full = 1;                          /* geometry change */
                } else if (!win->maximized) {
                    drag     = DRAG_MOVE;
                    drag_win = win;
                    grab_dx  = mx - win->x;
                    grab_dy  = my - win->y;
                    drag_t0_ms = timer_ticks_ms();
                    drag_compose0_ns = total_compose_ns;
                    drag_px0 = total_blit_px;
                    drag_f0  = frames_full + frames_partial;
                    drag_motions = drag_frames = 0;
                    drag_fast = drag_slow = 0;
                }
            } else if (!win->maximized &&
                       mx >= win->x + win->w - GRIP &&
                       my >= win->y + win->h - GRIP) {
                drag     = DRAG_RESIZE;
                drag_win = win;
                rubber_w = win->w;
                rubber_h = win->h;
            } else if (win->kind == WIN_APP) {
                int cxr = mx - win->x - BORDER;
                int cyr = my - win->y - TITLE_H;
                uint64_t now = timer_ticks_ms();
                int dbl = (win == lastclick_win &&
                           now - lastclick_ms < 400 &&
                           mx - lastclick_x < 6 && lastclick_x - mx < 6 &&
                           my - lastclick_y < 6 && lastclick_y - my < 6);
                lastclick_ms = now;
                lastclick_x = mx; lastclick_y = my;
                lastclick_win = win;
                /* Motion first (so the client's pointer is where the click
                 * happened), then the press itself.  A widget window ignores
                 * the button event; a client window needs both. */
                evq_push(win, cxr, cyr, dbl, 0, 0);
                evq_push(win, cxr, cyr, dbl, 1, 1);
                /* §M58 — and the phase stream, plus the grab that keeps it
                 * coming after the pointer leaves the widget. */
                evq_push_ptr(win, cxr, cyr, WPTR_PRESS);
                grab_win = win;
            } else if (win->kind == WIN_TERM && win->cells) {
                /* §M58 — TEXT SELECTION in a terminal window.  A terminal is
                 * not a widget window, so this cannot ride the widget pointer
                 * path; it works directly on the CELL GRID the compositor
                 * already owns.  Anchor here, extend in the motion handler,
                 * copy on release — the IRQ only ever records a range.
                 *
                 * The branch had to be added: content clicks were gated on
                 * `kind == WIN_APP`, so a press inside a terminal reached
                 * nothing at all.  That is why every command's output — the
                 * text people most want to copy — was the one thing that could
                 * not be selected. */
                int cxr = mx - win->x - BORDER;
                int cyr = my - win->y - TITLE_H;
                gterm_cell_at(win, cxr, cyr, &win->sel_ar, &win->sel_ac);
                win->sel_br = win->sel_ar;
                win->sel_bc = win->sel_ac;
                if (win->sel_on) { win->sel_on = 0; term_sel_dirty = win; }
                term_sel_win = win;
            }
        } else {
            /* §M64 — nothing under the pointer: this is a click on the DESKTOP
             * itself.  Hand it to the shell's desktop_click through the same
             * queue the panel uses, because we are in the mouse IRQ with the
             * WM lock held and a shortcut activation opens files and spawns
             * tasks (M22.7's rule, and §M49 found the same class of bug when
             * a console was bound outside its spawn).
             *
             * Double-click is detected HERE rather than in the shell: the
             * timestamps and the previous click position already live in this
             * file, and a second copy of the rule would drift from the title
             * bar's. */
            uint64_t now = timer_ticks_ms();
            int dbl = (lastclick_win == NULL &&
                       now - lastclick_ms < 400 &&
                       mx - lastclick_x < 6 && lastclick_x - mx < 6 &&
                       my - lastclick_y < 6 && lastclick_y - my < 6);
            lastclick_ms = now;
            lastclick_x = mx; lastclick_y = my;
            lastclick_win = NULL;
            pevq_push(dbl ? PEV_DESK_DBL : PEV_DESK_CLICK, mx, my);
        }
    }

drag_update:
    if (drag == DRAG_MOVE && drag_win) {
        /* M22.4 — rect-bounded drag damage: remember the old outer rect
         * so the post-unlock path can damage old ∪ new instead of the
         * whole screen.  Before this fix every motion event during a
         * drag raised gui_damage_all() — a full 1280×800 recompose +
         * ~4 MB blit per event, which made the scene "swim".
         *
         * §perf — THROTTLE: only actually move + damage the window every
         * DRAG_FRAME_MS; intermediate motions just advance the cursor (the
         * post-unlock `else` branch sets need_frame for a cheap cursor-only
         * recompose).  This caps the big per-move blit to ~33 fps so a fast
         * drag of a large window can't monopolise the CPU. */
        int tgt_x = mx - grab_dx, tgt_y = my - grab_dy;
        if (tgt_x < -(drag_win->w - 40)) tgt_x = -(drag_win->w - 40);
        if (tgt_x > fbsurf.w - 40)       tgt_x = fbsurf.w - 40;
        if (tgt_y < 0)                   tgt_y = 0;
        if (tgt_y > work_h - TITLE_H)    tgt_y = work_h - TITLE_H;
        uint64_t now = timer_ticks_ms();
        drag_motions++;
        if ((tgt_x != drag_win->x || tgt_y != drag_win->y) &&
            (uint64_t)(now - last_drag_frame_ms) >= DRAG_FRAME_MS) {
            drag_frames++;
            last_drag_frame_ms = now;
            drag_old_x = drag_win->x;  drag_old_y = drag_win->y;
            drag_win->x = tgt_x;  drag_win->y = tgt_y;
            drag_moved = 1;
            /* Publish the move.  If several motions coalesce before the
             * compositor runs, the LAST one wins and the origin stays the
             * position the screen actually shows — an intermediate origin
             * would name pixels that were never on screen. */
            if (!mv_hint.active || mv_hint.win != drag_win) {
                mv_hint.ox = drag_old_x;  mv_hint.oy = drag_old_y;
            }
            mv_hint.active = 1;
            mv_hint.win = drag_win;
            mv_hint.nx = tgt_x;  mv_hint.ny = tgt_y;
            mv_hint.w  = drag_win->w;  mv_hint.h = drag_win->h;
        }
        /* else: coalesce — the window catches up on the next allowed frame;
         * the cursor still glides (need_frame set below). */
    } else if (drag == DRAG_RESIZE && drag_win) {
        rubber_w = mx - drag_win->x + 2;
        rubber_h = my - drag_win->y + 2;
        if (rubber_w < MIN_W) rubber_w = MIN_W;
        if (rubber_h < MIN_H) rubber_h = MIN_H;
        if (rubber_w > fbsurf.w) rubber_w = fbsurf.w;
        if (rubber_h > work_h)   rubber_h = work_h;
    }

    /* Button RELEASE.  A client needs the up edge as much as the down one —
     * without it a link click never completes and a drag never ends. */
    if (released & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT)) {
        struct gui_window* rw = topmost_at(mx, my);
        if (rw && rw->kind == WIN_APP && rw->input_hook && !rw->minimized &&
            my >= rw->y + TITLE_H)
            evq_push(rw, mx - rw->x - BORDER, my - rw->y - TITLE_H, 0,
                     (released & MOUSE_BTN_LEFT) ? 1 : 2, 0);
    }

    if ((released & MOUSE_BTN_LEFT) && term_sel_win) {
        /* Finish the selection.  The COPY happens on the compositor task
         * (clipboard_set allocates), so all this does is mark it ready. */
        if (term_sel_win->sel_on) term_sel_copy = term_sel_win;
        term_sel_win = NULL;
    }

    if ((released & MOUSE_BTN_LEFT) && grab_win) {
        if (grab_win->used)
            evq_push_ptr(grab_win, mx - grab_win->x - BORDER,
                         my - grab_win->y - TITLE_H, WPTR_RELEASE);
        grab_win = NULL;
    }

    if (released & MOUSE_BTN_LEFT) {
        if (drag == DRAG_MOVE && drag_win && drag_report_on()) {
            uint32_t ms  = (uint32_t)(timer_ticks_ms() - drag_t0_ms);
            uint32_t cms = (uint32_t)((total_compose_ns - drag_compose0_ns) / 1000000ull);
            uint32_t kb  = (uint32_t)(((total_blit_px - drag_px0) * 4) / 1024);
            uint32_t fr  = (frames_full + frames_partial) - drag_f0;
            kprintf("gui: drag %dx%d — %u motions, %u moved, %u frames "
                    "(%u copied, %u repainted), %u KB, %u ms in compose of "
                    "%u ms elapsed\n",
                    drag_win->w, drag_win->h, drag_motions, drag_frames,
                    fr, drag_fast, drag_slow, kb, cms, ms);
        }
        if (drag == DRAG_RESIZE) force_full = 1;       /* geometry changes */
        if (drag == DRAG_RESIZE && drag_win &&
            (rubber_w != drag_win->w || rubber_h != drag_win->h)) {
            drag_win->pending_w = rubber_w;
            drag_win->pending_h = rubber_h;
        }
        drag     = DRAG_NONE;
        drag_win = NULL;
    }

    /* M22.7 — damage policy.  A resize rubber band spans the window and
     * shrinks/grows, so it keeps the full recompose (rare — only while
     * dragging the grip).  A press/release only changes focus + z-order:
     * damage just the affected windows (old focus un-highlights, the
     * clicked window raises + highlights) instead of the whole 9 MB
     * screen.  DRAG_MOVE damages old∪new; a pure glide is a bare wake. */
    struct gui_window* new_focus = focused_win;
    int resizing   = (drag == DRAG_RESIZE);
    int structural = (pressed || released) && !resizing && !force_full;
    int pop_x = popup.x, pop_y = popup.y, pop_w = popup.w, pop_h = popup.h;
    spin_unlock(&state_lock);

    /* Outside the lock: gui_damage takes damage_lock, and nesting it inside
     * state_lock is the one ordering this file does not allow. */
    if (popup_hover_moved) gui_damage(pop_x, pop_y, pop_w, pop_h);

    if (resizing || force_full) {
        gui_damage_all();                       /* rubber band / geometry apply */
    } else if (structural) {
        int hit = 0;
        if (old_focus && old_focus->used) { gui_damage_win(old_focus); hit = 1; }
        if (new_focus && new_focus != old_focus && new_focus->used) {
            gui_damage_win(new_focus); hit = 1;
        }
        if (clicked && clicked != old_focus && clicked != new_focus &&
            clicked->used) { gui_damage_win(clicked); hit = 1; }
        panel_gen++;                            /* taskbar buttons may change */
        if (!hit) need_frame = 1;               /* click on empty desktop */
    } else if (drag_moved) {
        /* NO damage rects for the move itself — the hint carries it, and
         * compose decides whether it can copy the image instead of repainting
         * it.  Damaging here as well would defeat the "nothing else changed"
         * test the fast path is built on.  What is still needed is a FRAME:
         * need_frame wakes the compositor without claiming any area. */
        need_frame = 1;
    } else {
        need_frame = 1;                         /* cursor glide only */
    }
}

/* M22.3 — Alt-Tab.  Raw keycode hook, runs in the keyboard IRQ before
 * keymap translation.  Rotate the top window to the bottom, then
 * activate the new top visible window — repeated presses cycle.
 *
 * M22.5 — the same hook also feeds the widget layer: navigation /
 * editing keys (arrows, Home/End, Delete, PgUp/PgDn, Insert) and
 * Ctrl+letter shortcuts are consumed here and queued as raw keycode
 * events whenever the focused window is an APP window.  TERMINAL
 * windows are untouched (their shells are char-based; unmapped
 * keycodes keep dying in keymap_translate as before). */
static int kc_is_nav(uint8_t kc) {
    return kc >= KC_INSERT && kc <= KC_UP;      /* 0x49..0x52 block */
}

static int gui_raw_key(uint8_t keycode, uint8_t mods) {
    if (!gui_active) return 0;

    /* §M65 — ESCAPE CLOSES AN OPEN POPUP, and consumes the key.  A menu you
     * can only dismiss with the mouse is a menu that traps a keyboard user;
     * the owner still hears about it (row -1) so a combo can put its old value
     * back rather than leaving the control in a half-open state. */
    if (popup.active && keycode == KC_ESC) {
        struct gui_window* ow = popup.owner;
        int tag = popup.tag;
        popup.active = 0;
        popup.hover = -1;
        if (ow && ow->used) {
            struct app_event e = {0};
            e.type = AE_POPUP;
            e.x = -1;
            e.y = (int16_t)tag;
            aq_push(ow, e);
        }
        gui_damage_all();
        return 1;
    }

    /* §M58/§M59 — COPY AND PASTE IN A TERMINAL WINDOW, from the keyboard.
     *
     * Reported from use: *"I can't manage with the clipboard, the selection
     * doesn't work either."*  Both worked in the automated test — and that test
     * pasted with the MIDDLE BUTTON, which a trackpad does not have.  A feature
     * whose only trigger is a button the user's hardware lacks is, from where
     * they sit, a feature that does not exist.
     *
     * So the keyboard route, which is what people reach for anyway:
     *   Ctrl+Shift+C / Ctrl+Insert — copy the selection to the clipboard
     *   Ctrl+Shift+V / Shift+Insert — paste the clipboard into the terminal
     *
     * SHIFT is what keeps Ctrl+C free to remain the interrupt: a shell's Ctrl+C
     * must not become "copy" just because something happens to be selected —
     * that would make the most important key on a terminal depend on invisible
     * state.  (The same reason every terminal emulator picked this binding.) */
    {
        struct gui_window* tw = focused_win;
        if (tw && tw->used && tw->kind == WIN_TERM && tw->cells) {
            int ctrl  = (mods & KBD_MOD_CTRL_MASK) != 0;
            int shift = (mods & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
            int copy  = (ctrl && shift && keycode == KC_C) ||
                        (ctrl && keycode == KC_INSERT);
            int paste = (ctrl && shift && keycode == KC_V) ||
                        (shift && keycode == KC_INSERT);
            if (copy)  { term_sel_copy_to_clip = tw; need_frame = 1; return 1; }
            if (paste) { term_paste_win = tw;        need_frame = 1; return 1; }

            /* §M58 — SHIFT+PgUp/PgDn walks the scrollback a page at a time.
             * Shift is load-bearing for the same reason it is on copy: plain
             * PgUp/PgDn belong to whatever is running IN the terminal (an
             * editor, a pager), and stealing them would break those programs
             * in a way the user cannot see or turn off. */
            if (shift && (keycode == KC_PGUP || keycode == KC_PGDN)) {
                int page = tw->rows > 2 ? tw->rows - 2 : 1;
                if (gterm_view_scroll(tw, keycode == KC_PGUP ? page : -page))
                    term_sel_dirty = tw;
                need_frame = 1;
                return 1;
            }

            /* Anything else TYPED means the user is done reading history: snap
             * back to the live bottom, exactly as every terminal does — output
             * appearing somewhere the user cannot see is how a shell looks
             * broken. */
            /* (Bare modifiers never reach here — the PS/2 driver consumes
             * shift/ctrl/alt make+break codes before translation — so this
             * needs no exception for them.) */
            if (tw->view_off > 0) {
                tw->view_off = 0;
                term_sel_dirty = tw;
                need_frame = 1;
            }
        }
    }

    if (keycode != KC_TAB || !(mods & KBD_MOD_LALT)) {
        /* Widget-bound keycodes?  focused_win is an atomic pointer
         * read; kind/used are stable for live windows. */
        struct gui_window* win = focused_win;
        /* §M40 — widget windows only want the keycodes their widgets act on
         * (nav + Ctrl-letter); a window forwarding to a CLIENT wants EVERY key,
         * because `wl_keyboard.key` carries raw keycodes and the application
         * does its own interpretation. */
        if (win && win->used && win->kind == WIN_APP &&
            (win->input_hook || kc_is_nav(keycode) ||
             ((mods & KBD_MOD_CTRL_MASK) &&
              keycode >= KC_A && keycode <= KC_Z))) {
            uint32_t n = (kcq_h + 1) % KCQ_SZ;
            if (n != kcq_t) {
                kcq[kcq_h] = (uint16_t)(keycode | ((uint16_t)mods << 8));
                kcq_h = n;
            }
            need_frame = 1;
            /* A client window takes the raw keycode AND must still let the
             * keymap run: returning 1 here consumed the key outright, so the
             * cooked character was never produced and a client with no keymap
             * of its own (NetSurf) could not receive letters at all.  Report
             * "not consumed" for those, so ps2_keyboard goes on to translate
             * and the character arrives as a second event. */
            return win->input_hook ? 0 : 1;
        }
        return 0;
    }
    spin_lock(&state_lock);
    if (zcount >= 2) {
        /* Demote the currently ACTIVE (top visible) window to the
         * bottom, then activate the next visible one — repeated
         * presses walk the whole visible set.  Rotating the raw top
         * would stall on minimized windows parked at the top of the
         * z-order. */
        struct gui_window* cur = top_visible_locked();
        if (cur) {
            int i;
            for (i = 0; i < zcount && zorder[i] != cur; i++) ;
            for (; i > 0; i--) zorder[i] = zorder[i - 1];
            zorder[0] = cur;
            struct gui_window* nf = top_visible_locked();
            if (nf) gui_wm_focus_raise_locked(nf);
        }
    }
    spin_unlock(&state_lock);
    gui_damage_all();
    return 1;
}

static int gui_kbd_hook(char c) {
    if (!gui_active) return 0;
    struct gui_window* win = focused_win;
    if (!win || win->kind != WIN_APP) return 0;
    uint32_t n = (keyq_h + 1) % KEYQ_SZ;
    if (n != keyq_t) {
        keyq[keyq_h] = c;
        keyq_h = n;
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Window creation + bring-up.                                                 */
/* -------------------------------------------------------------------------- */

static struct gui_window* window_alloc(const char* title, enum win_kind kind,
                                       int x, int y, int w, int h) {
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    if (h > work_h) h = work_h;

    /* M22.7 — the slot scan + claim runs under state_lock: app-host tasks
     * now create windows concurrently, so an unlocked "find !used then set
     * used=1" would hand the same slot to two apps.  All fields are set
     * before used=1 (the last store), so a compositor pass that observes
     * used==1 sees a fully-initialised window (x86 TSO — no barrier). */
    uint32_t fl = spin_lock_irqsave(&state_lock);
    struct gui_window* win = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (!windows[i].used) { win = &windows[i]; break; }
    if (!win) {
        spin_unlock_irqrestore(&state_lock, fl);
        kprintf("gui: window pool exhausted\n");
        return NULL;
    }

    win->kind = kind;
    win->x = x;  win->y = y;  win->w = w;  win->h = h;
    win->pending_w = win->pending_h = 0;
    win->want_close = 0;
    win->close_deadline_ms = 0;
    win->close_force_now = 0;
    win->widgets = NULL;  win->focusw = NULL;
    win->on_layout = NULL; win->on_close = NULL; win->app_ctx = NULL;
    win->ui_state = NULL;
    win->cells = NULL; win->vc = NULL;
    win->sb = NULL; win->sb_cap = win->sb_count = win->sb_head = 0;
    win->scrolled = win->view_off = 0;
    win->minimized = 0; win->on_tick = NULL;
    win->maximized = 0;
    win->sav_x = win->sav_y = win->sav_w = win->sav_h = 0;
    win->ccol = win->crow = win->cols = win->rows = 0;
    win->surf.px = NULL; win->surf.owns_px = 0;
    /* M22.7 — per-task app fields. */
    win->host_task = NULL;
    win->client_pid = 0;                /* §M46 — clear stale client on reuse */
    win->on_dispose = NULL;             /* §M54 — never inherit a dead owner   */
    win->dispose_ctx = NULL;
    win->input_hook = NULL;             /* dosgui/wayland re-arm per window     */
    win->input_ctx  = NULL;
    win->aq_h = win->aq_t = 0;
    win->tick_pending = win->layout_pending = win->host_released = 0;
    spin_lock_init(&win->lock);
    str_copy(win->title, title, (int)sizeof(win->title));
    win->used = 1;
    spin_unlock_irqrestore(&state_lock, fl);
    return win;
}

static void window_show(struct gui_window* win) {
    uint32_t fl = spin_lock_irqsave(&state_lock);
    zorder[zcount++] = win;
    focused_win = win;
    spin_unlock_irqrestore(&state_lock, fl);
    if (win->kind == WIN_TERM) vc_focus(win->vc);
    gui_damage_all();
}

/* Shared body of gui_window_create / gui_window_create_task: a
 * terminal window whose hosted task is the caller's choice.  The task
 * gets the window's offscreen VC as its output console and is owned by
 * the window (vc->task — the close path kills + reaps it). */
static struct gui_window* term_window_create(const char* title,
                                             int x, int y, int w, int h,
                                             const char* task_name,
                                             void (*entry)(void),
                                             int shell_ppid) {
    struct gui_window* win = window_alloc(title, WIN_TERM, x, y, w, h);
    if (!win) return NULL;

    win->cells = (char*)kcalloc(1, (size_t)gmax_cols * gmax_rows);
    if (!win->cells) { win->used = 0; return NULL; }

    /* §M58 — scrollback.  Sized from config, in LINES, because that is the unit
     * the user thinks in; the bytes follow from the screen width.  A failed
     * allocation is not fatal: sb_cap stays 0 and the terminal behaves exactly
     * as it did before scrollback existed — a window that refuses to open
     * because it could not get its history would be a worse trade. */
    {
        long want = config_get_long("gui.scrollback", 500);
        if (want < 0)    want = 0;
        if (want > 5000) want = 5000;
        if (want > 0) {
            win->sb = (char*)kcalloc(1, (size_t)gmax_cols * (size_t)want);
            win->sb_cap = win->sb ? (int)want : 0;
        }
        win->sb_count = win->sb_head = 0;
        win->scrolled = win->view_off = 0;
    }

    if (window_set_size(win, win->w, win->h) != 0) {
        kfree(win->cells);
        win->used = 0;
        return NULL;
    }

    win->vc = vc_create_offscreen(gterm_emit, win);
    if (!win->vc) {
        gfx_surface_free(&win->surf);
        kfree(win->cells);
        win->used = 0;
        return NULL;
    }

    /* M22.7 — parent the shell as requested: the desktop/session (session
     * shells, so a kill_tree(desktop) takes them with it), init (detached
     * shells, which outlive the session), or the caller (< 0).  Without this
     * a shell launched from the taskbar orphaned to init when its transient
     * launcher app-host exited.
     * §M49 — the window's VC is bound by the spawn; setting it afterwards
     * raced the task's own start on another core (preempt_disable is
     * per-CPU and does not hold that off). */
    struct task* t = task_spawn_console(task_name, entry, shell_ppid, win->vc);
    if (t) {
        win->vc->task = t;
        /* M27 — this window owns its shell's reap (the close teardown
         * kills + reaps it and nulls the pointer).  Tell init's universal
         * reaper to keep its hands off, so the two never race for the
         * same struct. */
        task_set_reap_owned(t, 1);
    }
    if (!t) kprintf("gui: task spawn failed for '%s'\n", win->title);

    window_show(win);
    return win;
}

struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h) {
    /* S.1: terminal windows spawn the ACTIVE shell provider.
     * M22.7 — SESSION mode: parent the shell to the desktop (once it exists;
     * the initial two shells are created before it and stay under whoever
     * ran `gui`).  A kill_tree(desktop) then takes session shells with it. */
    return term_window_create(title, x, y, w, h, "shell",
                              shell_provider_active()->entry,
                              desktop_pid > 0 ? desktop_pid : -1);
}

/* M22.7 — DETACHED mode: the shell is parented to init, so it OUTLIVES the
 * desktop session (a kill_tree(desktop) does not reach it).  Its window
 * stays composited as long as the compositor runs — a "detached terminal". */
struct gui_window* gui_window_create_detached(const char* title,
                                              int x, int y, int w, int h) {
    return term_window_create(title, x, y, w, h, "shell",
                              shell_provider_active()->entry,
                              task_reaper_pid());
}

struct gui_window* gui_window_create_task(const char* title, int x, int y,
                                          int w, int h,
                                          const char* task_name,
                                          void (*entry)(void)) {
    /* Custom-task terminals (e.g. BASIC) — parent to the desktop session too. */
    return term_window_create(title, x, y, w, h, task_name, entry,
                              desktop_pid > 0 ? desktop_pid : -1);
}

struct gui_window* gui_app_window_create(const char* title, int x, int y,
                                         int w, int h,
                                         void (*on_layout)(struct gui_window*),
                                         void* app_ctx) {
    struct gui_window* win = window_alloc(title, WIN_APP, x, y, w, h);
    if (!win) return NULL;
    win->on_layout = on_layout;
    win->app_ctx   = app_ctx;
    /* M22.7 — bind the window to the app-host that is creating it (this runs
     * inside the app's open fn, which the host task invokes).  The host's
     * loop then owns this window's events + rendering + teardown. */
    win->host_task = task_current();

    if (window_set_size(win, win->w, win->h) != 0) {
        win->used = 0;
        return NULL;
    }
    window_show(win);
    return win;
}

int gui_is_active(void) { return gui_active; }

/* §M42 — the desktop/session task pid (0 until gui_start spawns it).  A GUI app
 * launched from the taskbar should parent under this so it shows up under the
 * desktop session in the process tree (and dies with the session), rather than
 * being an init-owned detached task. */
int gui_desktop_pid(void) { return desktop_pid; }

/* Pick the desktop shell: `gui.shell` config value, matched against the
 * registry; falls back to "vista", then to the first registration. */
static const struct desktop_shell* pick_shell(void) {
    int n = (int)(__stop_desktop_shells - __start_desktop_shells);
    if (n == 0) return NULL;

    const char* want = config_get("gui.shell", "vista");
    for (int pass = 0; pass < 2; pass++) {
        const char* name = pass == 0 ? want : "vista";
        for (int i = 0; i < n; i++) {
            const char* a = __start_desktop_shells[i].name;
            const char* b = name;
            while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
            if (*a == 0 && *b == 0) return &__start_desktop_shells[i];
        }
    }
    return &__start_desktop_shells[0];
}

/* §M60 — fill the wallpaper surface from the configured source and stamp the
 * milestone label on top.  The label is drawn HERE rather than by wallpaper.c
 * on purpose: it is desktop chrome that must survive every source (a picture
 * must not swallow the version string), and its position depends on `work_h`,
 * which is the compositor's business and not the background's. */
static int paint_wallpaper(void) {
    int rc = wallpaper_render(&wallsurf);

    /* Desktop milestone label — sizes itself to the string so any DOS_MILESTONE
     * length stays right-aligned (see kernel/includes/version.h). */
    int lbl_w = 0; for (const char* p = DOS_LABEL; *p; p++) lbl_w++;
    int lx = wallsurf.w - lbl_w * GFX_GLYPH_W - 12;
    int ly = work_h - GFX_GLYPH_H - 8;
    /* A photograph can be any colour under the text, so give the label its own
     * dim backing rather than trusting contrast that the gradient guaranteed
     * and an arbitrary image does not. */
    gfx_blend_fill(&wallsurf, lx - 6, ly - 4,
                   lbl_w * GFX_GLYPH_W + 12, GFX_GLYPH_H + 8, 0x80101820u);
    gfx_text(&wallsurf, lx, ly, DOS_LABEL, 0xFF9FB6C9u);
    return rc;
}

void gui_window_set_key_hook(struct gui_window* win,
                             void (*fn)(struct gui_window*, char)) {
    if (win) win->key_hook = fn;
}

void gui_desktop_icons_changed(void) {
    if (!gui_active) return;
    /* The icon field is the whole work area (screen minus the taskbar).  A
     * finer rect would need the shell's layout, and this runs on an add or a
     * delete — rare, human-paced events, not the per-click path that §4.61's
     * damage discipline is about. */
    gui_damage(0, 0, fbsurf.w, work_h);
}

int gui_wallpaper_reload(void) {
    if (!gui_active) {
        /* Not running: the config key is set and boot will read it.  Report
         * what a render WOULD do without pretending we painted anything. */
        return 0;
    }
    int rc = paint_wallpaper();
    gui_damage_all();
    return rc;
}

/* ==========================================================================
 * §M61 — CHANGING THE RESOLUTION WHILE THE DESKTOP RUNS.
 *
 * The mode set itself is one call into the display backend.  The WORK is
 * everything above it: the backbuffer, the wallpaper and the panel are all
 * sized from the old screen, the shell's chrome layout was computed once, and
 * every window's position may now be off-screen.
 *
 * It runs on the COMPOSITOR TASK, between frames.  A mode set while compose()
 * is mid-blit writes into a buffer that is about to be freed, so the request is
 * queued and applied here — the same shape as every other structural change in
 * this file (apply_pending).
 * ========================================================================== */

static volatile int mode_req_w = 0, mode_req_h = 0;
/* §M61 — told AFTER the new mode is live, on the compositor task.  The confirm
 * dialog has to be created here and not by the requester: it must be centred on
 * the NEW screen (the requester still sees the old size, because the change is
 * queued) and it must be built on the task that owns the window machinery. */
static void (*mode_applied_cb)(int w, int h) = NULL;

/* Geometry saved before a mode change, so a REVERT restores the desktop and
 * not merely the resolution: windows clamped into a small screen must not stay
 * clamped when the big one comes back. */
struct saved_geom { int used, x, y, w, h; };
static struct saved_geom mode_saved[GUI_MAX_WINDOWS];
static int  mode_prev_w = 0, mode_prev_h = 0;
static int  mode_pending_confirm = 0;

void gui_set_mode_applied_cb(void (*fn)(int w, int h)) { mode_applied_cb = fn; }

int gui_request_mode(int w, int h) {
    if (!gui_active) return -1;
    if (w < 320 || h < 200) return -2;
    mode_req_w = w; mode_req_h = h;
    need_frame = 1;
    return 0;
}

int gui_current_mode(int* w, int* h) {
    if (!gui_active) return -1;
    if (w) *w = fbsurf.w;
    if (h) *h = fbsurf.h;
    return 0;
}

/* Re-establish every screen-sized thing after the display changed size. */
static int mode_rebuild_surfaces(void) {
    struct gfx_surface newfb;
    if (gfx_fb_surface(&newfb) != 0) return -1;

    /* Allocate the new buffers BEFORE freeing the old ones: an OOM must leave a
     * working desktop, not a compositor with no backbuffer. */
    struct gfx_surface nback, nwall;
    if (gfx_surface_init(&nback, newfb.w, newfb.h) != 0) return -2;
    if (gfx_surface_init(&nwall, newfb.w, newfb.h) != 0) {
        gfx_surface_free(&nback);
        return -3;
    }

    gfx_surface_free(&backsurf);
    gfx_surface_free(&wallsurf);
    fbsurf   = newfb;
    backsurf = nback;
    wallsurf = nwall;
    flip_ok  = 0;                       /* the flip belonged to the old size */

    /* The page flip's second buffer is derived from the geometry, so it has to
     * be re-established — and if it cannot be, the single-buffer path is still
     * correct (it only shears). */
    {
        volatile uint32_t* b0; volatile uint32_t* b1;
        if (fb_flip_init(&b0, &b1) == 0) {
            for (int i = 0; i < 2; i++) { flipbuf[i] = fbsurf; flipbuf[i].owns_px = 0; }
            flipbuf[0].px = (uint32_t*)(uintptr_t)b0;
            flipbuf[1].px = (uint32_t*)(uintptr_t)b1;
            flip_front = 0;
            flip_ok = 1;
        }
    }

    /* Chrome: the shell recomputes its layout from the new size. */
    if (shell && shell->init) shell->init(fbsurf.w, fbsurf.h);
    work_h = fbsurf.h -
             ((shell && shell->bottom_reserve) ? shell->bottom_reserve() : 0);
    gmax_cols = fbsurf.w / GFX_GLYPH_W;
    gmax_rows = fbsurf.h / GFX_GLYPH_H;

    /* The panel strip is screen-addressed and screen-wide. */
    {
        int reserve  = fbsurf.h - work_h;
        int strip_h  = reserve + PANEL_POPUP_MAX;
        if (strip_h > fbsurf.h) strip_h = fbsurf.h;
        uint32_t* nbuf = (uint32_t*)kmalloc((size_t)fbsurf.w * strip_h * 4);
        if (nbuf) {
            spin_lock(&panel_lock);
            uint32_t* old = panel_buf;
            panel_buf = nbuf;
            panel_strip_top = fbsurf.h - strip_h;
            panelsurf.w = fbsurf.w;
            panelsurf.h = fbsurf.h;
            panelsurf.stride = fbsurf.w;
            panelsurf.px = panel_buf - (size_t)panel_strip_top * fbsurf.w;
            panelsurf.owns_px = 0;
            gfx_set_clip(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h);
            gfx_fill(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h, COL_WALL_BOT);
            panel_ready = 1;
            spin_unlock(&panel_lock);
            if (old) kfree(old);
        }
    }

    paint_wallpaper();
    return 0;
}

/* Clamp every window into the new screen.  A window at x=1700 on a 1024-wide
 * display is unreachable — and unreachable is indistinguishable from lost. */
static void mode_clamp_windows(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* w = &windows[i];
        if (!w->used) continue;
        if (w->w > fbsurf.w) w->w = fbsurf.w;
        if (w->h > work_h)   w->h = work_h;
        if (w->x + w->w > fbsurf.w) w->x = fbsurf.w - w->w;
        if (w->y + w->h > work_h)   w->y = work_h - w->h;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        /* A client-managed window must be TOLD, or it keeps painting at the old
         * size — §4.60 built exactly this notification for the resize grip, and
         * a mode change is the same event from a different cause. */
        if (w->kind == WIN_APP && w->client_pid) {
            w->pending_w = w->w;
            w->pending_h = w->h;
        }
    }
}

static void apply_mode_change(void) {
    int rw = mode_req_w, rh = mode_req_h;
    if (!rw || !rh) return;
    mode_req_w = mode_req_h = 0;

    int prev_w = fbsurf.w, prev_h = fbsurf.h;
    if (fb_mode_set((uint32_t)rw, (uint32_t)rh, 32) != 0) {
        kprintf("gui: display refused %dx%d - unchanged\n", rw, rh);
        return;
    }

    /* Save the geometry BEFORE clamping, so a revert restores the desktop and
     * not just the resolution.
     *
     * The guard is "a confirm is pending AND nothing is saved yet".  It read
     * `!mode_pending_confirm` at first — the exact inverse — so the one case
     * that needs the snapshot (a provisional change, about to be confirmed or
     * reverted) was the one case that never took it, `mode_prev_w` stayed 0 and
     * `gui_mode_revert` returned immediately.  The dialog counted down, said
     * the right things, and reverted nothing. */
    if (mode_pending_confirm && !mode_prev_w) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            mode_saved[i].used = windows[i].used;
            mode_saved[i].x = windows[i].x; mode_saved[i].y = windows[i].y;
            mode_saved[i].w = windows[i].w; mode_saved[i].h = windows[i].h;
        }
        mode_prev_w = prev_w; mode_prev_h = prev_h;
    }

    if (mode_rebuild_surfaces() != 0) {
        kprintf("gui: out of memory resizing to %dx%d - reverting\n", rw, rh);
        fb_mode_set((uint32_t)prev_w, (uint32_t)prev_h, 32);
        mode_rebuild_surfaces();
        return;
    }
    mode_clamp_windows();
    gui_damage_all();
    kprintf("gui: mode %dx%d\n", fbsurf.w, fbsurf.h);
    if (mode_pending_confirm && mode_applied_cb)
        mode_applied_cb(fbsurf.w, fbsurf.h);
}

/* Restore the mode + window geometry saved before the last change.
 *
 * QUEUED, for the same reason the change itself is: it reallocates the
 * backbuffer, and doing that from the dialog's app-host task while the
 * compositor is mid-compose frees the buffer out from under it. */
static volatile int mode_revert_req = 0;

void gui_mode_revert(void) {
    if (!gui_active || !mode_prev_w) return;
    mode_revert_req = 1;
    need_frame = 1;
}

static void apply_mode_revert(void) {
    if (!mode_revert_req) return;
    mode_revert_req = 0;
    if (!mode_prev_w) return;
    if (fb_mode_set((uint32_t)mode_prev_w, (uint32_t)mode_prev_h, 32) != 0) return;
    mode_rebuild_surfaces();
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!mode_saved[i].used || !windows[i].used) continue;
        windows[i].x = mode_saved[i].x; windows[i].y = mode_saved[i].y;
        windows[i].w = mode_saved[i].w; windows[i].h = mode_saved[i].h;
        if (windows[i].kind == WIN_APP && windows[i].client_pid) {
            windows[i].pending_w = windows[i].w;
            windows[i].pending_h = windows[i].h;
        }
    }
    mode_pending_confirm = 0;
    mode_prev_w = mode_prev_h = 0;
    gui_damage_all();
    kprintf("gui: reverted to %dx%d\n", fbsurf.w, fbsurf.h);
}

void gui_mode_confirm(void) { mode_pending_confirm = 0; mode_prev_w = mode_prev_h = 0; }
void gui_mode_arm_confirm(void) { mode_pending_confirm = 1; }

/* ==========================================================================
 * gui_stop — end the session and hand the screen back to the text console.
 *
 * This is the exact inverse of gui_start, and the ORDER is the whole design:
 * every step below undoes something that a still-running compositor would
 * otherwise be using.
 *
 * It runs on a task of ITS OWN (`gui-stop`, detached → parented to init) for
 * one structural reason: the teardown kills the desktop session, and the
 * compositor — where a Start-menu click is dispatched — lives inside that
 * session.  A task cannot free the surfaces it is still composing from, and it
 * certainly cannot outlive its own kill_tree to do the tidying afterwards.
 * ========================================================================== */

static int gui_teardown(void) {
    if (!gui_active) return -1;

    /* 1. INPUT FIRST.  An event delivered into a compositor that is being torn
     *    down is the classic teardown crash: the queues it drains, the windows
     *    it routes to and the surfaces it draws into are all about to go away,
     *    and the mouse IRQ does not know that. */
    mouse_set_listener(NULL);
    mouse_set_wheel_listener(NULL);
    vc_set_kbd_hook(NULL);
    vc_set_raw_kbd_hook(NULL);
    task_set_change_hook(NULL);

    /* 2. Hand every app-host's REAP back to init before its owner dies.  The
     *    compositor claims the reap of the hosts it spawns (window-teardown
     *    ordering, apply_pending); with the compositor gone, a host still
     *    marked reap_owned would be a corpse nobody is allowed to collect —
     *    §M27's universal reaper deliberately skips owned tasks. */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].host_task)
            task_set_reap_owned(windows[i].host_task, 0);

    /* 3. Kill the session.  The desktop is the session root (gui_start), so one
     *    kill_tree takes the compositor, the app-hosts and every terminal with
     *    it — the same "parent dies → children die" rule the GUI is built on. */
    int dp = desktop_pid;
    if (dp > 0) task_kill_tree(dp);

    /* 4. WAIT for the session to actually be gone.  Freeing a surface while the
     *    compositor is mid-compose is a use-after-free of several megabytes,
     *    and "we asked it to die" is not the same statement as "it is dead".
     *    Poll for the task's DISAPPEARANCE rather than task_wait()ing on it:
     *    init is a universal reaper and may collect it first, and waiting on a
     *    child somebody else reaped never completes (§M57). */
    if (dp > 0) {
        for (int i = 0; i < 400 && task_find(dp); i++) task_msleep(5);
        if (task_find(dp))
            klog(KLOG_WARN, "gui", "desktop pid %d outlived the teardown "
                                   "deadline — freeing anyway\n", dp);
    }

    /* 5. Windows.  Their hosts are dead, so nothing will run on_close on its
     *    own task any more; destroy_window also fires the dispose callback
     *    that tells a dosgui client's bridge its window is gone (§M54). */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used) destroy_window(&windows[i]);
    zcount = 0;
    focused_win = NULL;
    drag = DRAG_NONE;
    drag_win = NULL;

    /* 6. The screen buffers. */
    gfx_surface_free(&backsurf);
    gfx_surface_free(&wallsurf);
    if (panel_buf) { kfree(panel_buf); panel_buf = NULL; }
    panel_ready = 0;
    panelsurf.px = NULL;

    /* 7. Put the SCANOUT back on buffer 0.  The console writes into the base
     *    framebuffer; if the page flip left the display panned to the second
     *    buffer, every restored line would be written to memory nobody is
     *    looking at — a black screen produced by a working console. */
    if (flip_ok) fb_flip_to(0);
    flip_ok = 0;
    flip_front = 0;

    gui_active   = 0;
    desktop_pid  = 0;
    exit_req     = 0;
    need_frame   = 0;

    /* 8. Give the screen back, and put something on it.  A leaf VC has no cell
     *    backing store — output produced while the GUI owned the screen was
     *    DROPPED, not buffered — so there is nothing to restore, only a clean
     *    slate to draw. */
    vc_screen_suppress(0);
    struct vc* root = vc_root();
    if (root) {
        vc_clear(root);
        vc_focus(root);
    }
    kprintf("gui: session ended - back at the text console\n");

    /* The shell is blocked reading a LINE; it prints its prompt after it gets
     * one.  Feed it an empty line so a prompt appears immediately instead of
     * the user having to press Enter at an apparently dead screen. */
    vc_kbd_push('\n');
    return 0;
}

static void gui_stop_main(void) {
    gui_teardown();
    task_exit();
}

int gui_autostart(void) {
    /* The text shell is spawned FIRST by both callers and stays behind the
     * desktop: the GUI only suppresses the console, so Start → Exit GUI (or
     * `gui stop`) lands on a shell that has been running all along. */
    const char* ga = config_get("gui.autostart", "1");
    if (!ga || !(ga[0] == '1' || ga[0] == 'y' || ga[0] == 't' ||
                 ga[0] == 'Y' || ga[0] == 'T')) return 0;
    if (gui_start() != 0) {
        kprintf("gui: autostart failed - staying on the text console\n");
        return -1;
    }
    return 1;
}

int gui_stop(void) {
    if (!gui_active) return -1;
    /* Called directly (the `gui stop` command, from a shell task): that task is
     * not in the session, so it may do the teardown itself. */
    return gui_teardown();
}

int gui_start(void) {
    if (gui_active) return 0;

    /* §M46 — whether the X button on a client-managed (package) window
     * force-kills a wedged client instead of only requesting a cooperative
     * close (default on: the chrome must keep working when the app is frozen).
     * A future refinement makes this a per-package policy; for now it is a
     * single global gate so it is configurable rather than hard-coded. */
    {
        const char* v = config_get("gui.close_forces_kill", "1");
        close_forces_kill = (v && (v[0]=='1'||v[0]=='y'||v[0]=='t'||v[0]=='Y'));
        /* The UNATTENDED backstop only — the primary escalation is the user's
         * second click on the X (see close_force_now).  Generous on purpose: it
         * must never pre-empt a client that is merely slow to shut down. */
        const char* g = config_get("gui.close_grace_ms", "10000");
        unsigned ms = 0;
        for (const char* c = g; c && *c >= '0' && *c <= '9'; c++)
            ms = ms * 10u + (unsigned)(*c - '0');
        if (ms) close_grace_ms = ms;
    }

    /* §M61 — a CONFIRMED resolution survives a reboot.  `gui.mode` is written
     * only by the OK button (or `mode confirm`), so a mode that was never
     * confirmed cannot come back and lock the user out at the next boot; and
     * it is applied HERE, before any surface is sized, because everything
     * below this line is derived from the screen's dimensions.
     *
     * A refusal is silent-but-logged rather than fatal: the display may be a
     * different one than the machine had when the mode was saved, and a GUI
     * that will not start because of a remembered preference is worse than one
     * that starts at the boot resolution. */
    {
        const char* m = config_get("gui.mode", "");
        int w = 0, h = 0;
        const char* p = m;
        while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
        if (*p == 'x' || *p == 'X') {
            p++;
            while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
        }
        if (w >= 320 && h >= 200) {
            if (fb_mode_set((uint32_t)w, (uint32_t)h, 32) == 0)
                kprintf("gui: mode %dx%d (from gui.mode)\n", w, h);
            else
                klog(KLOG_WARN, "gui", "gui.mode=%s refused by the display — "
                                       "using the boot mode\n", m);
        }
    }

    if (gfx_fb_surface(&fbsurf) != 0) {
        kprintf("gui: no 32-bpp framebuffer — GUI unavailable\n");
        return -1;
    }
    if (gfx_surface_init(&backsurf, fbsurf.w, fbsurf.h) != 0 ||
        gfx_surface_init(&wallsurf, fbsurf.w, fbsurf.h) != 0) {
        kprintf("gui: backbuffer OOM\n");
        return -1;
    }

    /* M22.6 — try to stand up the Bochs-VBE double buffer.  On success both
     * scanout buffers alias the same geometry as fbsurf; the first compose
     * is a full-frame damage (gui_damage_all below), so both buffers get a
     * complete paint within the first two frames — the buffer-age-2 present
     * is consistent from then on.  Any failure leaves flip_ok==0 and the
     * compositor keeps its single-buffer path. */
    {
        volatile uint32_t *b0, *b1;
        if (fb_flip_init(&b0, &b1) == 0) {
            for (int i = 0; i < 2; i++) {
                flipbuf[i] = fbsurf;                /* copy w/h/stride */
                flipbuf[i].owns_px = 0;
            }
            flipbuf[0].px = (uint32_t*)(uintptr_t)b0;
            flipbuf[1].px = (uint32_t*)(uintptr_t)b1;
            flip_front = 0;
            flip_ok = 1;
            kprintf("gui: page-flip present enabled (Bochs-VBE double buffer)\n");
        } else {
            kprintf("gui: no page flip — single-buffer present (may shear)\n");
        }
    }

    shell = pick_shell();
    if (shell && shell->init) shell->init(fbsurf.w, fbsurf.h);
    work_h = fbsurf.h -
             ((shell && shell->bottom_reserve) ? shell->bottom_reserve() : 0);

    gmax_cols = fbsurf.w / GFX_GLYPH_W;
    gmax_rows = fbsurf.h / GFX_GLYPH_H;

    /* §M60 — put the shipped default image on the filesystem before the first
     * render, so a fresh boot shows a picture rather than a gradient.  Once. */
    wallpaper_provision();
    paint_wallpaper();

    /* M22.7-B — panel surface: screen-addressed, but only the bottom strip
     * (taskbar reserve + popup headroom) is backed (see PANEL_POPUP_MAX).
     * The desktop task renders chrome into it; only the taskbar strip + open
     * popup are ever composited from it.  If it OOMs we run without a panel. */
    spin_lock_init(&panel_lock);
    {
        int reserve = fbsurf.h - work_h;                    /* bottom_reserve */
        int strip_h = reserve + PANEL_POPUP_MAX;
        if (strip_h > fbsurf.h) strip_h = fbsurf.h;
        panel_strip_top = fbsurf.h - strip_h;
        panel_buf = (uint32_t*)kmalloc((size_t)fbsurf.w * strip_h * 4);
        if (panel_buf) {
            panelsurf.w      = fbsurf.w;                    /* pretend full-screen */
            panelsurf.h      = fbsurf.h;
            panelsurf.stride = fbsurf.w;
            panelsurf.px     = panel_buf -
                               (size_t)panel_strip_top * fbsurf.w;
            panelsurf.owns_px = 0;                          /* panel_buf is the base */
            gfx_set_clip(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h);
            gfx_fill(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h,
                     COL_WALL_BOT);
            panel_ready = 1;
        } else {
            kprintf("gui: panel surface OOM — taskbar disabled\n");
        }
    }

    spin_lock_init(&state_lock);
    spin_lock_init(&damage_lock);
    mx = fbsurf.w / 2;
    my = fbsurf.h / 2;

    gui_active = 1;
    vc_screen_suppress(1);

    /* Input hooks first, so the compositor + desktop see events immediately. */
    mouse_set_listener(gui_mouse);
    vc_set_kbd_hook(gui_kbd_hook);
    vc_set_raw_kbd_hook(gui_raw_key);       /* Alt-Tab */
    mouse_set_wheel_listener(gui_wheel);    /* §M61 follow-up — scrolling */
    task_set_change_hook(gui_task_change_hook);  /* M22.4: taskman refresh */

    /* M22.7 — the GUI is its own SESSION.  The `desktop` task is the session
     * root; the compositor and the two starter shells hang UNDER it, not
     * under whatever shell happened to run `gui`.  So spawn the desktop
     * first, record its pid, and parent the rest to it — a kill_tree of the
     * desktop then cleanly closes the whole GUI session.  (The boot shell
     * remains only the launcher that started the session.) */
    if (panel_ready) {
        /* The desktop is the GUI SESSION ROOT — detached (parented to init), so
         * it survives whatever transient task ran `gui` / gui_start (the boot
         * worker, a shell): a launcher's exit must not take the whole session
         * down.  The session still tears down top-down — a kill_tree / crash of
         * the desktop takes the compositor + every app with it (they hang UNDER
         * the desktop), which is the "parent dies → children die" rule. */
        struct task* dt = task_spawn_detached("desktop", desktop_main);
        if (dt) desktop_pid = dt->pid;
        else    kprintf("gui: desktop task spawn failed — taskbar static\n");
    }

    int sess = desktop_pid > 0 ? desktop_pid : -1;   /* session parent, or caller */
    if (!task_spawn_under("compositor", gui_compositor_main, sess)) {
        kprintf("gui: FATAL — compositor spawn failed\n");
        vc_set_kbd_hook(NULL);
        vc_set_raw_kbd_hook(NULL);
        task_set_change_hook(NULL);
        mouse_set_listener(NULL);
        vc_screen_suppress(0);
        gui_active = 0;
        return -1;
    }

    /* M22.7 — no auto-started shells: the GUI comes up as a clean desktop
     * (wallpaper + taskbar).  The user opens a terminal when they want one,
     * from Start → "New Shell" (session) or "Detached Shell".  Zero windows
     * is a supported state — focus is simply NULL until one is opened. */
    gui_damage_all();

    kprintf("gui: up — %dx%d, %d windows, shell '%s', %d apps registered\n",
            fbsurf.w, fbsurf.h, zcount,
            shell ? shell->name : "none", gui_app_count());
    return 0;
}
