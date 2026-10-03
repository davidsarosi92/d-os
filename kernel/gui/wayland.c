/* =============================================================================
 * wayland.c — a minimal Wayland display server for d-os (§M26), stage 1.
 *
 * Stage 1 = the wire protocol + transport + the core objects (wl_display,
 * wl_registry, wl_callback) needed for a client to complete the canonical
 * handshake:  get_registry → the server advertises its globals → sync → the
 * server answers with wl_callback.done + wl_display.delete_id.
 *
 * The protocol is exactly the real Wayland wire format (little-endian, 8-byte
 * message header [object_id][size<<16|opcode], u32/string args), so a real
 * libwayland client would speak the same bytes — but there is no libwayland on
 * d-os yet, so `wl_selftest` drives a hand-marshalled client over a usock_pair
 * (the analogue of user/linuxhello.c proving the Linux ABI before real musl).
 *
 * Later stages: wl_registry.bind, wl_shm + wl_shm_pool (client memfd via the
 * SCM_RIGHTS fd passing usock already supports) + wl_buffer, wl_surface.attach/
 * commit bridged onto a gui_window's gfx_surface + gui_damage, then xdg_shell.
 * ============================================================================= */

#include "wayland.h"
#include "fd.h"          /* usock_pair/send/recv/close/can_read */
#include "gfx.h"         /* gfx_surface / gfx_fb_surface — the compositor bridge */
#include "gui.h"           /* gui_window_blit — the WM-managed window target      */

#include "timer.h"         /* timer_ticks_ms — wl_callback.done timestamps        */
#include "task.h"        /* task_msleep / task_start_arg                        */
#include "kmalloc.h"     /* the per-connection wl_conn for a server task        */
#include "printf.h"
#include "clipboard.h"   /* §M59 — the selection IS the d-os clipboard      */
#include "charset.h"
#include <stdint.h>
#include <stddef.h>

/* Interface tags stored in wl_conn.obj_iface[]. */
enum { WLI_NONE = 0, WLI_DISPLAY, WLI_REGISTRY, WLI_CALLBACK,
       WLI_COMPOSITOR, WLI_SHM, WLI_SHM_POOL, WLI_BUFFER, WLI_SURFACE,
       WLI_XDG_WM_BASE, WLI_XDG_SURFACE, WLI_XDG_TOPLEVEL,
       WLI_SEAT, WLI_POINTER, WLI_KEYBOARD, WLI_OUTPUT,
       WLI_DATA_DEVICE_MANAGER, WLI_DATA_SOURCE, WLI_DATA_DEVICE };

/* Real Wayland opcodes (from wayland.xml).  request = client→server. */
enum { WL_DISPLAY_REQ_SYNC = 0, WL_DISPLAY_REQ_GET_REGISTRY = 1 };
enum { WL_DISPLAY_EVT_ERROR = 0, WL_DISPLAY_EVT_DELETE_ID = 1 };
enum { WL_REGISTRY_REQ_BIND = 0 };
enum { WL_REGISTRY_EVT_GLOBAL = 0 };
enum { WL_CALLBACK_EVT_DONE = 0 };
enum { WL_COMPOSITOR_REQ_CREATE_SURFACE = 0 };
enum { WL_SHM_REQ_CREATE_POOL = 0 };
enum { WL_SHM_EVT_FORMAT = 0 };
enum { WL_SHM_POOL_REQ_CREATE_BUFFER = 0, WL_SHM_POOL_REQ_DESTROY = 1 };
enum { WL_BUFFER_EVT_RELEASE = 0 };
enum { WL_SURFACE_REQ_ATTACH = 1, WL_SURFACE_REQ_DAMAGE = 2,
       WL_SURFACE_REQ_FRAME = 3,
       WL_SURFACE_REQ_COMMIT = 6, WL_SURFACE_REQ_DAMAGE_BUFFER = 9 };
/* wl_output — every real toolkit enumerates outputs before it will start. */
enum { WL_OUTPUT_EVT_GEOMETRY = 0, WL_OUTPUT_EVT_MODE = 1,
       WL_OUTPUT_EVT_DONE = 2, WL_OUTPUT_EVT_SCALE = 3 };
enum { WL_OUTPUT_MODE_CURRENT = 1, WL_OUTPUT_MODE_PREFERRED = 2 };
/* xdg_shell (the modern window role protocol). */
enum { XDG_WM_BASE_REQ_GET_XDG_SURFACE = 2 };
enum { XDG_SURFACE_REQ_GET_TOPLEVEL = 1, XDG_SURFACE_REQ_ACK_CONFIGURE = 4 };
enum { XDG_SURFACE_EVT_CONFIGURE = 0 };
enum { XDG_TOPLEVEL_REQ_SET_TITLE = 2, XDG_TOPLEVEL_REQ_SET_APP_ID = 3 };
enum { XDG_TOPLEVEL_EVT_CONFIGURE = 0, XDG_TOPLEVEL_EVT_CLOSE = 1 };
/* wl_seat / wl_pointer / wl_keyboard (input). */
enum { WL_SEAT_REQ_GET_POINTER = 0, WL_SEAT_REQ_GET_KEYBOARD = 1 };
enum { WL_SEAT_EVT_CAPABILITIES = 0 };
enum { WL_SEAT_CAP_POINTER = 1, WL_SEAT_CAP_KEYBOARD = 2 };
enum { WL_POINTER_EVT_ENTER = 0, WL_POINTER_EVT_LEAVE = 1,
       WL_POINTER_EVT_MOTION = 2, WL_POINTER_EVT_BUTTON = 3,
       WL_POINTER_EVT_FRAME = 5 };
enum { WL_KEYBOARD_EVT_KEYMAP = 0, WL_KEYBOARD_EVT_ENTER = 1,
       WL_KEYBOARD_EVT_LEAVE = 2, WL_KEYBOARD_EVT_KEY = 3,
       WL_KEYBOARD_EVT_MODIFIERS = 4 };
enum { WL_KEY_RELEASED = 0, WL_KEY_PRESSED = 1 };
/* §M59 — wl_data_device_manager v3 and its three objects (wayland.xml). */
enum { WL_DDM_REQ_CREATE_DATA_SOURCE = 0, WL_DDM_REQ_GET_DATA_DEVICE = 1 };
enum { WL_DATA_SOURCE_REQ_OFFER = 0, WL_DATA_SOURCE_REQ_DESTROY = 1,
       WL_DATA_SOURCE_REQ_SET_ACTIONS = 2 };
enum { WL_DATA_SOURCE_EVT_TARGET = 0, WL_DATA_SOURCE_EVT_SEND = 1,
       WL_DATA_SOURCE_EVT_CANCELLED = 2 };
enum { WL_DATA_DEVICE_REQ_START_DRAG = 0, WL_DATA_DEVICE_REQ_SET_SELECTION = 1,
       WL_DATA_DEVICE_REQ_RELEASE = 2 };
enum { WL_DATA_DEVICE_EVT_DATA_OFFER = 0, WL_DATA_DEVICE_EVT_SELECTION = 5 };
enum { WL_DATA_OFFER_REQ_ACCEPT = 0, WL_DATA_OFFER_REQ_RECEIVE = 1,
       WL_DATA_OFFER_REQ_DESTROY = 2, WL_DATA_OFFER_REQ_FINISH = 3,
       WL_DATA_OFFER_REQ_SET_ACTIONS = 4 };
enum { WL_DATA_OFFER_EVT_OFFER = 0 };
/* Ids the SERVER allocates (a data_offer) start here, per the protocol. */
#define WL_SERVER_ID_BASE 0xff000000u

/* wl_shm pixel formats (subset). */
enum { WL_SHM_FORMAT_ARGB8888 = 0, WL_SHM_FORMAT_XRGB8888 = 1 };

/* The globals this server advertises (name = the bind() handle). */
struct wl_global { uint32_t name; const char* iface; uint32_t version; };
static const struct wl_global g_globals[] = {
    { 1, "wl_compositor", 4 },
    { 2, "wl_shm",        1 },
    { 3, "xdg_wm_base",   2 },
    { 4, "wl_seat",       5 },
    { 5, "wl_output",     2 },
    { 6, "wl_data_device_manager", 3 },
};
#define WL_NGLOBALS (int)(sizeof g_globals / sizeof g_globals[0])

/* ---- little-endian wire helpers ------------------------------------------ */

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }
static uint32_t cstrlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* §M59 — every server→client message goes through here.  It sends the WHOLE
 * message or none of it (usock_send_whole): a short write used to leave half
 * an event in the stream whenever the 4 KiB ring was full — a burst of pointer
 * motion at a client that was busy drawing was enough — and the next event's
 * header then landed mid-message, which libwayland treats as fatal.  If there
 * is no room it waits (bounded) for the client to drain; a client that does
 * not drain within the bound is STALLED, and further messages are dropped
 * without waiting until one fits again, so a wedged client costs the
 * compositor (which sends input on its own task) one timeout, not one per
 * event.  Dropped messages are counted and announced once. */
#define WL_SEND_WAIT_MS 500
static long wl_send(struct wl_conn* c, const void* m, uint32_t n, struct ofile* pf) {
    uint64_t deadline = timer_ticks_ms() + (c->stalled ? 0 : WL_SEND_WAIT_MS);
    for (;;) {
        long r = usock_send_whole(c->sock, m, n, pf);
        if (r != 0) {                               /* sent, or peer gone */
            if (r > 0) c->stalled = 0;
            return r;
        }
        if (timer_ticks_ms() >= deadline) break;
        task_msleep(1);
    }
    if (!c->stalled)
        kprintf("wayland: client is not reading its socket - dropping events "
                "until it does\n");
    c->stalled = 1;
    c->dropped++;
    return 0;
}

/* Read EXACTLY n bytes (the message framing is by the size field, so we always
 * know how many to pull).  Returns n, or <n on EOF/peer-close. */
static long recv_exact(struct usock* s, uint8_t* buf, long n) {
    long done = 0;
    while (done < n) {
        long r = usock_recv(s, buf + done, (size_t)(n - done), 1 /*block*/, NULL);
        if (r <= 0) break;                 /* peer closed / error */
        done += r;
    }
    return done;
}

/* ---- server → client events ---------------------------------------------- */

/* Emit wl_registry.global(name, interface, version) on the client's registry. */
static void send_global(struct wl_conn* c, const struct wl_global* g) {
    uint8_t msg[64];
    uint32_t slen = cstrlen(g->iface) + 1;            /* incl. NUL */
    uint32_t size = 8 + 4 /*name*/ + 4 /*strlen*/ + align4(slen) + 4 /*version*/;
    put32(msg + 0, c->registry_id);
    put32(msg + 4, (size << 16) | WL_REGISTRY_EVT_GLOBAL);
    put32(msg + 8, g->name);
    put32(msg + 12, slen);
    uint32_t o = 16;
    for (uint32_t i = 0; i < slen; i++) msg[o + i] = (uint8_t)g->iface[i];
    for (uint32_t i = slen; i < align4(slen); i++) msg[o + i] = 0;   /* pad */
    o += align4(slen);
    put32(msg + o, g->version);
    wl_send(c, msg, size, NULL);
}

/* Emit wl_callback.done(serial) on object `cb`. */
static void send_callback_done(struct wl_conn* c, uint32_t cb, uint32_t data) {
    uint8_t msg[12];
    put32(msg + 0, cb);
    put32(msg + 4, (12u << 16) | WL_CALLBACK_EVT_DONE);
    put32(msg + 8, data);
    wl_send(c, msg, 12, NULL);
}

/* Emit wl_display.delete_id(id) — tells the client the server released `id`. */
static void send_delete_id(struct wl_conn* c, uint32_t id) {
    uint8_t msg[12];
    put32(msg + 0, WL_DISPLAY_ID);
    put32(msg + 4, (12u << 16) | WL_DISPLAY_EVT_DELETE_ID);
    put32(msg + 8, id);
    wl_send(c, msg, 12, NULL);
}

/* Emit wl_shm.format(format) on the client's wl_shm object. */
static void send_shm_format(struct wl_conn* c, uint32_t shm_id, uint32_t fmt) {
    uint8_t msg[12];
    put32(msg + 0, shm_id);
    put32(msg + 4, (12u << 16) | WL_SHM_EVT_FORMAT);
    put32(msg + 8, fmt);
    wl_send(c, msg, 12, NULL);
}

/* Emit wl_buffer.release(buffer) — the server is done reading the buffer, so
 * the client may reuse it. */
static void send_buffer_release(struct wl_conn* c, uint32_t buffer_id) {
    uint8_t msg[8];
    put32(msg + 0, buffer_id);
    put32(msg + 4, (8u << 16) | WL_BUFFER_EVT_RELEASE);
    wl_send(c, msg, 8, NULL);
}

/* Emit xdg_toplevel.configure(width, height, states[]) — an empty state array
 * + 0×0 lets the client pick its own size. */
static void send_xdg_toplevel_configure(struct wl_conn* c, uint32_t tl,
                                        uint32_t w, uint32_t h) {
    uint8_t msg[20];
    put32(msg + 0, tl);
    put32(msg + 4, (20u << 16) | XDG_TOPLEVEL_EVT_CONFIGURE);
    put32(msg + 8, w);
    put32(msg + 12, h);
    put32(msg + 16, 0);                          /* states: array of length 0  */
    wl_send(c, msg, 20, NULL);
}

/* Emit xdg_surface.configure(serial) — the client must ack_configure(serial). */
static void send_xdg_surface_configure(struct wl_conn* c, uint32_t xs, uint32_t serial) {
    uint8_t msg[12];
    put32(msg + 0, xs);
    put32(msg + 4, (12u << 16) | XDG_SURFACE_EVT_CONFIGURE);
    put32(msg + 8, serial);
    wl_send(c, msg, 12, NULL);
}

/* Describe the one output we have: geometry + current mode + scale + done.
 *
 * This is not optional decoration.  Every real toolkit (SDL, GTK, Qt) walks the
 * registry looking for a wl_output and refuses to open a window without one —
 * it needs the size and scale before it can lay anything out.  A compositor
 * that advertises none simply looks broken to them.
 *
 * `done` is what tells the client the burst of property events is complete. */
static void send_output_info(struct wl_conn* c, uint32_t out) {
    int sw = gui_screen_w(), sh = gui_screen_h();
    if (sw <= 0) sw = 1280;
    if (sh <= 0) sh = 800;

    /* geometry(x, y, phys_w_mm, phys_h_mm, subpixel, make, model, transform).
     * The two strings are wire strings: u32 length (including the NUL) then the
     * bytes, padded to 4. */
    static const char mk[] = "d-os";
    static const char md[] = "virtual";
    uint32_t mkl = (uint32_t)sizeof mk, mdl = (uint32_t)sizeof md;
    uint32_t mkp = align4(mkl), mdp = align4(mdl);
    uint32_t size = 8 + 4 * 5 + 4 + mkp + 4 + mdp + 4;
    uint8_t msg[96];
    if (size <= sizeof msg) {
        uint32_t o = 0;
        put32(msg + o, out); o += 4;
        put32(msg + o, (size << 16) | WL_OUTPUT_EVT_GEOMETRY); o += 4;
        put32(msg + o, 0); o += 4;                   /* x                     */
        put32(msg + o, 0); o += 4;                   /* y                     */
        put32(msg + o, (uint32_t)(sw / 4)); o += 4;  /* physical width  (mm)  */
        put32(msg + o, (uint32_t)(sh / 4)); o += 4;  /* physical height (mm)  */
        put32(msg + o, 0); o += 4;                   /* subpixel: unknown     */
        put32(msg + o, mkl); o += 4;
        for (uint32_t i = 0; i < mkp; i++) msg[o + i] = (i < mkl) ? (uint8_t)mk[i] : 0;
        o += mkp;
        put32(msg + o, mdl); o += 4;
        for (uint32_t i = 0; i < mdp; i++) msg[o + i] = (i < mdl) ? (uint8_t)md[i] : 0;
        o += mdp;
        put32(msg + o, 0); o += 4;                   /* transform: normal     */
        wl_send(c, msg, o, NULL);
    }

    /* mode(flags, width, height, refresh_mHz) */
    uint8_t m[24];
    put32(m + 0, out);
    put32(m + 4, (24u << 16) | WL_OUTPUT_EVT_MODE);
    put32(m + 8, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED);
    put32(m + 12, (uint32_t)sw);
    put32(m + 16, (uint32_t)sh);
    put32(m + 20, 60000);                            /* 60 Hz in mHz          */
    wl_send(c, m, 24, NULL);

    uint8_t sc[12];
    put32(sc + 0, out);
    put32(sc + 4, (12u << 16) | WL_OUTPUT_EVT_SCALE);
    put32(sc + 8, 1);
    wl_send(c, sc, 12, NULL);

    uint8_t dn[8];
    put32(dn + 0, out);
    put32(dn + 4, (8u << 16) | WL_OUTPUT_EVT_DONE);
    wl_send(c, dn, 8, NULL);

    kprintf("wayland: wl_output -> %dx%d @60Hz scale 1\n", sw, sh);
}

/* Emit wl_seat.capabilities(caps) — a bitmask of WL_SEAT_CAP_*. */
static void send_seat_capabilities(struct wl_conn* c, uint32_t seat, uint32_t caps) {
    uint8_t msg[12];
    put32(msg + 0, seat);
    put32(msg + 4, (12u << 16) | WL_SEAT_EVT_CAPABILITIES);
    put32(msg + 8, caps);
    wl_send(c, msg, 12, NULL);
}

/* ---- focus: the events a toolkit waits for before it believes any input -----
 *
 * A real client does not act on `motion` or `key` at all until it has been told
 * the surface has focus.  SDL, GTK and Qt all drop input that arrives without a
 * preceding `enter`, because on a real compositor that is what "the pointer is
 * over someone else's window" looks like.  §M26 got away without them only
 * because its demo client had no such logic.
 *
 * We have exactly one surface per connection and it owns the window, so focus
 * follows the window: send `enter` once the client has both a surface and the
 * device object, and never take it away.
 * --------------------------------------------------------------------------- */
static void send_pointer_enter(struct wl_conn* c) {
    if (!c->pointer_id || !c->surface_id || c->ptr_entered) return;
    uint8_t msg[24];
    put32(msg + 0, c->pointer_id);
    put32(msg + 4, (24u << 16) | WL_POINTER_EVT_ENTER);
    put32(msg + 8, ++c->serial);
    put32(msg + 12, c->surface_id);
    put32(msg + 16, 0);                       /* surface_x, 24.8 fixed */
    put32(msg + 20, 0);                       /* surface_y             */
    wl_send(c, msg, 24, NULL);
    c->ptr_entered = 1;
}

/* wl_pointer.frame — v5 groups related events; a client that binds v5+ waits
 * for it before applying anything it has received. */
static void send_pointer_frame(struct wl_conn* c) {
    if (!c->pointer_id) return;
    uint8_t msg[8];
    put32(msg + 0, c->pointer_id);
    put32(msg + 4, (8u << 16) | WL_POINTER_EVT_FRAME);
    wl_send(c, msg, 8, NULL);
}

/* wl_keyboard.keymap(format, fd, size) — the keymap travels as a DESCRIPTOR
 * (SCM_RIGHTS), which is the whole reason the shm + fd-passing work had to come
 * first.  Sent once, immediately after the client asks for the keyboard: a
 * toolkit that gets `key` events before it has a keymap cannot translate them
 * and will simply discard them. */
static void send_keyboard_keymap(struct wl_conn* c) {
    if (!c->keyboard_id || c->keymap_sent) return;
    uint32_t size = 0;
    struct ofile* km = wl_keymap_make(&size);
    if (!km) return;
    uint8_t msg[16];
    put32(msg + 0, c->keyboard_id);
    put32(msg + 4, (16u << 16) | WL_KEYBOARD_EVT_KEYMAP);
    put32(msg + 8, 1);                    /* format: XKB_V1                   */
    put32(msg + 12, size);
    wl_send(c, msg, 16, km);     /* the fd rides the same message    */
    ofile_unref(km);                      /* the travelling reference is sent */
    c->keymap_sent = 1;
}

static void send_keyboard_enter(struct wl_conn* c) {
    if (!c->keyboard_id || !c->surface_id || c->kbd_entered) return;
    uint8_t msg[20];
    put32(msg + 0, c->keyboard_id);
    put32(msg + 4, (20u << 16) | WL_KEYBOARD_EVT_ENTER);
    put32(msg + 8, ++c->serial);
    put32(msg + 12, c->surface_id);
    put32(msg + 16, 0);                       /* keys: array of length 0 */
    wl_send(c, msg, 20, NULL);
    /* modifiers must follow enter — a client that never sees one assumes an
     * unknown modifier state and may ignore keys. */
    /* modifiers(serial, depressed, latched, locked, group) — FIVE uints, so 28
     * bytes.  Getting the size wrong is not silently tolerated: libwayland
     * reports "message too short" and drops the connection, which is exactly
     * the strictness a real client brings and our own demo client never did. */
    uint8_t m[28];
    put32(m + 0, c->keyboard_id);
    put32(m + 4, (28u << 16) | WL_KEYBOARD_EVT_MODIFIERS);
    put32(m + 8, ++c->serial);
    put32(m + 12, 0); put32(m + 16, 0); put32(m + 20, 0); put32(m + 24, 0);
    wl_send(c, m, 28, NULL);
    c->kbd_entered = 1;
}

/* Emit wl_keyboard.key(serial, time, key, state). */
void wl_send_key(struct wl_conn* c, uint32_t key, int pressed) {
    if (!c->keyboard_id) return;
    send_keyboard_enter(c);                   /* focus first, always */
    uint8_t msg[24];
    put32(msg + 0, c->keyboard_id);
    put32(msg + 4, (24u << 16) | WL_KEYBOARD_EVT_KEY);
    put32(msg + 8, ++c->serial);                 /* serial */
    put32(msg + 12, 0);                          /* time   */
    put32(msg + 16, key);
    put32(msg + 20, pressed ? WL_KEY_PRESSED : WL_KEY_RELEASED);
    wl_send(c, msg, 24, NULL);
}

/* Emit wl_pointer.motion(time, x, y) — coordinates are 24.8 fixed-point. */
void wl_send_motion(struct wl_conn* c, int x, int y) {
    if (!c->pointer_id) return;
    send_pointer_enter(c);                    /* focus first, always */
    uint8_t msg[20];
    put32(msg + 0, c->pointer_id);
    put32(msg + 4, (20u << 16) | WL_POINTER_EVT_MOTION);
    put32(msg + 8, 0);                            /* time            */
    put32(msg + 12, (uint32_t)(x << 8));          /* surface_x fixed */
    put32(msg + 16, (uint32_t)(y << 8));          /* surface_y fixed */
    wl_send(c, msg, 20, NULL);
    send_pointer_frame(c);
}

/* ---- connection + dispatch ----------------------------------------------- */

void wl_conn_init(struct wl_conn* c, struct usock* sock) {
    for (int i = 0; i < WL_MAX_OBJECTS; i++) c->obj_iface[i] = WLI_NONE;
    c->sock = sock;
    c->obj_iface[WL_DISPLAY_ID] = WLI_DISPLAY;   /* object 1 is always wl_display */
    c->registry_id = 0;
    c->serial = 0;
    c->pool_shm = NULL;
    c->surface_id = c->buffer_id = 0;
    c->buf_off = c->buf_w = c->buf_h = c->buf_stride = 0;
    c->xdg_surface_id = c->xdg_toplevel_id = 0;
    c->pointer_id = c->keyboard_id = 0;
    c->ptr_entered = c->kbd_entered = 0;
    c->keymap_sent = 0;
    c->target = NULL;
    c->blit_x = c->blit_y = 0;
    c->window = NULL;
    c->wm_mode = 0;
    c->title[0] = '\0';
    c->frame_cb = 0;
    c->stalled = 0; c->dropped = 0;
    c->ddev_id = 0;
    for (int i = 0; i < WL_MAX_SOURCES; i++) c->src[i].id = 0;
    c->sel_src = c->sel_set_gen = c->sel_seen_gen = 0;
    c->offer_id = c->offer_gen = 0;
    c->next_srv_id = WL_SERVER_ID_BASE;
}

/* §M26 — forward a WM-managed window's input to the Wayland client (wl_seat).
 * Registered as the window's input hook when wm_mode creates it. */
static void wl_window_input(struct gui_window* win, const struct gui_input* gi, void* ctx) {
    (void)win;
    struct wl_conn* c = (struct wl_conn*)ctx;
    /* keycode 0 = the character-only companion event (see gui.h `ch`): a
     * Wayland client gets scancodes and builds characters from its own xkb
     * keymap, so it has no use for this one and must not see a key 0. */
    if (gi->type == GUI_INPUT_KEY && gi->keycode)
        wl_send_key(c, (uint32_t)gi->keycode, gi->pressed);
    else if (gi->type == GUI_INPUT_MOTION) wl_send_motion(c, gi->x, gi->y);
}

/* wl_surface.commit — the moment the client's frame becomes current.  Read the
 * attached wl_buffer's pixels out of the wl_shm_pool's frames (via the kernel
 * identity map, like fd.c) and log proof they crossed the wire + the SCM_RIGHTS
 * fd passing intact.  Stage 3 gfx_blits these into a gui_window + gui_damage. */
static void wl_surface_commit(struct wl_conn* c) {
    struct shm* s = c->pool_shm;
    if (!s || !c->buf_w || !c->buf_h) { kprintf("wayland: commit with no buffer\n"); return; }

    struct gfx_surface* t = c->target;      /* NULL = headless (read/log only) */
    uint32_t topleft = 0, sum = 0;
    for (uint32_t y = 0; y < c->buf_h; y++) {
        for (uint32_t x = 0; x < c->buf_w; x++) {
            uint32_t bo = c->buf_off + y * c->buf_stride + x * 4;
            /* §M90 — through shm_kptr: frames are PHYSICAL addresses, reachable
             * only through the direct map (aarch64 has no RAM identity map). */
            volatile uint32_t* pp = (volatile uint32_t*)shm_kptr(s, bo);
            if (!pp) continue;
            uint32_t px = *pp;
            if (x == 0 && y == 0) topleft = px;
            sum += px;
            /* Bridge: paint the client's pixel onto the target (framebuffer /
             * a gui_window surface) — the surface becomes visible. */
            if (t) {
                int dx = c->blit_x + (int)x, dy = c->blit_y + (int)y;
                if (dx >= 0 && dx < t->w && dy >= 0 && dy < t->h)
                    t->px[dy * t->stride + dx] = px;
            }
        }
    }
    /* Server-per-surface: a top-level becomes a real desktop window the moment
     * it first has CONTENT — not at get_toplevel, because only now do we know
     * the size the client chose (our configure says 0x0 = "you pick").  Sizing
     * the window to the buffer is what makes an application look right instead
     * of a fixed placeholder rectangle with the pixels in one corner. */
    if (c->wm_mode && !c->window && c->buf_w && c->buf_h) {
        int ow = 0, oh = 0;
        gui_window_outer_for_content((int)c->buf_w, (int)c->buf_h, &ow, &oh);
        c->window = gui_app_window_create(c->title[0] ? c->title : "Wayland client",
                                          340, 220, ow, oh, NULL, NULL);
        if (c->window) {
            gui_window_set_input_hook(c->window, wl_window_input, c);
            kprintf("wayland: created a %dx%d WM window for the surface\n",
                    (int)c->buf_w, (int)c->buf_h);
        }
    }

    /* WM-managed window target: the buffer becomes the window's contents.
     * §M90 — gathered into one contiguous copy first: the pool's frames are
     * NOT contiguous, and blitting from frames[0] as if they were read the
     * neighbouring physical memory for every buffer larger than a page (it
     * looked right only while the allocator happened to hand out consecutive
     * frames). */
    if (c->window && s->nframes >= 1) {
        size_t row = (size_t)c->buf_w * 4, total = row * c->buf_h;
        uint32_t* tmp = (uint32_t*)kmalloc(total);
        if (tmp) {
            for (uint32_t y = 0; y < c->buf_h; y++)
                shm_read(s, (uint64_t)c->buf_off + (uint64_t)y * c->buf_stride,
                         (uint8_t*)tmp + y * row, row);
            gui_window_blit(c->window, 0, 0, tmp, (int)c->buf_w, (int)c->buf_h, (int)c->buf_w);
            kfree(tmp);
        }
    }

    kprintf("wayland: COMMIT surface %u: %ux%u buffer, top-left=%x checksum=%x%s\n",
            c->surface_id, c->buf_w, c->buf_h, topleft, sum,
            c->window ? " (blitted to window)" : t ? " (blitted to screen)" : "");
    send_buffer_release(c, c->buffer_id);

    /* The frame the client asked about is now on screen — release its callback
     * (and delete the object, as the protocol requires: a wl_callback is
     * single-shot). */
    if (c->frame_cb) {
        send_callback_done(c, c->frame_cb, (uint32_t)timer_ticks_ms());
        send_delete_id(c, c->frame_cb);
        if (c->frame_cb < WL_MAX_OBJECTS) c->obj_iface[c->frame_cb] = WLI_NONE;
        c->frame_cb = 0;
    }
}

/* Process one request whose 8-byte header has already been read. */
/* ---- §M59 — wl_data_device: the clipboard, over Wayland ---------------------
 *
 * ONE SELECTION PER MACHINE, AND IT IS THE d-os CLIPBOARD.  Each Wayland client
 * here has its own connection served by its own task (there is no shared
 * compositor socket), so "the selection" cannot live in a connection: it lives
 * in clipboard.c, where the terminal's Ctrl+Shift+C, the editor and
 * `/dev/clipboard` already put it.  The bridge has two directions:
 *
 *   IN  (a client copies): set_selection(source) → we send the source a `send`
 *       event carrying the write end of a pipe, read the data out of the read
 *       end (bounded wait) and store it with clipboard_set_typed.  The data is
 *       taken EAGERLY, at set_selection, rather than when somebody pastes:
 *       the consumers are kernel code (a terminal paste, `clip show`) that
 *       cannot wait on a client, and a client that exits after copying — the
 *       ordinary `wl-copy` shape — would otherwise take its selection with it.
 *   OUT (a client pastes): whenever the clipboard generation differs from what
 *       this client was last told, a fresh wl_data_offer is announced
 *       (data_offer + offer(mime)... + selection); `receive(mime, fd)` writes
 *       the clipboard into that fd and closes it.
 *
 * Announcements happen on the connection's OWN task, at the next request it
 * reads (and at get_data_device), because two tasks writing one socket is what
 * the whole-message rule in wl_send exists to survive, not something to add
 * more of.  So a client learns of an outside change when it next talks to us
 * — which any toolkit does on its next frame — not the instant it happens.
 *
 * TEXT IS CONVERTED.  d-os text is ISO-8859-2 (charset.c); the world is UTF-8.
 * A text selection is offered as text/plain;charset=utf-8 (converted), plus
 * UTF8_STRING and text/plain for old clients; incoming UTF-8 is converted back.
 * A non-text type (clipboard_type() not text/...) passes through as raw bytes
 * under its own name.
 *
 * NOT DONE: drag and drop (start_drag is answered with `cancelled`, which a
 * client handles as a drag the compositor refused) and the primary selection
 * (zwp_primary_selection is a separate protocol). */
#define WL_CLIP_MAX (64 * 1024)
#define WL_PIPE_WAIT_MS 2000

static int is_text_type(const char* t) {
    return t[0]=='t' && t[1]=='e' && t[2]=='x' && t[3]=='t' && t[4]=='/';
}
static int str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int mime_is_utf8(const char* m) {
    return str_eq(m, "text/plain;charset=utf-8") || str_eq(m, "UTF8_STRING");
}
static int mime_is_text(const char* m) {
    return mime_is_utf8(m) || str_eq(m, "text/plain") || str_eq(m, "TEXT") ||
           str_eq(m, "STRING");
}

/* One event carrying a single string argument (offer / send share the shape). */
static void send_string_event(struct wl_conn* c, uint32_t obj, uint32_t op,
                              const char* str, struct ofile* pf) {
    uint8_t msg[8 + 4 + 64];
    uint32_t slen = cstrlen(str) + 1;
    if (slen > 64) slen = 64;
    uint32_t size = 8 + 4 + align4(slen);
    put32(msg + 0, obj);
    put32(msg + 4, (size << 16) | op);
    put32(msg + 8, slen);
    for (uint32_t i = 0; i < align4(slen); i++)
        msg[12 + i] = i < slen - 1 ? (uint8_t)str[i] : 0;
    wl_send(c, msg, size, pf);
}
static void send_u32_event(struct wl_conn* c, uint32_t obj, uint32_t op, uint32_t v) {
    uint8_t msg[12];
    put32(msg + 0, obj);
    put32(msg + 4, (12u << 16) | op);
    put32(msg + 8, v);
    wl_send(c, msg, 12, NULL);
}

/* Tell the client what is on the clipboard now (and, if the client's own
 * source was replaced by something else, that it no longer owns it). */
static void dd_announce(struct wl_conn* c) {
    uint32_t gen = clipboard_gen();
    c->sel_seen_gen = gen;
    if (c->sel_src && gen != c->sel_set_gen) {
        send_u32_event(c, c->sel_src, WL_DATA_SOURCE_EVT_CANCELLED, 0);
        c->sel_src = 0;
    }
    if (!c->ddev_id) return;
    if (clipboard_len() == 0) {                       /* nothing to paste */
        send_u32_event(c, c->ddev_id, WL_DATA_DEVICE_EVT_SELECTION, 0);
        c->offer_id = 0;
        return;
    }
    char type[64];
    char dummy[1];
    clipboard_get_typed(dummy, 1, type, sizeof type);
    uint32_t id = c->next_srv_id++;
    c->offer_id = id; c->offer_gen = gen;
    send_u32_event(c, c->ddev_id, WL_DATA_DEVICE_EVT_DATA_OFFER, id);
    if (is_text_type(type)) {
        send_string_event(c, id, WL_DATA_OFFER_EVT_OFFER, "text/plain;charset=utf-8", NULL);
        send_string_event(c, id, WL_DATA_OFFER_EVT_OFFER, "UTF8_STRING", NULL);
        send_string_event(c, id, WL_DATA_OFFER_EVT_OFFER, "text/plain", NULL);
    } else {
        send_string_event(c, id, WL_DATA_OFFER_EVT_OFFER, type, NULL);
    }
    send_u32_event(c, c->ddev_id, WL_DATA_DEVICE_EVT_SELECTION, id);
}

/* Write all of buf into a pipe the client handed us (bounded: a client that
 * never reads its paste must not hold this connection forever). */
static void pipe_write_all(struct usock* s, const char* buf, int n) {
    uint64_t deadline = timer_ticks_ms() + WL_PIPE_WAIT_MS;
    int off = 0;
    while (off < n) {
        long w = usock_send(s, buf + off, (size_t)(n - off), NULL);
        if (w < 0) return;                                /* reader gone */
        off += (int)w;
        if (off < n) {
            if (timer_ticks_ms() >= deadline) return;
            task_msleep(1);
        }
    }
}

/* offer.receive(mime, fd): serve the clipboard into the client's pipe. */
static void dd_receive(struct wl_conn* c, uint32_t offer, const char* mime,
                       struct ofile* pf) {
    if (!pf) { kprintf("wayland: receive without a descriptor\n"); return; }
    if (pf->kind != FD_SOCK) { ofile_unref(pf); return; }
    int n = 0, out = 0;
    char* raw = (char*)kmalloc(WL_CLIP_MAX + 1);
    char* conv = raw ? (char*)kmalloc(WL_CLIP_MAX * 3 / 2 + 4) : NULL;
    char type[64];
    /* A STALE offer (the clipboard has changed since it was announced) gets
     * nothing: serving the new contents under the old offer would paste
     * something the user did not choose at the moment they chose to paste. */
    if (raw && conv && offer == c->offer_id && c->offer_gen == clipboard_gen()) {
        n = clipboard_get_typed(raw, WL_CLIP_MAX + 1, type, sizeof type);
        if (is_text_type(type) && mime_is_utf8(mime)) {
            out = charset_latin2_to_utf8(raw, n, conv, WL_CLIP_MAX * 3 / 2 + 4);
            pipe_write_all(pf->sock, conv, out);
        } else {
            out = n;
            pipe_write_all(pf->sock, raw, n);
        }
        kprintf("wayland: paste served - %d bytes as %s\n", out, mime);
    } else {
        kprintf("wayland: paste refused - offer %x is stale\n", offer);
    }
    if (raw) kfree(raw);
    if (conv) kfree(conv);
    ofile_unref(pf);                          /* our end closed: the client sees EOF */
}

/* set_selection(source): take the client's data now (see the header). */
static void dd_take_selection(struct wl_conn* c, uint32_t source) {
    int si = -1;
    for (int i = 0; i < WL_MAX_SOURCES; i++) if (c->src[i].id == source) si = i;
    if (si < 0 || c->src[si].nmime == 0) {
        kprintf("wayland: set_selection with an unknown or empty source %u\n", source);
        return;
    }
    /* Prefer UTF-8 text, then any text, then whatever came first. */
    int pick = -1;
    for (int k = 0; k < c->src[si].nmime && pick < 0; k++)
        if (mime_is_utf8(c->src[si].mime[k])) pick = k;
    for (int k = 0; k < c->src[si].nmime && pick < 0; k++)
        if (mime_is_text(c->src[si].mime[k])) pick = k;
    if (pick < 0) pick = 0;
    const char* mime = c->src[si].mime[pick];

    struct usock *rd, *wr;
    if (usock_pair(&rd, &wr) != 0) return;
    struct ofile* wr_of = ofile_from_sock(wr);
    if (!wr_of) { usock_close(rd); usock_close(wr); return; }
    send_string_event(c, source, WL_DATA_SOURCE_EVT_SEND, mime, wr_of);
    ofile_unref(wr_of);                      /* only the client's reference remains */

    char* buf = (char*)kmalloc(WL_CLIP_MAX);
    int n = 0, complete = 0;
    uint64_t deadline = timer_ticks_ms() + WL_PIPE_WAIT_MS;
    while (buf) {
        long r = usock_recv(rd, buf + n, (size_t)(WL_CLIP_MAX - n), 0, NULL);
        if (r > 0) { n += (int)r; if (n >= WL_CLIP_MAX) { complete = 1; break; } continue; }
        if (!usock_peer_open(rd)) { complete = 1; break; }   /* writer closed: EOF */
        if (timer_ticks_ms() >= deadline) break;
        task_msleep(1);
    }
    usock_close(rd);
    if (!buf) return;
    if (!complete)
        kprintf("wayland: copy timed out after %d bytes - the client never closed its end\n", n);
    if (mime_is_text(mime)) {
        char* l2 = (char*)kmalloc((size_t)n + 1);
        int m = 0;
        if (l2 && mime_is_utf8(mime)) m = charset_utf8_to_latin2(buf, n, l2, n);
        else if (l2) for (; m < n; m++) l2[m] = buf[m];
        if (l2) { clipboard_set_typed(l2, m, "text/plain"); kfree(l2); }
    } else {
        clipboard_set_typed(buf, n, mime);
    }
    kfree(buf);
    c->sel_src = source;
    c->sel_set_gen = clipboard_gen();
    kprintf("wayland: copy taken - %d bytes as %s\n", n, mime);
}

static int wl_process(struct wl_conn* c, const uint8_t* hdr) {
    uint32_t obj  = get32(hdr);
    uint32_t w2   = get32(hdr + 4);
    uint32_t size = w2 >> 16;
    uint32_t op   = w2 & 0xffffu;

    uint8_t body[256];
    uint32_t blen = (size >= 8) ? (size - 8) : 0;
    if (blen > sizeof body) blen = sizeof body;
    if (blen && recv_exact(c->sock, body, (long)blen) != (long)blen) return -1;

    uint8_t iface = (obj < WL_MAX_OBJECTS) ? c->obj_iface[obj] : WLI_NONE;

    /* §M59 — the clipboard changed since this client was last told. */
    if (c->ddev_id && clipboard_gen() != c->sel_seen_gen) dd_announce(c);

    if (iface == WLI_DISPLAY && op == WL_DISPLAY_REQ_GET_REGISTRY && blen >= 4) {
        uint32_t reg = get32(body);
        if (reg < WL_MAX_OBJECTS) c->obj_iface[reg] = WLI_REGISTRY;
        c->registry_id = reg;
        kprintf("wayland: get_registry(id=%u) -> advertising %d globals\n", reg, WL_NGLOBALS);
        for (int i = 0; i < WL_NGLOBALS; i++) send_global(c, &g_globals[i]);

    } else if (iface == WLI_DISPLAY && op == WL_DISPLAY_REQ_SYNC && blen >= 4) {
        uint32_t cb = get32(body);
        kprintf("wayland: sync(callback=%u) -> done + delete_id\n", cb);
        send_callback_done(c, cb, c->serial++);
        send_delete_id(c, cb);

    } else if (iface == WLI_REGISTRY && op == WL_REGISTRY_REQ_BIND && blen >= 12) {
        /* bind(name:uint, interface:string, version:uint, id:new_id). */
        uint32_t name = get32(body);
        uint32_t slen = get32(body + 4);
        uint32_t o = 8 + align4(slen);
        uint32_t new_id = get32(body + o + 4);           /* after version */
        uint8_t bi = (name == 1) ? WLI_COMPOSITOR :
                     (name == 2) ? WLI_SHM :
                     (name == 3) ? WLI_XDG_WM_BASE :
                     (name == 4) ? WLI_SEAT :
                     (name == 5) ? WLI_OUTPUT :
                     (name == 6) ? WLI_DATA_DEVICE_MANAGER : WLI_NONE;
        if (new_id < WL_MAX_OBJECTS) c->obj_iface[new_id] = bi;
        kprintf("wayland: bind(name=%u) -> object %u\n", name, new_id);
        if (bi == WLI_SHM) {                             /* advertise formats */
            send_shm_format(c, new_id, WL_SHM_FORMAT_ARGB8888);
            send_shm_format(c, new_id, WL_SHM_FORMAT_XRGB8888);
        } else if (bi == WLI_OUTPUT) {                   /* describe the screen */
            send_output_info(c, new_id);
        } else if (bi == WLI_SEAT) {                     /* advertise input caps */
            send_seat_capabilities(c, new_id, WL_SEAT_CAP_POINTER | WL_SEAT_CAP_KEYBOARD);
        }

    } else if (iface == WLI_COMPOSITOR && op == WL_COMPOSITOR_REQ_CREATE_SURFACE && blen >= 4) {
        uint32_t nid = get32(body);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_SURFACE;
        c->surface_id = nid;
        kprintf("wayland: create_surface -> object %u\n", nid);

    } else if (iface == WLI_SHM && op == WL_SHM_REQ_CREATE_POOL && blen >= 8) {
        /* create_pool(id:new_id, fd, size) — fd is out-of-band (no wire word). */
        uint32_t nid  = get32(body);
        uint32_t psize = get32(body + 4);
        struct ofile* pf = NULL; uint8_t d;
        usock_recv(c->sock, &d, 0, 0, &pf);              /* dequeue the passed fd */
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_SHM_POOL;
        c->pool_shm = (pf && pf->kind == FD_SHM) ? pf->shm : NULL;
        kprintf("wayland: create_pool(id=%u,size=%u) shm-fd=%s\n",
                nid, psize, c->pool_shm ? "received" : "MISSING");

    } else if (iface == WLI_SHM_POOL && op == WL_SHM_POOL_REQ_CREATE_BUFFER && blen >= 24) {
        /* create_buffer(id, offset, width, height, stride, format). */
        uint32_t nid = get32(body);
        c->buffer_id = nid;
        c->buf_off = get32(body + 4);  c->buf_w = get32(body + 8);
        c->buf_h   = get32(body + 12); c->buf_stride = get32(body + 16);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_BUFFER;
        kprintf("wayland: create_buffer(id=%u) %ux%u stride=%u\n",
                nid, c->buf_w, c->buf_h, c->buf_stride);

    } else if (iface == WLI_SURFACE && op == WL_SURFACE_REQ_ATTACH && blen >= 4) {
        kprintf("wayland: surface %u attach buffer %u\n", obj, get32(body));

    } else if (iface == WLI_XDG_TOPLEVEL && op == XDG_TOPLEVEL_REQ_SET_APP_ID) {
        /* set_app_id — the desktop-file identity.  We have no application
         * database to look it up in, but every real toolkit sends it and an
         * unanswered request is a protocol error to a strict client. */

    } else if (iface == WLI_SHM_POOL && op == WL_SHM_POOL_REQ_DESTROY) {
        /* pool.destroy — the client is done with the POOL, but any wl_buffer
         * carved out of it stays valid, so the frames must NOT be released
         * here.  weston-simple-shm destroys its pool immediately after creating
         * its buffers and then draws from them for the rest of its life. */

    } else if (iface == WLI_SURFACE && op == WL_SURFACE_REQ_FRAME && blen >= 4) {
        /* frame(callback) — "tell me when it is a good time to draw the next
         * frame".  Every toolkit's render loop requests one and then BLOCKS
         * until it fires, so a compositor that ignores this does not merely
         * lose throttling: the application stops drawing entirely.  We answer
         * on the next commit, which is the moment the current frame became
         * visible. */
        uint32_t cb = get32(body);
        if (cb < WL_MAX_OBJECTS) c->obj_iface[cb] = WLI_CALLBACK;
        c->frame_cb = cb;

    } else if (iface == WLI_SURFACE &&
               (op == WL_SURFACE_REQ_DAMAGE ||
                op == WL_SURFACE_REQ_DAMAGE_BUFFER) && blen >= 16) {
        /* damage(x, y, width, height) — the client marking which part of the
         * surface changed.  We always recomposite the whole surface on commit,
         * so the rect is advisory; ACCEPTING it is what matters, because every
         * real toolkit sends it and an "unhandled" reply is a protocol error to
         * a strict client. */
        (void)get32(body); (void)get32(body + 4);
        (void)get32(body + 8); (void)get32(body + 12);

    } else if (iface == WLI_SURFACE && op == WL_SURFACE_REQ_COMMIT) {
        wl_surface_commit(c);

    } else if (iface == WLI_XDG_WM_BASE && op == XDG_WM_BASE_REQ_GET_XDG_SURFACE && blen >= 8) {
        /* get_xdg_surface(new_id, wl_surface). */
        uint32_t nid = get32(body);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_XDG_SURFACE;
        c->xdg_surface_id = nid;
        kprintf("wayland: get_xdg_surface -> object %u (surface %u)\n", nid, get32(body + 4));

    } else if (iface == WLI_XDG_SURFACE && op == XDG_SURFACE_REQ_GET_TOPLEVEL && blen >= 4) {
        /* get_toplevel(new_id) → the window becomes a top-level; send the
         * initial configure pair (toplevel size + surface serial to ack). */
        uint32_t nid = get32(body);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_XDG_TOPLEVEL;
        c->xdg_toplevel_id = nid;
        kprintf("wayland: get_toplevel -> object %u; sending configure\n", nid);
        /* Server-per-surface: a top-level maps to a real desktop window.  Its
         * committed buffers become the window's contents (wl_conn.window) and
         * its input is routed to this client's wl_seat (input hook). */
        send_xdg_toplevel_configure(c, nid, 0, 0);       /* client picks a size */
        send_xdg_surface_configure(c, c->xdg_surface_id, ++c->serial);

    } else if (iface == WLI_XDG_TOPLEVEL && op == XDG_TOPLEVEL_REQ_SET_TITLE && blen >= 4) {
        uint32_t slen = get32(body);
        char t[64]; uint32_t k = 0;
        for (; k + 1 < slen && k < sizeof t - 1; k++) t[k] = (char)body[4 + k];
        t[k] = 0;
        kprintf("wayland: xdg_toplevel set_title(\"%s\")\n", t);
        /* A real client titles its window AFTER the role is assigned, so the
         * window already exists under a placeholder name — retitle it, or the
         * title bar keeps saying "Wayland client" for every application. */
        {   uint32_t i = 0;
            for (; t[i] && i < sizeof c->title - 1; i++) c->title[i] = t[i];
            c->title[i] = '\0';
        }
        if (c->window && t[0]) gui_window_set_title(c->window, t);

    } else if (iface == WLI_XDG_SURFACE && op == XDG_SURFACE_REQ_ACK_CONFIGURE && blen >= 4) {
        kprintf("wayland: xdg_surface ack_configure(serial=%u)\n", get32(body));

    } else if (iface == WLI_SEAT && op == WL_SEAT_REQ_GET_POINTER && blen >= 4) {
        uint32_t nid = get32(body);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_POINTER;
        c->pointer_id = nid;
        kprintf("wayland: get_pointer -> object %u\n", nid);

    } else if (iface == WLI_SEAT && op == WL_SEAT_REQ_GET_KEYBOARD && blen >= 4) {
        uint32_t nid = get32(body);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_KEYBOARD;
        c->keyboard_id = nid;
        kprintf("wayland: get_keyboard -> object %u\n", nid);
        send_keyboard_keymap(c);          /* before any key event can arrive */

    /* ---- §M59 — wl_data_device_manager / source / device / offer ---- */
    } else if (iface == WLI_DATA_DEVICE_MANAGER && op == WL_DDM_REQ_CREATE_DATA_SOURCE && blen >= 4) {
        uint32_t nid = get32(body);
        int slot = -1;
        for (int i = 0; i < WL_MAX_SOURCES; i++) if (!c->src[i].id) { slot = i; break; }
        if (slot < 0) {           /* reuse the oldest that is not the selection */
            for (int i = 0; i < WL_MAX_SOURCES; i++)
                if (c->src[i].id != c->sel_src) { slot = i; break; }
        }
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_DATA_SOURCE;
        if (slot >= 0) { c->src[slot].id = nid; c->src[slot].nmime = 0; }

    } else if (iface == WLI_DATA_DEVICE_MANAGER && op == WL_DDM_REQ_GET_DATA_DEVICE && blen >= 8) {
        uint32_t nid = get32(body);
        if (nid < WL_MAX_OBJECTS) c->obj_iface[nid] = WLI_DATA_DEVICE;
        c->ddev_id = nid;
        kprintf("wayland: data device %u - announcing the clipboard\n", nid);
        dd_announce(c);

    } else if (iface == WLI_DATA_SOURCE && op == WL_DATA_SOURCE_REQ_OFFER && blen >= 4) {
        uint32_t slen = get32(body);
        for (int i = 0; i < WL_MAX_SOURCES; i++) {
            if (c->src[i].id != obj || c->src[i].nmime >= WL_MAX_MIMES) continue;
            char* d = c->src[i].mime[c->src[i].nmime++];
            uint32_t k = 0;
            for (; k + 1 < 48 && k + 1 < slen && 4 + k < blen && body[4 + k]; k++)
                d[k] = (char)body[4 + k];
            d[k] = 0;
        }

    } else if (iface == WLI_DATA_SOURCE && op == WL_DATA_SOURCE_REQ_DESTROY) {
        for (int i = 0; i < WL_MAX_SOURCES; i++) if (c->src[i].id == obj) c->src[i].id = 0;
        if (c->sel_src == obj) c->sel_src = 0;
        c->obj_iface[obj] = WLI_NONE;
        send_delete_id(c, obj);

    } else if (iface == WLI_DATA_SOURCE && op == WL_DATA_SOURCE_REQ_SET_ACTIONS) {
        /* drag-and-drop actions: no drag support, nothing to record */

    } else if (iface == WLI_DATA_DEVICE && op == WL_DATA_DEVICE_REQ_SET_SELECTION && blen >= 8) {
        uint32_t source = get32(body);
        if (source) dd_take_selection(c, source);
        else kprintf("wayland: selection cleared by the client (the clipboard keeps its contents)\n");
        dd_announce(c);                   /* the client sees its own selection too */

    } else if (iface == WLI_DATA_DEVICE && op == WL_DATA_DEVICE_REQ_START_DRAG && blen >= 16) {
        uint32_t source = get32(body);
        kprintf("wayland: drag and drop is not supported - drag cancelled\n");
        if (source) send_u32_event(c, source, WL_DATA_SOURCE_EVT_CANCELLED, 0);

    } else if (iface == WLI_DATA_DEVICE && op == WL_DATA_DEVICE_REQ_RELEASE) {
        if (c->ddev_id == obj) c->ddev_id = 0;
        c->obj_iface[obj] = WLI_NONE;
        send_delete_id(c, obj);

    } else if (obj >= WL_SERVER_ID_BASE && op == WL_DATA_OFFER_REQ_RECEIVE && blen >= 4) {
        /* receive(mime_type, fd) — the fd travels out of band. */
        uint32_t slen = get32(body);
        char mime[48];
        uint32_t k = 0;
        for (; k + 1 < sizeof mime && k + 1 < slen && 4 + k < blen && body[4 + k]; k++)
            mime[k] = (char)body[4 + k];
        mime[k] = 0;
        struct ofile* pf = NULL; uint8_t d;
        usock_recv(c->sock, &d, 0, 0, &pf);
        dd_receive(c, obj, mime, pf);

    } else if (obj >= WL_SERVER_ID_BASE &&
               (op == WL_DATA_OFFER_REQ_ACCEPT || op == WL_DATA_OFFER_REQ_DESTROY ||
                op == WL_DATA_OFFER_REQ_FINISH || op == WL_DATA_OFFER_REQ_SET_ACTIONS)) {
        /* accept/finish/set_actions are drag-and-drop; destroy of a server-
         * created object needs no delete_id (that is for client ids). */

    } else {
        kprintf("wayland: object %u (iface %u) opcode %u - unhandled\n", obj, iface, op);
    }
    return 1;
}

int wl_conn_dispatch(struct wl_conn* c) {
    if (!usock_can_read(c->sock)) return 0;         /* non-blocking snapshot */
    uint8_t hdr[8];
    if (recv_exact(c->sock, hdr, 8) != 8) return -1;
    return wl_process(c, hdr);
}

/* Blocking server loop for a dedicated server task: process requests as they
 * arrive (recv_exact blocks) until the client closes the socket. */
void wl_conn_serve(struct wl_conn* c) {
    for (;;) {
        uint8_t hdr[8];
        if (recv_exact(c->sock, hdr, 8) != 8) break;   /* peer closed */
        if (wl_process(c, hdr) < 0) break;
    }
}

/* Server-task entry: the arg is a heap wl_conn (bound to the server socket).
 * Serve the client until it closes, then tear the connection down. */
void wl_server_task(void) {
    struct wl_conn* conn = (struct wl_conn*)task_start_arg();
    if (!conn) return;
    wl_conn_serve(conn);
    usock_close(conn->sock);
    kfree(conn);
}

/* ---- self-test: a hand-marshalled client over a usock_pair ---------------- */

/* The object ids this test client allocates (a real client tracks each object's
 * interface itself; opcode 0 alone is ambiguous — it is both wl_registry.global
 * and wl_callback.done — so the DISPATCH MUST key on the object's interface). */
#define WLC_REGISTRY     2u
#define WLC_CALLBACK     3u
#define WLC_COMPOSITOR   4u
#define WLC_SHM          5u
#define WLC_SURFACE      6u
#define WLC_POOL         7u
#define WLC_BUFFER       8u
#define WLC_XDG_WM_BASE  9u
#define WLC_XDG_SURFACE 10u
#define WLC_XDG_TOPLEVEL 11u
#define WLC_SEAT        12u
#define WLC_POINTER     13u
#define WLC_KEYBOARD    14u

/* The last xdg_surface.configure serial the client saw (to ack_configure). */
static uint32_t g_config_serial = 0;

/* Drain + decode every event the server has queued back to the client end. */
static void client_drain(struct usock* cli) {
    for (;;) {
        if (!usock_can_read(cli)) break;
        uint8_t hdr[8];
        if (recv_exact(cli, hdr, 8) != 8) break;
        uint32_t obj  = get32(hdr);
        uint32_t w2   = get32(hdr + 4);
        uint32_t size = w2 >> 16;
        uint32_t op   = w2 & 0xffffu;
        uint8_t body[256];
        uint32_t blen = (size >= 8) ? (size - 8) : 0;
        if (blen > sizeof body) blen = sizeof body;
        if (blen) recv_exact(cli, body, (long)blen);

        if (obj == WLC_REGISTRY && op == WL_REGISTRY_EVT_GLOBAL) {
            uint32_t name = get32(body);
            uint32_t slen = get32(body + 4);
            char nm[32]; uint32_t k = 0;
            for (; k + 1 < slen && k < sizeof nm - 1; k++) nm[k] = (char)body[8 + k];
            nm[k] = 0;
            uint32_t ver = get32(body + 8 + align4(slen));
            kprintf("  client: global name=%u iface=%s v%u\n", name, nm, ver);
        } else if (obj == WLC_CALLBACK && op == WL_CALLBACK_EVT_DONE) {
            kprintf("  client: callback.done(serial=%u)\n", get32(body));
        } else if (obj == WL_DISPLAY_ID && op == WL_DISPLAY_EVT_DELETE_ID) {
            kprintf("  client: delete_id(%u)\n", get32(body));
        } else if (obj == WLC_SHM && op == WL_SHM_EVT_FORMAT) {
            kprintf("  client: shm format=%u supported\n", get32(body));
        } else if (obj == WLC_BUFFER && op == WL_BUFFER_EVT_RELEASE) {
            kprintf("  client: buffer released\n");
        } else if (obj == WLC_XDG_SURFACE && op == XDG_SURFACE_EVT_CONFIGURE) {
            g_config_serial = get32(body);
            kprintf("  client: xdg_surface.configure(serial=%u)\n", g_config_serial);
        } else if (obj == WLC_XDG_TOPLEVEL && op == XDG_TOPLEVEL_EVT_CONFIGURE) {
            kprintf("  client: xdg_toplevel.configure(%ux%u)\n", get32(body), get32(body + 4));
        } else if (obj == WLC_SEAT && op == WL_SEAT_EVT_CAPABILITIES) {
            uint32_t caps = get32(body);
            kprintf("  client: seat capabilities=%x (%s%s)\n", caps,
                    (caps & WL_SEAT_CAP_POINTER) ? "pointer " : "",
                    (caps & WL_SEAT_CAP_KEYBOARD) ? "keyboard" : "");
        } else if (obj == WLC_POINTER && op == WL_POINTER_EVT_MOTION) {
            kprintf("  client: pointer motion (%d,%d)\n",
                    (int)get32(body + 4) >> 8, (int)get32(body + 8) >> 8);
        } else if (obj == WLC_KEYBOARD && op == WL_KEYBOARD_EVT_KEY) {
            kprintf("  client: key %u %s\n", get32(body + 8),
                    get32(body + 12) == WL_KEY_PRESSED ? "pressed" : "released");
        } else {
            kprintf("  client: event obj=%u op=%u\n", obj, op);
        }
    }
}

/* Marshal a wl_display request with one u32 arg (get_registry / sync). */
static void client_send1(struct usock* cli, uint32_t opcode, uint32_t new_id) {
    uint8_t msg[12];
    put32(msg + 0, WL_DISPLAY_ID);
    put32(msg + 4, (12u << 16) | opcode);
    put32(msg + 8, new_id);
    usock_send(cli, msg, 12, NULL);
}

/* Marshal a generic request: header + n u32 args (+ an optional passed fd). */
static void client_msg(struct usock* cli, uint32_t obj, uint32_t opcode,
                       const uint32_t* args, int n, struct ofile* passfd) {
    uint8_t msg[64];
    uint32_t size = 8 + (uint32_t)n * 4;
    put32(msg + 0, obj);
    put32(msg + 4, (size << 16) | opcode);
    for (int i = 0; i < n; i++) put32(msg + 8 + i * 4, args[i]);
    usock_send(cli, msg, size, passfd);
}

/* Marshal wl_registry.bind(name, interface, version, new_id). */
static void client_bind(struct usock* cli, uint32_t name, const char* iface,
                        uint32_t version, uint32_t new_id) {
    uint8_t msg[64];
    uint32_t slen = cstrlen(iface) + 1;
    uint32_t size = 8 + 4 + 4 + align4(slen) + 4 + 4;
    put32(msg + 0, WLC_REGISTRY);
    put32(msg + 4, (size << 16) | WL_REGISTRY_REQ_BIND);
    put32(msg + 8, name);
    put32(msg + 12, slen);
    uint32_t o = 16;
    for (uint32_t i = 0; i < slen; i++) msg[o + i] = (uint8_t)iface[i];
    for (uint32_t i = slen; i < align4(slen); i++) msg[o + i] = 0;
    o += align4(slen);
    put32(msg + o, version); o += 4;
    put32(msg + o, new_id);
    usock_send(cli, msg, size, NULL);
}

/* Marshal a request with a single string arg (xdg_toplevel.set_title). */
static void client_msg_str(struct usock* cli, uint32_t obj, uint32_t opcode, const char* s) {
    uint8_t msg[80];
    uint32_t slen = cstrlen(s) + 1;
    uint32_t size = 8 + 4 + align4(slen);
    put32(msg + 0, obj);
    put32(msg + 4, (size << 16) | opcode);
    put32(msg + 8, slen);
    uint32_t o = 12;
    for (uint32_t i = 0; i < slen; i++) msg[o + i] = (uint8_t)s[i];
    for (uint32_t i = slen; i < align4(slen); i++) msg[o + i] = 0;
    usock_send(cli, msg, size, NULL);
}

void wl_selftest(void) {
    struct usock *cli, *srv;
    if (usock_pair(&cli, &srv) != 0) { kprintf("waytest: usock_pair failed\n"); return; }

    struct wl_conn conn;
    wl_conn_init(&conn, srv);
    kprintf("waytest: Wayland wire handshake over a unix socket\n");

    /* Stage 1 — get_registry → globals; sync → callback.done + delete_id. */
    client_send1(cli, WL_DISPLAY_REQ_GET_REGISTRY, WLC_REGISTRY);
    wl_conn_dispatch(&conn);
    client_drain(cli);
    client_send1(cli, WL_DISPLAY_REQ_SYNC, WLC_CALLBACK);
    wl_conn_dispatch(&conn);
    client_drain(cli);
    kprintf("waytest: handshake complete\n");

    /* Stage 2 — bind + a shm buffer committed to a surface. -------------------
     * Build a 4x4 ARGB pixel buffer in SHARED memory, hand its fd to the server
     * over the socket (SCM_RIGHTS), and drive create_surface → create_pool →
     * create_buffer → attach → commit; the server reads the pixels back. */
    const uint32_t W = 4, H = 4, STRIDE = W * 4, COLOR = 0x3366CCFFu;
    struct shm* buf = shm_create((size_t)STRIDE * H);
    if (buf) {                                   /* §M90 — through shm_write */
        for (uint32_t i = 0; i < W * H; i++) shm_write(buf, (uint64_t)i * 4, &COLOR, 4);
    }
    struct ofile* buf_of = buf ? ofile_from_shm(buf) : NULL;

    kprintf("waytest: stage 2 - shm buffer -> surface commit (fill=%x)\n", COLOR);
    client_bind(cli, 1, "wl_compositor", 4, WLC_COMPOSITOR);
    client_bind(cli, 2, "wl_shm",        1, WLC_SHM);
    wl_conn_dispatch(&conn); wl_conn_dispatch(&conn);
    client_drain(cli);

    { uint32_t a[] = { WLC_SURFACE };
      client_msg(cli, WLC_COMPOSITOR, WL_COMPOSITOR_REQ_CREATE_SURFACE, a, 1, NULL); }
    wl_conn_dispatch(&conn);

    { uint32_t a[] = { WLC_POOL, STRIDE * H };
      client_msg(cli, WLC_SHM, WL_SHM_REQ_CREATE_POOL, a, 2, buf_of); }
    wl_conn_dispatch(&conn);

    { uint32_t a[] = { WLC_BUFFER, 0, W, H, STRIDE, WL_SHM_FORMAT_ARGB8888 };
      client_msg(cli, WLC_POOL, WL_SHM_POOL_REQ_CREATE_BUFFER, a, 6, NULL); }
    wl_conn_dispatch(&conn);

    { uint32_t a[] = { WLC_BUFFER, 0, 0 };
      client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_ATTACH, a, 3, NULL); }
    wl_conn_dispatch(&conn);

    client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_COMMIT, NULL, 0, NULL);
    wl_conn_dispatch(&conn);
    client_drain(cli);

    kprintf("waytest: done (server should have read top-left=%x)\n", COLOR);

    /* Stage 3 — give the surface an xdg_shell top-level role. -----------------
     * bind xdg_wm_base → get_xdg_surface(surface) → get_toplevel → the server
     * sends the initial configure pair → set_title → ack_configure. */
    kprintf("waytest: stage 3 - xdg_shell top-level role\n");
    client_bind(cli, 3, "xdg_wm_base", 2, WLC_XDG_WM_BASE);
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_XDG_SURFACE, WLC_SURFACE };
      client_msg(cli, WLC_XDG_WM_BASE, XDG_WM_BASE_REQ_GET_XDG_SURFACE, a, 2, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_XDG_TOPLEVEL };
      client_msg(cli, WLC_XDG_SURFACE, XDG_SURFACE_REQ_GET_TOPLEVEL, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    client_drain(cli);                           /* toplevel + surface configure */

    client_msg_str(cli, WLC_XDG_TOPLEVEL, XDG_TOPLEVEL_REQ_SET_TITLE, "d-os window");
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { g_config_serial };
      client_msg(cli, WLC_XDG_SURFACE, XDG_SURFACE_REQ_ACK_CONFIGURE, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    kprintf("waytest: xdg top-level configured, titled + acked\n");

    usock_close(cli);
    usock_close(srv);
}

/* ---- visible demo: a wl_shm buffer painted onto the framebuffer ----------- */

void wl_visible_demo(void) {
    struct gfx_surface fb;
    if (gfx_fb_surface(&fb) != 0) { kprintf("waydemo: no framebuffer (text mode?)\n"); return; }

    const uint32_t W = 32, H = 32, STRIDE = W * 4;
    const int BX = 200, BY = 150;                /* where on screen to paint */

    struct shm* buf = shm_create((size_t)STRIDE * H);
    if (!buf) { kprintf("waydemo: shm alloc failed\n"); return; }
    /* §M90 — written through shm_write (frames are physical, not contiguous) */
    uint32_t topleft = 0;
    for (uint32_t y = 0; y < H; y++)             /* a little gradient */
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v = 0xFF000000u | ((x * 8u) << 16) | ((y * 8u) << 8) | 0x40u;
            shm_write(buf, ((uint64_t)y * W + x) * 4, &v, 4);
            if (!x && !y) topleft = v;
        }
    struct ofile* buf_of = ofile_from_shm(buf);

    struct usock *cli, *srv;
    if (usock_pair(&cli, &srv) != 0) { kprintf("waydemo: usock_pair failed\n"); return; }
    struct wl_conn conn;
    wl_conn_init(&conn, srv);
    conn.target = &fb; conn.blit_x = BX; conn.blit_y = BY;   /* bridge to the FB */

    kprintf("waydemo: committing a %ux%u wl_shm buffer to the framebuffer at (%d,%d)\n",
            W, H, BX, BY);
    client_send1(cli, WL_DISPLAY_REQ_GET_REGISTRY, WLC_REGISTRY);   /* registry first */
    wl_conn_dispatch(&conn);
    client_drain(cli);
    client_bind(cli, 1, "wl_compositor", 4, WLC_COMPOSITOR);
    client_bind(cli, 2, "wl_shm",        1, WLC_SHM);
    wl_conn_dispatch(&conn); wl_conn_dispatch(&conn); client_drain(cli);

    { uint32_t a[] = { WLC_SURFACE };
      client_msg(cli, WLC_COMPOSITOR, WL_COMPOSITOR_REQ_CREATE_SURFACE, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_POOL, STRIDE * H };
      client_msg(cli, WLC_SHM, WL_SHM_REQ_CREATE_POOL, a, 2, buf_of); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_BUFFER, 0, W, H, STRIDE, WL_SHM_FORMAT_ARGB8888 };
      client_msg(cli, WLC_POOL, WL_SHM_POOL_REQ_CREATE_BUFFER, a, 6, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_BUFFER, 0, 0 };
      client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_ATTACH, a, 3, NULL); }
    wl_conn_dispatch(&conn);
    client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_COMMIT, NULL, 0, NULL);
    wl_conn_dispatch(&conn);                      /* commit → blit onto the FB */

    /* Read the framebuffer back at the paint origin as proof the client's
     * pixels reached the screen. */
    uint32_t got = fb.px[BY * fb.stride + BX];
    kprintf("waydemo: framebuffer[%d,%d]=%x (buffer top-left=%x) -> %s\n",
            BX, BY, got, topleft, got == topleft ? "VISIBLE OK" : "MISMATCH");

    usock_close(cli);
    usock_close(srv);
}

/* ---- windowed demo: a wl_surface backed by a WM-managed gui_window --------- */

void wl_window_demo(void) {
    gui_start();                 /* bring up the compositor if it isn't already */
    task_msleep(300);            /* let the desktop/compositor task initialise   */

    struct gui_window* win = gui_app_window_create("Wayland surface", 320, 200,
                                                   200, 160, NULL, NULL);
    if (!win) { kprintf("waywin: could not create a window\n"); return; }

    const uint32_t W = 32, H = 32, STRIDE = W * 4;
    struct shm* buf = shm_create((size_t)STRIDE * H);
    if (!buf) { kprintf("waywin: shm alloc failed\n"); return; }
    /* §M90 — written through shm_write (frames are physical, not contiguous) */
    uint32_t topleft = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v = 0xFF000000u | ((x * 8u) << 16) | ((y * 8u) << 8) | 0x40u;
            shm_write(buf, ((uint64_t)y * W + x) * 4, &v, 4);
            if (!x && !y) topleft = v;
        }
    struct ofile* buf_of = ofile_from_shm(buf);

    struct usock *cli, *srv;
    if (usock_pair(&cli, &srv) != 0) { kprintf("waywin: usock_pair failed\n"); return; }
    struct wl_conn conn;
    wl_conn_init(&conn, srv);
    conn.window = win;                            /* bridge to a real window */

    kprintf("waywin: committing a %ux%u wl_shm buffer into a gui_window\n", W, H);
    client_send1(cli, WL_DISPLAY_REQ_GET_REGISTRY, WLC_REGISTRY);
    wl_conn_dispatch(&conn); client_drain(cli);
    client_bind(cli, 1, "wl_compositor", 4, WLC_COMPOSITOR);
    client_bind(cli, 2, "wl_shm",        1, WLC_SHM);
    wl_conn_dispatch(&conn); wl_conn_dispatch(&conn); client_drain(cli);
    { uint32_t a[] = { WLC_SURFACE };
      client_msg(cli, WLC_COMPOSITOR, WL_COMPOSITOR_REQ_CREATE_SURFACE, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_POOL, STRIDE * H };
      client_msg(cli, WLC_SHM, WL_SHM_REQ_CREATE_POOL, a, 2, buf_of); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_BUFFER, 0, W, H, STRIDE, WL_SHM_FORMAT_ARGB8888 };
      client_msg(cli, WLC_POOL, WL_SHM_POOL_REQ_CREATE_BUFFER, a, 6, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_BUFFER, 0, 0 };
      client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_ATTACH, a, 3, NULL); }
    wl_conn_dispatch(&conn);
    client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_COMMIT, NULL, 0, NULL);
    wl_conn_dispatch(&conn);                      /* commit → blit into the window */

    uint32_t got = gui_window_pixel(win, 0, 0);
    kprintf("waywin: window[0,0]=%x (buffer top-left=%x) -> %s\n",
            got, topleft, got == topleft ? "IN-WINDOW OK" : "MISMATCH");

    usock_close(cli);
    usock_close(srv);
}

/* ---- input demo: wl_seat / wl_keyboard / wl_pointer ----------------------- */

void wl_input_demo(void) {
    struct usock *cli, *srv;
    if (usock_pair(&cli, &srv) != 0) { kprintf("wayinput: usock_pair failed\n"); return; }
    struct wl_conn conn;
    wl_conn_init(&conn, srv);
    kprintf("wayinput: wl_seat - keyboard + pointer\n");

    client_send1(cli, WL_DISPLAY_REQ_GET_REGISTRY, WLC_REGISTRY);
    wl_conn_dispatch(&conn); client_drain(cli);

    client_bind(cli, 4, "wl_seat", 5, WLC_SEAT);
    wl_conn_dispatch(&conn); client_drain(cli);            /* capabilities */

    { uint32_t a[] = { WLC_POINTER };
      client_msg(cli, WLC_SEAT, WL_SEAT_REQ_GET_POINTER, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_KEYBOARD };
      client_msg(cli, WLC_SEAT, WL_SEAT_REQ_GET_KEYBOARD, a, 1, NULL); }
    wl_conn_dispatch(&conn);

    /* The server side forwards input (here: synthetic; the M22.7 router feeds
     * real events the same way). */
    kprintf("wayinput: server forwarding key 30 ('a') + pointer motion (120,80)\n");
    wl_send_key(&conn, 30, 1);                             /* evdev keycode 30 = 'a' */
    wl_send_key(&conn, 30, 0);
    wl_send_motion(&conn, 120, 80);
    client_drain(cli);

    usock_close(cli);
    usock_close(srv);
}

/* ---- compositor integration: a surface IS a desktop window (+ input) ------ */

void wl_compositor_demo(void) {
    gui_start(); task_msleep(300);
    struct usock *cli, *srv;
    if (usock_pair(&cli, &srv) != 0) { kprintf("waycomp: usock_pair failed\n"); return; }
    struct wl_conn conn;
    wl_conn_init(&conn, srv);
    conn.wm_mode = 1;                                 /* server-per-surface */

    const uint32_t W = 32, H = 32, STRIDE = W * 4;
    struct shm* buf = shm_create((size_t)STRIDE * H);
    if (!buf) { kprintf("waycomp: shm failed\n"); return; }
    /* §M90 — written through shm_write (frames are physical, not contiguous) */
    uint32_t topleft = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v = 0xFF000000u | ((x * 8u) << 16) | ((y * 8u) << 8) | 0x40u;
            shm_write(buf, ((uint64_t)y * W + x) * 4, &v, 4);
            if (!x && !y) topleft = v;
        }
    struct ofile* buf_of = ofile_from_shm(buf);

    kprintf("waycomp: a Wayland client's surface becomes a desktop window\n");
    client_send1(cli, WL_DISPLAY_REQ_GET_REGISTRY, WLC_REGISTRY);
    wl_conn_dispatch(&conn); client_drain(cli);
    client_bind(cli, 1, "wl_compositor", 4, WLC_COMPOSITOR);
    client_bind(cli, 2, "wl_shm",        1, WLC_SHM);
    client_bind(cli, 3, "xdg_wm_base",   2, WLC_XDG_WM_BASE);
    client_bind(cli, 4, "wl_seat",       5, WLC_SEAT);
    for (int i = 0; i < 4; i++) wl_conn_dispatch(&conn);
    client_drain(cli);

    { uint32_t a[] = { WLC_POINTER };  client_msg(cli, WLC_SEAT, WL_SEAT_REQ_GET_POINTER, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_KEYBOARD }; client_msg(cli, WLC_SEAT, WL_SEAT_REQ_GET_KEYBOARD, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_SURFACE };  client_msg(cli, WLC_COMPOSITOR, WL_COMPOSITOR_REQ_CREATE_SURFACE, a, 1, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_XDG_SURFACE, WLC_SURFACE };
      client_msg(cli, WLC_XDG_WM_BASE, XDG_WM_BASE_REQ_GET_XDG_SURFACE, a, 2, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_XDG_TOPLEVEL };
      client_msg(cli, WLC_XDG_SURFACE, XDG_SURFACE_REQ_GET_TOPLEVEL, a, 1, NULL); }
    wl_conn_dispatch(&conn);                          /* → server creates the window */
    client_drain(cli);

    { uint32_t a[] = { WLC_POOL, STRIDE * H };
      client_msg(cli, WLC_SHM, WL_SHM_REQ_CREATE_POOL, a, 2, buf_of); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_BUFFER, 0, W, H, STRIDE, WL_SHM_FORMAT_ARGB8888 };
      client_msg(cli, WLC_POOL, WL_SHM_POOL_REQ_CREATE_BUFFER, a, 6, NULL); }
    wl_conn_dispatch(&conn);
    { uint32_t a[] = { WLC_BUFFER, 0, 0 };
      client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_ATTACH, a, 3, NULL); }
    wl_conn_dispatch(&conn);
    client_msg(cli, WLC_SURFACE, WL_SURFACE_REQ_COMMIT, NULL, 0, NULL);
    wl_conn_dispatch(&conn);                          /* → blit into the window */

    uint32_t got = conn.window ? gui_window_pixel(conn.window, 0, 0) : 0;
    kprintf("waycomp: window[0,0]=%x (buffer=%x) -> %s\n", got, topleft,
            (conn.window && got == topleft) ? "SURFACE-IN-WINDOW OK" : "no window");

    /* Input routing: simulate the compositor delivering input to the window;
     * the hook forwards it to the client's wl_seat. */
    if (conn.window) {
        struct gui_input k = { .type = GUI_INPUT_KEY, .keycode = 30, .pressed = 1, .x = 0, .y = 0 };
        wl_window_input(conn.window, &k, &conn);
        struct gui_input m = { .type = GUI_INPUT_MOTION, .keycode = 0, .pressed = 0, .x = 50, .y = 40 };
        wl_window_input(conn.window, &m, &conn);
        client_drain(cli);
    }

    usock_close(cli);
    usock_close(srv);
}
