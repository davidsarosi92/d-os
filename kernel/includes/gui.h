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

#include <stdint.h>

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

/* Queue an app launch onto the compositor.  The compositor spawns a
 * dedicated "app-host" task that runs the app (M22.7), so launching must
 * NOT call the app's open fn directly — that would run it on the caller's
 * task with no event loop.  Used by the taskbar and the `launch` command. */
struct gui_app_def;
void gui_queue_launch(const struct gui_app_def* app);

/* Create a terminal window (spawns a shell task on it).  Outer
 * geometry in pixels, including decorations.  M22.7 — the shell is a child
 * of the desktop SESSION (a kill_tree of the desktop takes it with it). */
struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h);

/* M22.7 — like gui_window_create but the shell is DETACHED (parented to
 * init), so it outlives the desktop session; its window stays composited as
 * long as the compositor runs.  The "detached terminal" mode. */
struct gui_window* gui_window_create_detached(const char* title,
                                              int x, int y, int w, int h);

/* M22.5 — terminal window hosting a CUSTOM task instead of a shell
 * (the BASIC interpreter uses this).  The task's kprintf output lands
 * in the window (out_console = the window's offscreen VC), keyboard
 * input reaches it via vc_getchar on that VC, and closing the window
 * kills + reaps it under the kthread contract — so the entry MUST hit
 * task_yield / vc_getchar / task_should_stop regularly. */
struct gui_window* gui_window_create_task(const char* title, int x, int y,
                                          int w, int h,
                                          const char* task_name,
                                          void (*entry)(void));

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

/* §M42 — poll whether the X button has been clicked (want_close).  A self-
 * driven WIN_APP (NetSurf via the dosgui bridge) checks this to quit itself. */
int  gui_window_want_close(struct gui_window* win);

/* §M42 — client-managed WIN_APP window lifecycle (the dosgui bridge).  The
 * client is a detached, init-reaped ring-3 task, NOT a compositor app-host:
 *   set_client_managed — sever host_task so the compositor never reads/reaps it;
 *   client_release      — client is finished; mark the window disposable. */
void gui_window_set_client_managed(struct gui_window* win);
void gui_window_client_release(struct gui_window* win);

/* Raise + focus a window (used by singleton apps on re-launch). */
void gui_window_raise(struct gui_window* win);

/* M22.5 — retitle a window (editor shows the open file's name). */
void gui_window_set_title(struct gui_window* win, const char* title);

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

/* §M26 — paint a raw ARGB pixel block into a window's content surface (at
 * content-relative x,y) and composite it.  The Wayland server uses this to turn
 * a committed wl_shm buffer into a real window's contents.  `gui_window_pixel`
 * reads a content pixel back (self-tests). */
void     gui_window_blit(struct gui_window* win, int x, int y,
                         const uint32_t* px, int w, int h, int stride);
uint32_t gui_window_pixel(struct gui_window* win, int x, int y);

/* §M26 — input forwarding.  When a hook is set, the window's keyboard/pointer
 * input is delivered here (instead of to widgets) — the Wayland server routes
 * it to wl_keyboard/wl_pointer.  `keycode` is a raw scancode; `pressed` = down. */
enum gui_input_type { GUI_INPUT_KEY, GUI_INPUT_MOTION };
struct gui_input {
    enum gui_input_type type;
    int keycode, pressed;               /* GUI_INPUT_KEY    */
    int x, y;                           /* GUI_INPUT_MOTION (content-relative) */
};
void gui_window_set_input_hook(struct gui_window* win,
        void (*fn)(struct gui_window*, const struct gui_input*, void*), void* ctx);

/* App context accessor (the pointer passed to gui_app_window_create). */
void* gui_window_ctx(struct gui_window* win);

/* Repaint the window's widgets into its surface + mark damage.  Call
 * after mutating widget state outside an event callback (event
 * dispatch redraws automatically). */
void gui_window_request_redraw(struct gui_window* win);

/* M22.7 — repaint + damage only a CONTENT sub-rect (widget-local coords),
 * for a frequently-updating region (e.g. a listview) so the whole window
 * chrome isn't re-blitted every refresh. */
void gui_window_request_redraw_rect(struct gui_window* win,
                                    int cx, int cy, int cw, int ch);

/* M22.3 — ~1 Hz callback on the compositor task (task-manager style
 * auto-refresh).  NULL to disable. */
void gui_window_set_tick(struct gui_window* win,
                         void (*fn)(struct gui_window*));

/* M22.3 — damage interface.  gui_damage marks a screen rect dirty;
 * the compositor recomposes ONLY the accumulated dirty region
 * (clip-box composition).  Most callers want the window helpers /
 * gui_request_frame instead; these are for advanced users. */
void gui_damage(int x, int y, int w, int h);
void gui_damage_all(void);

/* Frame counters since gui_start: full-screen vs. partial (dirty-rect)
 * recomposes.  Backs the `gui stats` shell command. */
void gui_get_stats(unsigned* full, unsigned* partial, unsigned* avg_kb);

#endif
