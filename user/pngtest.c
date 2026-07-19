/* =============================================================================
 * pngtest.c — dynamically-linked musl program exercising libpng (§M38 support
 * libs).  Encodes a tiny RGB image to an in-memory PNG (which drives libpng ->
 * zlib deflate) and checks the \x89PNG signature.  Proves the ported
 * libpng16.so.16 (a store package, DT_NEEDED libpng16.so.16 -> libz.so.1)
 * loads, relocates, and calls THROUGH into zlib across the store closure.
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <png.h>

struct membuf { unsigned char* p; size_t len, cap; };

static void mem_write(png_structp png, png_bytep data, png_size_t n) {
    struct membuf* m = (struct membuf*)png_get_io_ptr(png);
    if (m->len + n <= m->cap) memcpy(m->p + m->len, data, n);
    m->len += n;
}
static void mem_flush(png_structp png) { (void)png; }

int main(void) {
    unsigned char buf[8192];
    struct membuf m = { buf, 0, sizeof buf };

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { printf("pngtest: create_write_struct failed\n"); return 1; }
    png_infop info = png_create_info_struct(png);
    if (!info) { printf("pngtest: create_info_struct failed\n"); return 1; }
    if (setjmp(png_jmpbuf(png))) { printf("pngtest: libpng error (longjmp)\n"); return 1; }

    png_set_write_fn(png, &m, mem_write, mem_flush);

    const int W = 4, H = 4;
    png_set_IHDR(png, info, W, H, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    unsigned char row[4 * 3];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            row[x * 3 + 0] = (unsigned char)(x * 40);
            row[x * 3 + 1] = (unsigned char)(y * 40);
            row[x * 3 + 2] = 128;
        }
        png_write_row(png, row);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);

    int sig_ok = (m.len > 8 && buf[0] == 0x89 && buf[1] == 'P' &&
                  buf[2] == 'N' && buf[3] == 'G');
    printf("pngtest: libpng %s, encoded %dx%d RGB -> %lu-byte PNG, signature %s\n",
           png_get_libpng_ver(NULL), W, H, (unsigned long)m.len, sig_ok ? "OK" : "BAD");
    return sig_ok ? 0 : 1;
}
