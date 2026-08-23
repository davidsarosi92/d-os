/* =============================================================================
 * dosgui.h — the d-os display bridge for ring-3 graphical clients (§M42).
 *
 * A ring-3 program (e.g. the NetSurf framebuffer frontend via its libnsfb "dos"
 * surface backend) renders into a buffer IN ITS OWN address space, then hands
 * that buffer to the kernel to be composited into a WM-managed `gui_window`.
 * The transport is three linux-abi syscalls (see kernel/hal/x86_64/linux_abi.c),
 * which call straight through to these functions; the client buffer is read
 * directly out of user memory during the syscall (no shared mapping needed).
 *
 * This deliberately reuses the exact same present primitive (`gui_window_blit`)
 * the §M26 Wayland bridge uses — the browser is "just another compositor client".
 * ============================================================================= */
#ifndef DOSGUI_H
#define DOSGUI_H

#include <stdint.h>

/* Input event handed back to the client by dosgui_poll (mirrors gui_input). */
struct dosgui_event {
    int32_t type;      /* enum gui_input_type: 0 = key, 1 = motion,          */
                       /* 2 = button; 3 = close, 4 = RESIZE (dosgui-only —    */
                       /* neither has a gui_input, both are window state);     */
                       /* 5 = UI: a toolkit widget event (§M65) — keycode is   */
                       /* the widget id, pressed the UI_EV_* type, x the value */
    int32_t keycode;   /* key: raw scancode.  button: 1 = left, 2 = right     */
    int32_t pressed;   /* key/button: 1 = down, 0 = up                        */
    int32_t x, y;      /* motion/button: content-relative position;           */
                       /* RESIZE: the new content size in pixels              */
    int32_t ch;        /* key: the character the keymap produced, 0 if none    */
};

/* Create a WM window `w`x`h` titled `title` (copied); returns a small
 * non-negative handle, or -1 on failure (no compositor / table full). */
int dosgui_create(int w, int h, const char* title);

/* Blit the client's ARGB/XRGB `px` buffer (w x h, `stride` pixels per row) into
 * the window for `handle`.  Returns 0 on success, -1 on a bad handle. */
int dosgui_present(int handle, const uint32_t* px, int w, int h, int stride);

/* Dequeue one input event for `handle` into `out`.  Returns 1 if an event was
 * returned, 0 if the queue was empty, -1 on a bad handle. */
int dosgui_poll(int handle, struct dosgui_event* out);

/* §M65 — build the shared widget toolkit inside this window from a blob the
 * client sends: a header, fixed-size spec records and a string pool (offsets,
 * never pointers).  Widget events come back on the SAME queue dosgui_poll
 * already drains, as type 5 with (keycode = widget id, pressed = event type,
 * x = value).  Returns the number of widgets built, or -1.
 *
 * The blob format is described in dosgui.c; `user/uidemo.c` builds one. */
int dosgui_ui_build(int handle, const void* blob, int len);

/* Tear the window down (client exit / surface finalise). */
void dosgui_destroy(int handle);

#endif /* DOSGUI_H */
