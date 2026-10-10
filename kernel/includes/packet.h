/* packet.h — AF_PACKET sockets (§M90 rung 5, packet.c). */
#ifndef DOS_PACKET_H
#define DOS_PACKET_H
#include <stddef.h>
#include <stdint.h>

struct pktsock;
struct net_device;

/* Where a received frame came from — a sockaddr_ll's contents. */
struct packet_from {
    uint16_t proto;            /* ethertype, host order */
    int      ifindex;
    uint8_t  pkttype;          /* 0 PACKET_HOST, 1 PACKET_BROADCAST */
    uint8_t  addr[6];          /* the sender's MAC */
};

/* type: 3 SOCK_RAW (whole frame) | 2 SOCK_DGRAM (payload).  proto in HOST
 * order (3 = ETH_P_ALL).  The caller checks CAP_NET_RAW. */
struct pktsock* packet_create(int type, uint16_t proto_host);
void packet_close(struct pktsock* s);
int  packet_bind(struct pktsock* s, uint16_t proto_host, int ifindex);
void packet_name(struct pktsock* s, uint16_t* proto, int* ifindex, uint8_t mac[6]);
int  packet_can_read(struct pktsock* s);
long packet_recv(struct pktsock* s, void* buf, size_t n, int block, struct packet_from* from);
long packet_send(struct pktsock* s, const void* buf, size_t n, int ifindex,
                 const uint8_t* dst, uint16_t proto_host);
/* net_rx's tap, stack lock held. */
void packet_tap_locked(struct net_device* dev, const uint8_t* frame, uint32_t len);

#endif
