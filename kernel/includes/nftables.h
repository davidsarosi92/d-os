/* nftables.h — nf_tables (§M90 3b): the object store (nftables.c), the rule
 * VM and the hooks (nft_eval.c), conntrack and NAT (nft_ct.c). */
#ifndef NFTABLES_H
#define NFTABLES_H
#include <stddef.h>
#include <stdint.h>

#define NFT_NS_MAX    256       /* network namespaces (net.c's NET_NS_MAX)       */
#define NFT_NAME_MAX  64        /* table, chain, set names (Linux: 256)          */
#define NFT_KEY_MAX   64        /* a set key / data / immediate value, bytes      */

struct nft_op;                   /* a compiled expression (nft_eval.c) */

struct nft_rule {
    struct nft_rule* next;
    uint64_t handle;
    uint8_t* raw; uint32_t rawlen;   /* the attributes it was made with (dumps) */
    struct nft_op* prog; int nprog;  /* compiled                                */
};
struct nft_chain {
    struct nft_chain* next;
    char     name[NFT_NAME_MAX];
    uint64_t handle;
    int      base, hooknum, policy;  /* a BASE chain is hooked: NF_INET_* and its policy */
    int32_t  prio;
    char     type[16];               /* filter / nat / route                    */
    struct nft_rule* rules;
    uint8_t* raw; uint32_t rawlen;
};
struct nft_elem {
    struct nft_elem* next;
    uint8_t  key[NFT_KEY_MAX], key_end[NFT_KEY_MAX], data[NFT_KEY_MAX];
    uint8_t  klen, kelen, dlen, has_verdict;
    uint32_t flags;                  /* NFT_SET_ELEM_INTERVAL_END / CATCHALL    */
    int32_t  verdict;
    char     vchain[NFT_NAME_MAX];
    uint8_t* raw; uint32_t rawlen;
};
struct nft_set {
    struct nft_set* next;
    char     name[NFT_NAME_MAX];
    uint64_t handle;
    uint32_t flags, klen, dlen, dtype, id;
    struct nft_elem* elems;
    uint8_t* raw; uint32_t rawlen;
};
struct nft_table {
    struct nft_table* next;
    int      family;                 /* 2 ip, 10 ip6, 1 inet                     */
    char     name[NFT_NAME_MAX];
    uint32_t flags;
    uint64_t handle;
    struct nft_chain* chains;
    struct nft_set*   sets;
    uint8_t* raw; uint32_t rawlen;
};
struct nft_net {
    struct nft_table* tables;
    uint64_t handle_seq;
};

struct nft_net* nft_net_of(int ns);
uint32_t nft_lock(void);
void     nft_unlock(uint32_t flags);
struct nft_chain* nft_chain_find(struct nft_table* t, const char* name, uint64_t handle);
struct nft_set*   nft_set_find(struct nft_table* t, const char* name, uint64_t handle);
void nft_rule_free(struct nft_rule* r);

/* Netlink attribute helpers (shared with nft_eval.c). */
const uint8_t* nla_find(const uint8_t* a, uint32_t len, uint16_t type, uint16_t* plen);
const uint8_t* nla_nth(const uint8_t* a, uint32_t len, uint16_t type, int n, uint16_t* plen);
uint32_t nla_be32(const uint8_t* a, uint32_t len, uint16_t type, uint32_t def);

/* nft_eval.c — compile a rule's NFTA_RULE_EXPRESSIONS (0 or a negative errno:
 * EOPNOTSUPP names the expression), and the bookkeeping the store needs. */
int  nft_rule_compile(struct nft_table* t, struct nft_rule* r, const uint8_t* exprs, uint32_t len);
void nft_rule_uncompile(struct nft_rule* r);
void nft_rule_copy_compiled(struct nft_rule* dst, const struct nft_rule* src);
void nft_rule_patch_counters(const struct nft_rule* r, uint8_t* exprs, uint32_t len);
int  nft_rule_counter(const struct nft_rule* r, int i, uint64_t* packets, uint64_t* bytes);
int  nft_rule_refers_chain(const struct nft_rule* r, const char* chain);
int  nft_rule_refers_set(const struct nft_rule* r, const char* set);

/* netlink.c → here: one request stream from a NETLINK_NETFILTER socket. */
void nft_netlink(int ns, uint32_t pid, const uint8_t* buf, uint32_t n, uint8_t** out, uint32_t* outlen);
void nft_ns_gone(int ns);
void nft_stats(uint32_t* batches, uint32_t* aborts, uint32_t* genid);

#endif
