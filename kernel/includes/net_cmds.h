/* =============================================================================
 * net_cmds.h — network diagnostics + self-tests, callable from any shell.
 * See kernel/core/net_cmds.c for why they do not live in one.
 * ============================================================================= */

#ifndef NET_CMDS_H
#define NET_CMDS_H

/* `tcptest [n]`  — an echo server and n concurrent clients over the loopback.
 *                  Proves the connection TABLE: on a one-connection stack the
 *                  clients deadlock at their barrier and it reports FAIL.
 * `tcploss [permille] [kb] [readdelay_ms]`
 *                — a stream that survives a lossy link, asserted two ways:
 *                  every byte in order AND a non-zero retransmit count.
 * `netstat`      — the connection table, with the table's capacity.
 * `lo [drop <permille>]` — the loss injector itself.
 * `dhcp [dev|status]`    — ask the network for an address.
 */
void netcmd_tcptest(const char* args);
void netcmd_tcploss(const char* args);
void netcmd_netstat(void);
void netcmd_lo(const char* args);
void netcmd_dhcp(const char* args);

#endif
