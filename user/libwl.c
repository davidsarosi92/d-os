/* =============================================================================
 * libwl.c — minimal Wayland client library (see libwl.h).  Freestanding, native
 * d-os ABI (int 0x80: SYS_WRITE=2, SYS_READ=3); no libc.
 * ============================================================================= */
#include "libwl.h"

#define SYS_WRITE 2
#define SYS_READ  3

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void put32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned get32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Send a request: header + n u32 args. */
static void wl_send(struct wlctx* c, unsigned obj, unsigned opcode,
                    const unsigned* args, int n) {
    unsigned char msg[64];
    unsigned size = 8 + (unsigned)n * 4;
    put32(msg + 0, obj);
    put32(msg + 4, (size << 16) | opcode);
    for (int i = 0; i < n; i++) put32(msg + 8 + i * 4, args[i]);
    sys3(SYS_WRITE, c->fd, (long)msg, (long)size);
}

/* Pull one complete framed message into (*obj,*op,body,*blen); returns 1, or 0
 * on EOF.  Buffers partial reads in c->rbuf. */
static int wl_recv(struct wlctx* c, unsigned* obj, unsigned* op,
                   unsigned char* body, unsigned* blen) {
    for (;;) {
        /* Enough for a header + its body already buffered? */
        if (c->rlen >= 8) {
            unsigned w2 = get32(c->rbuf + 4);
            unsigned size = w2 >> 16;
            if (size >= 8 && c->rlen >= (int)size) {
                *obj = get32(c->rbuf);
                *op  = w2 & 0xffff;
                unsigned bl = size - 8;
                for (unsigned i = 0; i < bl && i < 256; i++) body[i] = c->rbuf[8 + i];
                *blen = bl;
                for (int i = (int)size; i < c->rlen; i++) c->rbuf[i - size] = c->rbuf[i];
                c->rlen -= (int)size;
                return 1;
            }
        }
        long r = sys3(SYS_READ, c->fd, (long)(c->rbuf + c->rlen),
                      (long)((int)sizeof c->rbuf - c->rlen));
        if (r <= 0) return 0;
        c->rlen += (int)r;
    }
}

int wl_connect(struct wlctx* c, int fd) {
    c->fd = fd; c->next_id = 2; c->nglobals = 0; c->rlen = 0;
    c->compositor_name = c->shm_name = c->xdg_name = c->seat_name = 0;
    return 0;
}

unsigned wl_alloc_id(struct wlctx* c) { return c->next_id++; }

int wl_registry_roundtrip(struct wlctx* c) {
    unsigned reg = wl_alloc_id(c);                  /* wl_registry object   */
    unsigned cb  = wl_alloc_id(c);                  /* wl_callback for sync */
    unsigned a[1];
    a[0] = reg; wl_send(c, 1, 1, a, 1);             /* wl_display.get_registry */
    a[0] = cb;  wl_send(c, 1, 0, a, 1);             /* wl_display.sync         */

    for (;;) {
        unsigned obj, op, blen; unsigned char body[256];
        if (!wl_recv(c, &obj, &op, body, &blen)) return -1;
        if (obj == reg && op == 0 && blen >= 8) {   /* wl_registry.global */
            unsigned name = get32(body);
            unsigned slen = get32(body + 4);
            char nm[32]; unsigned k = 0;
            for (; k + 1 < slen && k < sizeof nm - 1; k++) nm[k] = (char)body[8 + k];
            nm[k] = 0;
            if      (streq(nm, "wl_compositor")) c->compositor_name = name;
            else if (streq(nm, "wl_shm"))        c->shm_name        = name;
            else if (streq(nm, "xdg_wm_base"))   c->xdg_name        = name;
            else if (streq(nm, "wl_seat"))       c->seat_name       = name;
            c->nglobals++;
        } else if (obj == cb && op == 0) {          /* wl_callback.done → done */
            break;
        }
    }
    return c->nglobals;
}
