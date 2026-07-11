/* =============================================================================
 * httptest.c — fetch a web page from RING 3 (M24 socket API, TCP).
 *
 * The full userland-networking story: resolve example.com over a UDP socket,
 * then open a TCP (SOCK_STREAM) socket, connect(), write() an HTTP/1.0 GET and
 * read() the response — all from a ring-3 program over the in-kernel stack.
 * The §M39 TLS bridge target (swap TCP for TLS and it becomes HTTPS).
 * ============================================================================= */

#include "libc.h"

static int dns_name(const char* host, unsigned char* out) {
    int op = 0, lp = 0, ls = 0;
    out[op++] = 0;
    for (const char* c = host; ; c++) {
        if (*c == '.' || *c == '\0') { out[ls] = (unsigned char)lp; lp = 0; ls = op;
                                       if (*c == '\0') break; out[op++] = 0; }
        else { out[op++] = (unsigned char)*c; lp++; }
    }
    out[op++] = 0;
    return op;
}
static unsigned rd16(const unsigned char* p) { return (unsigned)((p[0] << 8) | p[1]); }

static unsigned resolve(const char* host) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    unsigned char q[512]; int o = 0;
    q[o++] = 0x12; q[o++] = 0x34; q[o++] = 0x01; q[o++] = 0x00;
    q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0;
    o += dns_name(host, q + o);
    q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 1;
    sendto(fd, q, (unsigned)o, IPV4(10, 0, 2, 3), 53);
    unsigned char r[512]; unsigned si; int sp;
    long n = recvfrom(fd, r, sizeof(r), &si, &sp);
    close(fd);
    if (n <= 0) return 0;
    int off = 12; while (off < n && r[off]) off += 1 + r[off]; off += 1 + 4;
    unsigned anc = rd16(r + 6);
    for (unsigned i = 0; i < anc && off + 12 <= n; i++) {
        if ((r[off] & 0xC0) == 0xC0) off += 2;
        else { while (off < n && r[off]) off += 1 + r[off]; off += 1; }
        unsigned type = rd16(r + off), rdl = rd16(r + off + 8); off += 10;
        if (type == 1 && rdl == 4) return IPV4(r[off], r[off+1], r[off+2], r[off+3]);
        off += rdl;
    }
    return 0;
}

int main(void) {
    unsigned ip = resolve("example.com");
    if (!ip) { printf("http: DNS resolve failed\n"); return 1; }
    printf("http: example.com = %u.%u.%u.%u\n",
           (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("http: socket() failed\n"); return 1; }
    if (connect_ip(fd, ip, 80) != 0) { printf("http: connect() failed\n"); close(fd); return 1; }
    printf("http: connected to :80, sending GET\n");

    const char* req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    write(fd, req, strlen(req));

    char buf[256]; int total = 0, n;
    n = (int)read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        int i = 0; while (i < n && buf[i] != '\r' && buf[i] != '\n') i++;
        buf[i] = '\0';
        printf("http: status: %s\n", buf);
        total = n;
    }
    while ((n = (int)read(fd, buf, sizeof(buf) - 1)) > 0) total += n;
    printf("http: %d bytes received total\n", total);

    close(fd);
    return 0;
}
