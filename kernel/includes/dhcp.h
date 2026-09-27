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

#include <stdint.h>
/* §M87 — the current lease as data (the panel's view of dhcp_status).
 * Returns -1 when there is no lease. */
int  dhcp_lease(struct net_device** dev, uint32_t* server,
                uint32_t* left_s, uint32_t* lease_s);
/* Forget the lease without telling the server (a static address was set). */
void dhcp_release_local(void);

#endif
