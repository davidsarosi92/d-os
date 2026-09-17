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
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "icons.h"
#include "config.h"
#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include "console_plate.h"
#include "locale.h"
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
 *      CFG_BOOL              -> SWITCH, not a checkbox — and the distinction is
 *                               the panel's whole semantics: everything here
 *                               goes through config_apply the moment it moves,
 *                               so the machine has ALREADY changed.  A checkbox
 *                               says "this will happen when you confirm", which
 *                               is a promise this panel does not make and has no
 *                               button to keep (widget_specs.md §4: "azonnali
 *                               hatás").  Rendering both states of affairs with
 *                               one control tells the user the wrong thing about
 *                               when their machine changed.
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
    /* §M69 — PENDING EDITS.  Reported from use: *"the change has to happen
     * when you click Save."*  This panel used to call `config_apply` the
     * instant a control moved, and then offered a Save button that only
     * WROTE THE FILE — so the machine had already changed and the button's
     * label promised something it did not do.  *A form with a commit button
     * that does not commit is the one arrangement that is worse than either
     * design on its own.*
     *
     * A pending edit is the TEXT, not the widget's value: `config_apply` takes
     * text, validation is defined on text, and a value would have to be
     * re-interpreted per key type at Save time — a second place to get the
     * enum-to-string mapping right. */
    char  pending[GP_MAX_KEYS][64];
    int   dirty[GP_MAX_KEYS];
    int   ndirty;
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
        /* APPLY, THEN PERSIST — in that order and both here.  `config_apply`
         * is what notifies the subsystem that read the key at boot (§M63's
         * watchers), so applying is what makes the change take effect; saving
         * is what makes it survive.  A Save that only wrote the file would
         * leave the running system on the old values, which is the mirror of
         * the defect this replaced. */
        for (int k = 0; k < g->n; k++) {
            if (!g->dirty[k]) continue;
            const struct config_key_def* dk = config_key_at(g->key_idx[k]);
            if (dk) config_apply(dk->key, g->pending[k]);
            g->dirty[k] = 0;
        }
        int had = g->ndirty;
        g->ndirty = 0;
        const char* p = config_persist_path();
        if (config_save() != 0)      gp_status(g, "set.savefail");
        else if (!p)                 gp_status(g, "set.applied_ram");
        else if (had)                gp_status(g, "set.applied");
        else                         gp_status(g, "set.nochange");
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
        gp_status(g, "set.rejected");
        return;
    }
    /* RECORDED, NOT APPLIED.  Nothing reaches the system until Save; see the
     * `pending` field.  Validation still happens HERE rather than at Save,
     * because a rejection has to point at the control the user just moved —
     * three rejections reported together at the end name nothing. */
    int k = i;
    int j = 0;
    while (text[j] && j < (int)sizeof g->pending[0] - 1) { g->pending[k][j] = text[j]; j++; }
    g->pending[k][j] = 0;
    if (!g->dirty[k]) { g->dirty[k] = 1; g->ndirty++; }
    gp_status(g, g->ndirty > 1 ? "set.unsaved_n"
                               : "set.unsaved_1");
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
                                    /* §M69 — THE KEY IS ITS OWN CATALOGUE
                                     * KEY.  Reported from use: *"the Control
                                     * Panel is full of untranslated labels
                                     * like `gui.theme`."*  It was showing the
                                     * raw identifier, which is right for a
                                     * `conf set` argument and wrong for a
                                     * label a person reads.
                                     *
                                     * No new descriptor field: `lstr` falls
                                     * back to its argument, so a key with a
                                     * catalogue entry shows a name and a key
                                     * without one shows exactly what it shows
                                     * today.  The identifier stays the
                                     * identifier — only what is DRAWN
                                     * changes. */
                                    .text = d->key };

        /* NO UI_FILL_W BY DEFAULT — rule 2 of the control convention
         * (console_plate.h).  Every control here used to fill the row, so a
         * switch and a radio group were as wide as the panel with their text
         * stranded at the left; only the controls that REPRESENT A RANGE ask
         * for the width below, because a half-width track says nothing about
         * the range it stands for. */
        struct ui_spec ctrl = { .id = GP_ID_CTRL(g->n), .parent = GP_ID_GRID };
        if (d->type == CFG_BOOL) {
            /* §M69 — A CHECKBOX AGAIN, and the reversal is the point.
             *
             * This was changed to a `switch` on the argument that a switch
             * says "the machine is like this NOW" while a checkbox promises
             * "when you confirm" — and that this panel had no confirm to
             * offer, because every control applied instantly.  **That premise
             * is gone**: Save is the commit point now, by request, so the
             * control that promises "when you confirm" is the honest one and
             * the switch would be the lie.
             *
             * Written down rather than quietly flipped, because from outside
             * it looks like drift.  *A decision derived from a premise has to
             * be revisited when the premise moves; the alternative is a
             * control that was right once.*  The `switch` class stays
             * registered and is the right control wherever a change really is
             * immediate (the volume flyout). */
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
            ctrl.flags |= UI_FILL_W;        /* a track IS its range */
        } else {
            ctrl.cls = "textinput";
            ctrl.text = cur;
            ctrl.flags |= UI_FILL_W;        /* a value can be any length */
        }
        sp[k++] = ctrl;
        g->key_idx[g->n++] = i;
    }

    if (!g->n)
        sp[k++] = (struct ui_spec){ .id = 0, .cls = "label", .flags = UI_FILL_W,
                                    .text = "(no settings declared for this group)" };

    sp[k++] = (struct ui_spec){ .id = GP_ID_STATUS, .cls = "label",
                                .text = "settings.hint",
                                .flags = UI_FILL_W };
    sp[k++] = (struct ui_spec){ .id = GP_ID_SAVE, .cls = "button", .text = "btn.save" };

    ui_build(win, sp, k, gp_event, g);
    kfree(sp);

    /* `gui.ui_dump` — the layout, on the serial line, at the moment it is built.
     *
     * THE HARNESS CANNOT TYPE ONCE A GUI WINDOW HAS FOCUS (§M64), so `ui dump`
     * is unreachable for exactly the windows most worth dumping: a panel is
     * opened by double-clicking, and from then on the keyboard belongs to it.
     * A config key can be set BEFORE the GUI takes over, which makes the dump
     * available on the one path that could not reach it. */
    if (config_get_long("gui.ui_dump", 0)) ui_dump(win);

    /* `gui.ui_scrolltest` — scroll the group's viewport from code, once, at
     * build time.
     *
     * THE INPUT TRANSPORT IS A HARNESS LIMIT, THE SCROLLING IS NOT.  A two-point
     * probe (ps2_mouse.c + gui.c) showed the QEMU monitor's `mouse_button 8/16`
     * produces ZERO wheel notches for a mouse decoding §M69's 4-byte packet —
     * so a wheel gesture cannot be delivered here at all, and "the panel did not
     * scroll" says nothing whatever about our routing.  This drives the same
     * model through `ui_scroll_by`, which is the half we own: if the offset
     * moves and the indicator follows, then everything above the packet decode
     * is working and exactly one link is untestable on this harness. */
    if (config_get_long("gui.ui_scrolltest", 0)) {
        int moved = ui_scroll_by(win, GP_ID_SCROLL, 400);
        kprintf("settings: ui_scroll_by(400) -> %s\n",
                moved ? "moved" : "declined");
        ui_dump(win);
    }

    /* §M69 — `gui.wheeltest` drives the WHEEL ROUTER, which `ui_scrolltest`
     * above deliberately does not: that one calls `ui_scroll_by` directly and
     * therefore proves the container scrolls while saying nothing about
     * whether a notch aimed at the CONTENT ever reaches it.  That distinction
     * is the whole of the report — *"on the scrollbar it is perfect, on the
     * content it is no good"* — and it is a claim about two POSITIONS, so the
     * probe has to name one.  Fired here because the harness cannot type once
     * a GUI window holds the focus (§4.74). */
    if (config_get_long("gui.wheeltest", 0)) {
        int cw = 0, ch = 0;
        gui_window_content_size(win, &cw, &ch);
        /* A MAP first: which points in this panel have a widget that would
         * swallow a notch?  Then one real notch, so the two can be compared. */
        /* Aim at the GROUP HEADING, which is outside the viewport: the point
         * the report was actually about. */
        kprintf("settings: wheeltest over the HEADING (%d,4)\n", cw / 3);
        gui_wheel_test(cw / 3, 4, -1);
    }
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
    gui_window_outer_for_content(cp_px(560), cp_px(360), &ow, &oh);   /* the viewport scrolls */
    gui_app_window_create(group, 140, 120, ow, oh, gp_layout, g);
}

/* `conf open` below hands the index over through a static because
 * `gui_queue_open` takes a bare function pointer — the app-host task it spawns
 * has no argument to carry one.  One request at a time, which is what a person
 * typing a command produces. */
static int g_open_idx = -1;
static void open_queued_panel(void) {
    int i = g_open_idx;
    g_open_idx = -1;
    if (i >= 0) settings_panel_open(i);
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
    .key = "gui.shell", .group = "Appearance", .type = CFG_ENUM,
    .values = "vista bare", .def = "vista",
    .help = "desktop shell (takes effect at the next `gui` start)",
    .scope = CFG_SCOPE_USER,
};
CONFIG_KEY(ck_desktop_view) = {
    .key = "desktop.view", .group = "Appearance", .type = CFG_ENUM,
    .values = "grid list table", .def = "grid",
    .help = "how desktop shortcuts are arranged",
    .scope = CFG_SCOPE_USER,
};
CONFIG_KEY(ck_cp_view) = {
    .key = "controlpanel.view", .group = "Appearance", .type = CFG_ENUM,
    .values = "grid list table", .def = "grid",
    .help = "how the Control Panel arranges its categories",
};
CONFIG_KEY(ck_layout) = {
    .key = "keyboard.layout", .group = "Region and input", .type = CFG_ENUM,
    .values = "us hu", .def = "us",
    .help = "active keyboard layout - applies immediately",
    .scope = CFG_SCOPE_USER,
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
    .key = "fileman.view", .group = "Appearance", .type = CFG_ENUM,
    .values = "table list grid", .def = "table",
    .help = "how the file manager shows a directory (applies to a new window)",
    .scope = CFG_SCOPE_USER,
};
CONFIG_KEY(ck_scrollback) = {
    .key = "gui.scrollback", .group = "Appearance", .type = CFG_INT,
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

    /* `conf open <name|index>` — open a settings panel WITHOUT A MOUSE.
     *
     * WHY THIS EXISTS.  This file's own header says every setting must be
     * reachable from the shell because the automated checks here are greps over
     * a serial log — and the PANEL was the one thing that was not: it is opened
     * by double-clicking a category, so reaching it needed a driven pointer,
     * and once it has focus the harness cannot type at all (§4.74).  A fault
     * that only happens when a panel opens was therefore reproducible only by
     * hand, which is most of why it survived a milestone.
     *
     * It goes through `gui_queue_open` and NOT a direct call: a window created
     * on a task with no app-host loop never lays out and never ticks (§M61),
     * so opening it from the shell task would produce a panel that LOOKS built
     * and answers nothing — a second failure mode on top of the one being
     * investigated. */
    if (streq_(cmd, "open")) {
        char which[48];
        /* §M32 — the WHOLE tail, not the first word.  §M70's rule is that a
         * verb owns its argument tail, and `conf open` had been taking one
         * word: every panel named so far happened to be a single word, and
         * "User accounts" was the first that was not — `conf: no panel 'User'`.
         * *A parser that is correct for every input tried so far is not a
         * parser, it is a coincidence.* */
        {
            int wi = 0;
            const char* r = rest;
            while (*r == ' ') r++;
            while (*r && wi < (int)sizeof which - 1) which[wi++] = *r++;
            while (wi > 0 && which[wi - 1] == ' ') wi--;   /* trailing spaces */
            which[wi] = 0;
        }
        if (!which[0]) { kprintf("conf: open <name|index>\n"); return; }
        int idx = -1;
        if (which[0] >= '0' && which[0] <= '9') {
            idx = 0;
            for (const char* p = which; *p >= '0' && *p <= '9'; p++)
                idx = idx * 10 + (*p - '0');
        } else {
            for (int i = 0; i < settings_panel_count(); i++)
                if (streq_(settings_panel_at(i)->name, which)) { idx = i; break; }
        }
        if (idx < 0 || idx >= settings_panel_count()) {
            kprintf("conf: no panel '%s' (try `conf list`)\n", which);
            return;
        }
        g_open_idx = idx;
        kprintf("conf: opening panel %d '%s'\n", idx, settings_panel_at(idx)->name);
        gui_queue_open(open_queued_panel);
        return;
    }

    kprintf("conf: list | show <key> | set <key> <value> | open <name|index>\n");
}

/* --- §M70 shell registration -----------------------------------------------
 * `conf set` VALIDATES against the CONFIG_KEY descriptors; `setconf` (config.c)
 * deliberately does not, because it must stay able to reach undeclared keys. */
SHELL_CMD(conf) = { "conf", "[list|show <key>|set <key> <value>|open <panel>]",
                    "declared settings, validated",
                    SHELL_G_SYS, settings_cmd, SHELL_P_ANY };
