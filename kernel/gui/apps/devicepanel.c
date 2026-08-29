/* =============================================================================
 * devicepanel.c — the device manager: what hardware this machine has, where
 * each driver runs, and what that placement is actually worth.
 *
 * §M33 built a great deal that could only be reached by typing: `lsdrv` for
 * state, `drv domain` for placement and isolation, `drv res` for what a driver
 * holds, `drvuser_*` for the ring-3 half.  Four commands, four separate lists,
 * and the ONE question a person actually asks — *what is this device doing and
 * is it all right* — was not answerable from any single one of them.
 *
 * A SETTINGS_PANEL(), NOT A GUI_APP, and that is §M63's whole argument being
 * used rather than restated: the Start menu has twelve slots with eleven taken,
 * so one entry per new tool does not fit.  The Control Panel is the container
 * that makes adding a tool a row.
 *
 * -----------------------------------------------------------------------------
 * WHAT THE COLUMNS ARE FOR
 *
 * Each column answers a question that the others cannot, which is the test a
 * column has to pass to be here at all:
 *
 *   Device / Class  what it is
 *   State           OK / absent / stopped / QUARANTINED — and the last two are
 *                   deliberately distinct (§M66): "the user turned it off" and
 *                   "it misbehaved" look the same to the automatic paths and
 *                   are entirely different facts to the person reading them.
 *   Runs in         kernel, or ring 3 WITH THE PID.  The pid is not decoration:
 *                   a driver that has been restarted has a different one, so
 *                   this column is where a supervisor's work becomes visible.
 *   Isolation       none / advisory / full.  The column §M33 exists for, and
 *                   the one that must never flatter: a DMA driver in ring 3
 *                   with nothing holding its device reports ADVISORY, because
 *                   reporting anything better is the isolation theatre that
 *                   plan refuses by name.
 *   Restarts        how many times it has died and come back.  A quiet 0 and a
 *                   quiet 7 are the same picture in every other column.
 *   Holds           ports / irq N / mmio / dma.
 *
 * -----------------------------------------------------------------------------
 * WHY IT CAN ALSO BREAK THINGS
 *
 * Start, Stop, a domain change and `drv crash` — because a panel that can only
 * observe cannot be used to TEST anything, and the mechanisms it displays are
 * exactly the ones whose correctness depends on having been made to fail
 * (§M31's argument for `hardlock`, §M33's for `drv crash`).  Crash is kept
 * visibly separate from the rest for the same reason it is spelled out in the
 * shell: it is the one button here that is supposed to break something.
 *
 * -----------------------------------------------------------------------------
 * THE SHELL COMMAND IS THE TEST, AND IT WALKS THIS MODEL
 *
 * `devices` prints the rows by calling THIS FILE'S `cell()` — not by re-reading
 * the registry.  That is what makes the panel falsifiable on a headless run:
 * every automated check in this project is a grep over a serial log, and a
 * command that reassembled the same facts by a second route would pass while
 * the panel showed something else (§M64's `shortcut check`, which prints the
 * view's own hit-test answer rather than a recomputed one).
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "itemview.h"
#include "settings.h"
#include "config.h"
#include "driver.h"
#include "drvuser.h"
#include "drvrt.h"
#include "domain.h"
#include "printf.h"
#include <stddef.h>

/* ---------------------------------------------------------------- */
/* Small string helpers.  The kernel printf has no width specifiers  */
/* (CLAUDE.md says so, and this project has now been bitten by it    */
/* three times), so a table's cells are assembled by hand.           */
/* ---------------------------------------------------------------- */

static int put(char* out, int cap, int n, const char* s) {
    if (!s) return n;
    for (int i = 0; s[i] && n < cap - 1; i++) out[n++] = s[i];
    if (n < cap) out[n] = 0;
    return n;
}

static int put_int(char* out, int cap, int n, int v) {
    if (v < 0) { n = put(out, cap, n, "-"); v = -v; }
    char d[12];
    int k = 0;
    if (!v) d[k++] = '0';
    while (v && k < 12) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && n < cap - 1) out[n++] = d[--k];
    if (n < cap) out[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- */
/* The facts, in one place.                                          */
/* ---------------------------------------------------------------- */

/* Mirrors driver.c's `state_label`.  Deliberately a SECOND copy rather than an
 * export: that one is a console string and this one is a table cell, and the
 * day the table wants "off" where the log wants "stopped" the two must be free
 * to differ.  They are three words each; a shared function would couple two
 * presentations to save nothing. */
static const char* dp_state(uint8_t st) {
    if (st & DRV_S_QUARANTINE) return "QUARANTINED";
    if (st & DRV_S_ADMIN_DOWN) return "stopped";
    if (st & DRV_S_INITED)     return "OK";
    if (st & DRV_S_INIT_FAIL)  return "init failed";
    if (st & DRV_S_PROBE_FAIL) return "absent";
    if (st & DRV_S_PROBED)     return "probed";
    return "-";
}

/* Where it RUNS, which is not always where config says it should.  The live
 * process is the authority: a domain change takes effect at the next restart,
 * and between the two the honest answer is neither "kernel" nor "ring 3" but
 * the pair of them (§M33's own rule, and the reason `lsdrv` prints it that
 * way).  A column has less room than a log line, so the pending case is a
 * suffix rather than a sentence. */
static void dp_where(struct driver* d, char* out, int cap) {
    int n = 0;
    int pid = drvuser_pid(d->name);
    if (pid > 0) {
        n = put(out, cap, n, "ring 3 pid ");
        n = put_int(out, cap, n, pid);
    } else {
        n = put(out, cap, n, "kernel");
        if (driver_domain(d) == DOMAIN_USER)
            n = put(out, cap, n, " (→ring 3 on restart)");
    }
    (void)n;
}

/* ---------------------------------------------------------------- */
/* The model.                                                        */
/* ---------------------------------------------------------------- */

enum { COL_NAME = 0, COL_CLASS, COL_STATE, COL_WHERE, COL_ISO, COL_RESTARTS,
       COL_HOLDS, COL__COUNT };

static int dm_count(void* ctx) { (void)ctx; return driver_count_all(); }

static char dm_label[64];

static int dm_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    struct driver* d = driver_at(i);
    if (!d) return -1;
    /* The list/grid views show one line, so it carries the two facts a person
     * scanning for trouble needs: the name and the state. */
    int n = put(dm_label, sizeof dm_label, 0, d->name);
    n = put(dm_label, sizeof dm_label, n, " — ");
    n = put(dm_label, sizeof dm_label, n, dp_state(driver_state(d)));
    (void)n;
    out->label = dm_label;
    out->sub   = d->class;
    out->icon  = ICON_CHIP;
    /* Dimmed = not running.  The one visual difference that survives being
     * squinted at from across the room. */
    out->dim   = (driver_state(d) & DRV_S_INITED) ? 0 : 1;
    return 0;
}

static int dm_columns(void* ctx) { (void)ctx; return COL__COUNT; }

static const char* dm_col_title(void* ctx, int c) {
    (void)ctx;
    switch (c) {
    case COL_NAME:     return "Device";
    case COL_CLASS:    return "Class";
    case COL_STATE:    return "State";
    case COL_WHERE:    return "Runs in";
    case COL_ISO:      return "Isolation";
    case COL_RESTARTS: return "Restarts";
    case COL_HOLDS:    return "Holds";
    }
    return "";
}

static int dm_col_weight(void* ctx, int c) {
    (void)ctx;
    return (c == COL_WHERE || c == COL_HOLDS) ? 2 : 1;
}

static int dm_cell(void* ctx, int i, int c, char* out, int cap) {
    (void)ctx;
    struct driver* d = driver_at(i);
    if (!d || cap <= 0) return -1;
    out[0] = 0;
    switch (c) {
    case COL_NAME:  put(out, cap, 0, d->name); break;
    case COL_CLASS: put(out, cap, 0, d->class ? d->class : "?"); break;
    case COL_STATE: put(out, cap, 0, dp_state(driver_state(d))); break;
    case COL_WHERE: dp_where(d, out, cap); break;
    case COL_ISO: {
        enum domain_isolation iso =
            domain_isolation_of(driver_domain(d),
                                (d->flags & DRVF_DMA) ? 1 : 0,
                                drvuser_confined(d->name));
        put(out, cap, 0, domain_isolation_name(iso));
        break;
    }
    case COL_RESTARTS: put_int(out, cap, 0, drvuser_restarts(d->name)); break;
    case COL_HOLDS:    drv_res_summary(d->name, out, cap); break;
    default: return -1;
    }
    return 0;
}

static void dm_activate(void* ctx, int i);      /* below — needs the window */

static const struct item_model dm_model = {
    .count = dm_count, .get = dm_get, .activate = dm_activate, .ctx = NULL,
    .columns = dm_columns, .col_title = dm_col_title,
    .col_weight = dm_col_weight, .cell = dm_cell,
};

/* ---------------------------------------------------------------- */
/* The window.                                                       */
/* ---------------------------------------------------------------- */

static struct gui_window* dm_win = NULL;
static struct w_itemview* dm_view = NULL;
static struct w_label*    dm_detail = NULL;
static int                dm_sel = -1;
/* Set by an action, consumed by the tick: an action changes what every row
 * says, and the row it changed may no longer exist (a stop can withdraw a
 * loaded driver).  Repainting from the action itself would draw from the
 * middle of a registry edit. */
static volatile int       dm_dirty = 0;

/* The selection is remembered BY NAME, not by index, and the actions resolve it
 * through `driver_find` every time.
 *
 * An index is not stable here: the registry is a slot table, `rmmod` removes a
 * slot and §M66's rescan can add one, and both can happen while this window is
 * open because neither goes through it.  A stale index does not fail — it names
 * a DIFFERENT driver, so "Crash" would stop a device the user never pointed at.
 * *An identifier that silently means something else after an unrelated event is
 * the wrong identifier*, and the cost of the right one is a 32-byte copy. */
static char dm_sel_name[32];

static void dm_remember(int idx) {
    dm_sel = idx;
    dm_sel_name[0] = 0;
    struct driver* d = driver_at(idx);
    if (!d || !d->name) return;
    int i = 0;
    for (; d->name[i] && i < (int)sizeof dm_sel_name - 1; i++)
        dm_sel_name[i] = d->name[i];
    dm_sel_name[i] = 0;
}

static struct driver* dm_selected(void) {
    if (!dm_sel_name[0]) return NULL;
    return driver_find(dm_sel_name);
}

/* The line under the table: everything that did not earn a column.  Written as
 * a sentence rather than more columns because these are the facts you want
 * once you have already picked a row — the WHY behind the isolation verdict
 * most of all, since "advisory" alone does not say whether that is this
 * machine's limit or work nobody has done, and those call for different
 * decisions (§M33 stage 5). */
static void dm_refresh_detail(void) {
    if (!dm_detail) return;
    struct driver* d = dm_selected();
    if (!d) { w_label_set(dm_detail, "Select a device."); return; }

    char msg[96];
    int n = put(msg, sizeof msg, 0, d->name);
    n = put(msg, sizeof msg, n, " v");
    n = put(msg, sizeof msg, n, d->version ? d->version : "?");

    char decl[40];
    domain_set_str(d->domains ? d->domains : DOMAIN_KERNEL, decl, sizeof decl);
    n = put(msg, sizeof msg, n, ", can run: ");
    n = put(msg, sizeof msg, n, decl);

    if (d->flags & DRVF_DMA) {
        const char* why = domain_isolation_reason(1);
        if (why && drvuser_confined(d->name) == 0) {
            n = put(msg, sizeof msg, n, " — DMA: ");
            n = put(msg, sizeof msg, n, why);
        } else {
            n = put(msg, sizeof msg, n, " — DMA, device confined");
        }
    } else if (d->flags & DRVF_BOOT_CRITICAL) {
        n = put(msg, sizeof msg, n, " — boot-critical, cannot be moved");
    }
    (void)n;
    w_label_set(dm_detail, msg);
}

static void dm_on_select(struct w_itemview* iv, int idx, void* ctx) {
    (void)iv; (void)ctx;
    dm_remember(idx);
    dm_refresh_detail();
    if (dm_win) gui_window_request_redraw(dm_win);
}

static void dm_activate(void* ctx, int i) {
    (void)ctx;
    dm_remember(i);
    dm_refresh_detail();
    if (dm_win) gui_window_request_redraw(dm_win);
}

/* ---- actions -------------------------------------------------------------- */

static void act_start(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct driver* d = dm_selected();
    if (d) driver_start(d->name);
    dm_dirty = 1;
}

static void act_stop(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct driver* d = dm_selected();
    if (d) driver_stop(d->name);
    dm_dirty = 1;
}

/* One button, both directions.  A pair of "Move to ring 3" / "Move to kernel"
 * buttons would need one of them greyed at all times, and a control whose only
 * state is disabled is a control nobody learns the meaning of. */
static void act_domain(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct driver* d = dm_selected();
    if (!d) return;
    driver_set_domain(d->name,
                      driver_domain(d) == DOMAIN_USER ? "kernel" : "user");
    dm_dirty = 1;
}

static void act_crash(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    struct driver* d = dm_selected();
    if (d) driver_crash(d->name);
    dm_dirty = 1;
}

/* ---- window plumbing ------------------------------------------------------ */

/* The table is live: a driver can be quarantined, restarted or hot-plugged
 * while this window is open, by something that is not this window.  A device
 * manager that shows a snapshot from the moment it opened is worse than no
 * device manager, because it is confidently wrong exactly when a person is
 * watching for a change (§M66's rescan, §M33's supervisor). */
static int dm_ticks = 0;

static void dm_tick(struct gui_window* win) {
    /* The window tick is ~500 ms (gui.c), and this acts on every other one —
     * so about once a second, which is enough to watch a restart land and
     * cheap enough to ignore.  An action's own change is never waited for: it
     * sets `dm_dirty` and is picked up on the next tick regardless. */
    if (!dm_dirty && (++dm_ticks & 1)) return;
    dm_ticks = 0;
    dm_dirty = 0;
    /* A driver that has LEFT the registry (rmmod) clears the selection, rather
     * than leaving buttons pointing at a name nothing answers to. */
    if (dm_sel_name[0] && !driver_find(dm_sel_name)) {
        dm_sel_name[0] = 0;
        dm_sel = -1;
    }
    dm_refresh_detail();
    gui_window_request_redraw(win);
}

static void dm_on_close(struct gui_window* w) {
    (void)w;
    dm_win = NULL; dm_view = NULL; dm_detail = NULL;
    dm_sel = -1; dm_sel_name[0] = 0;
}

static void dm_layout(struct gui_window* win) {
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    const int bar = 30;                 /* the action row at the bottom */
    dm_view = w_itemview_create(win, 6, 6, cw - 12, ch - bar - 28,
                                &dm_model,
                                config_get("devices.view", "table"), NULL);
    if (dm_view) dm_view->on_select = dm_on_select;

    dm_detail = w_label_create(win, 6, ch - bar - 20, cw - 12, "Select a device.");

    int y = ch - bar + 2, x = 6;
    w_button_create(win, x, y, 58, 22, "Start", act_start, NULL);   x += 62;
    w_button_create(win, x, y, 58, 22, "Stop",  act_stop,  NULL);   x += 62;
    w_button_create(win, x, y, 92, 22, "Move", act_domain, NULL);
    /* Crash sits at the far right, away from the others.  It is the one
     * control here whose whole purpose is to break something, and the gap is
     * the only thing standing between a curious click and a stopped device. */
    w_button_create(win, cw - 74, y, 68, 22, "Crash", act_crash, NULL);
}

static void devices_panel_open(void) {
    if (dm_win) { gui_window_raise(dm_win); return; }
    int ow, oh;
    gui_window_outer_for_content(760, 380, &ow, &oh);
    dm_win = gui_app_window_create("Devices", 120, 100, ow, oh, dm_layout, NULL);
    if (dm_win) {
        gui_window_set_on_close(dm_win, dm_on_close);
        gui_window_set_tick(dm_win, dm_tick);
    }
}

/* ---------------------------------------------------------------- */
/* `devices` — the same rows, on a serial line.                      */
/* ---------------------------------------------------------------- */

static int dev_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Pad to a column width by hand.  This is the function the kernel printf's
 * missing width specifiers cost us, and it is cheaper to write than to keep
 * rediscovering that `%-12s` prints literally. */
static void col(const char* s, int w) {
    int n = 0;
    for (; s[n]; n++) kprintf("%c", s[n]);
    for (; n < w; n++) kprintf(" ");
    kprintf(" ");
}

void devices_cmd(const char* args) {
    while (args && *args == ' ') args++;

    if (args && *args && !dev_streq(args, "list")) {
        kprintf("devices — the device manager's table, on the console\n");
        kprintf("usage: devices [list]\n");
        kprintf("(actions live on `drv`: start | stop | domain | crash)\n");
        return;
    }

    static const int width[COL__COUNT] = { 12, 8, 12, 24, 10, 8, 20 };

    for (int c = 0; c < COL__COUNT; c++) col(dm_col_title(NULL, c), width[c]);
    kprintf("\n");

    int n = dm_count(NULL);
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < COL__COUNT; c++) {
            char cell[48];
            if (dm_cell(NULL, i, c, cell, sizeof cell) != 0) cell[0] = 0;
            col(cell, width[c]);
        }
        kprintf("\n");
    }
    kprintf("%d device(s)\n", n);
}

SETTINGS_PANEL(sp_devices) = {
    .name    = "Devices",
    .summary = "drivers, where they run, what they hold",
    .icon    = ICON_CHIP,
    .open    = devices_panel_open,
};

CONFIG_KEY(ck_devices_view) = {
    .key = "devices.view", .group = "Devices", .type = CFG_ENUM,
    .values = "table list grid", .def = "table",
    .help = "how the device manager lays its devices out",
};
