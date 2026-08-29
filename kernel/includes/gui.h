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

/* §M58 — `termcheck`: write numbered lines until they scroll off, then select
 * one BY ABSOLUTE LINE NUMBER and print what the copy path returns.  A
 * screenshot cannot show that a selection still names the text it was given;
 * this can.  Must run inside a GUI terminal window. */
void gui_term_check(void);

/* Desktop-task loop counters, printed by `gui stats`.  They exist because
 * "the taskbar is not updating" has three causes that look identical from
 * outside the guest: the loop is not running, it runs but never marks itself
 * dirty, or it draws and the damage never reaches the compositor.  One line of
 * numbers separates them. */
struct gui_desktop_stats {
    unsigned iters;         /* desktop-loop iterations                       */
    unsigned draws;         /* panel repaints                                */
    unsigned events;        /* chrome clicks/motions taken off the queue     */
    unsigned ticks;         /* half-second housekeeping ticks                */
    unsigned tick_dirty;    /* …of which changed the chrome (clock, layout)  */
    unsigned clock_ms;      /* what timer_ticks_ms() reads on that task      */
};
void gui_get_desktop_stats(struct gui_desktop_stats* out);

/* Boot-time: start the desktop if `gui.autostart` says so (default: yes).
 *
 * ONE function rather than the six lines at each call site, because there are
 * TWO boot paths — x86's `kernel_main` and aarch64's own `main_entry` — and
 * this project has repeatedly shipped features that existed on one of them
 * only (§4.63's `setconf` on ARM is the same shape).  Returns 1 if the desktop
 * came up, 0 if it was declined by config, -1 if it was wanted and failed. */
int  gui_autostart(void);

/* Ask for the GUI session to end and the screen to go back to the text shell.
 * Safe from anywhere including IRQ context and the chrome (Start → Exit GUI):
 * it only sets a flag.  The teardown itself runs on a task of its own, because
 * it KILLS the desktop session — and the compositor, which is where chrome
 * clicks are dispatched, is inside that session.  A task cannot tidily free the
 * surfaces it is still composing from. */
void gui_queue_exit(void);

/* The teardown itself (normally reached through gui_queue_exit).  Kills the
 * desktop session, releases the input hooks, frees the surfaces and hands the
 * screen back to the console.  Returns 0 if a session was running.  After it
 * returns, `gui_start()` may be called again — this is a stop, not a shutdown. */
int  gui_stop(void);

/* §M60 — repaint the wallpaper surface from the current `gui.wallpaper` /
 * `gui.wallpaper_fit` config and damage the whole screen.  Called by
 * `wallpaper_set*` (and later the §M63 Personalisation panel) so a source
 * change is visible without a reboot.  A no-op returning 0 when the GUI is not
 * running — the config key is still what boot will read.  Returns 0 if the
 * requested source was used, -1 if it fell back to the gradient. */
int  gui_wallpaper_reload(void);

/* §M64 — the desktop's shortcut set changed (added, removed, reloaded).
 * Damages the icon area so the next compose repaints it from the model.
 * Deliberately NOT a redraw request into the shell: `draw_under` reads the
 * model live, so damage is the whole notification.  Safe (a no-op) when the
 * GUI is not running, which is the normal case for `shortcut add` typed at a
 * boot shell before `gui`. */
void gui_desktop_icons_changed(void);

/* §M64 tail — the desktop shell says whether it currently holds the keyboard
 * (i.e. something on the icon field is selected).  While it does, Enter and
 * Escape are delivered to the shell's `desktop_key` instead of falling through
 * to the console behind the desktop; while it does not, they reach the console
 * exactly as before — which is what keeps a shell command typeable with the
 * desktop up.  Called from the desktop task, read in the keyboard IRQ. */
void gui_desktop_focus(int on);

/* §M61 — change the display mode.  QUEUED: the switch happens on the
 * compositor task between frames, because a mode set while compose() is
 * mid-blit writes into a buffer that is about to be freed.  Returns 0 when the
 * request was accepted (not when the mode is live). */
int  gui_request_mode(int w, int h);
int  gui_current_mode(int* w, int* h);

/* Confirm-or-revert.  `arm` marks the change provisional (so the saved
 * geometry is kept), `confirm` keeps it, `revert` restores the previous mode
 * AND the window geometry saved before the change — a cancelled experiment
 * must leave the desktop exactly as it was, not merely the resolution. */
void gui_set_mode_applied_cb(void (*fn)(int w, int h));
void gui_mode_arm_confirm(void);
void gui_mode_confirm(void);
void gui_mode_revert(void);

/* §M61 — the `mode` command (displaypanel.c), shared by both shells. */
void display_cmd(const char* args);
int  display_set_mode(int w, int h, int force);

/* The device manager's table, printed (devicepanel.c).  It walks the PANEL's
 * own model, so a headless run falsifies the panel rather than an independent
 * re-reading of the registry that could agree while the panel is wrong. */
void devices_cmd(const char* args);

/* §M61 — a window-level cooked-key hook, consulted before the focused widget.
 * The confirm dialog uses it so Enter/Esc work with nothing focused. */
void gui_window_set_key_hook(struct gui_window* win,
                             void (*fn)(struct gui_window*, char));

/* §M42 — pid of the desktop/session task (0 before gui_start).  Taskbar-launched
 * apps parent under this so they appear under the desktop in the process tree. */
int  gui_desktop_pid(void);

/* Queue an app launch onto the compositor.  The compositor spawns a
 * dedicated "app-host" task that runs the app (M22.7), so launching must
 * NOT call the app's open fn directly — that would run it on the caller's
 * task with no event loop.  Used by the taskbar and the `launch` command. */
struct gui_app_def;
void gui_queue_launch(const struct gui_app_def* app);

/* §M61 — the same thing for a window that is not a launcher entry: run
 * `open_fn` on a fresh app-host task.  A window created anywhere else is bound
 * to a task with no app-host loop, so it never lays out and never ticks. */
void gui_queue_open(void (*open_fn)(void));

/* §M46 secure-attention key (Ctrl+Alt+X): request the compositor close/force the
 * top-most app window.  Safe to call from the keyboard IRQ (single volatile
 * store); the compositor — never the frozen app — performs the close. */
void gui_request_close_last(void);

/* Create a terminal window (spawns a shell task on it).  Outer
 * geometry in pixels, including decorations.  M22.7 — the shell is a child
 * of the desktop SESSION (a kill_tree of the desktop takes it with it). */
struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h);

/* M22.7 — like gui_window_create but the shell is DETACHED (parented to
 * init), so it outlives the desktop session; its window stays composited as
 * long as the compositor runs.  The "detached terminal" mode. */
struct gui_window* gui_window_create_detached(const char* title,
                                              int x, int y, int w, int h);

/* The console behind a terminal window, or NULL for any other kind.  Exists so
 * a caller can hand that window's shell some input (§M64's `run:` shortcut) —
 * the window is the thing a user points at, the VC is the thing a shell reads
 * from, and only the compositor knows which belongs to which. */
struct vc* gui_window_console(struct gui_window* win);

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
void gui_window_set_client_managed(struct gui_window* win, int client_pid);
void gui_window_client_release(struct gui_window* win);

/* §M54 — tell the bridge when the compositor DISPOSES the window, whatever
 * route disposed it (client_release, the X button, or the client dying without
 * releasing anything).  Without this the bridge can only learn about the
 * disposals it caused itself, so a client that CRASHES leaves its handle
 * occupied forever and its window pointer dangling — the handle table runs out
 * after a few crashes and the app stops being able to open at all.
 *
 * A handle whose lifetime is INFERRED is a handle that leaks: the owner of a
 * handle has to be told when the object behind it dies.  Runs on the compositor
 * task, once, before the window struct is recycled. */
void gui_window_set_dispose_cb(struct gui_window* win,
                               void (*cb)(struct gui_window*, void*), void* ctx);

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

/* §M65 — THE WINDOW POPUP: one overlay above every window, used by the menu
 * bar and the combo box.  `items` is one string with '\n' between entries and
 * "-" for a separator — flat, because the same call has to survive being sent
 * from ring 3.  The choice comes back to `owner` as an app event carrying the
 * row index (-1 = dismissed) and the `tag` handed in here, so ONE handler can
 * tell which menu was open. */
void gui_popup_open(struct gui_window* owner, int sx, int sy,
                    const char* items, int tag);
void gui_popup_close(void);
int  gui_popup_active(void);

/* §M65 — the toolkit's per-window state slot (owned by ui.c, freed with the
 * window).  Accessors, not a field, so ui.c stays out of gui.c's internals. */
/* Where the window's CONTENT starts on screen — widgets think in content
 * coordinates, the popup overlay thinks in screen ones. */
void gui_window_content_origin(struct gui_window* win, int* sx, int* sy);

/* Ask for a layout + repaint of this window's toolkit widgets. */
void gui_window_request_layout(struct gui_window* win);

void* gui_window_ui(struct gui_window* win);
void  gui_window_set_ui(struct gui_window* win, void* p);

/* Keyboard focus within the window (textinput click handler calls it). */
void gui_window_focus_widget(struct gui_window* win, struct widget* w);
struct widget* gui_window_focused_widget(struct gui_window* win);

/* §M65 — Tab / Shift+Tab: move the keyboard focus to the next / previous
 * focusable widget, wrapping.  Handled at the WINDOW level because no control
 * can know what comes after it. */
void gui_window_focus_cycle(struct gui_window* win, int backwards);
int  gui_widget_focused(struct widget* w);      /* is w the focused one? */

/* Content-area size in pixels (excludes decorations). */
int  gui_window_content_size(struct gui_window* win, int* w, int* h);

/* §M40 — the inverse: what OUTER size does a window need so its content area is
 * exactly cw x ch?  A Wayland client picks its own buffer size and the window
 * must be built around it, so the decoration thickness cannot stay private to
 * gui.c. */
void gui_window_outer_for_content(int cw, int ch, int* ow, int* oh);

/* Screen size in pixels (0 before gui_start).  The Wayland server reports this
 * as its wl_output mode. */
int  gui_screen_w(void);
int  gui_screen_h(void);

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
/* §M48 — GUI_INPUT_BUTTON is new, and its absence was a real gap: a window
 * that forwards input to a client (NetSurf via dosgui, any Wayland surface)
 * could receive keys and pointer MOTION but had no way to learn that a button
 * was pressed.  A click reached the compositor, raised the window, and was
 * then delivered as an indistinguishable motion event — which is why the
 * browser rendered pages it could not be clicked on: no link, no form field,
 * no scrollbar could ever be activated. */
enum gui_input_type { GUI_INPUT_KEY, GUI_INPUT_MOTION, GUI_INPUT_BUTTON };
struct gui_input {
    enum gui_input_type type;
    int keycode, pressed;               /* KEY: scancode; BUTTON: 1=L 2=R 3=M */
    int x, y;                           /* MOTION / BUTTON (content-relative) */
    /* KEY: the character the active keymap produced, or 0 for a key that has
     * none (arrows, Home/End).  Both halves are needed and neither can stand
     * in for the other: Wayland wants the SCANCODE (its xkb keymap is built
     * from d-os scancodes), while a client like NetSurf wants the CHARACTER
     * and has no keymap of its own.  Forwarding only the scancode is what made
     * typing a URL produce garbage — the browser read raw scancodes as if they
     * were character codes. */
    int ch;
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
/* Drop this window's widgets.  An app whose `on_layout` BUILDS widgets must
 * call this at the top of it.
 *
 * WHY IT IS THE APP'S CALL AND NOT THE COMPOSITOR'S: a resize re-runs
 * `on_layout`, and that function means two different things across this tree.
 * Most apps build their widgets in it; the editor only REPOSITIONS widgets it
 * created when the window opened.  Clearing the list for the second kind would
 * hand it freed pointers to write coordinates into, so the compositor cannot
 * safely decide — the app knows which it is.
 *
 * Skipping it in a building `on_layout` is the bug this exists for: every
 * resize left a whole second set of widgets behind the new one, still drawn at
 * the old geometry and still answering clicks there. */
void gui_window_clear_widgets(struct gui_window* win);

/* Diagnostics for the above: how many widgets each app window holds, and a way
 * to force the re-layout a resize would cause.  `gui widgets` / `gui relayout`.
 * The count is the test — a duplicated widget list is invisible in a
 * screenshot, because the new widgets draw over the old ones. */
void gui_widget_report(void);
void gui_relayout_all(void);
/* Count, re-layout N times, count again.  One command because a GUI window with
 * focus stops the harness typing the second one. */
void gui_relayout_test(int rounds);

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
