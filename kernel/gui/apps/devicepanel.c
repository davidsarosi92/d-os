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
#include "hwdev.h"
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

static int put_hex(char* out, int cap, int n, unsigned v) {
    static const char* dig = "0123456789abcdef";
    char d[8];
    int k = 0;
    if (!v) d[k++] = '0';
    while (v && k < 8) { d[k++] = dig[v & 0xF]; v >>= 4; }
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
/* The model — ONE ROW PER PIECE OF HARDWARE.                        */
/*                                                                   */
/* This started driver-centric and was wrong in a way that only      */
/* showed once somebody asked the obvious question: *what if the     */
/* device has no driver?*  In a driver list it is not a row at all,  */
/* so the one case where a person has to go and FIND a driver was    */
/* the one case nothing reported.  A list of drivers cannot describe */
/* the hardware it does not cover.                                   */
/* ---------------------------------------------------------------- */

enum { COL_HW = 0, COL_ADDR, COL_ID, COL_DRIVER, COL_STATE, COL_WHERE,
       COL_ISO, COL_HOLDS, COL__COUNT };

/* Re-scanned on a tick, not on every cell: `get`/`cell` are called once per
 * column per row per repaint, and a PCI walk per cell would be hundreds of
 * config-space reads a frame. */
#define DM_MAX_HW 48
static struct hw_device dm_hw[DM_MAX_HW];
static int              dm_nhw = 0;

static void dm_rescan(void) { dm_nhw = hw_enumerate(dm_hw, DM_MAX_HW); }

static int dm_count(void* ctx) { (void)ctx; return dm_nhw; }

/* The driver behind row `i`, or NULL.  Two things can be true and neither
 * implies the other: hardware can be present with no driver, and a driver can
 * be registered with no hardware. */
static struct driver* dm_drv_at(int i) {
    if (i < 0 || i >= dm_nhw || !dm_hw[i].driver) return NULL;
    return driver_find(dm_hw[i].driver);
}

static char dm_label[80];

static int dm_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    if (i < 0 || i >= dm_nhw) return -1;
    struct hw_device* h = &dm_hw[i];
    struct driver* d = dm_drv_at(i);

    /* The single line the list and grid views get carries the two facts that
     * decide whether this row needs attention: what it is, and whether
     * anything is driving it. */
    int n = put(dm_label, sizeof dm_label, 0, h->name);
    n = put(dm_label, sizeof dm_label, n, " — ");
    if (!h->online)   n = put(dm_label, sizeof dm_label, n, "not present");
    else if (!d)      n = put(dm_label, sizeof dm_label, n,
                             (h->is_pci &&
                              !hw_needs_driver(h->class_code, h->subclass))
                             ? "no driver needed" : "NO DRIVER");
    else              n = put(dm_label, sizeof dm_label, n, dp_state(driver_state(d)));
    (void)n;

    out->label = dm_label;
    out->sub   = h->driver;
    out->icon  = ICON_CHIP;
    /* Dimmed = nothing is running against it, for either reason. */
    out->dim   = (d && (driver_state(d) & DRV_S_INITED)) ? 0 : 1;
    return 0;
}

static int dm_columns(void* ctx) { (void)ctx; return COL__COUNT; }

static const char* dm_col_title(void* ctx, int c) {
    (void)ctx;
    switch (c) {
    case COL_HW:     return "Hardware";
    case COL_ADDR:   return "Where";
    case COL_ID:     return "ID";
    case COL_DRIVER: return "Driver";
    case COL_STATE:  return "State";
    case COL_WHERE:  return "Runs in";
    case COL_ISO:    return "Isolation";
    case COL_HOLDS:  return "Holds";
    }
    return "";
}

static int dm_col_weight(void* ctx, int c) {
    (void)ctx;
    return (c == COL_HW || c == COL_WHERE) ? 2 : 1;
}

/* "0:3.0" — the PCI address, which is what a person compares against the
 * hypervisor's own device list.  Hex without width specifiers (this printf has
 * none), and the punctuation carries the structure. */
static int put_bdf(char* out, int cap, int n, uint16_t bdf) {
    n = put_hex(out, cap, n, (unsigned)(bdf >> 8));
    n = put(out, cap, n, ":");
    n = put_hex(out, cap, n, (unsigned)((bdf >> 3) & 0x1F));
    n = put(out, cap, n, ".");
    n = put_hex(out, cap, n, (unsigned)(bdf & 7));
    return n;
}

static int dm_cell(void* ctx, int i, int c, char* out, int cap) {
    (void)ctx;
    if (i < 0 || i >= dm_nhw || cap <= 0) return -1;
    struct hw_device* h = &dm_hw[i];
    struct driver* d = dm_drv_at(i);
    out[0] = 0;

    switch (c) {
    case COL_HW: put(out, cap, 0, h->name); break;

    case COL_ADDR:
        if (!h->online)     put(out, cap, 0, "offline");
        else if (h->is_pci) put_bdf(out, cap, 0, h->bdf);
        else                put(out, cap, 0, "platform");
        break;

    case COL_ID:
        if (h->is_pci) {
            int n = put_hex(out, cap, 0, h->vendor);
            n = put(out, cap, n, ":");
            put_hex(out, cap, n, h->device);
        }
        break;

    case COL_DRIVER:
        /* THE ROW THAT MATTERS.  Present hardware with nothing claiming it is
         * the only thing in this table that asks the user to go and do
         * something, so it says so in words rather than being blank — a blank
         * cell reads as "not applicable", which is the opposite. */
        if (h->driver)      put(out, cap, 0, h->driver);
        else if (h->online && h->is_pci &&
                 !hw_needs_driver(h->class_code, h->subclass))
            put(out, cap, 0, "(not needed)");
        /* ASCII, deliberately.  The console pads columns by BYTE count and an
         * em-dash is three bytes to one column, so "— none —" was twelve bytes
         * wide and eight columns wide and pushed the rest of the row sideways.
         * A table whose alignment depends on the encoding of one cell is a
         * table that will come apart again. */
        else if (h->online) put(out, cap, 0, "(none)");
        break;

    case COL_STATE:
        if (!h->online)  put(out, cap, 0, "not present");
        else if (!d) {
            /* "Needs one" and "wants none" are different answers, and printing
             * the first for a host bridge makes the column untrustworthy — after
             * which the row that DOES need attention reads like more of the
             * same. */
            if (h->is_pci && !hw_needs_driver(h->class_code, h->subclass))
                put(out, cap, 0, "no driver needed");
            else
                put(out, cap, 0, "needs a driver");
        }
        /* State 0 is "nothing has probed this yet" — §M67 attaches a module
         * without starting it.  `dp_state` prints "-" for that, which is the
         * driver registry's shorthand and tells a person nothing. */
        else if (driver_state(d) == 0) put(out, cap, 0, "not started");
        else             put(out, cap, 0, dp_state(driver_state(d)));
        break;

    case COL_WHERE:
        /* Only where something is actually RUNNING.  `dp_where` answers
         * "kernel" for any driver that is not placed in ring 3, which for a
         * driver whose hardware is absent is a claim about a thing that is not
         * happening. */
        if (d && (driver_state(d) & DRV_S_INITED)) dp_where(d, out, cap);
        break;

    case COL_ISO: {
        /* Same rule: an isolation verdict about a driver that is not running
         * describes nothing. */
        if (!d || !(driver_state(d) & DRV_S_INITED)) break;
        enum domain_isolation iso =
            domain_isolation_of(driver_domain(d),
                                (d->flags & DRVF_DMA) ? 1 : 0,
                                drvuser_confined(d->name));
        put(out, cap, 0, domain_isolation_name(iso));
        break;
    }

    case COL_HOLDS: if (d) drv_res_summary(d->name, out, cap); break;
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
    if (idx < 0 || idx >= dm_nhw) return;
    const char* nm = dm_hw[idx].driver;
    if (!nm) return;            /* hardware with no driver: nothing to act on */
    int i = 0;
    for (; nm[i] && i < (int)sizeof dm_sel_name - 1; i++)
        dm_sel_name[i] = nm[i];
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
    if (dm_sel < 0 || dm_sel >= dm_nhw) {
        w_label_set(dm_detail, "Select a device.");
        return;
    }
    struct hw_device* h = &dm_hw[dm_sel];
    struct driver* d = dm_selected();

    char msg[96];
    int n = put(msg, sizeof msg, 0, h->name);

    if (!h->online) {
        n = put(msg, sizeof msg, n, " — not present; the driver is here if it "
                                    "ever is");
        (void)n;
        w_label_set(dm_detail, msg);
        return;
    }
    if (!d) {
        /* The one row that asks for an action, so it gets the sentence rather
         * than a state word. */
        if (h->is_pci && !hw_needs_driver(h->class_code, h->subclass))
            n = put(msg, sizeof msg, n,
                    " — present; nothing drives it and nothing should");
        else
            n = put(msg, sizeof msg, n, " — present, and nothing here drives it");
        (void)n;
        w_label_set(dm_detail, msg);
        return;
    }

    n = put(msg, sizeof msg, n, " — driver ");
    n = put(msg, sizeof msg, n, d->name);
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
    /* Re-enumerate, not just repaint.  §M66 made hot-plug real, so a device can
     * APPEAR while this window is open — and a device manager that has to be
     * closed and reopened to notice new hardware is the one thing it must not
     * be.  The user asked for exactly this: if something changes, it has to
     * show here immediately. */
    dm_rescan();
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
    /* This layout BUILDS its widgets, so it must replace the old set rather
     * than stack a new one on top — see gui_window_clear_widgets. */
    gui_window_clear_widgets(win);
    dm_view = NULL; dm_detail = NULL;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    const int bar = 30;                 /* the action row at the bottom */
    dm_rescan();
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
        kprintf("devices — the hardware in this machine, and what drives it\n");
        kprintf("usage: devices [list]\n");
        kprintf("(actions live on `drv`: start | stop | domain | crash)\n");
        return;
    }

    dm_rescan();

    static const int width[COL__COUNT] = { 34, 9, 10, 12, 15, 22, 10, 18 };

    /* TWO SECTIONS, because they are two different questions and one list
     * cannot ask both.  "Present and nothing drives it" means go and find a
     * driver; "we have a driver and the hardware is not here" means nothing at
     * all.  A single State column makes them look like neighbouring degrees of
     * the same problem. */
    int online = 0, offline = 0, undriven = 0;
    for (int i = 0; i < dm_nhw; i++) {
        if (!dm_hw[i].online) { offline++; continue; }
        online++;
        if (!dm_hw[i].driver && (!dm_hw[i].is_pci ||
                                 hw_needs_driver(dm_hw[i].class_code,
                                                 dm_hw[i].subclass)))
            undriven++;
    }

    for (int pass = 0; pass < 2; pass++) {
        int want_online = (pass == 0);
        int shown = 0;
        for (int i = 0; i < dm_nhw; i++) {
            if (!!dm_hw[i].online != want_online) continue;
            if (!shown) {
                kprintf("\n%s:\n", want_online
                        ? "PRESENT — hardware this machine has"
                        : "NOT PRESENT — hardware we could drive and have not got");
                for (int c = 0; c < COL__COUNT; c++) {
                    /* The offline half has no bus address and no IDs, so its
                     * header does not pretend to. */
                    if (!want_online && (c == COL_ADDR || c == COL_ID)) continue;
                    col(dm_col_title(NULL, c), width[c]);
                }
                kprintf("\n");
            }
            shown++;
            for (int c = 0; c < COL__COUNT; c++) {
                if (!want_online && (c == COL_ADDR || c == COL_ID)) continue;
                char cell[64];
                if (dm_cell(NULL, i, c, cell, sizeof cell) != 0) cell[0] = 0;
                col(cell, width[c]);
            }
            kprintf("\n");
        }
        if (!shown)
            kprintf("\n%s: none\n", want_online ? "PRESENT" : "NOT PRESENT");
    }

    kprintf("\n%d present (%d with no driver), %d known but absent\n",
            online, undriven, offline);
    /* Say what to DO about it, once, at the bottom — the count above is a fact
     * and this is the action it implies, and running them together in every row
     * would bury both. */
    if (undriven)
        kprintf("hardware with no driver is not a fault; it means this system "
                "has nothing that claims it\n");
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
