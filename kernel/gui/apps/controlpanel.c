/* =============================================================================
 * controlpanel.c — the Control Panel window (§M63).
 *
 * This file NAMES NO SETTING.  It walks the SETTINGS_PANEL() registry, hands
 * the result to an item_view chosen by `controlpanel.view`, and activates
 * whatever was double-clicked.  Adding "Display" later is a registration in
 * §M61's own file and no edit here — which is the entire architectural point
 * of the milestone.
 *
 * The panels open as SEPARATE WINDOWS rather than as pages inside this one.
 * §M22.7 put every WIN_APP on its own task precisely so a slow or wedged app
 * cannot take the GUI down, and hosting eight panels in one window would undo
 * that for the app whose job is to change display modes and keyboard layouts —
 * the two settings most able to wedge.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "itemview.h"
#include "settings.h"
#include "config.h"
#include "kmalloc.h"
#include <stddef.h>

/* Singleton: a second Control Panel would be two views of one config, and the
 * second one's list would go stale the moment the first changed anything. */
static struct gui_window* cp_win = NULL;

struct cp_state {
    struct w_itemview* iv;
};

/* ---- the model over the panel registry ---------------------------------- */

static int cp_count(void* ctx) { (void)ctx; return settings_panel_count(); }

static int cp_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    const struct settings_panel* p = settings_panel_at(i);
    if (!p) return -1;
    out->label = p->name;
    out->sub   = p->summary;
    out->icon  = p->icon ? p->icon : ICON_SETTINGS;
    out->dim   = 0;
    return 0;
}

static void cp_activate(void* ctx, int i) { (void)ctx; settings_panel_open(i); }

/* §M65 stage 3 — the SAME model, answering about columns.  A table is not a
 * different data source: it is this one asked "what is item i's column c"
 * instead of "what is item i".  Declaring these three makes
 * `controlpanel.view = table` a config change rather than a new app. */
static int cp_columns(void* ctx) { (void)ctx; return 2; }

static const char* cp_col_title(void* ctx, int c) {
    (void)ctx;
    return c == 0 ? "CATEGORY" : "WHAT IT CONTAINS";
}

static int cp_col_weight(void* ctx, int c) { (void)ctx; return c == 0 ? 1 : 2; }

static int cp_cell(void* ctx, int i, int c, char* out, int cap) {
    (void)ctx;
    const struct settings_panel* p = settings_panel_at(i);
    if (!p || cap <= 0) { if (cap) out[0] = 0; return -1; }
    const char* src = (c == 0) ? p->name : (p->summary ? p->summary : "");
    int k = 0;
    for (; src[k] && k < cap - 1; k++) out[k] = src[k];
    out[k] = 0;
    return 0;
}

static const struct item_model cp_model = {
    .count = cp_count, .get = cp_get, .activate = cp_activate, .ctx = NULL,
    .columns = cp_columns, .col_title = cp_col_title,
    .col_weight = cp_col_weight, .cell = cp_cell,
};

/* ---- window ------------------------------------------------------------- */

static void cp_on_close(struct gui_window* win) { (void)win; cp_win = NULL; }

static void cp_layout(struct gui_window* win) {
    struct cp_state* st = (struct cp_state*)gui_window_ctx(win);
    if (!st) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    w_label_create(win, 8, 6, cw - 16,
                   "Settings - double-click a category");
    st->iv = w_itemview_create(win, 8, 24, cw - 16, ch - 32,
                               &cp_model,
                               config_get("controlpanel.view", "grid"), st);
}

static void controlpanel_open(void) {
    if (cp_win) { gui_window_raise(cp_win); return; }

    struct cp_state* st = (struct cp_state*)kcalloc(1, sizeof *st);
    if (!st) return;

    int ow, oh;
    gui_window_outer_for_content(560, 340, &ow, &oh);
    struct gui_window* win =
        gui_app_window_create("Control Panel", 180, 100, ow, oh, cp_layout, st);
    if (!win) { kfree(st); return; }
    cp_win = win;
    gui_window_set_on_close(win, cp_on_close);
}

GUI_APP_ICON("Control Panel", controlpanel_open, ICON_SETTINGS);

/* =============================================================================
 * The panels that exist today.  Each is a REGISTRATION, and the ones with no
 * `open` are rendered by the generic CONFIG_KEY panel — the setting itself is
 * declared next to the code that reads it, not here.
 * ============================================================================= */

SETTINGS_PANEL(sp_personalisation) = {
    .name    = "Personalisation",
    .summary = "wallpaper, desktop layout, shell",
    .icon    = ICON_BRUSH,
};

SETTINGS_PANEL(sp_system) = {
    .name    = "System",
    .summary = "boot, faults, crash reporting",
    .icon    = ICON_SETTINGS,
};

SETTINGS_PANEL(sp_input) = {
    .name    = "Region and input",
    .summary = "keyboard layout",
    .icon    = ICON_KEYBOARD,
};

/* §M23 — Sound.  No `open`, so this is the generic CONFIG_KEY panel: the two
 * keys are declared in audio.c next to the code that reads them, and this
 * registration is the ENTIRE cost of giving them a page.  That is the whole
 * claim §M63 made — a setting should be a line, not an app — and it is worth
 * noting that adding sound to the Control Panel really did take one struct.
 *
 * The taskbar indicator and this page write the SAME keys, so there is one
 * answer to "what is the volume" rather than two that drift. */
SETTINGS_PANEL(sp_sound) = {
    .name    = "Sound",
    .summary = "output volume and mute",
    .icon    = ICON_VOLUME,
};
