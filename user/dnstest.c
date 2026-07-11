/* =============================================================================
 * dnstest.c — resolve a hostname from RING 3 over a UDP socket (M24 socket API).
 *
 * Proves the userland BSD-sockets surface end to end: socket(SOCK_DGRAM),
 * sendto() a DNS A-query to the SLIRP resolver (10.0.2.3:53), recvfrom() the
 * response, and parse the answer — all from a ring-3 program over the in-kernel
 * net stack.  The direct precursor to getaddrinfo (§M39).
 * ============================================================================= */

#include "libc.h"

/* Encode "example.com" as DNS labels (3www...7example3com0). */
static int dns_name(const char* host, unsigned char* out) {
    int op = 0, lp = 0, ls = 0;
    out[op++] = 0;                              /* first length placeholder */
    for (const char* c = host; ; c++) {
        if (*c == '.' || *c == '\0') {
            out[ls] = (unsigned char)lp; lp = 0; ls = op;
            if (*c == '\0') break;
            out[op++] = 0;
        } else { out[op++] = (unsigned char)*c; lp++; }
    }
    out[op++] = 0;                              /* root label */
    return op;
}

static unsigned rd16(const unsigned char* p) { return (unsigned)((p[0] << 8) | p[1]); }

int main(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("dns: socket() failed\n"); return 1; }
    printf("dns: socket fd=%d\n", fd);

    /* Build an A-query for example.com. */
    unsigned char q[512]; int o = 0;
    q[o++] = 0x12; q[o++] = 0x34;               /* id */
    q[o++] = 0x01; q[o++] = 0x00;               /* flags: recursion desired */
    q[o++] = 0x00; q[o++] = 0x01;               /* qdcount = 1 */
    q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0; q[o++] = 0;
    o += dns_name("example.com", q + o);
    q[o++] = 0x00; q[o++] = 0x01;               /* qtype  = A */
    q[o++] = 0x00; q[o++] = 0x01;               /* qclass = IN */

    long s = sendto(fd, q, (unsigned)o, IPV4(10, 0, 2, 3), 53);
    printf("dns: sent %d bytes to 10.0.2.3:53\n", (int)s);

    unsigned char r[512]; unsigned srcip = 0; int srcport = 0;
    long n = recvfrom(fd, r, sizeof(r), &srcip, &srcport);
    if (n <= 0) { printf("dns: no response\n"); close(fd); return 1; }
    printf("dns: %d-byte reply from %u.%u.%u.%u:%d\n", (int)n,
           (srcip >> 24) & 255, (srcip >> 16) & 255, (srcip >> 8) & 255, srcip & 255,
           srcport);

    /* Skip the header + our (uncompressed) question, then walk the answers. */
    int off = 12;
    while (off < n && r[off]) off += 1 + r[off];
    off += 1 + 4;                               /* root label + qtype + qclass */

    unsigned anc = rd16(r + 6);
    for (unsigned i = 0; i < anc && off + 12 <= n; i++) {
        if ((r[off] & 0xC0) == 0xC0) off += 2;  /* compression pointer */
        else { while (off < n && r[off]) off += 1 + r[off]; off += 1; }
        unsigned type = rd16(r + off);
        unsigned rdl  = rd16(r + off + 8);
        off += 10;
        if (type == 1 && rdl == 4) {
            printf("dns: example.com A = %u.%u.%u.%u\n",
                   r[off], r[off + 1], r[off + 2], r[off + 3]);
            close(fd);
            return 0;
        }
        off += rdl;
    }
    printf("dns: no A record in reply\n");
    close(fd);
    return 0;
}
