/* =============================================================================
 * putest.c — dyn musl program exercising libparserutils (§M42 NetSurf libs).
 * Resolves a charset alias to its MIB enum and back, driving the generated
 * charset alias table.  Proves the ported libparserutils.so.0 (a store package)
 * loads + runs.
 * ============================================================================= */
#include <stdio.h>
#include <parserutils/charset/mibenum.h>

int main(void) {
    uint16_t m = parserutils_charset_mibenum_from_name("UTF-8", 5);
    const char* n = parserutils_charset_mibenum_to_name(m);
    printf("putest: libparserutils charset: \"UTF-8\" -> mibenum %u -> \"%s\"\n",
           (unsigned)m, n ? n : "(null)");
    return m ? 0 : 1;
}
