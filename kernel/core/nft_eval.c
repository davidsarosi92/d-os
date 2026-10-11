/* =============================================================================
 * nft_eval.c — the nf_tables rule VM, §M90 3b.
 *
 * A rule is a list of expressions over a REGISTER FILE (Linux's layout: the
 * verdict, then twenty 32-bit words; the legacy 128-bit registers 1-4 are
 * words 4-19 in fours, NFT_REG32_00.. are words 4.. one each).  Each
 * expression is COMPILED once, when the rule is made, into a `struct nft_op`;
 * evaluation walks the ops until one breaks the rule (a comparison failed, a
 * lookup missed) or sets a verdict.
 *
 * IMPLEMENTED: immediate, cmp, range, bitwise (bool and shifts), byteorder,
 * payload (load; link/network/transport header), meta (the keys a ruleset
 * filters on: lengths, protocols, interfaces by index/name/type/kind, mark,
 * pkttype, cpu, skuid/skgid), counter, lookup (sets and maps, verdict maps,
 * intervals), ct (state, direction, status, mark, addresses, ports, protocol),
 * nat (snat/dnat), masq, redir, fib (daddr type local/oif), log (to the kernel
 * log), reject (drops; the RST/ICMP it would send is NOT sent — said), notrack
 * (accepted, nothing to skip).  Anything else REFUSES the rule (EOPNOTSUPP,
 * named in the log): a rule silently not enforced is the worst outcome.
 * ============================================================================= */

#include "nftables.h"
#include "nft_vm.h"
#include "net.h"
#include "kmalloc.h"
#include "printf.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}
static int s_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void s_cpy(char* d, const uint8_t* s, uint16_t len, int cap) {
    int i = 0;
    for (; i < len && i + 1 < cap && s[i]; i++) d[i] = (char)s[i];
    d[i] = 0;
}

/* NFT_REG_* → a word index into the register file (Linux's nft_parse_register). */
static int reg_word(uint32_t r) {
    if (r == 0) return 0;                       /* the verdict */
    if (r <= 4) return (int)(r * 4);            /* 128-bit NFT_REG_1..4 */
    if (r >= 8 && r <= 23) return (int)(r - 8 + 4);
    return -1;
}

/* NFTA_DATA: a value (copied) or a verdict (code + chain). */
static int parse_data(const uint8_t* d, uint16_t dl, uint8_t* val, uint8_t* vlen, int32_t* verdict,
                      char* chain, int* is_verdict) {
    uint16_t l;
    const uint8_t* v = nla_find(d, dl, 1 /* VALUE */, &l);
    if (v) {
        if (l > NFT_KEY_MAX) return -22;
        for (uint16_t i = 0; i < l; i++) val[i] = v[i];
        *vlen = (uint8_t)l;
        if (is_verdict) *is_verdict = 0;
        return 0;
    }
    const uint8_t* vd = nla_find(d, dl, 2 /* VERDICT */, &l);
    if (vd && verdict) {
        *verdict = (int32_t)nla_be32(vd, l, 1, 0);
        uint16_t cl; const uint8_t* c = nla_find(vd, l, 2, &cl);
        if (chain) { if (c) s_cpy(chain, c, cl, NFT_NAME_MAX); else chain[0] = 0; }
        if (is_verdict) *is_verdict = 1;
        return 0;
    }
    return -22;
}

static int known_meta(uint32_t k) {
    switch (k) {
    case NFT_META_LEN: case NFT_META_PROTOCOL: case NFT_META_MARK: case NFT_META_IIF: case NFT_META_OIF:
    case NFT_META_IIFNAME: case NFT_META_OIFNAME: case NFT_META_IIFTYPE: case NFT_META_OIFTYPE:
    case NFT_META_SKUID: case NFT_META_SKGID: case NFT_META_NFPROTO: case NFT_META_L4PROTO:
    case NFT_META_PKTTYPE: case NFT_META_CPU: case NFT_META_IIFKIND: case NFT_META_OIFKIND:
    case NFT_META_PRIORITY: case NFT_META_IIFGROUP: case NFT_META_OIFGROUP: case NFT_META_PRANDOM:
        return 1;
    }
    return 0;
}
static int known_ct(uint32_t k) {
    switch (k) {
    case NFT_CT_STATE: case NFT_CT_DIRECTION: case NFT_CT_STATUS: case NFT_CT_MARK:
    case NFT_CT_L3PROTOCOL: case NFT_CT_SRC: case NFT_CT_DST: case NFT_CT_PROTOCOL:
    case NFT_CT_PROTO_SRC: case NFT_CT_PROTO_DST: case NFT_CT_SRC_IP: case NFT_CT_DST_IP:
    case NFT_CT_SRC_IP6: case NFT_CT_DST_IP6: case NFT_CT_ZONE:
        return 1;
    }
    return 0;
}

static uint32_t g_refused;

/* One expression → one op.  0, or a negative errno. */
static int compile_one(struct nft_table* t, const uint8_t* e, uint16_t el, struct nft_op* op) {
    uint16_t l;
    const uint8_t* nm = nla_find(e, el, 1 /* NFTA_EXPR_NAME */, &l);
    if (!nm) return -22;
    char name[24]; s_cpy(name, nm, l, sizeof name);
    uint16_t dl = 0;
    const uint8_t* d = nla_find(e, el, 2 /* NFTA_EXPR_DATA */, &dl);
    if (!d) { d = e; dl = 0; }
    #define U(attr, def) nla_be32(d, dl, attr, def)
    #define REG(attr) reg_word(U(attr, 0))
    op->kind = OP_NONE;
    if (s_eq(name, "immediate")) {
        op->kind = OP_IMM;
        op->dreg = REG(1);
        const uint8_t* dd = nla_find(d, dl, 2, &l);
        if (!dd || op->dreg < 0) return -22;
        int isv = 0;
        if (parse_data(dd, l, op->data, &op->len, &op->verdict, op->name, &isv)) return -22;
        op->is_verdict = (uint8_t)isv;
        return 0;
    }
    if (s_eq(name, "cmp")) {
        op->kind = OP_CMP;
        op->sreg = REG(1); op->sub = (uint8_t)U(2, 0);
        const uint8_t* dd = nla_find(d, dl, 3, &l);
        if (!dd || op->sreg < 0 || parse_data(dd, l, op->data, &op->len, NULL, NULL, NULL)) return -22;
        return op->sub <= NFT_CMP_GTE ? 0 : -22;
    }
    if (s_eq(name, "range")) {
        op->kind = OP_RANGE;
        op->sreg = REG(1); op->sub = (uint8_t)U(2, 0);
        const uint8_t* f = nla_find(d, dl, 3, &l);
        uint8_t fl = 0, tl = 0;
        if (!f || parse_data(f, l, op->data, &fl, NULL, NULL, NULL)) return -22;
        const uint8_t* tt = nla_find(d, dl, 4, &l);
        if (!tt || parse_data(tt, l, op->data2, &tl, NULL, NULL, NULL)) return -22;
        op->len = fl;
        return op->sreg >= 0 ? 0 : -22;
    }
    if (s_eq(name, "bitwise")) {
        op->kind = OP_BITWISE;
        op->sreg = REG(1); op->dreg = REG(2); op->len = (uint8_t)U(3, 0); op->sub = (uint8_t)U(6, 0);
        if (op->sub == NFT_BITWISE_BOOL) {
            const uint8_t* mk = nla_find(d, dl, 4, &l);
            uint8_t ml = 0, xl = 0;
            if (!mk || parse_data(mk, l, op->data, &ml, NULL, NULL, NULL)) return -22;
            const uint8_t* xr = nla_find(d, dl, 5, &l);
            if (!xr || parse_data(xr, l, op->data2, &xl, NULL, NULL, NULL)) return -22;
        } else {
            const uint8_t* sh = nla_find(d, dl, 7, &l);
            uint8_t sl = 0;
            if (!sh || parse_data(sh, l, op->data, &sl, NULL, NULL, NULL) || sl < 4) return -22;
            op->x = be32(op->data);
            /* the shift amount is a host-order u32 in the attribute */
            op->x = (uint32_t)op->data[0] | (uint32_t)op->data[1] << 8 | (uint32_t)op->data[2] << 16 | (uint32_t)op->data[3] << 24;
        }
        return op->sreg >= 0 && op->dreg >= 0 && op->len <= NFT_KEY_MAX ? 0 : -22;
    }
    if (s_eq(name, "byteorder")) {
        op->kind = OP_BYTEORDER;
        op->sreg = REG(1); op->dreg = REG(2); op->sub = (uint8_t)U(3, 0);
        op->len = (uint8_t)U(4, 0); op->x = U(5, 0);
        return op->sreg >= 0 && op->dreg >= 0 && (op->x == 2 || op->x == 4 || op->x == 8) ? 0 : -22;
    }
    if (s_eq(name, "payload")) {
        if (nla_find(d, dl, 5 /* SREG: a write */, &l)) {
            kprintf("nft: payload STORE (packet mangling) is not implemented - rule refused\n");
            g_refused++;
            return -95;
        }
        op->kind = OP_PAYLOAD;
        op->dreg = REG(1); op->sub = (uint8_t)U(2, 0); op->x = U(3, 0); op->len = (uint8_t)U(4, 0);
        if (op->sub > NFT_PAYLOAD_TRANSPORT_HEADER) { kprintf("nft: payload base %d refused\n", op->sub); return -95; }
        return op->dreg >= 0 && op->len <= NFT_KEY_MAX ? 0 : -22;
    }
    if (s_eq(name, "meta")) {
        if (nla_find(d, dl, 3 /* SREG: meta set */, &l)) {
            uint32_t k = U(2, 0);
            if (k != NFT_META_MARK) { kprintf("nft: meta set key %u refused\n", k); return -95; }
            op->kind = OP_META_SET; op->sreg = REG(3); op->x = k;
            return op->sreg >= 0 ? 0 : -22;
        }
        op->kind = OP_META; op->dreg = REG(1); op->x = U(2, 0);
        if (!known_meta(op->x)) { kprintf("nft: meta key %u refused - not implemented\n", op->x); g_refused++; return -95; }
        return op->dreg >= 0 ? 0 : -22;
    }
    if (s_eq(name, "counter")) {
        op->kind = OP_COUNTER;
        op->bytes = 0; op->packets = 0;
        return 0;
    }
    if (s_eq(name, "lookup")) {
        op->kind = OP_LOOKUP;
        const uint8_t* sn = nla_find(d, dl, 1, &l);
        if (sn) s_cpy(op->name, sn, l, sizeof op->name);
        else {                                             /* an anonymous set by its batch id */
            uint32_t id = U(4, 0);
            struct nft_set* s = NULL;
            for (struct nft_set* x = t->sets; x; x = x->next) if (x->id == id) s = x;
            if (!s) return -2;
            for (int i = 0; s->name[i]; i++) op->name[i] = s->name[i];
        }
        if (!nft_set_find(t, op->name, 0)) return -2;
        op->sreg = REG(2);
        op->dreg = nla_find(d, dl, 3, &l) ? REG(3) : -1;
        op->sub = (uint8_t)(U(5, 0) & 1);               /* NFT_LOOKUP_F_INV */
        return op->sreg >= 0 ? 0 : -22;
    }
    if (s_eq(name, "ct")) {
        if (nla_find(d, dl, 4 /* SREG: ct set */, &l)) {
            uint32_t k = U(2, 0);
            if (k != NFT_CT_MARK) { kprintf("nft: ct set key %u refused\n", k); return -95; }
            op->kind = OP_CT_SET; op->sreg = REG(4); op->x = k;
            return op->sreg >= 0 ? 0 : -22;
        }
        op->kind = OP_CT; op->dreg = REG(1); op->x = U(2, 0);
        op->sub = nla_find(d, dl, 3, &l) ? (uint8_t)(l ? *nla_find(d, dl, 3, &l) : 0) + 1 : 0;   /* direction+1, 0 = packet's */
        if (!known_ct(op->x)) { kprintf("nft: ct key %u refused - not implemented\n", op->x); g_refused++; return -95; }
        return op->dreg >= 0 ? 0 : -22;
    }
    if (s_eq(name, "nat")) {
        op->kind = OP_NAT;
        op->sub = (uint8_t)U(1, 0);                       /* SNAT / DNAT */
        op->x = U(2, 0);                                  /* family */
        op->sreg = nla_find(d, dl, 3, &l) ? REG(3) : -1;  /* addr min */
        op->sreg2 = nla_find(d, dl, 4, &l) ? REG(4) : op->sreg;
        op->dreg = nla_find(d, dl, 5, &l) ? REG(5) : -1;  /* proto min (reused field) */
        op->dreg2 = nla_find(d, dl, 6, &l) ? REG(6) : op->dreg;
        op->y = U(7, 0);                                  /* NF_NAT_RANGE_* flags */
        return 0;
    }
    if (s_eq(name, "masq")) {
        op->kind = OP_MASQ;
        op->y = U(1, 0);
        op->dreg = nla_find(d, dl, 2, &l) ? REG(2) : -1;
        op->dreg2 = nla_find(d, dl, 3, &l) ? REG(3) : op->dreg;
        return 0;
    }
    if (s_eq(name, "redir")) {
        op->kind = OP_REDIR;
        op->dreg = nla_find(d, dl, 1, &l) ? REG(1) : -1;
        op->dreg2 = nla_find(d, dl, 2, &l) ? REG(2) : op->dreg;
        op->y = U(3, 0);
        return 0;
    }
    if (s_eq(name, "fib")) {
        op->kind = OP_FIB;
        op->dreg = REG(1); op->sub = (uint8_t)U(2, 0); op->y = U(3, 0);
        if (op->sub != NFT_FIB_RESULT_ADDRTYPE && op->sub != NFT_FIB_RESULT_OIF && op->sub != NFT_FIB_RESULT_OIFNAME) {
            kprintf("nft: fib result %d refused\n", op->sub); return -95;
        }
        return op->dreg >= 0 ? 0 : -22;
    }
    if (s_eq(name, "log"))     { op->kind = OP_LOG; return 0; }
    if (s_eq(name, "reject"))  { op->kind = OP_REJECT; return 0; }
    if (s_eq(name, "notrack")) { op->kind = OP_NOP; return 0; }
    kprintf("nft: expression '%s' refused - not implemented here\n", name);
    g_refused++;
    return -95;
    #undef U
    #undef REG
}

int nft_rule_compile(struct nft_table* t, struct nft_rule* r, const uint8_t* exprs, uint32_t len) {
    int n = 0;
    for (;; n++) { uint16_t l; if (!nla_nth(exprs, len, 1, n, &l)) break; }
    if (n > NFT_MAX_OPS) { kprintf("nft: rule with %d expressions refused (max %d)\n", n, NFT_MAX_OPS); return -22; }
    r->prog = n ? (struct nft_op*)kcalloc((size_t)n, sizeof(struct nft_op)) : NULL;
    if (n && !r->prog) return -12;
    r->nprog = n;
    for (int i = 0; i < n; i++) {
        uint16_t l; const uint8_t* e = nla_nth(exprs, len, 1, i, &l);
        int rc = compile_one(t, e, l, &r->prog[i]);
        if (rc) { kfree(r->prog); r->prog = NULL; r->nprog = 0; return rc; }
    }
    return 0;
}
void nft_rule_uncompile(struct nft_rule* r) { if (r) { kfree(r->prog); r->prog = NULL; r->nprog = 0; } }
void nft_rule_copy_compiled(struct nft_rule* dst, const struct nft_rule* src) {
    dst->prog = NULL; dst->nprog = 0;
    if (!src->nprog) return;
    dst->prog = (struct nft_op*)kmalloc(sizeof(struct nft_op) * (size_t)src->nprog);
    if (!dst->prog) return;
    for (int i = 0; i < src->nprog; i++) dst->prog[i] = src->prog[i];
    dst->nprog = src->nprog;
}
/* A dump shows the counters' LIVE values: the stored request carried zeros. */
void nft_rule_patch_counters(const struct nft_rule* r, uint8_t* exprs, uint32_t len) {
    int k = 0;
    for (int i = 0; i < r->nprog; i++) {
        uint16_t l; uint8_t* e = (uint8_t*)nla_nth(exprs, len, 1, i, &l);
        if (!e || r->prog[i].kind != OP_COUNTER) continue;
        uint16_t dl; uint8_t* d = (uint8_t*)nla_find(e, l, 2, &dl);
        if (!d) continue;
        uint16_t vl;
        uint8_t* b = (uint8_t*)nla_find(d, dl, 1, &vl);
        if (b && vl == 8) put_be64(b, r->prog[i].bytes);
        uint8_t* p = (uint8_t*)nla_find(d, dl, 2, &vl);
        if (p && vl == 8) put_be64(p, r->prog[i].packets);
        k++;
    }
    (void)k;
}
/* Is expression `i` a counter?  Its live values. */
int nft_rule_counter(const struct nft_rule* r, int i, uint64_t* packets, uint64_t* bytes) {
    if (i < 0 || i >= r->nprog || r->prog[i].kind != OP_COUNTER) return 0;
    *packets = r->prog[i].packets; *bytes = r->prog[i].bytes;
    return 1;
}
int nft_rule_refers_chain(const struct nft_rule* r, const char* chain) {
    for (int i = 0; i < r->nprog; i++)
        if (r->prog[i].kind == OP_IMM && r->prog[i].is_verdict &&
            (r->prog[i].verdict == NFT_JUMP || r->prog[i].verdict == NFT_GOTO) && s_eq(r->prog[i].name, chain))
            return 1;
    return 0;
}
int nft_rule_refers_set(const struct nft_rule* r, const char* set) {
    for (int i = 0; i < r->nprog; i++)
        if (r->prog[i].kind == OP_LOOKUP && s_eq(r->prog[i].name, set)) return 1;
    return 0;
}
uint32_t nft_refused_count(void) { return g_refused; }
