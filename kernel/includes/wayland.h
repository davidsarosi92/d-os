/* =============================================================================
 * wayland.h — a minimal Wayland display server for d-os (§M26).
 *
 * Wayland is a wire protocol spoken over a unix-domain socket: the client and
 * server exchange messages of the form
 *
 *     [ object_id : u32 ] [ (size << 16) | opcode : u32 ] [ args… ]
 *
 * where `object_id` names a protocol object (id 1 is always wl_display), the
 * opcode selects a request (client→server) or event (server→client), and
 * `size` is the whole message length in bytes (header included).  Argument
 * types: uint/int/object/new_id are one u32; string/array are a u32 length
 * followed by the bytes padded up to 4; file descriptors travel out-of-band via
 * SCM_RIGHTS (d-os: usock_send's passfile).
 *
 * This first stage carries the transport + the core objects — wl_display,
 * wl_registry, wl_callback — enough for a client to complete the canonical
 * handshake (get_registry → receive the advertised globals → sync → receive
 * wl_callback.done).  wl_shm / wl_compositor / wl_surface buffers and the
 * bridge to the M22 compositor come in later stages.  The whole thing sits on
 * the M25 substrate (unix sockets + fd passing + memfd shm) and the M22.7
 * compositor, which was built surface-compositor-shaped precisely so
 * wl_surface maps onto it 1:1.
 * ============================================================================= */
#ifndef WAYLAND_H
#define WAYLAND_H

#include <stdint.h>
#include <stddef.h>

struct usock;
struct shm;
struct gfx_surface;
struct gui_window;

/* The fixed object-id of wl_display (the protocol root). */
#define WL_DISPLAY_ID  1u

/* A server-side client connection: the socket + a small object table. */
#define WL_MAX_OBJECTS 256
struct wl_conn {
    struct usock* sock;                 /* transport to this client            */
    uint8_t       obj_iface[WL_MAX_OBJECTS];  /* object id → WLI_* (0 = unused) */
    uint32_t      registry_id;          /* client's wl_registry object (0 = none) */
    uint32_t      serial;               /* monotonically increasing event tag  */

    /* Stage 2 — a single-surface buffer path (multi-surface is a follow-up):
     * the client's wl_shm_pool frames + the geometry of the wl_buffer it
     * attaches to its wl_surface, so `commit` can read the pixels. */
    struct shm*   pool_shm;             /* wl_shm_pool's shared frames         */
    uint32_t      surface_id;           /* the wl_surface object               */
    uint32_t      buffer_id;            /* the wl_buffer object                */
    uint32_t      buf_off, buf_w, buf_h, buf_stride;   /* wl_buffer geometry   */

    /* Stage 3 — xdg_shell top-level role. */
    uint32_t      xdg_surface_id;       /* xdg_surface wrapping the wl_surface  */
    uint32_t      xdg_toplevel_id;      /* xdg_toplevel role                    */

    /* Compositor bridge: when set, wl_surface.commit blits the buffer's pixels
     * into `target` at (blit_x, blit_y) — the surface's contents become visible
     * pixels.  NULL = headless (commit only reads/logs the buffer). */
    struct gfx_surface* target;
    int           blit_x, blit_y;
    /* Alternatively, a WM-managed window whose content IS the surface: commit
     * blits the buffer into it (chrome + move/resize come free from the WM). */
    struct gui_window*  window;
};

/* Initialise a connection over `sock` (registers wl_display as object 1). */
void wl_conn_init(struct wl_conn* c, struct usock* sock);

/* Process every request currently readable on the connection, sending the
 * appropriate events back.  Non-blocking.  Returns the number of requests
 * handled (0 if none were buffered), or -1 if the peer closed / errored. */
int  wl_conn_dispatch(struct wl_conn* c);

/* Self-test entry (shell `waytest`): drive a client handshake over a
 * usock_pair against wl_conn_dispatch and log the exchange. */
void wl_selftest(void);

/* Visible demo (shell `waydemo`): a client commits a colour buffer to a
 * surface whose server side is bridged to the framebuffer, so the pixels land
 * on screen; then reads a framebuffer pixel back as proof. */
void wl_visible_demo(void);

/* Windowed demo (shell `waywin`, GUI mode): the surface is backed by a real
 * WM-managed gui_window; the committed buffer becomes the window's contents. */
void wl_window_demo(void);

#endif /* WAYLAND_H */
