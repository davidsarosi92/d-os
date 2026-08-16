/* =============================================================================
 * dhcp.h — the DHCP client (§M24 stage 7).  Implementation + rationale in
 * kernel/core/dhcp.c.
 * ============================================================================= */

#ifndef DHCP_H
#define DHCP_H

struct net_device;

/* Run the DISCOVER/OFFER/REQUEST/ACK exchange on `dev` (NULL = the default
 * route's device) and apply the result: address, mask, gateway and nameserver.
 * Blocks for at most three attempts of two seconds.  Returns 0 on success.
 *
 * Safe to call again — a renewal is exactly the same exchange. */
int  dhcp_configure(struct net_device* dev);

/* Print the current lease (address, server, seconds remaining). */
void dhcp_status(void);

#endif
