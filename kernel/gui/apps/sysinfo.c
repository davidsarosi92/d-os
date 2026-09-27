/* =============================================================================
 * sysinfo.c — "System information": the machine, described (2026-09-28).
 *
 * Asked for from use: *"a system summary page where the machine's properties
 * are written down."*  Everything on it was already known to some subsystem
 * and reachable only by typing a different command for each (`uname`, `lscpu`
 * has no equivalent here, `meminfo`, `swap`, `disk`, `lsnic`, `mode`,
 * `lsaudio`) — the page is the one place that answers "what is this machine".
 *
 * A SETTINGS_PANEL with its own window, not a GUI_APP: the Start menu is full
 * (§M63's argument), and a description of the machine belongs beside the
 * machine's settings.  The data is a TABLE MODEL (§M65): two columns, the
 * property and its value, so the view, the density and the theme are the
 * toolkit's and this file only answers questions.
 *
 * LIVE, CHEAPLY.  Free memory, swap and uptime change, so the rows are
 * re-collected once a second and handed to `w_itemview_refresh`, whose content
 * diff damages only the rows whose text changed (§M69: a tick damages what it
 * changed) — normally the uptime row and nothing else.
 *
 * WHAT IT DOES NOT SAY: anything it would have to guess.  A board that does
 * not name itself is "not reported", an unrecognised ARM core is shown by its
 * MIDR value (hal_cpu_model), and nothing is inferred from the emulator.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "settings.h"
#include "icons.h"
#include "widget.h"
#include "itemview.h"
#include "ui.h"
#include "console_plate.h"
#include "locale.h"
#include "hal_api.h"
#include "percpu.h"
#include "pmm.h"
#include "swap.h"
#include "storage.h"
#include "net.h"
#include "fb_present.h"
#include "audio.h"
#include "timer.h"
#include "version.h"
#include "shellcmd.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

#define SI_MAX_ROWS 40

struct si_row {
    char prop[40];                  /* a catalogue key, or a key + a name  */
    char value[96];
};
static struct si_row si_rows[SI_MAX_ROWS];
static int si_n;

static struct gui_window* si_win;
static struct w_itemview* si_view;
static uint64_t si_last_ms;

/* ---- building rows ------------------------------------------------------ */

static int sput(char* b, int cap, int n, const char* s) {
    while (s && *s && n < cap - 1) b[n++] = *s++;
    b[n] = 0;
    return n;
}
static int sputu(char* b, int cap, int n, uint64_t v) {
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 23);
    while (k && n < cap - 1) b[n++] = t[--k];
    b[n] = 0;
    return n;
}
static struct si_row* si_add(const char* prop_key, const char* suffix) {
    if (si_n >= SI_MAX_ROWS) return NULL;
    struct si_row* r = &si_rows[si_n++];
    int n = sput(r->prop, sizeof r->prop, 0, prop_key);
    if (suffix) { n = sput(r->prop, sizeof r->prop, n, " "); sput(r->prop, sizeof r->prop, n, suffix); }
    r->value[0] = 0;
    return r;
}

/* A row's property is "<key>" or "<key> <name>" ("si.disk vda"): the key part
 * is translated at DRAW time and the name is not (a device name is data). */
static void si_prop_text(const struct si_row* r, char* out, int cap) {
    char key[40]; int k = 0;
    while (r->prop[k] && r->prop[k] != ' ' && k < (int)sizeof key - 1) { key[k] = r->prop[k]; k++; }
    key[k] = 0;
    int n = sput(out, cap, 0, lstr(key));
    if (r->prop[k] == ' ') sput(out, cap, n, r->prop + k);
}

static void si_net_row(struct net_device* d, void* ctx) {
    (void)ctx;
    if (!d || !d->name) return;
    struct si_row* r = si_add("si.net", d->name);
    if (!r) return;
    char* v = r->value; int cap = sizeof r->value, n = 0;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) n = sput(v, cap, n, ":");
        char b[3] = { hx[d->mac[i] >> 4], hx[d->mac[i] & 15], 0 };
        n = sput(v, cap, n, b);
    }
    if (d->ip) {
        n = sput(v, cap, n, "   ");
        for (int i = 3; i >= 0; i--) {
            n = sputu(v, cap, n, (d->ip >> (8 * i)) & 0xFF);
            if (i) n = sput(v, cap, n, ".");
        }
    } else {
        n = sput(v, cap, n, "   ");
        n = sput(v, cap, n, lstr("si.noaddr"));
    }
}

static void si_collect(void) {
    si_n = 0;
    struct si_row* r;
    char buf[96];

    if ((r = si_add("si.system", NULL)))
        sput(r->value, sizeof r->value, 0, "d-os " DOS_MILESTONE DOS_MILESTONE_NOTE);
    if ((r = si_add("si.arch", NULL)))
        sput(r->value, sizeof r->value, 0, hal_arch_name());
    if ((r = si_add("si.board", NULL))) {
        const char* b = hal_board_name();
        sput(r->value, sizeof r->value, 0, b ? b : lstr("si.notreported"));
    }
    if ((r = si_add("si.cpu", NULL))) {
        hal_cpu_model(buf, sizeof buf);
        sput(r->value, sizeof r->value, 0, buf);
    }
    if ((r = si_add("si.cpus", NULL)))
        sputu(r->value, sizeof r->value, 0, (uint64_t)smp_ncpus());
    if ((r = si_add("si.numa", NULL))) {
        int nn = pmm_node_count();
        sputu(r->value, sizeof r->value, 0, (uint64_t)(nn > 0 ? nn : 1));
    }
    if ((r = si_add("si.mem", NULL))) {
        int n = sputu(r->value, sizeof r->value, 0, pmm_nr_frames / 256u);
        n = sput(r->value, sizeof r->value, n, " MB, ");
        n = sputu(r->value, sizeof r->value, n, pmm_free_frames() / 256u);
        n = sput(r->value, sizeof r->value, n, " MB ");
        sput(r->value, sizeof r->value, n, lstr("si.free"));
    }
    if ((r = si_add("si.swap", NULL))) {
        uint32_t used = 0, total = 0;
        swap_stats(&used, &total);
        static const char* pol[] = { "off", "emergency", "normal" };
        int n = sputu(r->value, sizeof r->value, 0, used / 256u);
        n = sput(r->value, sizeof r->value, n, " MB ");
        n = sput(r->value, sizeof r->value, n, lstr("si.inuse"));
        n = sput(r->value, sizeof r->value, n, " (");
        n = sput(r->value, sizeof r->value, n, lstr(pol[swap_policy()]));
        sput(r->value, sizeof r->value, n, ")");
    }

    /* Disks and volumes. */
    struct storage_info st[8];
    int ns = storage_query(st, 8);
    for (int i = 0; i < ns; i++) {
        if (!(r = si_add("si.disk", st[i].name))) break;
        char sz[24];
        storage_fmt_size(st[i].bytes, sz, sizeof sz);
        int n = sput(r->value, sizeof r->value, 0, sz);
        n = sput(r->value, sizeof r->value, n, ", ");
        n = sput(r->value, sizeof r->value, n, lstr(st[i].fs));
        if (st[i].mounted) {
            n = sput(r->value, sizeof r->value, n, ", ");
            n = sput(r->value, sizeof r->value, n, st[i].mount);
            if (st[i].have_space) {
                storage_fmt_size(st[i].free, sz, sizeof sz);
                n = sput(r->value, sizeof r->value, n, " (");
                n = sput(r->value, sizeof r->value, n, sz);
                n = sput(r->value, sizeof r->value, n, " ");
                n = sput(r->value, sizeof r->value, n, lstr("si.free"));
                sput(r->value, sizeof r->value, n, ")");
            }
        }
    }
    if (ns == 0 && (r = si_add("si.disks", NULL)))
        sput(r->value, sizeof r->value, 0, lstr("si.none"));

    /* Network adapters. */
    int before = si_n;
    net_for_each(si_net_row, NULL);
    if (si_n == before && (r = si_add("si.network", NULL)))
        sput(r->value, sizeof r->value, 0, lstr("si.none"));

    /* Display and sound. */
    if ((r = si_add("si.display", NULL))) {
        struct fb_mode m;
        if (fb_mode_current(&m) == 0) {
            int n = sputu(r->value, sizeof r->value, 0, m.w);
            n = sput(r->value, sizeof r->value, n, " x ");
            n = sputu(r->value, sizeof r->value, n, m.h);
            n = sput(r->value, sizeof r->value, n, ", ");
            n = sputu(r->value, sizeof r->value, n, m.bpp);
            n = sput(r->value, sizeof r->value, n, " bpp, ");
            n = sputu(r->value, sizeof r->value, n, (uint64_t)fb_mode_count());
            n = sput(r->value, sizeof r->value, n, " ");
            sput(r->value, sizeof r->value, n, lstr("si.modes"));
        } else {
            sput(r->value, sizeof r->value, 0, lstr("si.none"));
        }
    }
    if ((r = si_add("si.audio", NULL))) {
        struct audio_dev* a = audio_primary();
        sput(r->value, sizeof r->value, 0, a && a->name ? a->name : lstr("si.none"));
    }
    if ((r = si_add("si.uptime", NULL))) {
        uint64_t s = timer_ticks_ms() / 1000;
        int n = 0;
        if (s >= 3600) { n = sputu(r->value, sizeof r->value, n, s / 3600); n = sput(r->value, sizeof r->value, n, " h "); }
        n = sputu(r->value, sizeof r->value, n, (s / 60) % 60);
        n = sput(r->value, sizeof r->value, n, " min ");
        n = sputu(r->value, sizeof r->value, n, s % 60);
        sput(r->value, sizeof r->value, n, " s");
    }
}

/* ---- the table model ------------------------------------------------------ */

static int si_count(void* c) { (void)c; return si_n; }
static char si_lbl[40];
static int si_get(void* c, int i, struct item_entry* out) {
    (void)c;
    if (i < 0 || i >= si_n) return -1;
    si_prop_text(&si_rows[i], si_lbl, sizeof si_lbl);
    out->label = si_lbl;
    out->icon  = ICON_NONE;
    out->dim   = 0;
    return 0;
}
static int si_columns(void* c) { (void)c; return 2; }
static const char* si_col_title(void* c, int col) {
    (void)c;
    return col == 0 ? "si.col.prop" : "si.col.value";
}
static int si_col_weight(void* c, int col) { (void)c; return col == 0 ? 1 : 3; }
static int si_col_style(void* c, int col) { (void)c; return col == 0 ? ICOL_DIM : 0; }
static int si_cell(void* c, int i, int col, char* out, int cap) {
    (void)c;
    if (i < 0 || i >= si_n || cap <= 0) return -1;
    if (col == 0) si_prop_text(&si_rows[i], out, cap);
    else          sput(out, cap, 0, si_rows[i].value);
    return 0;
}
static const struct item_model si_model = {
    .count = si_count, .get = si_get,
    .columns = si_columns, .col_title = si_col_title,
    .col_weight = si_col_weight, .cell = si_cell, .col_style = si_col_style,
};

/* ---- the window ------------------------------------------------------------ */

#define SI_ID_VIEW 1

static void si_tick(struct gui_window* win) {
    (void)win;
    uint64_t now = timer_ticks_ms();
    if (now - si_last_ms < 1000) return;
    si_last_ms = now;
    si_collect();
    if (si_view) w_itemview_refresh(si_view);
}

static void si_on_close(struct gui_window* w) {
    (void)w;
    si_view = NULL;
}

static void si_layout(struct gui_window* win) {
    if (ui_node_count(win) > 0) { ui_layout(win); return; }
    si_collect();
    si_last_ms = timer_ticks_ms();
    static const struct ui_spec spec[] = {
        { .id = SI_ID_VIEW, .cls = "view", .text = "table", .weight = 1,
          .flags = UI_FILL_W | UI_FOCUSABLE },
    };
    ui_build(win, spec, 1, NULL, NULL);
    si_view = (struct w_itemview*)ui_by_id(win, SI_ID_VIEW);
    if (si_view) w_itemview_set_model(&si_view->base, &si_model, NULL);
}

static void sysinfo_open(void) {
    struct gui_app_spec sp = {
        .title = "System information",
        .content_w = cp_px(620), .content_h = cp_px(420),
        .layout = si_layout, .tick = si_tick,
        .on_close = si_on_close, .slot = &si_win,
    };
    gui_app_open(&sp);
}

SETTINGS_PANEL(sp_sysinfo) = {
    .name    = "System information",
    .summary = "processor, memory, disks, network, display",
    .icon    = ICON_INFO,
    .open    = sysinfo_open,
};

/* `sysinfo` — the same rows on a console, walked through the SAME model, so
 * what a test greps is what the page shows (the `devices` command's rule). */
static void cmd_sysinfo(const char* args) {
    (void)args;
    si_collect();
    char p[48], v[96];
    for (int i = 0; i < si_n; i++) {
        si_cell(NULL, i, 0, p, sizeof p);
        si_cell(NULL, i, 1, v, sizeof v);
        kprintf("  %s: %s\n", p, v);
    }
}
SHELL_CMD(sysinfo) = { "sysinfo", "", "the machine, described: processor, memory, disks, network, display",
                       SHELL_G_SYS, cmd_sysinfo, SHELL_P_ANY };
