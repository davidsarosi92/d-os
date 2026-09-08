/* =============================================================================
 * w_menubar.c — a window's menu bar (§M65 stage 2).
 *
 * WHY A WIDGET AND NOT WINDOW CHROME.  A menu bar could live in the title-bar
 * region, owned by the WM.  It lives in the content instead, as an ordinary
 * widget, because that is what makes it participate in the LAYOUT: it measures
 * itself, the rows below it get what is left, and a resize needs no special
 * case anywhere.  The thing that argument would normally cost — a menu that
 * scrolls away with the content — costs nothing here, because no window in
 * this system scrolls its content as a whole.
 *
 * THE MENU IS A MODEL, NOT CODE.  An app hands over an array of
 * (menu, item, id) triples and gets (id) back through the window's ONE event
 * sink.  Flat triples rather than a tree of pointers for the reason the whole
 * toolkit is shaped this way: a ring-3 client has to be able to send it.
 *
 * The drop-down itself is gui.c's popup — the same overlay the combo box uses,
 * because a menu and a combo differ in what they mean, not in what they do.
 * ============================================================================= */

#include "ui.h"
#include "widget.h"
#include "console_plate.h"
#include "locale.h"
#include "gui.h"
#include "gfx.h"
#include "keymap.h"
#include "kmalloc.h"
#include <stddef.h>

#define MB_H        20
#define MB_PAD      10
#define MB_MAX      8               /* top-level menus per window            */
#define MB_TITLELEN 16

/* §M69 — THE THEME, READ LIVE.  Three literals, and they were dark-theme
 * values: under the light theme the bar stayed a navy band with its own dark
 * text on it, which is the same defect the item views had one file over.
 * widget_specs.md §13 puts the menu bar on `raised` with `text` on it and
 * `hover` under the pointer. */
#define MBCOL_BG    (cp_current_theme()->raised)
#define MBCOL_TEXT  (cp_current_theme()->text)
#define MBCOL_HOT   (cp_current_theme()->hover)

struct w_menubar {
    struct widget base;
    const struct ui_menu_def* defs;
    int   ndefs;
    char  titles[MB_MAX][MB_TITLELEN];
    int   ntitles;
    int   open_menu;                /* -1 = none                             */
};

static int mb_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int mb_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

/* Distinct menu titles, in the order they first appear.  Order matters: it is
 * the order the user sees, and deriving it from the declaration means the app
 * controls it without a second list to keep in step. */
static void mb_collect(struct w_menubar* m) {
    m->ntitles = 0;
    for (int i = 0; i < m->ndefs && m->ntitles < MB_MAX; i++) {
        /* §M69 — THROUGH THE CATALOGUE.  Safe to apply to every menu in the
         * tree because `lstr` falls back to the key itself: a plain English
         * literal that is not a catalogue key renders as itself, so the
         * catalogue can be adopted one string at a time instead of in one
         * flag-day edit. */
        const char* t = lstr(m->defs[i].menu);
        if (!t) continue;
        int seen = 0;
        for (int j = 0; j < m->ntitles; j++)
            if (mb_streq(m->titles[j], t)) { seen = 1; break; }
        if (seen) continue;
        int n = 0;
        for (; t[n] && n < MB_TITLELEN - 1; n++) m->titles[m->ntitles][n] = t[n];
        m->titles[m->ntitles][n] = 0;
        m->ntitles++;
    }
}

/* Where each title starts, so draw and hit-test cannot disagree. */
static int mb_title_x(struct w_menubar* m, int idx) {
    int x = m->base.x + 4;
    for (int i = 0; i < idx && i < m->ntitles; i++)
        x += cp_text_w(m->titles[i]) + MB_PAD * 2;
    return x;
}

static int mb_title_at(struct w_menubar* m, int lx) {
    int x = 4;
    for (int i = 0; i < m->ntitles; i++) {
        int w = cp_text_w(m->titles[i]) + MB_PAD * 2;
        if (lx >= x && lx < x + w) return i;
        x += w;
    }
    return -1;
}

static void mb_draw(struct widget* w, struct gfx_surface* s) {
    struct w_menubar* m = (struct w_menubar*)w;
    gfx_fill(s, w->x, w->y, w->w, MB_H, MBCOL_BG);
    for (int i = 0; i < m->ntitles; i++) {
        int tx = mb_title_x(m, i);
        int tw = mb_len(m->titles[i]) * cp_fw() + MB_PAD * 2;
        if (i == m->open_menu)
            gfx_fill(s, tx - MB_PAD, w->y, tw, MB_H, MBCOL_HOT);
        cp_text(s, tx, w->y + (MB_H - cp_fh()) / 2, m->titles[i], MBCOL_TEXT);
    }
}

/* Build the popup's item string for menu `idx`: one entry per line, which is
 * the flat form gui_popup_open takes. */
static void mb_open(struct w_menubar* m, int idx) {
    if (idx < 0 || idx >= m->ntitles) return;
    char buf[POPUP_TEXT_MAX];
    int n = 0;
    for (int i = 0; i < m->ndefs; i++) {
        if (!mb_streq(m->defs[i].menu, m->titles[idx])) continue;
        const char* it = lstr(m->defs[i].item) ? lstr(m->defs[i].item) : "-";
        for (int k = 0; it[k] && n < (int)sizeof buf - 2; k++) buf[n++] = it[k];
        if (n < (int)sizeof buf - 1) buf[n++] = '\n';
    }
    if (n && buf[n - 1] == '\n') n--;
    buf[n] = 0;

    m->open_menu = idx;
    ui_popup_from(&m->base, idx);       /* the answer comes back to this class */
    /* Screen coordinates: the popup is a compositor overlay, and the widget
     * knows only where it is inside the window's content. */
    int sx = 0, sy = 0;
    gui_window_content_origin(m->base.win, &sx, &sy);
    gui_popup_open(m->base.win, sx + mb_title_x(m, idx) - MB_PAD,
                   sy + m->base.y + MB_H, buf, idx);
    gui_window_request_redraw(m->base.win);
}

static void mb_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)ly; (void)kind;
    struct w_menubar* m = (struct w_menubar*)w;
    mb_open(m, mb_title_at(m, lx));
}

static const struct widget_ops mb_ops = {
    .draw = mb_draw, .mouse = mb_mouse,
};

/* The popup closed.  Turn (menu, row) into the declared command id and raise it
 * as an ordinary click on the MENU BAR's own id — the app then has one place to
 * handle commands, whether they came from a menu or a button. */
static void mb_popup_pick(struct widget* w, int tag, int row) {
    struct w_menubar* m = (struct w_menubar*)w;
    m->open_menu = -1;
    gui_window_request_redraw(w->win);
    if (row < 0) return;                       /* dismissed — nothing to do */
    int seen = 0;
    for (int i = 0; i < m->ndefs; i++) {
        if (tag < 0 || tag >= m->ntitles) break;
        if (!mb_streq(m->defs[i].menu, m->titles[tag])) continue;
        if (seen == row) { ui_emit(w, UI_EV_CLICK, m->defs[i].id); return; }
        seen++;
    }
}

static struct widget* mb_create(struct gui_window* win, const struct ui_spec* sp) {
    (void)sp;
    struct w_menubar* m = (struct w_menubar*)kcalloc(1, sizeof *m);
    if (!m) return NULL;
    m->open_menu = -1;
    widget_init(&m->base, win, 0, 0, 200, MB_H, &mb_ops, NULL, 0);
    return &m->base;
}

static void mb_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    (void)w;
    *min_w  = 40;
    *pref_w = avail_w;                  /* a menu bar spans its window */
    *pref_h = MB_H;
}

WIDGET_CLASS(wc_menubar) = {
    .name = "menubar", .create = mb_create, .measure = mb_measure,
    .popup_pick = mb_popup_pick,
};

/* ---------------------------------------------------------------------------
 * The app-facing half.
 * ------------------------------------------------------------------------- */

void ui_menubar_set(struct gui_window* win, int id,
                    const struct ui_menu_def* defs, int n) {
    struct widget* w = ui_by_id(win, id);
    if (!w) return;
    struct w_menubar* m = (struct w_menubar*)w;
    m->defs = defs;                     /* borrowed: apps declare these static */
    m->ndefs = n;
    mb_collect(m);
    m->open_menu = -1;
}

void ui_menubar_closed(struct gui_window* win, int id) {
    struct widget* w = ui_by_id(win, id);
    if (w) ((struct w_menubar*)w)->open_menu = -1;
}
