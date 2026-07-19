/* =============================================================================
 * ztest.c — dynamically-linked musl program that exercises zlib (§M38 support
 * libs).  Proves the ported zlib.so (a store package, DT_NEEDED libz.so.1)
 * loads + relocates + runs: a compress()/uncompress() round-trip.
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void) {
    const char* src =
        "d-os zlib test: the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog. "
        "the quick brown fox jumps over the lazy dog.";
    unsigned long slen = (unsigned long)strlen(src) + 1;

    unsigned char comp[512];
    unsigned long clen = sizeof comp;
    if (compress(comp, &clen, (const unsigned char*)src, slen) != Z_OK) {
        printf("ztest: compress failed\n");
        return 1;
    }

    unsigned char decomp[512];
    unsigned long dlen = sizeof decomp;
    if (uncompress(decomp, &dlen, comp, clen) != Z_OK) {
        printf("ztest: uncompress failed\n");
        return 1;
    }

    printf("ztest: zlib %s, %lu -> %lu -> %lu bytes, roundtrip %s\n",
           zlibVersion(), slen, clen, dlen,
           strcmp(src, (char*)decomp) == 0 ? "OK" : "MISMATCH");
    return 0;
}
