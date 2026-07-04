/* =============================================================================
 * shell_vista.c — the default (Vista-flavoured) desktop shell (M22.2).
 *
 * Extracted from gui.c during the modularity cut: taskbar (Start
 * button + one button per window + RTC clock) and the Start menu.
 * The launcher menu is built from the GUI_APP registry — apps appear
 * here by registering themselves, this file names none of them.
 * Only the power actions (Reboot / Shut Down) are fixed tail items.
 *
 * Threading: see desktop.h.  click/motion run in the mouse IRQ with
 * the WM lock held (hence the *_locked services); draw + second_tick
 * run on the compositor task (the RTC port I/O lives in the tick).
 * ============================================================================= */

#include "desktop.h"
#include "gui_app.h"
#include "gui_internal.h"
#include "gfx.h"
#include "rtc.h"
#include <stdint.h>
#include <stddef.h>

#define TASKBAR_H   34
#define START_W     74
#define TBTN_W      150
#define CLOCK_W     78
#define SM_W        210
#define SM_ITEM_H   26
#define SM_MAX_APPS 10                  /* menu rows before the power tail */

#define COL_TB_TOP      0xFF4B5A70u
#define COL_TB_BOT      0xFF222A37u
#define COL_TB_HILITE   0xFF7C8CA4u
#define COL_START_TOP   0xFF58A84Bu
#define COL_START_BOT   0xFF2C6626u
#define COL_START_EDGE  0xFF83C877u
#define COL_TBTN_TOP    0xFF39465Au
#define COL_TBTN_BOT    0xFF2A3444u
#define COL_TBTN_F_TOP  0xFF5A83C0u
#define COL_TBTN_F_BOT  0xFF39598Cu
#define COL_TBTN_EDGE   0xFF55647Cu
#define COL_SM_BG       0xFF2B3546u
#define COL_SM_EDGE     0xFF55647Cu
#define COL_SM_HOVER    0xFF3D5C92u
#define COL_TEXT        0xFFF2F5FAu
#define COL_SHADOW      0x48000000u
#define COL_SEP         0xFF4A576Au

#define TB_MAX_BTNS 8

static int scr_w = 0, scr_h = 0;

/* Menu state — written by click/motion (IRQ, WM lock held), read by
 * draw (compositor snapshot race is a benign one-frame lag). */
static int menu_open  = 0;
static int menu_hover = -1;

/* Clock cache — compositor-owned (second_tick + draw). */
static char clock_str[12] = "";

/* -------------------------------------------------------------------------- */
/* Geometry helpers shared by draw + hit-test.                                 */
/* -------------------------------------------------------------------------- */

static int menu_rows(void) {
    int apps = gui_app_count();
    if (apps > SM_MAX_APPS) apps = SM_MAX_APPS;
    return apps + 2;                    /* + Reboot + Shut Down */
}

static int menu_h(void)   { return menu_rows() * SM_ITEM_H + 12; }
static int menu_top(void) { return scr_h - TASKBAR_H - menu_h(); }

static int tbtn_width(int nslots) {
    int avail = scr_w - (START_W + 12) - CLOCK_W - 8;
    if (nslots <= 0) return TBTN_W;
    int w = avail / nslots - 6;
    if (w > TBTN_W) w = TBTN_W;
    if (w < 48)     w = 48;
    return w;
}

/* Label for a menu row: an app name, or a power tail item. */
static const char* menu_label(int row) {
    int apps = gui_app_count();
    if (apps > SM_MAX_APPS) apps = SM_MAX_APPS;
    if (row < apps) {
        const struct gui_app_def* a = gui_app_at(row);
        return a ? a->name : "?";
    }
    return (row == apps) ? "Reboot" : "Shut Down";
}

/* -------------------------------------------------------------------------- */
/* desktop_shell callbacks.                                                    */
/* -------------------------------------------------------------------------- */

static void vista_init(int w, int h) {
    scr_w = w;
    scr_h = h;
    menu_open = 0;
    menu_hover = -1;
    clock_str[0] = 0;
}

static int vista_bottom_reserve(void) { return TASKBAR_H; }

static void vista_draw(struct gfx_surface* back) {
    int ty = scr_h - TASKBAR_H;

    gfx_vgradient(back, 0, ty, scr_w, TASKBAR_H, COL_TB_TOP, COL_TB_BOT);
    gfx_fill(back, 0, ty, scr_w, 1, COL_TB_HILITE);

    /* Start button. */
    gfx_vgradient(back, 4, ty + 4, START_W, TASKBAR_H - 8,
                  menu_open ? COL_START_EDGE : COL_START_TOP, COL_START_BOT);
    gfx_fill(back, 4, ty + 4, START_W, 1, COL_START_EDGE);
    gfx_fill(back, 4, ty + TASKBAR_H - 5, START_W, 1, COL_START_EDGE);
    gfx_fill(back, 4, ty + 4, 1, TASKBAR_H - 8, COL_START_EDGE);
    gfx_fill(back, 4 + START_W - 1, ty + 4, 1, TASKBAR_H - 8, COL_START_EDGE);
    gfx_text(back, 4 + (START_W - 5 * GFX_GLYPH_W) / 2,
             ty + (TASKBAR_H - GFX_GLYPH_H) / 2, "Start", COL_TEXT);

    /* One button per open window. */
    struct gui_window* slots[TB_MAX_BTNS];
    int n  = gui_wm_windows(slots, TB_MAX_BTNS);
    int bw = tbtn_width(n);
    int x  = START_W + 12;
    struct gui_window* focused = gui_wm_focused();
    for (int i = 0; i < n; i++) {
        int f = (slots[i] == focused);
        gfx_vgradient(back, x, ty + 5, bw, TASKBAR_H - 10,
                      f ? COL_TBTN_F_TOP : COL_TBTN_TOP,
                      f ? COL_TBTN_F_BOT : COL_TBTN_BOT);
        gfx_fill(back, x, ty + 5, bw, 1, COL_TBTN_EDGE);
        gfx_fill(back, x, ty + TASKBAR_H - 6, bw, 1, COL_TBTN_EDGE);
        gfx_fill(back, x, ty + 5, 1, TASKBAR_H - 10, COL_TBTN_EDGE);
        gfx_fill(back, x + bw - 1, ty + 5, 1, TASKBAR_H - 10, COL_TBTN_EDGE);

        char t[20];
        const char* title = gui_window_title(slots[i]);
        int maxch = (bw - 12) / GFX_GLYPH_W;
        if (maxch > (int)sizeof(t) - 1) maxch = (int)sizeof(t) - 1;
        int k = 0;
        for (; title[k] && k < maxch; k++) t[k] = title[k];
        t[k] = 0;
        gfx_text(back, x + 6, ty + (TASKBAR_H - GFX_GLYPH_H) / 2, t, COL_TEXT);
        x += bw + 6;
    }

    /* Clock. */
    int cx = scr_w - CLOCK_W;
    gfx_fill(back, cx - 1, ty + 6, 1, TASKBAR_H - 12, 0xFF141B26u);
    gfx_fill(back, cx,     ty + 6, 1, TASKBAR_H - 12, COL_TB_HILITE);
    if (clock_str[0]) {
        int len = 0;
        while (clock_str[len]) len++;
        gfx_text(back, cx + (CLOCK_W - len * GFX_GLYPH_W) / 2,
                 ty + (TASKBAR_H - GFX_GLYPH_H) / 2, clock_str, COL_TEXT);
    }

    /* Start menu overlay. */
    if (menu_open) {
        int mh = menu_h(), myy = menu_top();
        int apps = gui_app_count();
        if (apps > SM_MAX_APPS) apps = SM_MAX_APPS;

        gfx_blend_fill(back, 8, myy + 4, SM_W, mh, COL_SHADOW);
        gfx_fill(back, 4, myy, SM_W, mh, COL_SM_BG);
        gfx_fill(back, 4, myy, SM_W, 1, COL_SM_EDGE);
        gfx_fill(back, 4, myy + mh - 1, SM_W, 1, COL_SM_EDGE);
        gfx_fill(back, 4, myy, 1, mh, COL_SM_EDGE);
        gfx_fill(back, 4 + SM_W - 1, myy, 1, mh, COL_SM_EDGE);

        for (int i = 0; i < menu_rows(); i++) {
            int iy = myy + 6 + i * SM_ITEM_H;
            if (i == apps)               /* separator above the power tail */
                gfx_fill(back, 10, iy - 1, SM_W - 12, 1, COL_SEP);
            if (i == menu_hover)
                gfx_fill(back, 6, iy, SM_W - 4, SM_ITEM_H, COL_SM_HOVER);
            gfx_text(back, 18, iy + (SM_ITEM_H - GFX_GLYPH_H) / 2,
                     menu_label(i), COL_TEXT);
        }
    }
}

static void vista_motion(int x, int y) {
    if (!menu_open) return;
    int myy = menu_top();
    if (x >= 4 && x < 4 + SM_W &&
        y >= myy + 6 && y < myy + 6 + menu_rows() * SM_ITEM_H)
        menu_hover = (y - myy - 6) / SM_ITEM_H;
    else
        menu_hover = -1;
}

static int vista_click(int x, int y) {
    int ty = scr_h - TASKBAR_H;

    /* Open menu gets first pick. */
    if (menu_open) {
        int myy = menu_top();
        if (x >= 4 && x < 4 + SM_W && y >= myy && y < ty) {
            int idx = (y - myy - 6) / SM_ITEM_H;
            int apps = gui_app_count();
            if (apps > SM_MAX_APPS) apps = SM_MAX_APPS;
            if (idx >= 0 && idx < apps)
                gui_queue_launch(gui_app_at(idx));
            else if (idx == apps)
                gui_queue_power(1);
            else if (idx == apps + 1)
                gui_queue_power(0);
            menu_open = 0;
            return 1;
        }
        /* Click elsewhere just closes the menu; windows still get it. */
        menu_open = 0;
        gui_request_frame();
    }

    if (y < ty) return 0;               /* not our chrome */

    /* Start button. */
    if (x >= 4 && x < 4 + START_W) {
        menu_open = !menu_open;
        menu_hover = -1;
        return 1;
    }

    /* Window buttons. */
    struct gui_window* slots[TB_MAX_BTNS];
    int n  = gui_wm_windows_locked(slots, TB_MAX_BTNS);
    int bw = tbtn_width(n);
    int bx = START_W + 12;
    for (int i = 0; i < n; i++) {
        if (x >= bx && x < bx + bw) {
            gui_wm_focus_raise_locked(slots[i]);
            return 1;
        }
        bx += bw + 6;
    }
    return 1;                           /* dead taskbar area still consumed */
}

static int vista_second_tick(void) {
    struct rtc_time t;
    if (rtc_read(&t) != 0) return 0;
    char s[12];
    s[0] = (char)('0' + t.hour / 10);  s[1] = (char)('0' + t.hour % 10);
    s[2] = ':';
    s[3] = (char)('0' + t.min / 10);   s[4] = (char)('0' + t.min % 10);
    s[5] = ':';
    s[6] = (char)('0' + t.sec / 10);   s[7] = (char)('0' + t.sec % 10);
    s[8] = 0;
    for (int i = 0; i < 9; i++) {
        if (clock_str[i] != s[i]) {
            for (int j = 0; j < 9; j++) clock_str[j] = s[j];
            return 1;                   /* changed → repaint */
        }
    }
    return 0;
}

DESKTOP_SHELL(vista) = {
    .name           = "vista",
    .init           = vista_init,
    .bottom_reserve = vista_bottom_reserve,
    .draw           = vista_draw,
    .click          = vista_click,
    .motion         = vista_motion,
    .second_tick    = vista_second_tick,
};
