/* =============================================================================
 * desktop.h — swappable desktop shell interface (M22.2).
 *
 * A desktop shell is the CHROME around the window manager: wallpaper
 * accents, taskbar, launcher menu, clock — the Cinnamon/Xfce role.
 * The compositor core (gui.c) owns windows, z-order, input routing and
 * damage; the shell only decorates and launches.
 *
 * Registration: drop a `struct desktop_shell` into the
 * `desktop_shells` linker section via DESKTOP_SHELL().  The active
 * shell is chosen at gui_start by the `gui.shell` config key
 * (`setconf gui.shell bare` before running `gui`); default is the
 * first registered entry whose name matches "vista", else entry 0.
 *
 * Threading contract (MUST read before implementing a shell):
 *   - `click` / `motion` are called from the MOUSE IRQ with the WM
 *     state lock held.  Keep them short; only touch shell-local state
 *     and the *_locked / queue services in gui_internal.h.  Return
 *     non-zero from `click` to consume the event (windows below won't
 *     see it).
 *   - `draw` runs on the compositor task, after the windows are
 *     composed, before the cursor.  Reading shell-local state the IRQ
 *     just changed is a benign one-frame race.
 *   - `second_tick` runs on the compositor task at ~1 Hz; do slow
 *     things (RTC port I/O) here, never in click/motion.  Return
 *     non-zero to request a repaint.
 * ============================================================================= */

#ifndef DESKTOP_H
#define DESKTOP_H

struct gfx_surface;

struct desktop_shell {
    const char* name;                   /* config value, e.g. "vista"     */

    /* Called once when the GUI starts with this shell active. */
    void (*init)(int screen_w, int screen_h);

    /* Pixels reserved at the BOTTOM of the screen for chrome (taskbar).
     * The WM keeps windows out of this strip.  0 = nothing reserved. */
    int  (*bottom_reserve)(void);

    /* Paint the chrome onto the backbuffer (compositor task). */
    void (*draw)(struct gfx_surface* back);

    /* §M64 — paint UNDER the windows, straight after the wallpaper blit and
     * before any window is composed (compositor task).  Desktop icons live
     * here: they are part of the background layer, not chrome floating over
     * the applications.  Drawing them in `draw` would put a shortcut icon on
     * top of every window, which is the one thing a desktop icon must never
     * do.  NULL = nothing under the windows. */
    void (*draw_under)(struct gfx_surface* back);

    /* §M64 — a left click that hit NO window and is not over the chrome, i.e.
     * a click on the desktop itself.  `dbl` is non-zero for a double click.
     *
     * UNLIKE `click`/`motion` below, this runs on the DESKTOP TASK with NO
     * lock held, so it may open files, allocate and launch apps — which is
     * exactly what activating a shortcut does.  It must still be quick: the
     * desktop task is also what repaints the chrome. */
    void (*desktop_click)(int x, int y, int dbl);

    /* §M64 tail — the pointer PHASE stream on the desktop: press, drag,
     * release, using §M58's vocabulary (WPTR_PRESS / WPTR_DRAG / WPTR_RELEASE)
     * rather than a second one, because this system should have one answer to
     * "what phase is this pointer event in".
     *
     * Like `desktop_click` and unlike `click`/`motion`, this runs on the
     * DESKTOP TASK with NO lock held — a release persists a shortcut's
     * position, which opens a file.
     *
     * The compositor GRABS on press: once a desktop drag has started, drag and
     * release arrive here no matter what the pointer is over.  Without the
     * grab, dragging an icon across a window would end the drag at the
     * window's edge, which is §M58's lesson stated one layer up.  NULL = this
     * shell does not arrange anything. */
    void (*desktop_pointer)(int x, int y, int phase);

    /* §M64 tail — a raw keycode that no window wanted.  The desktop is the
     * focus of last resort: with nothing open, the arrow keys and Enter have
     * no other owner, and until now they were DROPPED — an icon field you
     * could only reach with a mouse, which is half the reason §M65 added Tab
     * cycling to windows.
     *
     * Runs on the DESKTOP TASK with no lock held, like the two above, because
     * Enter launches an app.  NULL = this shell ignores the keyboard. */
    void (*desktop_key)(int keycode, int mods);

    /* Pointer events (mouse IRQ, WM lock held — see header comment). */
    int  (*click) (int x, int y);       /* non-zero = consumed           */
    void (*motion)(int x, int y);

    /* ~1 Hz housekeeping (compositor task).  Non-zero = repaint. */
    int  (*second_tick)(void);
};

extern struct desktop_shell __start_desktop_shells[];
extern struct desktop_shell __stop_desktop_shells[];

#define DESKTOP_SHELL(_var)                                              \
    static const struct desktop_shell                                    \
    __attribute__((used, section("desktop_shells"), aligned(4)))         \
    _var##_registration

/* Usage:
 *   DESKTOP_SHELL(vista) = { .name = "vista", .init = ..., ... };
 */

#endif
