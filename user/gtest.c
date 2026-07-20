/* =============================================================================
 * gtest.c — dyn musl program exercising libnsgif (§M42 NetSurf libs).
 * Scans a tiny embedded 1x1 GIF and reads back its dimensions/frame count.
 * Proves the ported libnsgif.so.0 (a store package) loads + decodes.
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <nsgif.h>

static nsgif_bitmap_t* bmp_create(int w, int h) { return calloc((size_t)w * h, 4); }
static void           bmp_destroy(nsgif_bitmap_t* b) { free(b); }
static uint8_t*       bmp_buffer(nsgif_bitmap_t* b) { return (uint8_t*)b; }

static const nsgif_bitmap_cb_vt vt = {
    .create = bmp_create, .destroy = bmp_destroy, .get_buffer = bmp_buffer,
};

/* A minimal, valid GIF89a: 1x1, global colour table, one image. */
static const unsigned char gif[] = {
    0x47,0x49,0x46,0x38,0x39,0x61, 0x01,0x00,0x01,0x00, 0x80,0x01,0x00,
    0x00,0x00,0x00, 0xff,0xff,0xff,
    0x21,0xf9,0x04,0x01,0x00,0x00,0x00,0x00,
    0x2c,0x00,0x00,0x00,0x00, 0x01,0x00,0x01,0x00,0x00,
    0x02,0x02,0x44,0x01,0x00, 0x3b
};

int main(void) {
    nsgif_t* g = NULL;
    if (nsgif_create(&vt, NSGIF_BITMAP_FMT_R8G8B8A8, &g) != NSGIF_OK || !g) {
        printf("gtest: nsgif_create failed\n");
        return 1;
    }
    nsgif_error e = nsgif_data_scan(g, sizeof gif, gif);
    const nsgif_info_t* info = nsgif_get_info(g);
    printf("gtest: libnsgif scanned GIF (%s): %ux%u, %u frame(s)\n",
           nsgif_strerror(e), info->width, info->height, info->frame_count);
    int ok = (info->width == 1 && info->height == 1);
    nsgif_destroy(g);
    return ok ? 0 : 1;
}
