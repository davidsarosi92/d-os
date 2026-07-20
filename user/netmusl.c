/* =============================================================================
 * netmusl.c — RING-3 networking from an UNMODIFIED musl binary (§M39 stage 3b).
 *
 * The musl counterpart of user/httptest.c (which used the native d-os libc):
 * this is an ordinary hosted program using the standard BSD-sockets API
 * (<sys/socket.h>, <netinet/in.h>) linked against pristine musl.  On i386 musl
 * funnels every socket op through the Linux `socketcall` multiplexer (syscall
 * 102), which kernel/hal/x86/linux_abi.c now translates onto the M24 stack —
 * so this proves the socket ABI end to end without any getaddrinfo/TLS yet:
 *
 *   1. UDP: resolve example.com via a hand-built DNS A-query to the SLIRP
 *      resolver (10.0.2.3:53) — socket()/sendto()/recvfrom().
 *   2. TCP: socket()/connect()/write()/read() an HTTP/1.0 GET to the resolved
 *      address and print the status line.
 *
 * Needs QEMU user networking (`-netdev user -device virtio-net`).
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Encode "example.com" as DNS labels: 7example3com0. */
static int dns_name(const char *host, unsigned char *out) {
    int op = 0, lp = 0, ls = 0;
    out[op++] = 0;
    for (const char *c = host;; c++) {
        if (*c == '.' || *c == '\0') {
            out[ls] = (unsigned char)lp; lp = 0; ls = op;
            if (*c == '\0') break;
            out[op++] = 0;
        } else { out[op++] = (unsigned char)*c; lp++; }
    }
    out[op++] = 0;
    return op;
}
static unsigned rd16(const unsigned char *p) { return (unsigned)((p[0] << 8) | p[1]); }

/* Build a sockaddr_in from dotted host-order octets + a host-order port. */
static void mk_addr(struct sockaddr_in *sa, unsigned ip, int port) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);
    sa->sin_addr.s_addr = htonl(ip);
}

static unsigned resolve(const char *host) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("netmusl: UDP socket() failed\n"); return 0; }

    unsigned char q[512]; int o = 0;
    q[o++] = 0x12; q[o++] = 0x34; q[o++] = 0x01; q[o++] = 0x00;
    q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0;
    o += dns_name(host, q + o);
    q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 1;

    struct sockaddr_in ns;
    mk_addr(&ns, (10u << 24) | (0u << 16) | (2u << 8) | 3u, 53);   /* 10.0.2.3 */
    if (sendto(fd, q, (size_t)o, 0, (struct sockaddr *)&ns, sizeof ns) < 0) {
        printf("netmusl: DNS sendto() failed\n"); close(fd); return 0;
    }

    unsigned char r[512];
    struct sockaddr_in from; socklen_t fl = sizeof from;
    long n = recvfrom(fd, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
    close(fd);
    if (n <= 0) { printf("netmusl: no DNS response\n"); return 0; }

    int off = 12; while (off < n && r[off]) off += 1 + r[off]; off += 1 + 4;
    unsigned anc = rd16(r + 6);
    for (unsigned i = 0; i < anc && off + 12 <= n; i++) {
        if ((r[off] & 0xC0) == 0xC0) off += 2;
        else { while (off < n && r[off]) off += 1 + r[off]; off += 1; }
        unsigned type = rd16(r + off), rdl = rd16(r + off + 8); off += 10;
        if (type == 1 && rdl == 4)
            return ((unsigned)r[off] << 24) | ((unsigned)r[off + 1] << 16) |
                   ((unsigned)r[off + 2] << 8) | (unsigned)r[off + 3];
        off += rdl;
    }
    return 0;
}

int main(void) {
    unsigned ip = resolve("example.com");
    if (!ip) { printf("netmusl: DNS resolve failed\n"); return 1; }
    printf("netmusl: example.com = %u.%u.%u.%u (via musl socketcall)\n",
           (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("netmusl: TCP socket() failed\n"); return 1; }

    struct sockaddr_in sa;
    mk_addr(&sa, ip, 80);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        printf("netmusl: connect() failed\n"); close(fd); return 1;
    }
    printf("netmusl: connected to :80, sending GET\n");

    const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) {
        printf("netmusl: write() failed\n"); close(fd); return 1;
    }

    char buf[1024]; long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) { printf("netmusl: no HTTP response\n"); return 1; }
    buf[n] = 0;
    char *nl = strchr(buf, '\r'); if (nl) *nl = 0;
    printf("netmusl: HTTP status: %s\n", buf);
    return (strncmp(buf, "HTTP/1.", 7) == 0) ? 0 : 1;
}
