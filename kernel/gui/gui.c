/* =============================================================================
 * gui.c — compositor, window manager, taskbar, terminal + app windows
 * (M22 stages 3-5+7, M22.1: widget windows, taskbar, clock, file mgr glue).
 *
 * Threading model (three actors, three lock scopes):
 *
 *   shell task(s)   — gterm_emit renders shell output into the window's
 *                     content surface.       Holds:  win->lock.
 *   mouse/kbd IRQ   — updates cursor, focus, z-order, drag state,
 *                     pending resize/close; ENQUEUES app events/keys/
 *                     start-menu actions.    Holds:  state_lock.
 *   compositor task — drains the queues (widget callbacks run HERE, so
 *                     they may touch the VFS / kmalloc / create
 *                     windows), applies pending resizes + closes,
 *                     composes into the backbuffer, blits to the FB.
 *                     Holds: state_lock (irqsave, short) and win->lock
 *                     (per window), never both nested vs. other actors
 *                     in reverse order.
 *
 * Damage model: `need_frame` global flag; the compositor sleeps on
 * hlt+yield and recomposes the whole scene when it is up.  The taskbar
 * clock flips the flag once per second (RTC poll).
 *
 * Terminal windows keep a CHARACTER BACKING STORE (cells[]) sized for
 * the largest possible grid, so a resize re-renders the visible text
 * into the new surface — content survives (M22.1; the M22 cut lost it).
 * App (widget) windows re-run their on_layout + redraw on resize.
 *
 * Resize remains wireframe-style (rubber band while dragging, one
 * realloc on release) — but now with no content loss afterwards.
 * ============================================================================= */

#include "gui.h"
#include "gfx.h"
#include "widget.h"
#include "vc.h"
#include "task.h"
#include "lock.h"
#include "mouse.h"
#include "rtc.h"
#include "timer.h"
#include "kmalloc.h"
#include "printf.h"
#include "hal.h"
#include "hal_api.h"
#include <stddef.h>
#include <stdint.h>

/* shell.c export — same task entry the pane-split path spawns. */
extern void shell_task_entry(void);
/* fileman.c export — Start menu launches it. */
extern void fileman_open(void);

/* -------------------------------------------------------------------------- */
/* Metrics + palette.                                                          */
/* -------------------------------------------------------------------------- */

#define GUI_MAX_WINDOWS 8

#define BORDER      2
#define TITLE_H     18
#define GRIP        14
#define PAD         3
#define MIN_W       160
#define MIN_H       96
#define CLOSE_W     14                  /* app-window X button           */
#define CLOSE_H     11

#define TASKBAR_H   34
#define START_W     74
#define TBTN_W      150                 /* taskbar window button          */
#define CLOCK_W     78
#define SM_W        210                 /* start menu                     */
#define SM_ITEM_H   26

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

#define COL_TB_TOP      0xFF4B5A70u     /* taskbar gradient               */
#define COL_TB_BOT      0xFF222A37u
#define COL_TB_HILITE   0xFF7C8CA4u     /* 1px gloss line                 */
#define COL_START_TOP   0xFF58A84Bu     /* Start button (Vista green)     */
#define COL_START_BOT   0xFF2C6626u
#define COL_START_EDGE  0xFF83C877u
#define COL_TBTN_TOP    0xFF39465Au
#define COL_TBTN_BOT    0xFF2A3444u
#define COL_TBTN_F_TOP  0xFF5A83C0u     /* focused window's button        */
#define COL_TBTN_F_BOT  0xFF39598Cu
#define COL_TBTN_EDGE   0xFF55647Cu
#define COL_SM_BG       0xFF2B3546u
#define COL_SM_EDGE     0xFF55647Cu
#define COL_SM_HOVER    0xFF3D5C92u

/* -------------------------------------------------------------------------- */
/* Window object.                                                              */
/* -------------------------------------------------------------------------- */

enum win_kind { WIN_TERM, WIN_APP };

struct gui_window {
    int  used;
    enum win_kind kind;
    int  x, y, w, h;                    /* outer rect (state_lock)        */
    char title[24];

    /* Content surface (win->lock). */
    spinlock_t         lock;
    struct gfx_surface surf;

    /* WIN_TERM: grid cursor + char backing store + input VC. */
    int   cols, rows, ccol, crow;
    char* cells;                        /* gmax_cols × gmax_rows, row-major */
    struct vc* vc;

    /* WIN_APP: widgets + layout + lifetime hooks. */
    struct widget* widgets;
    struct widget* focusw;
    void (*on_layout)(struct gui_window*);
    void (*on_close) (struct gui_window*);
    void* app_ctx;                      /* kfree'd at destroy if non-NULL */

    /* IRQ → compositor handoff (state_lock). */
    int  pending_w, pending_h;          /* 0 = nothing pending            */
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

/* Start menu. */
static int menu_open  = 0;
static int menu_hover = -1;
static const char* const menu_items[] =
    { "File Manager", "New Shell", "About d-os", "Reboot", "Shut Down" };
#define MENU_N ((int)(sizeof menu_items / sizeof menu_items[0]))

/* Double-click tracking (IRQ only). */
static uint64_t lastclick_ms = 0;
static int lastclick_x = -100, lastclick_y = -100;
static struct gui_window* lastclick_win = NULL;

/* Scene. */
static struct gfx_surface fbsurf, backsurf, wallsurf;
static int work_h = 0;                  /* fb height minus taskbar        */
static int gmax_cols = 0, gmax_rows = 0;/* largest term grid possible     */

static volatile int need_frame = 0;
static int gui_active = 0;

/* Taskbar clock cache (compositor-owned). */
static char clock_str[12] = "";

/* ---- IRQ → compositor queues (all SPSC: IRQ produces, compositor consumes) - */

struct gev {                            /* widget event                   */
    struct gui_window* win;
    int16_t x, y;                       /* content-relative               */
    uint8_t dbl;
};
#define EVQ_SZ 32
static struct gev        evq[EVQ_SZ];
static volatile uint32_t evq_h = 0, evq_t = 0;

#define KEYQ_SZ 32
static volatile char     keyq[KEYQ_SZ];
static volatile uint32_t keyq_h = 0, keyq_t = 0;

enum gui_action { ACT_FILEMAN = 1, ACT_NEWSHELL, ACT_ABOUT, ACT_REBOOT, ACT_SHUTDOWN };
#define ACTQ_SZ 8
static volatile uint8_t  actq[ACTQ_SZ];
static volatile uint32_t actq_h = 0, actq_t = 0;

static void evq_push(struct gui_window* w, int cx, int cy, int dbl) {
    uint32_t n = (evq_h + 1) % EVQ_SZ;
    if (n == evq_t) return;
    evq[evq_h].win = w;
    evq[evq_h].x = (int16_t)cx;
    evq[evq_h].y = (int16_t)cy;
    evq[evq_h].dbl = (uint8_t)dbl;
    evq_h = n;
}
static void actq_push(uint8_t a) {
    uint32_t n = (actq_h + 1) % ACTQ_SZ;
    if (n == actq_t) return;
    actq[actq_h] = a;
    actq_h = n;
}

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
/* Terminal-in-a-window ("gterm").  Emit runs in the shell task's context;    */
/* pixels + cells update under win->lock.                                     */
/* -------------------------------------------------------------------------- */

static void gterm_draw_cell(struct gui_window* win, int col, int row, char c) {
    int px = PAD + col * GFX_GLYPH_W;
    int py = PAD + row * GFX_GLYPH_H;
    char s[2] = { c, 0 };
    gfx_fill(&win->surf, px, py, GFX_GLYPH_W, GFX_GLYPH_H, COL_WIN_BG);
    if (c > 0x20) gfx_text(&win->surf, px, py, s, COL_WIN_FG);
}

static void gterm_scroll(struct gui_window* win) {
    /* Pixel scroll (upward copy is safe top-to-bottom)... */
    struct gfx_surface* s = &win->surf;
    int top    = PAD;
    int bottom = PAD + win->rows * GFX_GLYPH_H;
    int lift   = GFX_GLYPH_H * s->stride;
    for (int y = top; y < bottom - GFX_GLYPH_H; y++) {
        uint32_t* row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = row[x + lift];
    }
    gfx_fill(s, 0, bottom - GFX_GLYPH_H, s->w, GFX_GLYPH_H, COL_WIN_BG);

    /* ...mirrored in the char backing store. */
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

/* Redraw every populated cell (used after a resize). */
static void gterm_rerender_locked(struct gui_window* win) {
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    for (int r = 0; r < win->rows; r++)
        for (int c = 0; c < win->cols; c++) {
            char ch = win->cells[(size_t)r * gmax_cols + c];
            if (ch) gterm_draw_cell(win, c, r, ch);
        }
}

/* -------------------------------------------------------------------------- */
/* App-window redraw.                                                          */
/* -------------------------------------------------------------------------- */

static void app_redraw(struct gui_window* win) {
    spin_lock(&win->lock);
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    widget_draw_all(win->widgets, &win->surf);
    spin_unlock(&win->lock);
    need_frame = 1;
}

/* (Re)build the content surface for outer size w×h and restore the
 * content (term: from cells; app: re-layout + widget redraw). */
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
            /* Keep the cursor visible: if the new grid is shorter than
             * the cursor row, scroll the store up by the difference. */
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

    gfx_surface_free(&old);                      /* no-op on first call */

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
    struct widget** p = &win->widgets;           /* append: draw order = z */
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

/* Defined later (needs raise_window); declared here for reading order. */
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
/* Taskbar geometry helpers — shared by draw (compositor) and hit-test (IRQ). */
/* -------------------------------------------------------------------------- */

/* Collect used windows in stable (pool-index) order.  Returns count. */
static int taskbar_slots(struct gui_window** out) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used) out[n++] = &windows[i];
    return n;
}

static int tbtn_width(int nslots) {
    int avail = fbsurf.w - (START_W + 12) - CLOCK_W - 8;
    if (nslots <= 0) return TBTN_W;
    int w = avail / nslots - 6;
    if (w > TBTN_W) w = TBTN_W;
    if (w < 48)     w = 48;
    return w;
}

static void draw_taskbar(void) {
    int ty = work_h;
    gfx_vgradient(&backsurf, 0, ty, fbsurf.w, TASKBAR_H, COL_TB_TOP, COL_TB_BOT);
    gfx_fill(&backsurf, 0, ty, fbsurf.w, 1, COL_TB_HILITE);

    /* Start button. */
    gfx_vgradient(&backsurf, 4, ty + 4, START_W, TASKBAR_H - 8,
                  menu_open ? COL_START_EDGE : COL_START_TOP, COL_START_BOT);
    draw_rect_outline(&backsurf, 4, ty + 4, START_W, TASKBAR_H - 8, 1,
                      COL_START_EDGE);
    gfx_text(&backsurf, 4 + (START_W - 5 * GFX_GLYPH_W) / 2,
             ty + (TASKBAR_H - GFX_GLYPH_H) / 2, "Start", COL_TITLE_TEXT);

    /* Window buttons. */
    struct gui_window* slots[GUI_MAX_WINDOWS];
    int n  = taskbar_slots(slots);
    int bw = tbtn_width(n);
    int x  = START_W + 12;
    for (int i = 0; i < n; i++) {
        struct gui_window* w = slots[i];
        int focused = (w == focused_win);
        gfx_vgradient(&backsurf, x, ty + 5, bw, TASKBAR_H - 10,
                      focused ? COL_TBTN_F_TOP : COL_TBTN_TOP,
                      focused ? COL_TBTN_F_BOT : COL_TBTN_BOT);
        draw_rect_outline(&backsurf, x, ty + 5, bw, TASKBAR_H - 10, 1,
                          COL_TBTN_EDGE);
        char t[20];
        int maxch = (bw - 12) / GFX_GLYPH_W;
        if (maxch > (int)sizeof(t) - 1) maxch = (int)sizeof(t) - 1;
        int k = 0;
        for (; w->title[k] && k < maxch; k++) t[k] = w->title[k];
        t[k] = 0;
        gfx_text(&backsurf, x + 6, ty + (TASKBAR_H - GFX_GLYPH_H) / 2, t,
                 COL_TITLE_TEXT);
        x += bw + 6;
    }

    /* Clock. */
    int cx = fbsurf.w - CLOCK_W;
    gfx_fill(&backsurf, cx - 1, ty + 6, 1, TASKBAR_H - 12, 0xFF141B26u);
    gfx_fill(&backsurf, cx,     ty + 6, 1, TASKBAR_H - 12, COL_TB_HILITE);
    if (clock_str[0]) {
        int len = 0;
        while (clock_str[len]) len++;
        gfx_text(&backsurf, cx + (CLOCK_W - len * GFX_GLYPH_W) / 2,
                 ty + (TASKBAR_H - GFX_GLYPH_H) / 2, clock_str, COL_TITLE_TEXT);
    }
}

static void draw_start_menu(void) {
    int mh = MENU_N * SM_ITEM_H + 12;
    int myy = work_h - mh;
    gfx_blend_fill(&backsurf, 4 + 4, myy + 4, SM_W, mh, COL_SHADOW);
    gfx_fill(&backsurf, 4, myy, SM_W, mh, COL_SM_BG);
    draw_rect_outline(&backsurf, 4, myy, SM_W, mh, 1, COL_SM_EDGE);
    for (int i = 0; i < MENU_N; i++) {
        int iy = myy + 6 + i * SM_ITEM_H;
        if (i == menu_hover)
            gfx_fill(&backsurf, 6, iy, SM_W - 4, SM_ITEM_H, COL_SM_HOVER);
        gfx_text(&backsurf, 18, iy + (SM_ITEM_H - GFX_GLYPH_H) / 2,
                 menu_items[i], COL_TITLE_TEXT);
    }
}

/* -------------------------------------------------------------------------- */
/* Composition.                                                                */
/* -------------------------------------------------------------------------- */

static void compose(void) {
    /* Snapshot WM state so the mouse IRQ can keep mutating while we
     * paint. */
    struct gui_window* zsnap[GUI_MAX_WINDOWS];
    int   zn;
    int   cx, cy;
    enum drag_mode dsnap;
    struct gui_window* dwin;
    int   rw, rh, rx = 0, ry = 0;
    struct gui_window* fsnap;
    int   wx[GUI_MAX_WINDOWS], wy[GUI_MAX_WINDOWS],
          ww[GUI_MAX_WINDOWS], wh[GUI_MAX_WINDOWS];
    int   msnap, hsnap;

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
    msnap = menu_open; hsnap = menu_hover;
    spin_unlock_irqrestore(&state_lock, fl);

    gfx_blit(&backsurf, 0, 0, &wallsurf, 0, 0, wallsurf.w, wallsurf.h);

    /* Windows, bottom → top. */
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

        /* App windows get a close button. */
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

    draw_taskbar();
    if (msnap) { menu_hover = hsnap; draw_start_menu(); }

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

    /* Free widget chain + surface + ctx.  No lock needed: the IRQ never
     * touches these, and we (the compositor) are the only other user. */
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
/* About window (tiny built-in app).                                           */
/* -------------------------------------------------------------------------- */

static struct gui_window* about_win = NULL;

static void about_on_close(struct gui_window* win) {
    (void)win;
    about_win = NULL;
}

static void about_open(void) {
    if (about_win) return;                       /* singleton */
    struct gui_window* w =
        gui_app_window_create("About d-os", fbsurf.w / 2 - 150,
                              fbsurf.h / 2 - 90, 300, 150, NULL, NULL);
    if (!w) return;
    about_win = w;
    gui_window_set_on_close(w, about_on_close);
    w_label_create(w, 16, 12, 260, "d-os — hobby teaching kernel");
    w_label_create(w, 16, 34, 260, "M22.1: compositor + taskbar +");
    w_label_create(w, 16, 50, 260, "widgets + file manager");
    struct w_label* d = w_label_create(w, 16, 78, 260,
                                       "i386 / x86_64 - PLAN.md M22");
    if (d) d->color = 0xFF8C9AAAu;
    gui_window_request_redraw(w);
}

/* -------------------------------------------------------------------------- */
/* Queue dispatch — runs on the compositor task.                               */
/* -------------------------------------------------------------------------- */

static int next_shell_no = 3;                    /* 1 + 2 exist at start   */

static void dispatch_actions(void) {
    while (actq_t != actq_h) {
        uint8_t a = actq[actq_t];
        actq_t = (actq_t + 1) % ACTQ_SZ;
        switch (a) {
        case ACT_FILEMAN:  fileman_open(); break;
        case ACT_ABOUT:    about_open();   break;
        case ACT_NEWSHELL: {
            char name[16] = "shell ";
            int n = next_shell_no++;
            int p = 6;
            if (n >= 10) name[p++] = (char)('0' + n / 10);
            name[p++] = (char)('0' + n % 10);
            name[p] = 0;
            int k = n % 5;
            gui_window_create(name, 120 + k * 48, 80 + k * 40, 560, 360);
            break;
        }
        case ACT_REBOOT:   hal_reboot();   break;
        case ACT_SHUTDOWN: hal_shutdown(); break;
        default: break;
        }
    }
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
        if (win->used) app_redraw(win);          /* callback may have closed it */
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

/* Refresh the taskbar clock; flips need_frame when the second rolls. */
static void update_clock(void) {
    struct rtc_time t;
    if (rtc_read(&t) != 0) return;
    char s[12];
    s[0] = (char)('0' + t.hour / 10);  s[1] = (char)('0' + t.hour % 10);
    s[2] = ':';
    s[3] = (char)('0' + t.min / 10);   s[4] = (char)('0' + t.min % 10);
    s[5] = ':';
    s[6] = (char)('0' + t.sec / 10);   s[7] = (char)('0' + t.sec % 10);
    s[8] = 0;
    int same = 1;
    for (int i = 0; i < 9; i++) if (clock_str[i] != s[i]) { same = 0; break; }
    if (!same) {
        for (int i = 0; i < 9; i++) clock_str[i] = s[i];
        need_frame = 1;
    }
}

static void gui_compositor_main(void) {
    kprintf("gui: compositor up on pid %d\n",
            task_current() ? task_current()->pid : -1);
    for (;;) {
        dispatch_actions();
        dispatch_events();
        dispatch_keys();
        apply_pending();
        update_clock();
        if (need_frame) {
            need_frame = 0;
            compose();
        }
        hal_cpu_idle();
        task_yield();
    }
}

/* -------------------------------------------------------------------------- */
/* Pointer handling — IRQ context.  Geometry + queueing only.                  */
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

/* Click landed on the taskbar strip.  Returns with state updated. */
static void taskbar_click(int px) {
    if (px >= 4 && px < 4 + START_W) {           /* Start button */
        menu_open = !menu_open;
        menu_hover = -1;
        return;
    }
    menu_open = 0;

    struct gui_window* slots[GUI_MAX_WINDOWS];
    int n  = taskbar_slots(slots);
    int bw = tbtn_width(n);
    int x  = START_W + 12;
    for (int i = 0; i < n; i++) {
        if (px >= x && px < x + bw) {
            raise_window(slots[i]);
            focused_win = slots[i];
            if (slots[i]->kind == WIN_TERM) vc_focus(slots[i]->vc);
            return;
        }
        x += bw + 6;
    }
}

static void gui_mouse(int dx, int dy, unsigned buttons) {
    spin_lock(&state_lock);                      /* IRQ context: IF clear */

    mx += dx;  my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= fbsurf.w) mx = fbsurf.w - 1;
    if (my >= fbsurf.h) my = fbsurf.h - 1;

    unsigned pressed  =  buttons & ~btn_prev;
    unsigned released = ~buttons &  btn_prev;
    btn_prev = buttons;

    /* Start-menu hover follows the pointer. */
    if (menu_open) {
        int mh  = MENU_N * SM_ITEM_H + 12;
        int myy = work_h - mh;
        if (mx >= 4 && mx < 4 + SM_W && my >= myy + 6 && my < myy + 6 + MENU_N * SM_ITEM_H)
            menu_hover = (my - myy - 6) / SM_ITEM_H;
        else
            menu_hover = -1;
    }

    if (pressed & MOUSE_BTN_LEFT) {
        int mh  = MENU_N * SM_ITEM_H + 12;
        int myy = work_h - mh;

        if (menu_open && mx >= 4 && mx < 4 + SM_W && my >= myy && my < work_h) {
            /* Start-menu item click → action queue (work happens on the
             * compositor task, never in the IRQ). */
            int idx = (my - myy - 6) / SM_ITEM_H;
            if (idx >= 0 && idx < MENU_N)
                actq_push((uint8_t)(ACT_FILEMAN + idx));
            menu_open = 0;
        } else if (my >= work_h) {
            taskbar_click(mx);
        } else {
            menu_open = 0;
            struct gui_window* win = topmost_at(mx, my);
            if (win) {
                raise_window(win);
                focused_win = win;
                if (win->kind == WIN_TERM) vc_focus(win->vc);

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
                    /* Content click → widget event (dispatch on task). */
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
    }

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

/* Keyboard intercept (vc_kbd_push choke point): typing aimed at an APP
 * window goes to the key queue instead of a VC ring. */
static int gui_kbd_hook(char c) {
    if (!gui_active) return 0;
    struct gui_window* win = focused_win;        /* pointer read is atomic */
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

    /* Full reset — the slot may have hosted a closed window before. */
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

    /* Spawn the shell exactly like cmd_pane_split does. */
    preempt_disable();
    struct task* t = task_spawn("shell", shell_task_entry);
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

    work_h    = fbsurf.h - TASKBAR_H;
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

    kprintf("gui: up — %dx%d, %d windows, taskbar %dpx\n",
            fbsurf.w, fbsurf.h, zcount, TASKBAR_H);
    return 0;
}
