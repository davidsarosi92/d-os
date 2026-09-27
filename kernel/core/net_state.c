/* =============================================================================
 * net_state.c — the network as a PERSON sees it (§M87).
 *
 * The stack below this file answers questions about frames, sockets and
 * routes.  Three new consumers asked a different kind of question — the
 * taskbar icon, the Control Panel's Network page and the `netstate` command:
 *
 *     "Am I online?  Through what?  If not, why not?"
 *
 * That answer has SIX outcomes, not two, and each one points at a different
 * fix (NETSTATE_* in net.h):
 *
 *     no adapter       — nothing to configure; a driver or a device is missing
 *     disabled         — somebody turned it off; turn it back on
 *     no link          — the cable is out / not associated with a network
 *     no address       — the link is up and nothing gave us an IPv4 address
 *     wired / wireless — online
 *
 * A tray icon with two states ("connected", "not") collapses the four failure
 * cases into one picture, and the person then goes looking for the wrong
 * problem — §M23's argument for three sound icons, one layer over.
 *
 * ONE FUNCTION DECIDES (`net_get_state`) and all three consumers call it.
 * The alternative — each consumer inspecting net_device on its own — is how
 * the icon ends up green while the panel says "no address".
 *
 * -----------------------------------------------------------------------------
 * CARRIER IS REPORTED, NOT ASSUMED
 *
 * A device whose driver cannot tell link state (`dev->link == NULL`) is
 * reported with `link_known = 0`.  The state machine still treats it as up —
 * refusing to would declare every such machine offline — but the panel says
 * "link: unknown" rather than "up", so what the machine does not know is not
 * dressed up as something it measured.
 *
 * -----------------------------------------------------------------------------
 * STATIC ADDRESSES ARE CONFIG, SO THEY SURVIVE A REBOOT
 *
 * `net.static` + `net.static.{ip,netmask,gateway,dns}` are §M63 keys applied
 * by a boot service and by a CONFIG_WATCH — the same pair every other setting
 * in this system uses, so the panel's Save and a `conf set` typed at a prompt
 * take exactly the same route.
 * ============================================================================= */

#include "net.h"
#include "dhcp.h"
#include "config.h"
#include "settings.h"
#include "shellcmd.h"
#include "cmd_util.h"
#include "service.h"
#include "task.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* ----------------------- the one answer ----------------------------------- */

int net_link_state(struct net_device* dev) {
    if (!dev) return 0;
    if (dev->wireless && dev->wireless->status) {
        struct wifi_status ws;
        if (dev->wireless->status(dev, &ws) != 0) return -1;
        return ws.associated ? 1 : 0;
    }
    if (!dev->link) return -1;
    return dev->link(dev) ? 1 : 0;
}

/* Rank a device's state: higher = more online.  The machine's state is the
 * BEST adapter's, because one working adapter is enough to be online, and the
 * icon should describe the path traffic actually takes. */
static int dev_state(struct net_device* d, int* link_known, int* signal) {
    *link_known = 1;
    *signal = -1;
    if (d->admin_down) return NETSTATE_DISABLED;
    int l = net_link_state(d);
    if (l == 0) return NETSTATE_NO_LINK;
    if (l < 0) *link_known = 0;
    if (d->wireless && d->wireless->status) {
        struct wifi_status ws;
        if (d->wireless->status(d, &ws) == 0) *signal = ws.signal;
    }
    if (!d->ip) return NETSTATE_NO_ADDRESS;
    return d->wireless ? NETSTATE_WIRELESS : NETSTATE_WIRED;
}

struct gs_ctx { struct net_state* out; };

static void gs_one(struct net_device* d, void* vctx) {
    struct gs_ctx* c = (struct gs_ctx*)vctx;
    if (d->flags & NETDEV_F_LOOPBACK) return;
    int known, sig;
    int st = dev_state(d, &known, &sig);
    /* WIRED beats WIRELESS when both are up: that is also what net_primary
     * routes through (registration order puts the NIC first), and an icon
     * showing Wi-Fi bars while traffic leaves through the cable describes a
     * path nothing uses. */
    int rank = (st == NETSTATE_WIRED) ? 10 : st;
    int cur  = !c->out->dev ? -1
             : (c->out->state == NETSTATE_WIRED ? 10 : c->out->state);
    if (rank > cur) {
        c->out->state      = st;
        c->out->dev        = d;
        c->out->signal     = sig;
        c->out->link_known = known;
    }
}

void net_get_state(struct net_state* out) {
    out->state = NETSTATE_NONE;
    out->dev = NULL;
    out->signal = -1;
    out->link_known = 1;
    struct gs_ctx c = { out };
    net_for_each(gs_one, &c);
}

const char* net_state_name(int s) {
    switch (s) {
    case NETSTATE_NONE:       return "no network adapter";
    case NETSTATE_DISABLED:   return "disabled";
    case NETSTATE_NO_LINK:    return "not connected";
    case NETSTATE_NO_ADDRESS: return "no address";
    case NETSTATE_WIRED:      return "connected (wired)";
    case NETSTATE_WIRELESS:   return "connected (Wi-Fi)";
    }
    return "?";
}

const char* net_config_src_name(uint32_t s) {
    switch (s) {
    case NETCFG_DHCP:   return "DHCP";
    case NETCFG_STATIC: return "static";
    default:            return "built-in default";
    }
}

/* ----------------------- actions ------------------------------------------ */

int net_set_admin(struct net_device* dev, int up) {
    if (!dev || (dev->flags & NETDEV_F_LOOPBACK)) return -1;
    dev->admin_down = up ? 0 : 1;
    kprintf("net: %s %s\n", dev->name, up ? "enabled" : "disabled by the user");
    return 0;
}

int net_set_static(struct net_device* dev, uint32_t ip, uint32_t mask,
                   uint32_t gw, uint32_t dns) {
    if (!dev || !ip || !mask) return -1;
    /* The address changes under the stack lock, so a frame being assembled on
     * another CPU sees either the old address or the new one, never half. */
    uint32_t f = net_lock();
    dev->ip = ip;
    dev->netmask = mask;
    dev->gateway = gw;
    dev->config_src = NETCFG_STATIC;
    net_unlock(f);
    if (dns) net_set_dns(dns);
    dhcp_release_local();
    char a[16], m[16], g[16];
    net_fmt_ip(ip, a); net_fmt_ip(mask, m); net_fmt_ip(gw, g);
    kprintf("net: %s static %s/%s gw %s\n", dev->name, a, m, g);
    return 0;
}

/* ----------------------- persistent static config ------------------------- */

CONFIG_KEY(ck_net_dhcp) = {
    .key = "net.dhcp", .group = "Network", .type = CFG_BOOL, .def = "0",
    .help = "ask a DHCP server for an address at boot",
};
CONFIG_KEY(ck_net_static) = {
    .key = "net.static", .group = "Network", .type = CFG_BOOL, .def = "0",
    .help = "use the static address below instead of the default or DHCP",
};
CONFIG_KEY(ck_net_sip) = {
    .key = "net.static.ip", .group = "Network", .type = CFG_STRING, .def = "",
    .help = "static IPv4 address, e.g. 192.168.1.20",
};
CONFIG_KEY(ck_net_smask) = {
    .key = "net.static.netmask", .group = "Network", .type = CFG_STRING,
    .def = "255.255.255.0", .help = "static subnet mask",
};
CONFIG_KEY(ck_net_sgw) = {
    .key = "net.static.gateway", .group = "Network", .type = CFG_STRING, .def = "",
    .help = "static default gateway",
};
CONFIG_KEY(ck_net_sdns) = {
    .key = "net.static.dns", .group = "Network", .type = CFG_STRING, .def = "",
    .help = "static DNS server",
};

/* Apply the static keys to the default adapter.  Returns 0 when applied, 1
 * when static configuration is off, <0 when the keys do not parse — and in
 * that case NOTHING is changed: half an address (a new IP with the old
 * gateway) is worse than the old one. */
static int apply_static(int loud) {
    if (!config_get_long("net.static", 0)) return 1;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
    if (net_parse_ip(config_get("net.static.ip", ""), &ip) != 0 || !ip) {
        if (loud) kprintf("net: net.static is on but net.static.ip is not an address\n");
        return -2;
    }
    if (net_parse_ip(config_get("net.static.netmask", "255.255.255.0"), &mask) != 0)
        return -3;
    const char* g = config_get("net.static.gateway", "");
    if (g[0] && net_parse_ip(g, &gw) != 0) return -4;
    const char* d = config_get("net.static.dns", "");
    if (d[0] && net_parse_ip(d, &dns) != 0) return -5;
    return net_set_static(dev, ip, mask, gw, dns);
}
int net_apply_static_config(void) { return apply_static(1); }

static void net_conf_changed(const char* key, const char* value) {
    (void)key; (void)value;
    /* Every net.static.* key comes through here; applying after each one of a
     * panel's four writes is harmless (the last one wins, and each step is a
     * complete, parsed address or nothing).  QUIET: when the persistent store
     * is overlaid at boot the keys arrive one at a time and `net.static = 1`
     * can come before the address it switches on — a complaint per key would
     * report a half-loaded store as a broken configuration. */
    apply_static(0);
}
CONFIG_WATCH(net_static_watch) = { .prefix = "net.static", .changed = net_conf_changed };

static void netcfg_service_entry(void) {
    /* The NIC registers from its driver's init; give it the same grace the
     * DHCP service does before deciding there is nothing to configure. */
    task_msleep(200);
    net_apply_static_config();
    task_exit();
}
SERVICE("netcfg", netcfg_service_entry, 1, SVC_RESTART_NO);

/* ----------------------- commands ----------------------------------------- */

/* Split `args` into up to `max` space-separated words, in place. */
static int split(char* s, char** w, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        w[n++] = s;
        while (*s && *s != ' ') s++;
    }
    return n;
}

static void print_dev(struct net_device* d) {
    char ip[16], mask[16], gw[16], mac[18];
    net_fmt_ip(d->ip, ip); net_fmt_ip(d->netmask, mask); net_fmt_ip(d->gateway, gw);
    net_fmt_mac(d->mac, mac);
    int l = net_link_state(d);
    kprintf("%s  %s%s  link %s  %s\n", d->name,
            (d->flags & NETDEV_F_LOOPBACK) ? "loopback"
                : d->wireless ? "wireless" : "wired",
            (d->flags & NETDEV_F_SIMULATED) ? " (simulated)" : "",
            l > 0 ? "up" : l == 0 ? "DOWN" : "unknown",
            d->admin_down ? "DISABLED" : "enabled");
    kprintf("     mac %s  ip %s/%s  gw %s  (%s)\n", mac, ip, mask, gw,
            net_config_src_name(d->config_src));
}

static void each_print(struct net_device* d, void* c) { (void)c; print_dev(d); }

static void cmd_netstate(const char* args) {
    (void)args;
    struct net_state st;
    net_get_state(&st);
    kprintf("network: %s", net_state_name(st.state));
    if (st.dev) kprintf(" via %s%s", st.dev->name,
                        (st.dev->flags & NETDEV_F_SIMULATED) ? " (SIMULATED - no traffic)" : "");
    if (st.signal >= 0) kprintf(", signal %d%%", st.signal);
    if (!st.link_known) kprintf(" (carrier not reported by the driver)");
    char dns[16];
    net_fmt_ip(net_get_dns(), dns);
    kprintf("\ndns %s\n", dns);
}

static void cmd_ifconfig(const char* args) {
    char buf[160];
    int i = 0;
    for (; args && args[i] && i < (int)sizeof buf - 1; i++) buf[i] = args[i];
    buf[i] = 0;
    char* w[8];
    int n = split(buf, w, 8);
    if (n == 0) { net_for_each(each_print, NULL); return; }
    struct net_device* d = net_find(w[0]);
    if (!d) { kprintf("ifconfig: no device '%s'\n", w[0]); return; }
    if (n == 1) { print_dev(d); return; }
    if (cmd_streq(w[1], "up") || cmd_streq(w[1], "down")) {
        if (net_set_admin(d, cmd_streq(w[1], "up")) != 0)
            kprintf("ifconfig: %s cannot be disabled\n", d->name);
        return;
    }
    if (cmd_streq(w[1], "dhcp")) {
        kprintf(dhcp_configure(d) == 0 ? "ifconfig: lease obtained\n"
                                       : "ifconfig: no DHCP answer\n");
        return;
    }
    if (cmd_streq(w[1], "static") && n >= 4) {
        uint32_t ip, mask, gw = 0, dns = 0;
        if (net_parse_ip(w[2], &ip) || net_parse_ip(w[3], &mask) ||
            (n > 4 && net_parse_ip(w[4], &gw)) || (n > 5 && net_parse_ip(w[5], &dns))) {
            kprintf("ifconfig: not an address\n");
            return;
        }
        net_set_static(d, ip, mask, gw, dns);
        return;
    }
    kprintf("usage: ifconfig [<dev> [up|down|dhcp|static <ip> <mask> [gw] [dns]]]\n");
}

/* `wifi` — the chooser, on a console.  Walks the first wireless adapter. */
struct wf_find { struct net_device* d; };
static void wf_one(struct net_device* d, void* c) {
    struct wf_find* f = (struct wf_find*)c;
    if (!f->d && d->wireless) f->d = d;
}
struct net_device* net_first_wireless(void) {
    struct wf_find f = { NULL };
    net_for_each(wf_one, &f);
    return f.d;
}

static void cmd_wifi(const char* args) {
    char buf[160];
    int i = 0;
    for (; args && args[i] && i < (int)sizeof buf - 1; i++) buf[i] = args[i];
    buf[i] = 0;
    char* w[4];
    int n = split(buf, w, 4);
    struct net_device* d = net_first_wireless();
    if (!d) { kprintf("wifi: no wireless adapter\n"); return; }
    const struct net_wireless_ops* o = d->wireless;
    if (n == 0 || cmd_streq(w[0], "status")) {
        struct wifi_status ws;
        if (o->status(d, &ws) != 0) { kprintf("wifi: radio not answering\n"); return; }
        if (ws.associated) kprintf("wifi: %s joined '%s', signal %d%%\n",
                                   d->name, ws.ssid, ws.signal);
        else               kprintf("wifi: %s not connected\n", d->name);
        return;
    }
    if (cmd_streq(w[0], "scan")) {
        struct wifi_network nets[16];
        int k = o->scan(d, nets, 16);
        if (k < 0) { kprintf("wifi: scan failed\n"); return; }
        kprintf("wifi: %d network(s)\n", k);
        for (int j = 0; j < k; j++)
            kprintf("  %s  %d%%  %s\n", nets[j].ssid, nets[j].signal,
                    nets[j].security == WIFI_SEC_OPEN ? "open" : "WPA2");
        return;
    }
    if (cmd_streq(w[0], "connect") && n >= 2) {
        int r = o->connect(d, w[1], n >= 3 ? w[2] : "");
        kprintf("wifi: %s\n", r == 0 ? "connected"
                            : r == -1 ? "no such network"
                            : r == -2 ? "wrong passphrase" : "radio failure");
        return;
    }
    if (cmd_streq(w[0], "disconnect")) {
        o->disconnect(d);
        kprintf("wifi: disconnected\n");
        return;
    }
    kprintf("usage: wifi [status|scan|connect <ssid> [pass]|disconnect]\n");
}

SHELL_CMD(netstate) = { "netstate", "", "online or not, and why (what the tray shows)",
                        SHELL_G_NET, cmd_netstate, SHELL_P_ANY };
SHELL_CMD(ifconfig) = { "ifconfig", "[<dev> [up|down|dhcp|static ...]]",
                        "adapters: state, address, enable/disable",
                        SHELL_G_NET, cmd_ifconfig, SHELL_P_ADMIN };
SHELL_CMD(wifi)     = { "wifi", "[status|scan|connect <ssid> [pass]|disconnect]",
                        "the wireless network chooser", SHELL_G_NET, cmd_wifi,
                        SHELL_P_ANY };
