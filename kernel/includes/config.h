/* =============================================================================
 * config.h — kernel-wide key/value configuration store.
 *
 * Backed by `/etc/d-os.conf` on the live filesystem.  At boot:
 *   1. `config_init` populates the in-memory cache with hardcoded
 *      defaults (`builtin_defaults` in config.c).
 *   2. If `/etc/d-os.conf` exists it is parsed and overlaid on the
 *      defaults — so users can override any default by setting the
 *      same key in the file.
 *
 * `config_get` is the only call that consumers (drivers, shell) make
 * during normal operation.  Pass a `default_value` so callers can keep
 * working even if the key is absent.
 *
 * File format (one entry per line):
 *
 *     # comment lines start with hash
 *     key.with.dots = value can have spaces
 *     blank lines OK
 *
 * The "value" extends to end-of-line and is trimmed of leading +
 * trailing whitespace.  Quoting is not supported.
 * ============================================================================= */

#ifndef CONFIG_H
#define CONFIG_H

/* Default at compile time — registered statically per consumer. */
struct config_default {
    const char* key;
    const char* value;
};

/* Populate the cache with defaults, then try to read the conf file. */
void config_init(void);

/* Look up `key`.  Returns the value pointer (owned by the cache, do
 * not modify or free) or `default_value` if no entry exists. */
const char* config_get(const char* key, const char* default_value);

/* Parse a config value as a base-10 integer (missing/invalid → `def`). */
long config_get_long(const char* key, long def);

/* Set or replace a key.  Strings are copied (cache owns them).  Does NOT
 * notify watchers — see config_apply. */
int  config_set(const char* key, const char* value);

/* §M63 stage 0 — set + NOTIFY.  Use this from anything a person drives
 * (`setconf`, a settings panel, a config file being overlaid); use plain
 * `config_set` for machinery that is populating the cache rather than changing
 * a decision (`config_init`'s defaults would otherwise fire a notification for
 * every key before a single subsystem exists).
 * Watchers run only when the value ACTUALLY CHANGED. */
int  config_apply(const char* key, const char* value);

/* Persist the current cache.  Writes to config_persist_path() when a writable
 * volume has been attached, otherwise to `/etc/d-os.conf` — which is on ramfs
 * and therefore does NOT survive a reboot.  Callers that report success to a
 * human must say which of the two happened; see the `saveconf` command. */
int  config_save(void);

/* Reload from `/etc/d-os.conf` (does not clear cache; existing entries
 * stay unless overridden by the file). */
int  config_load(void);

/* Same, from an arbitrary path.  Entries are overlaid with config_apply, so
 * loading a file after boot notifies watchers about what changed. */
int  config_load_path(const char* path);

/* =====================================================================
 * §M63 stage 0 — persistence.
 *
 * The problem this solves: `/` is ramfs.  `config_init()` runs early in
 * `kernel_main` (before any disk is mounted — it has to, since half the boot
 * reads config), so the conf file it loads and the one `config_save` wrote
 * both live in MEMORY.  Every setting a user changed was therefore lost at the
 * next boot, silently, and the settings UI in §M63 would have been theatre on
 * top of that.
 *
 * `config_attach_persistent(dir)` is called once a WRITABLE volume is mounted
 * (kernel.c, right after the exFAT mount).  It:
 *   1. overlays `<dir>/d-os.conf` on top of the current cache — via
 *      config_apply, so subsystems that already read a key are told it moved;
 *   2. makes that file the target of every later config_save().
 *
 * It returns 0 only when the file was actually READ or CREATED, i.e. when the
 * volume proved writable.  A machine with no disk keeps working with defaults
 * and `config_persist_path()` stays NULL — *the honest answer is "this will not
 * survive a reboot", not a save that silently goes nowhere.*
 * ===================================================================== */
int         config_attach_persistent(const char* dir);
const char* config_persist_path(void);      /* NULL = nothing survives */

/* ---------------------------------------------------------------------
 * Change notification.
 *
 * A settings panel that writes a key and a subsystem that read it at boot are
 * two halves of one operation, and without this they are permanently out of
 * step: the keyboard layout is chosen at init, the wallpaper at gui_start, the
 * fault policy at first fault.  A watcher closes that gap ONCE, for every
 * consumer, instead of each panel knowing which function to poke.
 *
 * `prefix` is matched against the start of the key ("gui." catches
 * gui.wallpaper and gui.shell; "" catches everything).  Callbacks run on the
 * caller's task with no locks held — do not block, and do not assume the GUI
 * exists.
 * --------------------------------------------------------------------- */
struct config_watch {
    const char* prefix;
    void (*changed)(const char* key, const char* value);
};

extern struct config_watch __start_config_watches[];
extern struct config_watch __stop_config_watches[];

#define CONFIG_WATCH(_var)                                               \
    static const struct config_watch                                     \
    __attribute__((used, section("config_watches"), aligned(4)))         \
    _var##_registration

/* Usage:
 *   static void gui_conf_changed(const char* k, const char* v) { ... }
 *   CONFIG_WATCH(gui_watch) = { .prefix = "gui.", .changed = gui_conf_changed };
 */

/* Fire the watchers for one key.  config_apply does this for you; call it
 * directly only when a value changed by a route that bypasses the cache. */
void config_notify(const char* key, const char* value);

/* Shell commands, implemented in config.c so BOTH shells (x86 shell.c and the
 * aarch64 serial REPL) run one copy — §M24's rule.  They were x86-only until
 * §M63 stage 0, which meant ARM could not change or save a setting at all. */
void config_cmd_getconf(const char* key);
void config_cmd_setconf(const char* args);
void config_cmd_saveconf(void);

/* Diagnostic — print every entry to the console. */
void config_dump(void);

/* Iterate every key/value in the cache.  Used by procfs to render
 * `/proc/config`. */
typedef void (*config_iter_fn)(const char* key, const char* value, void* ctx);
void config_for_each(config_iter_fn fn, void* ctx);

#endif
