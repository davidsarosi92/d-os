/* nft_hook.h — the stack's side of nf_tables (§M90 3b, nft_hook.c). */
#ifndef NFT_HOOK_H
#define NFT_HOOK_H
#include <stdint.h>
struct net_device;
/* NF_INET_* */
#define NFT_HOOK_PREROUTING  0
#define NFT_HOOK_INPUT       1
#define NFT_HOOK_FORWARD     2
#define NFT_HOOK_OUTPUT      3
#define NFT_HOOK_POSTROUTING 4
/* Does namespace `ns` have a ruleset at all?  (The cheap gate.) */
int  nft_active(int ns);
/* One packet (network header first, writable, `len` bytes) at one hook, with
 * the stack lock held.  1 = accept, 0 = drop.  NAT rewrites it in place. */
int  nft_ip_hook(int ns, int fam, int hook, struct net_device* in, struct net_device* out,
                 uint8_t* nh, uint32_t len);
void nft_ct_ns_gone(int ns);
/* §M90 3b — ctnetlink's view of the conntrack table (conntrack -L / -D, and
 * what Docker uses to drop a removed container's flows). */
struct nft_ct_view {
    int      fam;                         /* 2 or 10 */
    uint8_t  proto;
    uint8_t  osrc[16], odst[16], rsrc[16], rdst[16];
    uint16_t osport, odport, rsport, rdport;   /* ICMP echo: the id in all four */
    uint32_t status, mark, timeout_s, id;
};
int  nft_ct_list(int ns, int fam, struct nft_ct_view* v, int max);   /* fam 0 = both */
/* Remove the connection whose ORIGINAL (which 0) or REPLY (which 1) tuple this
 * is; 0 = removed, -2 = none.  `fam` 0 with `t` NULL flushes the namespace. */
int  nft_ct_delete(int ns, int fam, uint8_t proto, const uint8_t* src, const uint8_t* dst,
                   uint16_t sport, uint16_t dport, int which);
int  nft_ct_flush(int ns, int fam);
void nft_hook_stats(uint32_t* evals, uint32_t* dropped, uint32_t* ct_new, uint32_t* nat_bind, uint32_t* ct_live);
#endif
