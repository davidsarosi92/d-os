/* =============================================================================
 * nftables.c — nf_tables over NETLINK_NETFILTER, §M90 3b.
 *
 * WHY.  A fixed kernel policy (`net.masquerade`) did what Docker's firewall
 * rules do for the default network and nothing else.  Docker — and anybody
 * with `nft` — writes RULES: tables of chains hooked into the packet path,
 * each rule a small program of expressions (load a header field into a
 * register, compare it, look it up in a set, count, NAT, decide).  This file
 * is that machine:
 *
 *   the OBJECT STORE   tables, chains, rules, sets and set elements, per
 *                      network namespace (each namespace has its own ruleset,
 *                      as on Linux), named and numbered by handle;
 *   the NETLINK SIDE   nfnetlink batches (BATCH_BEGIN … BATCH_END) of NEW /
 *                      DEL / DESTROY messages applied as ONE transaction — if
 *                      any message fails, the namespace's ruleset is put back
 *                      exactly as it was before the batch (it is cloned at
 *                      BATCH_BEGIN) — and GET dumps that hand back what was
 *                      stored, so `nft list ruleset` reads the ruleset that is
 *                      in force;
 *   the VM             (nft_eval.c) — rules run at the five IP hooks.
 *
 * WHAT IS STORED.  Each object keeps the ATTRIBUTES it was created with (its
 * request, minus what the kernel assigns: handles and use counts), so a dump
 * is that blob plus the kernel's own attributes — no second encoder that could
 * disagree with the first.  A rule's expressions are also COMPILED (nft_eval.c)
 * at creation, and an expression the VM does not implement REFUSES the rule
 * with EOPNOTSUPP and its name in the log: a rule accepted and then not
 * enforced is a firewall that lies.
 *
 * NOT HERE (said): flowtables, stateful objects (named counters, quotas,
 * limits), the netdev/bridge/arp families, rule tracing, NFNLGRP_NFTABLES
 * notifications (`nft monitor`), set timeouts and element expressions.
 * ============================================================================= */

#include "nftables.h"
#include "nft_hook.h"   /* ctnetlink */
#include "net.h"
#include "kmalloc.h"
#include "printf.h"
#include "task.h"
#include "lock.h"
#include "config.h"
#include <stddef.h>
#include <stdint.h>

/* ---- wire constants ---------------------------------------------------------- */

#define NLMSG_ERROR_      2
#define NLMSG_DONE_       3
#define NLM_F_REQUEST_    0x001
#define NLM_F_MULTI_      0x002
#define NLM_F_ACK_        0x004
#define NLM_F_DUMP_       0x300
#define NLM_F_REPLACE_    0x100
#define NLM_F_EXCL_       0x200
#define NLM_F_CREATE_     0x400
#define NLM_F_APPEND_     0x800
#define NLA_F_NESTED_     0x8000
#define NLA_TYPE_MASK_    0x3fff

#define NFNL_SUBSYS_NFTABLES_ 10
#define NFNL_MSG_BATCH_BEGIN_ 16
#define NFNL_MSG_BATCH_END_   17

enum { M_NEWTABLE, M_GETTABLE, M_DELTABLE, M_NEWCHAIN, M_GETCHAIN, M_DELCHAIN,
       M_NEWRULE, M_GETRULE, M_DELRULE, M_NEWSET, M_GETSET, M_DELSET,
       M_NEWSETELEM, M_GETSETELEM, M_DELSETELEM, M_NEWGEN, M_GETGEN, M_TRACE,
       M_NEWOBJ, M_GETOBJ, M_DELOBJ, M_GETOBJ_RESET, M_NEWFLOWTABLE, M_GETFLOWTABLE,
       M_DELFLOWTABLE, M_GETRULE_RESET, M_DESTROYTABLE, M_DESTROYCHAIN, M_DESTROYRULE,
       M_DESTROYSET, M_DESTROYSETELEM, M_DESTROYOBJ, M_DESTROYFLOWTABLE };

/* attribute numbers (nf_tables.h) */
enum { TA_NAME = 1, TA_FLAGS, TA_USE, TA_HANDLE, TA_PAD, TA_USERDATA, TA_OWNER };
enum { CA_TABLE = 1, CA_HANDLE, CA_NAME, CA_HOOK, CA_POLICY, CA_USE, CA_TYPE, CA_COUNTERS,
       CA_PAD, CA_FLAGS, CA_ID, CA_USERDATA };
enum { HA_HOOKNUM = 1, HA_PRIORITY, HA_DEV, HA_DEVS };
enum { RA_TABLE = 1, RA_CHAIN, RA_HANDLE, RA_EXPRESSIONS, RA_COMPAT, RA_POSITION, RA_USERDATA,
       RA_PAD, RA_ID, RA_POSITION_ID, RA_CHAIN_ID };
enum { SA_TABLE = 1, SA_NAME, SA_FLAGS, SA_KEY_TYPE, SA_KEY_LEN, SA_DATA_TYPE, SA_DATA_LEN,
       SA_POLICY, SA_DESC, SA_ID, SA_TIMEOUT, SA_GC_INTERVAL, SA_USERDATA, SA_PAD, SA_OBJ_TYPE,
       SA_HANDLE, SA_EXPR, SA_EXPRESSIONS };
enum { ELA_TABLE = 1, ELA_SET, ELA_ELEMENTS, ELA_SET_ID };
enum { EA_KEY = 1, EA_DATA, EA_FLAGS, EA_TIMEOUT, EA_EXPIRATION, EA_USERDATA, EA_EXPR, EA_PAD,
       EA_OBJREF, EA_KEY_END, EA_EXPRESSIONS };
enum { GA_ID = 1, GA_PROC_PID, GA_PROC_NAME };

/* ---- the store ----------------------------------------------------------------- */

static struct nft_net g_nets[NFT_NS_MAX];          /* one ruleset per namespace */
static uint32_t g_genid = 1;                        /* bumped by every committed batch */
static spinlock_t g_nft_lock = SPINLOCK_INIT;       /* the RX path reads, a batch writes */

struct nft_net* nft_net_of(int ns) { return ns >= 0 && ns < NFT_NS_MAX ? &g_nets[ns] : NULL; }
uint32_t nft_lock(void)            { return spin_lock_irqsave(&g_nft_lock); }
void     nft_unlock(uint32_t f)    { spin_unlock_irqrestore(&g_nft_lock, f); }

static int s_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void s_cpy(char* d, const uint8_t* s, uint16_t len, int cap) {
    int i = 0;
    for (; i < len && i + 1 < cap && s[i]; i++) d[i] = (char)s[i];
    d[i] = 0;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_be64(uint8_t* p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Attributes: a walk over an area, and a find. */
const uint8_t* nla_find(const uint8_t* a, uint32_t len, uint16_t type, uint16_t* plen) {
    while (len >= 4) {
        uint16_t l = le16(a), t = (uint16_t)(le16(a + 2) & NLA_TYPE_MASK_);
        if (l < 4 || l > len) return NULL;
        if (t == type) { if (plen) *plen = (uint16_t)(l - 4); return a + 4; }
        uint32_t al = (l + 3u) & ~3u;
        if (al >= len) return NULL;
        a += al; len -= al;
    }
    return NULL;
}
/* The n-th attribute of `type` (for lists of NFTA_LIST_ELEM). */
const uint8_t* nla_nth(const uint8_t* a, uint32_t len, uint16_t type, int n, uint16_t* plen) {
    while (len >= 4) {
        uint16_t l = le16(a), t = (uint16_t)(le16(a + 2) & NLA_TYPE_MASK_);
        if (l < 4 || l > len) return NULL;
        if (t == type && n-- == 0) { if (plen) *plen = (uint16_t)(l - 4); return a + 4; }
        uint32_t al = (l + 3u) & ~3u;
        if (al >= len) return NULL;
        a += al; len -= al;
    }
    return NULL;
}
uint32_t nla_be32(const uint8_t* a, uint32_t len, uint16_t type, uint32_t def) {
    uint16_t l = 0; const uint8_t* v = nla_find(a, len, type, &l);
    return v && l >= 4 ? be32(v) : def;
}

/* A copy of an attribute area WITHOUT the listed types (what the kernel
 * assigns and re-emits itself). */
static uint8_t* raw_keep(const uint8_t* a, uint32_t len, const uint16_t* drop, int ndrop, uint32_t* out_len) {
    uint8_t* r = (uint8_t*)kmalloc(len ? len : 4);
    if (!r) return NULL;
    uint32_t o = 0;
    while (len >= 4) {
        uint16_t l = le16(a), t = (uint16_t)(le16(a + 2) & NLA_TYPE_MASK_);
        if (l < 4 || l > len) break;
        uint32_t al = (l + 3u) & ~3u;
        if (al > len) al = len;
        int keep = 1;
        for (int i = 0; i < ndrop; i++) if (t == drop[i]) keep = 0;
        if (keep) { for (uint32_t i = 0; i < al; i++) r[o + i] = a[i]; o += al; }
        a += al; len -= al;
    }
    *out_len = o;
    return r;
}
static uint8_t* blob_dup(const uint8_t* p, uint32_t n) {
    uint8_t* r = (uint8_t*)kmalloc(n ? n : 1);
    if (r) for (uint32_t i = 0; i < n; i++) r[i] = p[i];
    return r;
}

/* ---- object helpers ------------------------------------------------------------ */

static struct nft_table* table_find(struct nft_net* n, int fam, const char* name, uint64_t handle) {
    for (struct nft_table* t = n->tables; t; t = t->next)
        if ((fam == 0 || t->family == fam) && (handle ? t->handle == handle : s_eq(t->name, name))) return t;
    return NULL;
}
struct nft_chain* nft_chain_find(struct nft_table* t, const char* name, uint64_t handle) {
    for (struct nft_chain* c = t->chains; c; c = c->next)
        if (handle ? c->handle == handle : s_eq(c->name, name)) return c;
    return NULL;
}
struct nft_set* nft_set_find(struct nft_table* t, const char* name, uint64_t handle) {
    for (struct nft_set* s = t->sets; s; s = s->next)
        if (handle ? s->handle == handle : s_eq(s->name, name)) return s;
    return NULL;
}
static struct nft_set* set_find_id(struct nft_table* t, uint32_t id) {
    for (struct nft_set* s = t->sets; s; s = s->next) if (s->id && s->id == id) return s;
    return NULL;
}

static void elem_free(struct nft_elem* e) { kfree(e->raw); kfree(e); }
static void set_free(struct nft_set* s) {
    for (struct nft_elem* e = s->elems; e; ) { struct nft_elem* nx = e->next; elem_free(e); e = nx; }
    kfree(s->raw); kfree(s);
}
void nft_rule_free(struct nft_rule* r) { nft_rule_uncompile(r); kfree(r->raw); kfree(r); }
static void chain_free(struct nft_chain* c) {
    for (struct nft_rule* r = c->rules; r; ) { struct nft_rule* nx = r->next; nft_rule_free(r); r = nx; }
    kfree(c->raw); kfree(c);
}
static void table_free(struct nft_table* t) {
    for (struct nft_chain* c = t->chains; c; ) { struct nft_chain* nx = c->next; chain_free(c); c = nx; }
    for (struct nft_set* s = t->sets; s; ) { struct nft_set* nx = s->next; set_free(s); s = nx; }
    kfree(t->raw); kfree(t);
}
static void net_free(struct nft_table* list) {
    for (struct nft_table* t = list; t; ) { struct nft_table* nx = t->next; table_free(t); t = nx; }
}

/* A deep copy of a namespace's tables — the transaction's undo.  Rules are
 * re-compiled from their raw attributes (a compiled rule refers to nothing by
 * pointer that survives the copy: sets and chains are named). */
static struct nft_table* net_clone(const struct nft_table* src, int* fail) {
    struct nft_table* head = NULL; struct nft_table** tp = &head;
    for (const struct nft_table* t = src; t; t = t->next) {
        struct nft_table* nt = (struct nft_table*)kcalloc(1, sizeof *nt);
        if (!nt) { *fail = 1; return head; }
        *nt = *t; nt->next = NULL; nt->chains = NULL; nt->sets = NULL;
        nt->raw = blob_dup(t->raw, t->rawlen);
        *tp = nt; tp = &nt->next;
        struct nft_chain** cp = &nt->chains;
        for (const struct nft_chain* c = t->chains; c; c = c->next) {
            struct nft_chain* nc = (struct nft_chain*)kcalloc(1, sizeof *nc);
            if (!nc) { *fail = 1; return head; }
            *nc = *c; nc->next = NULL; nc->rules = NULL;
            nc->raw = blob_dup(c->raw, c->rawlen);
            *cp = nc; cp = &nc->next;
            struct nft_rule** rp = &nc->rules;
            for (const struct nft_rule* r = c->rules; r; r = r->next) {
                struct nft_rule* nr = (struct nft_rule*)kcalloc(1, sizeof *nr);
                if (!nr) { *fail = 1; return head; }
                *nr = *r; nr->next = NULL; nr->prog = NULL; nr->nprog = 0;
                nr->raw = blob_dup(r->raw, r->rawlen);
                nft_rule_copy_compiled(nr, r);
                *rp = nr; rp = &nr->next;
            }
        }
        struct nft_set** sp = &nt->sets;
        for (const struct nft_set* s = t->sets; s; s = s->next) {
            struct nft_set* ns = (struct nft_set*)kcalloc(1, sizeof *ns);
            if (!ns) { *fail = 1; return head; }
            *ns = *s; ns->next = NULL; ns->elems = NULL;
            ns->raw = blob_dup(s->raw, s->rawlen);
            *sp = ns; sp = &ns->next;
            struct nft_elem** ep = &ns->elems;
            for (const struct nft_elem* e = s->elems; e; e = e->next) {
                struct nft_elem* ne = (struct nft_elem*)kcalloc(1, sizeof *ne);
                if (!ne) { *fail = 1; return head; }
                *ne = *e; ne->next = NULL;
                ne->raw = blob_dup(e->raw, e->rawlen);
                *ep = ne; ep = &ne->next;
            }
        }
    }
    return head;
}

/* ---- reply building ------------------------------------------------------------ */

struct nbuf { uint8_t* p; uint32_t n, cap; int oom; };
static void* nb_put(struct nbuf* b, uint32_t len) {
    uint32_t a = (len + 3u) & ~3u;
    if (b->n + a > b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + a) nc *= 2;
        uint8_t* np = (uint8_t*)kmalloc(nc);
        if (!np) { b->oom = 1; return NULL; }
        for (uint32_t i = 0; i < b->n; i++) np[i] = b->p[i];
        kfree(b->p); b->p = np; b->cap = nc;
    }
    uint8_t* r = b->p + b->n;
    for (uint32_t i = 0; i < a; i++) r[i] = 0;
    b->n += a;
    return r;
}
static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32le(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Start a message (nlmsghdr + nfgenmsg); the length is fixed by msg_end. */
static uint32_t msg_begin(struct nbuf* b, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid,
                          uint8_t family, uint32_t res) {
    uint32_t at = b->n;
    uint8_t* h = (uint8_t*)nb_put(b, 20);
    if (!h) return at;
    w16(h + 4, type); w16(h + 6, flags); w32le(h + 8, seq); w32le(h + 12, pid);
    h[16] = family; h[17] = 0; h[18] = (uint8_t)(res >> 8); h[19] = (uint8_t)res;
    return at;
}
static void msg_end(struct nbuf* b, uint32_t at) { if (!b->oom) w32le(b->p + at, b->n - at); }
static void attr(struct nbuf* b, uint16_t type, const void* data, uint32_t len) {
    uint8_t* a = (uint8_t*)nb_put(b, 4 + len);
    if (!a) return;
    w16(a, (uint16_t)(4 + len)); w16(a + 2, type);
    const uint8_t* d = (const uint8_t*)data;
    for (uint32_t i = 0; i < len; i++) a[4 + i] = d[i];
}
static void attr_str(struct nbuf* b, uint16_t type, const char* s) {
    uint32_t l = 0; while (s[l]) l++;
    attr(b, type, s, l + 1);
}
static void attr_be32(struct nbuf* b, uint16_t type, uint32_t v) { uint8_t t[4]; put_be32(t, v); attr(b, type, t, 4); }
static void attr_be64(struct nbuf* b, uint16_t type, uint64_t v) { uint8_t t[8]; put_be64(t, v); attr(b, type, t, 8); }
static void raw_out(struct nbuf* b, const uint8_t* r, uint32_t n) {
    uint8_t* d = (uint8_t*)nb_put(b, n);
    if (d) for (uint32_t i = 0; i < n; i++) d[i] = r[i];
}
static uint32_t nest_begin(struct nbuf* b, uint16_t type) {
    uint32_t at = b->n;
    uint8_t* a = (uint8_t*)nb_put(b, 4);
    if (a) w16(a + 2, (uint16_t)(type | NLA_F_NESTED_));
    return at;
}
static void nest_end(struct nbuf* b, uint32_t at) { if (!b->oom) w16(b->p + at, (uint16_t)(b->n - at)); }

static void put_err(struct nbuf* b, int err, const uint8_t* req, uint32_t pid) {
    uint32_t at = b->n;
    uint8_t* h = (uint8_t*)nb_put(b, 16 + 4 + 16);
    if (!h) return;
    w16(h + 4, NLMSG_ERROR_); w16(h + 6, err ? 0 : 0x100 /* NLM_F_CAPPED */);
    w32le(h + 8, le32(req + 8)); w32le(h + 12, pid);
    w32le(h + 16, (uint32_t)err);
    for (int i = 0; i < 16; i++) h[20 + i] = req[i];
    w32le(b->p + at, 36);
}
static void put_done(struct nbuf* b, uint32_t seq, uint32_t pid) {
    uint32_t at = b->n;
    uint8_t* h = (uint8_t*)nb_put(b, 20);
    if (!h) return;
    w16(h + 4, NLMSG_DONE_); w16(h + 6, NLM_F_MULTI_); w32le(h + 8, seq); w32le(h + 12, pid);
    w32le(b->p + at, 20);
}

/* ---- dumps --------------------------------------------------------------------- */

#define TYPE_OF(m) (uint16_t)((NFNL_SUBSYS_NFTABLES_ << 8) | (m))

static void dump_table(struct nbuf* b, struct nft_table* t, uint32_t seq, uint32_t pid, uint16_t fl) {
    uint32_t at = msg_begin(b, TYPE_OF(M_NEWTABLE), fl, seq, pid, (uint8_t)t->family, g_genid);
    attr_str(b, TA_NAME, t->name);
    attr_be32(b, TA_FLAGS, t->flags);
    uint32_t use = 0;
    for (struct nft_chain* c = t->chains; c; c = c->next) use++;
    for (struct nft_set* s = t->sets; s; s = s->next) use++;
    attr_be32(b, TA_USE, use);
    attr_be64(b, TA_HANDLE, t->handle);
    if (t->raw) {                               /* userdata and anything else it carried */
        uint16_t l; const uint8_t* u = nla_find(t->raw, t->rawlen, TA_USERDATA, &l);
        if (u) attr(b, TA_USERDATA, u, l);
    }
    msg_end(b, at);
}
static void dump_chain(struct nbuf* b, struct nft_table* t, struct nft_chain* c, uint32_t seq, uint32_t pid, uint16_t fl) {
    uint32_t at = msg_begin(b, TYPE_OF(M_NEWCHAIN), fl, seq, pid, (uint8_t)t->family, g_genid);
    attr_str(b, CA_TABLE, t->name);
    attr_be64(b, CA_HANDLE, c->handle);
    attr_str(b, CA_NAME, c->name);
    if (c->base) {
        uint32_t h = nest_begin(b, CA_HOOK);
        attr_be32(b, HA_HOOKNUM, (uint32_t)c->hooknum);
        attr_be32(b, HA_PRIORITY, (uint32_t)c->prio);
        nest_end(b, h);
        attr_be32(b, CA_POLICY, (uint32_t)c->policy);
        attr_str(b, CA_TYPE, c->type[0] ? c->type : "filter");
        attr_be32(b, CA_FLAGS, 1 /* NFT_CHAIN_BASE */);
    }
    uint32_t use = 0;
    for (struct nft_rule* r = c->rules; r; r = r->next) use++;
    attr_be32(b, CA_USE, use);
    if (c->raw) {
        uint16_t l; const uint8_t* u = nla_find(c->raw, c->rawlen, CA_USERDATA, &l);
        if (u) attr(b, CA_USERDATA, u, l);
    }
    msg_end(b, at);
}
static void dump_rule(struct nbuf* b, struct nft_table* t, struct nft_chain* c, struct nft_rule* r,
                      uint32_t seq, uint32_t pid, uint16_t fl) {
    uint32_t at = msg_begin(b, TYPE_OF(M_NEWRULE), fl, seq, pid, (uint8_t)t->family, g_genid);
    attr_str(b, RA_TABLE, t->name);
    attr_str(b, RA_CHAIN, c->name);
    attr_be64(b, RA_HANDLE, r->handle);
    uint16_t l;
    const uint8_t* e = nla_find(r->raw, r->rawlen, RA_EXPRESSIONS, &l);
    if (e) {
        /* element by element: a counter is re-emitted with its LIVE values
         * (nft sends one with no data at all), everything else as made */
        uint32_t list = nest_begin(b, RA_EXPRESSIONS);
        for (int i = 0; ; i++) {
            uint16_t el; const uint8_t* x = nla_nth(e, l, 1, i, &el);
            if (!x) break;
            uint64_t pk, by;
            if (nft_rule_counter(r, i, &pk, &by)) {
                uint32_t one = nest_begin(b, 1);
                attr_str(b, 1 /* NFTA_EXPR_NAME */, "counter");
                uint32_t d = nest_begin(b, 2 /* NFTA_EXPR_DATA */);
                attr_be64(b, 1 /* BYTES */, by);
                attr_be64(b, 2 /* PACKETS */, pk);
                nest_end(b, d);
                nest_end(b, one);
            } else attr(b, (uint16_t)(1 | NLA_F_NESTED_), x, el);
        }
        nest_end(b, list);
    }
    const uint8_t* u = nla_find(r->raw, r->rawlen, RA_USERDATA, &l);
    if (u) attr(b, RA_USERDATA, u, l);
    msg_end(b, at);
}
static void dump_set(struct nbuf* b, struct nft_table* t, struct nft_set* s, uint32_t seq, uint32_t pid, uint16_t fl) {
    uint32_t at = msg_begin(b, TYPE_OF(M_NEWSET), fl, seq, pid, (uint8_t)t->family, g_genid);
    attr_str(b, SA_TABLE, t->name);
    attr_str(b, SA_NAME, s->name);
    attr_be64(b, SA_HANDLE, s->handle);
    raw_out(b, s->raw, s->rawlen);              /* flags, key/data types and lengths, desc, userdata */
    msg_end(b, at);
}
static void dump_elems(struct nbuf* b, struct nft_table* t, struct nft_set* s, uint32_t seq, uint32_t pid, uint16_t fl) {
    if (!s->elems) return;
    uint32_t at = msg_begin(b, TYPE_OF(M_NEWSETELEM), fl, seq, pid, (uint8_t)t->family, g_genid);
    attr_str(b, ELA_TABLE, t->name);
    attr_str(b, ELA_SET, s->name);
    uint32_t list = nest_begin(b, ELA_ELEMENTS);
    for (struct nft_elem* e = s->elems; e; e = e->next) {
        uint32_t one = nest_begin(b, 1 /* NFTA_LIST_ELEM */);
        raw_out(b, e->raw, e->rawlen);
        nest_end(b, one);
    }
    nest_end(b, list);
    msg_end(b, at);
}

/* Match a GET request's selectors (family, table name, chain/set name). */
static void do_get(struct nft_net* n, int type, const uint8_t* m, uint32_t len, uint32_t pid, struct nbuf* b) {
    uint16_t flags = le16(m + 6);
    uint32_t seq = le32(m + 8);
    uint8_t fam = m[16];
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    int dump = (flags & NLM_F_DUMP_) == NLM_F_DUMP_;
    uint16_t mf = dump ? NLM_F_MULTI_ : 0;
    char tname[NFT_NAME_MAX] = {0}, oname[NFT_NAME_MAX] = {0};
    uint16_t l;
    const uint8_t* v;
    if (type == M_GETGEN) {
        uint32_t at = msg_begin(b, TYPE_OF(M_NEWGEN), 0, seq, pid, 0, g_genid);
        attr_be32(b, GA_ID, g_genid);
        struct task* me = task_current();
        attr_be32(b, GA_PROC_PID, me ? (uint32_t)me->pid : 0);
        attr_str(b, GA_PROC_NAME, me ? me->name : "?");
        msg_end(b, at);
        if (flags & NLM_F_ACK_) put_err(b, 0, m, pid);
        return;
    }
    /* the table selector is attribute 1 for every object type */
    if ((v = nla_find(a, al, 1, &l))) s_cpy(tname, v, l, sizeof tname);
    uint16_t oattr = type == M_GETCHAIN ? CA_NAME : type == M_GETSET ? SA_NAME :
                     type == M_GETSETELEM ? ELA_SET : type == M_GETRULE ? RA_CHAIN : 0;
    if (oattr && (v = nla_find(a, al, oattr, &l))) s_cpy(oname, v, l, sizeof oname);
    int found = 0;
    for (struct nft_table* t = n->tables; t; t = t->next) {
        if (fam && t->family != fam) continue;
        if (tname[0] && !s_eq(t->name, tname)) continue;
        if (type == M_GETTABLE) { dump_table(b, t, seq, pid, mf); found = 1; continue; }
        if (type == M_GETCHAIN)
            for (struct nft_chain* c = t->chains; c; c = c->next)
                if (!oname[0] || s_eq(c->name, oname)) { dump_chain(b, t, c, seq, pid, mf); found = 1; }
        if (type == M_GETRULE)
            for (struct nft_chain* c = t->chains; c; c = c->next) {
                if (oname[0] && !s_eq(c->name, oname)) continue;
                for (struct nft_rule* r = c->rules; r; r = r->next) { dump_rule(b, t, c, r, seq, pid, mf); found = 1; }
            }
        if (type == M_GETSET)
            for (struct nft_set* s = t->sets; s; s = s->next)
                if (!oname[0] || s_eq(s->name, oname)) { dump_set(b, t, s, seq, pid, mf); found = 1; }
        if (type == M_GETSETELEM)
            for (struct nft_set* s = t->sets; s; s = s->next)
                if (!oname[0] || s_eq(s->name, oname)) { dump_elems(b, t, s, seq, pid, mf); found = 1; }
    }
    if (dump) put_done(b, seq, pid);
    else if (!found) put_err(b, -2 /* ENOENT */, m, pid);
    else if (flags & NLM_F_ACK_) put_err(b, 0, m, pid);  /* the object, then the ack that ends the read */
}

/* ---- changes ------------------------------------------------------------------- */

static uint64_t next_handle(struct nft_net* n) { return ++n->handle_seq; }

static int new_table(struct nft_net* n, const uint8_t* m, uint32_t len, uint16_t flags) {
    uint8_t fam = m[16];
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    uint16_t l; const uint8_t* v = nla_find(a, al, TA_NAME, &l);
    if (!v) return -22;
    char name[NFT_NAME_MAX]; s_cpy(name, v, l, sizeof name);
    if (fam != 2 && fam != 10 && fam != 1) {
        kprintf("nft: table family %d refused — only ip, ip6 and inet are implemented\n", fam);
        return -97;                                     /* EAFNOSUPPORT */
    }
    struct nft_table* t = table_find(n, fam, name, 0);
    if (t) {
        if (flags & NLM_F_EXCL_) return -17;
        t->flags = nla_be32(a, al, TA_FLAGS, t->flags);
        return 0;
    }
    t = (struct nft_table*)kcalloc(1, sizeof *t);
    if (!t) return -12;
    t->family = fam;
    for (int i = 0; name[i]; i++) t->name[i] = name[i];
    t->flags = nla_be32(a, al, TA_FLAGS, 0);
    t->handle = next_handle(n);
    static const uint16_t drop[] = { TA_HANDLE, TA_USE };
    t->raw = raw_keep(a, al, drop, 2, &t->rawlen);
    struct nft_table** p = &n->tables;
    while (*p) p = &(*p)->next;
    *p = t;
    return 0;
}

static struct nft_table* req_table(struct nft_net* n, const uint8_t* m, uint32_t len, uint16_t attr_no) {
    uint16_t l; const uint8_t* v = nla_find(m + 20, len - 20, attr_no, &l);
    if (!v) return NULL;
    char name[NFT_NAME_MAX]; s_cpy(name, v, l, sizeof name);
    return table_find(n, m[16], name, 0);
}

static int del_table(struct nft_net* n, const uint8_t* m, uint32_t len, int destroy) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    uint16_t l;
    const uint8_t* v = nla_find(a, al, TA_NAME, &l);
    const uint8_t* h = nla_find(a, al, TA_HANDLE, &l);
    char name[NFT_NAME_MAX] = {0};
    if (v) { uint16_t nl; nla_find(a, al, TA_NAME, &nl); s_cpy(name, v, nl, sizeof name); }
    int fam = m[16];
    if (!v && !h) {                                     /* every table (of the family): `nft flush ruleset` */
        struct nft_table** p = &n->tables;
        while (*p) {
            if (fam == 0 || (*p)->family == fam) { struct nft_table* d = *p; *p = d->next; table_free(d); }
            else p = &(*p)->next;
        }
        return 0;
    }
    for (struct nft_table** p = &n->tables; *p; p = &(*p)->next) {
        struct nft_table* t = *p;
        if (fam && t->family != fam) continue;
        if (h ? t->handle == be64(h) : s_eq(t->name, name)) { *p = t->next; table_free(t); return 0; }
    }
    return destroy ? 0 : -2;
}

static int new_chain(struct nft_net* n, const uint8_t* m, uint32_t len, uint16_t flags) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, CA_TABLE);
    if (!t) return -2;
    uint16_t l; const uint8_t* v = nla_find(a, al, CA_NAME, &l);
    const uint8_t* h = nla_find(a, al, CA_HANDLE, &l);
    char name[NFT_NAME_MAX] = {0};
    if (v) { uint16_t nl; nla_find(a, al, CA_NAME, &nl); s_cpy(name, v, nl, sizeof name); }
    struct nft_chain* c = h ? nft_chain_find(t, NULL, be64(h)) : nft_chain_find(t, name, 0);
    if (c && h && v) {                                 /* rename by handle */
        for (int i = 0; i < NFT_NAME_MAX; i++) c->name[i] = name[i];
        return 0;
    }
    if (c) {
        if (flags & NLM_F_EXCL_) return -17;
        uint32_t pol = nla_be32(a, al, CA_POLICY, (uint32_t)c->policy);
        if (c->base) c->policy = (int)pol;
        return 0;
    }
    if (!v) return -22;
    c = (struct nft_chain*)kcalloc(1, sizeof *c);
    if (!c) return -12;
    for (int i = 0; name[i]; i++) c->name[i] = name[i];
    c->handle = next_handle(n);
    c->policy = 1;                                     /* NF_ACCEPT */
    uint16_t hl; const uint8_t* hook = nla_find(a, al, CA_HOOK, &hl);
    if (hook) {
        c->base = 1;
        c->hooknum = (int)nla_be32(hook, hl, HA_HOOKNUM, 0);
        c->prio = (int32_t)nla_be32(hook, hl, HA_PRIORITY, 0);
        if (c->hooknum > 4) { kfree(c); return -95; }   /* ingress: netdev family only */
        c->policy = (int)nla_be32(a, al, CA_POLICY, 1);
        uint16_t tl; const uint8_t* ty = nla_find(a, al, CA_TYPE, &tl);
        if (ty) s_cpy(c->type, ty, tl, sizeof c->type);
        else { c->type[0] = 'f'; c->type[1] = 'i'; c->type[2] = 'l'; c->type[3] = 't'; c->type[4] = 'e'; c->type[5] = 'r'; }
        if (!s_eq(c->type, "filter") && !s_eq(c->type, "nat") && !s_eq(c->type, "route")) {
            kprintf("nft: chain type '%s' refused\n", c->type);
            kfree(c); return -95;
        }
    }
    static const uint16_t drop[] = { CA_HANDLE, CA_USE };
    c->raw = raw_keep(a, al, drop, 2, &c->rawlen);
    struct nft_chain** p = &t->chains;
    while (*p) p = &(*p)->next;
    *p = c;
    return 0;
}

static int chain_referenced(struct nft_table* t, const char* name) {
    for (struct nft_chain* c = t->chains; c; c = c->next)
        for (struct nft_rule* r = c->rules; r; r = r->next)
            if (nft_rule_refers_chain(r, name)) return 1;
    return 0;
}

static int del_chain(struct nft_net* n, const uint8_t* m, uint32_t len, int destroy) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, CA_TABLE);
    if (!t) return destroy ? 0 : -2;
    uint16_t l; const uint8_t* v = nla_find(a, al, CA_NAME, &l);
    const uint8_t* h = nla_find(a, al, CA_HANDLE, &l);
    char name[NFT_NAME_MAX] = {0};
    if (v) { uint16_t nl; nla_find(a, al, CA_NAME, &nl); s_cpy(name, v, nl, sizeof name); }
    for (struct nft_chain** p = &t->chains; *p; p = &(*p)->next) {
        struct nft_chain* c = *p;
        if (h ? c->handle != be64(h) : !s_eq(c->name, name)) continue;
        if (c->rules) return -16;                        /* EBUSY: flush it first */
        if (chain_referenced(t, c->name)) return -16;
        *p = c->next; chain_free(c);
        return 0;
    }
    return destroy ? 0 : -2;
}

static int new_rule(struct nft_net* n, const uint8_t* m, uint32_t len, uint16_t flags) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, RA_TABLE);
    if (!t) return -2;
    uint16_t l; const uint8_t* cv = nla_find(a, al, RA_CHAIN, &l);
    if (!cv) return -22;
    char cname[NFT_NAME_MAX]; s_cpy(cname, cv, l, sizeof cname);
    struct nft_chain* c = nft_chain_find(t, cname, 0);
    if (!c) return -2;
    struct nft_rule* r = (struct nft_rule*)kcalloc(1, sizeof *r);
    if (!r) return -12;
    static const uint16_t drop[] = { RA_HANDLE, RA_POSITION, RA_ID, RA_POSITION_ID, RA_CHAIN_ID };
    r->raw = raw_keep(a, al, drop, 5, &r->rawlen);
    uint16_t el; const uint8_t* ex = nla_find(r->raw, r->rawlen, RA_EXPRESSIONS, &el);
    int rc = nft_rule_compile(t, r, ex, ex ? el : 0);
    if (rc) { nft_rule_free(r); return rc; }
    const uint8_t* pos = nla_find(a, al, RA_POSITION, &l);
    const uint8_t* hnd = nla_find(a, al, RA_HANDLE, &l);
    if ((flags & NLM_F_REPLACE_) && hnd) {             /* replace the rule with that handle */
        for (struct nft_rule** p = &c->rules; *p; p = &(*p)->next)
            if ((*p)->handle == be64(hnd)) {
                r->handle = (*p)->handle; r->next = (*p)->next;
                nft_rule_free(*p); *p = r;
                return 0;
            }
        nft_rule_free(r);
        return -2;
    }
    r->handle = next_handle(n);
    if (pos) {                                         /* relative to an existing rule */
        uint64_t ph = be64(pos);
        for (struct nft_rule** p = &c->rules; *p; p = &(*p)->next)
            if ((*p)->handle == ph) {
                if (flags & NLM_F_APPEND_) { r->next = (*p)->next; (*p)->next = r; }
                else { r->next = *p; *p = r; }
                return 0;
            }
        nft_rule_free(r);
        return -2;
    }
    if (flags & NLM_F_APPEND_) {
        struct nft_rule** p = &c->rules;
        while (*p) p = &(*p)->next;
        *p = r;
    } else { r->next = c->rules; c->rules = r; }       /* `nft insert`: at the head */
    return 0;
}

static int del_rule(struct nft_net* n, const uint8_t* m, uint32_t len, int destroy) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, RA_TABLE);
    if (!t) return destroy ? 0 : -2;
    uint16_t l; const uint8_t* cv = nla_find(a, al, RA_CHAIN, &l);
    const uint8_t* h = nla_find(a, al, RA_HANDLE, &l);
    char cname[NFT_NAME_MAX] = {0};
    if (cv) { uint16_t cl; nla_find(a, al, RA_CHAIN, &cl); s_cpy(cname, cv, cl, sizeof cname); }
    for (struct nft_chain* c = t->chains; c; c = c->next) {
        if (cv && !s_eq(c->name, cname)) continue;
        if (!h) {                                       /* flush the chain (or the table) */
            for (struct nft_rule* r = c->rules; r; ) { struct nft_rule* nx = r->next; nft_rule_free(r); r = nx; }
            c->rules = NULL;
            continue;
        }
        for (struct nft_rule** p = &c->rules; *p; p = &(*p)->next)
            if ((*p)->handle == be64(h)) { struct nft_rule* d = *p; *p = d->next; nft_rule_free(d); return 0; }
    }
    return h ? (destroy ? 0 : -2) : 0;
}

static int new_set(struct nft_net* n, const uint8_t* m, uint32_t len, uint16_t flags) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, SA_TABLE);
    if (!t) return -2;
    uint16_t l; const uint8_t* v = nla_find(a, al, SA_NAME, &l);
    if (!v) return -22;
    char name[NFT_NAME_MAX]; s_cpy(name, v, l, sizeof name);
    struct nft_set* s = nft_set_find(t, name, 0);
    if (s) return (flags & NLM_F_EXCL_) ? -17 : 0;
    s = (struct nft_set*)kcalloc(1, sizeof *s);
    if (!s) return -12;
    for (int i = 0; name[i]; i++) s->name[i] = name[i];
    s->flags = nla_be32(a, al, SA_FLAGS, 0);
    s->klen = nla_be32(a, al, SA_KEY_LEN, 0);
    s->dlen = nla_be32(a, al, SA_DATA_LEN, 0);
    s->dtype = nla_be32(a, al, SA_DATA_TYPE, 0);
    s->id = nla_be32(a, al, SA_ID, 0);
    if (s->klen == 0 || s->klen > NFT_KEY_MAX || s->dlen > NFT_KEY_MAX) {
        kprintf("nft: set '%s' key %u / data %u bytes refused (max %d)\n", name, s->klen, s->dlen, NFT_KEY_MAX);
        kfree(s); return -22;
    }
    if (s->flags & 0x10 /* TIMEOUT */) kprintf("nft: set '%s': element timeouts are not implemented (elements stay)\n", name);
    s->handle = next_handle(n);
    static const uint16_t drop[] = { SA_TABLE, SA_NAME, SA_HANDLE, SA_ID };
    s->raw = raw_keep(a, al, drop, 4, &s->rawlen);
    struct nft_set** p = &t->sets;
    while (*p) p = &(*p)->next;
    *p = s;
    return 0;
}

static int del_set(struct nft_net* n, const uint8_t* m, uint32_t len, int destroy) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, SA_TABLE);
    if (!t) return destroy ? 0 : -2;
    uint16_t l; const uint8_t* v = nla_find(a, al, SA_NAME, &l);
    const uint8_t* h = nla_find(a, al, SA_HANDLE, &l);
    char name[NFT_NAME_MAX] = {0};
    if (v) { uint16_t nl; nla_find(a, al, SA_NAME, &nl); s_cpy(name, v, nl, sizeof name); }
    for (struct nft_set** p = &t->sets; *p; p = &(*p)->next) {
        struct nft_set* s = *p;
        if (h ? s->handle != be64(h) : !s_eq(s->name, name)) continue;
        for (struct nft_chain* c = t->chains; c; c = c->next)
            for (struct nft_rule* r = c->rules; r; r = r->next)
                if (nft_rule_refers_set(r, s->name)) return -16;
        *p = s->next; set_free(s);
        return 0;
    }
    return destroy ? 0 : -2;
}

/* Parse one element's key / key_end / data out of its attributes. */
static int elem_parse(struct nft_set* s, const uint8_t* e, uint16_t el, struct nft_elem* out) {
    uint16_t l;
    const uint8_t* k = nla_find(e, el, EA_KEY, &l);
    out->flags = nla_be32(e, el, EA_FLAGS, 0);
    if (k) {
        uint16_t vl; const uint8_t* v = nla_find(k, l, 1 /* NFTA_DATA_VALUE */, &vl);
        if (!v || vl > NFT_KEY_MAX) return -22;
        for (uint16_t i = 0; i < vl; i++) out->key[i] = v[i];
        out->klen = (uint8_t)vl;
    } else if (!(out->flags & 2 /* CATCHALL */)) return -22;
    const uint8_t* ke = nla_find(e, el, EA_KEY_END, &l);
    if (ke) {
        uint16_t vl; const uint8_t* v = nla_find(ke, l, 1, &vl);
        if (v && vl <= NFT_KEY_MAX) { for (uint16_t i = 0; i < vl; i++) out->key_end[i] = v[i]; out->kelen = (uint8_t)vl; }
    }
    const uint8_t* d = nla_find(e, el, EA_DATA, &l);
    if (d) {
        uint16_t vl; const uint8_t* v = nla_find(d, l, 1, &vl);
        const uint8_t* vd = nla_find(d, l, 2 /* NFTA_DATA_VERDICT */, &vl);
        if (v) {
            nla_find(d, l, 1, &vl);
            if (vl > NFT_KEY_MAX) return -22;
            for (uint16_t i = 0; i < vl; i++) out->data[i] = v[i];
            out->dlen = (uint8_t)vl;
        } else if (vd) {
            out->has_verdict = 1;
            out->verdict = (int32_t)nla_be32(vd, vl, 1 /* CODE */, 0);
            uint16_t cl; const uint8_t* ch = nla_find(vd, vl, 2 /* CHAIN */, &cl);
            if (ch) s_cpy(out->vchain, ch, cl, sizeof out->vchain);
        }
    }
    (void)s;
    return 0;
}
static int key_eq(const struct nft_elem* a, const struct nft_elem* b) {
    if (a->klen != b->klen || (a->flags & 2) != (b->flags & 2)) return 0;
    for (int i = 0; i < a->klen; i++) if (a->key[i] != b->key[i]) return 0;
    if ((a->flags & 1) != (b->flags & 1)) return 0;     /* interval start vs end */
    return 1;
}

static int setelem_change(struct nft_net* n, const uint8_t* m, uint32_t len, int add, uint16_t flags, int destroy) {
    const uint8_t* a = m + 20; uint32_t al = len - 20;
    struct nft_table* t = req_table(n, m, len, ELA_TABLE);
    if (!t) return destroy ? 0 : -2;
    uint16_t l;
    const uint8_t* sv = nla_find(a, al, ELA_SET, &l);
    struct nft_set* s = NULL;
    if (sv) { char sn[NFT_NAME_MAX]; s_cpy(sn, sv, l, sizeof sn); s = nft_set_find(t, sn, 0); }
    if (!s) s = set_find_id(t, nla_be32(a, al, ELA_SET_ID, 0));
    if (!s) return destroy ? 0 : -2;
    uint16_t ll; const uint8_t* list = nla_find(a, al, ELA_ELEMENTS, &ll);
    if (!list) {                                        /* no list: flush the set */
        if (!add) { for (struct nft_elem* e = s->elems; e; ) { struct nft_elem* nx = e->next; elem_free(e); e = nx; } s->elems = NULL; }
        return 0;
    }
    for (int i = 0; ; i++) {
        uint16_t el; const uint8_t* e = nla_nth(list, ll, 1, i, &el);
        if (!e) break;
        struct nft_elem tmp;
        for (unsigned k = 0; k < sizeof tmp; k++) ((uint8_t*)&tmp)[k] = 0;
        int rc = elem_parse(s, e, el, &tmp);
        if (rc) return rc;
        struct nft_elem** p = &s->elems;
        for (; *p; p = &(*p)->next) if (key_eq(*p, &tmp)) break;
        if (add) {
            if (*p) { if (flags & NLM_F_EXCL_) return -17; continue; }
            struct nft_elem* ne = (struct nft_elem*)kcalloc(1, sizeof *ne);
            if (!ne) return -12;
            *ne = tmp; ne->next = NULL;
            ne->raw = blob_dup(e, el); ne->rawlen = el;
            *p = ne;
        } else {
            if (!*p) { if (destroy) continue; return -2; }
            struct nft_elem* d = *p; *p = d->next; elem_free(d);
        }
    }
    return 0;
}

/* ---- the request entry --------------------------------------------------------- */

static int apply(struct nft_net* n, int type, const uint8_t* m, uint32_t len, uint16_t flags) {
    switch (type) {
    case M_NEWTABLE:      return new_table(n, m, len, flags);
    case M_DELTABLE:      return del_table(n, m, len, 0);
    case M_DESTROYTABLE:  return del_table(n, m, len, 1);
    case M_NEWCHAIN:      return new_chain(n, m, len, flags);
    case M_DELCHAIN:      return del_chain(n, m, len, 0);
    case M_DESTROYCHAIN:  return del_chain(n, m, len, 1);
    case M_NEWRULE:       return new_rule(n, m, len, flags);
    case M_DELRULE:       return del_rule(n, m, len, 0);
    case M_DESTROYRULE:   return del_rule(n, m, len, 1);
    case M_NEWSET:        return new_set(n, m, len, flags);
    case M_DELSET:        return del_set(n, m, len, 0);
    case M_DESTROYSET:    return del_set(n, m, len, 1);
    case M_NEWSETELEM:    return setelem_change(n, m, len, 1, flags, 0);
    case M_DELSETELEM:    return setelem_change(n, m, len, 0, flags, 0);
    case M_DESTROYSETELEM:return setelem_change(n, m, len, 0, flags, 1);
    default:
        kprintf("nft: message %d refused - not implemented (objects, flowtables)\n", type);
        return -95;
    }
}

/* ---- ctnetlink (§M90 3b): the conntrack table over NETLINK_NETFILTER ----------
 *
 * Subsystem 1 (NFNL_SUBSYS_CTNETLINK).  GET dumps the namespace's connections
 * (or finds one by tuple), DELETE removes the one a tuple names — or, with no
 * tuple, flushes the family.  That is `conntrack -L/-D/-F`, and it is what
 * dockerd does when a container goes: it dumps the table and deletes every
 * flow touching the container's address, so that a new container given the
 * same address does not inherit the old one's NAT state ("Failed to delete
 * conntrack state ... operation not supported" until now).  Creating or
 * updating an entry from userspace (NEW), statistics and the expectation
 * subsystem's objects are not offered: NEW is refused by name, the
 * expectation table dumps empty (there are no helpers, hence no expectations). */
#define CTA_TUPLE_ORIG 1
#define CTA_TUPLE_REPLY 2
#define CTA_STATUS 3
#define CTA_TIMEOUT 7
#define CTA_MARK 8
#define CTA_USE 11
#define CTA_ID 12
static void ct_tuple_out(struct nbuf* b, uint16_t type, int fam, uint8_t proto,
                         const uint8_t* src, const uint8_t* dst, uint16_t sp, uint16_t dp, int reply) {
    uint32_t t = nest_begin(b, type);
    uint32_t ip = nest_begin(b, 1 /* CTA_TUPLE_IP */);
    if (fam == 10) { attr(b, 3, src, 16); attr(b, 4, dst, 16); }
    else           { attr(b, 1, src, 4);  attr(b, 2, dst, 4); }
    nest_end(b, ip);
    uint32_t pr = nest_begin(b, 2 /* CTA_TUPLE_PROTO */);
    attr(b, 1, &proto, 1);
    uint8_t p2[2];
    if (proto == 1 || proto == 58) {                      /* echo: id, type, code */
        int v6 = proto == 58;
        p2[0] = (uint8_t)(sp >> 8); p2[1] = (uint8_t)sp;
        attr(b, v6 ? 7 : 4, p2, 2);
        uint8_t ty = v6 ? (reply ? 129 : 128) : (reply ? 0 : 8), code = 0;
        attr(b, v6 ? 8 : 5, &ty, 1);
        attr(b, v6 ? 9 : 6, &code, 1);
    } else {
        p2[0] = (uint8_t)(sp >> 8); p2[1] = (uint8_t)sp; attr(b, 2, p2, 2);
        p2[0] = (uint8_t)(dp >> 8); p2[1] = (uint8_t)dp; attr(b, 3, p2, 2);
    }
    nest_end(b, pr);
    nest_end(b, t);
}
static void ct_entry_out(struct nbuf* b, const struct nft_ct_view* v, uint32_t seq, uint32_t pid, uint16_t fl) {
    uint32_t at = msg_begin(b, (uint16_t)(1 << 8 | 0 /* IPCTNL_MSG_CT_NEW */), fl, seq, pid, (uint8_t)v->fam, 0);
    ct_tuple_out(b, CTA_TUPLE_ORIG, v->fam, v->proto, v->osrc, v->odst, v->osport, v->odport, 0);
    ct_tuple_out(b, CTA_TUPLE_REPLY, v->fam, v->proto, v->rsrc, v->rdst, v->rsport, v->rdport, 1);
    attr_be32(b, CTA_STATUS, v->status);
    attr_be32(b, CTA_TIMEOUT, v->timeout_s);
    attr_be32(b, CTA_MARK, v->mark);
    attr_be32(b, CTA_USE, 1);
    attr_be32(b, CTA_ID, v->id);
    msg_end(b, at);
}
/* A CTA_TUPLE_* nest → addresses and ports; 0 = parsed. */
static int ct_tuple_in(const uint8_t* t, uint16_t tl, int fam, uint8_t* src, uint8_t* dst,
                       uint8_t* proto, uint16_t* sp, uint16_t* dp) {
    uint16_t il, pl, l;
    const uint8_t* ip = nla_find(t, tl, 1, &il);
    const uint8_t* pr = nla_find(t, tl, 2, &pl);
    if (!ip || !pr) return -22;
    int n = fam == 10 ? 16 : 4;
    const uint8_t* a = nla_find(ip, il, fam == 10 ? 3 : 1, &l);
    if (!a || l != n) return -22;
    for (int i = 0; i < n; i++) src[i] = a[i];
    a = nla_find(ip, il, fam == 10 ? 4 : 2, &l);
    if (!a || l != n) return -22;
    for (int i = 0; i < n; i++) dst[i] = a[i];
    a = nla_find(pr, pl, 1, &l);
    if (!a || l < 1) return -22;
    *proto = a[0];
    if (*proto == 1 || *proto == 58) {
        a = nla_find(pr, pl, *proto == 58 ? 7 : 4, &l);
        if (!a || l != 2) return -22;
        *sp = *dp = (uint16_t)(a[0] << 8 | a[1]);
    } else {
        a = nla_find(pr, pl, 2, &l); *sp = a && l == 2 ? (uint16_t)(a[0] << 8 | a[1]) : 0;
        a = nla_find(pr, pl, 3, &l); *dp = a && l == 2 ? (uint16_t)(a[0] << 8 | a[1]) : 0;
    }
    return 0;
}
static void ctnl(int ns, const uint8_t* m, uint32_t len, uint32_t pid, struct nbuf* b) {
    uint16_t type = le16(m + 4), flags = le16(m + 6);
    uint32_t seq = le32(m + 8);
    int fam = len > 16 ? m[16] : 0;
    int sub = type >> 8, t = type & 0xFF;
    const uint8_t* at = m + 20;
    uint32_t al = len > 20 ? len - 20 : 0;
    if (sub == 2) {                                        /* expectations: there are none */
        if (t == 1 && (flags & NLM_F_DUMP_) == NLM_F_DUMP_) put_done(b, seq, pid);
        else put_err(b, t == 1 ? -2 : -95, m, pid);
        return;
    }
    if (t == 1 /* GET */ || t == 3 /* GET_CTRZERO */) {
        struct nft_ct_view* v = (struct nft_ct_view*)kmalloc(sizeof *v * 512);
        if (!v) { put_err(b, -12, m, pid); return; }
        int k = nft_ct_list(ns, fam == 2 || fam == 10 ? fam : 0, v, 512);
        if ((flags & NLM_F_DUMP_) == NLM_F_DUMP_) {
            for (int i = 0; i < k; i++) ct_entry_out(b, &v[i], seq, pid, NLM_F_MULTI_);
            put_done(b, seq, pid);
        } else {
            uint16_t tl; const uint8_t* tp = nla_find(at, al, CTA_TUPLE_ORIG, &tl);
            int which = 0;
            if (!tp) { tp = nla_find(at, al, CTA_TUPLE_REPLY, &tl); which = 1; }
            uint8_t src[16], dst[16], proto = 0; uint16_t sp = 0, dp = 0;
            int found = -1;
            if (tp && (fam == 2 || fam == 10) && ct_tuple_in(tp, tl, fam, src, dst, &proto, &sp, &dp) == 0) {
                int n = fam == 10 ? 16 : 4;
                for (int i = 0; i < k && found < 0; i++) {
                    const uint8_t* s0 = which ? v[i].rsrc : v[i].osrc;
                    const uint8_t* d0 = which ? v[i].rdst : v[i].odst;
                    uint16_t s1 = which ? v[i].rsport : v[i].osport, d1 = which ? v[i].rdport : v[i].odport;
                    int eq = v[i].fam == fam && v[i].proto == proto && s1 == sp && d1 == dp;
                    for (int j = 0; eq && j < n; j++) if (s0[j] != src[j] || d0[j] != dst[j]) eq = 0;
                    if (eq) found = i;
                }
            }
            if (found >= 0) {
                ct_entry_out(b, &v[found], seq, pid, 0);
                if (flags & NLM_F_ACK_) put_err(b, 0, m, pid);
            } else put_err(b, tp ? -2 : -22, m, pid);
        }
        kfree(v);
        return;
    }
    if (t == 2 /* DELETE */) {
        uint16_t tl; const uint8_t* tp = nla_find(at, al, CTA_TUPLE_ORIG, &tl);
        int which = 0;
        if (!tp) { tp = nla_find(at, al, CTA_TUPLE_REPLY, &tl); which = 1; }
        int rc;
        if (!tp) { nft_ct_flush(ns, fam == 2 || fam == 10 ? fam : 0); rc = 0; }   /* conntrack -F */
        else {
            uint8_t src[16], dst[16], proto = 0; uint16_t sp = 0, dp = 0;
            rc = (fam == 2 || fam == 10) ? ct_tuple_in(tp, tl, fam, src, dst, &proto, &sp, &dp) : -22;
            if (rc == 0) rc = nft_ct_delete(ns, fam, proto, src, dst, sp, dp, which);
        }
        if (rc || (flags & NLM_F_ACK_)) put_err(b, rc, m, pid);
        return;
    }
    kprintf("ctnetlink: message %d refused - %s\n", t,
            t == 0 ? "entries are made by traffic, not by userspace" : "not implemented (statistics)");
    put_err(b, -95, m, pid);
}

static uint32_t g_batches, g_aborts;

/* A request stream from a NETLINK_NETFILTER socket made in namespace `ns`.
 * Replies (acks, errors, dumps) go into `out` (grown as needed). */
void nft_netlink(int ns, uint32_t pid, const uint8_t* buf, uint32_t n, uint8_t** out, uint32_t* outlen) {
    struct nbuf b = { 0 };
    struct nft_net* net = nft_net_of(ns);
    struct nft_table* undo = NULL;
    int in_batch = 0, failed = 0;
    uint32_t off = 0;
    while (off + 16 <= n) {
        const uint8_t* m = buf + off;
        uint32_t len = le32(m);
        if (len < 16 || off + len > n) break;
        uint16_t type = le16(m + 4), flags = le16(m + 6);
        off += (len + 3u) & ~3u;
        if (config_get_long("debug.nft", 0))
            kprintf("nft: req type %x flags %x len %u fam %d (reply so far %u)\n", type, flags, len,
                    len > 16 ? m[16] : -1, b.n);
        if (type == NFNL_MSG_BATCH_BEGIN_) {
            /* the namespace's ruleset as it is now: the batch's undo */
            uint32_t f = nft_lock();
            int fail = 0;
            undo = net ? net_clone(net->tables, &fail) : NULL;
            nft_unlock(f);
            if (fail) { net_free(undo); undo = NULL; put_err(&b, -12, m, pid); failed = 1; }
            in_batch = 1; failed = 0;
            g_batches++;
            continue;
        }
        if (type == NFNL_MSG_BATCH_END_) {
            uint32_t f = nft_lock();
            if (failed && net) {                  /* put the ruleset back */
                struct nft_table* cur = net->tables;
                net->tables = undo; undo = cur;
                g_aborts++;
            } else if (!failed) g_genid++;
            nft_unlock(f);
            net_free(undo); undo = NULL;
            in_batch = 0;
            if (flags & NLM_F_ACK_) put_err(&b, 0, m, pid);
            continue;
        }
        if (((type >> 8) == 1 || (type >> 8) == 2) && len >= 20 && !in_batch) {
            ctnl(ns, m, len, pid, &b);                  /* §M90 3b — ctnetlink */
            continue;
        }
        if ((type >> 8) != NFNL_SUBSYS_NFTABLES_ || len < 20 || !net) {
            put_err(&b, -95, m, pid);                    /* nfnetlink subsystems other than nftables */
            if (in_batch) failed = 1;
            continue;
        }
        int t = type & 0xFF;
        if (t == M_GETTABLE || t == M_GETCHAIN || t == M_GETRULE || t == M_GETSET ||
            t == M_GETSETELEM || t == M_GETGEN || t == M_GETOBJ || t == M_GETFLOWTABLE) {
            uint32_t f = nft_lock();
            if (t == M_GETOBJ || t == M_GETFLOWTABLE) {          /* none exist here */
                if ((flags & NLM_F_DUMP_) == NLM_F_DUMP_) put_done(&b, le32(m + 8), pid);
                else put_err(&b, -2, m, pid);
            } else do_get(net, t, m, len, pid, &b);
            nft_unlock(f);
            continue;
        }
        if (in_batch && failed) {                        /* the rest of a failed batch is not applied */
            continue;
        }
        uint32_t f = nft_lock();
        int rc = apply(net, t, m, len, flags);
        if (!in_batch && rc == 0) g_genid++;
        nft_unlock(f);
        if (rc) {
            put_err(&b, rc, m, pid);
            if (in_batch) failed = 1;
        } else if (flags & NLM_F_ACK_) put_err(&b, 0, m, pid);
    }
    if (in_batch) {                                      /* a batch with no END: abort it */
        uint32_t f = nft_lock();
        if (net) { struct nft_table* cur = net->tables; net->tables = undo; undo = cur; }
        nft_unlock(f);
        net_free(undo);
        g_aborts++;
    }
    *out = b.p; *outlen = b.oom ? 0 : b.n;
}

/* A namespace's end takes its ruleset. */
void nft_ns_gone(int ns) {
    struct nft_net* n = nft_net_of(ns);
    if (!n) return;
    uint32_t f = nft_lock();
    struct nft_table* t = n->tables;
    n->tables = NULL;
    nft_unlock(f);
    net_free(t);
}

void nft_stats(uint32_t* batches, uint32_t* aborts, uint32_t* genid) {
    if (batches) *batches = g_batches;
    if (aborts) *aborts = g_aborts;
    if (genid) *genid = g_genid;
}
