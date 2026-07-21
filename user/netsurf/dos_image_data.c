/* =============================================================================
 * dos_image_data.c — d-os stubs for the NetSurf framebuffer frontend's built-in
 * toolbar/cursor/throbber bitmaps.
 *
 * Upstream these are generated at build time by the `convert_image` tool from
 * the PNGs in frontends/framebuffer/res/{icons,pointers,throbber}.  For the
 * first-render bring-up (headless about:welcome) the chrome images are not on
 * the layout/paint path, so we provide ABI-compatible 1x1 fully-transparent
 * bitmaps for every symbol image_data.h declares.  Replacing these with the
 * real converted images later is a drop-in (delete this file, add the generated
 * image-*.c set).
 * ============================================================================= */
#include <stdint.h>
#include <stdbool.h>
/* Mirror fbtk.c's include order: fbtk.h uses nsfb_t / nsfb_event_t / bbox_t,
 * all provided by these libnsfb headers. */
#include <libnsfb.h>
#include <libnsfb_event.h>
#include <libnsfb_plot.h>
#include <libnsfb_plot_util.h>
#include "framebuffer/gui.h"    /* typedef ... bbox_t — used by fbtk.h */
#include "framebuffer/fbtk.h"

/* One shared 1x1 RGBA transparent pixel backs every stub bitmap. */
static uint8_t dos_blank_px[4] = { 0, 0, 0, 0 };

#define STUB_BITMAP(name) \
    struct fbtk_bitmap name = { \
        .width = 1, .height = 1, .pixdata = dos_blank_px, \
        .opaque = false, .hot_x = 0, .hot_y = 0 }

/* Toolbar icons (active + greyed `_g` variants). */
STUB_BITMAP(left_arrow);      STUB_BITMAP(left_arrow_g);
STUB_BITMAP(right_arrow);     STUB_BITMAP(right_arrow_g);
STUB_BITMAP(reload);          STUB_BITMAP(reload_g);
STUB_BITMAP(stop_image);      STUB_BITMAP(stop_image_g);
STUB_BITMAP(history_image);   STUB_BITMAP(history_image_g);

/* Scrollbar arrows. */
STUB_BITMAP(scrolll);         STUB_BITMAP(scrollr);
STUB_BITMAP(scrollu);         STUB_BITMAP(scrolld);

/* Pointers / cursors. */
STUB_BITMAP(pointer_image);   STUB_BITMAP(hand_image);
STUB_BITMAP(caret_image);     STUB_BITMAP(menu_image);
STUB_BITMAP(progress_image);  STUB_BITMAP(move_image);
STUB_BITMAP(osk_image);

/* Throbber animation frames. */
STUB_BITMAP(throbber0); STUB_BITMAP(throbber1); STUB_BITMAP(throbber2);
STUB_BITMAP(throbber3); STUB_BITMAP(throbber4); STUB_BITMAP(throbber5);
STUB_BITMAP(throbber6); STUB_BITMAP(throbber7); STUB_BITMAP(throbber8);
