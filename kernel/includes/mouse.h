/* =============================================================================
 * mouse.h — pointer-input listener interface (M22).
 *
 * Mirrors the keyboard pipeline's shape: the hardware driver (PS/2 aux
 * today, USB HID boot-mouse later) decodes movement packets and hands
 * normalized deltas to a single registered listener.  The listener runs
 * in IRQ context — it must be short and lock-light (the GUI's handler
 * only updates cursor state + drag bookkeeping and sets a repaint flag).
 *
 * Buttons are a bitmask; deltas use screen convention (positive dy =
 * down), already flipped from the PS/2 wire format by the driver.
 * ============================================================================= */

#ifndef MOUSE_H
#define MOUSE_H

#define MOUSE_BTN_LEFT    0x1
#define MOUSE_BTN_RIGHT   0x2
#define MOUSE_BTN_MIDDLE  0x4

typedef void (*mouse_listener_t)(int dx, int dy, unsigned buttons);

/* Register the (single) movement listener.  Pass NULL to detach.
 * Until a listener is set, packets are decoded and dropped. */
void mouse_set_listener(mouse_listener_t fn);

/* §M61 follow-up — the WHEEL, reported separately from motion because it is not
 * a movement: routing a wheel delta as `dy` would move the cursor instead of
 * scrolling what is under it.  `dz` is positive for wheel-up.  Only fires on a
 * device that accepted the IntelliMouse knock (ps2_mouse.c reads the ID back);
 * on one that did not, no wheel events arrive and everything else still works. */
typedef void (*mouse_wheel_t)(int dz);
void mouse_set_wheel_listener(mouse_wheel_t fn);

#endif
