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
#include "vc.h"
#include "task.h"
#include "lock.h"
#include "mouse.h"
#include "timer.h"
#include "config.h"
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

/* -------------------------------------------------------------------------- */
/* Window object.                                                              */
/* -------------------------------------------------------------------------- */

enum win_kind { WIN_TERM, WIN_APP };

/* M22.7 — per-window input event (compositor produces, the window's app-host
 * task consumes).  Widget hit-testing + dispatch happens on the host, not the
 * compositor, so a slow app handler can no longer stall the whole GUI. */
enum ae_type { AE_MOUSE, AE_KEY, AE_KEYCODE };
struct app_event {
    uint8_t type;
    int16_t x, y;                       /* AE_MOUSE: content-relative     */
    uint8_t dbl;
    char    c;                          /* AE_KEY                         */
    uint8_t kc, mods;                   /* AE_KEYCODE                     */
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

    /* WIN_APP: widgets + layout + lifetime hooks. */
    struct widget* widgets;
    struct widget* focusw;
    void (*on_layout)(struct gui_window*);
    void (*on_close) (struct gui_window*);
    void* app_ctx;

    /* M22.7 — per-task app.  Every WIN_APP window is driven by its own
     * "app-host" task: it creates the widgets, drains this window's event
     * queue, runs on_tick/on_layout, and renders into `surf` — all off the
     * compositor.  The compositor only composites `surf` (under `lock`) and
     * routes input into `aq`.  Teardown: on want_close the host frees the
     * widgets + calls on_close + sets host_released; the compositor then
     * disposes the window struct (see apply_pending / destroy_window). */
    struct task* host_task;
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

/* Double-click tracking (IRQ only). */
static uint64_t lastclick_ms = 0;
static int lastclick_x = -100, lastclick_y = -100;
static struct gui_window* lastclick_win = NULL;

/* Scene. */
static struct gfx_surface fbsurf, backsurf, wallsurf;
static int work_h = 0;                  /* screen minus shell chrome     */
static int gmax_cols = 0, gmax_rows = 0;

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
/* Published launcher-popup extent (set by the shell via gui_panel_set_popup)
 * — read by the compositor (what to composite) and input routing. */
static volatile int pnl_pop_on = 0;
static volatile int pnl_pop_x = 0, pnl_pop_y = 0, pnl_pop_w = 0, pnl_pop_h = 0;

/* Panel input queue (compositor/IRQ produces, panel task consumes). */
struct pev { uint8_t type; int16_t x, y; };
#define PEV_CLICK  0
#define PEV_MOTION 1
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

/* ---- IRQ → compositor queues (SPSC: IRQ produces, compositor consumes) ---- */

struct gev {
    struct gui_window* win;
    int16_t x, y;                       /* content-relative              */
    uint8_t dbl;
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
static volatile int power_req = 0;      /* 0 none / 1 reboot / 2 shutdown */

static void evq_push(struct gui_window* w, int cx, int cy, int dbl) {
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) return;
    evq[evq_h].win = w;
    evq[evq_h].x = (int16_t)cx;
    evq[evq_h].y = (int16_t)cy;
    evq[evq_h].dbl = (uint8_t)dbl;
    evq_h = n;
}

/* -------------------------------------------------------------------------- */
/* App registry walk helpers (gui_app.h).                                      */
/* -------------------------------------------------------------------------- */

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

void gui_queue_power(int reboot) {
    power_req = reboot ? 1 : 2;
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

static void gterm_draw_cell(struct gui_window* win, int col, int row, char c) {
    int px = PAD + col * GFX_GLYPH_W;
    int py = PAD + row * GFX_GLYPH_H;
    char s[2] = { c, 0 };
    gfx_fill(&win->surf, px, py, GFX_GLYPH_W, GFX_GLYPH_H, COL_WIN_BG);
    if (c > 0x20) gfx_text(&win->surf, px, py, s, COL_WIN_FG);
}

static void gterm_scroll(struct gui_window* win) {
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
            gterm_draw_cell(win, win->ccol, win->crow, ' ');
        }
    } else {
        win->cells[(size_t)win->crow * gmax_cols + win->ccol] = c;
        gterm_draw_cell(win, win->ccol, win->crow, c);
        if (++win->ccol >= win->cols) {
            win->ccol = 0;
            if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
        }
    }

    spin_unlock(&win->lock);
    gui_damage_win(win);
}

static void gterm_rerender_locked(struct gui_window* win) {
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    for (int r = 0; r < win->rows; r++)
        for (int c = 0; c < win->cols; c++) {
            char ch = win->cells[(size_t)r * gmax_cols + c];
            if (ch) gterm_draw_cell(win, c, r, ch);
        }
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
}

static void app_dispatch_event(struct gui_window* win, const struct app_event* e) {
    if (e->type == AE_MOUSE) {
        struct widget* w = widget_at(win->widgets, e->x, e->y);
        if (w && w->ops && w->ops->mouse)
            w->ops->mouse(w, e->x - w->x, e->y - w->y, e->dbl);
    } else if (e->type == AE_KEY) {
        struct widget* w = win->focusw;
        if (w && w->ops && w->ops->key) w->ops->key(w, e->c);
    } else if (e->type == AE_KEYCODE) {
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

void gui_window_set_tick(struct gui_window* win,
                         void (*fn)(struct gui_window*)) {
    if (win) win->on_tick = fn;
}

int gui_window_minimized(struct gui_window* w) {
    return w ? w->minimized : 0;
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

static void desktop_main(void) {
    kprintf("gui: desktop shell up on pid %d (shell '%s')\n",
            task_current() ? task_current()->pid : -1, shell ? shell->name : "none");
    uint64_t last_tick = 0;
    int gen_seen = -1;
    for (;;) {
        int busy = 0;

        /* M22.7 — launches run HERE now: an app spawned from the taskbar (or
         * the `launch` command's queue) becomes a child of the desktop/
         * session, not of the display server (compositor). */
        dispatch_launches();

        while (pevq_t != pevq_h) {
            struct pev e = pevq[pevq_t];
            pevq_t = (pevq_t + 1) % PEVQ_SZ;
            uint32_t fl = spin_lock_irqsave(&state_lock);
            if (e.type == PEV_CLICK)  { if (shell && shell->click)  shell->click(e.x, e.y); }
            else                      { if (shell && shell->motion) shell->motion(e.x, e.y); }
            spin_unlock_irqrestore(&state_lock, fl);
            /* Clicks change chrome state (menu, focus) → always repaint.
             * Motion is frequent; let the shell request a repaint itself
             * (vista only does so when the hover row changes) so a mouse
             * drag across the open menu doesn't repaint the chrome on every
             * event. */
            if (e.type == PEV_CLICK) panel_dirty = 1;
            busy = 1;
        }

        uint64_t now = timer_ticks_ms();
        if (now - last_tick >= 500) {
            last_tick = now;
            if (shell && shell->second_tick && shell->second_tick()) panel_dirty = 1;
        }
        if (panel_gen != gen_seen) { gen_seen = panel_gen; panel_dirty = 1; }

        if (panel_dirty) {
            panel_dirty = 0;
            busy = 1;
            spin_lock(&panel_lock);
            if (shell && shell->draw) shell->draw(&panelsurf);
            spin_unlock(&panel_lock);
            gui_damage(0, work_h, fbsurf.w, fbsurf.h - work_h);   /* taskbar */
            if (pnl_pop_on)
                gui_damage(pnl_pop_x, pnl_pop_y, pnl_pop_w, pnl_pop_h);
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

    draw_cursor(&backsurf, s->cx, s->cy);
}

static void compose(void) {
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
    spin_unlock_irqrestore(&state_lock, fl);

    /* 3. Cursor damage from HERE (M22.4): erase the last-drawn sprite and
     *    draw the fresh one — two SEPARATE small rects, appended to the
     *    list (not unioned with far-away window damage). */
    int cur_moved = (s.cx != last_cur_x || s.cy != last_cur_y);
    if (rn == 0 && !cur_moved) return;          /* spurious wake */
    if (cur_moved && rn < DMG_MAX + 2)
        rl[rn++] = (struct rect){ CUR_DMG_X(last_cur_x), CUR_DMG_Y(last_cur_y),
                                  CUR_DMG_X(last_cur_x) + CUR_DMG_W,
                                  CUR_DMG_Y(last_cur_y) + CUR_DMG_H };
    if (cur_moved && rn < DMG_MAX + 2)
        rl[rn++] = (struct rect){ CUR_DMG_X(s.cx), CUR_DMG_Y(s.cy),
                                  CUR_DMG_X(s.cx) + CUR_DMG_W,
                                  CUR_DMG_Y(s.cy) + CUR_DMG_H };
    last_cur_x = s.cx;  last_cur_y = s.cy;

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
    if (fn == 0) return;
    if (any_full) frames_full++; else frames_partial++;
    for (int k = 0; k < fn; k++)                 /* actual damage this frame */
        total_blit_px += (uint64_t)(fr[k].x1 - fr[k].x0) * (fr[k].y1 - fr[k].y0);

    /* 5. Draw pass — paint each rect into backsurf. */
    for (int k = 0; k < fn; k++)
        draw_scene_rect(&s, fr[k].x0, fr[k].y0, fr[k].x1, fr[k].y1);
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
}

/* -------------------------------------------------------------------------- */
/* Window teardown (compositor task only).                                     */
/* -------------------------------------------------------------------------- */

static void destroy_window(struct gui_window* win) {
    /* M22.7 — a released WIN_APP already ran on_close + freed its widgets on
     * its host task; don't repeat it here.  WIN_TERM keeps the old path. */
    if (win->on_close && !win->host_released) win->on_close(win);

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
    if (win->app_ctx) { kfree(win->app_ctx); win->app_ctx = NULL; }
    win->used = 0;
    gui_damage_all();
}

/* -------------------------------------------------------------------------- */
/* Queue dispatch — runs on the compositor task.                               */
/* -------------------------------------------------------------------------- */

static void dispatch_launches(void) {
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
        struct app_event ae = { .type = AE_MOUSE, .x = e.x, .y = e.y,
                                .dbl = e.dbl };
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
        apply_pending();

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

static void gui_mouse(int dx, int dy, unsigned buttons) {
    /* M22.4 — per-motion drag damage bookkeeping (filled under the
     * lock, consumed after unlock so gui_damage isn't nested deeper
     * than it has to be). */
    int drag_moved = 0;
    int drag_old_x = 0, drag_old_y = 0, drag_old_w = 0, drag_old_h = 0;
    int drag_new_x = 0, drag_new_y = 0;
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

    /* M22.7-B — chrome hover (launcher highlight): only meaningful while the
     * popup is open; route it to the desktop task instead of running the
     * shell in the IRQ. */
    if (pnl_pop_on) pevq_push(PEV_MOTION, mx, my);

    if (pressed & MOUSE_BTN_LEFT) {
        /* Desktop chrome gets first refusal.  A click over the taskbar or
         * the open popup is consumed and handed to the desktop task; a click
         * elsewhere while the popup is open also goes there (to dismiss the
         * menu) but still falls through to the windows below. */
        if (in_panel_region(mx, my)) {
            pevq_push(PEV_CLICK, mx, my);
            goto drag_update;
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
                evq_push(win, cxr, cyr, dbl);
            }
        }
    }

drag_update:
    if (drag == DRAG_MOVE && drag_win) {
        /* M22.4 — rect-bounded drag damage: remember the old outer rect
         * so the post-unlock path can damage old ∪ new instead of the
         * whole screen.  Before this fix every motion event during a
         * drag raised gui_damage_all() — a full 1280×800 recompose +
         * ~4 MB blit per event, which made the scene "swim". */
        drag_old_x = drag_win->x;  drag_old_y = drag_win->y;
        drag_old_w = drag_win->w;  drag_old_h = drag_win->h;
        drag_win->x = mx - grab_dx;
        drag_win->y = my - grab_dy;
        if (drag_win->x < -(drag_win->w - 40)) drag_win->x = -(drag_win->w - 40);
        if (drag_win->x > fbsurf.w - 40)       drag_win->x = fbsurf.w - 40;
        if (drag_win->y < 0)                   drag_win->y = 0;
        if (drag_win->y > work_h - TITLE_H)    drag_win->y = work_h - TITLE_H;
        drag_new_x = drag_win->x;  drag_new_y = drag_win->y;
        drag_moved = (drag_new_x != drag_old_x || drag_new_y != drag_old_y);
    } else if (drag == DRAG_RESIZE && drag_win) {
        rubber_w = mx - drag_win->x + 2;
        rubber_h = my - drag_win->y + 2;
        if (rubber_w < MIN_W) rubber_w = MIN_W;
        if (rubber_h < MIN_H) rubber_h = MIN_H;
        if (rubber_w > fbsurf.w) rubber_w = fbsurf.w;
        if (rubber_h > work_h)   rubber_h = work_h;
    }

    if (released & MOUSE_BTN_LEFT) {
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
    spin_unlock(&state_lock);

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
        /* Same margins as gui_damage_win (+5 shadow, +2 safety). */
        gui_damage(drag_old_x - 2, drag_old_y - 2,
                   drag_old_w + 9, drag_old_h + 9);
        gui_damage(drag_new_x - 2, drag_new_y - 2,
                   drag_old_w + 9, drag_old_h + 9);
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
    if (keycode != KC_TAB || !(mods & KBD_MOD_LALT)) {
        /* Widget-bound keycodes?  focused_win is an atomic pointer
         * read; kind/used are stable for live windows. */
        struct gui_window* win = focused_win;
        if (win && win->used && win->kind == WIN_APP &&
            (kc_is_nav(keycode) ||
             ((mods & KBD_MOD_CTRL_MASK) &&
              keycode >= KC_A && keycode <= KC_Z))) {
            uint32_t n = (kcq_h + 1) % KCQ_SZ;
            if (n != kcq_t) {
                kcq[kcq_h] = (uint16_t)(keycode | ((uint16_t)mods << 8));
                kcq_h = n;
            }
            need_frame = 1;
            return 1;
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
    win->widgets = NULL;  win->focusw = NULL;
    win->on_layout = NULL; win->on_close = NULL; win->app_ctx = NULL;
    win->cells = NULL; win->vc = NULL;
    win->minimized = 0; win->on_tick = NULL;
    win->maximized = 0;
    win->sav_x = win->sav_y = win->sav_w = win->sav_h = 0;
    win->ccol = win->crow = win->cols = win->rows = 0;
    win->surf.px = NULL; win->surf.owns_px = 0;
    /* M22.7 — per-task app fields. */
    win->host_task = NULL;
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

    preempt_disable();
    /* M22.7 — parent the shell as requested: the desktop/session (session
     * shells, so a kill_tree(desktop) takes them with it), init (detached
     * shells, which outlive the session), or the caller (< 0).  Without this
     * a shell launched from the taskbar orphaned to init when its transient
     * launcher app-host exited. */
    struct task* t = task_spawn_under(task_name, entry, shell_ppid);
    if (t) {
        task_set_out_console(t, win->vc);
        win->vc->task = t;
        /* M27 — this window owns its shell's reap (the close teardown
         * kills + reaps it and nulls the pointer).  Tell init's universal
         * reaper to keep its hands off, so the two never race for the
         * same struct. */
        task_set_reap_owned(t, 1);
    }
    preempt_enable();
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

int gui_start(void) {
    if (gui_active) return 0;

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

    gfx_vgradient(&wallsurf, 0, 0, wallsurf.w, wallsurf.h,
                  COL_WALL_TOP, COL_WALL_BOT);
    /* Desktop milestone label — sizes itself to the string so any DOS_MILESTONE
     * length stays right-aligned (see kernel/includes/version.h). */
    int lbl_w = 0; for (const char* p = DOS_LABEL; *p; p++) lbl_w++;
    gfx_text(&wallsurf, wallsurf.w - lbl_w * GFX_GLYPH_W - 12,
             work_h - GFX_GLYPH_H - 8, DOS_LABEL, 0xFF9FB6C9u);

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
    task_set_change_hook(gui_task_change_hook);  /* M22.4: taskman refresh */

    /* M22.7 — the GUI is its own SESSION.  The `desktop` task is the session
     * root; the compositor and the two starter shells hang UNDER it, not
     * under whatever shell happened to run `gui`.  So spawn the desktop
     * first, record its pid, and parent the rest to it — a kill_tree of the
     * desktop then cleanly closes the whole GUI session.  (The boot shell
     * remains only the launcher that started the session.) */
    if (panel_ready) {
        struct task* dt = task_spawn("desktop", desktop_main);
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
