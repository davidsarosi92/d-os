/* =============================================================================
 * libwl.h — a minimal reusable user-space Wayland client library for d-os.
 *
 * The "libwayland-client layer": a d-os app links this and calls wl_* functions
 * instead of hand-marshalling the wire protocol.  It is NOT the upstream
 * libwayland (that is §M40 — it needs wayland-scanner + the protocol XML →
 * generated proxies, built as a musl library); this is a small hand-written
 * core that covers the connection + the registry roundtrip + object-id
 * allocation, enough for an app to discover the compositor's globals.
 *
 * The connection is an already-open socket fd (the compositor hands the client
 * one end of a socketpair — d-os installs it as fd 3, the WAYLAND_SOCKET model).
 * ============================================================================= */
#ifndef LIBWL_H
#define LIBWL_H

struct wlctx {
    int      fd;                        /* the Wayland connection socket        */
    unsigned next_id;                   /* next client object id to allocate    */
    int      nglobals;                  /* globals seen in the last roundtrip   */
    /* Registry names of the well-known globals (0 = not advertised). */
    unsigned compositor_name, shm_name, xdg_name, seat_name;
    /* Receive buffer for message framing. */
    unsigned char rbuf[1024];
    int      rlen;
};

/* Wrap an open connection fd.  Returns 0. */
int      wl_connect(struct wlctx* c, int fd);

/* Allocate a fresh client object id (wl_display is always id 1). */
unsigned wl_alloc_id(struct wlctx* c);

/* wl_display.get_registry + wl_display.sync, then read events until the sync
 * callback fires, recording each advertised global's registry name.  Returns
 * the number of globals seen, or -1 on I/O error. */
int      wl_registry_roundtrip(struct wlctx* c);

#endif /* LIBWL_H */
