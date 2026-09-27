/* =============================================================================
 * wlclip.c — copy and paste through wl_data_device, with the REAL libwayland
 * client (§M59, 2026-09-27).
 *
 * The client that the Wayland clipboard needed before it could be claimed to
 * work.  §M59 declined `wl_data_device` for exactly this reason: a protocol
 * surface with nothing to exercise it "works" until its first real user.  This
 * is that user, written against public libwayland API only — the same calls
 * wl-clipboard's wl-copy / wl-paste and every toolkit make:
 *
 *   wlclip copy <text>   create a wl_data_source, offer text/plain;charset=
 *                        utf-8 + text/plain, set_selection, then serve the
 *                        compositor's `send` request by writing <text> into
 *                        the descriptor it hands us.
 *   wlclip paste         get a wl_data_device, wait for its `selection`
 *                        event, `receive` the offer as UTF-8 into a pipe and
 *                        print what arrived — as text AND as hex, because the
 *                        interesting cases (é, ő, ű) are exactly the bytes a
 *                        console may render wrongly while they are right.
 *
 * Each prints a PASS/FAIL line of its own for the harness to grep.
 * ============================================================================= */

#include <wayland-client.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static struct wl_seat* seat;
static struct wl_data_device_manager* ddm;
static struct wl_data_device* ddev;
static struct wl_data_offer* cur_offer;       /* the latest `selection`      */
static int got_selection;                     /* a selection event arrived    */
static int offer_has_utf8;
static int sent;                              /* copy: `send` was served      */
static int cancelled;
static const char* copy_text;

/* ---- registry ------------------------------------------------------------ */
static void reg_global(void* d, struct wl_registry* r, uint32_t name,
                       const char* iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, "wl_seat"))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, "wl_data_device_manager"))
        ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface,
                               ver < 3 ? ver : 3);
}
static void reg_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* ---- the offer ------------------------------------------------------------ */
static void offer_offer(void* d, struct wl_data_offer* o, const char* mime) {
    (void)d; (void)o;
    printf("wlclip: offer carries %s\n", mime);
    if (!strcmp(mime, "text/plain;charset=utf-8")) offer_has_utf8 = 1;
}
static void offer_src_actions(void* d, struct wl_data_offer* o, uint32_t a) { (void)d;(void)o;(void)a; }
static void offer_action(void* d, struct wl_data_offer* o, uint32_t a) { (void)d;(void)o;(void)a; }
static const struct wl_data_offer_listener offer_l = {
    offer_offer, offer_src_actions, offer_action };

/* ---- the device ----------------------------------------------------------- */
static void dd_data_offer(void* d, struct wl_data_device* dd, struct wl_data_offer* o) {
    (void)d; (void)dd;
    offer_has_utf8 = 0;
    wl_data_offer_add_listener(o, &offer_l, NULL);
}
static void dd_enter(void* d, struct wl_data_device* dd, uint32_t s, struct wl_surface* sf,
                     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o)
{ (void)d;(void)dd;(void)s;(void)sf;(void)x;(void)y;(void)o; }
static void dd_leave(void* d, struct wl_data_device* dd) { (void)d;(void)dd; }
static void dd_motion(void* d, struct wl_data_device* dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ (void)d;(void)dd;(void)t;(void)x;(void)y; }
static void dd_drop(void* d, struct wl_data_device* dd) { (void)d;(void)dd; }
static void dd_selection(void* d, struct wl_data_device* dd, struct wl_data_offer* o) {
    (void)d; (void)dd;
    if (cur_offer && cur_offer != o) wl_data_offer_destroy(cur_offer);
    cur_offer = o;
    got_selection = 1;
}
static const struct wl_data_device_listener dd_l = {
    dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

/* ---- the source (copy) ---------------------------------------------------- */
static void src_target(void* d, struct wl_data_source* s, const char* m) { (void)d;(void)s;(void)m; }
static void src_send(void* d, struct wl_data_source* s, const char* mime, int32_t fd) {
    (void)d; (void)s;
    size_t n = strlen(copy_text), off = 0;
    while (off < n) {
        ssize_t w = write(fd, copy_text + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    printf("wlclip: served %zu bytes as %s\n", off, mime);
    sent = 1;
}
static void src_cancelled(void* d, struct wl_data_source* s) {
    (void)d;
    cancelled = 1;
    wl_data_source_destroy(s);
}
static void src_dnd_drop(void* d, struct wl_data_source* s) { (void)d;(void)s; }
static void src_dnd_fin(void* d, struct wl_data_source* s) { (void)d;(void)s; }
static void src_action(void* d, struct wl_data_source* s, uint32_t a) { (void)d;(void)s;(void)a; }
static const struct wl_data_source_listener src_l = {
    src_target, src_send, src_cancelled, src_dnd_drop, src_dnd_fin, src_action };

/* d-os hands a Linux-personality program its argv in ISO-8859-2, its own
 * text encoding (an open item: the personality boundary does not convert).
 * A Wayland client must offer what its label says, so a copy argument that is
 * NOT valid UTF-8 is taken as Latin-2 and converted — exactly what any real
 * program receiving legacy-encoded input would have to do. */
static const unsigned short l2_hi[96] = {
    0x00A0,0x0104,0x02D8,0x0141,0x00A4,0x013D,0x015A,0x00A7,0x00A8,0x0160,0x015E,0x0164,0x0179,0x00AD,0x017D,0x017B,
    0x00B0,0x0105,0x02DB,0x0142,0x00B4,0x013E,0x015B,0x02C7,0x00B8,0x0161,0x015F,0x0165,0x017A,0x02DD,0x017E,0x017C,
    0x0154,0x00C1,0x00C2,0x0102,0x00C4,0x0139,0x0106,0x00C7,0x010C,0x00C9,0x0118,0x00CB,0x011A,0x00CD,0x00CE,0x010E,
    0x0110,0x0143,0x0147,0x00D3,0x00D4,0x0150,0x00D6,0x00D7,0x0158,0x016E,0x00DA,0x0170,0x00DC,0x00DD,0x0162,0x00DF,
    0x0155,0x00E1,0x00E2,0x0103,0x00E4,0x013A,0x0107,0x00E7,0x010D,0x00E9,0x0119,0x00EB,0x011B,0x00ED,0x00EE,0x010F,
    0x0111,0x0144,0x0148,0x00F3,0x00F4,0x0151,0x00F6,0x00F7,0x0159,0x016F,0x00FA,0x0171,0x00FC,0x00FD,0x0163,0x02D9,
};
static int valid_utf8(const unsigned char* s) {
    while (*s) {
        int len = *s < 0x80 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 :
                  (*s & 0xF8) == 0xF0 ? 4 : 0;
        if (!len) return 0;
        for (int k = 1; k < len; k++) if ((s[k] & 0xC0) != 0x80) return 0;
        s += len;
    }
    return 1;
}
static void latin2_to_utf8(const unsigned char* in, char* out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned u = *in < 0xA0 ? *in : l2_hi[*in - 0xA0];
        if (u < 0x80) out[o++] = (char)u;
        else if (u < 0x800) { out[o++] = (char)(0xC0 | (u >> 6)); out[o++] = (char)(0x80 | (u & 0x3F)); }
        else { out[o++] = (char)(0xE0 | (u >> 12)); out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
               out[o++] = (char)(0x80 | (u & 0x3F)); }
    }
    out[o] = 0;
}

static void hexdump(const char* tag, const unsigned char* b, int n) {
    printf("wlclip: %s hex:", tag);
    for (int i = 0; i < n && i < 48; i++) printf(" %02x", b[i]);
    printf("%s\n", n > 48 ? " ..." : "");
}

static int do_paste(struct wl_display* dpy) {
    for (int i = 0; i < 20 && !got_selection; i++) wl_display_roundtrip(dpy);
    if (!got_selection) { printf("wlclip: paste FAIL - no selection event\n"); return 1; }
    if (!cur_offer) { printf("wlclip: paste - the clipboard is empty\n"); return 2; }
    int p[2];
    if (pipe(p) != 0) { printf("wlclip: paste FAIL - pipe\n"); return 1; }
    wl_data_offer_receive(cur_offer, offer_has_utf8 ? "text/plain;charset=utf-8"
                                                    : "text/plain", p[1]);
    close(p[1]);                           /* only the compositor may write now */
    wl_display_flush(dpy);
    static unsigned char buf[65536];
    int n = 0;
    for (;;) {
        ssize_t r = read(p[0], buf + n, sizeof buf - 1 - n);
        if (r <= 0) break;
        n += (int)r;
    }
    close(p[0]);
    buf[n] = 0;
    printf("wlclip: pasted %d bytes: %s\n", n, buf);
    hexdump("pasted", buf, n);
    printf("wlclip: paste %s\n", n > 0 ? "PASS" : "FAIL (empty)");
    return n > 0 ? 0 : 1;
}

static int do_copy(struct wl_display* dpy, const char* text) {
    copy_text = text;
    struct wl_data_source* src = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(src, &src_l, NULL);
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_source_offer(src, "text/plain");
    wl_data_device_set_selection(ddev, src, 0);
    /* The compositor asks for the data during set_selection; one roundtrip
     * drives our `send` handler, a second collects the selection echo. */
    for (int i = 0; i < 20 && !sent; i++) wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    hexdump("copied", (const unsigned char*)text, (int)strlen(text));
    printf("wlclip: copy %s\n", sent ? "PASS" : "FAIL - the compositor never asked for the data");
    return sent ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2 || (strcmp(argv[1], "paste") && (strcmp(argv[1], "copy") || argc < 3))) {
        printf("usage: wlclip copy <text> | wlclip paste\n");
        return 2;
    }
    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) { printf("wlclip: FAIL - cannot connect\n"); return 1; }
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!seat || !ddm) {
        printf("wlclip: FAIL - no %s\n", !ddm ? "wl_data_device_manager" : "wl_seat");
        return 1;
    }
    ddev = wl_data_device_manager_get_data_device(ddm, seat);
    wl_data_device_add_listener(ddev, &dd_l, NULL);
    wl_display_roundtrip(dpy);

    int rc;
    if (!strcmp(argv[1], "paste")) {
        rc = do_paste(dpy);
    } else {
        /* argv[2..] joined with spaces: the shell splits on them. */
        static char text[1024];
        size_t o = 0;
        for (int i = 2; i < argc; i++) {
            for (const char* p = argv[i]; *p && o + 2 < sizeof text; p++) text[o++] = *p;
            if (i + 1 < argc && o + 2 < sizeof text) text[o++] = ' ';
        }
        text[o] = 0;
        static char utf8[3072];
        if (!valid_utf8((const unsigned char*)text)) {
            latin2_to_utf8((const unsigned char*)text, utf8, sizeof utf8);
            rc = do_copy(dpy, utf8);
        } else {
            rc = do_copy(dpy, text);
        }
    }
    wl_display_disconnect(dpy);
    return rc;
}
