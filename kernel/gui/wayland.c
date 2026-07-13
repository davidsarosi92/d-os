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
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* Interface tags stored in wl_conn.obj_iface[]. */
enum { WLI_NONE = 0, WLI_DISPLAY, WLI_REGISTRY, WLI_CALLBACK,
       WLI_COMPOSITOR, WLI_SHM, WLI_SHM_POOL, WLI_BUFFER, WLI_SURFACE,
       WLI_XDG_WM_BASE, WLI_XDG_SURFACE, WLI_XDG_TOPLEVEL };

/* Real Wayland opcodes (from wayland.xml).  request = client→server. */
enum { WL_DISPLAY_REQ_SYNC = 0, WL_DISPLAY_REQ_GET_REGISTRY = 1 };
enum { WL_DISPLAY_EVT_ERROR = 0, WL_DISPLAY_EVT_DELETE_ID = 1 };
enum { WL_REGISTRY_REQ_BIND = 0 };
enum { WL_REGISTRY_EVT_GLOBAL = 0 };
enum { WL_CALLBACK_EVT_DONE = 0 };
enum { WL_COMPOSITOR_REQ_CREATE_SURFACE = 0 };
enum { WL_SHM_REQ_CREATE_POOL = 0 };
enum { WL_SHM_EVT_FORMAT = 0 };
enum { WL_SHM_POOL_REQ_CREATE_BUFFER = 0 };
enum { WL_BUFFER_EVT_RELEASE = 0 };
enum { WL_SURFACE_REQ_ATTACH = 1, WL_SURFACE_REQ_COMMIT = 6 };
/* xdg_shell (the modern window role protocol). */
enum { XDG_WM_BASE_REQ_GET_XDG_SURFACE = 2 };
enum { XDG_SURFACE_REQ_GET_TOPLEVEL = 1, XDG_SURFACE_REQ_ACK_CONFIGURE = 4 };
enum { XDG_SURFACE_EVT_CONFIGURE = 0 };
enum { XDG_TOPLEVEL_REQ_SET_TITLE = 2 };
enum { XDG_TOPLEVEL_EVT_CONFIGURE = 0, XDG_TOPLEVEL_EVT_CLOSE = 1 };

/* wl_shm pixel formats (subset). */
enum { WL_SHM_FORMAT_ARGB8888 = 0, WL_SHM_FORMAT_XRGB8888 = 1 };

/* The globals this server advertises (name = the bind() handle). */
struct wl_global { uint32_t name; const char* iface; uint32_t version; };
static const struct wl_global g_globals[] = {
    { 1, "wl_compositor", 4 },
    { 2, "wl_shm",        1 },
    { 3, "xdg_wm_base",   2 },
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
    usock_send(c->sock, msg, size, NULL);
}

/* Emit wl_callback.done(serial) on object `cb`. */
static void send_callback_done(struct wl_conn* c, uint32_t cb, uint32_t data) {
    uint8_t msg[12];
    put32(msg + 0, cb);
    put32(msg + 4, (12u << 16) | WL_CALLBACK_EVT_DONE);
    put32(msg + 8, data);
    usock_send(c->sock, msg, 12, NULL);
}

/* Emit wl_display.delete_id(id) — tells the client the server released `id`. */
static void send_delete_id(struct wl_conn* c, uint32_t id) {
    uint8_t msg[12];
    put32(msg + 0, WL_DISPLAY_ID);
    put32(msg + 4, (12u << 16) | WL_DISPLAY_EVT_DELETE_ID);
    put32(msg + 8, id);
    usock_send(c->sock, msg, 12, NULL);
}

/* Emit wl_shm.format(format) on the client's wl_shm object. */
static void send_shm_format(struct wl_conn* c, uint32_t shm_id, uint32_t fmt) {
    uint8_t msg[12];
    put32(msg + 0, shm_id);
    put32(msg + 4, (12u << 16) | WL_SHM_EVT_FORMAT);
    put32(msg + 8, fmt);
    usock_send(c->sock, msg, 12, NULL);
}

/* Emit wl_buffer.release(buffer) — the server is done reading the buffer, so
 * the client may reuse it. */
static void send_buffer_release(struct wl_conn* c, uint32_t buffer_id) {
    uint8_t msg[8];
    put32(msg + 0, buffer_id);
    put32(msg + 4, (8u << 16) | WL_BUFFER_EVT_RELEASE);
    usock_send(c->sock, msg, 8, NULL);
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
    usock_send(c->sock, msg, 20, NULL);
}

/* Emit xdg_surface.configure(serial) — the client must ack_configure(serial). */
static void send_xdg_surface_configure(struct wl_conn* c, uint32_t xs, uint32_t serial) {
    uint8_t msg[12];
    put32(msg + 0, xs);
    put32(msg + 4, (12u << 16) | XDG_SURFACE_EVT_CONFIGURE);
    put32(msg + 8, serial);
    usock_send(c->sock, msg, 12, NULL);
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
}

/* wl_surface.commit — the moment the client's frame becomes current.  Read the
 * attached wl_buffer's pixels out of the wl_shm_pool's frames (via the kernel
 * identity map, like fd.c) and log proof they crossed the wire + the SCM_RIGHTS
 * fd passing intact.  Stage 3 gfx_blits these into a gui_window + gui_damage. */
static void wl_surface_commit(struct wl_conn* c) {
    struct shm* s = c->pool_shm;
    if (!s || !c->buf_w || !c->buf_h) { kprintf("wayland: commit with no buffer\n"); return; }

    uint32_t topleft = 0, sum = 0;
    for (uint32_t y = 0; y < c->buf_h; y++) {
        for (uint32_t x = 0; x < c->buf_w; x++) {
            uint32_t bo = c->buf_off + y * c->buf_stride + x * 4;
            uint32_t fi = bo / 4096, fo = bo % 4096;
            if ((int)fi >= s->nframes) continue;
            uint32_t px = *(volatile uint32_t*)(uintptr_t)(s->frames[fi] + fo);
            if (x == 0 && y == 0) topleft = px;
            sum += px;
        }
    }
    kprintf("wayland: COMMIT surface %u: %ux%u buffer, top-left=%x checksum=%x\n",
            c->surface_id, c->buf_w, c->buf_h, topleft, sum);
    send_buffer_release(c, c->buffer_id);
}

int wl_conn_dispatch(struct wl_conn* c) {
    if (!usock_can_read(c->sock)) return 0;

    uint8_t hdr[8];
    if (recv_exact(c->sock, hdr, 8) != 8) return -1;     /* peer closed */
    uint32_t obj  = get32(hdr);
    uint32_t w2   = get32(hdr + 4);
    uint32_t size = w2 >> 16;
    uint32_t op   = w2 & 0xffffu;

    uint8_t body[256];
    uint32_t blen = (size >= 8) ? (size - 8) : 0;
    if (blen > sizeof body) blen = sizeof body;
    if (blen && recv_exact(c->sock, body, (long)blen) != (long)blen) return -1;

    uint8_t iface = (obj < WL_MAX_OBJECTS) ? c->obj_iface[obj] : WLI_NONE;

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
                     (name == 3) ? WLI_XDG_WM_BASE : WLI_NONE;
        if (new_id < WL_MAX_OBJECTS) c->obj_iface[new_id] = bi;
        kprintf("wayland: bind(name=%u) -> object %u\n", name, new_id);
        if (bi == WLI_SHM) {                             /* advertise formats */
            send_shm_format(c, new_id, WL_SHM_FORMAT_ARGB8888);
            send_shm_format(c, new_id, WL_SHM_FORMAT_XRGB8888);
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
        send_xdg_toplevel_configure(c, nid, 0, 0);       /* client picks a size */
        send_xdg_surface_configure(c, c->xdg_surface_id, ++c->serial);

    } else if (iface == WLI_XDG_TOPLEVEL && op == XDG_TOPLEVEL_REQ_SET_TITLE && blen >= 4) {
        uint32_t slen = get32(body);
        char t[64]; uint32_t k = 0;
        for (; k + 1 < slen && k < sizeof t - 1; k++) t[k] = (char)body[4 + k];
        t[k] = 0;
        kprintf("wayland: xdg_toplevel set_title(\"%s\")\n", t);

    } else if (iface == WLI_XDG_SURFACE && op == XDG_SURFACE_REQ_ACK_CONFIGURE && blen >= 4) {
        kprintf("wayland: xdg_surface ack_configure(serial=%u)\n", get32(body));

    } else {
        kprintf("wayland: object %u (iface %u) opcode %u — unhandled\n", obj, iface, op);
    }
    return 1;
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
    if (buf) {
        volatile uint32_t* px = (volatile uint32_t*)(uintptr_t)buf->frames[0];
        for (uint32_t i = 0; i < W * H; i++) px[i] = COLOR;
    }
    struct ofile* buf_of = buf ? ofile_from_shm(buf) : NULL;

    kprintf("waytest: stage 2 — shm buffer -> surface commit (fill=%x)\n", COLOR);
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
    kprintf("waytest: stage 3 — xdg_shell top-level role\n");
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
