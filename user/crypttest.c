/* =============================================================================
 * crypttest.c — Mbed TLS crypto primitives on d-os (§M39 stage 2).
 *
 * Proves the ported Mbed TLS crypto library works in ring 3: a SHA-256 known-
 * answer test (FIPS "abc" vector) and an AES-256-GCM encrypt→decrypt round-trip
 * with tag verification.  Uses primitives only (fixed key/IV), so it needs no
 * entropy or sockets — just the library linked against musl.  (Entropy-seeded
 * RNG + the TLS handshake come in stage 3.)
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <mbedtls/sha256.h>
#include <mbedtls/gcm.h>

static int check_sha256(void) {
    /* FIPS 180-4 test vector: SHA-256("abc"). */
    static const unsigned char want[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    unsigned char out[32];
    if (mbedtls_sha256((const unsigned char*)"abc", 3, out, 0) != 0) return 0;
    return memcmp(out, want, 32) == 0;
}

static int check_gcm(void) {
    unsigned char key[32] = {0};                 /* fixed test key */
    unsigned char iv[12]  = {1,2,3,4,5,6,7,8,9,10,11,12};
    const char*   pt      = "d-os mbedTLS AES-256-GCM round-trip";
    size_t        n       = strlen(pt);
    unsigned char ct[64], tag[16], dec[64];

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) != 0) return 0;
    int enc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, sizeof iv,
                                        NULL, 0, (const unsigned char*)pt, ct,
                                        sizeof tag, tag);
    int dc  = mbedtls_gcm_auth_decrypt(&g, n, iv, sizeof iv, NULL, 0, tag,
                                       sizeof tag, ct, dec);
    mbedtls_gcm_free(&g);
    if (enc != 0 || dc != 0) return 0;
    return n <= sizeof dec && memcmp(dec, pt, n) == 0;
}

int main(void) {
    int sha = check_sha256();
    int gcm = check_gcm();
    printf("crypttest: SHA-256 KAT      %s\n", sha ? "PASS" : "FAIL");
    printf("crypttest: AES-256-GCM RTT  %s\n", gcm ? "PASS" : "FAIL");
    printf("crypttest: %s\n", (sha && gcm) ? "all crypto OK" : "FAILURE");
    return (sha && gcm) ? 0 : 1;
}
