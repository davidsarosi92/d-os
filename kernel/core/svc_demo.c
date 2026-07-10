/* =============================================================================
 * svc_demo.c — demonstrator services + a demo bus contract (M29).
 *
 * Proves the M29 machinery end-to-end without pulling in a real subsystem:
 *
 *   Services (supervisor):
 *     - "heartbeat" — autostart, restart=always: a daemon that should stay up.
 *       It logs only on start/stop (no per-tick spam), so a restart is visible
 *       as a fresh "up (pid …)" line + a bumped /proc/services restart count —
 *       exactly the DoD "an always-restart service killed by hand comes back".
 *     - "crasher"  — manual, restart=on-failure: exits code 1 shortly after
 *       start, so `service start crasher` shows the supervisor restarting it
 *       with crash-loop backoff.
 *
 *   Bus contract (Greeter v1/v2):
 *     - a v2 provider at endpoint "greeter.default";
 *     - an ADAPTER(Greeter, 1→2) that synthesises a v1 iface over it.
 *     `bustest` binds v2 exactly, shows a strict v1 bind MISSING with
 *     allow-adaptation off, and SUCCEEDING via the shim with it on.
 *
 * The contracts are marshalling-shaped (arguments are copied C strings / return
 * a copied string, no shared raw kernel pointers) so they could later move to a
 * non-local transport (§M25/§M33) unchanged — convention #5.
 * ============================================================================= */

#include "service.h"
#include "bus.h"
#include "task.h"
#include "klog.h"
#include "printf.h"
#include "config.h"
#include <stddef.h>

/* ------------------------------------------------------------------- */
/* Demo services.                                                       */
/* ------------------------------------------------------------------- */

static void heartbeat_entry(void) {
    klog(KLOG_INFO, "heartbeat", "up (pid %d)\n",
         task_current() ? task_current()->pid : -1);
    for (;;) {
        if (task_should_stop()) { klog(KLOG_INFO, "heartbeat", "stopping\n"); task_exit(); }
        task_msleep(2000);          /* silent tick — evidence is start/restart */
    }
}
SERVICE("heartbeat", heartbeat_entry, 1, SVC_RESTART_ALWAYS);

static void crasher_entry(void) {
    task_msleep(400);               /* run briefly … */
    task_exit_code(1);              /* … then "crash" so on-failure restarts it */
}
SERVICE("crasher", crasher_entry, 0, SVC_RESTART_ON_FAILURE);

/* ------------------------------------------------------------------- */
/* Demo bus contract: Greeter v1 / v2 + a 1→2 adapter.                  */
/* ------------------------------------------------------------------- */

/* Contract interfaces (hal_api.h-shaped structs-of-fn-pointers). */
struct greeter_v1 { const char* (*greet)(void); };
struct greeter_v2 { const char* (*greet)(const char* who); };

static int demo_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* v2 provider: greet(who) → "hello, <who> (v2)" in a static buffer (single-
 * binding demo). */
static char g2buf[80];
static const char* greeter2_greet(const char* who) {
    int k = 0;
    const char* pre = "hello, ";
    const char* suf = " (v2)";
    for (const char* p = pre; *p && k < 79; p++)          g2buf[k++] = *p;
    for (const char* p = who; p && *p && k < 79; p++)     g2buf[k++] = *p;
    for (const char* p = suf; *p && k < 79; p++)          g2buf[k++] = *p;
    g2buf[k] = 0;
    return g2buf;
}
static const struct greeter_v2 greeter2_iface = { .greet = greeter2_greet };
BUS_PROVIDER(greeter2, "greeter.default", "Greeter", 2,
             BUS_DOMAIN_KERNEL, BUS_LOCALCALL, &greeter2_iface);

/* Adapter: build a v1 iface over a bound v2 iface.  v1.greet() has no `who`, so
 * the shim supplies a default ("world") — a real backward-compat shim's job. */
static const struct greeter_v2* g_adapt_v2;      /* the bound v2 iface */
static const char* adapt_v1_greet(void) {
    return g_adapt_v2 ? g_adapt_v2->greet("world") : "(no v2)";
}
static const struct greeter_v1 adapt_v1_iface = { .greet = adapt_v1_greet };
static const void* greeter_adapt_1_to_2(const void* to_iface) {
    g_adapt_v2 = (const struct greeter_v2*)to_iface;
    return &adapt_v1_iface;
}
BUS_ADAPTER(greeter_1_2, "Greeter", 1, 2, greeter_adapt_1_to_2);

/* ------------------------------------------------------------------- */
/* Bus self-test (shell `bustest`).                                     */
/* ------------------------------------------------------------------- */

void svc_demo_bustest(void) {
    struct bus_binding b;

    /* Exact v2 bind → provider iface, not adapted. */
    int v2_ok = 0;
    if (bus_bind("greeter.default", "Greeter", 2, &b) == 0 && !b.adapted) {
        const struct greeter_v2* g = (const struct greeter_v2*)b.iface;
        v2_ok = demo_streq(g->greet("world"), "hello, world (v2)");
    }

    /* Strict v1 bind with adaptation OFF → must miss (no v1 provider). */
    config_set("bus.allow-adaptation", "0");
    int strict_ok = (bus_bind("greeter.default", "Greeter", 1, &b) < 0);

    /* v1 bind with adaptation ON → bridged to v2 via the registered adapter. */
    config_set("bus.allow-adaptation", "1");
    int adapt_ok = 0;
    if (bus_bind("greeter.default", "Greeter", 1, &b) == 0 && b.adapted) {
        const struct greeter_v1* g = (const struct greeter_v1*)b.iface;
        adapt_ok = demo_streq(g->greet(), "hello, world (v2)");
    }
    config_set("bus.allow-adaptation", "0");     /* restore default */

    kprintf("bustest: exact-v2=%s strict-v1-miss=%s adapted-v1->v2=%s\n",
            v2_ok ? "PASS" : "FAIL", strict_ok ? "PASS" : "FAIL",
            adapt_ok ? "PASS" : "FAIL");
}
