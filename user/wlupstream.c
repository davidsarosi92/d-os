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
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* musl only declares memfd_create when _GNU_SOURCE is on and the headers are
 * new enough; go straight to the syscall so this file needs no feature-test
 * gymnastics.  The name is advisory on Linux and ignored by d-os. */
static int memfd_create(const char *name, unsigned flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}
#include <xdg-shell-client-protocol.h>

#define W      160
#define H      120
#define STRIDE (W * 4)

static int have_compositor, have_shm, have_xdg, have_seat, n_globals;
static int configured, ok;
static struct wl_compositor *compositor;
static struct wl_shm        *shm;
static struct xdg_wm_base   *wm_base;

/* xdg_surface.configure must be acknowledged before the surface may be shown —
 * this is the handshake that makes the window real. */
static void xsurf_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
    (void)data;
    configured = 1;
    xdg_surface_ack_configure(xs, serial);
}
static const struct xdg_surface_listener xsurf_listener = { xsurf_configure };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)data;
    n_globals++;
    printf("wlupstream:   global %u: %s v%u\n", name, iface, version);
    /* Bind the ones we need.  Version 1 across the board: this client only uses
     * the base requests, and asking for no more than we use is what keeps it
     * portable to any compositor. */
    if (!strcmp(iface, "wl_compositor")) {
        have_compositor = 1;
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
    } else if (!strcmp(iface, "wl_shm")) {
        have_shm = 1;
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, "xdg_wm_base")) {
        have_xdg = 1;
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
    } else if (!strcmp(iface, "wl_seat")) {
        have_seat = 1;
    }
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

    if (!(compositor && shm && wm_base)) {
        printf("wlupstream: FAIL — a required global is missing\n");
        wl_display_disconnect(dpy);
        return 2;
    }
    printf("wlupstream: stage 1 PASS — handshake through upstream libwayland\n");

    /* ---- stage 2: a real shm buffer and a real xdg_toplevel ---------------
     * Everything below is the ordinary Wayland client sequence, written the way
     * any application writes it.  The interesting part on d-os is what it
     * exercises underneath: memfd_create + ftruncate + mmap, and the pool fd
     * travelling to the server over SCM_RIGHTS. */
    int fd = memfd_create("wl-shm", 0);
    if (fd < 0) { printf("wlupstream: memfd_create failed\n"); goto out; }
    if (ftruncate(fd, STRIDE * H) < 0) {
        printf("wlupstream: ftruncate failed\n"); goto out;
    }
    uint32_t *px = mmap(NULL, STRIDE * H, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) { printf("wlupstream: mmap failed\n"); goto out; }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = ((x ^ y) & 16) ? 0xFF3366CCu : 0xFF102040u;
    printf("wlupstream: shm buffer %dx%d ready (fd %d)\n", W, H, fd);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, STRIDE * H);
    if (!pool) { printf("wlupstream: create_pool failed\n"); goto out; }
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, W, H, STRIDE,
                                                      WL_SHM_FORMAT_ARGB8888);
    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    if (!buf || !surf) { printf("wlupstream: buffer/surface failed\n"); goto out; }

    struct xdg_surface *xsurf = xdg_wm_base_get_xdg_surface(wm_base, surf);
    if (!xsurf) { printf("wlupstream: get_xdg_surface failed\n"); goto out; }
    xdg_surface_add_listener(xsurf, &xsurf_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xsurf);
    if (!top) { printf("wlupstream: get_toplevel failed\n"); goto out; }
    xdg_toplevel_set_title(top, "Upstream Wayland");
    wl_surface_commit(surf);            /* role assigned → server configures */

    if (wl_display_roundtrip(dpy) < 0) {
        printf("wlupstream: configure roundtrip failed\n"); goto out;
    }
    printf("wlupstream: xdg_surface.configure seen = %d\n", configured);

    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, W, H);
    wl_surface_commit(surf);
    if (wl_display_roundtrip(dpy) < 0) {
        printf("wlupstream: commit roundtrip failed\n"); goto out;
    }

    ok = configured;
    printf("wlupstream: %s — UPSTREAM libwayland-client drove a real "
           "xdg_toplevel + shm buffer on d-os\n", ok ? "PASS" : "FAIL");

out:
    wl_display_disconnect(dpy);
    return ok ? 0 : 2;
}
