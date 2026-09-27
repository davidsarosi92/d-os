/* =============================================================================
 * wifisim.c — a SIMULATED wireless adapter (§M87).
 *
 * WHY A SIMULATION, SAID FIRST: QEMU emulates no Wi-Fi hardware at all, and
 * neither do the other targets this project boots.  So the wireless half of
 * the network panel and the taskbar's Wi-Fi chooser would otherwise be code
 * that no run of this project could ever execute — §M59's reason for
 * declining `wl_data_device`: a surface with no client to falsify it against
 * "works" until the first real user.
 *
 * This adapter makes the INTERFACE testable: it scans (three fixed networks),
 * joins (an open one freely, a WPA2 one only with the right passphrase — so
 * the chooser's password prompt and its "wrong passphrase" answer are both
 * reachable), reports signal, and disconnects.  It carries NO TRAFFIC: its
 * transmit refuses every frame, and it is flagged NETDEV_F_SIMULATED so the
 * router never picks it (net.c) — enabling a test adapter must not take the
 * machine off the network.
 *
 * It is OFF unless asked for (`wifisim on`, or `net.wifisim = 1` at boot),
 * and every place that shows it says "simulated".  A real driver replaces it
 * by filling `struct net_wireless_ops` — nothing above that struct knows this
 * file exists.
 * ============================================================================= */

#include "net.h"
#include "config.h"
#include "settings.h"
#include "shellcmd.h"
#include "cmd_util.h"
#include "service.h"
#include "task.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

struct sim_net { const char* ssid; int signal; int sec; const char* pass; };

static const struct sim_net g_air[] = {
    { "d-os-lab",     78, WIFI_SEC_WPA2, "dos-wifi" },
    { "Kavezo",       45, WIFI_SEC_OPEN, NULL       },
    { "Szomszed-5G",  22, WIFI_SEC_WPA2, "secret12" },
};
#define G_AIR_N ((int)(sizeof g_air / sizeof g_air[0]))

static struct net_device g_wlan;
static int g_registered;
static int g_joined = -1;          /* index into g_air, -1 = not associated */

static void cpy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int eq(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int ws_scan(struct net_device* d, struct wifi_network* out, int max) {
    (void)d;
    int n = 0;
    for (int i = 0; i < G_AIR_N && n < max; i++, n++) {
        cpy(out[n].ssid, g_air[i].ssid, WIFI_SSID_MAX);
        out[n].signal   = g_air[i].signal;
        out[n].security = g_air[i].sec;
    }
    return n;
}

static int ws_connect(struct net_device* d, const char* ssid, const char* pass) {
    for (int i = 0; i < G_AIR_N; i++) {
        if (!eq(g_air[i].ssid, ssid)) continue;
        if (g_air[i].sec != WIFI_SEC_OPEN && !eq(g_air[i].pass, pass)) {
            kprintf("wifisim: '%s' refused the passphrase\n", ssid);
            return -2;
        }
        g_joined = i;
        /* An address "from the access point".  Marked as the built-in kind,
         * not DHCP: no DHCP exchange happened and the panel must not say one
         * did. */
        d->ip      = IPV4(192, 168, 77, 23);
        d->netmask = IPV4(255, 255, 255, 0);
        d->gateway = IPV4(192, 168, 77, 1);
        d->config_src = NETCFG_DEFAULT;
        kprintf("wifisim: joined '%s' (simulated - carries no traffic)\n", ssid);
        return 0;
    }
    return -1;
}

static int ws_disconnect(struct net_device* d) {
    g_joined = -1;
    d->ip = d->netmask = d->gateway = 0;
    kprintf("wifisim: disconnected\n");
    return 0;
}

static int ws_status(struct net_device* d, struct wifi_status* out) {
    (void)d;
    out->associated = g_joined >= 0;
    out->signal = g_joined >= 0 ? g_air[g_joined].signal : 0;
    cpy(out->ssid, g_joined >= 0 ? g_air[g_joined].ssid : "", WIFI_SSID_MAX);
    return 0;
}

static const struct net_wireless_ops g_ops = {
    .scan = ws_scan, .connect = ws_connect,
    .disconnect = ws_disconnect, .status = ws_status,
};

/* No wire: every frame is refused, which the stack counts as a send error. */
static int ws_transmit(struct net_device* d, const void* f, uint32_t len) {
    (void)d; (void)f; (void)len;
    return -1;
}

int wifisim_set(int on) {
    if (on && !g_registered) {
        g_wlan.name     = "wlan0";
        g_wlan.flags    = NETDEV_F_SIMULATED;
        const uint8_t mac[ETH_ALEN] = { 0x02, 0x00, 0x5E, 0x10, 0x20, 0x30 };
        for (int i = 0; i < ETH_ALEN; i++) g_wlan.mac[i] = mac[i];
        g_wlan.mtu      = ETH_MTU;
        g_wlan.transmit = ws_transmit;
        g_wlan.poll     = NULL;
        g_wlan.wireless = &g_ops;
        g_joined = -1;
        g_wlan.ip = g_wlan.netmask = g_wlan.gateway = 0;
        if (net_register(&g_wlan) != 0) return -1;
        g_registered = 1;
        kprintf("wifisim: wlan0 up (SIMULATED wireless adapter - no traffic)\n");
    } else if (!on && g_registered) {
        net_unregister(&g_wlan);
        g_registered = 0;
        g_joined = -1;
        kprintf("wifisim: wlan0 removed\n");
    }
    return 0;
}

CONFIG_KEY(ck_wifisim) = {
    .key = "net.wifisim", .group = "Network", .type = CFG_BOOL, .def = "0",
    .help = "add a SIMULATED Wi-Fi adapter (tests the chooser; carries no traffic)",
};

static void wifisim_service(void) {
    if (config_get_long("net.wifisim", 0)) wifisim_set(1);
    task_exit();
}
SERVICE("wifisim", wifisim_service, 1, SVC_RESTART_NO);

static void cmd_wifisim(const char* args) {
    while (args && *args == ' ') args++;
    if (args && cmd_streq(args, "on"))       wifisim_set(1);
    else if (args && cmd_streq(args, "off")) wifisim_set(0);
    else kprintf("usage: wifisim on|off   (a simulated adapter: %s)\n",
                 g_registered ? "present" : "absent");
}
SHELL_CMD(wifisim) = { "wifisim", "on|off", "a simulated Wi-Fi adapter for testing",
                       SHELL_G_TEST, cmd_wifisim, SHELL_P_ADMIN };
