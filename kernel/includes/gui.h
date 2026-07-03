/* =============================================================================
 * gui.h — compositor + window manager public surface (M22).
 *
 * Object model is deliberately Wayland-shaped (see PLAN.md §M22 and the
 * 2026-07-03 evaluation): a window owns an off-screen content SURFACE,
 * output is COMMITted implicitly by marking damage, and input follows a
 * SEAT-like focus model (keyboard → focused window's VC ring, pointer →
 * hit-tested window).  When §M25 lands the userland substrate, this
 * maps 1:1 onto wl_surface / wl_seat without a compositor rewrite.
 *
 * Today's windows are all terminal windows: each hosts a shell task
 * bound to an offscreen VC (vc_create_offscreen), so the entire shell +
 * console plumbing is reused unchanged.
 * ============================================================================= */

#ifndef GUI_H
#define GUI_H

struct gui_window;                     /* opaque outside kernel/gui/ */

/* Bring up the GUI: wraps the framebuffer, allocates the backbuffer +
 * wallpaper, creates two shell windows, registers the mouse listener,
 * suppresses pane rendering (vc_screen_suppress) and spawns the
 * compositor task.  Idempotent — a second call is a no-op.
 * Returns 0 on success, -1 if there is no usable framebuffer. */
int  gui_start(void);

/* Non-zero once gui_start succeeded. */
int  gui_is_active(void);

/* Create an additional terminal window (spawns a shell task on it).
 * Outer geometry in pixels, including decorations.  Returns NULL when
 * the window pool or VC table is exhausted. */
struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h);

#endif
