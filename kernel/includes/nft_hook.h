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
void nft_hook_stats(uint32_t* evals, uint32_t* dropped, uint32_t* ct_new, uint32_t* nat_bind, uint32_t* ct_live);
#endif
