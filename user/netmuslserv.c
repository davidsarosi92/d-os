/* =============================================================================
 * netmuslserv.c — the SERVER half of the socket API, through UNMODIFIED musl.
 *
 * The kernel-side `tcptest` proves the mechanism; this proves the TRANSLATION.
 * Everything here is stock POSIX — socket/bind/listen/accept/getsockname/
 * getpeername/shutdown against `struct sockaddr_in` — compiled by a Linux
 * toolchain against a pristine musl, and nothing in it knows it is not on
 * Linux.  That is the whole point: §M24's server half is only real if a
 * program written for Linux can use it without being told.
 *
 * WHY ONE PROCESS CAN TEST BOTH ENDS.  connect() returns once the handshake is
 * complete, and at that moment the peer is already sitting in the listener's
 * backlog — so a single-threaded program can connect to itself and then
 * accept, with no fork and no thread.  That matters here beyond convenience:
 * it means this test exercises TWO SIMULTANEOUS CONNECTIONS in the kernel's
 * table (both endpoints are local), which is exactly what the stack this
 * milestone replaced could not hold.
 *
 * The address checks are not decoration.  `sockaddr_in` keeps its port and
 * address in NETWORK byte order while the kernel's own API is host order, so a
 * missing swap is invisible on a palindrome and wrong everywhere else —
 * hence a port (9100 = 0x238C) and an address (127.0.0.1) whose bytes differ
 * in every position.
 * ============================================================================= */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9100

static int fail(const char* what) {
    printf("netmuslserv: FAIL (%s, errno=%d)\n", what, errno);
    return 1;
}

int main(void) {
    struct sockaddr_in sa;
    socklen_t sl;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return fail("socket(listen)");

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(PORT);
    sa.sin_addr.s_addr = htonl(0x7F000001u);        /* 127.0.0.1 */
    if (bind(ls, (struct sockaddr*)&sa, sizeof sa) < 0) return fail("bind");
    if (listen(ls, 4) < 0) return fail("listen");

    /* getsockname on the listener: the port we asked for must come back, in
     * network order.  A stack that forgot the byte swap answers 0x8C23. */
    memset(&sa, 0, sizeof sa);
    sl = sizeof sa;
    if (getsockname(ls, (struct sockaddr*)&sa, &sl) < 0) return fail("getsockname");
    if (ntohs(sa.sin_port) != PORT) {
        printf("netmuslserv: FAIL (getsockname port %u, want %u)\n",
               ntohs(sa.sin_port), PORT);
        return 1;
    }

    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return fail("socket(client)");
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(PORT);
    sa.sin_addr.s_addr = htonl(0x7F000001u);
    if (connect(cs, (struct sockaddr*)&sa, sizeof sa) < 0) return fail("connect");

    memset(&sa, 0, sizeof sa);
    sl = sizeof sa;
    int as = accept(ls, (struct sockaddr*)&sa, &sl);
    if (as < 0) return fail("accept");
    if (sl != sizeof sa) {
        printf("netmuslserv: FAIL (accept addrlen %u, want %u)\n",
               (unsigned)sl, (unsigned)sizeof sa);
        return 1;
    }
    if (ntohl(sa.sin_addr.s_addr) != 0x7F000001u) {
        printf("netmuslserv: FAIL (peer address 0x%08x)\n",
               (unsigned)ntohl(sa.sin_addr.s_addr));
        return 1;
    }
    printf("netmuslserv: accepted from %s:%u\n",
           inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));

    /* getpeername on the accepted socket must agree with what accept said —
     * they come from different code paths and disagreeing is the bug. */
    struct sockaddr_in pa;
    memset(&pa, 0, sizeof pa);
    sl = sizeof pa;
    if (getpeername(as, (struct sockaddr*)&pa, &sl) < 0) return fail("getpeername");
    if (pa.sin_port != sa.sin_port || pa.sin_addr.s_addr != sa.sin_addr.s_addr) {
        printf("netmuslserv: FAIL (getpeername disagrees with accept)\n");
        return 1;
    }

    static const char msg[] = "the quick brown fox";
    if (write(cs, msg, sizeof msg - 1) != (ssize_t)(sizeof msg - 1))
        return fail("write");

    /* Half-close the writing end: the server must still be able to read what
     * was already sent, and then see a clean end of stream.  A shutdown that
     * merely returned 0 without sending anything would pass the write above
     * and hang on the read below. */
    if (shutdown(cs, SHUT_WR) < 0) return fail("shutdown");

    char buf[64];
    ssize_t n = read(as, buf, sizeof buf);
    if (n != (ssize_t)(sizeof msg - 1) || memcmp(buf, msg, (size_t)n) != 0) {
        printf("netmuslserv: FAIL (read %d bytes)\n", (int)n);
        return 1;
    }
    n = read(as, buf, sizeof buf);
    if (n != 0) {
        printf("netmuslserv: FAIL (expected EOF after shutdown, got %d)\n", (int)n);
        return 1;
    }

    close(as); close(cs); close(ls);
    printf("netmuslserv: PASS (bind/listen/accept/getpeername/shutdown via musl)\n");
    return 0;
}
