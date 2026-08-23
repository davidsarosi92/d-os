/* =============================================================================
 * settings.h — the Control Panel's two registries (§M63).
 *
 * §M60, §M61 and §M62 each end with "…and a UI for it".  Written three times
 * that is three apps, three Start-menu entries and three places to keep
 * consistent — and the Start menu physically cannot take them
 * (`shell_vista.c` caps the launcher, and it was already nearly full).  So the
 * container is not a nicety that comes after the settings: it decides whether
 * adding a setting is a LINE or an APP.
 *
 * TWO REGISTRIES, because there are two kinds of setting.
 *
 * 1. SETTINGS_PANEL() — a panel that needs real UI (a preview, a file picker,
 *    a confirm-or-revert timer).  It ships next to the code it configures and
 *    opens its own window; `controlpanel.c` never names one.
 *
 * 2. CONFIG_KEY() — a DESCRIPTOR for a plain setting: key, type, allowed
 *    values, default, one line of help.  ONE generic panel renders every key
 *    that declares itself, so most settings need **no UI code at all** — which
 *    is the point.  It also gives `setconf` something to validate against and
 *    `conf -l` something to list: today a mistyped value is discovered by the
 *    subsystem that reads it, if at all.
 *
 * The rule that keeps both honest (§M63): a panel is a VIEW over the config
 * store, never the storage.  Every setting must be reachable from the shell,
 * because every automated check in this project is a grep over a serial log
 * and a GUI-only setting cannot be regression-tested here at all.
 *
 * Threading: `open` runs on the compositor task (the item view's activate
 * callback), so it may create windows, allocate and touch the VFS — the same
 * contract every GUI_APP has.
 * ============================================================================= */

#ifndef SETTINGS_H
#define SETTINGS_H

/* ---------------------------------------------------------------- */
/* 1. Panels.                                                        */
/* ---------------------------------------------------------------- */

struct settings_panel {
    const char* name;           /* "Display"                                  */
    const char* summary;        /* one line under the name in list view       */
    int         icon;           /* enum icon_id                               */
    void      (*open)(void);    /* open (or raise) the panel's own window     */
    /* When `open` is NULL the panel is rendered by the GENERIC key panel over
     * every CONFIG_KEY whose group matches `name`.  That is the common case:
     * a settings page with no code. */
};

extern struct settings_panel __start_settings_panels[];
extern struct settings_panel __stop_settings_panels[];

#define SETTINGS_PANEL(_var)                                             \
    static const struct settings_panel                                   \
    __attribute__((used, section("settings_panels"), aligned(4)))        \
    _var##_registration

int  settings_panel_count(void);
const struct settings_panel* settings_panel_at(int i);

/* Open panel `i` — the one place that decides between a panel's own window
 * and the generic key panel. */
void settings_panel_open(int i);

/* ---------------------------------------------------------------- */
/* 2. Key descriptors.                                               */
/* ---------------------------------------------------------------- */

enum config_key_type {
    CFG_BOOL = 0,       /* "0"/"1"                                    */
    CFG_ENUM,           /* one of `values` (space separated)          */
    CFG_INT,            /* decimal                                    */
    CFG_STRING,         /* anything                                   */
    CFG_PATH            /* a VFS path (a picker, once one exists)     */
};

struct config_key_def {
    const char* key;        /* "gui.wallpaper_fit"                        */
    const char* group;      /* panel name it appears under, e.g. "System" */
    int         type;
    const char* values;     /* CFG_ENUM: "fill stretch center tile"       */
    const char* def;        /* default, shown when the key is unset       */
    const char* help;       /* one line                                   */
    /* §M65 — a numeric key's RANGE, so the panel can offer a slider instead of
     * a text box.  Both zero = unknown, and the panel falls back to free text:
     * a slider over a range nobody declared would invent limits. */
    int         min, max;
};

extern struct config_key_def __start_config_keys[];
extern struct config_key_def __stop_config_keys[];

#define CONFIG_KEY(_var)                                                 \
    static const struct config_key_def                                   \
    __attribute__((used, section("config_keys"), aligned(4)))            \
    _var##_registration

int  config_key_count(void);
const struct config_key_def* config_key_at(int i);
const struct config_key_def* config_key_find(const char* key);

/* Validate `value` against a descriptor.  Returns 0 when acceptable (or when
 * the key has no descriptor — an undeclared key is not an invalid one, it is
 * simply undescribed).  This is what lets `setconf` refuse "fil" for a fit
 * mode instead of leaving the mistake to be found by whoever reads it. */
int  config_key_validate(const char* key, const char* value);

/* The `conf` shell command: list descriptors, show one, or set with
 * validation.  Lives with the registry so both shells share it. */
void settings_cmd(const char* args);

#endif
