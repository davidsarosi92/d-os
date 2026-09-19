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
#include "gui.h"
#include "cred.h"
#include "console_plate.h"
#include "locale.h"           /* gui_damage — the icon layer damages its own rects */
#include "widget.h"        /* WPTR_* — §M58's pointer phases, shared vocabulary */
#include "printf.h"
#include "klog.h"          /* a drag must leave evidence on the serial log */
#include "icons.h"         /* ICON_APP — the item_entry's default glyph */
#include "audio.h"         /* §M23 — the taskbar sound indicator */
#include "gfx.h"
#include "rtc.h"
#include "keymap.h"
#include <stdint.h>
#include <stddef.h>

#define TASKBAR_H   cp_taskbar_h()
/* DERIVED FROM THE FONT, NOT A CONSTANT.  These were sized for the 8x8 font
 * and became wrong the moment `gui.font_scale` could move: the clock overran
 * its box and drew over the element beside it, which is what the doubled date
 * in the first 2x screenshot actually was.  A width that assumes a text size
 * has to be recomputed when the text size becomes a setting. */
/* §M69 — MEASURED IN THE FACE IT IS DRAWN IN.  This was `5 * cp_fw() + 24`:
 * a character count times a DIGIT's advance, which is the habit the 8x8 font
 * taught and which console_plate.h warns about in capitals.  With a
 * proportional face the box is the wrong width AND the label inside it is
 * off-centre by the difference — reported from use as "the Start caption
 * should be in the middle".  One source for both, so they cannot disagree. */
#define START_TEXT  lstr("taskbar.start")
#define START_W     (cp_text_w(START_TEXT) + 2 * cp_ctrl_pad_x())
#define TBTN_W      150
/* Clock panel: "YYYY-MM-DD  HH:MM:SS" = 20 glyphs + padding.
 *
 * THE LAYOUT USED TO BE APPENDED HERE and is not any more: it has its own
 * indicator now (below), and a fact displayed in two places is two things that
 * can drift.  The same argument the sound work made — the taskbar control and
 * the Control Panel page write the SAME keys, so there is one answer to "what
 * is the volume" rather than two. */
/* THE STATUS AREA IS SIZED FROM THE TYPE, AND SET IN MONO — widget_specs.md
 * §16: "jobb oldalon mono 12px `muted` státuszok 16px réssel, az óra `text`
 * színnel".  Everything here used to be a 2007 pixel count (icons 20 and
 * cp_fw()+4, boxes 28 and 52) that did not move when §M69 made the type a
 * runtime fact, which is what "the tray looks unfinished" was describing: a
 * 20 px icon beside 20 px text reads as an afterthought, and a 15 px one reads
 * as damage.
 *
 * ONE derived size for every tray glyph, so the two indicators cannot drift
 * apart, and the clock measured in the face it is DRAWN in. */
#define TRAY_ICON   (cp_fh() + cp_px(6))
#define TRAY_GAP    cp_px(16)                  /* the design's status spacing */
#define CLOCK_W     (19 * cp_mono_cell_w() + 2 * TRAY_GAP)
/* §M67 tail — the keyboard-layout indicator, immediately LEFT of the sound one.
 * Same shape and the same rules: always drawn, one flyout at a time, the
 * geometry in one place so draw and hit-test cannot disagree.
 *
 * It is wider than the sound button because it shows the layout's NAME rather
 * than a state: an icon alone would say "this is about the keyboard" and leave
 * the one question the indicator exists to answer — *which* layout — unanswered,
 * and a wrong layout is the single most confusing thing that can happen while
 * typing. */
#define KBD_ICON    TRAY_ICON
#define KBD_W       (TRAY_ICON + cp_px(4) + 3 * cp_mono_cell_w() + TRAY_GAP)
/* §M81 — through cp_px(), like everything §M69 swept out of the apps.  These
 * were flat 150 and 22: an 8x8-era box and an 8x8-era row, in the chrome, which
 * is the one place that sweep did not reach.  The row is the MODEL's answer
 * now (kb_row_h), so this constant is only the box. */
#define KBDPOP_W    cp_px(150)
#define KBD_MAX     8            /* layouts the flyout will show; see kbd_names */
/* §M23 — the sound indicator, immediately left of the clock.  Square, so the
 * icon renderer gets the box it expects. */
#define VOL_ICON    TRAY_ICON
#define VOL_W       (TRAY_ICON + TRAY_GAP)
/* The volume popup: a slider and a mute row.  Deliberately small — this is a
 * status indicator's flyout, not a settings page.  The Sound page in the
 * Control Panel is where the full set lives (§M63 renders it from the
 * CONFIG_KEY descriptors with no per-key UI code). */
#define VOLPOP_W    180
#define VOLPOP_H    76
#define SM_W        (18 * cp_fw() + 24)
#define SM_ITEM_H   (cp_fh() + 12)
/* §M63 — raised from 10.  The Control Panel made it eleven apps, and the cap
 * silently DROPS the overflow: the last registered app simply stops appearing
 * in the launcher, which looks like a broken registration rather than a full
 * menu.  Twelve is still a cap (a scrolling menu is a different feature), but
 * it is now above the number of apps that exist, and the failure mode is
 * written down instead of discovered. */
#define SM_MAX_APPS 12                  /* menu rows before the power tail */

#define COL_TB_TOP (cp_current_theme()->raised)
#define COL_TB_BOT (cp_current_theme()->raised)
#define COL_TB_HILITE (cp_current_theme()->line)
#define COL_START_TOP (cp_current_theme()->accent)
#define COL_START_BOT (cp_current_theme()->accent)
#define COL_START_EDGE (cp_current_theme()->line)
#define COL_TBTN_TOP (cp_current_theme()->tray)
#define COL_TBTN_BOT (cp_current_theme()->tray)
#define COL_TBTN_F_TOP (cp_current_theme()->raised)
#define COL_TBTN_F_BOT (cp_current_theme()->raised)
#define COL_TBTN_EDGE (cp_current_theme()->line)
#define COL_SM_BG (cp_current_theme()->raised)
#define COL_SM_EDGE (cp_current_theme()->line)
#define COL_SM_HOVER (cp_current_theme()->hover)
#define COL_TEXT (cp_current_theme()->text)
#define COL_SHADOW      0x48000000u
#define COL_SEP (cp_current_theme()->line_soft)
/* §M32 — the Start menu's account band.  `tray` is the token the design gives
 * a table header and a tab tray, which is what this band is: a caption over a
 * list, not one of its entries. */
#define COL_SM_HEAD (cp_current_theme()->tray)
#define COL_ACCENT  (cp_current_theme()->accent)

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
    struct item_entry e = { .icon = ICON_APP };
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

/* ---------------------------------------------------------------------------
 * §M81 — THE KEYBOARD FLYOUT IS A LIST TOO, and the shared view draws it.
 *
 * The same shape the Start menu had one conversion earlier: a box, a title, and
 * one row per registered layout — with `py + 26 + i * KBDPOP_ROW` computed in
 * the painter AND in the hit test.  Two copies rather than the menu's three,
 * and the same §4.79 defect: the row you see highlighted and the row a click
 * runs are two different calculations that happen to agree.
 *
 * THE ACTIVE LAYOUT IS THE SELECTION, which is not a trick — it is what the
 * list means here, so the view's `sel` highlight is exactly right.  The old
 * code also drew a `*`, and its reason is worth keeping: *"a selection shown
 * only by a background colour is invisible in a screenshot taken for a bug
 * report, and this project's tests read pixels."*  So the active row carries
 * the keyboard ICON and the others carry none — a GLYPH difference, which
 * survives a screenshot, through the view's own icon column.
 * ------------------------------------------------------------------------- */

/* The chrome's list view — the Start menu and this flyout are both lists, and
 * one lookup serves both.  Defined with the menu below. */
static const struct item_view* sm_view(void);

static int kb_count(void* c) { (void)c; return kbd_count; }

static int kb_active(void) {
    const char* cur = keymap_current();
    for (int i = 0; i < kbd_count; i++)
        if (cur && kbd_names[i] &&
            cur[0] == kbd_names[i][0] && cur[1] == kbd_names[i][1])
            return i;
    return -1;
}

static int kb_get(void* c, int i, struct item_entry* out) {
    (void)c;
    if (i < 0 || i >= kbd_count || !kbd_names[i]) return -1;
    out->label = kbd_names[i];
    out->icon  = (i == kb_active()) ? ICON_KEYBOARD : ICON_NONE;
    return 0;
}

static void kb_activate(void* c, int i) {
    (void)c;
    if (i < 0 || i >= kbd_count || !kbd_names[i]) return;
    /* `config_apply`, NOT `keymap_select` — §M63 stage 0's whole point.
     * `keymap_select` changes the live layout and nothing else, so the setting
     * would revert at the next boot while the panel and the store both said
     * otherwise.  This records the decision and NOTIFIES; the keymap watcher
     * does the switch, so this control, `setlayout` and the Control Panel's
     * Region page cannot disagree about what the layout is. */
    config_apply("keyboard.layout", kbd_names[i]);
}

/* A flyout row is a menu row, not a table row — see the Start menu's
 * `sm_row_h` for why the model is what answers this. */
static int kb_row_h(void* c) { (void)c; return SM_ITEM_H; }

static const struct item_model kb_model = {
    .count = kb_count, .get = kb_get, .activate = kb_activate,
    .row_h = kb_row_h,
};

/* The title band above the list: its own height, so the list box below it and
 * the rule under it come from one number. */
#define KBDPOP_HEAD (cp_fh() + cp_px(14))

static int kb_list_h(void) {
    const struct item_view* v = sm_view();
    int rows = kbd_count > 0 ? kbd_count : 1;
    return (v && v->height_for) ? v->height_for(KBDPOP_W, rows, &kb_model) : 0;
}

static int kbdpop_h(void) { return KBDPOP_HEAD + kb_list_h() + cp_px(6); }
static int kbdpop_x(void);
static int kbdpop_y(void);

/* THE FLYOUT'S LIST BOX, IN ONE PLACE — the painter and the hit test both take
 * it from here.  That is the entire conversion: `py + 26 + i * KBDPOP_ROW` was
 * written out twice, and two calculations that happen to agree are §4.79. */
static void kbd_list_box(int* x, int* y, int* w, int* h) {
    *x = kbdpop_x() + cp_px(4);
    *y = kbdpop_y() + KBDPOP_HEAD;
    *w = KBDPOP_W - cp_px(8);
    *h = kb_list_h();
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

/* THE TAIL IS A TABLE, and that is a fix as much as an addition.
 *
 * Its rows used to be open-coded as `apps`, `apps + 1`, `apps + 2` in FOUR
 * places — the row count, the label, the separator and the click handler — so
 * adding an item meant editing all four in step.  That is §4.79's shape, the
 * one that produces a menu which draws correctly and hit-tests wrongly, and it
 * was one edit away from happening here.  One list now, read by all four. */
/* Lock ends nothing: the session stays, the screen is covered.  Sign out
 * additionally FORGETS who was here, so the next person is asked rather than
 * shown the previous one's name in the header. */
static void sm_lock(void)    { gui_lock_raise(); }
static void sm_signout(void) { gui_session_clear(); gui_lock_raise(); }
static void sm_exitgui(void)  { gui_queue_exit(); }
static void sm_reboot(void)   { gui_queue_power(1); }
static void sm_shutdown(void) { gui_queue_power(0); }

static const struct {
    const char* key;
    void (*act)(void);
    int  icon;
} sm_tail[] = {
    /* Escalating order of what each one ends: the screen, the session, the
     * desktop, the kernel, the machine.  Lock is first because it is the one
     * that ends nothing at all.
     *
     * §M81 — THE ICON IS DATA HERE TOO.  The rows are drawn by the shared list
     * view now, which draws one per item, so the alternative was a fourth
     * open-coded `row < apps ? … : …` in the painter — the very shape the
     * comment above this table warns about. */
    { "menu.lock",     sm_lock,     ICON_USERS    },
    { "menu.signout",  sm_signout,  ICON_USERS    },
    { "menu.exitgui",  sm_exitgui,  ICON_TERMINAL },
    { "menu.reboot",   sm_reboot,   ICON_UPDATE   },
    { "menu.shutdown", sm_shutdown, ICON_POWER    },
};
#define SM_TAIL_N ((int)(sizeof sm_tail / sizeof sm_tail[0]))

/* The header band: who is signed in.  Not a row — it cannot be clicked, and
 * giving it a row index would put a non-action into the same arithmetic as the
 * actions.
 *
 * IT WAS WITHDRAWN ONCE AND THE REASON WAS A DIFFERENT BUG.  The band appeared
 * nowhere, and the measured geometry (myy=540, head=41, box on screen at ~650)
 * looked like a coordinate-system problem.  It was not: `panelsurf` is backed
 * only below `panel_strip_top`, and adding Lock + Sign out had already pushed
 * the menu past PANEL_POPUP_MAX, so the strip's clip was silently removing the
 * TOP of it — two application rows first (Control Panel and File Manager
 * vanished and nobody noticed), the header after.  *A clip that removes what
 * you just added looks exactly like what you just added not working.* */
#define SM_HEAD_H (cp_fh() + cp_px(14))

static int menu_rows(void) {
    int apps = gui_app_count();
    if (apps > SM_MAX_APPS) apps = SM_MAX_APPS;
    return apps + SM_TAIL_N;
}

/* ---------------------------------------------------------------------------
 * §M81 — THE START MENU IS AN ITEM MODEL, AND THE SHARED LIST VIEW DRAWS IT.
 *
 * Asked for directly: *"list items, which the Start menu could also use."*
 * The desktop's icon field, the Control Panel and the file manager have shared
 * `item_model` + `ITEM_VIEW()` since §M64; the menu was the list that did not,
 * and it paid the usual price — its row arithmetic
 * (`myy + 6 + SM_HEAD_H + i * SM_ITEM_H`) existed in THREE places: the
 * painter, the click handler and the hover.  That is §4.79's shape exactly,
 * and this file's own comment above `sm_tail` says so about the row TABLE
 * while the row GEOMETRY stayed triplicated one screen below it.
 *
 * What the conversion needed from the view, both appended and optional:
 * `item_entry.group_start` (a menu is a list with divisions) and
 * `item_view.height_for` (a menu sizes itself to its contents, where every
 * other caller is given a box and asks how much fits).
 * ------------------------------------------------------------------------- */

static int sm_count(void* c) { (void)c; return menu_rows(); }

static int sm_apps(void) {
    int apps = gui_app_count();
    return apps > SM_MAX_APPS ? SM_MAX_APPS : apps;
}

static int sm_get(void* c, int i, struct item_entry* out) {
    (void)c;
    int apps = sm_apps();
    if (i < 0) return -1;
    if (i < apps) {
        const struct gui_app_def* a = gui_app_at(i);
        if (!a) return -1;
        /* §M69 — THE ENGLISH NAME IS THE KEY.  `gui_app_def.name` is a stable
         * IDENTIFIER (§M64's shortcut resolver matches `app:File Manager`
         * against it, so a `.lnk` survives a rebuild), which is why it is not
         * translated at the registry: looking it up gives a translated LABEL
         * while the identity stays put. */
        out->label = lstr(a->name);
        out->icon  = a->icon ? a->icon : ICON_APP;
        return 0;
    }
    int t = i - apps;
    if (t >= SM_TAIL_N) return -1;
    out->label = lstr(sm_tail[t].key);
    out->icon  = sm_tail[t].icon;
    /* The rule above the session tail — the model saying where a group begins,
     * rather than the painter counting rows a second time. */
    out->group_start = (t == 0);
    return 0;
}

static void sm_activate(void* c, int i) {
    (void)c;
    int apps = sm_apps();
    if (i < 0) return;
    if (i < apps) { gui_queue_launch(gui_app_at(i)); return; }
    int t = i - apps;
    if (t < SM_TAIL_N) sm_tail[t].act();
}

/* A MENU ROW IS NOT A TABLE ROW, and the model is what knows that.  At the
 * density's `row_h` a full menu comes to 1088 px at the 200 % cap — past
 * `PANEL_POPUP_MAX` and past the screen — so the clip would silently eat the
 * top rows, which is §M32's defect verbatim.  Text plus padding, as it always
 * was here. */
static int sm_row_h(void* c) { (void)c; return SM_ITEM_H; }

static const struct item_model sm_model = {
    .count = sm_count, .get = sm_get, .activate = sm_activate,
    .row_h = sm_row_h,
};

/* The list view, looked up once.  It is a linker-section registration compiled
 * into every build, so a NULL here means the registry itself is broken — which
 * is worth saying out loud rather than papering over with a fallback that
 * recomputes the row geometry.  *A fallback kept past its usefulness is what
 * makes one path work and its twin silently not* (§M52), and the whole point of
 * this conversion is that the geometry has ONE owner. */
static const struct item_view* sm_view(void) {
    static const struct item_view* v;
    static int moaned;
    if (!v) v = item_view_by_name("list");
    if (!v && !moaned) {
        moaned = 1;
        kprintf("vista: the 'list' item view is not registered - "
                "the Start menu cannot be drawn\n");
    }
    return v;
}

static int menu_list_h(void) {
    const struct item_view* v = sm_view();
    if (!v || !v->height_for) return 0;
    return v->height_for(SM_W, menu_rows(), &sm_model);
}

static int menu_h(void)   { return menu_list_h() + 12 + SM_HEAD_H; }
static int menu_top(void) { return scr_h - TASKBAR_H - menu_h(); }

/* THE MENU'S LIST BOX, IN ONE PLACE.  The painter, the hit test and the hover
 * all take it from here, which is the whole point of the conversion: three
 * copies of `myy + 6 + SM_HEAD_H + i * SM_ITEM_H` could disagree and one
 * cannot. */
static void menu_list_box(int* x, int* y, int* w, int* h) {
    int myy = menu_top();
    *x = 6;
    *y = myy + 6 + SM_HEAD_H;
    *w = SM_W - 4;
    *h = menu_list_h();
}

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

/* ---------------------------------------------------------------------------
 * §M81 — THE TASKBAR'S TWO BOXES, EACH IN ONE PLACE.
 *
 * The last of the chrome's §4.79 duplications.  The Start button was drawn at
 * `(4, ty + 4, START_W, TASKBAR_H - 8)` and hit-tested as `x >= 4 && x < 4 +
 * START_W` — the same box written twice, and the second copy never mentioned
 * the vertical extent at all.  The window buttons were worse: a whole strip
 * (`x = START_W + 12; … x += bw + 6`) laid out in the painter and laid out
 * AGAIN in the click handler.
 *
 * THE TWO COPIES ALSO TOOK SEPARATE SNAPSHOTS OF THE WINDOW LIST, and that is
 * the part with teeth: `bw` is `avail / n`, so a window opening or closing
 * between the paint and the click changes every button's width and the strip
 * shifts under the pointer.  One geometry cannot remove the race, but it does
 * make the two ends agree about what they were given — and the shared function
 * takes `n` as an ARGUMENT for exactly that reason, so the caller's count is
 * visibly the one being used.
 * ------------------------------------------------------------------------- */

/* The Start button, including its vertical extent — which the hit test used to
 * leave out entirely. */
static void start_box(int* x, int* y, int* w, int* h) {
    *x = cp_px(4);
    *y = scr_h - TASKBAR_H + cp_px(4);
    *w = START_W;
    *h = TASKBAR_H - cp_px(8);
}

static int tbtn_width(int nslots) {
    int avail = scr_w - (START_W + 12) - CLOCK_W - 8;
    if (nslots <= 0) return TBTN_W;
    int w = avail / nslots - 6;
    if (w > TBTN_W) w = TBTN_W;
    if (w < 48)     w = 48;
    return w;
}

/* ONE window button's rect.  `n` is the caller's own count, so the two ends
 * cannot silently be sizing against different window lists. */
static void tbtn_rect(int i, int n, int* x, int* y, int* w, int* h) {
    int bw = tbtn_width(n);
    *w = bw;
    *x = START_W + cp_px(12) + i * (bw + cp_px(6));
    *y = scr_h - TASKBAR_H + cp_px(5);
    *h = TASKBAR_H - cp_px(10);
}

/* WHAT THE HEADER SAYS.  `gui_session_user()` when somebody signed in at the
 * lock screen; otherwise the desktop is running as the system and says so —
 * the same word `ps`, /proc and the Task Manager use, because inventing a
 * friendlier one here would make this the only place in the tree where that
 * identity has a different name. */
/* §M81 — WHO THIS SESSION ACTUALLY RUNS AS, and who signed in, when they are
 * not the same thing.
 *
 * They are not the same thing today, and this header was reporting the wrong
 * one.  `gui_session_user()` is a STRING the lock screen records after checking
 * a password; it is not an identity.  `cred_become_user` — the only call that
 * gives a task one — is reached from the shell's `login` and from NOTHING in
 * the GUI, so the desktop, the compositor and every app-host stay SYSTEM after
 * a successful sign-in.
 *
 * Reported from use, and the reporter had to work it out from a refusal: they
 * turned on `gui.login`, rebooted, signed in as root, and the accounts panel
 * still answered *"sign in as root first.  Acting as system."*  The panel was
 * telling the truth; THIS was the line claiming otherwise.
 *
 * *A report that agrees with the intention rather than with the machine is the
 * most expensive kind of wrong* — §4.83's IOMMU boundary that widened while
 * `drv domain` went on saying `isolation full`, one subsystem over.  So until
 * the GUI session really adopts an identity (see gui.h), this says both. */
static char menu_user_buf[56];

static const char* menu_user(void) {
    /* THE RETURN VALUE, NOT THE BUFFER.  cred.h says it plainly — *"takes a
     * caller-owned buffer and RETURNS IT (or a string literal, when the answer
     * is a constant)"* — and the constant cases are `kernel`, `system` and
     * `root`, i.e. every case that matters here.  Reading the buffer instead
     * printed uninitialised stack, which came out as `?`. */
    char buf[40];
    const char* real = cred_owner_name(cred_current(), buf, sizeof buf);
    const char* signed_in = gui_session_user();

    int n = 0;
    for (const char* p = real; *p && n < (int)sizeof menu_user_buf - 1; p++)
        menu_user_buf[n++] = *p;
    /* Only when they disagree — a machine where they agree should not carry a
     * permanent parenthetical about a distinction that is not being made. */
    if (signed_in) {
        int same = 1;
        for (int i = 0; ; i++) {
            if (real[i] != signed_in[i]) { same = 0; break; }
            if (!real[i]) break;
        }
        if (!same) {
            const char* a = " (signed in: ";
            for (int i = 0; a[i] && n < (int)sizeof menu_user_buf - 2; i++)
                menu_user_buf[n++] = a[i];
            for (int i = 0; signed_in[i] && n < (int)sizeof menu_user_buf - 2; i++)
                menu_user_buf[n++] = signed_in[i];
            menu_user_buf[n++] = ')';
        }
    }
    menu_user_buf[n] = 0;
    return menu_user_buf;
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

    /* The panel is one flat `raised` strip with a 1 px `line` along its top —
     * the design's panel, not a gradient bar. */
    gfx_fill(back, 0, ty, scr_w, TASKBAR_H, COL_TB_TOP);
    gfx_fill(back, 0, ty, scr_w, 1, COL_TB_HILITE);

    /* Start button — the box from start_box(), which the hit test also uses. */
    {
        int sx, sy, sw, sh;
        start_box(&sx, &sy, &sw, &sh);
        cp_plate(back, sx, sy, sw, sh,
                 menu_open ? cp_current_theme()->press : COL_START_TOP,
                 COL_START_EDGE);
        cp_text(back, sx + (sw - cp_text_w(START_TEXT)) / 2,
                ty + (TASKBAR_H - cp_fh()) / 2, START_TEXT, COL_TEXT);
    }

    /* One button per open window. */
    struct gui_window* slots[TB_MAX_BTNS];
    int n  = gui_wm_windows(slots, TB_MAX_BTNS);
    int bw = tbtn_width(n);
    struct gui_window* focused = gui_wm_focused();
    for (int i = 0; i < n; i++) {
        int f = (slots[i] == focused);
        int m = gui_window_minimized(slots[i]);
        int x, by, bh;
        tbtn_rect(i, n, &x, &by, &bw, &bh);
        cp_plate(back, x, by, bw, bh,
                 f ? COL_TBTN_F_TOP : (m ? cp_current_theme()->tray
                                         : COL_TBTN_TOP),
                 COL_TBTN_EDGE);

        char t[20];
        const char* title = gui_window_title(slots[i]);
        int maxch = (bw - 12) / cp_fw();
        if (maxch > (int)sizeof(t) - 1) maxch = (int)sizeof(t) - 1;
        int k = 0;
        for (; title[k] && k < maxch; k++) t[k] = title[k];
        t[k] = 0;
        cp_text(back, x + 6, ty + (TASKBAR_H - cp_fh()) / 2, t, COL_TEXT);
        x += bw + 6;
    }

    /* Clock. */
    /* NO BEVELLED DIVIDER.  Two hardcoded 1 px fills used to stand here — an
     * M22 Vista-era groove in literal colours belonging to no theme, and the
     * bright line visible between the speaker and the date in every screenshot.
     * The design separates status items with SPACE (TRAY_GAP), not with rules;
     * a groove down a flat panel is the one gradient-era habit that survived
     * the port because nobody was looking at that corner. */
    int cx = scr_w - CLOCK_W;
    if (clock_str[0]) {
        /* MONO, per §16 — a clock set in a proportional face changes width as
         * the digits change, so the whole tray shifts once a second.  Centred
         * with the mono advance, which is exact for it. */
        int len = 0;
        while (clock_str[len]) len++;
        cp_mono_text(back, cx + (CLOCK_W - len * cp_mono_cell_w()) / 2,
                     ty + (TASKBAR_H - cp_fh()) / 2, clock_str, COL_TEXT);
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
        icon_draw(back, kx + cp_px(2), ky, KBD_ICON, ICON_KEYBOARD);
        const char* kb = keymap_current();
        if (kb && kb[0]) {
            char up[4];
            int i = 0;
            for (; kb[i] && i < 3; i++) {
                char c = kb[i];
                up[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
            }
            up[i] = 0;
            cp_mono_text(back, kx + KBD_ICON + cp_px(4),
                         ty + (TASKBAR_H - cp_fh()) / 2, up, COL_TEXT);
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

        cp_text(back, px + cp_px(10), py + cp_px(7), lstr("tray.kbdlayout"),
                COL_TB_HILITE);
        gfx_fill(back, px + cp_px(8), py + KBDPOP_HEAD - 1,
                 KBDPOP_W - cp_px(16), 1, COL_SEP);

        if (kbd_count == 0) {
            cp_text(back, px + cp_px(10), py + KBDPOP_HEAD + cp_px(4),
                    lstr("tray.nolayouts"), COL_TB_HILITE);
        } else {
            /* §M81 — the rows are the shared list view's.  The ACTIVE layout is
             * the `sel`, which is what the highlight means here, and the active
             * row's keyboard ICON is the glyph the old `*` was: a selection
             * shown only by a background colour is invisible in a screenshot,
             * and this project's tests read pixels. */
            int bx, by, bw, bh;
            kbd_list_box(&bx, &by, &bw, &bh);
            const struct item_view* v = sm_view();
            if (v && v->draw) v->draw(back, bx, by, bw, bh, &kb_model,
                                      kb_active(), 0);
        }
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
            cp_text(back, px + 10, py + 12, "No audio device", COL_TEXT);
            cp_text(back, px + 10, py + 30, "nothing to play through", COL_TB_HILITE);
        } else {
            char line[24];
            int n = 0;
            const char* lbl = "Volume ";
            for (int i = 0; lbl[i]; i++) line[n++] = lbl[i];
            if (pct >= 100) { line[n++] = '1'; line[n++] = '0'; line[n++] = '0'; }
            else if (pct >= 10) { line[n++] = (char)('0' + pct / 10); line[n++] = (char)('0' + pct % 10); }
            else line[n++] = (char)('0' + pct);
            line[n++] = '%'; line[n] = 0;
            cp_text(back, px + 10, py + 10, line, COL_TEXT);

            /* The slider: a track and a filled portion.  Drawn from the SAME
             * geometry the click handler reads back (vol_track_*), so the
             * knob cannot end up somewhere the click does not land. */
            int tx = px + 10, tw = VOLPOP_W - 20, tyy = py + 30;
            gfx_fill(back, tx, tyy, tw, 6, 0xFF1B2434u);
            int fill = muted ? 0 : (tw * pct) / 100;
            gfx_fill(back, tx, tyy, fill, 6, muted ? 0xFF556070u : 0xFF4C8BE0u);
            gfx_fill(back, tx + (fill ? fill - 2 : 0), tyy - 3, 4, 12,
                     muted ? 0xFF8B94A6u : COL_TEXT);

            cp_text(back, px + 10, py + 52, muted ? "[ Unmute ]" : "[ Mute ]", COL_TEXT);
        }
    }

    /* Start menu overlay. */
    if (menu_open) {
        int mh = menu_h(), myy = menu_top();

        gfx_blend_fill(back, 8, myy + 4, SM_W, mh, COL_SHADOW);
        gfx_fill(back, 4, myy, SM_W, mh, COL_SM_BG);
        gfx_fill(back, 4, myy, SM_W, 1, COL_SM_EDGE);
        gfx_fill(back, 4, myy + mh - 1, SM_W, 1, COL_SM_EDGE);
        gfx_fill(back, 4, myy, 1, mh, COL_SM_EDGE);
        gfx_fill(back, 4 + SM_W - 1, myy, 1, mh, COL_SM_EDGE);

        /* THE HEADER: the account, set apart rather than listed.  Its own band
         * and the accent colour, with a rule under it — a name sitting in the
         * same column as "Shut Down", in the same colour, reads as another
         * command. */
        gfx_fill(back, 5, myy + 1, SM_W - 2, SM_HEAD_H - 1, COL_SM_HEAD);
        cp_text(back, 18, myy + 1 + (SM_HEAD_H - 1 - cp_fh()) / 2,
                menu_user(), COL_ACCENT);
        gfx_fill(back, 10, myy + SM_HEAD_H, SM_W - 12, 1, COL_SEP);

        /* §M81 — THE ROWS ARE THE SHARED LIST VIEW'S NOW.  The separator comes
         * from the model's `group_start` and the highlight from `sel`, so the
         * three things this loop used to compute — the row box, the divider and
         * the hover band — are one piece of geometry owned by the view.  The
         * icons are new and come for free: the view draws one per item, and the
         * registry has had them since §M64. */
        {
            int bx, by, bw, bh;
            menu_list_box(&bx, &by, &bw, &bh);
            const struct item_view* v = sm_view();
            if (v && v->draw)
                v->draw(back, bx, by, bw, bh, &sm_model, menu_hover, 0);
        }
    }
}

static void vista_motion(int x, int y) {
    if (!menu_open) return;
    /* §M81 — the third copy of the row arithmetic, now the same call the
     * painter and the click make.  A hover that highlighted a different row
     * from the one a click would run is precisely the defect a shared view
     * makes unrepresentable. */
    int bx, by, bw, bh;
    menu_list_box(&bx, &by, &bw, &bh);
    const struct item_view* v = sm_view();
    int nh = (v && v->hit) ? v->hit(x - bx, y - by, bw, bh, &sm_model, 0) : -1;
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
            /* §M81 — the VIEW answers.  A click on the title band is outside
             * the list box, so `hit` returns -1 and only the dismissal below
             * runs: the title is not a row and cannot become one by rounding,
             * which a division by the row height could do. */
            int bx, by, bw, bh;
            kbd_list_box(&bx, &by, &bw, &bh);
            const struct item_view* v = sm_view();
            int idx = (v && v->hit) ? v->hit(x - bx, y - by, bw, bh, &kb_model, 0)
                                    : -1;
            if (idx >= 0) {
                kb_activate(NULL, idx);
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
            /* §M81 — the VIEW answers.  A click on the header band is outside
             * the list box, so `hit` returns -1 and nothing runs: the header is
             * not a row, and it cannot become one by rounding — which is what a
             * division by the row height did before. */
            int bx, by, bw, bh;
            menu_list_box(&bx, &by, &bw, &bh);
            const struct item_view* v = sm_view();
            int idx = (v && v->hit) ? v->hit(x - bx, y - by, bw, bh, &sm_model, 0)
                                    : -1;
            if (idx >= 0) sm_activate(NULL, idx);
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

    /* Start button — the SAME box the painter drew (start_box), vertical
     * extent included. */
    {
        int sx, sy, sw, sh;
        start_box(&sx, &sy, &sw, &sh);
        if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) {
        menu_open = !menu_open;
        menu_hover = -1;
        vol_pop_open = 0;
        kbd_pop_open = 0;
        publish_popup();
        return 1;
        }
    }

    /* Window buttons — the same strip the painter laid out. */
    struct gui_window* slots[TB_MAX_BTNS];
    int n = gui_wm_windows_locked(slots, TB_MAX_BTNS);
    for (int i = 0; i < n; i++) {
        int bx, by, bw, bh;
        tbtn_rect(i, n, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            gui_wm_taskbar_activate_locked(slots[i]);   /* M22.3 */
            return 1;
        }
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
