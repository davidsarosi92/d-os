/* =============================================================================
 * bus.h — service bus: named, versioned, transport-abstracted bindings (M29).
 *
 * The discovery + binding half of M29.  Today a caller reaches a subsystem by
 * hard-linking to its symbols (`net_send()`), coupling it to *this*
 * implementation in *this* address space.  The bus replaces the hard link with
 * a named, versioned, transport-abstracted binding — the QNX resource-manager
 * / Fuchsia FIDL / Android Binder shape, sized down for a teaching OS.  Three
 * concepts (mirroring how hal_api.h already versions an interface):
 *
 *   - Endpoint  — a name in a flat namespace ("greeter.default", "block.vda").
 *                 Callers resolve an endpoint; they never name an
 *                 implementation or an address space.
 *   - Contract  — a versioned interface identified by (name, version), e.g.
 *                 Greeter v1.  Concretely a versioned struct-of-function-
 *                 pointers (hal_api.h's shape).  No IDL / codegen — hand-
 *                 written C interface structs.
 *   - Transport — HOW a binding is invoked.  LocalCall (direct call, same
 *                 address space — the only real one today); SharedMemory / IPC
 *                 are defined now but reserved (need M25 spaces + fd passing).
 *
 * Binding resolution is STRICT ON THE WIRE: only an exact contract@version
 * match binds.  Compatibility is an opt-in *mechanism*, not a policy branch:
 * an ADAPTER(contract, from, to) registry entry synthesises a `from`-shaped
 * iface over a `to` provider, inserted by the broker only when a strict miss
 * can be bridged AND the `allow-adaptation` config bit is set.  A provider
 * that speaks several versions just registers as its own multi-version
 * adapter — "backward-compatible" is that special case, no extra code path.
 *
 * Marshalling discipline (convention #5): contracts are designed as if
 * marshalled even while only LocalCall exists — arguments are handles +
 * copied/shared buffers, never freely-shared raw kernel pointers — so a
 * contract can later move to a USER domain (§M33) by a config flip, not a
 * rewrite.
 * ============================================================================= */

#ifndef BUS_H
#define BUS_H

/* HOW a binding is invoked.  Only LocalCall is real today; the other two are
 * reserved interface points that light up with M25's non-local transports. */
enum bus_transport {
    BUS_LOCALCALL   = 0,   /* direct function call, same address space  */
    BUS_SHAREDMEMORY,      /* reserved — needs M25 shared memory         */
    BUS_IPC,               /* reserved — needs M25 unix sockets          */
};

/* WHERE a provider runs (its declared execution domain — the §M33 axis).  The
 * broker enforces the domain↔transport rule at bind: a KERNEL provider serves
 * LocalCall; USER/ISOLATED need IPC/SharedMemory (reserved today). */
enum bus_domain {
    BUS_DOMAIN_KERNEL   = 0,  /* in-kernel provider (LocalCall)          */
    BUS_DOMAIN_USER,          /* ring-3 process — reserved (needs M25)    */
    BUS_DOMAIN_ISOLATED,      /* isolated process — reserved             */
};

struct bus_provider {
    const char*        endpoint;    /* "greeter.default"                    */
    const char*        contract;    /* "Greeter"                            */
    int                version;     /* 1, 2, …                              */
    enum bus_domain    domain;
    enum bus_transport transport;
    const void*        iface;       /* pointer to the contract's vtable     */
};

struct bus_adapter {
    const char* contract;           /* "Greeter"                            */
    int         from_version;       /* the version a caller asks for        */
    int         to_version;         /* the version a provider actually has  */
    /* Build a `from`-shaped iface backed by a `to`-shaped iface.  Returns a
     * (typically static) vtable.  Single-binding demo semantics — a real
     * multi-binding adapter would allocate per-binding state. */
    const void* (*adapt)(const void* to_iface);
};

extern struct bus_provider __start_bus_providers[];
extern struct bus_provider __stop_bus_providers[];
extern struct bus_adapter  __start_bus_adapters[];
extern struct bus_adapter  __stop_bus_adapters[];

/* Publish a provider at an endpoint. */
#define BUS_PROVIDER(_sym, _ep, _contract, _ver, _domain, _transport, _iface) \
    static const struct bus_provider                                          \
    __attribute__((used, section("bus_providers"), aligned(4)))               \
    __bus_provider_##_sym = {                                                 \
        .endpoint = (_ep), .contract = (_contract), .version = (_ver),        \
        .domain = (_domain), .transport = (_transport), .iface = (_iface),    \
    }

/* Register a version-bridging adapter. */
#define BUS_ADAPTER(_sym, _contract, _from, _to, _adaptfn)               \
    static const struct bus_adapter                                      \
    __attribute__((used, section("bus_adapters"), aligned(4)))           \
    __bus_adapter_##_sym = {                                            \
        .contract = (_contract), .from_version = (_from),                \
        .to_version = (_to), .adapt = (_adaptfn),                        \
    }

/* Result of a resolve. */
struct bus_binding {
    const struct bus_provider* provider;   /* the provider actually bound   */
    const void*        iface;              /* the vtable the caller invokes */
    enum bus_transport transport;
    int                adapted;            /* 1 if reached via an ADAPTER    */
};

/* Boot: register /proc/bus. */
void bus_init(void);

/* Resolve (endpoint, contract@version) → a binding the caller invokes
 * transport-agnostically.  Strict exact match first; on a miss, if
 * `allow-adaptation` is set and a registered adapter bridges the requested
 * version to a higher-version provider at the same endpoint, insert the shim.
 * Returns 0 on success; negative on: no provider / version mismatch with no
 * adapter / a non-LocalCall transport that isn't implemented yet. */
int bus_bind(const char* endpoint, const char* contract, int version,
             struct bus_binding* out);

#endif
