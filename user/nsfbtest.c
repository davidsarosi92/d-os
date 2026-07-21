/* =============================================================================
 * nsfbtest.c — dyn musl program exercising libnsfb (§M42 NetSurf runway).
 * Creates a RAM surface, fills it with a solid colour via the plotter, then
 * reads a pixel back out of the surface buffer.  This is the exact path the
 * NetSurf framebuffer frontend uses to render — a RAM surface whose buffer we
 * later blit into a gui_window — so it proves the ported libnsfb.so.0 (a store
 * package) loads, registers the RAM surface (constructor), and plots.
 *
 * A neutral grey (R==G==B) is used so the check is independent of the surface's
 * channel order (XRGB vs XBGR both round-trip a grey unchanged).
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>   /* libnsfb_plot.h uses `bool` without including this */
#include <libnsfb.h>
#include <libnsfb_plot.h>

#define W 64
#define H 32
#define GREY 0x00777777u

int main(void) {
    nsfb_t *nsfb = nsfb_new(NSFB_SURFACE_RAM);
    if (nsfb == NULL) { printf("nsfbtest: nsfb_new failed\n"); return 1; }

    if (nsfb_set_geometry(nsfb, W, H, NSFB_FMT_XRGB8888) != 0) {
        printf("nsfbtest: set_geometry failed\n"); return 1;
    }
    if (nsfb_init(nsfb) != 0) { printf("nsfbtest: nsfb_init failed\n"); return 1; }

    uint8_t *buf = NULL;
    int linelen = 0;
    if (nsfb_get_buffer(nsfb, &buf, &linelen) != 0 || buf == NULL) {
        printf("nsfbtest: get_buffer failed\n"); return 1;
    }
    printf("nsfbtest: RAM surface %dx%d, linelen=%d\n", W, H, linelen);

    nsfb_bbox_t rect = { .x0 = 0, .y0 = 0, .x1 = W, .y1 = H };
    nsfb_claim(nsfb, &rect);
    if (!nsfb_plot_rectangle_fill(nsfb, &rect, GREY)) {
        printf("nsfbtest: rectangle_fill failed\n"); return 1;
    }
    nsfb_update(nsfb, &rect);

    uint32_t px = *(uint32_t *)(buf + 1 * linelen + 4 /* pixel (1,1) */);
    int ok = ((px & 0x00ffffffu) == GREY);
    printf("nsfbtest: pixel = 0x%08x, %s\n", px, ok ? "OK" : "FAIL");

    nsfb_free(nsfb);
    return ok ? 0 : 1;
}
