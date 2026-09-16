/* =============================================================================
 * theme.c — the Console Plate tokens, and the one assignment that switches them.
 *
 * The colours are copied VERBATIM from `design/console_plate.h`; the densities
 * are this kernel's rescaled pair (see console_plate.h's header for why the
 * geometry had to move and the colours did not).  Keeping the colours literal
 * rather than "improved" is deliberate: the browser reference in
 * design/reference/ is the visual truth we are checked against, and a tweaked
 * token would make every later comparison ambiguous.
 *
 * ONE DEFINITION, NOT A COPY PER FILE.  The design header declared both themes
 * `static const`, which is correct for a single-file browser build and wrong
 * here — every translation unit including it would get its own pair, and a
 * theme switch would then have to find all of them.  They are defined once here
 * and reached through a pointer.
 *
 * WHY A CONFIG KEY RATHER THAN A SETTER.  §M63 already solved "a setting that
 * survives a reboot, applies immediately by whatever route it changed, and
 * appears in the Control Panel with no per-key UI code".  Declaring the key next
 * to the code that reads it is that milestone's rule, and it means the theme
 * switch works from `setconf`, from the persistent store being overlaid at
 * mount, and from the settings panel — without three code paths.
 * ============================================================================= */

#include "console_plate.h"
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "config.h"
#include "settings.h"
#include "vfont.h"
#include "gui.h"      /* CONFIG_KEY lives here, not in config.h */
#include "icons.h"
#include "gui.h"      /* ICON_BRUSH for the panel row */
#include "printf.h"
#include "klog.h"
#include <stddef.h>

/* --- the tokens ---------------------------------------------------------- */

const cp_theme cp_dark = {
    .bg = 0xFF16232F, .surface = 0xFF1D2D3D, .sunken = 0xFF16232F,
    .raised = 0xFF22374A, .tray = 0xFF1A2937,
    .line = 0xFF416180, .line_soft = 0xFF2C455D,
    .text = 0xFFEEF6FF, .muted = 0xFF94BCE3,
    .accent = 0xFF94BCE3, .on_accent = 0xFF16232F,
    .hover = 0xFF2C455D, .press = 0xFF416180,
    .switch_off = 0xFF416180, .knob = 0xFFFFFFFF,
    .sel_bg = 0xFF416180, .sel_fg = 0xFFEEF6FF,
    .focus = 0x8094BCE3,
    .shadow_sm = {  1,  2, 0x66000000 },
    .shadow_md = {  3, 10, 0x6B000000 },
    .shadow_lg = { 14, 34, 0x8C000000 },
};

const cp_theme cp_light = {
    .bg = 0xFFE7E7EA, .surface = 0xFFF5F5F8, .sunken = 0xFFFFFFFF,
    .raised = 0xFFFFFFFF, .tray = 0xFFE7E7EA,
    .line = 0xFFD4D4D7, .line_soft = 0xFFE7E7EA,
    .text = 0xFF1D1F20, .muted = 0xFF5D5D60,
    .accent = 0xFF5980A6, .on_accent = 0xFFFFFFFF,
    .hover = 0xFFEEF6FF, .press = 0xFFD6EBFF,
    .switch_off = 0xFFB7B7BA, .knob = 0xFFFFFFFF,
    .sel_bg = 0xFFD6EBFF, .sel_fg = 0xFF1D1F20,
    .focus = 0x735980A6,
    .shadow_sm = {  1,  2, 0x242B2B2D },
    .shadow_md = {  3, 10, 0x292B2B2D },
    .shadow_lg = { 12, 32, 0x382B2B2D },
};

/* Rescaled against the 8x8 font — see console_plate.h.  The design's own pair
 * was { 40, 18, 40, 15, 14 } and { 32, 14, 32, 14, 13 }. */
/* THE DESIGN'S OWN NUMBERS.  An earlier version shrank these to 24/16 px to
 * "keep the proportions against an 8 px font" — which was the reasoning
 * backwards, and a screenshot is what showed it: at 1920x1200 the result was a
 * 28 px taskbar and 8 px text, i.e. a miniature of the design rather than the
 * design.  These are LOGICAL PIXELS and our screen is BIGGER than the 1400x860
 * canvas they were drawn for, not smaller.
 *
 * THE FONT, HOWEVER, STAYS AT 1x, AND THE FIRST ANSWER HERE WAS WRONG.
 * Matching the design's 15 px body with 2x the 8x8 bitmap (16 px) compares the
 * wrong dimension: Barlow at 15 px advances about 7 px per glyph, while this
 * font advances its full 8 px.  At 1x the two are already within a pixel of
 * each other HORIZONTALLY, which is what sets the density of a line of text;
 * at 2x every word became twice as wide as the same word in the design, which
 * reads exactly as "the letters are enormous" — and it was.
 *
 * The remaining mismatch is vertical (8 px against Barlow's ~11 px cap height)
 * and cannot be closed with one bitmap font.  COMPACT is therefore the default
 * density: 32 px rows around an 8 px font is airier than the design's 32/14,
 * but far closer than 40/8 would be. */
const cp_density cp_comfort = { 40, 18, 40, CP_FONT_BODY, CP_FONT_MONO };
const cp_density cp_compact = { 32, 14, 32, CP_FONT_BODY_COMPACT,
                                CP_FONT_MONO_COMPACT };

/* --- the live selection --------------------------------------------------- */

static const cp_theme*   g_theme   = &cp_dark;      /* the design's default */
static const cp_density* g_density = &cp_compact;
static int               g_font_scale = 1;
static int               g_icon_size  = 24;
static int               g_font_kind  = CP_FONT_VECTOR;

const cp_theme*   cp_current_theme(void)   { return g_theme; }
/* THE DENSITY IS DELIVERED IN DEVICE PIXELS, and that is the rule that keeps
 * the two coordinate systems apart.
 *
 * `cp_comfort` / `cp_compact` hold the DESIGN's numbers, drawn for its 1400 px
 * canvas.  This returns them converted for the screen actually attached, so a
 * caller never has to know which kind of pixel it is holding — which matters
 * because the alternative was already tried today and produced a glyph scaled
 * twice (see dev16() in vfont.c).  One rule, stated once:
 *
 *     THE DENSITY IS DEVICE PIXELS.  The raw CP_* macros are DESIGN pixels and
 *     must go through cp_px().
 *
 * Recomputed only when the screen changes size or the density is switched — a
 * mode set (§M61) moves it, so a value computed once at boot would be wrong
 * from the first resolution change onwards. */
const cp_density* cp_current_density(void) {
    static cp_density scaled;
    static const cp_density* last_base;
    static int last_w;

    int w = gui_screen_w();
    if (g_density != last_base || w != last_w) {
        scaled.control_h     = cp_px(g_density->control_h);
        scaled.control_pad_x = cp_px(g_density->control_pad_x);
        scaled.row_h         = cp_px(g_density->row_h);
        scaled.font_body     = cp_px(g_density->font_body);
        scaled.font_mono     = cp_px(g_density->font_mono);
        last_base = g_density;
        last_w = w;
    }
    return &scaled;
}

/* The chrome's own heights, same conversion.  gui.c reaches these through its
 * TITLE_H / CLOSE_W macros, so the chrome scales with everything else instead
 * of staying at the design's absolute pixels while the text around it grows —
 * which is the state the last round left, visible as rows too tight for their
 * own contents. */
int cp_titlebar_h(void)  { return cp_px(CP_TITLEBAR_H); }
int cp_panel_h(void)     { return cp_px(CP_PANEL_H); }
int cp_taskbar_h(void)   { return cp_px(CP_TASKBAR_H); }
int cp_window_btn(void)  { return cp_px(CP_WINDOW_BTN); }
int cp_menu_item_h(void) { return cp_px(CP_MENU_ITEM_H); }
int cp_tab_h(void)       { return cp_px(CP_TAB_H); }
int cp_scrollbar_w(void) { return cp_px(CP_SCROLLBAR_W); }
int               cp_font_scale(void)      { return g_font_scale; }
int               cp_icon_size(void)       { return g_icon_size; }
int               cp_font_kind(void)       { return g_font_kind; }

/* --- config -------------------------------------------------------------- */

/* GROUP "Appearance".  A group name IS a Control Panel page (§M63: a panel with
 * `open = NULL` is rendered generically over every key whose group matches), so
 * these four keys become a page with no UI code written for them.  They are
 * deliberately NOT in "Personalisation" — that page is about the wallpaper, and
 * a page that mixes "which picture" with "how big is the text" is the kind of
 * grab-bag every settings UI drifts into. */

CONFIG_KEY(ck_theme) = {
    .key = "gui.theme", .group = "Appearance", .type = CFG_ENUM,
    .values = "dark light", .def = "dark",
    .help = "Console Plate colour theme",
    .scope = CFG_SCOPE_USER,
};

CONFIG_KEY(ck_density) = {
    .key = "gui.density", .group = "Appearance", .type = CFG_ENUM,
    .values = "comfort compact", .def = "compact",
    .help = "control and row height (24 px / 16 px)",
    .scope = CFG_SCOPE_USER,
};

/* TEXT SIZE.
 *
 * This key used to say "a whole multiple of the 8x8 font", and the wording was
 * doing real work: with one bitmap face there was nothing else it COULD mean,
 * and offering points or small/medium/large would have been a control whose
 * values mostly could not be honoured.
 *
 * §M69 removed that constraint — the faces are outlines now and any pixel size
 * is renderable — but the key keeps its shape on purpose.  The design fixes the
 * body size per DENSITY (15 px comfort, 14 px compact), so the useful knob is
 * "everything bigger", which is what a multiplier is.  A free pixel size would
 * be a second, overlapping way to say the same thing, and the two would
 * disagree the first time somebody set both. */
CONFIG_KEY(ck_font_scale) = {
    .key = "gui.font_scale", .group = "Appearance", .type = CFG_INT,
    .def = "1", .min = 1, .max = 2,
    .help = "text size multiplier (the density sets the base size)",
};

/* WHICH TYPEFACE MACHINERY DRAWS THE TEXT (§M69).
 *
 * `vector` is the design's real faces — Barlow, Barlow Condensed SemiBold and
 * IBM Plex Mono, as outlines.  `bitmap` is the 8x8 font this system had for its
 * whole life, still used by the boot console and the terminal grid.
 *
 * The key exists because a font is the one component whose failure removes the
 * means of diagnosing it: a machine that renders no legible text cannot be
 * asked what went wrong.  §M23 makes the same argument for a taskbar button
 * that is always drawn — keep a way back that does not depend on the thing
 * being tested. */
CONFIG_KEY(ck_font_kind) = {
    .key = "gui.font", .group = "Appearance", .type = CFG_ENUM,
    .values = "vector bitmap", .def = "vector",
    .help = "outline typefaces, or the built-in 8x8 bitmap font",
    .scope = CFG_SCOPE_USER,
};

CONFIG_KEY(ck_icon_size) = {
    .key = "gui.icon_size", .group = "Appearance", .type = CFG_ENUM,
    .values = "24 32 48", .def = "24",
    .help = "desktop and list icon size in pixels",
    .scope = CFG_SCOPE_USER,
};

/* Local rather than reached for: this file needs exactly "leading digits, no
 * sign, no base prefix", and a shared parser would have to be looked up to know
 * what it does with the rest. */
static int cp_atoi(const char* s) {
    int v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static int str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Re-read BOTH keys on any change under the prefix.  Reading both rather than
 * dispatching on `key` is not laziness: the watcher also fires when the
 * persistent store is overlaid at mount, and at that moment either key may have
 * arrived.  Two reads of a cached string cost nothing next to the repaint. */
static void theme_conf_changed(const char* key, const char* value) {
    (void)key; (void)value;

    const cp_theme*   nt = str_eq(config_get("gui.theme", "dark"), "light")
                             ? &cp_light : &cp_dark;
    const cp_density* nd = str_eq(config_get("gui.density", "compact"), "comfort")
                             ? &cp_comfort : &cp_compact;

    /* CLAMPED, not trusted.  `setconf` reaches undeclared and unvalidated keys
     * by design (§M63), so a value that never passed the descriptor's range can
     * arrive here — and a font scale of 0 is an invisible desktop. */
    int nfs = cp_atoi(config_get("gui.font_scale", "1"));
    if (nfs < 1) nfs = 1;
    if (nfs > 2) nfs = 2;

    int nis = cp_atoi(config_get("gui.icon_size", "24"));
    if (nis != 24 && nis != 32 && nis != 48) nis = 24;

    int nfk = str_eq(config_get("gui.font", "vector"), "bitmap")
                ? CP_FONT_BITMAP : CP_FONT_VECTOR;

    if (nt == g_theme && nd == g_density && nfs == g_font_scale &&
        nis == g_icon_size && nfk == g_font_kind) return;    /* no repaint */

    g_theme      = nt;
    g_density    = nd;
    g_font_scale = nfs;
    g_icon_size  = nis;
    g_font_kind  = nfk;
    /* INVALIDATE.  The palette is read live, so without this the change is
     * real and invisible — and worse than invisible: a partial repaint then
     * paints new-theme content into an old-theme window, which is what the
     * streak along the mouse path was. */
    gui_theme_changed();

    klog(KLOG_INFO, "theme", "now %s / %s (control %d px, %s font %d px x%d, icon %d)\n",
         nt == &cp_light ? "light" : "dark",
         nd == &cp_compact ? "compact" : "comfort",
         nd->control_h, nfk == CP_FONT_VECTOR ? "vector" : "bitmap",
         nd->font_body, nfs, nis);

    /* The chrome and the widget toolkit read these tokens live (the palette
     * macros in gui.c / widget.c / w_controls.c expand to a read of the current
     * theme), so the next composite already uses the new values.  What is NOT
     * repainted here is the screen itself: nothing is marked damaged, so the
     * change appears as windows redraw for their own reasons.  A full-desktop
     * invalidation belongs with the compositor, not in a config watcher. */
}

CONFIG_WATCH(theme_watch) = {
    .prefix  = "gui.theme",      /* also covers gui.density via the two reads */
    .changed = theme_conf_changed,
};

CONFIG_WATCH(density_watch) = {
    .prefix  = "gui.density",
    .changed = theme_conf_changed,
};

CONFIG_WATCH(font_scale_watch) = {
    .prefix  = "gui.font_scale",
    .changed = theme_conf_changed,
};

CONFIG_WATCH(icon_size_watch) = {
    .prefix  = "gui.icon_size",
    .changed = theme_conf_changed,
};

/* THE PAGE ITSELF.  `open` is NULL, so §M63's generic key panel renders every
 * key whose group is "Appearance" — the four above, and anything a later
 * subsystem declares into the same group without touching this file.  That is
 * the whole point of the registry: adding a setting is a LINE, not an app. */
SETTINGS_PANEL(sp_appearance) = {
    .name    = "Appearance",
    .summary = "Theme, density, text and icon size",
    .icon    = ICON_BRUSH,
    .open    = NULL,
};

/* --- the report ----------------------------------------------------------
 * Lives here, not in a shell: `shell.c` is x86-only and aarch64 runs its own
 * `serial_shell.c`, so a command written in either can only be run on the
 * arches that build it (§M24's rule, and §4.63 paid for breaking it).
 *
 * It prints the tokens the NEXT draw would use, which is the only thing worth
 * printing while no widget reads them yet. */
void cp_cmd_theme(const char* arg) {
    if (arg && *arg) {
        /* config_apply, not config_set: apply RECORDS a decision and notifies
         * the watchers, which is what makes the change take effect now rather
         * than at the next boot (§M63 stage 0). */
        const char* key = (str_eq(arg, "comfort") || str_eq(arg, "compact"))
                            ? "gui.density" : "gui.theme";
        if (config_apply(key, arg) != 0) {
            kprintf("theme: '%s' is not one of dark|light|comfort|compact\n", arg);
            return;
        }
    }

    const cp_theme*   t = g_theme;
    /* The LIVE density — the scaled one every painter uses.  Reading g_density
     * here printed the design's numbers while the screen showed device pixels:
     * a report that answers from a different source than the code it describes
     * is the one kind of report worse than none (§M64's `devices` walks the
     * panel's own cell() for exactly this reason). */
    const cp_density* d = cp_current_density();

    kprintf("theme %s, density %s\n",
            t == &cp_light ? "light" : "dark",
            d == &cp_compact ? "compact" : "comfort");
    kprintf("  bg %x  surface %x  sunken %x  raised %x  tray %x\n",
            t->bg, t->surface, t->sunken, t->raised, t->tray);
    kprintf("  line %x  line_soft %x  text %x  muted %x  accent %x\n",
            t->line, t->line_soft, t->text, t->muted, t->accent);
    kprintf("  hover %x  press %x  sel_bg %x  focus %x\n",
            t->hover, t->press, t->sel_bg, t->focus);
    /* SAY WHICH PIXELS THESE ARE.  Run from the boot shell there is no
     * framebuffer yet, so the scale is 100 % and these are the design's own
     * numbers; run with the desktop up they are device pixels.  Printing them
     * without the factor makes the two indistinguishable — and the first thing
     * that confused was me, reading 14 px off a screen rendering 19. */
    kprintf("  ui scale %d%% of the design's 1400 px canvas%s\n",
            cp_px(100), gui_screen_w() > 0 ? "" : " (no framebuffer yet)");
    kprintf("  control %d, pad_x %d, row %d, body %d px, mono %d px\n",
            d->control_h, d->control_pad_x, d->row_h, d->font_body,
            d->font_mono);
    kprintf("  titlebar %d, panel %d, taskbar %d, menu item %d, tab %d\n",
            CP_TITLEBAR_H, CP_PANEL_H, CP_TASKBAR_H, CP_MENU_ITEM_H, CP_TAB_H);
    kprintf("  wallpaper is NOT themed: it is a picture the user chose\n");
    {
        /* The glyph cache, in the one place somebody already looks.  A cache
         * whose hit rate nobody can see is a cache nobody can defend when the
         * desktop gets slower — §4.61's argument for the compositor's own
         * counters, one layer down. */
        unsigned n, by, hit, miss, ev;
        vfont_cache_stats(&n, &by, &hit, &miss, &ev);
        kprintf("  glyph cache: %u entries, %u bytes, %u hits, %u misses, "
                "%u evictions\n", n, by, hit, miss, ev);
    }

    if (g_font_scale != 1)
        kprintf("  font_scale %dx is STORED, NOT APPLIED: gfx_text_scaled\n"
                "  exists but the layout still measures in 1x cells (\u00a7M65),\n"
                "  so scaling text without scaling the boxes would overflow them\n",
                g_font_scale);
}

/* --- §M70 shell registration ----------------------------------------------- */
SHELL_CMD(theme) = { "theme", "[light|dark]", "the active theme, and its live density",
                     SHELL_G_GUI, cp_cmd_theme, SHELL_P_ANY };
