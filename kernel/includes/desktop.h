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
