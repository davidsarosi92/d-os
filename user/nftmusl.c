/* nftmusl.c — §M90 3b falsifier for nf_tables, run by `nfttest`.
 *
 * Speaks NETLINK_NETFILTER directly — the same wire format nft(8) produces
 * through libnftnl, built by hand here so the test needs nothing but musl and
 * runs on every arch (the static nft binary is an aarch64 disk-image artifact).
 * Everything happens on loopback in the caller's namespace, in a table of its
 * own that is deleted at the end:
 *
 *   1. FILTER   — `udp dport 7001 drop` at OUTPUT: a datagram that arrived
 *                 before the rule does not arrive after it, and does again
 *                 once the rule's table is gone.
 *   2. DNAT     — `tcp dport 7002 dnat to 127.0.0.1:7003` at OUTPUT (nat): a
 *                 connect to 7002 is accepted by the listener on 7003, data
 *                 crosses both ways, and the CLIENT still sees its peer as
 *                 7002 (the reply was un-DNATed — the conntrack half).
 *   3. CT STATE — at INPUT, `ct state established,related accept` then
 *                 `udp dport 7005 ct state new drop`: a reply to a datagram
 *                 7005 sent is delivered (established), a fresh datagram to
 *                 7005 from elsewhere is not (new).
 *   4. ROLLBACK — a batch whose second rule names a chain that does not exist
 *                 is refused as a whole: the first rule of it must not exist
 *                 afterwards (the transaction is atomic).
 *
 * Every check can fail; the program prints "nft: PASS" only if all four held.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- the constants (linux/netlink.h, linux/netfilter/nf_tables.h) ---- */
#define NETLINK_NETFILTER 12
#define NLM_F_REQUEST 1
#define NLM_F_ACK     4
#define NLM_F_CREATE  0x400
#define NLMSG_ERROR   2
#define NLA_F_NESTED  0x8000
#define NFNL_SUBSYS_NFTABLES 10
#define NFNL_MSG_BATCH_BEGIN 16
#define NFNL_MSG_BATCH_END   17
#define NFT_MSG_NEWTABLE 0
#define NFT_MSG_DELTABLE 2
#define NFT_MSG_NEWCHAIN 3
#define NFT_MSG_NEWRULE  6
#define NFPROTO_IPV4 2
/* hooks */
#define HOOK_INPUT  1
#define HOOK_OUTPUT 3
/* registers */
#define REG_VERDICT 0
#define REG_1 1
#define REG_2 2
/* verdicts */
#define NF_DROP   0
#define NF_ACCEPT 1
/* expressions' fields */
#define META_L4PROTO 16
#define CT_STATE 0
#define CTS_ESTABLISHED 2
#define CTS_RELATED     4
#define CTS_NEW         8
#define CMP_EQ  0
#define CMP_NEQ 1
#define PAYLOAD_TH 2
#define NAT_DNAT 1

static int fail(const char* what) { printf("nft: FAIL - %s (errno %d)\n", what, errno); return 1; }

/* ---- a message builder -------------------------------------------------- */
static uint8_t g_buf[8192];
static unsigned g_len;
static unsigned g_seq = 100;
static unsigned g_nest[8];
static int g_depth;
static unsigned g_msg;                     /* offset of the message being built */

static void put(const void* p, unsigned n) { memcpy(g_buf + g_len, p, n); g_len += n; }
static void pad(void) { while (g_len & 3) g_buf[g_len++] = 0; }

static void msg_begin(uint16_t type, uint16_t flags, uint8_t family, uint16_t res_id) {
    g_msg = g_len;
    uint32_t hdr[4] = { 0, (uint32_t)type | (uint32_t)flags << 16, g_seq++, 0 };
    put(hdr, 16);
    uint8_t nfg[4] = { family, 0, (uint8_t)(res_id >> 8), (uint8_t)res_id };
    put(nfg, 4);
}
static void msg_end(void) { *(uint32_t*)(g_buf + g_msg) = g_len - g_msg; }

static void attr(uint16_t type, const void* p, uint16_t n) {
    uint16_t h[2] = { (uint16_t)(4 + n), type };
    put(h, 4); put(p, n); pad();
}
static void attr_str(uint16_t type, const char* s) { attr(type, s, (uint16_t)(strlen(s) + 1)); }
static void attr_be32(uint16_t type, uint32_t v) { uint32_t b = htonl(v); attr(type, &b, 4); }
static void nest(uint16_t type) {
    g_nest[g_depth++] = g_len;
    uint16_t h[2] = { 0, (uint16_t)(type | NLA_F_NESTED) };
    put(h, 4);
}
static void nest_end(void) { unsigned at = g_nest[--g_depth]; *(uint16_t*)(g_buf + at) = (uint16_t)(g_len - at); }

/* ---- expressions --------------------------------------------------------- */
static void expr_begin(const char* name) { nest(1 /* LIST_ELEM */); attr_str(1, name); nest(2 /* DATA */); }
static void expr_end(void) { nest_end(); nest_end(); }
static void e_meta_l4proto(void) {
    expr_begin("meta"); attr_be32(1, REG_1); attr_be32(2, META_L4PROTO); expr_end();
}
static void e_cmp(int op, const void* v, uint16_t n) {
    expr_begin("cmp"); attr_be32(1, REG_1); attr_be32(2, (uint32_t)op);
    nest(3); attr(1, v, n); nest_end(); expr_end();
}
static void e_th_dport(void) {
    expr_begin("payload"); attr_be32(1, REG_1); attr_be32(2, PAYLOAD_TH);
    attr_be32(3, 2); attr_be32(4, 2); expr_end();
}
static void e_verdict(int code) {
    expr_begin("immediate"); attr_be32(1, REG_VERDICT);
    nest(2); nest(2 /* VERDICT */); attr_be32(1, (uint32_t)code); nest_end(); nest_end(); expr_end();
}
static void e_imm(int reg, const void* v, uint16_t n) {
    expr_begin("immediate"); attr_be32(1, (uint32_t)reg); nest(2); attr(1, v, n); nest_end(); expr_end();
}
static void e_ct_state(void) { expr_begin("ct"); attr_be32(1, REG_1); attr_be32(2, CT_STATE); expr_end(); }
static void e_and(uint32_t mask) {               /* reg1 = reg1 & mask (host order, as ct state is) */
    uint32_t x = 0;
    expr_begin("bitwise"); attr_be32(1, REG_1); attr_be32(2, REG_1); attr_be32(3, 4);
    nest(4); attr(1, &mask, 4); nest_end(); nest(5); attr(1, &x, 4); nest_end(); expr_end();
}
static void e_th_byte(int off) {                 /* reg1 = transport header byte `off` */
    expr_begin("payload"); attr_be32(1, REG_1); attr_be32(2, PAYLOAD_TH);
    attr_be32(3, (uint32_t)off); attr_be32(4, 1); expr_end();
}
static void e_and8(uint8_t mask) {
    uint8_t x = 0;
    expr_begin("bitwise"); attr_be32(1, REG_1); attr_be32(2, REG_1); attr_be32(3, 1);
    nest(4); attr(1, &mask, 1); nest_end(); nest(5); attr(1, &x, 1); nest_end(); expr_end();
}
static void e_dnat(void) {
    expr_begin("nat"); attr_be32(1, NAT_DNAT); attr_be32(2, NFPROTO_IPV4);
    attr_be32(3, REG_1); attr_be32(5, REG_2); attr_be32(7, 3 /* MAP_IPS | PROTO_SPECIFIED */);
    expr_end();
}
static void e_counter(void) { expr_begin("counter"); expr_end(); }

/* ---- batches ------------------------------------------------------------- */
static void batch_begin(void) {
    g_len = 0;
    msg_begin(NFNL_MSG_BATCH_BEGIN, NLM_F_REQUEST, 0, NFNL_SUBSYS_NFTABLES); msg_end();
}
static void batch_end(void) { msg_begin(NFNL_MSG_BATCH_END, NLM_F_REQUEST, 0, NFNL_SUBSYS_NFTABLES); msg_end(); }
static void m(int type) {
    msg_begin((uint16_t)(NFNL_SUBSYS_NFTABLES << 8 | type), NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK,
              NFPROTO_IPV4, 0);
}
static void new_table(void) { m(NFT_MSG_NEWTABLE); attr_str(1, "nfttest"); msg_end(); }
static void new_chain(const char* name, const char* type, int hook, int prio) {
    m(NFT_MSG_NEWCHAIN); attr_str(1, "nfttest"); attr_str(3, name);
    nest(4); attr_be32(1, (uint32_t)hook); attr_be32(2, (uint32_t)prio); nest_end();
    attr_be32(5, NF_ACCEPT); attr_str(7, type); msg_end();
}
static void rule_begin(const char* chain) {
    m(NFT_MSG_NEWRULE); attr_str(1, "nfttest"); attr_str(2, chain); nest(4);
}
static void rule_end(void) { nest_end(); msg_end(); }

/* Send the batch; collect one ack per NLM_F_ACK message.  Returns the first
 * error (negative errno) or 0. */
static int g_nl;
static void drain(void) {                       /* answers a refused batch left behind */
    uint8_t r[8192];
    while (recv(g_nl, r, sizeof r, MSG_DONTWAIT) > 0) { }
}
static int submit(int expect_acks) {
    drain();
    if (send(g_nl, g_buf, g_len, 0) != (ssize_t)g_len) return -errno;
    int got = 0, first_err = 0;
    while (got < expect_acks) {
        struct pollfd pf = { g_nl, POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0) return first_err ? first_err : -ETIMEDOUT;
        uint8_t r[8192];
        ssize_t n = recv(g_nl, r, sizeof r, 0);
        if (n <= 0) return -EIO;
        for (ssize_t o = 0; o + 16 <= n;) {
            uint32_t len = *(uint32_t*)(r + o);
            uint16_t type = *(uint16_t*)(r + o + 4);
            if (len < 16) break;
            if (type == NLMSG_ERROR) {
                int e = *(int*)(r + o + 16);
                if (e && !first_err) first_err = e;
                got++;
                /* a refused batch is answered by its first error alone */
                if (e) return first_err;
            }
            o += (len + 3) & ~3u;
        }
    }
    return first_err;
}
static int del_table(void) {
    batch_begin(); m(NFT_MSG_DELTABLE); attr_str(1, "nfttest"); msg_end(); batch_end();
    return submit(1);
}

/* ---- the sockets --------------------------------------------------------- */
static int udp_bound(int port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(0x7f000001);
    if (s < 0 || bind(s, (struct sockaddr*)&a, sizeof a) != 0) return -1;
    return s;
}
static int udp_to(int s, int port, const char* text) {
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(0x7f000001);
    return (int)sendto(s, text, strlen(text), 0, (struct sockaddr*)&a, sizeof a);
}
static int readable(int fd, int ms) {
    struct pollfd pf = { fd, POLLIN, 0 };
    return poll(&pf, 1, ms) > 0;
}
/* 1 if a datagram arrives within `ms`, with its source port in *from */
static int udp_got(int s, int ms, int* from) {
    struct pollfd pf = { s, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0) return 0;
    char b[64];
    struct sockaddr_in a; socklen_t al = sizeof a;
    if (recvfrom(s, b, sizeof b, 0, (struct sockaddr*)&a, &al) <= 0) return 0;
    if (from) *from = ntohs(a.sin_port);
    return 1;
}

/* ---- ctnetlink (subsystem 1) ---------------------------------------------- */
/* nested attribute `type` inside [p, p+len) */
static const uint8_t* nla(const uint8_t* p, int len, int type, int* out_len) {
    for (int o = 0; o + 4 <= len;) {
        uint16_t l = *(const uint16_t*)(p + o), t = *(const uint16_t*)(p + o + 2) & 0x3fff;
        if (l < 4 || o + l > len) break;
        if (t == type) { *out_len = l - 4; return p + o + 4; }
        o += (l + 3) & ~3;
    }
    return NULL;
}
/* Dump the IPv4 conntrack table; copy out the whole CTA_TUPLE_ORIG attribute
 * of the UDP flow sport -> dport (*flen 0 = not found).  <0 = errno. */
static int ct_dump_find(int sport, int dport, uint8_t* out, int* flen) {
    *flen = 0;
    drain();
    g_len = 0;
    msg_begin((uint16_t)(1 << 8 | 1 /* IPCTNL_MSG_CT_GET */), NLM_F_REQUEST | 0x300 /* DUMP */, NFPROTO_IPV4, 0);
    msg_end();
    if (send(g_nl, g_buf, g_len, 0) != (ssize_t)g_len) return -errno;
    for (;;) {
        struct pollfd pf = { g_nl, POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0) return -ETIMEDOUT;
        static uint8_t r[16384];
        ssize_t n = recv(g_nl, r, sizeof r, 0);
        if (n <= 0) return -EIO;
        for (ssize_t o = 0; o + 16 <= n;) {
            uint32_t len = *(uint32_t*)(r + o);
            uint16_t type = *(uint16_t*)(r + o + 4);
            if (len < 16) break;
            if (type == 3 /* DONE */) return 0;
            if (type == NLMSG_ERROR) return *(int*)(r + o + 16);
            if (type == (1 << 8 | 0) && len > 20) {
                const uint8_t* a = r + o + 20; int al = (int)len - 20, tl, pl, l;
                const uint8_t* t = nla(a, al, 1 /* ORIG */, &tl);
                const uint8_t* pr = t ? nla(t, tl, 2, &pl) : NULL;
                const uint8_t* num = pr ? nla(pr, pl, 1, &l) : NULL;
                const uint8_t* sp = pr ? nla(pr, pl, 2, &l) : NULL;
                const uint8_t* dp = pr ? nla(pr, pl, 3, &l) : NULL;
                if (num && sp && dp && num[0] == 17 && (sp[0] << 8 | sp[1]) == sport &&
                    (dp[0] << 8 | dp[1]) == dport && tl + 4 <= 256) {
                    memcpy(out, t - 4, (size_t)tl + 4);       /* the attribute, header included */
                    *flen = tl + 4;
                }
            }
            o += (len + 3) & ~3u;
        }
    }
}
static int ct_delete(const uint8_t* tuple_attr, int n) {
    g_len = 0;
    msg_begin((uint16_t)(1 << 8 | 2 /* IPCTNL_MSG_CT_DELETE */), NLM_F_REQUEST | NLM_F_ACK, NFPROTO_IPV4, 0);
    put(tuple_attr, (unsigned)n); pad();
    msg_end();
    return submit(1);
}

int main(void) {
    g_nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER);
    if (g_nl < 0) return fail("socket(AF_NETLINK, NETLINK_NETFILTER)");
    del_table();                                       /* a leftover from an aborted run */

    /* ---- 1. filter ---- */
    int rx = udp_bound(7001), tx = udp_bound(7010);
    if (rx < 0 || tx < 0) return fail("udp bind");
    udp_to(tx, 7001, "before");
    if (!udp_got(rx, 500, NULL)) return fail("baseline datagram did not arrive");
    uint8_t udp = 17, tcp = 6;
    uint16_t p7001 = htons(7001);
    batch_begin();
    new_table();
    new_chain("out", "filter", HOOK_OUTPUT, 0);
    rule_begin("out"); e_meta_l4proto(); e_cmp(CMP_EQ, &udp, 1); e_th_dport(); e_cmp(CMP_EQ, &p7001, 2);
    e_counter(); e_verdict(NF_DROP); rule_end();
    batch_end();
    int e = submit(3);
    if (e) { errno = -e; return fail("filter batch refused"); }
    udp_to(tx, 7001, "during");
    if (udp_got(rx, 500, NULL)) return fail("a datagram to a dropped port arrived");
    if (del_table()) return fail("delete table");
    udp_to(tx, 7001, "after");
    if (!udp_got(rx, 500, NULL)) return fail("the drop outlived its table");
    printf("nft: filter - dropped while the rule existed, delivered before and after\n");

    /* ---- 2. DNAT ---- */
    uint16_t p7002 = htons(7002), p7003 = htons(7003);
    uint32_t lo = htonl(0x7f000001);
    batch_begin();
    new_table();
    new_chain("natout", "nat", HOOK_OUTPUT, -100);
    rule_begin("natout"); e_meta_l4proto(); e_cmp(CMP_EQ, &tcp, 1); e_th_dport(); e_cmp(CMP_EQ, &p7002, 2);
    e_imm(REG_1, &lo, 4); e_imm(REG_2, &p7003, 2); e_dnat(); rule_end();
    /* The translated packet must stay ONE connection at every later hook: an
     * ACK to 7003 that conntrack thinks is NEW (a second connection started by
     * the already-DNATed packet, which then never sees its reply) is dropped
     * here — so the data exchange below fails if the lookup splits it. */
    new_chain("in", "filter", HOOK_INPUT, 0);
    {
        uint32_t zero4 = 0, isnew4 = CTS_NEW; uint8_t zero1 = 0;
        rule_begin("in"); e_meta_l4proto(); e_cmp(CMP_EQ, &tcp, 1); e_th_dport(); e_cmp(CMP_EQ, &p7003, 2);
        e_ct_state(); e_and(isnew4); e_cmp(CMP_NEQ, &zero4, 4);
        e_th_byte(13); e_and8(0x10 /* ACK */); e_cmp(CMP_NEQ, &zero1, 1);
        e_counter(); e_verdict(NF_DROP); rule_end();
    }
    batch_end();
    e = submit(5);
    if (e) { errno = -e; return fail("dnat batch refused"); }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET; a.sin_port = p7003; a.sin_addr.s_addr = lo;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr*)&a, sizeof a) || listen(ls, 4)) return fail("listen 7003");
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_port = p7002;
    if (connect(cs, (struct sockaddr*)&a, sizeof a) != 0) return fail("connect to the DNATed port");
    if (!readable(ls, 3000)) return fail("no connection behind the DNAT (handshake ACK dropped?)");
    int as = accept(ls, NULL, NULL);
    if (as < 0) return fail("accept behind the DNAT");
    char b[16] = { 0 };
    if (write(cs, "ping", 4) != 4 || !readable(as, 3000) || read(as, b, sizeof b) != 4 || memcmp(b, "ping", 4))
        return fail("data to the server");
    if (write(as, "pong", 4) != 4 || !readable(cs, 3000) || read(cs, b, sizeof b) != 4 || memcmp(b, "pong", 4))
        return fail("data back (un-DNAT)");
    struct sockaddr_in peer; socklen_t pl = sizeof peer;
    getpeername(cs, (struct sockaddr*)&peer, &pl);
    if (peer.sin_port != p7002) return fail("the client's peer is not the address it dialled");
    close(cs); close(as); close(ls);
    if (del_table()) return fail("delete table");
    printf("nft: dnat - 7002 reached the listener on 7003, replies came back as 7002\n");

    /* ---- 3. ct state ---- */
    uint16_t p7005 = htons(7005);
    uint32_t estrel = CTS_ESTABLISHED | CTS_RELATED, zero = 0, isnew = CTS_NEW;
    batch_begin();
    new_table();
    new_chain("in", "filter", HOOK_INPUT, 0);
    rule_begin("in"); e_ct_state(); e_and(estrel); e_cmp(CMP_NEQ, &zero, 4); e_verdict(NF_ACCEPT); rule_end();
    rule_begin("in"); e_meta_l4proto(); e_cmp(CMP_EQ, &udp, 1); e_th_dport(); e_cmp(CMP_EQ, &p7005, 2);
    e_ct_state(); e_and(isnew); e_cmp(CMP_NEQ, &zero, 4); e_verdict(NF_DROP); rule_end();
    batch_end();
    e = submit(4);
    if (e) { errno = -e; return fail("ct batch refused"); }
    int s5 = udp_bound(7005), s6 = udp_bound(7006), s7 = udp_bound(7007);
    if (s5 < 0 || s6 < 0 || s7 < 0) return fail("udp bind 7005-7007");
    udp_to(s5, 7006, "hello");                         /* 7005 starts a conversation */
    int from = 0;
    if (!udp_got(s6, 500, &from) || from != 7005) return fail("7005's datagram to 7006");
    udp_to(s6, 7005, "reply");                         /* ... the answer is established */
    if (!udp_got(s5, 500, NULL)) return fail("an established reply was dropped");
    udp_to(s7, 7005, "stranger");                      /* a new one to 7005 is not */
    if (udp_got(s5, 500, NULL)) return fail("a new datagram to 7005 got through");
    close(s5); close(s6); close(s7);
    if (del_table()) return fail("delete table");
    printf("nft: ct state - the reply was established and passed, a new datagram was dropped\n");

    /* ---- 4. rollback ---- */
    batch_begin();
    new_table();
    new_chain("out", "filter", HOOK_OUTPUT, 0);
    rule_begin("out"); e_meta_l4proto(); e_cmp(CMP_EQ, &udp, 1); e_th_dport(); e_cmp(CMP_EQ, &p7001, 2);
    e_verdict(NF_DROP); rule_end();
    rule_begin("nosuchchain"); e_verdict(NF_DROP); rule_end();
    batch_end();
    e = submit(4);
    if (e == 0) return fail("a rule in a missing chain was accepted");
    udp_to(tx, 7001, "rolled back");
    if (!udp_got(rx, 500, NULL)) return fail("a refused batch left its first rule in force");
    if (del_table() == 0) return fail("a refused batch left its table behind");
    printf("nft: rollback - the refused batch (%d) left nothing behind\n", e);

    /* ---- 5. ctnetlink: the conntrack table as dockerd reads and prunes it ---- */
    {
        uint8_t tup[256]; int tlen = 0;
        int e5 = ct_dump_find(7005, 7006, tup, &tlen);
        if (e5 < 0) { errno = -e5; return fail("conntrack dump"); }
        if (!tlen) return fail("the 7005->7006 flow is not in the conntrack dump");
        if (ct_delete(tup, tlen) != 0) return fail("conntrack delete by the dumped tuple");
        uint8_t again[256]; int alen2 = 0;
        if (ct_dump_find(7005, 7006, again, &alen2) < 0 || alen2) return fail("the deleted flow is still dumped");
        int e6 = ct_delete(tup, tlen);
        if (e6 != -ENOENT) { errno = -e6; return fail("deleting it twice is not ENOENT"); }
        printf("nft: ctnetlink - the 7005->7006 flow was dumped, deleted by its tuple, gone, and a second delete is ENOENT\n");
    }

    printf("nft: PASS\n");
    return 0;
}
