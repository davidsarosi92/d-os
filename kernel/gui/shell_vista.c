/* =============================================================================
 * shell_vista.c — the default (Vista-flavoured) desktop shell (M22.2).
 *
 * Extracted from gui.c during the modularity cut: taskbar (Start
 * button + one button per window + RTC clock) and the Start menu.
 * The launcher menu is built from the GUI_APP registry — apps appear
 * here by registering themselves, this file names none of them.
 * Only the session/power actions (Exit GUI / Reboot / Shut Down) are fixed
 * tail items.
 *
 * Threading: see desktop.h.  click/motion run in the mouse IRQ with
 * the WM lock held (hence the *_locked services); draw + second_tick
 * run on the compositor task (the RTC port I/O lives in the tick).
 * ============================================================================= */

#include "desktop.h"
#include "itemview.h"      /* §M64 — the layout is swappable, not hardcoded */
#include "shortcut.h"
#include "config.h"
#include "gui_app.h"
#include "gui_internal.h"
#include "gui.h"           /* gui_damage — the icon layer damages its own rects */
#include "widget.h"        /* WPTR_* — §M58's pointer phases, shared vocabulary */
#include "klog.h"          /* a drag must leave evidence on the serial log */
#include "gfx.h"
#include "rtc.h"
#include "keymap.h"
#include <stdint.h>
#include <stddef.h>

#define TASKBAR_H   34
#define START_W     74
#define TBTN_W      150
/* Clock panel: "YYYY-MM-DD  HH:MM:SS  XX" = 24 glyphs + padding.  The date and
 * the active keyboard layout live here because both are things you check by
 * glancing at the corner, not by running a command — and a wrong layout is the
 * single most confusing thing that can happen while typing. */
#define CLOCK_W     212
#define SM_W        210
#define SM_ITEM_H   26
/* §M63 — raised from 10.  The Control Panel made it eleven apps, and the cap
 * silently DROPS the overflow: the last registered app simply stops appearing
 * in the launcher, which looks like a broken registration rather than a full
 * menu.  Twelve is still a cap (a scrolling menu is a different feature), but
 * it is now above the number of apps that exist, and the failure mode is
 * written down instead of discovered. */
#define SM_MAX_APPS 12                  /* menu rows before the power tail */

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

/* -------------------------------------------------------------------------- */
/* §M64 — desktop icons.                                                       */
/*                                                                             */
/* The shell owns the SELECTION and the layout choice; the items themselves    */
/* come from shortcut.c and the arrangement from an ITEM_VIEW picked by name   */
/* (`desktop.view`).  That split is the whole point: switching this desktop to */
/* a list is a config value, not a code change here.                           */
/* -------------------------------------------------------------------------- */

#define ICONS_PAD   12                  /* inset from the screen edges */

static const struct item_view* iview = NULL;
static int icon_sel = -1;               /* selected shortcut, -1 = none */

/* The icon field's box on screen.  Full desktop area minus the taskbar. */
static void icons_box(int* x, int* y, int* w, int* h) {
    *x = ICONS_PAD;
    *y = ICONS_PAD;
    *w = scr_w - 2 * ICONS_PAD;
    *h = scr_h - TASKBAR_H - 2 * ICONS_PAD;
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

/* Damage exactly one icon's cell — used when the selection moves, so clicking
 * around the desktop costs two small rects rather than a full recompose
 * (§4.61 measured what a full-screen repaint costs). */
static void icons_damage_item(int idx) {
    if (idx < 0 || !iview || !iview->rect) return;
    int bx, by, bw, bh;
    icons_box(&bx, &by, &bw, &bh);
    int ox, oy, ow, oh;
    if (iview->rect(idx, bw, bh, shortcut_model(), 0, &ox, &oy, &ow, &oh) != 0)
        return;
    gui_damage(bx + ox, by + oy, ow, oh);
}

static int icon_last_n = -1;            /* to notice add/remove */

static void vista_draw_under(struct gfx_surface* back) {
    if (!iview || !iview->draw) return;
    const struct item_model* m = shortcut_model();

    /* Drop a selection that no longer means what it meant.  Indices are
     * positions, not identities, so after a delete the same number is a
     * DIFFERENT shortcut — highlighting it would be quietly wrong, and the
     * next Enter would open something the user did not point at.  Detected
     * here rather than pushed from the mutation path, so it stays true no
     * matter who changed the set. */
    int n = m->count ? m->count(m->ctx) : 0;
    if (n != icon_last_n) { icon_last_n = n; icon_sel = -1; }

    int bx, by, bw, bh;
    icons_box(&bx, &by, &bw, &bh);
    iview->draw(back, bx, by, bw, bh, m, icon_sel, 0);
}

static void vista_desktop_click(int x, int y, int dbl) {
    if (!iview || !iview->hit) return;
    int bx, by, bw, bh;
    icons_box(&bx, &by, &bw, &bh);
    int idx = iview->hit(x - bx, y - by, bw, bh, shortcut_model(), 0);

    if (idx != icon_sel) {
        int prev = icon_sel;
        icon_sel = idx;
        icons_damage_item(prev);
        icons_damage_item(idx);
    }
    /* Double-click activates.  We are on the desktop task with no lock held
     * (desktop.h), so the launch may do real work. */
    if (dbl && idx >= 0) shortcut_launch(idx);
}

/* ------------------------------------------------------------------------- */
/* §M64 tail — DRAG AN ICON TO A SLOT.                                        */
/*                                                                            */
/* The whole feature is three lines of state and one rule: remember what was   */
/* pressed, follow it while the button is down, and write the slot down when   */
/* the button comes up.  It could not exist before §M58 because a drag had no  */
/* transport at all — `widget_ops.mouse` carried click and double-click, and   */
/* nothing else.                                                              */
/* ------------------------------------------------------------------------- */

static int drag_idx  = -1;              /* shortcut being dragged, -1 = none */
static int drag_col  = -1, drag_row = -1;   /* slot last previewed */
static int drag_moved = 0;              /* did it ever leave its own slot?   */
/* Where it STARTED.  Kept because the live preview overwrites the item's
 * stored slot on every cell it crosses, and the drop needs the original to
 * hand to whatever it swaps with — otherwise the displaced icon inherits a
 * position from the middle of the gesture, which is a slot the user never
 * pointed at and cannot predict. */
static int drag_from_col = -1, drag_from_row = -1;

static void vista_desktop_pointer(int x, int y, int phase) {
    if (!iview || !iview->slot_at) return;   /* this layout cannot be arranged */

    int bx, by, bw, bh;
    icons_box(&bx, &by, &bw, &bh);
    int px = x - bx, py = y - by;

    if (phase == WPTR_PRESS) {
        drag_idx = iview->hit(px, py, bw, bh, shortcut_model(), 0);
        drag_col = drag_row = -1;
        drag_moved = 0;
        drag_from_col = drag_from_row = -1;
        if (drag_idx >= 0)
            shortcut_pos_of(drag_idx, &drag_from_col, &drag_from_row);
        return;
    }
    if (drag_idx < 0) return;                /* the press hit empty desktop */

    int col, row;
    if (iview->slot_at(px, py, bw, bh, &col, &row) != 0) {
        /* Dragged outside the icon field (over the taskbar, off the screen).
         * The last previewed slot stands — an icon that vanishes because the
         * pointer left the field would be a shortcut the user cannot find. */
        if (phase == WPTR_RELEASE) { drag_idx = -1; }
        return;
    }

    if (phase == WPTR_DRAG) {
        if (col == drag_col && row == drag_row) return;   /* same cell, no work */
        /* Move it in the MODEL and damage the two cells — this is the live
         * preview, and it costs two small rects rather than a recompose
         * (§4.61 measured what a full-screen repaint is worth).  The file is
         * NOT written here: a drag crosses a dozen cells and each one would be
         * a `.lnk` rewrite, i.e. VFS traffic proportional to hand tremor. */
        icons_damage_item(drag_idx);
        shortcut_set_pos_live(drag_idx, col, row);
        drag_col = col; drag_row = row;
        drag_moved = 1;
        icons_damage_item(drag_idx);
        gui_desktop_icons_changed();
        return;
    }

    if (phase == WPTR_RELEASE) {
        /* Persist exactly once, and only if it actually moved: a plain click
         * is a press and a release with nothing between them, and rewriting a
         * file on every click would make selecting an icon a disk write.
         *
         * Put it back where it started FIRST, so the commit sees the gesture's
         * real origin and the swap hands that slot to whatever was displaced. */
        if (drag_moved && drag_col >= 0) {
            shortcut_set_pos_live(drag_idx, drag_from_col, drag_from_row);
            shortcut_set_pos(drag_idx, drag_col, drag_row);
            gui_desktop_icons_changed();
            /* Say so on the log.  Without this the ONLY evidence a drag ever
             * happened is a screenshot, and a screenshot cannot distinguish an
             * icon that moved and was SAVED from one that moved and will be
             * back in its old slot at the next boot — which is exactly the bug
             * this milestone's tail turned out to contain. */
            klog(KLOG_INFO, "gui", "desktop: shortcut %d moved (%d,%d) -> (%d,%d)\n",
                 drag_idx, drag_from_col, drag_from_row, drag_col, drag_row);
        }
        drag_idx = -1;
        drag_col = drag_row = -1;
        drag_moved = 0;
    }
}



/* Menu state — written by click/motion (IRQ, WM lock held), read by
 * draw (compositor snapshot race is a benign one-frame lag). */
static int menu_open  = 0;
static int menu_hover = -1;

/* Clock cache — compositor-owned (second_tick + draw).  Holds the whole
 * "date  time  layout" string, so draw() stays a single gfx_text call. */
#define CLOCK_STR_MAX 32
static char clock_str[CLOCK_STR_MAX] = "";

/* -------------------------------------------------------------------------- */
/* Geometry helpers shared by draw + hit-test.                                 */
/* -------------------------------------------------------------------------- */

static int menu_rows(void) {
    int apps = gui_app_count();
    if (apps > SM_MAX_APPS) apps = SM_MAX_APPS;
    return apps + 3;                    /* + Exit GUI + Reboot + Shut Down */
}

static int menu_h(void)   { return menu_rows() * SM_ITEM_H + 12; }
static int menu_top(void) { return scr_h - TASKBAR_H - menu_h(); }

/* M22.7-B — tell the compositor the popup's on-screen rect so it composites
 * (and hit-routes) it while open.  Called whenever menu_open changes. */
static void publish_popup(void) {
    if (menu_open) gui_panel_set_popup(1, 4, menu_top(), SM_W, menu_h());
    else           gui_panel_set_popup(0, 0, 0, 0, 0);
}

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
    /* Three fixed tail items, in escalating order of what they end: the GUI
     * session, the kernel, the machine.  "Exit GUI" sits above Reboot because
     * leaving the desktop is the reversible one — `gui` at the shell brings it
     * straight back. */
    if (row == apps)     return "Exit GUI";
    if (row == apps + 1) return "Reboot";
    return "Shut Down";
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
    publish_popup();

    /* §M64 — the layout comes from config, so "I would like a list instead of
     * icons" is a setconf away and not a rewrite.  An unknown name falls back
     * to the first registered view rather than to nothing (itemview.c). */
    iview = item_view_by_name(config_get("desktop.view", "grid"));
    icon_sel = -1;
    shortcut_reload();
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
        int m = gui_window_minimized(slots[i]);
        gfx_vgradient(back, x, ty + 5, bw, TASKBAR_H - 10,
                      f ? COL_TBTN_F_TOP : (m ? COL_TB_BOT : COL_TBTN_TOP),
                      f ? COL_TBTN_F_BOT : (m ? COL_TB_BOT : COL_TBTN_BOT));
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
    int nh;
    if (x >= 4 && x < 4 + SM_W &&
        y >= myy + 6 && y < myy + 6 + menu_rows() * SM_ITEM_H)
        nh = (y - myy - 6) / SM_ITEM_H;
    else
        nh = -1;
    if (nh != menu_hover) {
        menu_hover = nh;
        gui_panel_dirty();          /* chrome-only repaint (M22.7 — was a
                                     * full recompose per motion: the lag) */
    }
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
                gui_queue_exit();               /* back to the text shell */
            else if (idx == apps + 1)
                gui_queue_power(1);
            else if (idx == apps + 2)
                gui_queue_power(0);
            menu_open = 0;
            publish_popup();
            return 1;
        }
        /* Click elsewhere just closes the menu; windows still get it. */
        menu_open = 0;
        publish_popup();
        gui_request_frame();
    }

    if (y < ty) return 0;               /* not our chrome */

    /* Start button. */
    if (x >= 4 && x < 4 + START_W) {
        menu_open = !menu_open;
        menu_hover = -1;
        publish_popup();
        return 1;
    }

    /* Window buttons. */
    struct gui_window* slots[TB_MAX_BTNS];
    int n  = gui_wm_windows_locked(slots, TB_MAX_BTNS);
    int bw = tbtn_width(n);
    int bx = START_W + 12;
    for (int i = 0; i < n; i++) {
        if (x >= bx && x < bx + bw) {
            gui_wm_taskbar_activate_locked(slots[i]);   /* M22.3 */
            return 1;
        }
        bx += bw + 6;
    }
    return 1;                           /* dead taskbar area still consumed */
}

/* Two digits, zero-padded, at s[p]; returns the next write position. */
static int put2(char* s, int p, unsigned v) {
    s[p]     = (char)('0' + (v / 10) % 10);
    s[p + 1] = (char)('0' + v % 10);
    return p + 2;
}

static int vista_second_tick(void) {
    struct rtc_time t;
    if (rtc_read(&t) != 0) return 0;

    /* "YYYY-MM-DD  HH:MM:SS  XX" — ISO date (unambiguous in every locale),
     * wall clock, then the active keyboard layout as an upper-case ISO code. */
    char s[CLOCK_STR_MAX];
    int p = 0;
    p = put2(s, p, (unsigned)(t.year / 100));
    p = put2(s, p, (unsigned)(t.year % 100));
    s[p++] = '-'; p = put2(s, p, t.month);
    s[p++] = '-'; p = put2(s, p, t.day);
    s[p++] = ' '; s[p++] = ' ';
    p = put2(s, p, t.hour); s[p++] = ':';
    p = put2(s, p, t.min);  s[p++] = ':';
    p = put2(s, p, t.sec);

    const char* kb = keymap_current();
    if (kb && kb[0]) {
        s[p++] = ' '; s[p++] = ' ';
        for (int i = 0; kb[i] && p < CLOCK_STR_MAX - 1; i++) {
            char c = kb[i];
            s[p++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
    }
    s[p] = 0;

    /* Repaint only on an actual change — the tick fires every second but the
     * layout (and, most seconds, the date) has not moved. */
    for (int i = 0; i <= p; i++) {
        if (clock_str[i] != s[i]) {
            for (int j = 0; j <= p; j++) clock_str[j] = s[j];
            return 1;
        }
    }
    return 0;
}

DESKTOP_SHELL(vista) = {
    .name           = "vista",
    .init           = vista_init,
    .bottom_reserve = vista_bottom_reserve,
    .draw           = vista_draw,
    .draw_under     = vista_draw_under,        /* §M64 — icons under windows */
    .click          = vista_click,
    .motion         = vista_motion,
    .desktop_click  = vista_desktop_click,
    .desktop_pointer = vista_desktop_pointer, /* §M64 tail — drag an icon */
    .second_tick    = vista_second_tick,
};
