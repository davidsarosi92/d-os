/* =============================================================================
 * wlupstream.c — a client that uses the REAL libwayland-client (§M40).
 *
 * §M26 shipped d-os's own Wayland server plus a hand-written mini client library
 * (user/libwl).  That proved the WIRE PROTOCOL.  This proves the thing that
 * actually matters for running unmodified applications: the upstream library —
 * the same libwayland-client.a a GTK/Qt/SDL program links against on Linux —
 * talking to our server.  Not one line of wayland is forked; everything below
 * is public API.
 *
 * HOW IT CONNECTS.  d-os has no named UNIX sockets yet, so there is no
 * $XDG_RUNTIME_DIR/wayland-0 to open.  We do not need one: libwayland honours
 * WAYLAND_SOCKET, an ALREADY-CONNECTED file descriptor number, which is exactly
 * the shape d-os already uses (the shell hands the client fd 3 and runs the
 * server on the other end).  That is a documented upstream mechanism — it is how
 * a Wayland compositor launches its own clients — not a workaround.
 *
 * The DoD: wl_display_connect + wl_display_get_registry + a real listener +
 * wl_display_roundtrip, and the globals our server advertises come back through
 * the upstream dispatch machinery (which means libffi's ffi_call is working too).
 * ============================================================================= */

#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int have_compositor, have_shm, have_xdg, have_seat, n_globals;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data; (void)reg;
    n_globals++;
    printf("wlupstream:   global %u: %s v%u\n", name, iface, version);
    if      (!strcmp(iface, "wl_compositor")) have_compositor = 1;
    else if (!strcmp(iface, "wl_shm"))        have_shm        = 1;
    else if (!strcmp(iface, "xdg_wm_base"))   have_xdg        = 1;
    else if (!strcmp(iface, "wl_seat"))       have_seat       = 1;
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener reg_listener = {
    reg_global, reg_global_remove
};

int main(void)
{
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        printf("wlupstream: wl_display_connect failed "
               "(is WAYLAND_SOCKET set?)\n");
        return 1;
    }
    printf("wlupstream: connected, display fd = %d\n", wl_display_get_fd(dpy));

    struct wl_registry *reg = wl_display_get_registry(dpy);
    if (!reg) { printf("wlupstream: get_registry failed\n"); return 1; }
    wl_registry_add_listener(reg, &reg_listener, NULL);

    /* The roundtrip is the real test: it marshals wl_display.sync, flushes,
     * reads the server's reply and dispatches every queued event through
     * libwayland's own closure machinery (libffi) into our listener. */
    if (wl_display_roundtrip(dpy) < 0) {
        /* Ask the library WHY: a protocol error names the interface, the opcode
         * and the object, which is exactly what is needed to find the mismatch
         * on the server side.  A bare "failed" would leave us guessing. */
        int err = wl_display_get_error(dpy);
        printf("wlupstream: roundtrip failed, wl_display_get_error=%d\n", err);
        if (err == EPROTO) {
            const struct wl_interface *iface = NULL;
            uint32_t id = 0;
            uint32_t code = wl_display_get_protocol_error(dpy, &iface, &id);
            printf("wlupstream: protocol error code=%u object=%u interface=%s\n",
                   code, id, iface && iface->name ? iface->name : "(none)");
        }
        wl_display_disconnect(dpy);
        return 1;
    }

    printf("wlupstream: %d global(s) — compositor=%d shm=%d xdg_wm_base=%d seat=%d\n",
           n_globals, have_compositor, have_shm, have_xdg, have_seat);

    int ok = have_compositor && have_shm && have_xdg && have_seat;
    printf("wlupstream: %s — UPSTREAM libwayland-client spoke to the d-os server\n",
           ok ? "PASS" : "FAIL");

    wl_display_disconnect(dpy);
    return ok ? 0 : 2;
}
