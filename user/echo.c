/* =============================================================================
 * echo.c — a minimal coreutils `echo`, linked against REAL musl (§M36).
 *
 * Prints its arguments separated by spaces, followed by a newline.  Compiled
 * with musl headers + statically linked with musl (see the muslelf rule in the
 * Makefile); installed into the §M35.5 store and run from /store by `pkgrun`
 * under the Linux personality (declared via the package's .abi = "linux").
 * ============================================================================= */
#include <stdio.h>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc) putchar(' ');
    }
    putchar('\n');
    return 0;
}
