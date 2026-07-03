/* =============================================================================
 * gui.h — compositor + window manager public surface (M22 + M22.1).
 *
 * Object model is deliberately Wayland-shaped (see PLAN.md §M22 and the
 * 2026-07-03 evaluation): a window owns an off-screen content SURFACE,
 * output is COMMITted implicitly by marking damage, and input follows a
 * SEAT-like focus model (keyboard → focused window, pointer →
 * hit-tested window).  When §M25 lands the userland substrate, this
 * maps 1:1 onto wl_surface / wl_seat without a compositor rewrite.
 *
 * Two window kinds:
 *   - TERMINAL windows host a stock shell task through an offscreen VC
 *     (vc_create_offscreen) — the whole shell/console stack is reused.
 *   - APP windows host a flat widget list (widget.h); their events are
 *     queued by the mouse IRQ and dispatched on the compositor task,
 *     so widget callbacks may do real work (VFS, kmalloc, windows).
 *
 * M22.1 additions: Vista-style taskbar (Start menu, per-window buttons,
 * RTC clock), close button on app windows, content-preserving resize
 * (terminal windows re-render from a char backing store; app windows
 * re-layout + redraw).
 * ============================================================================= */

#ifndef GUI_H
#define GUI_H

struct gui_window;                     /* opaque outside kernel/gui/ */
struct widget;

/* Bring up the GUI: wraps the framebuffer, allocates the backbuffer +
 * wallpaper, creates two shell windows + the taskbar, registers the
 * mouse listener + keyboard hook, suppresses pane rendering and spawns
 * the compositor task.  Idempotent.  Returns 0 on success, -1 if there
 * is no usable framebuffer. */
int  gui_start(void);

/* Non-zero once gui_start succeeded. */
int  gui_is_active(void);

/* Create a terminal window (spawns a shell task on it).  Outer
 * geometry in pixels, including decorations. */
struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h);

/* Create an APP (widget) window.  `on_layout` is called with the
 * window whenever the content size is (re)established — create widgets
 * in your builder AFTER this call returns, position them in on_layout.
 * `app_ctx` is kfree'd automatically on window close (pass NULL if the
 * lifetime is managed elsewhere). */
struct gui_window* gui_app_window_create(const char* title, int x, int y,
                                         int w, int h,
                                         void (*on_layout)(struct gui_window*),
                                         void* app_ctx);

/* Ask for an app window to be closed (same path as its X button).
 * Actual teardown happens on the compositor task. */
void gui_window_close(struct gui_window* win);

/* Raise + focus a window (used by singleton apps on re-launch). */
void gui_window_raise(struct gui_window* win);

/* Optional close notification (runs on the compositor task, before the
 * widgets/ctx are freed).  Apps use it to drop their singletons. */
void gui_window_set_on_close(struct gui_window* win,
                             void (*fn)(struct gui_window*));

/* ---- API for widget implementations + apps -------------------------------- */

/* Append a widget to the window's list (constructors call this). */
void gui_window_add_widget(struct gui_window* win, struct widget* w);

/* Keyboard focus within the window (textinput click handler calls it). */
void gui_window_focus_widget(struct gui_window* win, struct widget* w);
int  gui_widget_focused(struct widget* w);      /* is w the focused one? */

/* Content-area size in pixels (excludes decorations). */
int  gui_window_content_size(struct gui_window* win, int* w, int* h);

/* App context accessor (the pointer passed to gui_app_window_create). */
void* gui_window_ctx(struct gui_window* win);

/* Repaint the window's widgets into its surface + mark damage.  Call
 * after mutating widget state outside an event callback (event
 * dispatch redraws automatically). */
void gui_window_request_redraw(struct gui_window* win);

#endif
