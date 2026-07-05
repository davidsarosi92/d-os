/* =============================================================================
 * gui_internal.h — WM services for desktop-shell implementations (M22.2).
 *
 * NOT a public kernel header on purpose: only shell_*.c files under
 * kernel/gui/ may include it.  Apps use the public gui.h / widget.h.
 *
 * Two calling conventions, matching desktop.h's threading contract:
 *
 *   *_locked  — ONLY from inside shell->click / shell->motion (the
 *               mouse IRQ already holds the WM state lock; taking it
 *               again would deadlock).
 *   plain     — from shell->draw / shell->second_tick (compositor
 *               task); they take the lock internally where needed.
 *
 *   queue services (gui_queue_*) are lock-free SPSC pushes, callable
 *   from either side.
 * ============================================================================= */

#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

struct gui_window;
struct gui_app_def;

/* ---- window enumeration (stable pool order, for taskbar buttons) ---------- */

/* Fill `out` with up to `max` live windows; returns the count.  The
 * _locked variant requires the WM lock (click/motion); the plain one
 * snapshots internally (draw/tick). */
int  gui_wm_windows_locked(struct gui_window** out, int max);
int  gui_wm_windows(struct gui_window** out, int max);

struct gui_window* gui_wm_focused(void);        /* atomic pointer read */
const char*        gui_window_title(struct gui_window* w);

/* ---- actions -------------------------------------------------------------- */

/* Focus + raise (taskbar button click).  WM lock must be held. */
void gui_wm_focus_raise_locked(struct gui_window* w);

/* M22.3 — Windows-style taskbar button semantics: minimized →
 * restore+focus, focused → minimize, else → focus+raise.  WM lock
 * must be held (click callback). */
void gui_wm_taskbar_activate_locked(struct gui_window* w);

/* Non-zero if the window is minimized (dim its taskbar button). */
int  gui_window_minimized(struct gui_window* w);

/* Queue an app launch / power action; executed on the compositor task
 * on the next loop pass.  Safe from IRQ context. */
void gui_queue_launch(const struct gui_app_def* app);
void gui_queue_power(int reboot);               /* 1 = reboot, 0 = shutdown */

/* Ask for a recompose (shells call it after mutating visual state). */
void gui_request_frame(void);

/* M22.7-B — publish the shell's launcher-popup extent to the compositor.
 * `on`=1 with the popup's screen rect while the menu is open; `on`=0 to
 * hide it.  The compositor composites this rect (on top of the windows)
 * only while open, and routes clicks inside it to the desktop task. */
void gui_panel_set_popup(int on, int x, int y, int w, int h);

/* M22.7 — request a chrome-only repaint (taskbar + open popup) without a
 * full-screen recompose.  Shells use it for cheap visual changes (hover). */
void gui_panel_dirty(void);

/* Screen geometry (constant after gui_start). */
int  gui_screen_w(void);
int  gui_screen_h(void);

#endif
