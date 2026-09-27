/* =============================================================================
 * netui.h — the network's windows, as the taskbar reaches them (§M87).
 *
 * The taskbar's network flyout is chrome: it runs on the compositor with the WM
 * lock held, so it must never block (§M22.7).  Everything that can wait on the
 * network — joining a Wi-Fi network, asking a DHCP server — goes through one of
 * these, which either queue a window (gui_queue_open) or hand the work to a
 * task of its own.
 * ============================================================================= */

#ifndef NETUI_H
#define NETUI_H

/* Open (or raise) the Control Panel's Network page. */
void netui_open_panel(void);

/* Open (or raise) the Wi-Fi chooser, with `ssid` preselected when non-NULL —
 * the route a secured network takes from the tray, because a passphrase needs
 * a text field and a flyout has none. */
void netui_open_wifi(const char* ssid);

/* Join an OPEN network without blocking the caller.  0 = the attempt was
 * started (its result lands on the console and in the tray within a second). */
int  netui_wifi_join_async(const char* ssid);

/* Leave the current wireless network without blocking the caller. */
int  netui_wifi_leave_async(void);

#endif
