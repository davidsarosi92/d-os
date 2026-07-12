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
enum { WLI_NONE = 0, WLI_DISPLAY, WLI_REGISTRY, WLI_CALLBACK };

/* wl_display opcodes (request = client→server, event = server→client). */
enum { WL_DISPLAY_REQ_SYNC = 0, WL_DISPLAY_REQ_GET_REGISTRY = 1 };
enum { WL_DISPLAY_EVT_ERROR = 0, WL_DISPLAY_EVT_DELETE_ID = 1 };
/* wl_registry event / wl_callback event. */
enum { WL_REGISTRY_EVT_GLOBAL = 0 };
enum { WL_CALLBACK_EVT_DONE = 0 };

/* The globals this server advertises (name is the registry handle a client
 * would bind()).  Only advertised in stage 1; bind() lands in stage 2. */
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

/* ---- connection + dispatch ----------------------------------------------- */

void wl_conn_init(struct wl_conn* c, struct usock* sock) {
    for (int i = 0; i < WL_MAX_OBJECTS; i++) c->obj_iface[i] = WLI_NONE;
    c->sock = sock;
    c->obj_iface[WL_DISPLAY_ID] = WLI_DISPLAY;   /* object 1 is always wl_display */
    c->registry_id = 0;
    c->serial = 0;
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

    if (obj == WL_DISPLAY_ID && c->obj_iface[obj] == WLI_DISPLAY) {
        if (op == WL_DISPLAY_REQ_GET_REGISTRY && blen >= 4) {
            uint32_t reg = get32(body);
            if (reg < WL_MAX_OBJECTS) c->obj_iface[reg] = WLI_REGISTRY;
            c->registry_id = reg;
            kprintf("wayland: get_registry(id=%u) -> advertising %d globals\n",
                    reg, WL_NGLOBALS);
            for (int i = 0; i < WL_NGLOBALS; i++) send_global(c, &g_globals[i]);
        } else if (op == WL_DISPLAY_REQ_SYNC && blen >= 4) {
            uint32_t cb = get32(body);
            kprintf("wayland: sync(callback=%u) -> done + delete_id\n", cb);
            send_callback_done(c, cb, c->serial++);
            send_delete_id(c, cb);
        } else {
            kprintf("wayland: wl_display: unhandled opcode %u\n", op);
        }
    } else {
        kprintf("wayland: request for object %u (iface %u) opcode %u — unhandled\n",
                obj, obj < WL_MAX_OBJECTS ? c->obj_iface[obj] : 0, op);
    }
    return 1;
}

/* ---- self-test: a hand-marshalled client over a usock_pair ---------------- */

/* The object ids this test client allocates (a real client tracks each object's
 * interface itself; opcode 0 alone is ambiguous — it is both wl_registry.global
 * and wl_callback.done — so the DISPATCH MUST key on the object's interface). */
#define WLC_REGISTRY 2u
#define WLC_CALLBACK 3u

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

void wl_selftest(void) {
    struct usock *cli, *srv;
    if (usock_pair(&cli, &srv) != 0) { kprintf("waytest: usock_pair failed\n"); return; }

    struct wl_conn conn;
    wl_conn_init(&conn, srv);
    kprintf("waytest: Wayland wire handshake over a unix socket\n");

    /* 1. wl_display.get_registry(new_id = 2) → globals. */
    client_send1(cli, WL_DISPLAY_REQ_GET_REGISTRY, 2);
    wl_conn_dispatch(&conn);
    client_drain(cli);

    /* 2. wl_display.sync(new_id = 3) → wl_callback.done + delete_id. */
    client_send1(cli, WL_DISPLAY_REQ_SYNC, 3);
    wl_conn_dispatch(&conn);
    client_drain(cli);

    kprintf("waytest: handshake complete\n");
    usock_close(cli);
    usock_close(srv);
}
