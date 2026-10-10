/* sock6musl.c — §M90 3a stage d: AF_INET6 sockets as a Linux program uses
 * them, from an unmodified musl binary.  Prints "sock6: PASS" or the first
 * failing check.
 *   - a dual-stack listener on [::]:0 (V6ONLY 0) gets an ephemeral port
 *   - a v6 client to [::1]:port is accepted; the peer is ::1
 *   - a v4 client to 127.0.0.1:port is accepted by the SAME listener; the
 *     peer shows as ::ffff:127.0.0.1
 *   - data crosses both
 *   - IPV6_V6ONLY reads back what was set; a v6-only listener refuses v4
 *   - UDP over ::1 with the sender's address in recvfrom */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int fail(const char* why) { printf("sock6: FAIL %s\n", why); return 1; }

static int listen6(int v6only, int* port) {
    int l = socket(AF_INET6, SOCK_STREAM, 0);
    if (l < 0) return -1;
    setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
    struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_ANY_INIT };
    if (bind(l, (struct sockaddr*)&a, sizeof a) || listen(l, 4)) return -1;
    socklen_t al = sizeof a;
    if (getsockname(l, (struct sockaddr*)&a, &al) || a.sin6_family != AF_INET6) return -1;
    *port = ntohs(a.sin6_port);
    return l;
}

int main(void) {
    int port = 0;
    int l = listen6(0, &port);
    if (l < 0 || !port) return fail("dual-stack listen on [::]:0");
    int v = -1; socklen_t vl = sizeof v;
    if (getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) || v != 0) return fail("V6ONLY reads back 0");
    printf("sock6: listening on [::]:%d\n", port);

    /* IPv6 client */
    int c = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 to = { .sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = IN6ADDR_LOOPBACK_INIT };
    if (connect(c, (struct sockaddr*)&to, sizeof to)) return fail("connect [::1]");
    struct sockaddr_in6 pa; socklen_t pl = sizeof pa;
    int s = accept(l, (struct sockaddr*)&pa, &pl);
    char buf[64];
    if (s < 0 || pa.sin6_family != AF_INET6 || !IN6_IS_ADDR_LOOPBACK(&pa.sin6_addr)) return fail("accept a v6 peer ::1");
    if (write(c, "hello6", 6) != 6 || read(s, buf, sizeof buf) != 6 || memcmp(buf, "hello6", 6)) return fail("data over v6");
    char txt[64];
    inet_ntop(AF_INET6, &pa.sin6_addr, txt, sizeof txt);
    printf("sock6: accepted %s\n", txt);
    close(s); close(c);

    /* IPv4 client to the dual-stack listener */
    c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in t4 = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(0x7f000001) };
    if (connect(c, (struct sockaddr*)&t4, sizeof t4)) return fail("v4 connect to the dual-stack port");
    pl = sizeof pa;
    s = accept(l, (struct sockaddr*)&pa, &pl);
    if (s < 0 || !IN6_IS_ADDR_V4MAPPED(&pa.sin6_addr)) return fail("v4 peer shown as ::ffff:a.b.c.d");
    inet_ntop(AF_INET6, &pa.sin6_addr, txt, sizeof txt);
    printf("sock6: accepted %s\n", txt);
    if (write(s, "hi4", 3) != 3 || read(c, buf, sizeof buf) != 3) return fail("data over the mapped v4 connection");
    close(s); close(c); close(l);

    /* v6-only: v4 is refused */
    l = listen6(1, &port);
    vl = sizeof v;
    if (l < 0 || getsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &v, &vl) || v != 1) return fail("v6-only listener");
    c = socket(AF_INET, SOCK_STREAM, 0);
    t4.sin_port = htons(port);
    if (connect(c, (struct sockaddr*)&t4, sizeof t4) == 0) return fail("v4 reached a v6-only listener");
    close(c); close(l);

    /* UDP over ::1 */
    int u1 = socket(AF_INET6, SOCK_DGRAM, 0), u2 = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 ua = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT };
    if (bind(u1, (struct sockaddr*)&ua, sizeof ua)) return fail("udp bind [::1]:0");
    socklen_t ul = sizeof ua;
    getsockname(u1, (struct sockaddr*)&ua, &ul);
    if (sendto(u2, "dgram6", 6, 0, (struct sockaddr*)&ua, sizeof ua) != 6) return fail("udp sendto [::1]");
    struct sockaddr_in6 from; socklen_t fl = sizeof from;
    int n = (int)recvfrom(u1, buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
    if (n != 6 || from.sin6_family != AF_INET6 || !IN6_IS_ADDR_LOOPBACK(&from.sin6_addr)) return fail("udp recvfrom with a v6 source");
    printf("sock6: udp from port %d\n", ntohs(from.sin6_port));
    /* ICMPv6: a ping socket (echo to ::1, the reply carries the KERNEL's id)
     * and a raw one (CAP_NET_RAW; the message arrives without an IP header) */
    {
        int ps = socket(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6);
        int rs = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
        if (ps < 0 || rs < 0) return fail("ICMPv6 ping/raw sockets");
        unsigned char echo[16] = { 128, 0, 0, 0, 0x12, 0x34, 0, 1, 'd', '-', 'o', 's', '6' };
        struct sockaddr_in6 lo6 = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT };
        if (sendto(ps, echo, sizeof echo, 0, (struct sockaddr*)&lo6, sizeof lo6) != sizeof echo) return fail("ping6 sendto");
        unsigned char r[64];
        int k = (int)recv(ps, r, sizeof r, 0);
        if (k != sizeof echo || r[0] != 129 || r[8] != 'd') return fail("ping6 reply");
        k = (int)recv(rs, r, sizeof r, 0);                /* the raw socket saw the request ... */
        if (k < 8 || (r[0] != 128 && r[0] != 129) || (r[0] >> 4) == 6) return fail("raw ICMPv6 without the IP header");
        printf("sock6: ICMPv6 ping socket answered, raw socket saw type %d\n", r[0]);
        close(ps); close(rs);
    }
    printf("sock6: PASS\n");
    return 0;
}
