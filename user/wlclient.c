/* =============================================================================
 * wlclient.c — a REAL user-space (ring 3) Wayland client for d-os (§M26).
 *
 * Freestanding, native d-os ABI (int 0x80: SYS_WRITE=2, SYS_READ=3, SYS_EXIT=1),
 * no libc.  It speaks the Wayland wire protocol over an INHERITED socket fd
 * (fd 3 — the shell installs one end of a usock_pair before exec'ing this, and
 * the server runs on its own task on the other end).  It performs the canonical
 * handshake — get_registry + sync — parses the advertised globals off the wire
 * and the final wl_callback.done, and reports.  This is the analogue of a real
 * libwayland client, hand-rolled until libwayland is ported (§M40).
 * ============================================================================= */

#define SYS_EXIT  1
#define SYS_WRITE 2
#define SYS_READ  3
#define WLFD      3           /* the inherited Wayland connection socket */

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void emit(const char* s) { long n = 0; while (s[n]) n++; sys3(SYS_WRITE, 1, (long)s, n); }
static void put32(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned get32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

void _start(void) {
    unsigned char msg[12];
    /* wl_display.get_registry(new_id=2): obj=1, opcode=1, size=12. */
    put32(msg, 1); put32(msg + 4, (12u << 16) | 1); put32(msg + 8, 2);
    sys3(SYS_WRITE, WLFD, (long)msg, 12);
    /* wl_display.sync(new_id=3): obj=1, opcode=0, size=12. */
    put32(msg, 1); put32(msg + 4, (12u << 16) | 0); put32(msg + 8, 3);
    sys3(SYS_WRITE, WLFD, (long)msg, 12);

    unsigned char buf[1024];
    int len = 0, nglobals = 0, done = 0;
    while (!done) {
        long r = sys3(SYS_READ, WLFD, (long)(buf + len), (long)(sizeof buf - len));
        if (r <= 0) break;                              /* server closed */
        len += (int)r;
        int off = 0;
        while (len - off >= 8) {
            unsigned obj  = get32(buf + off);
            unsigned w2   = get32(buf + off + 4);
            unsigned size = w2 >> 16, op = w2 & 0xffff;
            if (size < 8 || len - off < (int)size) break;   /* need more bytes */
            if (obj == 2 && op == 0) {                  /* wl_registry.global */
                unsigned slen = get32(buf + off + 12);
                char nm[32]; unsigned k = 0;
                for (; k + 1 < slen && k < sizeof nm - 1; k++) nm[k] = (char)buf[off + 16 + k];
                nm[k] = 0;
                emit("  wlclient: global "); emit(nm); emit("\n");
                nglobals++;
            } else if (obj == 3 && op == 0) {           /* wl_callback.done */
                done = 1;
            }
            off += size;
        }
        int rem = len - off;
        for (int i = 0; i < rem; i++) buf[i] = buf[off + i];
        len = rem;
    }

    emit(nglobals > 0 ? "wlclient: handshake OK (globals received from ring 3)\n"
                      : "wlclient: no globals received\n");
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
