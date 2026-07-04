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
#include "widget.h"
#include "vc.h"
#include "task.h"
#include "lock.h"
#include "mouse.h"
#include "timer.h"
#include "config.h"
#include "keymap.h"          /* M22.3: Alt-Tab raw keycodes */
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

    /* M22.3 */
    int  minimized;                     /* skipped by compose + hit-test  */
    void (*on_tick)(struct gui_window*);/* APP: ~1 Hz on compositor task  */

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

static volatile int need_frame = 0;
static int gui_active = 0;

/* M22.4 — set by the task-lifecycle hook (any context) and consumed by
 * the compositor loop: run every window's on_tick NOW instead of at
 * the next 1 Hz beat, so a closed/killed program vanishes from the
 * Task Manager within one frame. */
static volatile int tasks_changed = 0;

static void gui_task_change_hook(void) {
    tasks_changed = 1;
    need_frame = 1;
}

/* M22.3 — damage tracking.  The dirty rect accumulates under
 * damage_lock (nested inside state_lock on the mouse path — never the
 * other way around) and is snapshotted + cleared by compose().  The
 * full/partial counters back the `gui stats` shell command. */
static spinlock_t damage_lock;
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;      /* empty when x1 <= x0 */
static uint32_t frames_full = 0, frames_partial = 0;

static void damage_add_locked(int x0, int y0, int x1, int y1) {
    if (dmg_x1 <= dmg_x0) {                     /* empty → replace */
        dmg_x0 = x0; dmg_y0 = y0; dmg_x1 = x1; dmg_y1 = y1;
    } else {
        if (x0 < dmg_x0) dmg_x0 = x0;
        if (y0 < dmg_y0) dmg_y0 = y0;
        if (x1 > dmg_x1) dmg_x1 = x1;
        if (y1 > dmg_y1) dmg_y1 = y1;
    }
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
    damage_add_locked(0, 0, fbsurf.w, fbsurf.h);
    spin_unlock_irqrestore(&damage_lock, fl);
    need_frame = 1;
}

/* Window rect + margin for border/shadow (+5 shadow, +2 safety). */
static void gui_damage_win(struct gui_window* w) {
    gui_damage(w->x - 2, w->y - 2, w->w + 9, w->h + 9);
}

void gui_get_stats(uint32_t* full, uint32_t* partial) {
    if (full)    *full    = frames_full;
    if (partial) *partial = frames_partial;
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

    if (win->kind == WIN_APP) {
        if (win->on_layout) win->on_layout(win);
        app_redraw(win);
    }
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

static void rect_union(int* x0, int* y0, int* x1, int* y1,
                       int ax0, int ay0, int ax1, int ay1) {
    if (*x1 <= *x0) {                           /* empty → replace */
        *x0 = ax0; *y0 = ay0; *x1 = ax1; *y1 = ay1;
        return;
    }
    if (ax0 < *x0) *x0 = ax0;
    if (ay0 < *y0) *y0 = ay0;
    if (ax1 > *x1) *x1 = ax1;
    if (ay1 > *y1) *y1 = ay1;
}

static void compose(void) {
    /* Snapshot + clear the damage rect first: anything damaged while
     * we paint lands in the NEXT frame. */
    int rx0, ry0, rx1, ry1;
    {
        uint32_t dfl = spin_lock_irqsave(&damage_lock);
        rx0 = dmg_x0; ry0 = dmg_y0; rx1 = dmg_x1; ry1 = dmg_y1;
        dmg_x0 = dmg_y0 = dmg_x1 = dmg_y1 = 0;
        spin_unlock_irqrestore(&damage_lock, dfl);
    }

    struct gui_window* zsnap[GUI_MAX_WINDOWS];
    int   zn;
    int   cx, cy;
    enum drag_mode dsnap;
    struct gui_window* dwin;
    int   rw, rh, rx = 0, ry = 0;
    struct gui_window* fsnap;
    int   wx[GUI_MAX_WINDOWS], wy[GUI_MAX_WINDOWS],
          ww[GUI_MAX_WINDOWS], wh[GUI_MAX_WINDOWS];

    uint32_t fl = spin_lock_irqsave(&state_lock);
    zn = 0;
    for (int i = 0; i < zcount; i++) {
        if (zorder[i]->minimized) continue;     /* M22.3 */
        zsnap[zn] = zorder[i];
        wx[zn] = zorder[i]->x;  wy[zn] = zorder[i]->y;
        ww[zn] = zorder[i]->w;  wh[zn] = zorder[i]->h;
        zn++;
    }
    cx = mx; cy = my;
    dsnap = drag; dwin = drag_win; rw = rubber_w; rh = rubber_h;
    if (dwin) { rx = dwin->x; ry = dwin->y; }
    fsnap = focused_win;
    spin_unlock_irqrestore(&state_lock, fl);

    /* M22.4 — cursor rects come from HERE, not from the IRQ: union the
     * previously-drawn and the just-snapshotted cursor positions so the
     * erase and the redraw are guaranteed to land in the same frame. */
    int cur_moved = (cx != last_cur_x || cy != last_cur_y);
    if (rx1 <= rx0 && !cur_moved) return;       /* spurious wake */
    if (cur_moved || rx1 > rx0) {
        rect_union(&rx0, &ry0, &rx1, &ry1,
                   CUR_DMG_X(last_cur_x), CUR_DMG_Y(last_cur_y),
                   CUR_DMG_X(last_cur_x) + CUR_DMG_W,
                   CUR_DMG_Y(last_cur_y) + CUR_DMG_H);
        rect_union(&rx0, &ry0, &rx1, &ry1,
                   CUR_DMG_X(cx), CUR_DMG_Y(cy),
                   CUR_DMG_X(cx) + CUR_DMG_W,
                   CUR_DMG_Y(cy) + CUR_DMG_H);
    }
    last_cur_x = cx;  last_cur_y = cy;

    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > fbsurf.w) rx1 = fbsurf.w;
    if (ry1 > fbsurf.h) ry1 = fbsurf.h;
    if (rx1 <= rx0 || ry1 <= ry0) return;       /* off-screen cursor twitch */
    int full_frame = (rx0 == 0 && ry0 == 0 &&
                      rx1 == fbsurf.w && ry1 == fbsurf.h);
    if (full_frame) frames_full++; else frames_partial++;

    /* Everything below is confined to the dirty rect by the clip box —
     * a partial frame only touches (and finally blits) that region. */
    gfx_set_clip(&backsurf, rx0, ry0, rx1 - rx0, ry1 - ry0);

    gfx_blit(&backsurf, 0, 0, &wallsurf, 0, 0, wallsurf.w, wallsurf.h);

    for (int i = 0; i < zn; i++) {
        struct gui_window* win = zsnap[i];
        int x = wx[i], y = wy[i], w = ww[i], h = wh[i];
        int focused = (win == fsnap);

        gfx_blend_fill(&backsurf, x + 5, y + 5, w, h, COL_SHADOW);
        draw_rect_outline(&backsurf, x, y, w, h, BORDER,
                          focused ? COL_BORDER_F : COL_BORDER_U);
        gfx_vgradient(&backsurf, x + BORDER, y + BORDER,
                      w - 2 * BORDER, TITLE_H - BORDER,
                      focused ? COL_TITLE_F_TOP : COL_TITLE_U_TOP,
                      focused ? COL_TITLE_F_BOT : COL_TITLE_U_BOT);
        gfx_text(&backsurf, x + 8, y + (TITLE_H - GFX_GLYPH_H + BORDER) / 2,
                 win->title, COL_TITLE_TEXT);

        /* M22.3: every window gets minimize (_) + close (x). */
        {
            int bx = x + w - BORDER - CLOSE_W - 3;
            int by = y + 4;
            gfx_fill(&backsurf, bx, by, CLOSE_W, CLOSE_H, COL_CLOSE_BG);
            gfx_text(&backsurf, bx + (CLOSE_W - GFX_GLYPH_W) / 2, by + 2, "x",
                     COL_CLOSE_FG);
            int mx2 = bx - CLOSE_W - 3;
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

    if (dsnap == DRAG_RESIZE && dwin)
        draw_rect_outline(&backsurf, rx, ry, rw, rh, 2, COL_RUBBER);

    /* Desktop chrome (taskbar, launcher, clock — whatever the active
     * shell wants), then the cursor on the very top. */
    if (shell && shell->draw) shell->draw(&backsurf);

    draw_cursor(&backsurf, cx, cy);
    gfx_clear_clip(&backsurf);
    /* M22.4 tearing note: QEMU's std-VGA exposes no vblank / present
     * boundary, so this blit can shear mid-scanout when it is large.
     * Not fully fixable on this device — the cursor + drag damage fixes
     * shrink the typical blit far enough that the residue sits below
     * perception.  A real fix needs a flush-capable display (virtio-gpu
     * with explicit VIRTIO_GPU_CMD_RESOURCE_FLUSH is the candidate,
     * post-M24). */
    gfx_blit(&fbsurf, rx0, ry0, &backsurf, rx0, ry0, rx1 - rx0, ry1 - ry0);
}

/* -------------------------------------------------------------------------- */
/* Window teardown (compositor task only).                                     */
/* -------------------------------------------------------------------------- */

static void destroy_window(struct gui_window* win) {
    if (win->on_close) win->on_close(win);

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
    while (w) { struct widget* nx = w->next; kfree(w); w = nx; }
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
        if (app && app->launch) app->launch();
    }
    if (power_req == 1) hal_reboot();
    if (power_req == 2) hal_shutdown();
}

static void dispatch_events(void) {
    while (evq_t != evq_h) {
        struct gev e = evq[evq_t];
        evq_t = (evq_t + 1) % EVQ_SZ;
        struct gui_window* win = e.win;
        if (!win || !win->used || win->kind != WIN_APP) continue;
        struct widget* w = widget_at(win->widgets, e.x, e.y);
        if (w && w->ops && w->ops->mouse)
            w->ops->mouse(w, e.x - w->x, e.y - w->y, e.dbl);
        if (win->used) app_redraw(win);
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
        struct widget* w = win->focusw;
        if (w && w->ops && w->ops->key) {
            w->ops->key(w, c);
            app_redraw(win);
        }
    }
}

static void apply_pending(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used) continue;

        if (win->want_close) {
            if (win->kind == WIN_TERM && win->vc && win->vc->task) {
                /* Kill the hosted shell first (cooperative — it dies at
                 * its next vc_getchar yield), then reap; retry on the
                 * next compositor pass until the reap succeeds.  Only
                 * then is it safe to free the VC and the surface. */
                struct task* t = win->vc->task;
                task_kill(t->pid);
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
        dispatch_launches();
        dispatch_events();
        dispatch_keys();
        apply_pending();

        /* ~1 Hz shell housekeeping (clock).  Rate-limited here so a
         * shell can just do the slow thing (RTC port I/O) in its tick. */
        uint64_t now = timer_ticks_ms();
        if (now - last_tick >= 500) {
            last_tick = now;
            if (shell && shell->second_tick && shell->second_tick()) {
                /* Clock repaint only dirties the chrome strip — keeps
                 * the 1 Hz tick on the cheap partial-frame path. */
                if (work_h < fbsurf.h)
                    gui_damage(0, work_h, fbsurf.w, fbsurf.h - work_h);
                else
                    gui_damage_all();
            }
            /* M22.3: per-window ~1 Hz ticks (task manager refresh). */
            for (int i = 0; i < GUI_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick)
                    windows[i].on_tick(&windows[i]);
        }

        /* M22.4: task set changed (spawn/kill/exit/reap) → fire the
         * on_tick refreshes immediately, don't wait for the 1 Hz beat.
         * Ticks are idempotent refreshes, so an early call is safe. */
        if (tasks_changed) {
            tasks_changed = 0;
            for (int i = 0; i < GUI_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick)
                    windows[i].on_tick(&windows[i]);
        }

        if (need_frame) {
            need_frame = 0;
            compose();
        }
        hal_cpu_idle();
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

    spin_lock(&state_lock);

    mx += dx;  my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= fbsurf.w) mx = fbsurf.w - 1;
    if (my >= fbsurf.h) my = fbsurf.h - 1;

    unsigned pressed  =  buttons & ~btn_prev;
    unsigned released = ~buttons &  btn_prev;
    btn_prev = buttons;

    /* Chrome hover (launcher-menu highlight etc.). */
    if (shell && shell->motion) shell->motion(mx, my);

    if (pressed & MOUSE_BTN_LEFT) {
        /* The desktop shell gets first refusal (taskbar, menus).  If it
         * consumes the click, the windows below never see it. */
        if (shell && shell->click && shell->click(mx, my)) {
            goto drag_update;
        }

        struct gui_window* win = topmost_at(mx, my);
        if (win) {
            gui_wm_focus_raise_locked(win);

            int bx1 = win->x + win->w - BORDER - 3;          /* close right edge */
            int in_btn_row = (my >= win->y + 4 && my < win->y + 4 + CLOSE_H);
            int in_close = (in_btn_row &&
                            mx >= bx1 - CLOSE_W && mx < bx1);
            int in_min   = (in_btn_row &&
                            mx >= bx1 - 2 * CLOSE_W - 3 &&
                            mx <  bx1 - CLOSE_W - 3);
            if (in_close) {
                win->want_close = 1;
            } else if (in_min) {
                win->minimized = 1;
                struct gui_window* nf = top_visible_locked();
                focused_win = nf;
                if (nf && nf->kind == WIN_TERM) vc_focus(nf->vc);
            } else if (my < win->y + TITLE_H) {
                drag     = DRAG_MOVE;
                drag_win = win;
                grab_dx  = mx - win->x;
                grab_dy  = my - win->y;
            } else if (mx >= win->x + win->w - GRIP &&
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
        if (drag == DRAG_RESIZE && drag_win &&
            (rubber_w != drag_win->w || rubber_h != drag_win->h)) {
            drag_win->pending_w = rubber_w;
            drag_win->pending_h = rubber_h;
        }
        drag     = DRAG_NONE;
        drag_win = NULL;
    }

    /* M22.4 — damage policy: full recompose only for the RARE events
     * (press/release change focus/z-order; the resize rubber band is
     * thin but spans the window).  A DRAG_MOVE motion damages just
     * old-rect ∪ new-rect; a pure pointer glide is a bare wake — the
     * compositor adds the cursor rects itself (see compose()). */
    int structural = (pressed || released || drag == DRAG_RESIZE);
    spin_unlock(&state_lock);

    if (structural) {
        gui_damage_all();                       /* focus/z/rubber changes */
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
 * activate the new top visible window — repeated presses cycle. */
static int gui_raw_key(uint8_t keycode, uint8_t mods) {
    if (!gui_active) return 0;
    if (keycode != KC_TAB || !(mods & KBD_MOD_LALT)) return 0;
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
    struct gui_window* win = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (!windows[i].used) { win = &windows[i]; break; }
    if (!win) { kprintf("gui: window pool exhausted\n"); return NULL; }
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    if (h > work_h) h = work_h;

    win->kind = kind;
    win->x = x;  win->y = y;  win->w = w;  win->h = h;
    win->pending_w = win->pending_h = 0;
    win->want_close = 0;
    win->widgets = NULL;  win->focusw = NULL;
    win->on_layout = NULL; win->on_close = NULL; win->app_ctx = NULL;
    win->cells = NULL; win->vc = NULL;
    win->minimized = 0; win->on_tick = NULL;
    win->ccol = win->crow = win->cols = win->rows = 0;
    win->surf.px = NULL; win->surf.owns_px = 0;
    spin_lock_init(&win->lock);
    str_copy(win->title, title, (int)sizeof(win->title));
    win->used = 1;
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

struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h) {
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
    struct task* t = task_spawn("shell", shell_provider_active()->entry);
    if (t) {
        task_set_out_console(t, win->vc);
        win->vc->task = t;
    }
    preempt_enable();
    if (!t) kprintf("gui: shell spawn failed for '%s'\n", win->title);

    window_show(win);
    return win;
}

struct gui_window* gui_app_window_create(const char* title, int x, int y,
                                         int w, int h,
                                         void (*on_layout)(struct gui_window*),
                                         void* app_ctx) {
    struct gui_window* win = window_alloc(title, WIN_APP, x, y, w, h);
    if (!win) return NULL;
    win->on_layout = on_layout;
    win->app_ctx   = app_ctx;

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

    shell = pick_shell();
    if (shell && shell->init) shell->init(fbsurf.w, fbsurf.h);
    work_h = fbsurf.h -
             ((shell && shell->bottom_reserve) ? shell->bottom_reserve() : 0);

    gmax_cols = fbsurf.w / GFX_GLYPH_W;
    gmax_rows = fbsurf.h / GFX_GLYPH_H;

    gfx_vgradient(&wallsurf, 0, 0, wallsurf.w, wallsurf.h,
                  COL_WALL_TOP, COL_WALL_BOT);
    gfx_text(&wallsurf, wallsurf.w - 8 * GFX_GLYPH_W - 12,
             work_h - GFX_GLYPH_H - 8, "d-os M22", 0xFF9FB6C9u);

    spin_lock_init(&state_lock);
    spin_lock_init(&damage_lock);
    mx = fbsurf.w / 2;
    my = fbsurf.h / 2;

    gui_active = 1;
    vc_screen_suppress(1);

    gui_window_create("shell 1", 110,  80, 560, 360);
    gui_window_create("shell 2", 430, 300, 560, 360);

    mouse_set_listener(gui_mouse);
    vc_set_kbd_hook(gui_kbd_hook);
    vc_set_raw_kbd_hook(gui_raw_key);       /* Alt-Tab */
    task_set_change_hook(gui_task_change_hook);  /* M22.4: taskman refresh */
    gui_damage_all();

    if (!task_spawn("compositor", gui_compositor_main)) {
        kprintf("gui: FATAL — compositor spawn failed\n");
        vc_set_kbd_hook(NULL);
        vc_set_raw_kbd_hook(NULL);
        task_set_change_hook(NULL);
        mouse_set_listener(NULL);
        vc_screen_suppress(0);
        gui_active = 0;
        return -1;
    }

    kprintf("gui: up — %dx%d, %d windows, shell '%s', %d apps registered\n",
            fbsurf.w, fbsurf.h, zcount,
            shell ? shell->name : "none", gui_app_count());
    return 0;
}
