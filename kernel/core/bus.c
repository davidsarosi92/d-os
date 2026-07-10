/* =============================================================================
 * bus.c — service-bus broker: strict binding + opt-in version adaptation (M29,
 * discovery half).  See bus.h for the model.
 *
 * Resolution is strict on the wire (exact endpoint + contract@version).  A
 * strict miss is bridged ONLY when the `bus.allow-adaptation` config bit is set
 * AND a registered ADAPTER can synthesise the requested version over a
 * higher-version provider at the same endpoint.  The domain↔transport rule is
 * enforced here: only a KERNEL-domain / LocalCall provider is invokable today;
 * USER/ISOLATED providers need the reserved IPC/SharedMemory transports (M25),
 * so a bind to one fails cleanly rather than pretending.
 * ============================================================================= */

#include "bus.h"
#include "config.h"
#include "printf.h"
#include "procfs.h"
#include <stddef.h>

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int provider_count(void) {
    return (int)(__stop_bus_providers - __start_bus_providers);
}
static int adapter_count(void) {
    return (int)(__stop_bus_adapters - __start_bus_adapters);
}

static int allow_adaptation(void) {
    const char* v = config_get("bus.allow-adaptation", "0");
    return v && v[0] == '1';
}

static const char* domain_name(enum bus_domain d) {
    switch (d) {
        case BUS_DOMAIN_KERNEL:   return "kernel";
        case BUS_DOMAIN_USER:     return "user";
        case BUS_DOMAIN_ISOLATED: return "isolated";
    }
    return "?";
}
static const char* transport_name(enum bus_transport t) {
    switch (t) {
        case BUS_LOCALCALL:    return "localcall";
        case BUS_SHAREDMEMORY: return "sharedmem";
        case BUS_IPC:          return "ipc";
    }
    return "?";
}

/* Is this provider invokable with today's transports?  Only an in-kernel
 * LocalCall provider is real; the non-local transports are reserved until
 * M25 lights them up. */
static int provider_invokable(const struct bus_provider* p) {
    return p->domain == BUS_DOMAIN_KERNEL && p->transport == BUS_LOCALCALL;
}

int bus_bind(const char* endpoint, const char* contract, int version,
             struct bus_binding* out) {
    if (!endpoint || !contract || !out) return -1;

    /* 1) Strict exact match. */
    int np = provider_count();
    for (int i = 0; i < np; i++) {
        const struct bus_provider* p = &__start_bus_providers[i];
        if (streq(p->endpoint, endpoint) && streq(p->contract, contract) &&
            p->version == version) {
            if (!provider_invokable(p)) return -4;   /* reserved transport/domain */
            out->provider  = p;
            out->iface     = p->iface;
            out->transport = p->transport;
            out->adapted   = 0;
            return 0;
        }
    }

    /* 2) Adaptation — opt-in, and only for a real version bridge. */
    if (!allow_adaptation()) return -2;              /* strict miss, no bridging */

    int na = adapter_count();
    for (int i = 0; i < np; i++) {
        const struct bus_provider* p = &__start_bus_providers[i];
        if (!streq(p->endpoint, endpoint) || !streq(p->contract, contract))
            continue;
        /* p is a same-endpoint provider of some other version; is there an
         * adapter from the requested version to p's version? */
        for (int j = 0; j < na; j++) {
            const struct bus_adapter* a = &__start_bus_adapters[j];
            if (streq(a->contract, contract) &&
                a->from_version == version && a->to_version == p->version) {
                if (!provider_invokable(p)) return -4;
                out->provider  = p;
                out->iface     = a->adapt(p->iface);  /* synthesise `from` iface */
                out->transport = p->transport;
                out->adapted   = 1;
                if (!out->iface) return -5;            /* adapter refused */
                return 0;
            }
        }
    }
    return -3;                                          /* no provider / no bridge */
}

/* ------------------------------------------------------------------- */
/* /proc/bus + boot wiring.                                             */
/* ------------------------------------------------------------------- */

static void gen_bus(struct procfs_writer* w) {
    pw_puts(w, "# endpoints (endpoint  contract@version  domain  transport  invokable)\n");
    int np = provider_count();
    for (int i = 0; i < np; i++) {
        const struct bus_provider* p = &__start_bus_providers[i];
        pw_puts(w, p->endpoint); pw_putc(w, ' ');
        pw_puts(w, p->contract); pw_putc(w, '@');
        pw_put_uint(w, (unsigned)p->version); pw_putc(w, ' ');
        pw_puts(w, domain_name(p->domain)); pw_putc(w, ' ');
        pw_puts(w, transport_name(p->transport)); pw_putc(w, ' ');
        pw_puts(w, provider_invokable(p) ? "yes" : "no");
        pw_putc(w, '\n');
    }
    int na = adapter_count();
    if (na > 0) {
        pw_puts(w, "# adapters (contract  from->to)  allow-adaptation=");
        pw_puts(w, allow_adaptation() ? "on" : "off");
        pw_putc(w, '\n');
        for (int j = 0; j < na; j++) {
            const struct bus_adapter* a = &__start_bus_adapters[j];
            pw_puts(w, a->contract); pw_putc(w, ' ');
            pw_put_uint(w, (unsigned)a->from_version); pw_puts(w, "->");
            pw_put_uint(w, (unsigned)a->to_version);
            pw_putc(w, '\n');
        }
    }
}

static struct procfs_node nd_bus = { .name = "bus", .gen = gen_bus };

void bus_init(void) {
    procfs_register(&nd_bus);
}
