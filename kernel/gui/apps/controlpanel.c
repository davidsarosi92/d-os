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
#include "console_plate.h"
#include "locale.h"
#include "printf.h"
#include "itemview.h"
#include "settings.h"
#include "config.h"
#include "kmalloc.h"
#include <stddef.h>

/* Singleton: a second Control Panel would be two views of one config, and the
 * second one's list would go stale the moment the first changed anything. */
static struct gui_window* cp_win = NULL;

struct cpanel_state {
    struct w_itemview* iv;
};

/* ---- the model over the panel registry ---------------------------------- */

static int cp_count(void* ctx) { (void)ctx; return settings_panel_count(); }

static int cp_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    const struct settings_panel* p = settings_panel_at(i);
    if (!p) return -1;
    /* §M69 — TRANSLATED HERE, IN THE MODEL, and deliberately not in the item
     * VIEW.  The view draws file names, task names and device names too, and
     * routing every item label through the catalogue would rename a file
     * called "Save" to "Mentés" — user DATA is not a message.  Only a model
     * that knows its items are interface labels may translate them.
     *
     * The registry name stays English because it is an IDENTIFIER: a
     * CONFIG_KEY's `.group` is matched against it, so translating it at the
     * registration would detach every setting from its panel.  Same rule as
     * the app names in the Start menu — the English string is the key. */
    out->label = lstr(p->name);
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


static void cp_layout(struct gui_window* win) {
    struct cpanel_state* st = (struct cpanel_state*)gui_window_ctx(win);
    if (!st) return;
    /* Builds its widgets — so replace the old set (gui_window_clear_widgets). */
    gui_window_clear_widgets(win);
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    /* §M69 — the header used to sit at y=6 and the view at y=24, while a label
     * is `cp_fh() + 4` tall: at 137 % density that is ~24 px, so the view
     * started 6 px INSIDE the label and painted over its descenders.  The two
     * offsets agreed only at the 8x8 font they were measured for. */
    const int pad = cp_px(8), gap = cp_px(6), row = cp_row_h();
    w_label_create(win, pad, gap, cw - 2 * pad,
                   "cp.hint");
    st->iv = w_itemview_create(win, pad, gap + row, cw - 2 * pad,
                               ch - gap - row - gap,
                               &cp_model,
                               config_get("controlpanel.view", "grid"), st);

    /* §M69 — `gui.wheeltest`: fire one notch at the grid, on the surface the
     * report was about.  Reported from use: *"the Control Panel scrolls even
     * though everything fits, and most of the icons vanish."*  The harness
     * cannot type once a window has focus (§4.74), so the panel triggers its
     * own probe. */
    if (st->iv && config_get_long("gui.wheeltest", 0)) {
        kprintf("controlpanel: scroll=%d before the notch\n", st->iv->scroll);
        gui_wheel_test(cw / 2, gap + row + cp_px(20), -1);
    }
}

static void controlpanel_open(void) {
    struct cpanel_state* st = (struct cpanel_state*)kcalloc(1, sizeof *st);
    if (!st) return;
    struct gui_app_spec sp = {
        .title = "Control Panel",
        .content_w = cp_px(560), .content_h = cp_px(340),
        .layout = cp_layout, .ctx = st, .slot = &cp_win,
    };
    if (!gui_app_open(&sp)) kfree(st);
}

GUI_APP_ICON("Control Panel", controlpanel_open, ICON_SETTINGS);

/* =============================================================================
 * The panels that exist today.  Each is a REGISTRATION, and the ones with no
 * `open` are rendered by the generic CONFIG_KEY panel — the setting itself is
 * declared next to the code that reads it, not here.
 * ============================================================================= */

/* Personalisation was FOLDED INTO "Appearance" (kernel/gui/theme.c).
 * It held exactly two keys — the wallpaper and its fit — and answered the same
 * question the theme, density, text-size and icon-size keys answer: how does
 * the desktop look.  Two pages meant two rows wanting the same icon, and the
 * icon set has exactly one for "appearance"; picking a second, less accurate
 * one to break the tie would have made the menu honest-looking and wrong.
 * The keys are unchanged, so nothing anybody saved stops working. */

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
