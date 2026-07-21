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
    int32_t type;      /* 0 = key, 1 = motion (matches enum gui_input_type) */
    int32_t keycode;   /* key: raw scancode */
    int32_t pressed;   /* key: 1 = down, 0 = up */
    int32_t x, y;      /* motion: content-relative position */
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

/* Tear the window down (client exit / surface finalise). */
void dosgui_destroy(int handle);

#endif /* DOSGUI_H */
