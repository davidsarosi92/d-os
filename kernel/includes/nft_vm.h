/* nft_vm.h — the nf_tables VM's private types (§M90 3b): compiled ops, the
 * packet a rule runs against, and a conntrack entry.  nft_eval.c compiles,
 * nft_hook.c runs. */
#ifndef NFT_VM_H
#define NFT_VM_H
#include <stdint.h>
#include "nftables.h"

#define NFT_MAX_OPS 32          /* expressions per rule */
#define NFT_REG_WORDS 20        /* the register file: verdict (4) + 16 data words */

enum { OP_NONE, OP_IMM, OP_CMP, OP_RANGE, OP_BITWISE, OP_BYTEORDER, OP_PAYLOAD, OP_META,
       OP_META_SET, OP_COUNTER, OP_LOOKUP, OP_CT, OP_CT_SET, OP_NAT, OP_MASQ, OP_REDIR,
       OP_FIB, OP_LOG, OP_REJECT, OP_NOP };

struct nft_op {
    uint8_t  kind, sub, len, is_verdict;
    int8_t   sreg, sreg2, dreg, dreg2;        /* register WORD indexes, -1 = none */
    uint32_t x, y;
    int32_t  verdict;
    uint8_t  data[NFT_KEY_MAX], data2[NFT_KEY_MAX];
    char     name[NFT_NAME_MAX];              /* set (lookup) / chain (jump, goto) */
    uint64_t packets, bytes;                  /* counter                           */
};

/* nf_tables numbers the VM uses */
#define NF_DROP_   0
#define NF_ACCEPT_ 1
#define NFT_CONTINUE (-1)
#define NFT_BREAK    (-2)
#define NFT_JUMP     (-3)
#define NFT_GOTO     (-4)
#define NFT_RETURN   (-5)
enum { NFT_CMP_EQ, NFT_CMP_NEQ, NFT_CMP_LT, NFT_CMP_LTE, NFT_CMP_GT, NFT_CMP_GTE };
enum { NFT_BITWISE_BOOL, NFT_BITWISE_LSHIFT, NFT_BITWISE_RSHIFT };
enum { NFT_BYTEORDER_NTOH, NFT_BYTEORDER_HTON };
enum { NFT_PAYLOAD_LL_HEADER, NFT_PAYLOAD_NETWORK_HEADER, NFT_PAYLOAD_TRANSPORT_HEADER };
enum { NFT_META_LEN, NFT_META_PROTOCOL, NFT_META_PRIORITY, NFT_META_MARK, NFT_META_IIF,
       NFT_META_OIF, NFT_META_IIFNAME, NFT_META_OIFNAME, NFT_META_IIFTYPE, NFT_META_OIFTYPE,
       NFT_META_SKUID, NFT_META_SKGID, NFT_META_NFTRACE, NFT_META_RTCLASSID, NFT_META_SECMARK,
       NFT_META_NFPROTO, NFT_META_L4PROTO, NFT_META_BRI_IIFNAME, NFT_META_BRI_OIFNAME,
       NFT_META_PKTTYPE, NFT_META_CPU, NFT_META_IIFGROUP, NFT_META_OIFGROUP, NFT_META_CGROUP,
       NFT_META_PRANDOM, NFT_META_SECPATH, NFT_META_IIFKIND, NFT_META_OIFKIND };
enum { NFT_CT_STATE, NFT_CT_DIRECTION, NFT_CT_STATUS, NFT_CT_MARK, NFT_CT_SECMARK,
       NFT_CT_EXPIRATION, NFT_CT_HELPER, NFT_CT_L3PROTOCOL, NFT_CT_SRC, NFT_CT_DST,
       NFT_CT_PROTOCOL, NFT_CT_PROTO_SRC, NFT_CT_PROTO_DST, NFT_CT_LABELS, NFT_CT_PKTS,
       NFT_CT_BYTES, NFT_CT_AVGPKT, NFT_CT_ZONE, NFT_CT_EVENTMASK, NFT_CT_SRC_IP,
       NFT_CT_DST_IP, NFT_CT_SRC_IP6, NFT_CT_DST_IP6, NFT_CT_ID };
enum { NFT_FIB_RESULT_UNSPEC, NFT_FIB_RESULT_OIF, NFT_FIB_RESULT_OIFNAME, NFT_FIB_RESULT_ADDRTYPE };
#define NFTA_FIB_F_SADDR 1
#define NFTA_FIB_F_DADDR 2
#define NFTA_FIB_F_IIF   8
#define NFTA_FIB_F_OIF   16
#define NFTA_FIB_F_PRESENT 32
/* NF_NAT_RANGE_* */
#define NAT_MAP_IPS        1
#define NAT_PROTO_SPECIFIED 2

/* conntrack state bits (NF_CT_STATE_*_BIT) and status (IPS_*) */
#define CTS_INVALID     1
#define CTS_ESTABLISHED 2
#define CTS_RELATED     4
#define CTS_NEW         8
#define CTS_UNTRACKED   64
#define IPS_SEEN_REPLY  2
#define IPS_ASSURED     4
#define IPS_CONFIRMED   8
#define IPS_SRC_NAT     16
#define IPS_DST_NAT     32

struct net_device;

/* One tracked connection: the ORIGINAL tuple as the first packet carried it,
 * and the REPLY tuple a reply carries — which differs from the inverse of the
 * original exactly by the NAT applied (Linux's model: one entry, two tuples). */
struct nft_tuple { uint8_t src[16], dst[16]; uint16_t sport, dport; };
struct nft_conn {
    int      used, ns, fam;               /* fam: 2 or 10 */
    uint8_t  proto;
    struct nft_tuple orig, reply;
    uint32_t status, mark;
    uint64_t last_ms;
    uint8_t  fin_seen;
};

/* A packet as the VM sees it.  `nh` is WRITABLE (NAT rewrites it). */
struct nft_pkt {
    int      ns, fam, hook;
    uint8_t* nh;
    uint32_t len, thoff;
    uint8_t  l4proto, pkttype;
    struct net_device* in;
    struct net_device* out;
    struct nft_conn* ct;
    int      dir;                         /* 0 original, 1 reply */
    uint32_t ctstate;                     /* CTS_* */
    uint32_t mark;
    /* a NAT the ruleset asked for (applied by the hook, recorded in ct) */
    int      nat;                         /* 0 none, 1 SNAT, 2 DNAT */
    uint8_t  nat_addr[16];
    int      nat_has_addr, nat_has_port, nat_masq;
    uint16_t nat_pmin, nat_pmax;
};

#endif
