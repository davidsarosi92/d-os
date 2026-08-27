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
#include "icons.h"         /* ICON_APP — the item_entry's default glyph */
#include "audio.h"         /* §M23 — the taskbar sound indicator */
#include "gfx.h"
#include "rtc.h"
#include "keymap.h"
#include <stdint.h>
#include <stddef.h>

#define TASKBAR_H   34
#define START_W     74
#define TBTN_W      150
/* Clock panel: "YYYY-MM-DD  HH:MM:SS" = 20 glyphs + padding.
 *
 * THE LAYOUT USED TO BE APPENDED HERE and is not any more: it has its own
 * indicator now (below), and a fact displayed in two places is two things that
 * can drift.  The same argument the sound work made — the taskbar control and
 * the Control Panel page write the SAME keys, so there is one answer to "what
 * is the volume" rather than two. */
#define CLOCK_W     180
/* §M67 tail — the keyboard-layout indicator, immediately LEFT of the sound one.
 * Same shape and the same rules: always drawn, one flyout at a time, the
 * geometry in one place so draw and hit-test cannot disagree.
 *
 * It is wider than the sound button because it shows the layout's NAME rather
 * than a state: an icon alone would say "this is about the keyboard" and leave
 * the one question the indicator exists to answer — *which* layout — unanswered,
 * and a wrong layout is the single most confusing thing that can happen while
 * typing. */
#define KBD_W       52
#define KBD_ICON    20
#define KBDPOP_W    150
#define KBDPOP_ROW  22
#define KBD_MAX     8            /* layouts the flyout will show; see kbd_names */
/* §M23 — the sound indicator, immediately left of the clock.  Square, so the
 * icon renderer gets the box it expects. */
#define VOL_W       28
#define VOL_ICON    20
/* The volume popup: a slider and a mute row.  Deliberately small — this is a
 * status indicator's flyout, not a settings page.  The Sound page in the
 * Control Panel is where the full set lives (§M63 renders it from the
 * CONFIG_KEY descriptors with no per-key UI code). */
#define VOLPOP_W    180
#define VOLPOP_H    76
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

static void icon_select(int idx);       /* §M64 tail — defined with the keys */

static void vista_desktop_click(int x, int y, int dbl) {
    if (!iview || !iview->hit) return;
    int bx, by, bw, bh;
    icons_box(&bx, &by, &bw, &bh);
    int idx = iview->hit(x - bx, y - by, bw, bh, shortcut_model(), 0);

    /* One selection routine for both input paths — two copies would be two
     * chances for the mouse and the keyboard to disagree about what is
     * highlighted, and only one of them would be on screen. */
    icon_select(idx);
    /* Double-click activates.  We are on the desktop task with no lock held
     * (desktop.h), so the launch may do real work. */
    if (dbl && idx >= 0) shortcut_launch(idx);
}

/* ------------------------------------------------------------------------- */
/* §M64 tail — KEYBOARD NAVIGATION.                                           */
/*                                                                            */
/* "The icon to the right" used to be "the next index", and with explicit      */
/* slots it is not: item 5 may sit left of item 2.  So the neighbour is found  */
/* GEOMETRICALLY, from the same rectangles the view draws — one source of      */
/* truth again, and it works unchanged for the list view, where the geometry   */
/* happens to agree with the order.                                            */
/* ------------------------------------------------------------------------- */

static int icon_rect_of(int i, int* cx, int* cy) {
    int bx, by, bw, bh;
    icons_box(&bx, &by, &bw, &bh);
    int ox, oy, ow, oh;
    if (!iview || !iview->rect ||
        iview->rect(i, bw, bh, shortcut_model(), 0, &ox, &oy, &ow, &oh) != 0)
        return -1;
    *cx = ox + ow / 2;
    *cy = oy + oh / 2;
    return 0;
}

/* The nearest item strictly in direction (dx,dy).  The perpendicular offset is
 * weighted four times the parallel one, so "right" prefers the same row and
 * only falls to another row when that row has run out — which is what a person
 * means by the word. */
static int icon_neighbour(int from, int dx, int dy) {
    const struct item_model* m = shortcut_model();
    int n = m->count ? m->count(m->ctx) : 0;
    int fx, fy;
    if (from < 0 || icon_rect_of(from, &fx, &fy) != 0) return n ? 0 : -1;

    int best = -1, best_score = 0;
    for (int i = 0; i < n; i++) {
        if (i == from) continue;
        int cx, cy;
        if (icon_rect_of(i, &cx, &cy) != 0) continue;
        int along = (cx - fx) * dx + (cy - fy) * dy;
        if (along <= 0) continue;                  /* not in that direction */
        int perp  = (cx - fx) * dy + (cy - fy) * dx;
        if (perp < 0) perp = -perp;
        int score = along + 4 * perp;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

static void icon_select(int idx) {
    if (idx == icon_sel) return;
    int prev = icon_sel;
    icon_sel = idx;
    icons_damage_item(prev);
    icons_damage_item(idx);
    gui_desktop_icons_changed();
    /* Selecting something is what gives the desktop the keyboard; clearing it
     * hands Enter and Escape back to the console behind us. */
    gui_desktop_focus(idx >= 0);

    /* Which icon is selected is otherwise INVISIBLE without a screenshot, and
     * a screenshot cannot be taken on the arch with no display device at all
     * (§M60's reason for `wallpaper check`).  One line per deliberate user
     * action, so the keyboard path and the mouse path are both observable on
     * the serial log. */
    struct item_entry e = { 0, 0, ICON_APP, 0 };
    const struct item_model* m = shortcut_model();
    if (idx >= 0 && m->get && m->get(m->ctx, idx, &e) == 0)
        klog(KLOG_INFO, "gui", "desktop: selected %d (%s)\n", idx, e.label);
    else
        klog(KLOG_INFO, "gui", "desktop: selection cleared\n");
}

/* Move the selection one step, or start it.  A search that finds nothing must
 * NOT clear the selection: at the edge of the field the honest answer to "move
 * right" is "stay", and an icon that vanishes from under the arrow keys is how
 * a keyboard user loses their place. */
static void icon_move(int dx, int dy) {
    if (icon_sel < 0) { icon_select(0); return; }
    int nb = icon_neighbour(icon_sel, dx, dy);
    if (nb >= 0) icon_select(nb);
}

static void vista_desktop_key(int keycode, int mods) {
    (void)mods;
    const struct item_model* m = shortcut_model();
    int n = m->count ? m->count(m->ctx) : 0;
    if (!n) return;

    switch (keycode) {
    case KC_RIGHT: icon_move( 1,  0); break;
    case KC_LEFT:  icon_move(-1,  0); break;
    case KC_DOWN:  icon_move( 0,  1); break;
    case KC_UP:    icon_move( 0, -1); break;
    case KC_HOME:  icon_select(0);     break;
    case KC_END:   icon_select(n - 1); break;
    case KC_ESC:   icon_select(-1);    break;
    case KC_ENTER:
        /* The desktop task, no lock (desktop.h) — so this may spawn. */
        if (icon_sel >= 0 && icon_sel < n) shortcut_launch(icon_sel);
        break;
    default: break;
    }
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

/* §M23 — sound indicator + its flyout. */
static int vol_pop_open = 0;
/* §M67 tail — keyboard indicator + its flyout. */
static int kbd_pop_open = 0;

/* The button's box, so draw and hit-test cannot disagree about where it is. */
static void vol_box(int* x, int* y, int* w, int* h) {
    *x = scr_w - CLOCK_W - VOL_W;
    *y = scr_h - TASKBAR_H + (TASKBAR_H - VOL_ICON) / 2;
    *w = VOL_W;
    *h = VOL_ICON;
}

/* Which of the THREE icons applies right now.  The distinction is the whole
 * point of the control: a machine with no working audio shows a different
 * glyph from one the user silenced, so "I muted it" and "it is broken" are
 * never the same picture. */
static int vol_icon_id(void) {
    if (!audio_available()) return ICON_VOLUME_OFF;
    int vol, muted;
    audio_master_get(&vol, &muted);
    return (muted || vol == 0) ? ICON_VOLUME_MUTED : ICON_VOLUME;
}

/* The registered layouts, collected once per flyout open.
 *
 * COLLECTED RATHER THAN QUERIED PER ROW: `keymap_for_each` is a callback walk,
 * and calling it from inside draw AND from inside the hit test would be two
 * walks that could disagree about row order the moment a layout is registered
 * between them.  One snapshot, read by both. */
static const char* kbd_names[KBD_MAX];
static int         kbd_count;

static void kbd_collect_one(const struct kbd_layout* l, void* ctx) {
    (void)ctx;
    if (kbd_count < KBD_MAX && l && l->name) kbd_names[kbd_count++] = l->name;
}

static void kbd_collect(void) {
    kbd_count = 0;
    keymap_for_each(kbd_collect_one, NULL);
}

/* The button's box — the same one-place rule as vol_box. */
static void kbd_box(int* x, int* y, int* w, int* h) {
    *x = scr_w - CLOCK_W - VOL_W - KBD_W;
    *y = scr_h - TASKBAR_H + (TASKBAR_H - KBD_ICON) / 2;
    *w = KBD_W;
    *h = KBD_ICON;
}

static int kbdpop_h(void) {
    int rows = kbd_count > 0 ? kbd_count : 1;
    return rows * KBDPOP_ROW + 26;          /* + title row + padding */
}
static int kbdpop_x(void) {
    int x = scr_w - CLOCK_W - VOL_W - KBDPOP_W;
    if (x < 0) x = 0;
    return x;
}
static int kbdpop_y(void) { return scr_h - TASKBAR_H - kbdpop_h(); }

static int volpop_x(void) {
    int x = scr_w - CLOCK_W - VOLPOP_W;
    if (x < 0) x = 0;
    return x;
}
static int volpop_y(void) { return scr_h - TASKBAR_H - VOLPOP_H; }

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
/* Tell the compositor which chrome popup is open, so it composites that rect
 * on top of the windows and routes clicks inside it here.  BOTH popups go
 * through this one function — a second publisher would be a second thing that
 * can forget to clear the extent, and a stale extent swallows clicks over a
 * window for reasons nothing on screen explains. */
static void publish_popup(void) {
    if (menu_open)         gui_panel_set_popup(1, 4, menu_top(), SM_W, menu_h());
    else if (vol_pop_open) gui_panel_set_popup(1, volpop_x(), volpop_y(),
                                               VOLPOP_W, VOLPOP_H);
    else if (kbd_pop_open) gui_panel_set_popup(1, kbdpop_x(), kbdpop_y(),
                                               KBDPOP_W, kbdpop_h());
    else                   gui_panel_set_popup(0, 0, 0, 0, 0);
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
    vol_pop_open = 0;
    kbd_pop_open = 0;
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

    /* §M23 — the sound indicator.  ALWAYS drawn, including when audio is
     * unavailable: a control that disappears when the subsystem fails leaves
     * the user with nothing to point at, and "there is no icon" is not a
     * diagnosis.  That is the same argument §M46 made for chrome that keeps
     * working while an app is wedged. */
    {
        int vx, vy, vw, vh;
        vol_box(&vx, &vy, &vw, &vh);
        icon_draw(back, vx + (vw - VOL_ICON) / 2, vy, VOL_ICON, vol_icon_id());
    }

    /* §M67 tail — the keyboard indicator: icon + the active layout's name.
     * ALWAYS DRAWN, for the reason the sound button is: a control that
     * disappears leaves nothing to point at.  The name is upper-cased because
     * that is how every other system writes a layout code, and because two
     * upper-case glyphs are distinguishable at a glance in a way lower-case
     * ones are not. */
    {
        int kx, ky, kw, kh;
        kbd_box(&kx, &ky, &kw, &kh);
        icon_draw(back, kx + 2, ky, KBD_ICON, ICON_KEYBOARD);
        const char* kb = keymap_current();
        if (kb && kb[0]) {
            char up[4];
            int i = 0;
            for (; kb[i] && i < 3; i++) {
                char c = kb[i];
                up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }
            up[i] = 0;
            gfx_text(back, kx + KBD_ICON + 4,
                     ty + (TASKBAR_H - GFX_GLYPH_H) / 2, up, COL_TEXT);
        }
    }

    /* The keyboard flyout — one row per registered layout, the active one
     * marked.  A LIST, not a cycle button: with two layouts a cycle is
     * indistinguishable from a choice, and with three it becomes a guessing
     * game about what comes next. */
    if (kbd_pop_open) {
        int px = kbdpop_x(), py = kbdpop_y(), ph = kbdpop_h();
        gfx_fill(back, px, py, KBDPOP_W, ph, COL_SM_BG);
        gfx_fill(back, px, py, KBDPOP_W, 1, COL_TB_HILITE);
        gfx_fill(back, px, py, 1, ph, COL_TB_HILITE);
        gfx_fill(back, px + KBDPOP_W - 1, py, 1, ph, 0xFF141B26u);

        gfx_text(back, px + 10, py + 8, "Keyboard layout", COL_TB_HILITE);
        gfx_fill(back, px + 8, py + 22, KBDPOP_W - 16, 1, COL_SEP);

        const char* cur = keymap_current();
        for (int i = 0; i < kbd_count; i++) {
            int iy = py + 26 + i * KBDPOP_ROW;
            int active = cur && kbd_names[i] &&
                         cur[0] == kbd_names[i][0] && cur[1] == kbd_names[i][1];
            if (active) gfx_fill(back, px + 4, iy - 2, KBDPOP_W - 8, KBDPOP_ROW,
                                 COL_SM_HOVER);
            /* The marker is a GLYPH, not just the highlight: a selection shown
             * only by a background colour is invisible in a screenshot taken
             * for a bug report, and this project's tests read pixels. */
            gfx_text(back, px + 10, iy + 2, active ? "*" : " ", COL_TEXT);
            gfx_text(back, px + 24, iy + 2, kbd_names[i], COL_TEXT);
        }
        if (kbd_count == 0)
            gfx_text(back, px + 10, py + 28, "no layouts", COL_TB_HILITE);
    }

    /* §M23 — the volume flyout. */
    if (vol_pop_open) {
        int px = volpop_x(), py = volpop_y();
        gfx_fill(back, px, py, VOLPOP_W, VOLPOP_H, COL_SM_BG);
        gfx_fill(back, px, py, VOLPOP_W, 1, COL_TB_HILITE);
        gfx_fill(back, px, py, 1, VOLPOP_H, COL_TB_HILITE);
        gfx_fill(back, px + VOLPOP_W - 1, py, 1, VOLPOP_H, 0xFF141B26u);

        int vol, muted;
        audio_master_get(&vol, &muted);
        int pct = (vol * 100 + 128) / 256;

        if (!audio_available()) {
            gfx_text(back, px + 10, py + 12, "No audio device", COL_TEXT);
            gfx_text(back, px + 10, py + 30, "nothing to play through", COL_TB_HILITE);
        } else {
            char line[24];
            int n = 0;
            const char* lbl = "Volume ";
            for (int i = 0; lbl[i]; i++) line[n++] = lbl[i];
            if (pct >= 100) { line[n++] = '1'; line[n++] = '0'; line[n++] = '0'; }
            else if (pct >= 10) { line[n++] = (char)('0' + pct / 10); line[n++] = (char)('0' + pct % 10); }
            else line[n++] = (char)('0' + pct);
            line[n++] = '%'; line[n] = 0;
            gfx_text(back, px + 10, py + 10, line, COL_TEXT);

            /* The slider: a track and a filled portion.  Drawn from the SAME
             * geometry the click handler reads back (vol_track_*), so the
             * knob cannot end up somewhere the click does not land. */
            int tx = px + 10, tw = VOLPOP_W - 20, tyy = py + 30;
            gfx_fill(back, tx, tyy, tw, 6, 0xFF1B2434u);
            int fill = muted ? 0 : (tw * pct) / 100;
            gfx_fill(back, tx, tyy, fill, 6, muted ? 0xFF556070u : 0xFF4C8BE0u);
            gfx_fill(back, tx + (fill ? fill - 2 : 0), tyy - 3, 4, 12,
                     muted ? 0xFF8B94A6u : COL_TEXT);

            gfx_text(back, px + 10, py + 52, muted ? "[ Unmute ]" : "[ Mute ]", COL_TEXT);
        }
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

/* The slider's track, in ONE place: the drawing reads it and so does the hit
 * test.  Two copies of this arithmetic is how a slider ends up looking right
 * and responding at the wrong offset. */
static void vol_track(int* tx, int* ty_, int* tw) {
    *tx  = volpop_x() + 10;
    *tw  = VOLPOP_W - 20;
    *ty_ = volpop_y() + 30;
}

/* Set the level from a point on the track, and remember it. */
static void vol_set_from_x(int x) {
    int tx, tyy, tw;
    vol_track(&tx, &tyy, &tw);
    int pct = tw > 0 ? ((x - tx) * 100) / tw : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int vol, muted;
    audio_master_get(&vol, &muted);
    /* Dragging the slider UNMUTES: reaching for the volume is unambiguous
     * about what the user wants, and leaving it muted would look like the
     * control does nothing. */
    audio_master_set((pct * 256 + 50) / 100, 0);
    (void)vol; (void)muted;
    audio_volume_persist();
}

static int vista_click(int x, int y) {
    int ty = scr_h - TASKBAR_H;

    /* §M23 — an open volume flyout owns the next click, wherever it lands
     * (§M65's popup rule): inside it is a choice, outside it is a dismissal,
     * and in both cases it must not also reach whatever is underneath. */
    if (vol_pop_open) {
        int px = volpop_x(), py = volpop_y();
        int inside = (x >= px && x < px + VOLPOP_W && y >= py && y < py + VOLPOP_H);
        if (inside && audio_available()) {
            int tx, tyy, tw;
            vol_track(&tx, &tyy, &tw);
            if (y >= tyy - 6 && y <= tyy + 12) {
                vol_set_from_x(x);
                gui_request_frame();
                return 1;                       /* stay open: allow re-aiming */
            }
            if (y >= py + 48 && y <= py + 64) { /* the Mute / Unmute row */
                int vol, muted;
                audio_master_get(&vol, &muted);
                audio_master_set(vol, !muted);
                audio_volume_persist();
                gui_request_frame();
                return 1;
            }
        }
        vol_pop_open = 0;
        publish_popup();
        gui_request_frame();
        if (inside) return 1;
        /* fall through: a click outside only dismissed the flyout */
    }

    /* §M67 tail — the keyboard flyout, under the SAME rule as the sound one:
     * while it is open it owns the next click wherever that lands, and must
     * not also reach the window underneath (§M65's popup rule). */
    if (kbd_pop_open) {
        int px = kbdpop_x(), py = kbdpop_y(), ph = kbdpop_h();
        int inside = (x >= px && x < px + KBDPOP_W && y >= py && y < py + ph);
        if (inside) {
            int idx = (y - (py + 26)) / KBDPOP_ROW;
            if (idx >= 0 && idx < kbd_count && kbd_names[idx]) {
                /* config_apply, NOT keymap_select.
                 *
                 * The difference is the whole reason §M63 stage 0 exists.
                 * `keymap_select` changes the live layout and nothing else, so
                 * the setting would revert at the next boot while the panel and
                 * the store both said otherwise.  `config_apply` records the
                 * decision and NOTIFIES — the keymap watcher does the actual
                 * switch — so this control, `setlayout`, and the Control
                 * Panel's Region page all go through one path and cannot
                 * disagree about what the layout is. */
                config_apply("keyboard.layout", kbd_names[idx]);
                kbd_pop_open = 0;
                publish_popup();
                gui_request_frame();
                return 1;
            }
        }
        kbd_pop_open = 0;
        publish_popup();
        gui_request_frame();
        if (inside) return 1;
        /* fall through: a click outside only dismissed the flyout */
    }

    /* The sound button itself. */
    {
        int vx, vy, vw, vh;
        vol_box(&vx, &vy, &vw, &vh);
        if (x >= vx && x < vx + vw && y >= scr_h - TASKBAR_H) {
            vol_pop_open = !vol_pop_open;
            kbd_pop_open = 0;
            menu_open = 0;                       /* one popup at a time */
            publish_popup();
            gui_request_frame();
            return 1;
        }
    }

    /* The keyboard button itself. */
    {
        int kx, ky, kw, kh;
        kbd_box(&kx, &ky, &kw, &kh);
        if (x >= kx && x < kx + kw && y >= scr_h - TASKBAR_H) {
            kbd_pop_open = !kbd_pop_open;
            if (kbd_pop_open) kbd_collect();   /* snapshot for draw + hit test */
            vol_pop_open = 0;
            menu_open = 0;
            publish_popup();
            gui_request_frame();
            return 1;
        }
    }

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
        vol_pop_open = 0;
        kbd_pop_open = 0;
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

    /* "YYYY-MM-DD  HH:MM:SS" — ISO date (unambiguous in every locale) and the
     * wall clock.  The keyboard layout USED to be appended here and now has its
     * own indicator with a flyout: showing it in two places would be two things
     * that can drift, and only one of them can be clicked. */
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

    s[p] = 0;

    /* Repaint only on an actual change — the tick fires every second but most
     * seconds the date has not moved. */
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
    .desktop_key    = vista_desktop_key,      /* §M64 tail — arrows + Enter */
    .second_tick    = vista_second_tick,
};
