/* =============================================================================
 * btest.c — dyn musl program exercising libnsbmp (§M42 NetSurf libs).
 * Analyses + decodes a tiny embedded 1x1 24-bit BMP and reads back its
 * dimensions.  Proves the ported libnsbmp.so.0 (a store package) loads +
 * decodes — completes the NetSurf image-decoder set alongside libnsgif.
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <libnsbmp.h>

static void* cb_create(int width, int height, unsigned int state) {
    (void)state;
    return calloc((size_t)width * height, 4);
}
static void cb_destroy(void* bitmap) { free(bitmap); }
static unsigned char* cb_get_buffer(void* bitmap) { return (unsigned char*)bitmap; }

/* A minimal, valid 1x1 24-bit BMP (BITMAPFILEHEADER + BITMAPINFOHEADER + one
 * BGR pixel padded to a 4-byte row). */
static unsigned char bmp[] = {
    /* file header */
    0x42,0x4D, 0x3A,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x36,0x00,0x00,0x00,
    /* info header (40 bytes) */
    0x28,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,
    0x01,0x00, 0x18,0x00, 0x00,0x00,0x00,0x00, 0x04,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    /* pixel data: one blue pixel (B,G,R) + 1 pad byte */
    0xFF,0x00,0x00, 0x00
};

int main(void) {
    bmp_bitmap_callback_vt vt = {
        .bitmap_create = cb_create,
        .bitmap_destroy = cb_destroy,
        .bitmap_get_buffer = cb_get_buffer,
    };
    bmp_image img;
    if (bmp_create(&img, &vt) != BMP_OK) { printf("btest: bmp_create failed\n"); return 1; }
    bmp_result r = bmp_analyse(&img, sizeof bmp, bmp);
    if (r != BMP_OK) { printf("btest: bmp_analyse -> %d\n", r); bmp_finalise(&img); return 1; }
    printf("btest: libnsbmp analysed BMP: %ux%u\n", img.width, img.height);
    r = bmp_decode(&img);
    int ok = (r == BMP_OK && img.width == 1 && img.height == 1);
    printf("btest: decode -> %d, %s\n", r, ok ? "OK" : "FAIL");
    bmp_finalise(&img);
    return ok ? 0 : 1;
}
