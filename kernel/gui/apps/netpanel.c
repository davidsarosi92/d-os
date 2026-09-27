/* =============================================================================
 * netpanel.c — the Control Panel's Network page, and the Wi-Fi chooser (§M87).
 *
 * Asked for directly: *"Network and its settings belong in the Control Panel;
 * a network icon next to the clock, like the sound one — is there a network or
 * not, is there Wi-Fi or not — and a Wi-Fi chooser, Windows or Linux style."*
 *
 * Until now the network was reachable only by typing: `lsnic` for the
 * adapters, `dhcp` for an address, `ping` to find out whether any of it
 * worked, and `setconf` for a key nobody could name.  Three windows here:
 *
 *   NETWORK       one row per adapter: type, state, address and — the column
 *                 that did not exist anywhere — WHERE the address came from
 *                 (built-in default / DHCP / static).  Enable, Disable, Renew,
 *                 Configure, Test, Wi-Fi.
 *   ADDRESS       the configuration dialog: automatic (DHCP), manual (static)
 *                 or the driver's built-in default, persisted through §M63's
 *                 keys so the Save here and a `conf set` at a prompt are the
 *                 SAME operation (net_state.c's watcher applies both).
 *   WI-FI         the networks in range, their signal and security, and a
 *                 passphrase field.  The tray's flyout offers the open ones
 *                 in one click and sends a secured one here.
 *
 * -----------------------------------------------------------------------------
 * NOTHING HERE BLOCKS THE WINDOW
 *
 * A DHCP exchange waits up to six seconds; joining a network, a ping and a DNS
 * lookup wait too.  A window's host task that sits in one of those cannot
 * repaint or take a click, which looks exactly like a hung program (§M22.7's
 * whole reason for per-app tasks).  So every action that waits runs on a
 * detached task of its own and reports through `g_result`, which the window's
 * tick picks up.  One action at a time: a second press while one is running
 * says so rather than queueing a pile of DHCP exchanges.
 *
 * -----------------------------------------------------------------------------
 * THE ICON AND THE PANEL READ ONE FUNCTION
 *
 * `net_get_state()` (net_state.c).  If the tray drew its own conclusion from
 * net_device, the two would eventually disagree — the tray green while the
 * panel says "no address" — and a user holding two contradicting answers
 * trusts neither.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "ui.h"
#include "console_plate.h"
#include "itemview.h"
#include "settings.h"
#include "config.h"
#include "locale.h"
#include "net.h"
#include "dhcp.h"
#include "netui.h"
#include "task.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/* String helpers — the kernel printf has no width specifiers and    */
/* no snprintf, so cells are assembled by hand (devicepanel's rule). */
/* ---------------------------------------------------------------- */

static int put(char* o, int cap, int n, const char* s) {
    if (!s) return n;
    for (int i = 0; s[i] && n < cap - 1; i++) o[n++] = s[i];
    if (n < cap) o[n] = 0;
    return n;
}
static int put_int(char* o, int cap, int n, int v) {
    if (v < 0) { n = put(o, cap, n, "-"); v = -v; }
    char d[12]; int k = 0;
    if (!v) d[k++] = '0';
    while (v && k < 12) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k && n < cap - 1) o[n++] = d[--k];
    if (n < cap) o[n] = 0;
    return n;
}
static int put_ip(char* o, int cap, int n, uint32_t ip) {
    char b[16];
    net_fmt_ip(ip, b);
    return put(o, cap, n, b);
}
static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int seq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------------------------------------------------------------- */
/* Background actions.                                               */
/* ---------------------------------------------------------------- */

/* One action at a time, for the whole network UI.  `g_busy` is claimed by the
 * PRESSING task (a window host) with an atomic exchange, so two windows cannot
 * both start one. */
static volatile int  g_busy;
static volatile int  g_result_ready;
static char          g_result[128];

static char g_job_dev[16];
static char g_job_ssid[WIFI_SSID_MAX];
static char g_job_pass[64];

static int job_claim(void) {
    return __atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQ_REL) == 0;
}
static void job_done(const char* msg) {
    scopy(g_result, msg, sizeof g_result);
    __atomic_store_n(&g_result_ready, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
}

static void job_dhcp(void) {
    struct net_device* d = net_find(g_job_dev);
    char m[128];
    if (!d) { job_done("net.msg.nodev"); task_exit(); return; }
    if (dhcp_configure(d) == 0) {
        int n = put(m, sizeof m, 0, lstr("net.msg.leased"));
        n = put(m, sizeof m, n, " ");
        put_ip(m, sizeof m, n, d->ip);
        job_done(m);
    } else {
        job_done("net.msg.nodhcp");
    }
    task_exit();
}

/* THE TEST answers the two questions a person means by "does the network
 * work": can I reach the router, and can I resolve a name.  Two separate
 * results, because "the gateway answers but names do not resolve" is a DNS
 * problem and "nothing answers" is a link problem — and a single pass/fail
 * would hide which. */
static void job_test(void) {
    struct net_device* d = net_find(g_job_dev);
    if (!d) d = net_primary();
    char m[128];
    if (!d) { job_done("net.msg.nodev"); task_exit(); return; }
    int gw_ok = d->gateway ? (net_ping(d, d->gateway, 1) > 0) : 0;
    uint32_t ip = 0;
    int dns_ok = net_dns_query(d, "example.com", &ip) == 0 && ip != 0;
    int n = put(m, sizeof m, 0, lstr("net.msg.gateway"));
    n = put(m, sizeof m, n, gw_ok ? lstr("net.msg.answers") : lstr("net.msg.silent"));
    n = put(m, sizeof m, n, "  ");
    n = put(m, sizeof m, n, lstr("net.msg.dns"));
    put(m, sizeof m, n, dns_ok ? lstr("net.msg.resolves") : lstr("net.msg.fails"));
    kprintf("net: test via %s - gateway %s, dns %s\n", d->name,
            gw_ok ? "answers" : "silent", dns_ok ? "resolves" : "fails");
    job_done(m);
    task_exit();
}

static void job_join(void) {
    struct net_device* d = net_first_wireless();
    if (!d) { job_done("net.msg.nowifi"); task_exit(); return; }
    int r = d->wireless->connect(d, g_job_ssid, g_job_pass);
    /* Wipe the passphrase as soon as it has been used: a static buffer that
     * keeps it would be readable by the next thing that dumps kernel memory. */
    for (int i = 0; i < (int)sizeof g_job_pass; i++) g_job_pass[i] = 0;
    job_done(r == 0 ? "net.msg.joined" : r == -1 ? "net.msg.nonet"
           : r == -2 ? "net.msg.badpass" : "net.msg.radio");
    task_exit();
}

static void job_leave(void) {
    struct net_device* d = net_first_wireless();
    if (d) d->wireless->disconnect(d);
    job_done("net.msg.left");
    task_exit();
}

static int start_job(const char* name, void (*fn)(void)) {
    if (!job_claim()) return -1;
    g_result_ready = 0;
    if (!task_spawn_detached(name, fn)) { job_done("net.msg.notask"); return -1; }
    return 0;
}

int netui_wifi_join_async(const char* ssid) {
    if (!ssid) return -1;
    if (g_busy) return -1;
    scopy(g_job_ssid, ssid, sizeof g_job_ssid);
    g_job_pass[0] = 0;
    return start_job("wifi-join", job_join);
}

int netui_wifi_leave_async(void) { return start_job("wifi-leave", job_leave); }

/* ================================================================ */
/* 1. The Network page.                                              */
/* ================================================================ */

enum { NC_NAME = 0, NC_TYPE, NC_STATE, NC_ADDR, NC_SRC, NC_MAC, NC__COUNT };

#define NP_MAX 8
static struct net_device* np_dev[NP_MAX];
static int np_n;
static int np_sel = -1;
static char np_sel_name[16];

static struct gui_window* np_win;
static struct w_itemview* np_view;
static struct w_label*    np_status;
static struct w_label*    np_detail;
static struct w_label*    np_result;

static void np_collect_one(struct net_device* d, void* c) {
    (void)c;
    if (np_n < NP_MAX) np_dev[np_n++] = d;
}
static void np_rescan(void) {
    np_n = 0;
    net_for_each(np_collect_one, NULL);
    np_sel = -1;
    for (int i = 0; i < np_n; i++)
        if (np_sel_name[0] && seq(np_dev[i]->name, np_sel_name)) np_sel = i;
}

static struct net_device* np_selected(void) {
    return (np_sel >= 0 && np_sel < np_n) ? np_dev[np_sel] : NULL;
}

static const char* dev_type(struct net_device* d) {
    if (d->flags & NETDEV_F_LOOPBACK) return "net.type.loopback";
    if (d->wireless) return (d->flags & NETDEV_F_SIMULATED)
                            ? "net.type.wifisim" : "net.type.wifi";
    return "net.type.wired";
}

static const char* dev_state(struct net_device* d) {
    if (d->admin_down) return "net.st.disabled";
    if (d->flags & NETDEV_F_LOOPBACK) return "net.st.up";
    int l = net_link_state(d);
    if (l == 0) return d->wireless ? "net.st.notjoined" : "net.st.nocable";
    if (!d->ip) return "net.st.noaddr";
    return l > 0 ? "net.st.up" : "net.st.upunknown";
}

static int np_count(void* c) { (void)c; return np_n; }
static char np_lbl[48];
static int np_get(void* c, int i, struct item_entry* out) {
    (void)c;
    if (i < 0 || i >= np_n) return -1;
    struct net_device* d = np_dev[i];
    int n = put(np_lbl, sizeof np_lbl, 0, d->name);
    n = put(np_lbl, sizeof np_lbl, n, " - ");
    put(np_lbl, sizeof np_lbl, n, lstr(dev_state(d)));
    out->label = np_lbl;
    out->icon  = d->wireless ? ICON_WIFI_3 : ICON_NET_WIRED;
    out->dim   = d->admin_down ? 1 : 0;
    return 0;
}
static int np_columns(void* c) { (void)c; return NC__COUNT; }
static const char* np_col_title(void* c, int col) {
    (void)c;
    switch (col) {
    case NC_NAME:  return "net.col.adapter";
    case NC_TYPE:  return "net.col.type";
    case NC_STATE: return "net.col.state";
    case NC_ADDR:  return "net.col.address";
    case NC_SRC:   return "net.col.source";
    case NC_MAC:   return "net.col.mac";
    }
    return "";
}
static int np_col_weight(void* c, int col) {
    (void)c;
    return (col == NC_ADDR || col == NC_STATE) ? 2 : 1;
}
static int np_col_style(void* c, int col) {
    (void)c;
    return (col == NC_ADDR || col == NC_MAC) ? ICOL_MONO : 0;
}
static int np_cell(void* c, int i, int col, char* out, int cap) {
    (void)c;
    if (i < 0 || i >= np_n || cap <= 0) return -1;
    struct net_device* d = np_dev[i];
    out[0] = 0;
    switch (col) {
    case NC_NAME:  put(out, cap, 0, d->name); break;
    /* THE CELLS ARE TRANSLATED HERE, not by the view — §M69's rule: the view
     * draws user data (file names, device names) and must not translate it;
     * only a model that KNOWS its cells are interface words may. */
    case NC_TYPE:  put(out, cap, 0, lstr(dev_type(d))); break;
    case NC_STATE: put(out, cap, 0, lstr(dev_state(d))); break;
    case NC_ADDR:
        if (d->ip) {
            int n = put_ip(out, cap, 0, d->ip);
            /* The prefix length rather than the dotted mask: shorter, and the
             * form every other tool prints. */
            int bits = 0;
            for (uint32_t m = d->netmask; m & 0x80000000u; m <<= 1) bits++;
            n = put(out, cap, n, "/");
            put_int(out, cap, n, bits);
        } else {
            put(out, cap, 0, "-");
        }
        break;
    case NC_SRC:
        if (d->flags & NETDEV_F_LOOPBACK) put(out, cap, 0, "-");
        else put(out, cap, 0, lstr(d->config_src == NETCFG_DHCP ? "net.src.dhcp"
                                 : d->config_src == NETCFG_STATIC ? "net.src.static"
                                 : "net.src.default"));
        break;
    case NC_MAC: {
        char b[18];
        net_fmt_mac(d->mac, b);
        put(out, cap, 0, b);
        break;
    }
    default: return -1;
    }
    return 0;
}

static const struct item_model np_model = {
    .count = np_count, .get = np_get,
    .columns = np_columns, .col_title = np_col_title,
    .col_weight = np_col_weight, .cell = np_cell, .col_style = np_col_style,
    .empty_text = "net.empty",
};

/* The status line at the top: the SAME sentence the tray's flyout shows. */
static char np_status_buf[96];
static void np_refresh_status(void) {
    if (!np_status) return;
    struct net_state st;
    net_get_state(&st);
    int n = put(np_status_buf, sizeof np_status_buf, 0,
                lstr(net_state_name(st.state)));
    if (st.dev) {
        n = put(np_status_buf, sizeof np_status_buf, n, " - ");
        n = put(np_status_buf, sizeof np_status_buf, n, st.dev->name);
        if (st.dev->ip) {
            n = put(np_status_buf, sizeof np_status_buf, n, " ");
            n = put_ip(np_status_buf, sizeof np_status_buf, n, st.dev->ip);
        }
    }
    (void)n;
    w_label_set(np_status, np_status_buf);
}

static char np_detail_buf[160];
static void np_refresh_detail(void) {
    if (!np_detail) return;
    struct net_device* d = np_selected();
    if (!d) { w_label_set(np_detail, "net.hint.select"); return; }
    int n = put(np_detail_buf, sizeof np_detail_buf, 0, d->name);
    n = put(np_detail_buf, sizeof np_detail_buf, n, " - ");
    n = put(np_detail_buf, sizeof np_detail_buf, n, lstr("net.d.gateway"));
    n = put(np_detail_buf, sizeof np_detail_buf, n, " ");
    n = d->gateway ? put_ip(np_detail_buf, sizeof np_detail_buf, n, d->gateway)
                   : put(np_detail_buf, sizeof np_detail_buf, n, "-");
    n = put(np_detail_buf, sizeof np_detail_buf, n, ", DNS ");
    n = put_ip(np_detail_buf, sizeof np_detail_buf, n, net_get_dns());
    n = put(np_detail_buf, sizeof np_detail_buf, n, ", RX ");
    n = put_int(np_detail_buf, sizeof np_detail_buf, n, (int)d->rx_packets);
    n = put(np_detail_buf, sizeof np_detail_buf, n, " / TX ");
    n = put_int(np_detail_buf, sizeof np_detail_buf, n, (int)d->tx_packets);
    struct net_device* ld;
    uint32_t left, total;
    if (dhcp_lease(&ld, NULL, &left, &total) == 0 && ld == d) {
        n = put(np_detail_buf, sizeof np_detail_buf, n, ", ");
        n = put(np_detail_buf, sizeof np_detail_buf, n, lstr("net.d.lease"));
        n = put(np_detail_buf, sizeof np_detail_buf, n, " ");
        n = put_int(np_detail_buf, sizeof np_detail_buf, n, (int)left);
        n = put(np_detail_buf, sizeof np_detail_buf, n, " s");
    }
    (void)n;
    w_label_set(np_detail, np_detail_buf);
}

static void np_on_select(struct w_itemview* iv, int idx, void* ctx) {
    (void)iv; (void)ctx;
    np_sel = idx;
    if (idx >= 0 && idx < np_n) scopy(np_sel_name, np_dev[idx]->name, sizeof np_sel_name);
    np_refresh_detail();
    if (np_win) gui_window_request_redraw(np_win);
}

static void np_say(const char* key) { if (np_result) w_label_set(np_result, key); }

static void addr_open(void);
static void wifi_open(void);

enum {
    NP_ID_STATUS = 1, NP_ID_VIEW, NP_ID_DETAIL, NP_ID_RESULT, NP_ID_ROW,
    NP_ID_ENABLE, NP_ID_DISABLE, NP_ID_RENEW, NP_ID_CONFIG, NP_ID_TEST,
    NP_ID_SPACER, NP_ID_WIFI,
};

static void np_event(struct gui_window* win, int id, int type, int value, void* ctx) {
    (void)win; (void)value; (void)ctx;
    if (type != UI_EV_CLICK) return;
    struct net_device* d = np_selected();
    switch (id) {
    case NP_ID_ENABLE:
    case NP_ID_DISABLE:
        if (!d) { np_say("net.hint.select"); break; }
        if (net_set_admin(d, id == NP_ID_ENABLE) != 0) np_say("net.msg.cannot");
        else np_say(id == NP_ID_ENABLE ? "net.msg.enabled" : "net.msg.disabled");
        break;
    case NP_ID_RENEW:
        if (!d) { np_say("net.hint.select"); break; }
        if (d->flags & (NETDEV_F_LOOPBACK | NETDEV_F_SIMULATED)) {
            np_say("net.msg.nodhcphere");
            break;
        }
        scopy(g_job_dev, d->name, sizeof g_job_dev);
        np_say(start_job("dhcp-renew", job_dhcp) == 0 ? "net.msg.asking" : "net.msg.busy");
        break;
    case NP_ID_CONFIG: addr_open(); break;
    case NP_ID_TEST:
        scopy(g_job_dev, d ? d->name : "", sizeof g_job_dev);
        np_say(start_job("net-test", job_test) == 0 ? "net.msg.testing" : "net.msg.busy");
        break;
    case NP_ID_WIFI:
        if (!net_first_wireless()) { np_say("net.msg.nowifi"); break; }
        wifi_open();
        break;
    }
    if (np_win) gui_window_request_redraw(np_win);
}

static int np_ticks;
static void np_tick(struct gui_window* win) {
    /* Live, like the device manager: an adapter can lose its cable, be
     * disabled from a prompt or appear (`wifisim on`) while this is open. */
    if (__atomic_load_n(&g_result_ready, __ATOMIC_ACQUIRE)) {
        g_result_ready = 0;
        np_say(g_result);
    } else if (++np_ticks & 1) {
        return;
    }
    np_rescan();
    np_refresh_status();
    np_refresh_detail();
    if (np_view) w_itemview_refresh(np_view);
    gui_window_request_redraw(win);
}

static void np_on_close(struct gui_window* w) {
    (void)w;
    np_view = NULL; np_status = np_detail = np_result = NULL;
}

static void np_layout(struct gui_window* win) {
    np_view = NULL;
    np_rescan();
    static const struct ui_spec spec[] = {
        { .id = NP_ID_STATUS, .cls = "label", .text = "", .flags = UI_FILL_W },
        { .id = NP_ID_VIEW,   .cls = "view", .text = "table", .weight = 1,
          .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = NP_ID_DETAIL, .cls = "label", .text = "net.hint.select", .flags = UI_FILL_W },
        { .id = NP_ID_RESULT, .cls = "label", .text = "", .flags = UI_FILL_W },
        { .id = NP_ID_ROW,    .cls = "box",   .flags = UI_ROW | UI_FILL_W },
        { .id = NP_ID_ENABLE, .parent = NP_ID_ROW, .cls = "button", .text = "Enable" },
        { .id = NP_ID_DISABLE,.parent = NP_ID_ROW, .cls = "button", .text = "Disable" },
        { .id = NP_ID_RENEW,  .parent = NP_ID_ROW, .cls = "button", .text = "net.btn.renew" },
        { .id = NP_ID_CONFIG, .parent = NP_ID_ROW, .cls = "button", .text = "net.btn.config" },
        { .id = NP_ID_TEST,   .parent = NP_ID_ROW, .cls = "button", .text = "net.btn.test" },
        { .id = NP_ID_SPACER, .parent = NP_ID_ROW, .cls = "box", .weight = 1 },
        { .id = NP_ID_WIFI,   .parent = NP_ID_ROW, .cls = "button", .text = "Wi-Fi" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), np_event, NULL);
    np_view   = (struct w_itemview*)ui_by_id(win, NP_ID_VIEW);
    np_status = (struct w_label*)ui_by_id(win, NP_ID_STATUS);
    np_detail = (struct w_label*)ui_by_id(win, NP_ID_DETAIL);
    np_result = (struct w_label*)ui_by_id(win, NP_ID_RESULT);
    if (np_view) {
        w_itemview_set_model(&np_view->base, &np_model, NULL);
        np_view->on_select = np_on_select;
    }
    np_refresh_status();
    np_refresh_detail();
}

static void network_panel_open(void) {
    struct gui_app_spec sp = {
        .title = "Network",
        .content_w = cp_px(800), .content_h = cp_px(340),
        .layout = np_layout, .tick = np_tick,
        .on_close = np_on_close, .slot = &np_win,
    };
    gui_app_open(&sp);
}

void netui_open_panel(void) { gui_queue_open(network_panel_open); }

SETTINGS_PANEL(sp_network) = {
    .name    = "Network",
    .summary = "adapters, addresses, Wi-Fi",
    .icon    = ICON_NET_WIRED,
    .open    = network_panel_open,
};

/* ================================================================ */
/* 2. The address dialog.                                            */
/* ================================================================ */

static struct gui_window* ad_win;
static int ad_mode;             /* 0 built-in default, 1 DHCP, 2 static */

enum {
    AD_ID_MODE = 1, AD_ID_GRID, AD_ID_L_IP, AD_ID_IP, AD_ID_L_MASK, AD_ID_MASK,
    AD_ID_L_GW, AD_ID_GW, AD_ID_L_DNS, AD_ID_DNS, AD_ID_MSG, AD_ID_ROW,
    AD_ID_SAVE, AD_ID_CANCEL,
};

static const char* ad_text(struct gui_window* w, int id) {
    struct w_textinput* t = (struct w_textinput*)ui_by_id(w, id);
    return t ? t->buf : "";
}
static void ad_msg(struct gui_window* w, const char* key) {
    struct w_label* l = (struct w_label*)ui_by_id(w, AD_ID_MSG);
    if (l) w_label_set(l, key);
}

/* SAVE applies and persists — the order §M69 fixed for every settings panel:
 * `config_apply` is what tells the subsystem (net_state.c's watcher sets the
 * address), `config_save` is what makes it survive.  Validation comes FIRST
 * and refuses the whole form: a half-applied address (new IP, old gateway) is
 * a machine that is neither the old configuration nor the new one. */
static void ad_save(struct gui_window* w) {
    if (ad_mode == 2) {
        uint32_t v;
        const char* ip = ad_text(w, AD_ID_IP);
        const char* mk = ad_text(w, AD_ID_MASK);
        const char* gw = ad_text(w, AD_ID_GW);
        const char* dn = ad_text(w, AD_ID_DNS);
        if (net_parse_ip(ip, &v) != 0 || !v)      { ad_msg(w, "net.err.ip");   return; }
        if (net_parse_ip(mk, &v) != 0 || !v)      { ad_msg(w, "net.err.mask"); return; }
        if (gw[0] && net_parse_ip(gw, &v) != 0)   { ad_msg(w, "net.err.gw");   return; }
        if (dn[0] && net_parse_ip(dn, &v) != 0)   { ad_msg(w, "net.err.dns");  return; }
        /* The four values first, the switch last — so the watcher that fires
         * on `net.static` finds a complete address already in place. */
        config_apply("net.static.ip", ip);
        config_apply("net.static.netmask", mk);
        config_apply("net.static.gateway", gw);
        config_apply("net.static.dns", dn);
        config_apply("net.dhcp", "0");
        config_apply("net.static", "1");    /* the watcher applies it (once) */
    } else {
        config_apply("net.static", "0");
        config_apply("net.dhcp", ad_mode == 1 ? "1" : "0");
        if (ad_mode == 1) {
            struct net_device* d = net_primary();
            if (d) {
                scopy(g_job_dev, d->name, sizeof g_job_dev);
                start_job("dhcp-renew", job_dhcp);
            }
        }
    }
    int persisted = config_save() == 0 && config_persist_path();
    kprintf("net: address mode %s saved%s\n",
            ad_mode == 2 ? "static" : ad_mode == 1 ? "dhcp" : "default",
            persisted ? "" : " (RAM only - no writable volume)");
    /* The built-in default cannot be "applied" to a running adapter: it is the
     * driver's own number, set at bring-up.  Say so rather than pretending. */
    if (ad_mode == 0)
        ad_msg(w, "net.msg.defaultreboot");
    else
        gui_window_close(w);
}

static void ad_event(struct gui_window* w, int id, int type, int value, void* ctx) {
    (void)ctx;
    if (id == AD_ID_MODE && type == UI_EV_CHANGE) { ad_mode = value; return; }
    if (type != UI_EV_CLICK) return;
    if (id == AD_ID_SAVE)   ad_save(w);
    if (id == AD_ID_CANCEL) gui_window_close(w);
}

static void ad_layout(struct gui_window* win) {
    static const struct ui_spec spec[] = {
        { .id = AD_ID_MODE, .cls = "radio",
          .text = "net.mode.default net.mode.dhcp net.mode.static" },
        { .id = AD_ID_GRID, .cls = "box", .flags = UI_GRID | UI_FILL_W },
        { .id = AD_ID_L_IP,   .parent = AD_ID_GRID, .cls = "label", .text = "net.f.ip" },
        { .id = AD_ID_IP,     .parent = AD_ID_GRID, .cls = "textinput", .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = AD_ID_L_MASK, .parent = AD_ID_GRID, .cls = "label", .text = "net.f.mask" },
        { .id = AD_ID_MASK,   .parent = AD_ID_GRID, .cls = "textinput", .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = AD_ID_L_GW,   .parent = AD_ID_GRID, .cls = "label", .text = "net.f.gw" },
        { .id = AD_ID_GW,     .parent = AD_ID_GRID, .cls = "textinput", .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = AD_ID_L_DNS,  .parent = AD_ID_GRID, .cls = "label", .text = "net.f.dns" },
        { .id = AD_ID_DNS,    .parent = AD_ID_GRID, .cls = "textinput", .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = AD_ID_MSG,  .cls = "label", .text = "net.hint.static", .flags = UI_FILL_W },
        { .id = AD_ID_ROW,  .cls = "box", .flags = UI_ROW | UI_FILL_W | UI_ALIGN_END },
        { .id = AD_ID_CANCEL, .parent = AD_ID_ROW, .cls = "button", .text = "Cancel" },
        { .id = AD_ID_SAVE,   .parent = AD_ID_ROW, .cls = "button", .text = "Save" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), ad_event, NULL);

    /* Prefill from what the machine is doing NOW, not from the keys: the keys
     * may be empty on a machine that has never been configured, and a form
     * that opens blank invites typing an address the machine already has. */
    ad_mode = config_get_long("net.static", 0) ? 2
            : config_get_long("net.dhcp", 0) ? 1 : 0;
    struct widget* r = ui_by_id(win, AD_ID_MODE);
    const struct widget_class* rc = ui_class_find("radio");
    if (r && rc && rc->set_value) rc->set_value(r, ad_mode);
    struct net_device* d = net_primary();
    char b[16];
    struct w_textinput* t;
    if ((t = (struct w_textinput*)ui_by_id(win, AD_ID_IP)) && d)   { net_fmt_ip(d->ip, b); w_textinput_set(t, b); }
    if ((t = (struct w_textinput*)ui_by_id(win, AD_ID_MASK)) && d) { net_fmt_ip(d->netmask, b); w_textinput_set(t, b); }
    if ((t = (struct w_textinput*)ui_by_id(win, AD_ID_GW)) && d)   { net_fmt_ip(d->gateway, b); w_textinput_set(t, b); }
    if ((t = (struct w_textinput*)ui_by_id(win, AD_ID_DNS)))       { net_fmt_ip(net_get_dns(), b); w_textinput_set(t, b); }
}

static void addr_open_now(void) {
    gui_app_open(&(struct gui_app_spec){
        .title = "net.title.address",
        /* Wide enough that the label/field grid stays two columns: stacked,
         * the four fields plus the mode radio ran the action row off the
         * bottom of the dialog (seen in the first screenshot). */
        .content_w = cp_px(520), .content_h = cp_px(290),
        .place = GUI_PLACE_DIALOG, .modal = 1,
        .layout = ad_layout, .slot = &ad_win,
    });
}
static void addr_open(void) { gui_queue_open(addr_open_now); }

/* ================================================================ */
/* 3. The Wi-Fi chooser.                                             */
/* ================================================================ */

#define WF_MAX 16
static struct wifi_network wf_nets[WF_MAX];
static int wf_n;
static int wf_sel = -1;
static char wf_pre[WIFI_SSID_MAX];      /* preselected by the tray */
static struct gui_window* wf_win;
static struct w_itemview* wf_view;
static struct w_label*    wf_status;
static struct w_label*    wf_result;

enum { WC_SSID = 0, WC_SIGNAL, WC_SEC, WC_JOINED, WC__COUNT };

/* Scanning runs on the WINDOW's host task, not the compositor: a real radio
 * takes seconds to sweep its channels and a window may wait for that; the
 * chrome may not. */
static void wf_scan(void) {
    struct net_device* d = net_first_wireless();
    wf_n = 0;
    if (d) {
        int k = d->wireless->scan(d, wf_nets, WF_MAX);
        wf_n = k > 0 ? k : 0;
    }
    wf_sel = -1;
    for (int i = 0; i < wf_n; i++)
        if (wf_pre[0] && seq(wf_nets[i].ssid, wf_pre)) wf_sel = i;
    if (wf_view) { wf_view->sel = wf_sel; w_itemview_refresh(wf_view); }
}

static int wf_joined(int i) {
    struct net_device* d = net_first_wireless();
    struct wifi_status ws;
    if (!d || d->wireless->status(d, &ws) != 0 || !ws.associated) return 0;
    return seq(ws.ssid, wf_nets[i].ssid);
}

static int wf_count(void* c) { (void)c; return wf_n; }
static int wf_get(void* c, int i, struct item_entry* out) {
    (void)c;
    if (i < 0 || i >= wf_n) return -1;
    out->label = wf_nets[i].ssid;
    int s = wf_nets[i].signal;
    out->icon = s >= 66 ? ICON_WIFI_3 : s >= 33 ? ICON_WIFI_2 : ICON_WIFI_1;
    return 0;
}
static int wf_columns(void* c) { (void)c; return WC__COUNT; }
static const char* wf_col_title(void* c, int col) {
    (void)c;
    switch (col) {
    case WC_SSID:   return "wifi.col.network";
    case WC_SIGNAL: return "wifi.col.signal";
    case WC_SEC:    return "wifi.col.security";
    case WC_JOINED: return "wifi.col.status";
    }
    return "";
}
static int wf_col_weight(void* c, int col) { (void)c; return col == WC_SSID ? 3 : 1; }
static int wf_col_style(void* c, int col) { (void)c; return col == WC_SIGNAL ? ICOL_RIGHT | ICOL_MONO : 0; }
static int wf_cell(void* c, int i, int col, char* out, int cap) {
    (void)c;
    if (i < 0 || i >= wf_n || cap <= 0) return -1;
    out[0] = 0;
    switch (col) {
    case WC_SSID:   put(out, cap, 0, wf_nets[i].ssid); break;
    case WC_SIGNAL: { int n = put_int(out, cap, 0, wf_nets[i].signal); put(out, cap, n, "%"); break; }
    case WC_SEC:    put(out, cap, 0, lstr(wf_nets[i].security == WIFI_SEC_OPEN
                                          ? "wifi.sec.open" : "WPA2")); break;
    case WC_JOINED: if (wf_joined(i)) put(out, cap, 0, lstr("wifi.connected")); break;
    default: return -1;
    }
    return 0;
}
static const struct item_model wf_model = {
    .count = wf_count, .get = wf_get,
    .columns = wf_columns, .col_title = wf_col_title,
    .col_weight = wf_col_weight, .cell = wf_cell, .col_style = wf_col_style,
    .empty_text = "wifi.empty",
};

static void wf_on_select(struct w_itemview* iv, int idx, void* ctx) {
    (void)iv; (void)ctx;
    wf_sel = idx;
    if (idx >= 0 && idx < wf_n) scopy(wf_pre, wf_nets[idx].ssid, sizeof wf_pre);
    if (wf_win) gui_window_request_redraw(wf_win);
}

enum {
    WF_ID_STATUS = 1, WF_ID_VIEW, WF_ID_GRID, WF_ID_L_PASS, WF_ID_PASS,
    WF_ID_RESULT, WF_ID_ROW, WF_ID_REFRESH, WF_ID_SPACER, WF_ID_LEAVE, WF_ID_JOIN,
};

static char wf_status_buf[96];
static void wf_refresh_status(void) {
    if (!wf_status) return;
    struct net_device* d = net_first_wireless();
    struct wifi_status ws;
    int n;
    if (!d) { w_label_set(wf_status, "net.msg.nowifi"); return; }
    n = put(wf_status_buf, sizeof wf_status_buf, 0, d->name);
    n = put(wf_status_buf, sizeof wf_status_buf, n, ": ");
    if (d->wireless->status(d, &ws) == 0 && ws.associated) {
        n = put(wf_status_buf, sizeof wf_status_buf, n, lstr("wifi.connected"));
        n = put(wf_status_buf, sizeof wf_status_buf, n, " - ");
        n = put(wf_status_buf, sizeof wf_status_buf, n, ws.ssid);
    } else {
        n = put(wf_status_buf, sizeof wf_status_buf, n, lstr("net.st.notjoined"));
    }
    if (d->flags & NETDEV_F_SIMULATED) {
        n = put(wf_status_buf, sizeof wf_status_buf, n, "  ");
        n = put(wf_status_buf, sizeof wf_status_buf, n, lstr("wifi.simulated"));
    }
    (void)n;
    w_label_set(wf_status, wf_status_buf);
}

static void wf_event(struct gui_window* w, int id, int type, int value, void* ctx) {
    (void)value; (void)ctx;
    if (type == UI_EV_SUBMIT && id == WF_ID_PASS) id = WF_ID_JOIN, type = UI_EV_CLICK;
    if (type != UI_EV_CLICK) return;
    struct w_label* res = (struct w_label*)ui_by_id(w, WF_ID_RESULT);
    if (id == WF_ID_REFRESH) { wf_scan(); if (res) w_label_set(res, "wifi.scanned"); }
    if (id == WF_ID_LEAVE) {
        if (res) w_label_set(res, netui_wifi_leave_async() == 0 ? "net.msg.leaving" : "net.msg.busy");
    }
    if (id == WF_ID_JOIN) {
        if (wf_sel < 0 || wf_sel >= wf_n) { if (res) w_label_set(res, "wifi.pick"); return; }
        struct w_textinput* p = (struct w_textinput*)ui_by_id(w, WF_ID_PASS);
        if (wf_nets[wf_sel].security != WIFI_SEC_OPEN && (!p || p->len == 0)) {
            if (res) w_label_set(res, "wifi.needpass");
            return;
        }
        if (!job_claim()) { if (res) w_label_set(res, "net.msg.busy"); return; }
        scopy(g_job_ssid, wf_nets[wf_sel].ssid, sizeof g_job_ssid);
        scopy(g_job_pass, p ? p->buf : "", sizeof g_job_pass);
        if (p) w_textinput_set(p, "");         /* not left on screen or in RAM */
        g_result_ready = 0;
        if (!task_spawn_detached("wifi-join", job_join)) job_done("net.msg.notask");
        if (res) w_label_set(res, "net.msg.joining");
    }
    gui_window_request_redraw(w);
}

static void wf_tick(struct gui_window* win) {
    if (__atomic_load_n(&g_result_ready, __ATOMIC_ACQUIRE)) {
        g_result_ready = 0;
        struct w_label* res = (struct w_label*)ui_by_id(win, WF_ID_RESULT);
        if (res) w_label_set(res, g_result);
    }
    wf_refresh_status();
    if (wf_view) w_itemview_refresh(wf_view);
    gui_window_request_redraw(win);
}

static void wf_on_close(struct gui_window* w) {
    (void)w;
    wf_view = NULL; wf_status = wf_result = NULL;
}

static void wf_layout(struct gui_window* win) {
    static const struct ui_spec spec[] = {
        { .id = WF_ID_STATUS, .cls = "label", .text = "", .flags = UI_FILL_W },
        { .id = WF_ID_VIEW,   .cls = "view", .text = "table", .weight = 1,
          .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = WF_ID_GRID,   .cls = "box", .flags = UI_GRID | UI_FILL_W },
        { .id = WF_ID_L_PASS, .parent = WF_ID_GRID, .cls = "label", .text = "wifi.passphrase" },
        { .id = WF_ID_PASS,   .parent = WF_ID_GRID, .cls = "textinput",
          .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = WF_ID_RESULT, .cls = "label", .text = "", .flags = UI_FILL_W },
        { .id = WF_ID_ROW,    .cls = "box", .flags = UI_ROW | UI_FILL_W },
        { .id = WF_ID_REFRESH,.parent = WF_ID_ROW, .cls = "button", .text = "wifi.btn.scan" },
        { .id = WF_ID_SPACER, .parent = WF_ID_ROW, .cls = "box", .weight = 1 },
        { .id = WF_ID_LEAVE,  .parent = WF_ID_ROW, .cls = "button", .text = "wifi.btn.disconnect" },
        { .id = WF_ID_JOIN,   .parent = WF_ID_ROW, .cls = "button", .text = "wifi.btn.connect" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), wf_event, NULL);
    wf_view   = (struct w_itemview*)ui_by_id(win, WF_ID_VIEW);
    wf_status = (struct w_label*)ui_by_id(win, WF_ID_STATUS);
    wf_result = (struct w_label*)ui_by_id(win, WF_ID_RESULT);
    struct w_textinput* p = (struct w_textinput*)ui_by_id(win, WF_ID_PASS);
    if (p) w_textinput_set_secret(p, 1);
    if (wf_view) {
        w_itemview_set_model(&wf_view->base, &wf_model, NULL);
        wf_view->on_select = wf_on_select;
    }
    wf_scan();
    wf_refresh_status();
    if (p && wf_sel >= 0) gui_window_focus_widget(win, &p->base);
}

static void wifi_open_now(void) {
    gui_app_open(&(struct gui_app_spec){
        .title = "Wi-Fi",
        .content_w = cp_px(460), .content_h = cp_px(320),
        .layout = wf_layout, .tick = wf_tick,
        .on_close = wf_on_close, .slot = &wf_win,
    });
}
static void wifi_open(void) { gui_queue_open(wifi_open_now); }

void netui_open_wifi(const char* ssid) {
    scopy(wf_pre, ssid ? ssid : "", sizeof wf_pre);
    wifi_open();
}
