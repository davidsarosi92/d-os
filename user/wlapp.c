/* =============================================================================
 * wlapp.c — a d-os app that speaks Wayland through the libwl client library
 * (not by hand-marshalling).  Connects over the inherited fd 3, does a registry
 * roundtrip via libwl, and reports the compositor's globals.  Demonstrates the
 * "app links a Wayland client library" layer (the shape a real libwayland app
 * has), pending the upstream libwayland port (§M40).
 * ============================================================================= */
#include "libwl.h"

#define SYS_EXIT  1
#define SYS_WRITE 2
#define WLFD      3

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void emit(const char* s) { long n = 0; while (s[n]) n++; sys3(SYS_WRITE, 1, (long)s, n); }

void _start(void) {
    struct wlctx c;
    wl_connect(&c, WLFD);
    int n = wl_registry_roundtrip(&c);

    if (n < 0) { emit("wlapp: connection error\n"); }
    else {
        emit("wlapp: connected via libwl; globals:");
        if (c.compositor_name) emit(" wl_compositor");
        if (c.shm_name)        emit(" wl_shm");
        if (c.xdg_name)        emit(" xdg_wm_base");
        if (c.seat_name)       emit(" wl_seat");
        emit("\n");
        /* Object-id allocation goes through the library too. */
        unsigned surface = wl_alloc_id(&c);
        (void)surface;
        emit(c.compositor_name ? "wlapp: ready to create a surface (libwl OK)\n"
                               : "wlapp: no compositor global\n");
    }
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
