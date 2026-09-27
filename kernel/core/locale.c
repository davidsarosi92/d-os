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

/* §M87 — THE TRANSLATION GAP, MEASURED.
 *
 * Reported from use: *"the translation is not complete, several things in the
 * Control Panel are not translated."*  Reading the source for untranslated
 * strings finds the ones somebody thought of; this finds the ones that were
 * actually DRAWN.  With `locale missing on`, every lookup the active catalogue
 * cannot answer is recorded once, and `locale missing` lists them — so the
 * sweep is "switch to Hungarian, open every panel, print the list", and the
 * list is exactly what a Hungarian user saw in English.
 *
 * Off by default: it runs inside every lstr call.  The table is bounded and
 * claims slots with one atomic add, because lstr runs on every task that
 * draws; a lost duplicate under a race costs nothing but a repeated line. */
#define MISSING_MAX 160
static volatile int g_missing_on;
static char g_missing[MISSING_MAX][48];
static volatile int g_missing_n;

static void note_missing(const char* key) {
    int n = g_missing_n;
    if (n > MISSING_MAX) n = MISSING_MAX;
    for (int i = 0; i < n; i++) {
        const char* a = g_missing[i]; const char* b = key;
        int k = 0;
        while (k < 47 && a[k] && a[k] == b[k]) k++;
        if (a[k] == b[k] || k == 47) return;
    }
    int slot = __atomic_fetch_add(&g_missing_n, 1, __ATOMIC_RELAXED);
    if (slot >= MISSING_MAX) return;
    int k = 0;
    for (; key[k] && k < 47; k++) g_missing[slot][k] = key[k];
    g_missing[slot][k] = 0;
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
    if (g_missing_on && g_active != g_english) note_missing(key);
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
    if (args[0] == 'm' && args[1] == 'i') {             /* "missing ..." */
        const char* a = args + 7;
        while (*a == ' ') a++;
        if (a[0] == 'o' && a[1] == 'n') { g_missing_on = 1; g_missing_n = 0;
                                          kprintf("locale: recording misses\n"); return; }
        if (a[0] == 'o' && a[1] == 'f') { g_missing_on = 0; return; }
        int n = g_missing_n > MISSING_MAX ? MISSING_MAX : g_missing_n;
        kprintf("locale: %d key(s) the '%s' catalogue could not answer:\n", n, g_lang);
        for (int i = 0; i < n; i++) kprintf("  MISSING %s\n", g_missing[i]);
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
    { "menu.lock",        "Lock" },
    { "menu.signout",     "Sign out" },
    { "menu.exitgui",     "Exit GUI" },
    { "menu.reboot",      "Reboot" },
    { "menu.shutdown",    "Shut Down" },
    { "tray.kbdlayout",   "Keyboard layout" },
    { "tray.nolayouts",   "no layouts" },
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
    { "console.serial_commands", "Run commands arriving on COM1" },
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
    { "set.close_title",  "Unsaved settings" },
    { "set.close_body",   "These settings have not been applied yet. Save them before closing?" },
    { "set.close_body_session", "The session is ending. These settings have not been applied yet - save them?" },
    { "btn.discard",      "Discard" },
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
    { "Locked", "Locked" },
    { "lock.prompt", "This screen is locked.  Choose an account and sign in." },
    { "lock.password", "Password" },
    { "lock.signin", "Sign in" },
    { "Network", "Network" },
    { "adapters, addresses, Wi-Fi", "adapters, addresses, Wi-Fi" },
    { "Disks", "Disks" },
    { "mount, unmount, format, RAM disks", "mount, unmount, format, RAM disks" },
    { "User accounts", "User accounts" },
    { "who may use this machine, and what each of them may do", "who may use this machine, and what each of them may do" },
    { "boot, faults, crash reporting", "boot, faults, crash reporting" },
    { "keyboard layout", "keyboard layout" },
    { "output volume and mute", "output volume and mute" },
    { "screen resolution", "screen resolution" },
    { "Theme, density, text and icon size", "Theme, density, text and icon size" },
    { "drivers, where they run, what they hold", "drivers, where they run, what they hold" },
    { "CATEGORY", "CATEGORY" },
    { "WHAT IT CONTAINS", "WHAT IT CONTAINS" },
    { "OK", "OK" },
    { "Cancel", "Cancel" },
    { "Save", "Save" },
    { "Delete", "Delete" },
    { "New", "New" },
    { "Enable", "Enable" },
    { "Disable", "Disable" },
    { "Wi-Fi", "Wi-Fi" },
    { "net.dhcp", "Use DHCP at boot" },
    { "net.static", "Use a static address" },
    { "net.static.ip", "Static IP address" },
    { "net.static.netmask", "Static subnet mask" },
    { "net.static.gateway", "Static gateway" },
    { "net.static.dns", "Static DNS server" },
    { "net.wifisim", "Simulated Wi-Fi adapter" },
    { "ask a DHCP server for an address at boot", "ask a DHCP server for an address at boot" },
    { "use the static address below instead of the default or DHCP", "use the static address below instead of the default or DHCP" },
    { "static IPv4 address, e.g. 192.168.1.20", "static IPv4 address, e.g. 192.168.1.20" },
    { "static subnet mask", "static subnet mask" },
    { "static default gateway", "static default gateway" },
    { "static DNS server", "static DNS server" },
    { "add a SIMULATED Wi-Fi adapter (tests the chooser; carries no traffic)", "add a SIMULATED Wi-Fi adapter (tests the chooser; carries no traffic)" },
    { "audit.interval_s", "Invariant audit interval (s)" },
    { "run the runtime invariant checks every N seconds (0 = only on demand)", "run the runtime invariant checks every N seconds (0 = only on demand)" },
    { "users.autologin", "Automatic sign-in" },
    { "sign in to this account without asking (it must have no password, and be the only account)", "sign in to this account without asking (it must have no password, and be the only account)" },
    { "users.default_user", "Default account" },
    { "the account the sign-in surfaces offer first", "the account the sign-in surfaces offer first" },
    { "gui.logout_grace_ms", "Sign-out grace time (ms)" },
    { "how long signing out / shutting down waits for apps to close (ms)", "how long signing out / shutting down waits for apps to close (ms)" },
    { "gui.page_flip", "Double buffering" },
    { "double-buffered present (off = single buffer; diagnostic)", "double-buffered present (off = single buffer; diagnostic)" },
    { "gui.autorun", "Command after start" },
    { "one shell command to run once the desktop is up (test hook)", "one shell command to run once the desktop is up (test hook)" },
    { "gui.iv_probe", "Item-view damage probe" },
    { "log item-view damage rects against paint extents (diagnostic)", "log item-view damage rects against paint extents (diagnostic)" },
    { "gui.locktest", "Lock screen test" },
    { "diagnostic: drive the lock screen's submit with user:password", "diagnostic: drive the lock screen's submit with user:password" },
    { "how many address bits the edu device has - match QEMU's dma_mask", "how many address bits the edu device has - match QEMU's dma_mask" },
    { "execute lines arriving on COM1 as shell commands (SYSTEM context)", "execute lines arriving on COM1 as shell commands (SYSTEM context)" },
    { "restarts of a ring-3 driver inside 30 s before it is quarantined; 0 = never restart", "restarts of a ring-3 driver inside 30 s before it is quarantined; 0 = never restart" },
    { "interface language", "interface language" },
    { "master output level, 0-100", "master output level, 0-100" },
    { "silence all output without forgetting the level", "silence all output without forgetting the level" },
    { "how often to look for newly attached hardware, 0 = never", "how often to look for newly attached hardware, 0 = never" },
    { "load /modules/*.ko at boot", "load /modules/*.ko at boot" },
    { "skip painting what an opaque window completely covers", "skip painting what an opaque window completely covers" },
    { "reverse the wheel direction (Mac-style natural scrolling)", "reverse the wheel direction (Mac-style natural scrolling)" },
    { "Console Plate colour theme", "Console Plate colour theme" },
    { "control and row height (24 px / 16 px)", "control and row height (24 px / 16 px)" },
    { "text size multiplier (the density sets the base size)", "text size multiplier (the density sets the base size)" },
    { "outline typefaces, or the built-in 8x8 bitmap font", "outline typefaces, or the built-in 8x8 bitmap font" },
    { "desktop and list icon size in pixels", "desktop and list icon size in pixels" },
    { "gradient | solid:RRGGBB | a path to a BMP", "gradient | solid:RRGGBB | a path to a BMP" },
    { "how the image is fitted to the screen", "how the image is fitted to the screen" },
    { "desktop shell (takes effect at the next `gui` start)", "desktop shell (takes effect at the next `gui` start)" },
    { "how desktop shortcuts are arranged", "how desktop shortcuts are arranged" },
    { "how the Control Panel arranges its categories", "how the Control Panel arranges its categories" },
    { "active keyboard layout - applies immediately", "active keyboard layout - applies immediately" },
    { "what a ring-0 fault does (ring-3 always kills just the task)", "what a ring-0 fault does (ring-3 always kills just the task)" },
    { "open the Crash Reports window when a record is delivered", "open the Crash Reports window when a record is delivered" },
    { "print compositor timings when a window drag ends", "print compositor timings when a window drag ends" },
    { "a second click on X force-kills an unresponsive client", "a second click on X force-kills an unresponsive client" },
    { "unattended backstop before a closing window is forced (ms)", "unattended backstop before a closing window is forced (ms)" },
    { "boot self-test window in ms (0 = skip; they cost 0.5 s of boot)", "boot self-test window in ms (0 = skip; they cost 0.5 s of boot)" },
    { "package store: ram rebuilds it each boot (82 ms), disk reuses it (7.8 s of reads)", "package store: ram rebuilds it each boot (82 ms), disk reuses it (7.8 s of reads)" },
    { "how the file manager shows a directory (applies to a new window)", "how the file manager shows a directory (applies to a new window)" },
    { "lines of terminal history kept per window (0 = none; applies to new windows)", "lines of terminal history kept per window (0 = none; applies to new windows)" },
    { "boot into the desktop (the text shell stays behind it: Start > Exit GUI)", "boot into the desktop (the text shell stays behind it: Start > Exit GUI)" },
    { "boot screen (applies at the next boot); the log is only hidden - dmesg keeps it", "boot screen (applies at the next boot); the log is only hidden - dmesg keeps it" },
    { "let the service bus adapt between contract versions", "let the service bus adapt between contract versions" },
    { "rows moved by one wheel notch", "rows moved by one wheel notch" },
    { "seconds before an unconfirmed resolution reverts (0 = no dialog)", "seconds before an unconfirmed resolution reverts (0 = no dialog)" },
    { "confirmed resolution, e.g. 1280x800 (written by the OK button)", "confirmed resolution, e.g. 1280x800 (written by the OK button)" },
    { "how the device manager lays its devices out", "how the device manager lays its devices out" },
    { "(no settings declared for this group)", "(no settings declared for this group)" },
    { "tray.network", "Network" },
    { "tray.secured", "(secured)" },
    { "tray.searching", "Searching for networks..." },
    { "tray.netsettings", "Network settings" },
    { "tray.noaudio", "No audio device" },
    { "tray.noaudio2", "nothing to play through" },
    { "tray.volume", "Volume" },
    { "tray.mute", "[ Mute ]" },
    { "tray.unmute", "[ Unmute ]" },
    { "no network adapter", "no network adapter" },
    { "disabled", "disabled" },
    { "not connected", "not connected" },
    { "no address", "no address" },
    { "connected (wired)", "connected (wired)" },
    { "connected (Wi-Fi)", "connected (Wi-Fi)" },
    { "net.empty", "No network adapters." },
    { "net.hint.select", "Select an adapter." },
    { "net.col.adapter", "Adapter" },
    { "net.col.type", "Type" },
    { "net.col.state", "State" },
    { "net.col.address", "Address" },
    { "net.col.source", "Source" },
    { "net.col.mac", "MAC" },
    { "net.type.loopback", "loopback" },
    { "net.type.wired", "wired" },
    { "net.type.wifi", "Wi-Fi" },
    { "net.type.wifisim", "Wi-Fi (simulated)" },
    { "net.st.disabled", "disabled" },
    { "net.st.up", "connected" },
    { "net.st.upunknown", "connected (link not reported)" },
    { "net.st.nocable", "cable unplugged" },
    { "net.st.notjoined", "not connected" },
    { "net.st.noaddr", "no address" },
    { "net.src.dhcp", "DHCP" },
    { "net.src.static", "static" },
    { "net.src.default", "built-in default" },
    { "net.d.gateway", "gateway" },
    { "net.d.lease", "lease left" },
    { "net.btn.renew", "Renew address" },
    { "net.btn.config", "Configure..." },
    { "net.btn.test", "Test" },
    { "net.msg.nodev", "no such adapter" },
    { "net.msg.leased", "Address from DHCP:" },
    { "net.msg.nodhcp", "No DHCP server answered." },
    { "net.msg.gateway", "Gateway " },
    { "net.msg.answers", "answers." },
    { "net.msg.silent", "does not answer." },
    { "net.msg.dns", "Names " },
    { "net.msg.resolves", "resolve." },
    { "net.msg.fails", "do not resolve." },
    { "net.msg.nowifi", "There is no wireless adapter." },
    { "net.msg.joined", "Connected." },
    { "net.msg.nonet", "That network is not in range." },
    { "net.msg.badpass", "Wrong passphrase." },
    { "net.msg.radio", "The radio did not respond." },
    { "net.msg.left", "Disconnected." },
    { "net.msg.notask", "Could not start the operation." },
    { "net.msg.cannot", "This adapter cannot be switched." },
    { "net.msg.enabled", "Adapter enabled." },
    { "net.msg.disabled", "Adapter disabled." },
    { "net.msg.nodhcphere", "This adapter does not use DHCP." },
    { "net.msg.asking", "Asking a DHCP server..." },
    { "net.msg.busy", "Another network operation is running." },
    { "net.msg.testing", "Testing the connection..." },
    { "net.msg.joining", "Connecting..." },
    { "net.msg.leaving", "Disconnecting..." },
    { "net.msg.defaultreboot", "Saved. The built-in address returns at the next restart." },
    { "net.title.address", "Network address" },
    { "net.mode.default", "Built-in" },
    { "net.mode.dhcp", "Automatic" },
    { "net.mode.static", "Manual" },
    { "net.f.ip", "IP address" },
    { "net.f.mask", "Subnet mask" },
    { "net.f.gw", "Gateway" },
    { "net.f.dns", "DNS server" },
    { "net.hint.static", "Automatic uses DHCP; Manual uses the fields below." },
    { "net.err.ip", "The IP address is not valid." },
    { "net.err.mask", "The subnet mask is not valid." },
    { "net.err.gw", "The gateway is not valid." },
    { "net.err.dns", "The DNS server is not valid." },
    { "wifi.col.network", "Network" },
    { "wifi.col.signal", "Signal" },
    { "wifi.col.security", "Security" },
    { "wifi.col.status", "Status" },
    { "wifi.sec.open", "open" },
    { "wifi.connected", "connected" },
    { "wifi.simulated", "(simulated adapter - no traffic)" },
    { "wifi.empty", "No networks in range." },
    { "wifi.scanned", "Networks refreshed." },
    { "wifi.pick", "Choose a network first." },
    { "wifi.needpass", "This network needs a passphrase." },
    { "wifi.passphrase", "Passphrase" },
    { "wifi.btn.scan", "Search" },
    { "wifi.btn.connect", "Connect" },
    { "wifi.btn.disconnect", "Disconnect" },
    { "disk.empty", "No disks." },
    { "disk.col.disk", "Disk" },
    { "disk.col.size", "Size" },
    { "disk.col.fs", "Filesystem" },
    { "disk.col.mount", "Mounted at" },
    { "disk.col.free", "Free" },
    { "disk.col.note", "Note" },
    { "disk.note.system", "system volume" },
    { "disk.note.ram", "RAM disk" },
    { "disk.hint.select", "Select a disk." },
    { "disk.d.used", "used" },
    { "disk.d.mounted", "mounted" },
    { "disk.d.notmounted", "not mounted" },
    { "disk.d.heldby", "in use by" },
    { "disk.v.mount", "Mount" },
    { "disk.v.umount", "Unmount" },
    { "disk.v.format", "Format" },
    { "disk.v.newram", "New RAM disk" },
    { "disk.v.remove", "Remove" },
    { "disk.v.sync", "Sync" },
    { "disk.btn.mount", "Mount" },
    { "disk.btn.umount", "Unmount" },
    { "disk.btn.format", "Format..." },
    { "disk.btn.sync", "Sync" },
    { "disk.btn.newram", "New RAM disk" },
    { "disk.btn.remove", "Remove" },
    { "disk.btn.formatnow", "Format" },
    { "disk.title.format", "Format disk" },
    { "disk.f.label", "Volume label" },
    { "disk.f.hint", "A new, empty exFAT filesystem is written over the whole disk." },
    { "disk.f.warn", "Everything on this disk will be erased:" },
    { "the settings store", "the settings store" },
    { "empty", "empty" },
    { "unknown", "unknown" },
    { "unreadable", "unreadable" },
    { "partitioned", "partitioned" },
    { "done", "done" },
    { "no such disk", "no such disk" },
    { "the disk is mounted - unmount it first", "the disk is mounted - unmount it first" },
    { "the disk is not mounted", "the disk is not mounted" },
    { "files are open on it", "files are open on it" },
    { "another volume is mounted inside it", "another volume is mounted inside it" },
    { "the system depends on it", "the system depends on it" },
    { "no filesystem this system can mount", "no filesystem this system can mount" },
    { "I/O error", "I/O error" },
    { "only a RAM disk can be removed", "only a RAM disk can be removed" },
    { "not enough memory", "not enough memory" },
    { "this filesystem cannot be unmounted", "this filesystem cannot be unmounted" },
    { "failed", "failed" },
    { "Hardware", "Hardware" },
    { "Where", "Where" },
    { "ID", "ID" },
    { "Driver", "Driver" },
    { "State", "State" },
    { "Runs in", "Runs in" },
    { "Isolation", "Isolation" },
    { "Holds", "Holds" },
    { "Select a device.", "Select a device." },
    { "Start", "Start" },
    { "Stop", "Stop" },
    { "Move", "Move" },
    { "Update", "Update" },
    { "Browse", "Browse" },
    { "Crash", "Crash" },
    { "QUARANTINED", "QUARANTINED" },
    { "stopped", "stopped" },
    { "init failed", "init failed" },
    { "absent", "absent" },
    { "probed", "probed" },
    { "not present", "not present" },
    { "needs a driver", "needs a driver" },
    { "no driver needed", "no driver needed" },
    { "not started", "not started" },
    { "offline", "offline" },
    { "platform", "platform" },
    { "(none)", "(none)" },
    { "(not needed)", "(not needed)" },
    { "NO DRIVER", "NO DRIVER" },
    { "kernel", "kernel" },
    { "ring 3 pid ", "ring 3 pid " },
    { " (->ring 3 on restart)", " (->ring 3 on restart)" },
    { " - driver ", " - driver " },
    { ", can run: ", ", can run: " },
    { " - DMA, device confined", " - DMA, device confined" },
    { " - DMA: ", " - DMA: " },
    { " - boot-critical, cannot be moved", " - boot-critical, cannot be moved" },
    { " - not present; the driver is here if it ever is", " - not present; the driver is here if it ever is" },
    { " - present, and nothing here drives it", " - present, and nothing here drives it" },
    { " - present; nothing drives it and nothing should", " - present; nothing drives it and nothing should" },
    { " - ", " - " },
    { "Account", "Account" },
    { "UID", "UID" },
    { "Role", "Role" },
    { "Sign-in", "Sign-in" },
    { "Home", "Home" },
    { "Administrator (protected)", "Administrator (protected)" },
    { "Administrator", "Administrator" },
    { "Standard", "Standard" },
    { "yes", "yes" },
    { "no password", "no password" },
    { "Select an account.", "Select an account." },
    { "Set password", "Set password" },
    { "Toggle admin", "Toggle admin" },
    { "Auto sign-in", "Auto sign-in" },
    { "Delete account", "Delete account" },
    { "New account name", "New account name" },
    { "New password for ", "New password for " },
    { "Password set for ", "Password set for " },
    { " (", " (" },
    { " chars, fp ", " chars, fp " },
    { ", verified ok).  They can sign in now.", ", verified ok).  They can sign in now." },
    { ", VERIFY FAILED - the record did not take).", ", VERIFY FAILED - the record did not take)." },
    { "NOT changed - ", "NOT changed - " },
    { ".  Acting as ", ".  Acting as " },
    { ".", "." },
    { "No password typed - nothing was changed. (To disable sign-in, use `passwd <name> -` at a shell.)", "No password typed - nothing was changed. (To disable sign-in, use `passwd <name> -` at a shell.)" },
    { "Auto sign-in is off.  The picker will ask.", "Auto sign-in is off.  The picker will ask." },
    { "Signing in to ", "Signing in to " },
    { " automatically from now on (it has no password, so there is nothing to ask).", " automatically from now on (it has no password, so there is nothing to ask)." },
    { "Delete '", "Delete '" },
    { "'?\nIts uid is retired permanently, and the files it owns\nwill then belong to no account.", "'?\nIts uid is retired permanently, and the files it owns\nwill then belong to no account." },
    { "  You are signed in as a standard user, so you may change your own password and nothing else.", "  You are signed in as a standard user, so you may change your own password and nothing else." },
    { " - uid ", " - uid " },
    { ", cannot sign in (no password)", ", cannot sign in (no password)" },
    { ".  root is protected: it cannot be deleted or demoted by anyone, including root.", ".  root is protected: it cannot be deleted or demoted by anyone, including root." },
    { ".  This is an administrator - only root may change or remove one.", ".  This is an administrator - only root may change or remove one." },
    { ".  Not your account: a standard user may change only their own password.", ".  Not your account: a standard user may change only their own password." },
    { "Keep this display mode?", "Keep this display mode?" },
    { "Keeping this resolution in ", "Keeping this resolution in " },
    { "Double-click a resolution", "Double-click a resolution" },
    { "This display cannot change resolution.", "This display cannot change resolution." },
    { "current", "current" },
    { "Choose an account first.", "Choose an account first." },
    { "Incorrect password for '", "Incorrect password for '" },
    { " - that account has no sign-in secret", " - that account has no sign-in secret" },
    { "nothing was typed", "nothing was typed" },
    { " char(s) received, fp ", " char(s) received, fp " },
    { "tray.noclock", "no clock device" },
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
    { "menu.lock",        "Z\xE1rol\xE1s" },
    { "menu.signout",     "Kijelentkez\xE9s" },
    { "menu.exitgui",     "Kil\xE9p\xE9s a fel\xFCletr\xF5l" },
    { "menu.reboot",      "\xDAjraind\xEDt\xE1s" },
    { "menu.shutdown",    "Le\xE1ll\xEDt\xE1s" },
    { "tray.kbdlayout",   "Billenty\xFBzetkioszt\xE1s" },
    { "tray.nolayouts",   "nincs kioszt\xE1s" },
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
    { "console.serial_commands", "COM1-en \xE9rkez\xF5 parancsok futtat\xE1sa" },
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
    { "set.close_title",  "Mentetlen be\xE1ll\xEDt\xE1sok" },
    { "set.close_body",   "Ezek a be\xE1ll\xEDt\xE1sok m\xE9g nincsenek alkalmazva. Mentsem bez\xE1r\xE1s el\xF5tt?" },
    { "set.close_body_session", "A munkamenet v\xE9get \xE9r. Ezek a be\xE1ll\xEDt\xE1sok m\xE9g nincsenek alkalmazva - mentsem?" },
    { "btn.discard",      "Elvet\xE9s" },
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
    { "Locked", "Z\xE1rolva" },
    { "lock.prompt", "A k\xE9perny\xF5 z\xE1rolva van.  V\xE1lasszon fi\xF3kot, \xE9s jelentkezzen be." },
    { "lock.password", "Jelsz\xF3" },
    { "lock.signin", "Bejelentkez\xE9s" },
    { "Network", "H\xE1l\xF3zat" },
    { "adapters, addresses, Wi-Fi", "adapterek, c\xEDmek, Wi-Fi" },
    { "Disks", "Lemezek" },
    { "mount, unmount, format, RAM disks", "csatol\xE1s, lev\xE1laszt\xE1s, form\xE1z\xE1s, RAM-lemezek" },
    { "User accounts", "Felhaszn\xE1l\xF3i fi\xF3kok" },
    { "who may use this machine, and what each of them may do", "ki haszn\xE1lhatja ezt a g\xE9pet, \xE9s mit tehet" },
    { "boot, faults, crash reporting", "ind\xEDt\xE1s, hib\xE1k, \xF6sszeoml\xE1s-jelent\xE9sek" },
    { "keyboard layout", "billenty\xFBzetkioszt\xE1s" },
    { "output volume and mute", "hanger\xF5 \xE9s n\xE9m\xEDt\xE1s" },
    { "screen resolution", "k\xE9perny\xF5" "felbont\xE1s" },
    { "Theme, density, text and icon size", "T\xE9ma, s\xFBr\xFBs\xE9g, sz\xF6veg- \xE9s ikonm\xE9ret" },
    { "drivers, where they run, what they hold", "meghajt\xF3k: hol futnak, mit foglalnak" },
    { "CATEGORY", "KATEG\xD3RIA" },
    { "WHAT IT CONTAINS", "MIT TARTALMAZ" },
    { "OK", "OK" },
    { "Cancel", "M\xE9gse" },
    { "Save", "Ment\xE9s" },
    { "Delete", "T\xF6rl\xE9s" },
    { "New", "\xDAj" },
    { "Enable", "Enged\xE9lyez\xE9s" },
    { "Disable", "Letilt\xE1s" },
    { "Wi-Fi", "Wi-Fi" },
    { "net.dhcp", "DHCP ind\xEDt\xE1skor" },
    { "net.static", "K\xE9zi (statikus) c\xEDm" },
    { "net.static.ip", "Statikus IP-c\xEDm" },
    { "net.static.netmask", "Statikus alh\xE1l\xF3zati maszk" },
    { "net.static.gateway", "Statikus \xE1tj\xE1r\xF3" },
    { "net.static.dns", "Statikus DNS-kiszolg\xE1l\xF3" },
    { "net.wifisim", "Szimul\xE1lt Wi-Fi adapter" },
    { "ask a DHCP server for an address at boot", "ind\xEDt\xE1skor DHCP-kiszolg\xE1l\xF3t\xF3l k\xE9r c\xEDmet" },
    { "use the static address below instead of the default or DHCP", "az al\xE1" "bbi k\xE9zi c\xEDmet haszn\xE1lja az alap\xE9rtelmezett vagy a DHCP helyett" },
    { "static IPv4 address, e.g. 192.168.1.20", "statikus IPv4-c\xEDm, pl. 192.168.1.20" },
    { "static subnet mask", "statikus alh\xE1l\xF3zati maszk" },
    { "static default gateway", "statikus alap\xE9rtelmezett \xE1tj\xE1r\xF3" },
    { "static DNS server", "statikus DNS-kiszolg\xE1l\xF3" },
    { "add a SIMULATED Wi-Fi adapter (tests the chooser; carries no traffic)", "SZIMUL\xC1LT Wi-Fi adapter (a v\xE1laszt\xF3 tesztel\xE9s\xE9hez; forgalmat nem visz)" },
    { "audit.interval_s", "Invari\xE1ns-ellen\xF5rz\xE9s id\xF5k\xF6ze (mp)" },
    { "run the runtime invariant checks every N seconds (0 = only on demand)", "N m\xE1sodpercenk\xE9nt futtatja a fut\xE1sidej\xFB ellen\xF5rz\xE9seket (0 = csak k\xE9r\xE9sre)" },
    { "users.autologin", "Automatikus bejelentkez\xE9s" },
    { "sign in to this account without asking (it must have no password, and be the only account)", "k\xE9rd\xE9s n\xE9lk\xFCl ebbe a fi\xF3kba l\xE9p be (nem lehet jelszava, \xE9s csak ez az egy fi\xF3k lehet)" },
    { "users.default_user", "Alap\xE9rtelmezett fi\xF3k" },
    { "the account the sign-in surfaces offer first", "a bejelentkez\xE9skor els\xF5k\xE9nt felk\xEDn\xE1lt fi\xF3k" },
    { "gui.logout_grace_ms", "Kijelentkez\xE9si t\xFCrelmi id\xF5 (ms)" },
    { "how long signing out / shutting down waits for apps to close (ms)", "kijelentkez\xE9skor / le\xE1ll\xEDt\xE1skor ennyit v\xE1r a programok bez\xE1r\xE1s\xE1ra (ms)" },
    { "gui.page_flip", "Dupla pufferel\xE9s" },
    { "double-buffered present (off = single buffer; diagnostic)", "k\xE9tpufferes megjelen\xEDt\xE9s (ki = egy puffer; diagnosztika)" },
    { "gui.autorun", "Parancs indul\xE1s ut\xE1n" },
    { "one shell command to run once the desktop is up (test hook)", "egy shell-parancs, amely az asztal elindul\xE1sa ut\xE1n lefut (teszthez)" },
    { "gui.iv_probe", "Elemn\xE9zet s\xE9r\xFCl\xE9s-szonda" },
    { "log item-view damage rects against paint extents (diagnostic)", "napl\xF3zza az elemn\xE9zet s\xE9r\xFClt t\xE9glalapjait a rajzolt ter\xFClethez k\xE9pest (diagnosztika)" },
    { "gui.locktest", "Z\xE1rk\xE9perny\xF5-teszt" },
    { "diagnostic: drive the lock screen's submit with user:password", "diagnosztika: a z\xE1rk\xE9perny\xF5 bek\xFCld\xE9se felhasznalo:jelszo alakban" },
    { "how many address bits the edu device has - match QEMU's dma_mask", "az edu eszk\xF6z c\xEDmbitjeinek sz\xE1ma - egyezzen a QEMU dma_mask \xE9rt\xE9k\xE9vel" },
    { "execute lines arriving on COM1 as shell commands (SYSTEM context)", "a COM1-en \xE9rkez\xF5 sorokat shell-parancsk\xE9nt futtatja (SYSTEM k\xF6rnyezet)" },
    { "restarts of a ring-3 driver inside 30 s before it is quarantined; 0 = never restart", "egy ring-3 meghajt\xF3 \xFAjraind\xEDt\xE1sai 30 mp-en bel\xFCl a karant\xE9n el\xF5tt; 0 = soha" },
    { "interface language", "a fel\xFClet nyelve" },
    { "master output level, 0-100", "f\xF5 hanger\xF5, 0-100" },
    { "silence all output without forgetting the level", "minden hang eln\xE9m\xEDt\xE1sa a hanger\xF5 meg\xF5rz\xE9s\xE9vel" },
    { "how often to look for newly attached hardware, 0 = never", "milyen gyakran keressen \xFAjonnan csatlakoztatott hardvert, 0 = soha" },
    { "load /modules/*.ko at boot", "a /modules/*.ko bet\xF6lt\xE9se ind\xEDt\xE1skor" },
    { "skip painting what an opaque window completely covers", "nem rajzolja meg, amit egy \xE1tl\xE1tszatlan ablak teljesen eltakar" },
    { "reverse the wheel direction (Mac-style natural scrolling)", "ford\xEDtott g\xF6rget\xE9si ir\xE1ny (Mac-f\xE9le term\xE9szetes g\xF6rget\xE9s)" },
    { "Console Plate colour theme", "Console Plate sz\xEDnt\xE9ma" },
    { "control and row height (24 px / 16 px)", "vez\xE9rl\xF5k \xE9s sorok magass\xE1ga (24 px / 16 px)" },
    { "text size multiplier (the density sets the base size)", "sz\xF6vegm\xE9ret-szorz\xF3 (az alapm\xE9retet a s\xFBr\xFBs\xE9g adja)" },
    { "outline typefaces, or the built-in 8x8 bitmap font", "vektoros bet\xFBt\xEDpusok, vagy a be\xE9p\xEDtett 8x8-as bitk\xE9pes bet\xFB" },
    { "desktop and list icon size in pixels", "asztali \xE9s listaikonok m\xE9rete pixelben" },
    { "gradient | solid:RRGGBB | a path to a BMP", "gradient | solid:RRGGBB | egy BMP \xFAtvonala" },
    { "how the image is fitted to the screen", "hogyan illeszkedjen a k\xE9p a k\xE9perny\xF5h\xF6z" },
    { "desktop shell (takes effect at the next `gui` start)", "asztali h\xE9j (a k\xF6vetkez\xF5 `gui` ind\xEDt\xE1skor l\xE9p \xE9letbe)" },
    { "how desktop shortcuts are arranged", "az asztali parancsikonok elrendez\xE9se" },
    { "how the Control Panel arranges its categories", "a Vez\xE9rl\xF5pult kateg\xF3ri\xE1inak elrendez\xE9se" },
    { "active keyboard layout - applies immediately", "akt\xEDv billenty\xFBzetkioszt\xE1s - azonnal \xE9rv\xE9nyes" },
    { "what a ring-0 fault does (ring-3 always kills just the task)", "mi t\xF6rt\xE9njen ring-0 hib\xE1n\xE1l (ring-3 hiba mindig csak a taszkot \xE1ll\xEDtja le)" },
    { "open the Crash Reports window when a record is delivered", "\xF6sszeoml\xE1s-jelent\xE9s \xE9rkez\xE9sekor megnyitja a jelent\xE9sek ablak\xE1t" },
    { "print compositor timings when a window drag ends", "ablakh\xFAz\xE1s v\xE9g\xE9n ki\xEDrja a kompozitor id\xF5m\xE9r\xE9seit" },
    { "a second click on X force-kills an unresponsive client", "az X m\xE1sodik kattint\xE1sa k\xE9nyszerrel le\xE1ll\xEDtja a nem v\xE1laszol\xF3 programot" },
    { "unattended backstop before a closing window is forced (ms)", "ennyi id\xF5 ut\xE1n z\xE1rja be k\xE9nyszerrel a bez\xE1r\xE1s alatt \xE1ll\xF3 ablakot (ms)" },
    { "boot self-test window in ms (0 = skip; they cost 0.5 s of boot)", "ind\xEDt\xE1si \xF6ntesztek ideje ms-ban (0 = kihagy\xE1s; 0,5 mp-be ker\xFClnek)" },
    { "package store: ram rebuilds it each boot (82 ms), disk reuses it (7.8 s of reads)", "csomagt\xE1r: ram = minden ind\xEDt\xE1skor \xFAjra\xE9p\xFCl (82 ms), disk = \xFAjrahasznos\xEDtja (7,8 mp olvas\xE1s)" },
    { "how the file manager shows a directory (applies to a new window)", "hogyan mutassa a f\xE1jlkezel\xF5 a mapp\xE1kat (\xFAj ablakra \xE9rv\xE9nyes)" },
    { "lines of terminal history kept per window (0 = none; applies to new windows)", "ablakonk\xE9nt meg\xF5rz\xF6tt termin\xE1lsorok sz\xE1ma (0 = nincs; \xFAj ablakokra \xE9rv\xE9nyes)" },
    { "boot into the desktop (the text shell stays behind it: Start > Exit GUI)", "indul\xE1s az asztalra (a sz\xF6veges shell m\xF6g\xF6tte marad: Start > Kil\xE9p\xE9s a fel\xFCletr\xF5l)" },
    { "boot screen (applies at the next boot); the log is only hidden - dmesg keeps it", "ind\xEDt\xF3k\xE9perny\xF5 (a k\xF6vetkez\xF5 ind\xEDt\xE1skor \xE9rv\xE9nyes); a napl\xF3 csak rejtve van - a dmesg meg\xF5rzi" },
    { "let the service bus adapt between contract versions", "a szolg\xE1ltat\xE1sbusz alkalmazkodhat a szerz\xF5" "d\xE9sverzi\xF3k k\xF6z\xF6tt" },
    { "rows moved by one wheel notch", "egy g\xF6rg\xF5kattan\xE1sra ennyi sort mozdul" },
    { "seconds before an unconfirmed resolution reverts (0 = no dialog)", "ennyi m\xE1sodperc ut\xE1n \xE1ll vissza a nem meger\xF5s\xEDtett felbont\xE1s (0 = nincs k\xE9rd\xE9s)" },
    { "confirmed resolution, e.g. 1280x800 (written by the OK button)", "meger\xF5s\xEDtett felbont\xE1s, pl. 1280x800 (az OK gomb \xEDrja)" },
    { "how the device manager lays its devices out", "hogyan rendezze az eszk\xF6zkezel\xF5 az eszk\xF6z\xF6ket" },
    { "(no settings declared for this group)", "(ehhez a csoporthoz nincs be\xE1ll\xEDt\xE1s)" },
    { "tray.network", "H\xE1l\xF3zat" },
    { "tray.secured", "(v\xE9" "dett)" },
    { "tray.searching", "H\xE1l\xF3zatok keres\xE9se..." },
    { "tray.netsettings", "H\xE1l\xF3zati be\xE1ll\xEDt\xE1sok" },
    { "tray.noaudio", "Nincs hangeszk\xF6z" },
    { "tray.noaudio2", "nincs min lej\xE1tszani" },
    { "tray.volume", "Hanger\xF5" },
    { "tray.mute", "[ N\xE9m\xEDt\xE1s ]" },
    { "tray.unmute", "[ Hang be ]" },
    { "no network adapter", "nincs h\xE1l\xF3zati adapter" },
    { "disabled", "letiltva" },
    { "not connected", "nincs kapcsolat" },
    { "no address", "nincs c\xEDm" },
    { "connected (wired)", "csatlakozva (vezet\xE9kes)" },
    { "connected (Wi-Fi)", "csatlakozva (Wi-Fi)" },
    { "net.empty", "Nincs h\xE1l\xF3zati adapter." },
    { "net.hint.select", "V\xE1lasszon egy adaptert." },
    { "net.col.adapter", "Adapter" },
    { "net.col.type", "T\xEDpus" },
    { "net.col.state", "\xC1llapot" },
    { "net.col.address", "C\xEDm" },
    { "net.col.source", "Forr\xE1s" },
    { "net.col.mac", "MAC" },
    { "net.type.loopback", "visszacsatol\xE1s" },
    { "net.type.wired", "vezet\xE9kes" },
    { "net.type.wifi", "Wi-Fi" },
    { "net.type.wifisim", "Wi-Fi (szimul\xE1lt)" },
    { "net.st.disabled", "letiltva" },
    { "net.st.up", "csatlakozva" },
    { "net.st.upunknown", "csatlakozva (link nem jelzett)" },
    { "net.st.nocable", "nincs k\xE1" "bel" },
    { "net.st.notjoined", "nincs csatlakozva" },
    { "net.st.noaddr", "nincs c\xEDm" },
    { "net.src.dhcp", "DHCP" },
    { "net.src.static", "k\xE9zi" },
    { "net.src.default", "be\xE9p\xEDtett alap\xE9rt\xE9k" },
    { "net.d.gateway", "\xE1tj\xE1r\xF3" },
    { "net.d.lease", "b\xE9rlet h\xE1tra" },
    { "net.btn.renew", "C\xEDm meg\xFAj\xEDt\xE1sa" },
    { "net.btn.config", "Be\xE1ll\xEDt\xE1s..." },
    { "net.btn.test", "Teszt" },
    { "net.msg.nodev", "nincs ilyen adapter" },
    { "net.msg.leased", "DHCP-b\xF5l kapott c\xEDm:" },
    { "net.msg.nodhcp", "Nem v\xE1laszolt DHCP-kiszolg\xE1l\xF3." },
    { "net.msg.gateway", "\xC1tj\xE1r\xF3 " },
    { "net.msg.answers", "v\xE1laszol." },
    { "net.msg.silent", "nem v\xE1laszol." },
    { "net.msg.dns", "N\xE9vfelold\xE1s " },
    { "net.msg.resolves", "m\xFBk\xF6" "dik." },
    { "net.msg.fails", "nem m\xFBk\xF6" "dik." },
    { "net.msg.nowifi", "Nincs vezet\xE9k n\xE9lk\xFCli adapter." },
    { "net.msg.joined", "Csatlakozva." },
    { "net.msg.nonet", "Ez a h\xE1l\xF3zat nincs hat\xF3t\xE1von bel\xFCl." },
    { "net.msg.badpass", "Hib\xE1s jelsz\xF3." },
    { "net.msg.radio", "A r\xE1" "di\xF3 nem v\xE1laszolt." },
    { "net.msg.left", "Lev\xE1lasztva." },
    { "net.msg.notask", "A m\xFBvelet nem ind\xEDthat\xF3 el." },
    { "net.msg.cannot", "Ez az adapter nem kapcsolhat\xF3." },
    { "net.msg.enabled", "Adapter enged\xE9lyezve." },
    { "net.msg.disabled", "Adapter letiltva." },
    { "net.msg.nodhcphere", "Ez az adapter nem haszn\xE1l DHCP-t." },
    { "net.msg.asking", "DHCP-kiszolg\xE1l\xF3 k\xE9rdez\xE9se..." },
    { "net.msg.busy", "Egy m\xE1sik h\xE1l\xF3zati m\xFBvelet fut." },
    { "net.msg.testing", "A kapcsolat tesztel\xE9se..." },
    { "net.msg.joining", "Csatlakoz\xE1s..." },
    { "net.msg.leaving", "Lev\xE1laszt\xE1s..." },
    { "net.msg.defaultreboot", "Mentve. A be\xE9p\xEDtett c\xEDm a k\xF6vetkez\xF5 \xFAjraind\xEDt\xE1skor t\xE9r vissza." },
    { "net.title.address", "H\xE1l\xF3zati c\xEDm" },
    { "net.mode.default", "Be\xE9p\xEDtett" },
    { "net.mode.dhcp", "Automatikus" },
    { "net.mode.static", "K\xE9zi" },
    { "net.f.ip", "IP-c\xEDm" },
    { "net.f.mask", "Alh\xE1l\xF3zati maszk" },
    { "net.f.gw", "\xC1tj\xE1r\xF3" },
    { "net.f.dns", "DNS-kiszolg\xE1l\xF3" },
    { "net.hint.static", "Automatikus: DHCP; K\xE9zi: az al\xE1" "bbi mez\xF5k." },
    { "net.err.ip", "Az IP-c\xEDm \xE9rv\xE9nytelen." },
    { "net.err.mask", "Az alh\xE1l\xF3zati maszk \xE9rv\xE9nytelen." },
    { "net.err.gw", "Az \xE1tj\xE1r\xF3 \xE9rv\xE9nytelen." },
    { "net.err.dns", "A DNS-kiszolg\xE1l\xF3 \xE9rv\xE9nytelen." },
    { "wifi.col.network", "H\xE1l\xF3zat" },
    { "wifi.col.signal", "Jel" },
    { "wifi.col.security", "V\xE9" "delem" },
    { "wifi.col.status", "\xC1llapot" },
    { "wifi.sec.open", "ny\xEDlt" },
    { "wifi.connected", "csatlakozva" },
    { "wifi.simulated", "(szimul\xE1lt adapter - nincs forgalom)" },
    { "wifi.empty", "Nincs h\xE1l\xF3zat hat\xF3t\xE1von bel\xFCl." },
    { "wifi.scanned", "H\xE1l\xF3zatok friss\xEDtve." },
    { "wifi.pick", "El\xF5" "bb v\xE1lasszon h\xE1l\xF3zatot." },
    { "wifi.needpass", "Ehhez a h\xE1l\xF3zathoz jelsz\xF3 kell." },
    { "wifi.passphrase", "Jelsz\xF3" },
    { "wifi.btn.scan", "Keres\xE9s" },
    { "wifi.btn.connect", "Csatlakoz\xE1s" },
    { "wifi.btn.disconnect", "Lev\xE1laszt\xE1s" },
    { "disk.empty", "Nincs lemez." },
    { "disk.col.disk", "Lemez" },
    { "disk.col.size", "M\xE9ret" },
    { "disk.col.fs", "F\xE1jlrendszer" },
    { "disk.col.mount", "Csatol\xE1si pont" },
    { "disk.col.free", "Szabad" },
    { "disk.col.note", "Megjegyz\xE9s" },
    { "disk.note.system", "rendszerk\xF6tet" },
    { "disk.note.ram", "RAM-lemez" },
    { "disk.hint.select", "V\xE1lasszon egy lemezt." },
    { "disk.d.used", "foglalt" },
    { "disk.d.mounted", "csatolva" },
    { "disk.d.notmounted", "nincs csatolva" },
    { "disk.d.heldby", "haszn\xE1lja:" },
    { "disk.v.mount", "Csatol\xE1s" },
    { "disk.v.umount", "Lev\xE1laszt\xE1s" },
    { "disk.v.format", "Form\xE1z\xE1s" },
    { "disk.v.newram", "\xDAj RAM-lemez" },
    { "disk.v.remove", "Elt\xE1vol\xEDt\xE1s" },
    { "disk.v.sync", "Szinkroniz\xE1l\xE1s" },
    { "disk.btn.mount", "Csatol\xE1s" },
    { "disk.btn.umount", "Lev\xE1laszt\xE1s" },
    { "disk.btn.format", "Form\xE1z\xE1s..." },
    { "disk.btn.sync", "Szinkroniz\xE1l\xE1s" },
    { "disk.btn.newram", "\xDAj RAM-lemez" },
    { "disk.btn.remove", "Elt\xE1vol\xEDt\xE1s" },
    { "disk.btn.formatnow", "Form\xE1z\xE1s" },
    { "disk.title.format", "Lemez form\xE1z\xE1sa" },
    { "disk.f.label", "K\xF6tetc\xEDmke" },
    { "disk.f.hint", "\xDAj, \xFCres exFAT f\xE1jlrendszer ker\xFCl a teljes lemezre." },
    { "disk.f.warn", "A lemez teljes tartalma t\xF6rl\xF5" "dik:" },
    { "the settings store", "a be\xE1ll\xEDt\xE1s-t\xE1rol\xF3" },
    { "empty", "\xFCres" },
    { "unknown", "ismeretlen" },
    { "unreadable", "olvashatatlan" },
    { "partitioned", "particion\xE1lt" },
    { "done", "k\xE9sz" },
    { "no such disk", "nincs ilyen lemez" },
    { "the disk is mounted - unmount it first", "a lemez csatolva van - el\xF5" "bb v\xE1lassza le" },
    { "the disk is not mounted", "a lemez nincs csatolva" },
    { "files are open on it", "f\xE1jlok vannak megnyitva rajta" },
    { "another volume is mounted inside it", "egy m\xE1sik k\xF6tet van benne csatolva" },
    { "the system depends on it", "a rendszer haszn\xE1lja" },
    { "no filesystem this system can mount", "nincs rajta csatolhat\xF3 f\xE1jlrendszer" },
    { "I/O error", "I/O-hiba" },
    { "only a RAM disk can be removed", "csak RAM-lemez t\xE1vol\xEDthat\xF3 el" },
    { "not enough memory", "nincs el\xE9g mem\xF3ria" },
    { "this filesystem cannot be unmounted", "ez a f\xE1jlrendszer nem v\xE1laszthat\xF3 le" },
    { "failed", "sikertelen" },
    { "Hardware", "Hardver" },
    { "Where", "Hol" },
    { "ID", "Azonos\xEDt\xF3" },
    { "Driver", "Meghajt\xF3" },
    { "State", "\xC1llapot" },
    { "Runs in", "Fut" },
    { "Isolation", "Elszigetel\xE9s" },
    { "Holds", "Foglal" },
    { "Select a device.", "V\xE1lasszon egy eszk\xF6zt." },
    { "Start", "Ind\xEDt\xE1s" },
    { "Stop", "Le\xE1ll\xEDt\xE1s" },
    { "Move", "\xC1thelyez\xE9s" },
    { "Update", "Friss\xEDt\xE9s" },
    { "Browse", "Tall\xF3z\xE1s" },
    { "Crash", "\xD6sszeomlaszt\xE1s" },
    { "QUARANTINED", "KARANT\xC9NBAN" },
    { "stopped", "le\xE1ll\xEDtva" },
    { "init failed", "ind\xEDt\xE1s sikertelen" },
    { "absent", "nincs jelen" },
    { "probed", "felismerve" },
    { "not present", "nincs jelen" },
    { "needs a driver", "meghajt\xF3 kell" },
    { "no driver needed", "nem kell meghajt\xF3" },
    { "not started", "nincs elind\xEDtva" },
    { "offline", "offline" },
    { "platform", "platform" },
    { "(none)", "(nincs)" },
    { "(not needed)", "(nem kell)" },
    { "NO DRIVER", "NINCS MEGHAJT\xD3" },
    { "kernel", "kernel" },
    { "ring 3 pid ", "ring 3, pid " },
    { " (->ring 3 on restart)", " (\xFAjraind\xEDt\xE1skor ring 3)" },
    { " - driver ", " - meghajt\xF3: " },
    { ", can run: ", ", futhat: " },
    { " - DMA, device confined", " - DMA, eszk\xF6z korl\xE1tozva" },
    { " - DMA: ", " - DMA: " },
    { " - boot-critical, cannot be moved", " - ind\xEDt\xE1shoz sz\xFCks\xE9ges, nem helyezhet\xF5 \xE1t" },
    { " - not present; the driver is here if it ever is", " - nincs jelen; ha megjelenik, a meghajt\xF3 k\xE9sz" },
    { " - present, and nothing here drives it", " - jelen van, de nincs meghajt\xF3ja" },
    { " - present; nothing drives it and nothing should", " - jelen van; nem kell meghajt\xF3" },
    { " - ", " - " },
    { "Account", "Fi\xF3k" },
    { "UID", "UID" },
    { "Role", "Szerep" },
    { "Sign-in", "Bejelentkez\xE9s" },
    { "Home", "Saj\xE1t mappa" },
    { "Administrator (protected)", "Rendszergazda (v\xE9" "dett)" },
    { "Administrator", "Rendszergazda" },
    { "Standard", "Norm\xE1l" },
    { "yes", "igen" },
    { "no password", "nincs jelsz\xF3" },
    { "Select an account.", "V\xE1lasszon egy fi\xF3kot." },
    { "Set password", "Jelsz\xF3 be\xE1ll\xEDt\xE1sa" },
    { "Toggle admin", "Rendszergazda ki/be" },
    { "Auto sign-in", "Automatikus bel\xE9p\xE9s" },
    { "Delete account", "Fi\xF3k t\xF6rl\xE9se" },
    { "New account name", "\xDAj fi\xF3k neve" },
    { "New password for ", "\xDAj jelsz\xF3: " },
    { "Password set for ", "Jelsz\xF3 be\xE1ll\xEDtva: " },
    { " (", " (" },
    { " chars, fp ", " karakter, fp " },
    { ", verified ok).  They can sign in now.", ", ellen\xF5rizve).  Most m\xE1r bejelentkezhet." },
    { ", VERIFY FAILED - the record did not take).", ", ELLEN\xD5RZ\xC9S SIKERTELEN - a rekord nem \xEDr\xF3" "dott be)." },
    { "NOT changed - ", "NEM v\xE1ltozott - " },
    { ".  Acting as ", ".  Aktu\xE1lis felhaszn\xE1l\xF3: " },
    { ".", "." },
    { "No password typed - nothing was changed. (To disable sign-in, use `passwd <name> -` at a shell.)", "Nem \xEDrt be jelsz\xF3t - semmi sem v\xE1ltozott. (A bejelentkez\xE9s tilt\xE1sa: `passwd <n\xE9v> -` a shellben.)" },
    { "Auto sign-in is off.  The picker will ask.", "Az automatikus bel\xE9p\xE9s ki van kapcsolva.  A v\xE1laszt\xF3 r\xE1k\xE9rdez." },
    { "Signing in to ", "Automatikus bel\xE9p\xE9s ebbe a fi\xF3kba: " },
    { " automatically from now on (it has no password, so there is nothing to ask).", " (nincs jelszava, \xEDgy nincs mit k\xE9rdezni)." },
    { "Delete '", "T\xF6rli ezt: '" },
    { "'?\nIts uid is retired permanently, and the files it owns\nwill then belong to no account.", "'?\nAz uid v\xE9glegesen kivon\xE1sra ker\xFCl, a f\xE1jljai\nezut\xE1n egyetlen fi\xF3khoz sem tartoznak." },
    { "  You are signed in as a standard user, so you may change your own password and nothing else.", "  Norm\xE1l felhaszn\xE1l\xF3k\xE9nt csak a saj\xE1t jelszav\xE1t m\xF3" "dos\xEDthatja." },
    { " - uid ", " - uid " },
    { ", cannot sign in (no password)", ", nem tud bel\xE9pni (nincs jelsz\xF3)" },
    { ".  root is protected: it cannot be deleted or demoted by anyone, including root.", ".  A root v\xE9" "dett: senki nem t\xF6r\xF6lheti vagy fokozhatja le, maga a root sem." },
    { ".  This is an administrator - only root may change or remove one.", ".  Ez rendszergazda - csak a root m\xF3" "dos\xEDthatja vagy t\xF6r\xF6lheti." },
    { ".  Not your account: a standard user may change only their own password.", ".  Nem az \xD6n fi\xF3kja: norm\xE1l felhaszn\xE1l\xF3 csak a saj\xE1t jelszav\xE1t m\xF3" "dos\xEDthatja." },
    { "Keep this display mode?", "Megtartja ezt a megjelen\xEDt\xE9si m\xF3" "dot?" },
    { "Keeping this resolution in ", "A felbont\xE1s megmarad: " },
    { "Double-click a resolution", "Kattintson dupl\xE1n egy felbont\xE1sra" },
    { "This display cannot change resolution.", "Ez a kijelz\xF5 nem tud felbont\xE1st v\xE1ltani." },
    { "current", "jelenlegi" },
    { "Choose an account first.", "El\xF5" "bb v\xE1lasszon fi\xF3kot." },
    { "Incorrect password for '", "Hib\xE1s jelsz\xF3: '" },
    { " - that account has no sign-in secret", " - ennek a fi\xF3knak nincs jelszava" },
    { "nothing was typed", "nem \xEDrt be semmit" },
    { " char(s) received, fp ", " karakter \xE9rkezett, fp " },
    { "tray.noclock", "nincs \xF3ra" },
};

LOCALE_CATALOG(loc_hu) = {
    .lang = "hu", .name = "Magyar",
    .entries = hu_strings,
    .count = (int)(sizeof hu_strings / sizeof hu_strings[0]),
};

/* --- §M70 shell registration ----------------------------------------------- */
SHELL_CMD(locale) = { "locale", "[<lang> | missing [on|off]]", "the string catalogue, and a sample lookup",
                      SHELL_G_SYS, locale_cmd, SHELL_P_ANY };
