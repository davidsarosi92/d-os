/* =============================================================================
 * nsutest.c — dyn musl program exercising libnsutils (§M42 NetSurf runway).
 * Base64-encodes then decodes a string and checks the round-trip.  Proves the
 * ported libnsutils.so.0 (a store package) loads + runs; libnsutils is a hard
 * dependency of the NetSurf binary (nsutils time/base64/unistd helpers).
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nsutils/base64.h>

int main(void) {
    const char *msg = "d-os NetSurf runway";
    uint8_t *enc = NULL, *dec = NULL;
    size_t enclen = 0, declen = 0;

    if (nsu_base64_encode_alloc((const uint8_t *)msg, strlen(msg),
                                &enc, &enclen) != NSUERROR_OK) {
        printf("nsutest: encode failed\n");
        return 1;
    }
    printf("nsutest: base64 = %.*s\n", (int)enclen, enc);

    if (nsu_base64_decode_alloc(enc, enclen, &dec, &declen) != NSUERROR_OK) {
        printf("nsutest: decode failed\n");
        return 1;
    }
    int ok = (declen == strlen(msg) && memcmp(dec, msg, declen) == 0);
    printf("nsutest: round-trip %s\n", ok ? "OK" : "FAIL");
    free(enc);
    free(dec);
    return ok ? 0 : 1;
}
