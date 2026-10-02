/* netlink.h — AF_NETLINK / NETLINK_ROUTE, §M90.  See kernel/core/netlink.c. */
#ifndef NETLINK_H
#define NETLINK_H

#include <stddef.h>
#include <stdint.h>

struct nlsock;
struct ofile;

struct nlsock* nl_create(int proto);            /* NULL: protocol not offered */
void     nl_set_owner(struct nlsock* s, struct ofile* o);
void     nl_close(struct nlsock* s);
int      nl_bind(struct nlsock* s, uint32_t portid, uint32_t groups);
uint32_t nl_portid(struct nlsock* s);
int      nl_can_read(struct nlsock* s);
/* A request stream (kernel memory): answered at once, the replies queued as
 * one datagram.  Returns n, or a negative Linux errno. */
long     nl_send(struct nlsock* s, const void* buf, size_t n);
/* One reply datagram into kernel memory; see netlink.c for peek/trunc. */
long     nl_recv(struct nlsock* s, void* buf, size_t n, int block, int peek, int trunc);

#endif
