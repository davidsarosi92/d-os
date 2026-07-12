/* =============================================================================
 * cat.c — a minimal coreutils `cat`, linked against REAL musl (§M36).
 *
 * Concatenates its file arguments to stdout (stdin if none).  Exercises musl's
 * buffered file I/O — fopen/fread/fwrite/fclose — over d-os's Linux-ABI
 * open/read/close, proving the store→exec path works for a program that does
 * real file work (not just printf).  Run from the store by `pkgrun`.
 * ============================================================================= */
#include <stdio.h>

static void cat_stream(FILE* f) {
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        fwrite(buf, 1, n, stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) { cat_stream(stdin); return 0; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "cat: %s: cannot open\n", argv[i]); rc = 1; continue; }
        cat_stream(f);
        fclose(f);
    }
    return rc;
}
