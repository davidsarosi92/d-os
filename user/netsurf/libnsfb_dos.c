/*
 * dos.c — d-os surface backend for libnsfb (§M42 NetSurf display bridge).
 *
 * Renders into a client-owned RAM buffer (like the ram surface) but PRESENTS
 * each frame to a WM-managed d-os window via three custom syscalls handled by
 * the kernel's linux-abi layer (kernel/gui/dosgui.c):
 *
 *   DOSGUI_CREATE(w, h, title)          -> window handle
 *   DOSGUI_PRESENT(handle, px, w,h,str) -> blit the buffer into the window
 *   DOSGUI_POLL(handle, &event)         -> pull one input event
 *
 * This is NOT part of upstream libnsfb; it is a d-os add-on compiled in by the
 * d-os Makefile so the NetSurf framebuffer frontend (`netsurf -f dos`) produces
 * a real, visible, WM-managed window.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"

#include "nsfb.h"
#include "surface.h"
#include "plot.h"

#define DOSGUI_CREATE  0xD050
#define DOSGUI_PRESENT 0xD051
#define DOSGUI_POLL    0xD052
#define DOSGUI_DESTROY 0xD053

/* Kernel dosgui_event layout (must match kernel/includes/dosgui.h). */
struct dos_event { int32_t type, keycode, pressed, x, y; };

static int dos_handle = -1;    /* one browser window per process is plenty */

static int dos_defaults(nsfb_t *nsfb)
{
    nsfb->width = 0;
    nsfb->height = 0;
    nsfb->format = NSFB_FMT_XRGB8888;   /* matches the d-os window surface */
    select_plotters(nsfb);
    return 0;
}

static int dos_initialise(nsfb_t *nsfb)
{
    size_t size = (size_t)(nsfb->width * nsfb->height * nsfb->bpp) / 8;
    uint8_t *fbptr = realloc(nsfb->ptr, size);
    if (fbptr == NULL)
        return -1;
    nsfb->ptr = fbptr;
    nsfb->linelen = (nsfb->width * nsfb->bpp) / 8;

    if (dos_handle < 0) {
        dos_handle = (int)syscall(DOSGUI_CREATE,
                                  (long)nsfb->width, (long)nsfb->height,
                                  (long)"NetSurf");
        if (dos_handle < 0)
            return -1;
    }
    return 0;
}

static int
dos_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    if (width > 0)  nsfb->width = width;
    if (height > 0) nsfb->height = height;
    if (format != NSFB_FMT_ANY) nsfb->format = format;
    select_plotters(nsfb);
    nsfb->linelen = (nsfb->width * nsfb->bpp) / 8;
    return 0;
}

static int dos_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)nsfb; (void)box;
    return 0;
}

/* Present the (dirty) surface to the window.  We push the whole surface — the
 * kernel blit is cheap and NetSurf's damage is usually most of the viewport. */
static int dos_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    (void)box;
    if (dos_handle >= 0 && nsfb->ptr != NULL) {
        syscall(DOSGUI_PRESENT, (long)dos_handle, (long)nsfb->ptr,
                (long)nsfb->width, (long)nsfb->height,
                (long)(nsfb->linelen / (nsfb->bpp / 8)));  /* stride in pixels */
    }
    return 0;
}

static int dos_finalise(nsfb_t *nsfb)
{
    if (dos_handle >= 0) {
        syscall(DOSGUI_DESTROY, (long)dos_handle);
        dos_handle = -1;
    }
    free(nsfb->ptr);
    nsfb->ptr = NULL;
    return 0;
}

static bool dos_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    (void)nsfb; (void)timeout;
    if (dos_handle < 0)
        return false;
    struct dos_event de;
    long r = syscall(DOSGUI_POLL, (long)dos_handle, (long)&de);
    if (r != 1)
        return false;
    if (de.type == 0) {                 /* key */
        event->type = de.pressed ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = (enum nsfb_key_code_e)de.keycode;
    } else {                            /* motion */
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = de.x;
        event->value.vector.y = de.y;
        event->value.vector.z = 0;
    }
    return true;
}

const nsfb_surface_rtns_t dos_rtns = {
    .defaults = dos_defaults,
    .initialise = dos_initialise,
    .finalise = dos_finalise,
    .input = dos_input,
    .claim = dos_claim,
    .update = dos_update,
    .geometry = dos_set_geometry,
};

/* Reuse the otherwise-unused NSFB_SURFACE_LINUX slot (no linux.c is built), so
 * the type is a valid enum; the frontend selects it by NAME via `netsurf -f dos`. */
NSFB_SURFACE_DEF(dos, NSFB_SURFACE_LINUX, &dos_rtns)
