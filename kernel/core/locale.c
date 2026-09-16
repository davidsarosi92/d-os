/* =============================================================================
 * locale.c — the string catalogue's lookup, plus the English and Hungarian
 * catalogues (§M69).  See locale.h for the four decisions behind the shape.
 * ========================================================================= */

#include "locale.h"
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "config.h"
#include "settings.h"
#include "printf.h"
#include "gui.h"
#include <stddef.h>

extern const struct locale_catalog* const __start_locale_catalogs[];
extern const struct locale_catalog* const __stop_locale_catalogs[];

int locale_count(void) {
    return (int)(__stop_locale_catalogs - __start_locale_catalogs);
}

const struct locale_catalog* locale_at(int i) {
    if (i < 0 || i >= locale_count()) return NULL;
    return __start_locale_catalogs[i];
}

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* The ACTIVE catalogue, and English kept separately as the fallback.  Both are
 * resolved once in locale_set rather than searched per lookup: `lstr` is called
 * from draw paths, and a registry walk per string would put a linear scan
 * inside every frame. */
static const struct locale_catalog* g_active;
static const struct locale_catalog* g_english;
static char g_lang[8] = "en";

static const struct locale_catalog* find_lang(const char* lang) {
    for (int i = 0; i < locale_count(); i++) {
        const struct locale_catalog* c = locale_at(i);
        if (c && streq(c->lang, lang)) return c;
    }
    return NULL;
}

static const char* lookup_in(const struct locale_catalog* c, const char* key) {
    if (!c || !c->entries) return NULL;
    /* A LINEAR SCAN, deliberately.  A catalogue is a few hundred entries and a
     * lookup happens per drawn string, so this is not free — but a hash table
     * here would need an allocator at registration time, and the registration
     * is a linker section that exists before the heap does.  If it ever
     * measures, the fix is a sorted table plus a binary search, which is a
     * build-time property and needs no runtime structure. */
    for (int i = 0; i < c->count; i++)
        if (streq(c->entries[i].key, key)) return c->entries[i].text;
    return NULL;
}

const char* lstr(const char* key) {
    if (!key) return "";
    /* LAZY RESOLUTION, and it is a correctness property rather than a
     * convenience.  `locale_init` runs after the persistent store is overlaid
     * — which is inside the "the disk mounted" branch — so on a machine with
     * no writable volume it never ran at all, and EVERY lookup fell through to
     * the key: the file manager rendered `btn.up`, `col.name`, `menu.file`.
     *
     * *The DEFAULT language must not depend on a disk.*  The catalogues are a
     * linker section and exist before the heap does, so the built-in fallback
     * can always be resolved on first use; the store's job is only to OVERRIDE
     * it.  (§M63 stage 0's defect in a new costume: initialisation hanging off
     * a conditional path, and the failure is silent because the fallback
     * looked deliberate.) */
    if (!g_active && !g_english) g_active = g_english = find_lang("en");
    const char* s = lookup_in(g_active, key);
    if (s) return s;
    s = lookup_in(g_english, key);
    if (s) return s;
    /* THE KEY ITSELF.  See locale.h: a blank button is a bug report, a visible
     * `btn.cancel` is a to-do list. */
    return key;
}

const char* locale_lang(void) { return g_lang; }

int locale_set(const char* lang) {
    if (!lang || !lang[0]) return -1;
    const struct locale_catalog* c = find_lang(lang);
    if (!c) {
        kprintf("locale: no catalogue for '%s' — keeping '%s'\n", lang, g_lang);
        return -1;
    }
    g_active = c;
    if (!g_english) g_english = find_lang("en");
    int i = 0;
    while (lang[i] && i < (int)sizeof g_lang - 1) { g_lang[i] = lang[i]; i++; }
    g_lang[i] = 0;
    kprintf("locale: %s (%s), %d catalogue(s) registered\n",
            c->lang, c->name, locale_count());
    return 0;
}

/* §M63 — the key lives next to the code that READS it, and the watcher is what
 * makes a change take effect on the boot it was made rather than the next one.
 * The re-layout is not optional: a translated caption is a DIFFERENT WIDTH, and
 * a layout computed for the old one leaves controls overlapping their labels. */
static void locale_watch(const char* key, const char* value) {
    (void)key;
    if (locale_set(value) == 0) gui_relayout_all();
}

CONFIG_KEY(ck_language) = {
    .key = "locale.language", .group = "Region and input", .type = CFG_ENUM,
    .values = "en hu", .def = "en",
    .help = "interface language",
    .scope = CFG_SCOPE_USER,
};
CONFIG_WATCH(cw_language) = { .prefix = "locale.language", .changed = locale_watch };

void locale_init(void) {
    g_english = find_lang("en");
    locale_set(config_get("locale.language", "en"));
}

void locale_cmd(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) {
        kprintf("locale: active '%s'; registered:\n", g_lang);
        for (int i = 0; i < locale_count(); i++) {
            const struct locale_catalog* c = locale_at(i);
            /* NO WIDTH SPECIFIERS in this printf — CLAUDE.md states it and
             * §M65 paid for it once; "%-4s" prints literally and takes the
             * next argument as the count. */
            kprintf("  %s\t%s (%d strings)%s\n", c->lang, c->name, c->count,
                    c == g_active ? "  <- active" : "");
        }
        /* A sample, so `locale` also answers "is the catalogue actually being
         * used" rather than only "is it registered" — the two failed
         * separately once already in this tree (§M52's shape). */
        kprintf("  sample: menu.file = '%s', btn.cancel = '%s'\n",
                lstr("menu.file"), lstr("btn.cancel"));
        return;
    }
    /* ONE ROUTE: config_apply records the decision and the §M63 watcher above
     * calls locale_set.  Calling locale_set here as well would switch the
     * language twice per command and, worse, would leave a path that changes
     * the language WITHOUT recording it — which is how a setting stops
     * surviving a reboot. */
    if (!find_lang(args)) {
        kprintf("locale: no catalogue for '%s'\n", args);
        return;
    }
    config_apply("locale.language", args);
}

/* ===========================================================================
 * The catalogues.
 *
 * ONE FILE FOR NOW, TWO LANGUAGES.  They are side by side deliberately while
 * the set is small: a missing Hungarian string is visible as a missing LINE
 * rather than as a difference between two files nobody diffs.  When the set
 * grows past a screen the split is mechanical, and `LOCALE_CATALOG` already
 * makes it a file rather than an edit here.
 *
 * THE HUNGARIAN IS IN ISO-8859-2, NOT UTF-8 — §4.66's font is indexed by byte
 * and ő/ű do not exist in Latin-1, which is why that encoding was chosen.  The
 * escapes below are those byte values; writing the letters literally would put
 * whatever this editor saves in as the string and render as noise.
 *
 * THAT IS NOT HYPOTHETICAL: one entry here was written with a literal UTF-8
 * a-acute (C3 A1) while its neighbours used the escape, and it reached the
 * screen as two wrong glyphs in the middle of an otherwise correct sentence -
 * reported from use as *"the accent is broken in this one word and fine in the
 * next."*  A rule stated in a header is not a rule the compiler checks; what
 * finds it is a grep for any byte >= 0x80 on a line holding an entry.
 * ========================================================================= */

static const struct locale_entry en_strings[] = {
    { "menu.file",        "File" },
    { "menu.view",        "View" },
    { "menu.go",          "Go" },
    { "chart.nodata",    "no samples yet" },
    { "btn.ok",           "OK" },
    { "btn.cancel",       "Cancel" },
    { "btn.save",         "Save" },
    { "btn.up",           "Up" },
    { "btn.mkdir",        "MkDir" },
    { "btn.touch",        "Touch" },
    { "btn.rename",       "Ren" },
    { "btn.copy",         "Copy" },
    { "btn.delete",       "Del" },
    { "btn.view",         "View" },
    { "btn.refresh",      "Refresh" },
    { "taskbar.start",    "Start" },
    { "app.filemanager",  "File Manager" },
    { "app.taskmanager",  "Task Manager" },
    { "app.controlpanel", "Control Panel" },
    { "app.editor",       "Editor" },
    { "cp.hint",          "Settings - double-click a category" },
    { "dlg.deletedir",    "Delete directory" },
    { "dlg.notempty",     "This directory is not empty." },
    { "dlg.alsodeleted",  "Everything inside it will be deleted too." },
    { "dlg.deletetree",   "Delete tree" },
    { "fm.confirm",       "confirm the deletion" },
    { "fm.treedeleted",   "tree deleted" },
    { "fm.selectfirst",   "select an entry first" },
    { "settings.hint",    "change a setting, then Save" },
    { "menu.exitgui",     "Exit GUI" },
    { "menu.reboot",      "Reboot" },
    { "menu.shutdown",    "Shut Down" },
    { "col.name",         "NAME" },
    { "col.size",         "SIZE" },
    { "empty.default",    "Nothing to show" },
    { "empty.folder",     "This folder is empty" },
    { "empty.folder.act", "Use MkDir or Touch to add something" },
    /* Setting NAMES.  The key is its own catalogue key (settings.c), so an
     * undeclared one still shows the identifier -- which is the behaviour
     * this replaced, and the right fallback for a key nobody has named. */
    { "gui.wallpaper",      "Wallpaper" },
    { "gui.wallpaper_fit",  "Wallpaper fit" },
    { "gui.theme",          "Theme" },
    { "gui.density",        "Density" },
    { "gui.font",           "Font" },
    { "gui.font_scale",     "Font scale" },
    { "gui.icon_size",      "Icon size" },
    { "gui.shell",          "Desktop shell" },
    { "gui.scroll_invert",  "Reverse wheel direction" },
    { "gui.scroll_lines",   "Rows per wheel notch" },
    { "gui.autostart",      "Start the desktop at boot" },
    { "gui.mode",           "Resolution" },
    { "gui.scrollback",     "Terminal scrollback" },
    { "desktop.view",       "Desktop layout" },
    { "controlpanel.view",  "Control Panel layout" },
    { "fileman.view",       "File Manager layout" },
    { "devices.view",       "Device list layout" },
    { "keyboard.layout",    "Keyboard layout" },
    { "locale.language",    "Language" },
    { "audio.volume",       "Volume" },
    { "audio.muted",        "Muted" },
    { "boot.splash",        "Boot splash" },
    { "modules.autoload",   "Load modules at boot" },
    { "drivers.rescan_ms",  "Device rescan interval" },
    { "driver.restart_max", "Driver restart limit" },
    { "kernel.fault_policy","On a kernel fault" },
    { "kernel.selftest_ms", "Self-test window" },
    { "crash.report",       "Show crash reports" },
    { "pkg.store",          "Package store" },
    { "gui.drag_stats",     "Log window-drag timings" },
    { "gui.occlude",        "Skip what a window covers" },
    { "gui.close_grace_ms", "Grace before a close is forced" },
    { "gui.close_forces_kill", "Second close forces a kill" },
    { "gui.mode_confirm_s", "Seconds to confirm a new mode" },
    { "bus.allow-adaptation", "Allow contract adapters on the bus" },
    { "driver.edu.dma_bits", "edu device address width" },
    /* Setting VALUES.  The value is an IDENTIFIER -- it is what config_apply
     * stores -- so it is translated only where it is DISPLAYED: the radio and
     * the dropdown both hand back an INDEX, and the text written to the store
     * comes from the descriptor, never from the label. */
    { "light",              "Light" },
    { "dark",               "Dark" },
    { "fill",               "Fill" },
    { "stretch",            "Stretch" },
    { "center",             "Centre" },
    { "tile",               "Tile" },
    { "comfort",            "Comfortable" },
    { "compact",            "Compact" },
    { "vector",             "Vector" },
    { "bitmap",             "Bitmap" },
    { "grid",               "Grid" },
    { "list",               "List" },
    { "table",              "Table" },
    { "vista",              "Vista" },
    { "bare",               "Bare" },
    { "halt",               "Halt" },
    { "reboot",             "Reboot" },
    { "kill",               "Kill the process" },
    { "off",                "Off" },
    { "on",                 "On" },
    { "quiet",              "Quiet" },
    { "ram",                "In memory" },
    { "disk",               "On disk" },
    /* Language and keymap CODES.  Numbers (24/32/48) are left alone: a
     * number is the same in every language, and an entry for one would be
     * a translation nobody can get wrong and everybody has to maintain. */
    { "en",                 "English" },
    { "hu",                 "Magyar" },
    { "us",                 "US" },
    { "set.unsaved_1",    "unsaved change - press Save to apply it" },
    { "set.unsaved_n",    "unsaved changes - press Save to apply them" },
    { "set.rejected",     "rejected - not a valid value for this setting" },
    { "set.savefail",     "save FAILED" },
    { "set.applied_ram",  "applied - RAM only, no writable volume" },
    { "set.applied",      "applied and saved" },
    { "set.nochange",     "saved - nothing had changed" },
    { "fm.deleted",       "deleted" },
    { "fm.delfail",       "delete failed (read-only volume?)" },
    { "fm.dlgbusy",       "cannot ask right now - a dialog is open" },
    { "fm.mkdirok",       "directory created" },
    { "fm.mkdirfail",     "mkdir failed (exists? read-only?)" },
    { "fm.touchok",       "file created" },
    { "fm.touchfail",     "create failed (exists? read-only?)" },
    /* Settings-group identifiers — English is the key, see controlpanel.c. */
    { "Appearance",       "Appearance" },
    { "System",           "System" },
    { "Region and input", "Region and input" },
    { "Sound",            "Sound" },
    { "Display",          "Display" },
    { "Devices",          "Devices" },
    /* THE ENGLISH NAME AS THE KEY, for app-registry identifiers only — see
     * shell_vista.c's menu_label.  `gui_app_def.name` is what a `.lnk` stores
     * and what §M64's resolver matches, so it stays English in the registry
     * and is translated only where it is DISPLAYED. */
    { "File Manager",     "File Manager" },
    { "Task Manager",     "Task Manager" },
    { "Control Panel",    "Control Panel" },
    { "Editor",           "Editor" },
};

LOCALE_CATALOG(loc_en) = {
    .lang = "en", .name = "English",
    .entries = en_strings,
    .count = (int)(sizeof en_strings / sizeof en_strings[0]),
};

/* ISO-8859-2: \xE9 = e-acute, \xE1 = a-acute, \xF3 = o-acute, \xF6 = o-umlaut,
 * \xFC = u-umlaut, \xFA = u-acute, \xED = i-acute, \xF5 = o-double-acute,
 * \xFB = u-double-acute.  The last two are the reason §4.66 chose Latin-2. */
static const struct locale_entry hu_strings[] = {
    { "menu.file",        "F\xE1jl" },
    { "menu.view",        "N\xE9zet" },
    { "menu.go",          "Ugr\xE1s" },
    { "chart.nodata",    "m\xE9g nincs minta" },
    { "btn.ok",           "OK" },
    { "btn.cancel",       "M\xE9gse" },
    { "btn.save",         "Ment\xE9s" },
    { "btn.up",           "Fel" },
    { "btn.mkdir",        "\xDAj mappa" },
    { "btn.touch",        "\xDAj f\xE1jl" },
    { "btn.rename",       "\xC1tnevez" },
    { "btn.copy",         "M\xE1sol" },
    { "btn.delete",       "T\xF6r\xF6l" },
    { "btn.view",         "Megnyit" },
    { "btn.refresh",      "Friss\xEDt" },
    { "taskbar.start",    "Start" },
    { "app.filemanager",  "F\xE1jlkezel\xF5" },
    { "app.taskmanager",  "Feladatkezel\xF5" },
    { "app.controlpanel", "Vez\xE9rl\xF5pult" },
    { "app.editor",       "Szerkeszt\xF5" },
    { "cp.hint",          "Be\xE1ll\xEDt\xE1sok - kattints dupl\xE1n egy kateg\xF3ri\xE1ra" },
    { "dlg.deletedir",    "Mappa t\xF6rl\xE9se" },
    { "dlg.notempty",     "Ez a mappa nem \xFCres." },
    { "dlg.alsodeleted",  "A benne l\xE9v\xF5 minden t\xF6rl\xF5\x64ik." },
    { "dlg.deletetree",   "Teljes t\xF6rl\xE9s" },
    { "fm.confirm",       "er\xF5s\xEDtsd meg a t\xF6rl\xE9st" },
    { "fm.treedeleted",   "a mappa t\xF6r\xF6lve" },
    { "fm.selectfirst",   "el\xF5sz\xF6r v\xE1lassz egy elemet" },
    { "settings.hint",    "m\xF3\x64os\xEDts egy be\xE1ll\xEDt\xE1st, majd Ment\xE9s" },
    { "menu.exitgui",     "Kil\xE9p\xE9s a fel\xFCletr\xF5l" },
    { "menu.reboot",      "\xDAjraind\xEDt\xE1s" },
    { "menu.shutdown",    "Le\xE1ll\xEDt\xE1s" },
    { "col.name",         "N\xC9V" },
    { "col.size",         "M\xC9RET" },
    { "empty.default",    "Nincs megjelen\xEDthet\xF5 elem" },
    { "empty.folder",     "Ez a mappa \xFCres" },
    { "empty.folder.act", "Hozz l\xE9tre valamit az \xDAj mappa vagy \xDAj f\xE1jl gombbal" },
    { "gui.wallpaper",      "H\xE1tt\xE9rk\xE9p" },
    { "gui.wallpaper_fit",  "H\xE1tt\xE9rk\xE9p igaz\xEDt\xE1sa" },
    { "gui.theme",          "T\xE9ma" },
    { "gui.density",        "S\xFBr\xFBs\xE9g" },
    { "gui.font",           "Bet\xFBt\xEDpus" },
    { "gui.font_scale",     "Bet\xFBm\xE9ret" },
    { "gui.icon_size",      "Ikonm\xE9ret" },
    { "gui.shell",          "Asztali fel\xFClet" },
    { "gui.scroll_invert",  "Ford\xEDtott g\xF6rget\xE9si ir\xE1ny" },
    { "gui.scroll_lines",   "Sorok egy kattan\xE1sra" },
    { "gui.autostart",      "Asztal indul\xE1sa bootkor" },
    { "gui.mode",           "Felbont\xE1s" },
    { "gui.scrollback",     "Termin\xE1l el\xF5zm\xE9ny" },
    { "desktop.view",       "Asztal elrendez\xE9se" },
    { "controlpanel.view",  "Vez\xE9rl\xF5pult elrendez\xE9se" },
    { "fileman.view",       "F\xE1jlkezel\xF5 elrendez\xE9se" },
    { "devices.view",       "Eszk\xF6zlista elrendez\xE9se" },
    { "keyboard.layout",    "Billenty\xFBzetkioszt\xE1s" },
    { "locale.language",    "Nyelv" },
    { "audio.volume",       "Hanger\xF5" },
    { "audio.muted",        "N\xE9m\xEDtva" },
    { "boot.splash",        "Ind\xEDt\xF3k\xE9perny\xF5" },
    { "modules.autoload",   "Modulok bet\xF6lt\xE9se indul\xE1skor" },
    { "drivers.rescan_ms",  "Eszk\xF6zkeres\xE9s gyakoris\xE1ga" },
    { "driver.restart_max", "Illeszt\xF5program \xFAjraind\xEDt\xE1si korl\xE1t" },
    { "kernel.fault_policy","Kernelhiba eset\xE9n" },
    { "kernel.selftest_ms", "\xD6nteszt id\xF5\x61\x62lak" },
    { "crash.report",       "Hibajelent\xE9sek mutat\xE1sa" },
    { "pkg.store",          "Csomagt\xE1r" },
    { "gui.drag_stats",     "Ablakh\xFAz\xE1s id\xF5inek napl\xF3z\xE1sa" },
    { "gui.occlude",        "Takart ter\xFClet kihagy\xE1sa" },
    { "gui.close_grace_ms", "T\xFCrelmi id\xF5 a bez\xE1r\xE1s el\xF5tt" },
    { "gui.close_forces_kill", "M\xE1sodik bez\xE1r\xE1s kik\xE9nyszer\xEDti" },
    { "gui.mode_confirm_s", "M\xE1sodperc az \xFAj m\xF3\x64 meger\xF5s\xEDt\xE9s\xE9re" },
    { "bus.allow-adaptation", "Adapterek enged\xE9lyez\xE9se a buszon" },
    { "driver.edu.dma_bits", "edu eszk\xF6z c\xEDmsz\xE9less\xE9g" },
    { "light",              "Vil\xE1gos" },
    { "dark",               "S\xF6t\xE9t" },
    { "fill",               "Kit\xF6lt\xE9s" },
    { "stretch",            "Ny\xFAjt\xE1s" },
    { "center",             "K\xF6z\xE9pre" },
    { "tile",               "Mozaik" },
    { "comfort",            "K\xE9nyelmes" },
    { "compact",            "T\xF6m\xF6r" },
    { "vector",             "Vektoros" },
    { "bitmap",             "Bitmap" },
    { "grid",               "R\xE1\x63s" },
    { "list",               "Lista" },
    { "table",              "T\xE1\x62l\xE1zat" },
    { "vista",              "Vista" },
    { "bare",               "Csupasz" },
    { "halt",               "Meg\xE1ll\xEDt\xE1s" },
    { "reboot",             "\xDAjraind\xEDt\xE1s" },
    { "kill",               "A folyamat lel\xF5\xE9se" },
    { "off",                "Ki" },
    { "on",                 "Be" },
    { "quiet",              "Csendes" },
    { "ram",                "Mem\xF3ri\xE1\x62\x61n" },
    { "disk",               "Lemezen" },
    { "en",                 "Angol" },
    { "hu",                 "Magyar" },
    { "us",                 "Amerikai" },
    { "set.unsaved_1",    "mentetlen m\xF3\x64os\xEDt\xE1s - a Ment\xE9s alkalmazza" },
    { "set.unsaved_n",    "mentetlen m\xF3\x64os\xEDt\xE1sok - a Ment\xE9s alkalmazza \xF5ket" },
    { "set.rejected",     "elutas\xEDtva - nem \xE9rv\xE9nyes \xE9rt\xE9k" },
    { "set.savefail",     "a ment\xE9s NEM SIKER\xDCLT" },
    { "set.applied_ram",  "alkalmazva - csak mem\xF3ri\xE1\x62\x61n, nincs \xEDrhat\xF3 k\xF6tet" },
    { "set.applied",      "alkalmazva \xE9s elmentve" },
    { "set.nochange",     "elmentve - nem v\xE1ltozott semmi" },
    { "fm.deleted",       "t\xF6r\xF6lve" },
    { "fm.delfail",       "a t\xF6rl\xE9s nem siker\xFClt (\xEDr\xE1sv\xE9\x64\x65tt k\xF6tet?)" },
    { "fm.dlgbusy",       "most nem lehet k\xE9rdezni - egy dial\xF3gus nyitva van" },
    { "fm.mkdirok",       "mappa l\xE9trehozva" },
    { "fm.mkdirfail",     "a mappa l\xE9trehoz\xE1sa nem siker\xFClt" },
    { "fm.touchok",       "f\xE1jl l\xE9trehozva" },
    { "fm.touchfail",     "a f\xE1jl l\xE9trehoz\xE1sa nem siker\xFClt" },
    { "Appearance",       "Megjelen\xE9s" },
    { "System",           "Rendszer" },
    { "Region and input", "R\xE9gi\xF3 \xE9s bevitel" },
    { "Sound",            "Hang" },
    { "Display",          "K\xE9pern\xF5" },
    { "Devices",          "Eszk\xF6z\xF6k" },
    { "File Manager",     "F\xE1jlkezel\xF5" },
    { "Task Manager",     "Feladatkezel\xF5" },
    { "Control Panel",    "Vez\xE9rl\xF5pult" },
    { "Editor",           "Szerkeszt\xF5" },
};

LOCALE_CATALOG(loc_hu) = {
    .lang = "hu", .name = "Magyar",
    .entries = hu_strings,
    .count = (int)(sizeof hu_strings / sizeof hu_strings[0]),
};

/* --- §M70 shell registration ----------------------------------------------- */
SHELL_CMD(locale) = { "locale", "[<lang>]", "the string catalogue, and a sample lookup",
                      SHELL_G_SYS, locale_cmd, SHELL_P_ANY };
