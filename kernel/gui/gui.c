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

void gui_request_frame(void) { need_frame = 1; }

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
    need_frame = 1;
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
    need_frame = 1;
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
    if (win && win->used && win->kind == WIN_APP) {
        win->want_close = 1;
        need_frame = 1;
    }
}

static void raise_window(struct gui_window* win);

void gui_window_raise(struct gui_window* win) {
    if (!win || !win->used) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    raise_window(win);
    focused_win = win;
    spin_unlock_irqrestore(&state_lock, fl);
    if (win->kind == WIN_TERM) vc_focus(win->vc);
    need_frame = 1;
}

void gui_wm_focus_raise_locked(struct gui_window* w) {
    if (!w || !w->used) return;
    raise_window(w);
    focused_win = w;
    if (w->kind == WIN_TERM) vc_focus(w->vc);
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

static void compose(void) {
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
    zn = zcount;
    for (int i = 0; i < zn; i++) {
        zsnap[i] = zorder[i];
        wx[i] = zorder[i]->x;  wy[i] = zorder[i]->y;
        ww[i] = zorder[i]->w;  wh[i] = zorder[i]->h;
    }
    cx = mx; cy = my;
    dsnap = drag; dwin = drag_win; rw = rubber_w; rh = rubber_h;
    if (dwin) { rx = dwin->x; ry = dwin->y; }
    fsnap = focused_win;
    spin_unlock_irqrestore(&state_lock, fl);

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

        if (win->kind == WIN_APP) {
            int bx = x + w - BORDER - CLOSE_W - 3;
            int by = y + 4;
            gfx_fill(&backsurf, bx, by, CLOSE_W, CLOSE_H, COL_CLOSE_BG);
            gfx_text(&backsurf, bx + (CLOSE_W - GFX_GLYPH_W) / 2, by + 2, "x",
                     COL_CLOSE_FG);
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
    gfx_blit(&fbsurf, 0, 0, &backsurf, 0, 0, backsurf.w, backsurf.h);
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
        (focused_win == win) ? (zcount ? zorder[zcount - 1] : NULL) : focused_win;
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
    need_frame = 1;
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

        if (win->want_close) { win->want_close = 0; destroy_window(win); continue; }

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
            if (shell && shell->second_tick && shell->second_tick())
                need_frame = 1;
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

            int in_close = (win->kind == WIN_APP &&
                            mx >= win->x + win->w - BORDER - CLOSE_W - 3 &&
                            mx <  win->x + win->w - BORDER - 3 &&
                            my >= win->y + 4 &&
                            my <  win->y + 4 + CLOSE_H);
            if (in_close) {
                win->want_close = 1;
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
        drag_win->x = mx - grab_dx;
        drag_win->y = my - grab_dy;
        if (drag_win->x < -(drag_win->w - 40)) drag_win->x = -(drag_win->w - 40);
        if (drag_win->x > fbsurf.w - 40)       drag_win->x = fbsurf.w - 40;
        if (drag_win->y < 0)                   drag_win->y = 0;
        if (drag_win->y > work_h - TITLE_H)    drag_win->y = work_h - TITLE_H;
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

    spin_unlock(&state_lock);
    need_frame = 1;
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
    need_frame = 1;
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
    mx = fbsurf.w / 2;
    my = fbsurf.h / 2;

    gui_active = 1;
    vc_screen_suppress(1);

    gui_window_create("shell 1", 110,  80, 560, 360);
    gui_window_create("shell 2", 430, 300, 560, 360);

    mouse_set_listener(gui_mouse);
    vc_set_kbd_hook(gui_kbd_hook);

    if (!task_spawn("compositor", gui_compositor_main)) {
        kprintf("gui: FATAL — compositor spawn failed\n");
        vc_set_kbd_hook(NULL);
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
