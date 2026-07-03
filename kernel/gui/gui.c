/* =============================================================================
 * gui.c — compositor, window manager, terminal windows (M22 stages 3-5+7).
 *
 * Threading model (three actors, three lock scopes):
 *
 *   shell task(s)   — gterm_emit renders shell output into the window's
 *                     content surface.       Holds:  win->lock.
 *   mouse IRQ       — updates cursor, focus, z-order, drag state,
 *                     pending resize.        Holds:  state_lock.
 *   compositor task — snapshots WM state (state_lock, irqsave because
 *                     the other holder is an IRQ), applies pending
 *                     resizes + blits content (win->lock per window),
 *                     composes into the backbuffer, blits to the FB.
 *
 * The two lock classes never nest across actors in opposite orders:
 * emit takes only win->lock; the IRQ takes only state_lock; the
 * compositor takes state_lock and win->lock strictly one-at-a-time.
 *
 * Damage model: `need_frame` is a cheap global "something changed" flag
 * (set by emit, mouse, resize).  The compositor sleeps on hlt+yield and
 * recomposes the whole scene when the flag is up.  Full recompose of
 * 1024×768 is ~3 MiB of writes — a per-window damage-rect refinement is
 * a follow-up, the flag already gives us "no work when idle."
 *
 * Resize is wireframe-style: while the grip is dragged we only draw a
 * rubber-band outline (no realloc per motion event); the surface is
 * reallocated once, on button release.  Content is not preserved across
 * a resize (same policy as vc.c's repaint_all on pane splits) — the
 * shell keeps printing from the new top-left.
 * ============================================================================= */

#include "gui.h"
#include "gfx.h"
#include "vc.h"
#include "task.h"
#include "lock.h"
#include "mouse.h"
#include "kmalloc.h"
#include "printf.h"
#include "hal_api.h"
#include <stddef.h>
#include <stdint.h>

/* shell.c export — same task entry the pane-split path spawns. */
extern void shell_task_entry(void);

/* -------------------------------------------------------------------------- */
/* Metrics + palette.                                                          */
/* -------------------------------------------------------------------------- */

#define GUI_MAX_WINDOWS 8

#define BORDER      2                   /* frame thickness, px               */
#define TITLE_H     18                  /* title bar height incl. top border */
#define GRIP        14                  /* resize hot-zone square, px        */
#define PAD         3                   /* text padding inside content, px   */
#define MIN_W       160                 /* outer minimum on resize           */
#define MIN_H       96

#define COL_WALL_TOP    0xFF10243Eu     /* wallpaper gradient                */
#define COL_WALL_BOT    0xFF1B5E63u
#define COL_WIN_BG      0xFF101828u     /* terminal background (matches VC)  */
#define COL_WIN_FG      0xFFE0E0E0u
#define COL_TITLE_F_TOP 0xFF3D7BD8u     /* focused title gradient            */
#define COL_TITLE_F_BOT 0xFF29579Eu
#define COL_TITLE_U_TOP 0xFF4A5568u     /* unfocused title gradient          */
#define COL_TITLE_U_BOT 0xFF353D49u
#define COL_BORDER_F    0xFF3D7BD8u
#define COL_BORDER_U    0xFF3A424Eu
#define COL_TITLE_TEXT  0xFFF2F5FAu
#define COL_SHADOW      0x48000000u     /* blend-fill: ~28% black            */
#define COL_RUBBER      0xFFE8C25Au     /* resize rubber-band                */

/* -------------------------------------------------------------------------- */
/* Window object.                                                              */
/* -------------------------------------------------------------------------- */

struct gui_window {
    int  used;
    int  x, y, w, h;                    /* outer rect, px (owned by state_lock) */
    char title[24];

    /* Content surface + terminal grid (owned by win->lock). */
    spinlock_t         lock;
    struct gfx_surface surf;
    int  cols, rows;                    /* grid derived from surf size      */
    int  ccol, crow;                    /* cursor                           */

    /* Resize handoff: IRQ writes, compositor consumes (state_lock). */
    int  pending_w, pending_h;          /* 0 = nothing pending              */

    struct vc* vc;                      /* offscreen VC feeding this window */
};

static struct gui_window windows[GUI_MAX_WINDOWS];

/* Z-order, bottom → top.  Owned by state_lock. */
static struct gui_window* zorder[GUI_MAX_WINDOWS];
static int                zcount = 0;
static struct gui_window* focused_win = NULL;

/* WM / pointer state.  Owned by state_lock (IRQ writer). */
static spinlock_t state_lock;
static int mx, my;                      /* cursor position                  */
static unsigned btn_prev = 0;
enum drag_mode { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };
static enum drag_mode      drag = DRAG_NONE;
static struct gui_window*  drag_win = NULL;
static int grab_dx, grab_dy;            /* cursor offset inside window      */
static int rubber_w, rubber_h;          /* live rubber-band size (RESIZE)   */

/* Scene. */
static struct gfx_surface fbsurf;       /* the real framebuffer             */
static struct gfx_surface backsurf;     /* compose target                   */
static struct gfx_surface wallsurf;     /* pre-rendered wallpaper           */

static volatile int need_frame = 0;     /* damage flag                      */
static int gui_active = 0;

/* -------------------------------------------------------------------------- */
/* Terminal-in-a-window ("gterm") — the vc emit hook.                          */
/* Runs in the shell task's context.  Caller-visible state is the grid        */
/* cursor; pixels land in win->surf under win->lock.                          */
/* -------------------------------------------------------------------------- */

static void gterm_draw_cell(struct gui_window* win, int col, int row, char c) {
    int px = PAD + col * GFX_GLYPH_W;
    int py = PAD + row * GFX_GLYPH_H;
    char s[2] = { c, 0 };
    gfx_fill(&win->surf, px, py, GFX_GLYPH_W, GFX_GLYPH_H, COL_WIN_BG);
    gfx_text(&win->surf, px, py, s, COL_WIN_FG);
}

/* Shift the text region up one glyph row.  Upward copies are safe
 * top-to-bottom within the same surface (src below dst). */
static void gterm_scroll(struct gui_window* win) {
    struct gfx_surface* s = &win->surf;
    int top    = PAD;
    int bottom = PAD + win->rows * GFX_GLYPH_H;      /* exclusive */
    int lift   = GFX_GLYPH_H * s->stride;
    for (int y = top; y < bottom - GFX_GLYPH_H; y++) {
        uint32_t* row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) row[x] = row[x + lift];
    }
    gfx_fill(s, 0, bottom - GFX_GLYPH_H, s->w, GFX_GLYPH_H, COL_WIN_BG);
}

static void gterm_emit(void* ctx, char c) {
    struct gui_window* win = (struct gui_window*)ctx;
    spin_lock(&win->lock);

    if (c == '\f') {                                  /* vc_clear() */
        gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
        win->ccol = win->crow = 0;
    } else if (c == '\n') {
        win->ccol = 0;
        if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
    } else if (c == '\r') {
        win->ccol = 0;
    } else if (c == '\b') {
        if (win->ccol > 0) {
            win->ccol--;
            gterm_draw_cell(win, win->ccol, win->crow, ' ');
        }
    } else {
        gterm_draw_cell(win, win->ccol, win->crow, c);
        if (++win->ccol >= win->cols) {
            win->ccol = 0;
            if (++win->crow >= win->rows) { gterm_scroll(win); win->crow = win->rows - 1; }
        }
    }

    spin_unlock(&win->lock);
    need_frame = 1;
}

/* (Re)build the content surface for outer size w×h.  Caller must NOT
 * hold win->lock; the swap itself is done under it so a concurrent
 * emit never dereferences freed pixels. */
static int gterm_set_size(struct gui_window* win, int outer_w, int outer_h) {
    int cw = outer_w - 2 * BORDER;
    int ch = outer_h - TITLE_H - BORDER;
    struct gfx_surface ns;
    if (gfx_surface_init(&ns, cw, ch) != 0) return -1;
    gfx_fill(&ns, 0, 0, cw, ch, COL_WIN_BG);

    spin_lock(&win->lock);
    struct gfx_surface old = win->surf;
    win->surf = ns;
    win->cols = (cw - 2 * PAD) / GFX_GLYPH_W;
    win->rows = (ch - 2 * PAD) / GFX_GLYPH_H;
    win->ccol = win->crow = 0;
    spin_unlock(&win->lock);

    gfx_surface_free(&old);                           /* no-op on first call */
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Cursor sprite — classic 11×17 arrow.  'X' = black outline, '.' = white.    */
/* Drawn directly onto the backbuffer AFTER the windows, so it is never       */
/* overwritten and never needs save-under bookkeeping.                        */
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

static void draw_rect_outline(struct gfx_surface* s, int x, int y, int w, int h,
                              int t, uint32_t c) {
    gfx_fill(s, x,         y,         w, t, c);
    gfx_fill(s, x,         y + h - t, w, t, c);
    gfx_fill(s, x,         y,         t, h, c);
    gfx_fill(s, x + w - t, y,         t, h, c);
}

static void compose(void) {
    /* 1. Snapshot WM state so the mouse IRQ can keep mutating while we
     *    paint.  Geometry ints copy fast; the surfaces are locked
     *    per-window at blit time. */
    struct gui_window* zsnap[GUI_MAX_WINDOWS];
    int   zn;
    int   cx, cy;
    enum drag_mode dsnap;
    struct gui_window* dwin;
    int   rw, rh;
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
    fsnap = focused_win;
    spin_unlock_irqrestore(&state_lock, fl);

    /* 2. Wallpaper. */
    gfx_blit(&backsurf, 0, 0, &wallsurf, 0, 0, wallsurf.w, wallsurf.h);

    /* 3. Windows, bottom → top. */
    for (int i = 0; i < zn; i++) {
        struct gui_window* win = zsnap[i];
        int x = wx[i], y = wy[i], w = ww[i], h = wh[i];
        int focused = (win == fsnap);

        /* Drop shadow — the one place the alpha-blend primitive earns
         * its keep in this milestone. */
        gfx_blend_fill(&backsurf, x + 5, y + 5, w, h, COL_SHADOW);

        /* Frame + title bar. */
        draw_rect_outline(&backsurf, x, y, w, h, BORDER,
                          focused ? COL_BORDER_F : COL_BORDER_U);
        gfx_vgradient(&backsurf, x + BORDER, y + BORDER,
                      w - 2 * BORDER, TITLE_H - BORDER,
                      focused ? COL_TITLE_F_TOP : COL_TITLE_U_TOP,
                      focused ? COL_TITLE_F_BOT : COL_TITLE_U_BOT);
        gfx_text(&backsurf, x + 8, y + (TITLE_H - GFX_GLYPH_H + BORDER) / 2,
                 win->title, COL_TITLE_TEXT);

        /* Content. */
        spin_lock(&win->lock);
        gfx_blit(&backsurf, x + BORDER, y + TITLE_H,
                 &win->surf, 0, 0, win->surf.w, win->surf.h);
        spin_unlock(&win->lock);

        /* Resize grip: three diagonal ticks in the bottom-right corner. */
        uint32_t gc = focused ? COL_BORDER_F : COL_BORDER_U;
        for (int t = 0; t < 3; t++) {
            int o = 4 + t * 4;
            gfx_line(&backsurf, x + w - 3 - o, y + h - 4,
                                x + w - 4,     y + h - 3 - o, gc);
        }
    }

    /* 4. Rubber-band outline while a resize drag is live. */
    if (dsnap == DRAG_RESIZE && dwin)
        draw_rect_outline(&backsurf, dwin->x, dwin->y, rw, rh, 2, COL_RUBBER);

    /* 5. Cursor, then push the finished frame to the framebuffer in one
     *    blit (no flicker: the FB is never partially drawn). */
    draw_cursor(&backsurf, cx, cy);
    gfx_blit(&fbsurf, 0, 0, &backsurf, 0, 0, backsurf.w, backsurf.h);
}

/* Apply resizes parked by the mouse IRQ.  Runs on the compositor task
 * so the kmalloc/kfree of surfaces never happens in IRQ context. */
static void apply_pending_resizes(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used) continue;

        int nw = 0, nh = 0;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        if (win->pending_w) {
            nw = win->pending_w;  nh = win->pending_h;
            win->pending_w = win->pending_h = 0;
            win->w = nw;  win->h = nh;
        }
        spin_unlock_irqrestore(&state_lock, fl);

        if (nw && gterm_set_size(win, nw, nh) != 0)
            kprintf("gui: resize OOM (%dx%d), window keeps stale surface\n", nw, nh);
    }
}

static void gui_compositor_main(void) {
    kprintf("gui: compositor up on pid %d\n",
            task_current() ? task_current()->pid : -1);
    for (;;) {
        if (need_frame) {
            need_frame = 0;
            apply_pending_resizes();
            compose();
        }
        /* Sleep until any IRQ (mouse, keyboard, timer tick), then give
         * other tasks a turn.  Worst-case frame latency = one tick. */
        hal_cpu_idle();
        task_yield();
    }
}

/* -------------------------------------------------------------------------- */
/* Pointer handling — runs in IRQ context.  Keep it short: geometry math      */
/* and flag flips only; no allocation, no surface access.                     */
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
    spin_lock(&state_lock);              /* IRQ context: IF already clear */

    mx += dx;  my += dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= fbsurf.w) mx = fbsurf.w - 1;
    if (my >= fbsurf.h) my = fbsurf.h - 1;

    unsigned pressed  =  buttons & ~btn_prev;
    unsigned released = ~buttons &  btn_prev;
    btn_prev = buttons;

    if (pressed & MOUSE_BTN_LEFT) {
        struct gui_window* win = topmost_at(mx, my);
        if (win) {
            raise_window(win);
            focused_win = win;
            vc_focus(win->vc);                   /* keyboard follows click */

            if (my < win->y + TITLE_H) {
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
            }
        }
    }

    if (drag == DRAG_MOVE && drag_win) {
        drag_win->x = mx - grab_dx;
        drag_win->y = my - grab_dy;
        /* Keep the title bar reachable: never let it leave the screen. */
        if (drag_win->x < -(drag_win->w - 40)) drag_win->x = -(drag_win->w - 40);
        if (drag_win->x > fbsurf.w - 40)       drag_win->x = fbsurf.w - 40;
        if (drag_win->y < 0)                   drag_win->y = 0;
        if (drag_win->y > fbsurf.h - TITLE_H)  drag_win->y = fbsurf.h - TITLE_H;
    } else if (drag == DRAG_RESIZE && drag_win) {
        rubber_w = mx - drag_win->x + 2;
        rubber_h = my - drag_win->y + 2;
        if (rubber_w < MIN_W) rubber_w = MIN_W;
        if (rubber_h < MIN_H) rubber_h = MIN_H;
        if (rubber_w > fbsurf.w) rubber_w = fbsurf.w;
        if (rubber_h > fbsurf.h) rubber_h = fbsurf.h;
    }

    if (released & MOUSE_BTN_LEFT) {
        if (drag == DRAG_RESIZE && drag_win &&
            (rubber_w != drag_win->w || rubber_h != drag_win->h)) {
            /* Park the new size; the compositor reallocates the surface
             * outside IRQ context. */
            drag_win->pending_w = rubber_w;
            drag_win->pending_h = rubber_h;
        }
        drag     = DRAG_NONE;
        drag_win = NULL;
    }

    spin_unlock(&state_lock);
    need_frame = 1;
}

/* -------------------------------------------------------------------------- */
/* Window creation + bring-up.                                                 */
/* -------------------------------------------------------------------------- */

static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h) {
    struct gui_window* win = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].used) { win = &windows[i]; break; }
    }
    if (!win) { kprintf("gui: window pool exhausted\n"); return NULL; }
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;

    win->used = 1;
    win->x = x;  win->y = y;  win->w = w;  win->h = h;
    win->pending_w = win->pending_h = 0;
    spin_lock_init(&win->lock);
    str_copy(win->title, title, (int)sizeof(win->title));

    if (gterm_set_size(win, w, h) != 0) { win->used = 0; return NULL; }

    win->vc = vc_create_offscreen(gterm_emit, win);
    if (!win->vc) {
        gfx_surface_free(&win->surf);
        win->used = 0;
        return NULL;
    }

    /* Spawn the shell exactly like cmd_pane_split does: bind the VC
     * before the runqueue can pick the task so its first kprintf
     * already lands in this window. */
    preempt_disable();
    struct task* t = task_spawn("shell", shell_task_entry);
    if (t) {
        task_set_out_console(t, win->vc);
        win->vc->task = t;
    }
    preempt_enable();
    if (!t) kprintf("gui: shell spawn failed for '%s'\n", win->title);

    /* Insert on top + take focus. */
    uint32_t fl = spin_lock_irqsave(&state_lock);
    zorder[zcount++] = win;
    focused_win = win;
    spin_unlock_irqrestore(&state_lock, fl);
    vc_focus(win->vc);

    need_frame = 1;
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

    /* Wallpaper is rendered once; compose() only blits it. */
    gfx_vgradient(&wallsurf, 0, 0, wallsurf.w, wallsurf.h,
                  COL_WALL_TOP, COL_WALL_BOT);
    gfx_text(&wallsurf, wallsurf.w - 8 * GFX_GLYPH_W - 12,
             wallsurf.h - GFX_GLYPH_H - 8, "d-os M22", 0xFF9FB6C9u);

    spin_lock_init(&state_lock);
    mx = fbsurf.w / 2;
    my = fbsurf.h / 2;

    gui_active = 1;                     /* before window creation: emits OK */

    /* From here on, pane VCs must not paint (their shells keep running
     * silently in the background) and Alt-N is disabled. */
    vc_screen_suppress(1);

    /* Two staggered shell windows — the M22 DoD scene. */
    gui_window_create("shell 1",  96,  84, 520, 320);
    gui_window_create("shell 2", 380, 300, 520, 320);

    mouse_set_listener(gui_mouse);

    if (!task_spawn("compositor", gui_compositor_main)) {
        kprintf("gui: FATAL — compositor spawn failed\n");
        /* Windows/shells stay usable-ish via emits, but nothing will
         * reach the screen; undo the suppression so panes come back. */
        vc_screen_suppress(0);
        gui_active = 0;
        return -1;
    }

    kprintf("gui: up — %dx%d, %d windows\n", fbsurf.w, fbsurf.h, zcount);
    return 0;
}
