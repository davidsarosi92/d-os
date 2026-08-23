/* =============================================================================
 * settings.c — the Control Panel's registries, the generic key panel, and the
 * `conf` command (§M63).  See settings.h for why there are two registries.
 *
 * The generic panel is the reason most settings need no UI code: it renders
 * every CONFIG_KEY whose `group` matches the panel's name, edits the value
 * according to the descriptor's TYPE, and writes it back with config_apply so
 * the owning subsystem hears about it immediately (§M63 stage 0's watchers).
 *
 * Widget layout, deliberately built from what the toolkit already has rather
 * than from new widgets:
 *
 *     ┌───────────────────────────────────────────┐
 *     │ <group> settings                          │   label
 *     │ ┌───────────────────────────────────────┐ │
 *     │ │ key = value                           │ │   listview (one row/key)
 *     │ └───────────────────────────────────────┘ │
 *     │ help text for the selected key            │   label
 *     │ [ value________________ ] [Set] [Cycle]   │   textinput + buttons
 *     │ [Save]  status                            │
 *     └───────────────────────────────────────────┘
 *
 * "Cycle" exists because a bool or a small enum is a click, not typing — and
 * because it is the only affordance that TELLS the user what the legal values
 * are without a dropdown widget the toolkit does not have.
 * ============================================================================= */

#include "settings.h"
#include "icons.h"
#include "config.h"
#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include "ui.h"           /* §M65 — the panel is built from specs now */
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include <stddef.h>

/* ------------------------------------------------------------------- */
/* Helpers.                                                             */
/* ------------------------------------------------------------------- */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
/* Is `word` one of the space-separated entries of `list`? */
static int in_list(const char* list, const char* word) {
    if (!list) return 0;
    const char* p = list;
    while (*p) {
        while (*p == ' ') p++;
        const char* q = word;
        const char* start = p;
        while (*p && *p != ' ' && *q && *p == *q) { p++; q++; }
        if ((!*p || *p == ' ') && !*q) return 1;
        p = start;
        while (*p && *p != ' ') p++;          /* skip this entry */
    }
    return 0;
}

/* ------------------------------------------------------------------- */
/* Registry walks.                                                      */
/* ------------------------------------------------------------------- */

int settings_panel_count(void) {
    return (int)(__stop_settings_panels - __start_settings_panels);
}
const struct settings_panel* settings_panel_at(int i) {
    if (i < 0 || i >= settings_panel_count()) return NULL;
    return &__start_settings_panels[i];
}

int config_key_count(void) {
    return (int)(__stop_config_keys - __start_config_keys);
}
const struct config_key_def* config_key_at(int i) {
    if (i < 0 || i >= config_key_count()) return NULL;
    return &__start_config_keys[i];
}
const struct config_key_def* config_key_find(const char* key) {
    if (!key) return NULL;
    for (int i = 0; i < config_key_count(); i++)
        if (streq_(__start_config_keys[i].key, key)) return &__start_config_keys[i];
    return NULL;
}

int config_key_validate(const char* key, const char* value) {
    const struct config_key_def* d = config_key_find(key);
    if (!d || !value) return 0;          /* undeclared ≠ invalid */
    switch (d->type) {
        case CFG_BOOL:
            return (streq_(value, "0") || streq_(value, "1")) ? 0 : -1;
        case CFG_ENUM:
            return in_list(d->values, value) ? 0 : -1;
        case CFG_INT: {
            const char* p = value;
            if (*p == '-' || *p == '+') p++;
            if (!*p) return -1;
            while (*p) { if (*p < '0' || *p > '9') return -1; p++; }
            return 0;
        }
        default:
            return 0;
    }
}

/* ------------------------------------------------------------------- */
/* The generic key panel.                                               */
/* ------------------------------------------------------------------- */

#define GP_MAX_KEYS 24

/* §M65 — THE PANEL NO LONGER KNOWS WHAT A SETTING LOOKS LIKE.
 *
 * The old version rendered every key as a row of text plus a shared text box
 * and a "Cycle" button, and computed every rectangle by hand — `ch - 24 - 78`,
 * `cw - 176`, and so on.  That was not a style choice: there was no checkbox to
 * put a bool in, and no layout to put one anywhere.
 *
 * Now the DESCRIPTOR chooses the control and the layout places it:
 *
 *      CFG_BOOL              -> checkbox
 *      CFG_ENUM              -> radio group (the `values` string is already
 *                               space-separated, which is exactly what the
 *                               control's spec wants)
 *      CFG_INT with a range  -> slider
 *      everything else       -> text box
 *
 * Every row is one container: [ label | control ], and the container carries
 * UI_WRAP_COMPACT — so on a narrow screen the same declaration becomes label
 * above control, without a second layout anywhere.
 * ========================================================================= */

struct genpanel {
    struct gui_window* win;
    const char*        group;
    int   key_idx[GP_MAX_KEYS];         /* control id → config_key index    */
    int   n;
    int   status_id;
};

/* Ids: rows get 100+i for their control, so an event names its key by
 * arithmetic instead of a lookup table that could drift from the build. */
#define GP_ID_CTRL(i)  (100 + (i))
#define GP_ID_SCROLL   8999
#define GP_ID_GRID     9000
#define GP_ID_STATUS   9001
#define GP_ID_SAVE     9002

static void gp_status(struct genpanel* g, const char* text) {
    struct widget* w = ui_by_id(g->win, GP_ID_STATUS);
    const struct widget_class* c = w ? ui_class_find("label") : NULL;
    if (w && c && c->set_text) c->set_text(w, text);
    gui_window_request_redraw(g->win);
}

/* Turn a control's numeric value back into the string the config store keeps.
 * The descriptor is what makes this possible without the panel knowing the
 * key: a bool is 0/1, an enum is its Nth word, an int is itself. */
static void gp_value_to_text(const struct config_key_def* d, int v,
                             char* out, int cap) {
    if (!cap) return;
    if (d->type == CFG_BOOL) { out[0] = v ? '1' : '0'; out[1] = 0; return; }
    if (d->type == CFG_ENUM) {
        const char* p = d->values;
        for (int i = 0; p && *p; i++) {
            while (*p == ' ') p++;
            int n = 0;
            while (p[n] && p[n] != ' ') n++;
            if (i == v) {
                int k = 0;
                for (; k < n && k < cap - 1; k++) out[k] = p[k];
                out[k] = 0;
                return;
            }
            p += n;
        }
        out[0] = 0;
        return;
    }
    /* CFG_INT — decimal, written by hand because this kernel's printf has no
     * width specifiers and this needs none. */
    int neg = v < 0; unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + u % 10); u /= 10; } while (u && t < 11);
    int k = 0;
    if (neg && k < cap - 1) out[k++] = '-';
    while (t > 0 && k < cap - 1) out[k++] = tmp[--t];
    out[k] = 0;
}

/* Which enum word is the current value?  -1 when it matches none, which the
 * caller shows as "no selection" rather than silently picking the first. */
static int gp_enum_index(const struct config_key_def* d, const char* cur) {
    const char* p = d->values;
    for (int i = 0; p && *p; i++) {
        while (*p == ' ') p++;
        int n = 0;
        while (p[n] && p[n] != ' ') n++;
        int j = 0;
        while (j < n && cur[j] && cur[j] == p[j]) j++;
        if (j == n && !cur[j]) return i;
        p += n;
    }
    return -1;
}

static int gp_atoi(const char* s) {
    int v = 0, neg = 0;
    if (s && *s == '-') { neg = 1; s++; }
    while (s && *s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ONE event sink for the whole window: (id, type, value).  This is the shape
 * that can cross a process boundary later — a per-widget callback pointer
 * cannot — and it is why the toolkit was built this way. */
static void gp_event(struct gui_window* win, int id, int type, int value,
                     void* ctx) {
    struct genpanel* g = (struct genpanel*)ctx;
    (void)win;

    if (id == GP_ID_SAVE && type == UI_EV_CLICK) {
        const char* p = config_persist_path();
        gp_status(g, config_save() == 0
                     ? (p ? "saved - survives a reboot"
                          : "saved to RAM only - no writable volume")
                     : "save FAILED");
        return;
    }

    int i = id - 100;
    if (i < 0 || i >= g->n) return;
    const struct config_key_def* d = config_key_at(g->key_idx[i]);
    if (!d) return;

    char text[64];
    if (type == UI_EV_SUBMIT) {
        struct widget* w = ui_by_id(win, id);
        const struct widget_class* c = ui_class_find("textinput");
        if (!w || !c || !c->get_text) return;
        c->get_text(w, text, sizeof text);
    } else if (type == UI_EV_TOGGLE || type == UI_EV_CHANGE) {
        gp_value_to_text(d, value, text, sizeof text);
    } else {
        return;
    }

    if (config_key_validate(d->key, text) != 0) {
        gp_status(g, "rejected - not a valid value for this setting");
        return;
    }
    /* config_apply, not config_set: the subsystem that read this key at boot
     * has to hear about it — the entire reason §M63 stage 0 built watchers. */
    config_apply(d->key, text);
    gp_status(g, "applied - use Save to keep it across a reboot");
}

static void gp_layout(struct gui_window* win) {
    struct genpanel* g = (struct genpanel*)gui_window_ctx(win);
    if (!g) return;
    g->win = win;

    /* on_layout fires on every resize.  The controls already exist by then —
     * re-running the build would add a second set of them (see ui.h) — so a
     * resize is a LAYOUT, which is the whole point of having one. */
    if (ui_node_count(win) > 0) { ui_layout(win); return; }

    /* Room for: title + (label,control) per key + status + Save. */
    int cap = GP_MAX_KEYS * 3 + 6;
    struct ui_spec* sp = (struct ui_spec*)kcalloc((size_t)cap, sizeof *sp);
    if (!sp) return;
    int k = 0;

    sp[k++] = (struct ui_spec){ .id = 1, .cls = "label", .text = g->group,
                                .flags = UI_FILL_W };
    /* A scrolling VIEWPORT holds the grid: a group with many keys no longer
     * needs a window tall enough for all of them, which is what the panel's
     * height used to be sized by hand for. */
    sp[k++] = (struct ui_spec){ .id = GP_ID_SCROLL, .cls = "box",
                                .flags = UI_SCROLL | UI_FILL_W, .weight = 1 };
    /* One grid holds every key/value pair — that is what makes the columns
     * shared instead of per-row. */
    sp[k++] = (struct ui_spec){ .id = GP_ID_GRID, .parent = GP_ID_SCROLL,
                                .cls = "box", .flags = UI_GRID | UI_FILL_W };

    g->n = 0;
    for (int i = 0; i < config_key_count() && g->n < GP_MAX_KEYS; i++) {
        const struct config_key_def* d = config_key_at(i);
        if (!d->group || !streq_(d->group, g->group)) continue;
        int row_id = 200 + g->n;
        const char* cur = config_get(d->key, d->def ? d->def : "");

        /* Label and control go into ONE grid, not a box per row: the grid
         * gives every key the SAME label-column width, so the controls line
         * up.  A box per row gave each its natural width and nothing aligned —
         * reported from use as "it all runs together". */
        (void)row_id;
        sp[k++] = (struct ui_spec){ .id = 0, .parent = GP_ID_GRID, .cls = "label",
                                    .text = d->key };

        struct ui_spec ctrl = { .id = GP_ID_CTRL(g->n), .parent = GP_ID_GRID,
                                .flags = UI_FILL_W };
        if (d->type == CFG_BOOL) {
            ctrl.cls = "checkbox";
            ctrl.text = d->help ? d->help : "";
            ctrl.value = (cur && cur[0] == '1');
        } else if (d->type == CFG_ENUM && d->values) {
            /* RADIO for a few options, COMBO for many.  Same data and the same
             * meaning — pick exactly one — but a radio group costs a row per
             * option, which is right for three and wrong for ten.  A layout
             * decision, taken from the data, not a second kind of setting. */
            int nopt = 1;
            for (const char* q = d->values; *q; q++) if (*q == ' ') nopt++;
            ctrl.cls = nopt > 3 ? "combo" : "radio";
            ctrl.text = d->values;              /* already space-separated */
            ctrl.value = gp_enum_index(d, cur);
        } else if (d->type == CFG_INT && d->max > d->min) {
            ctrl.cls = "slider";
            ctrl.min = d->min; ctrl.max = d->max;
            ctrl.value = gp_atoi(cur);
        } else {
            ctrl.cls = "textinput";
            ctrl.text = cur;
        }
        sp[k++] = ctrl;
        g->key_idx[g->n++] = i;
    }

    if (!g->n)
        sp[k++] = (struct ui_spec){ .id = 0, .cls = "label", .flags = UI_FILL_W,
                                    .text = "(no settings declared for this group)" };

    sp[k++] = (struct ui_spec){ .id = GP_ID_STATUS, .cls = "label",
                                .text = "change a setting, then Save",
                                .flags = UI_FILL_W };
    sp[k++] = (struct ui_spec){ .id = GP_ID_SAVE, .cls = "button", .text = "Save" };

    ui_build(win, sp, k, gp_event, g);
    kfree(sp);
}

/* Open the generic panel for `group`. */
static void generic_panel_open(const char* group) {
    struct genpanel* g = (struct genpanel*)kcalloc(1, sizeof *g);
    if (!g) return;
    g->group = group;
    /* Taller than the old panel because the controls are real now: a radio
     * group is one row per option, not one line of text.  Height that a
     * SCROLLING container should own — see the open item in DOCS §4.78. */
    int ow, oh;
    gui_window_outer_for_content(560, 360, &ow, &oh);   /* the viewport scrolls */
    gui_app_window_create(group, 140, 120, ow, oh, gp_layout, g);
}

void settings_panel_open(int i) {
    const struct settings_panel* p = settings_panel_at(i);
    if (!p) return;
    if (p->open) p->open();
    else         generic_panel_open(p->name);
}

/* =====================================================================
 * Key descriptors for settings whose owning code has no natural place to
 * declare them (a fault policy read inside an exception handler, a config
 * value consumed by a linker-section walk).  Everything with an obvious owner
 * declares itself THERE — gui.wallpaper in wallpaper.c, and so on — because
 * the registry only pays off if a new setting is a line next to the code that
 * reads it.
 * ===================================================================== */

CONFIG_KEY(ck_shell) = {
    .key = "gui.shell", .group = "Personalisation", .type = CFG_ENUM,
    .values = "vista bare", .def = "vista",
    .help = "desktop shell (takes effect at the next `gui` start)",
};
CONFIG_KEY(ck_desktop_view) = {
    .key = "desktop.view", .group = "Personalisation", .type = CFG_ENUM,
    .values = "grid list table", .def = "grid",
    .help = "how desktop shortcuts are arranged",
};
CONFIG_KEY(ck_cp_view) = {
    .key = "controlpanel.view", .group = "Personalisation", .type = CFG_ENUM,
    .values = "grid list table", .def = "grid",
    .help = "how the Control Panel arranges its categories",
};
CONFIG_KEY(ck_layout) = {
    .key = "keyboard.layout", .group = "Region and input", .type = CFG_ENUM,
    .values = "us hu", .def = "us",
    .help = "active keyboard layout - applies immediately",
};
CONFIG_KEY(ck_fault) = {
    .key = "kernel.fault_policy", .group = "System", .type = CFG_ENUM,
    .values = "halt reboot kill", .def = "halt",
    .help = "what a ring-0 fault does (ring-3 always kills just the task)",
};
CONFIG_KEY(ck_crash) = {
    .key = "crash.report", .group = "System", .type = CFG_BOOL, .def = "1",
    .help = "open the Crash Reports window when a record is delivered",
};
CONFIG_KEY(ck_dragstats) = {
    .key = "gui.drag_stats", .group = "System", .type = CFG_BOOL, .def = "0",
    .help = "print compositor timings when a window drag ends",
};
CONFIG_KEY(ck_closekill) = {
    .key = "gui.close_forces_kill", .group = "System", .type = CFG_BOOL, .def = "1",
    .help = "a second click on X force-kills an unresponsive client",
};
CONFIG_KEY(ck_closegrace) = {
    .key = "gui.close_grace_ms", .group = "System", .type = CFG_INT, .def = "10000",
    .help = "unattended backstop before a closing window is forced (ms)",
};
CONFIG_KEY(ck_selftest) = {
    .key = "kernel.selftest_ms", .group = "System", .type = CFG_INT, .def = "150",
    .help = "boot self-test window in ms (0 = skip; they cost 0.5 s of boot)",
};
CONFIG_KEY(ck_pkgstore) = {
    .key = "pkg.store", .group = "System", .type = CFG_ENUM,
    .values = "ram disk", .def = "ram",
    .help = "package store: ram rebuilds it each boot (82 ms), disk reuses it (7.8 s of reads)",
};
CONFIG_KEY(ck_fmview) = {
    .key = "fileman.view", .group = "Personalisation", .type = CFG_ENUM,
    .values = "table list grid", .def = "table",
    .help = "how the file manager shows a directory (applies to a new window)",
};
CONFIG_KEY(ck_scrollback) = {
    .key = "gui.scrollback", .group = "Personalisation", .type = CFG_INT,
    .def = "500",
    .help = "lines of terminal history kept per window (0 = none; applies to new windows)",
};
CONFIG_KEY(ck_autostart) = {
    .key = "gui.autostart", .group = "System", .type = CFG_BOOL, .def = "1",
    .help = "boot into the desktop (the text shell stays behind it: Start > Exit GUI)",
};
CONFIG_KEY(ck_splash) = {
    .key = "boot.splash", .group = "System", .type = CFG_ENUM,
    .values = "off on quiet", .def = "on",
    .help = "boot screen (applies at the next boot); the log is only hidden - dmesg keeps it",
};
CONFIG_KEY(ck_busadapt) = {
    .key = "bus.allow-adaptation", .group = "System", .type = CFG_BOOL, .def = "0",
    .help = "let the service bus adapt between contract versions",
};

/* ------------------------------------------------------------------- */
/* `conf` — the shell half.  Every setting must be reachable from the   */
/* shell: the automated checks here are greps over a serial log, so a   */
/* GUI-only setting cannot be regression-tested at all.                 */
/* ------------------------------------------------------------------- */

static const char* type_name(int t) {
    switch (t) {
        case CFG_BOOL:   return "bool";
        case CFG_ENUM:   return "enum";
        case CFG_INT:    return "int";
        case CFG_PATH:   return "path";
        default:         return "string";
    }
}

static const char* word_(const char* s, char* out, int cap) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && n < cap - 1) out[n++] = *s++;
    out[n] = 0;
    while (*s == ' ') s++;
    return s;
}

void settings_cmd(const char* args) {
    char cmd[16];
    const char* rest = word_(args ? args : "", cmd, sizeof cmd);

    if (!cmd[0] || streq_(cmd, "list")) {
        kprintf("settings panels (%d):\n", settings_panel_count());
        for (int i = 0; i < settings_panel_count(); i++) {
            const struct settings_panel* p = settings_panel_at(i);
            kprintf("  %s%s — %s\n", p->name, p->open ? " (own window)" : "",
                    p->summary ? p->summary : "");
        }
        kprintf("declared keys (%d):\n", config_key_count());
        for (int i = 0; i < config_key_count(); i++) {
            const struct config_key_def* d = config_key_at(i);
            kprintf("  [%s] %s (%s) = %s\n", d->group, d->key, type_name(d->type),
                    config_get(d->key, d->def ? d->def : "(unset)"));
        }
        return;
    }

    if (streq_(cmd, "show")) {
        char key[64];
        word_(rest, key, sizeof key);
        const struct config_key_def* d = config_key_find(key);
        if (!d) { kprintf("conf: '%s' has no descriptor\n", key); return; }
        kprintf("%s\n  group : %s\n  type  : %s\n", d->key, d->group,
                type_name(d->type));
        if (d->values) kprintf("  values: %s\n", d->values);
        kprintf("  default: %s\n  current: %s\n  %s\n",
                d->def ? d->def : "(none)",
                config_get(d->key, "(unset)"), d->help ? d->help : "");
        return;
    }

    if (streq_(cmd, "set")) {
        char key[64];
        const char* val = word_(rest, key, sizeof key);
        if (!key[0] || !*val) { kprintf("conf: set <key> <value>\n"); return; }
        /* THE difference from `setconf`: this one validates.  A wrong value is
         * refused here instead of being discovered later by whichever
         * subsystem reads it — or never. */
        if (config_key_validate(key, val) != 0) {
            const struct config_key_def* d = config_key_find(key);
            kprintf("conf: '%s' is not a valid %s for %s", val, type_name(d->type), key);
            if (d->values) kprintf(" (%s)", d->values);
            kprintf("\n");
            return;
        }
        if (config_apply(key, val) == 0) kprintf("%s = %s\n", key, val);
        else                             kprintf("conf: failed\n");
        return;
    }

    kprintf("conf: list | show <key> | set <key> <value>\n");
}
