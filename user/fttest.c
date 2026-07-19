/* =============================================================================
 * fttest.c — dynamically-linked musl program exercising FreeType (§M38 support
 * libs).  Initialises the library (which pulls in FreeType's whole module-
 * registration chain) and reports the version.  Proves the ported
 * libfreetype.so.6 (a store package, DT_NEEDED libfreetype.so.6 -> libz.so.1)
 * loads, relocates, and runs.  Glyph rasterisation from a real font awaits a
 * font file in the VFS — a follow-up.
 * ============================================================================= */
#include <stdio.h>
#include <ft2build.h>
#include FT_FREETYPE_H

int main(void) {
    FT_Library lib;
    FT_Error err = FT_Init_FreeType(&lib);
    if (err) { printf("fttest: FT_Init_FreeType failed (err=%d)\n", err); return 1; }

    FT_Int major = 0, minor = 0, patch = 0;
    FT_Library_Version(lib, &major, &minor, &patch);
    printf("fttest: FreeType %d.%d.%d initialised OK\n", major, minor, patch);

    FT_Done_FreeType(lib);
    return 0;
}
