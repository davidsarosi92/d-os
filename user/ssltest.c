/* =============================================================================
 * ssltest.c — an in-memory TLS handshake on d-os (§M39 stage 3).
 *
 * Proves the TLS *integration* end to end WITHOUT external network
 * (deterministic, boot-testable): a client and a server mbedtls_ssl_context
 * complete a full handshake, driven in-process.  Two BIO callbacks ferry bytes
 * through two ring buffers (client→server, server→client); no sockets needed —
 * but the same mbedtls_ssl_set_bio seam takes real M24 socket send/recv for a
 * network client (stage 3b, `wget https`).
 *
 * Exercises the mbedTLS SSL state machine, the record layer (AEAD from §M39
 * stage 2), and the CSPRNG (§M39 stage 1 — the handshake randomness flows
 * mbedtls_entropy → getrandom → our kernel CSPRNG).  The server uses an
 * embedded self-signed ECDSA P-256 cert; the client trusts it as its CA, so
 * certificate verification is REAL (authmode = REQUIRED, not disabled).
 * (NB: named ssltest, not tlstest — user/tlstest.c is the M35 thread-local test.)
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>

/* Self-signed ECDSA P-256 cert with CA:TRUE + keyCertSign + serverAuth, so it
 * is a valid trust anchor (the client's CA) AND a valid TLS server cert. */
static const char srv_crt_pem[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBVjCB/aADAgECAgkAsjqMQCAF6pcwCgYIKoZIzj0EAwIwFDESMBAGA1UEAwwJ\n"
"ZC1vcy10ZXN0MB4XDTI2MDcxOTA4MTA1MVoXDTM2MDcxNjA4MTA1MVowFDESMBAG\n"
"A1UEAwwJZC1vcy10ZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE8gbgb5aQ\n"
"mn/LmyhCjfoNvQY8Z8AXAThRirIrbQrfkvdAaTHMwf8A4HQZTX0IkYw4n2ghZ8sf\n"
"zLHyIWMaU/lLiaM4MDYwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAoQw\n"
"EwYDVR0lBAwwCgYIKwYBBQUHAwEwCgYIKoZIzj0EAwIDSAAwRQIhALUVwkPiQyu3\n"
"c49zGhS/wULewsjcpPGIy9VJh8eRD4DyAiB1o1k+P+3G5OnrGSGFBIvbbAJ0jso6\n"
"zW1odYl1y4W4VA==\n"
"-----END CERTIFICATE-----\n";

static const char srv_key_pem[] =
"-----BEGIN EC PRIVATE KEY-----\n"
"MHcCAQEEICghyRwle7OM2crRm/5q+xcul6U7kxP/6+3gFSaR2q7foAoGCCqGSM49\n"
"AwEHoUQDQgAE8gbgb5aQmn/LmyhCjfoNvQY8Z8AXAThRirIrbQrfkvdAaTHMwf8A\n"
"4HQZTX0IkYw4n2ghZ8sfzLHyIWMaU/lLiQ==\n"
"-----END EC PRIVATE KEY-----\n";

/* One direction of the in-memory transport. */
#define RB 16384
struct pipe { unsigned char buf[RB]; int head, tail; };
static struct pipe c2s, s2c;                 /* client→server, server→client */
struct bio { struct pipe* tx; struct pipe* rx; };
static struct bio cli_bio = { &c2s, &s2c };
static struct bio srv_bio = { &s2c, &c2s };

static int bio_send(void* ctx, const unsigned char* b, size_t n) {
    struct pipe* p = ((struct bio*)ctx)->tx;
    size_t w = 0;
    while (w < n && ((p->tail + 1) % RB) != p->head) {
        p->buf[p->tail] = b[w++]; p->tail = (p->tail + 1) % RB;
    }
    return w ? (int)w : MBEDTLS_ERR_SSL_WANT_WRITE;
}
static int bio_recv(void* ctx, unsigned char* b, size_t n) {
    struct pipe* p = ((struct bio*)ctx)->rx;
    size_t r = 0;
    while (r < n && p->head != p->tail) {
        b[r++] = p->buf[p->head]; p->head = (p->head + 1) % RB;
    }
    return r ? (int)r : MBEDTLS_ERR_SSL_WANT_READ;
}

int main(void) {
    psa_crypto_init();

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char*)"d-os-tls", 8) != 0) {
        printf("ssltest: RNG seed FAILED (entropy source?)\n");
        return 1;
    }

    mbedtls_x509_crt srvcrt, ca;
    mbedtls_pk_context srvkey;
    mbedtls_x509_crt_init(&srvcrt); mbedtls_x509_crt_init(&ca);
    mbedtls_pk_init(&srvkey);
    int pc = mbedtls_x509_crt_parse(&srvcrt, (const unsigned char*)srv_crt_pem, sizeof srv_crt_pem);
    int pa = mbedtls_x509_crt_parse(&ca,     (const unsigned char*)srv_crt_pem, sizeof srv_crt_pem);
    int pk = mbedtls_pk_parse_key(&srvkey, (const unsigned char*)srv_key_pem, sizeof srv_key_pem,
                                  NULL, 0, mbedtls_ctr_drbg_random, &drbg);
    printf("ssltest: parse crt=%d ca=%d key=%d (0 = ok)\n", pc, pa, pk);
    if (pc != 0 || pa != 0 || pk != 0) { printf("ssltest: cert/key parse FAILED\n"); return 1; }

    mbedtls_ssl_config sconf, cconf;
    mbedtls_ssl_config_init(&sconf); mbedtls_ssl_config_init(&cconf);
    mbedtls_ssl_config_defaults(&sconf, MBEDTLS_SSL_IS_SERVER,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_config_defaults(&cconf, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&sconf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_rng(&cconf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_own_cert(&sconf, &srvcrt, &srvkey);
    mbedtls_ssl_conf_ca_chain(&cconf, &ca, NULL);
    /* REQUIRED: real certificate verification — the client validates the
     * server's cert against its trusted CA (the same self-signed CA:TRUE cert)
     * and aborts the handshake if it doesn't check out. */
    mbedtls_ssl_conf_authmode(&cconf, MBEDTLS_SSL_VERIFY_REQUIRED);

    mbedtls_ssl_context scli, ssrv;
    mbedtls_ssl_init(&scli); mbedtls_ssl_init(&ssrv);
    if (mbedtls_ssl_setup(&scli, &cconf) != 0 || mbedtls_ssl_setup(&ssrv, &sconf) != 0) {
        printf("ssltest: ssl_setup FAILED\n"); return 1;
    }
    mbedtls_ssl_set_hostname(&scli, "d-os-test");
    mbedtls_ssl_set_bio(&scli, &cli_bio, bio_send, bio_recv, NULL);
    mbedtls_ssl_set_bio(&ssrv, &srv_bio, bio_send, bio_recv, NULL);

    /* Drive the handshake: alternate the two ends until both complete. */
    int cs = MBEDTLS_ERR_SSL_WANT_READ, ss = MBEDTLS_ERR_SSL_WANT_READ;
    for (int i = 0; i < 80 && (cs != 0 || ss != 0); i++) {
        if (cs != 0) {
            cs = mbedtls_ssl_handshake(&scli);
            if (cs != 0 && cs != MBEDTLS_ERR_SSL_WANT_READ && cs != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        }
        if (ss != 0) {
            ss = mbedtls_ssl_handshake(&ssrv);
            if (ss != 0 && ss != MBEDTLS_ERR_SSL_WANT_READ && ss != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        }
    }

    if (cs != 0 || ss != 0) {
        char ec[128] = {0}, es[128] = {0};
        mbedtls_strerror(cs, ec, sizeof ec);
        mbedtls_strerror(ss, es, sizeof es);
        printf("ssltest: handshake FAILED\n  client=-0x%x %s\n  server=-0x%x %s\n",
               (unsigned)(-cs), ec, (unsigned)(-ss), es);
        printf("ssltest: client cert verify flags = 0x%x\n",
               (unsigned)mbedtls_ssl_get_verify_result(&scli));
        return 1;
    }

    printf("ssltest: handshake OK — %s / %s\n",
           mbedtls_ssl_get_version(&scli), mbedtls_ssl_get_ciphersuite(&scli));
    printf("ssltest: cert verify flags = 0x%x (0 = trusted)\n",
           (unsigned)mbedtls_ssl_get_verify_result(&scli));

    /* Exchange one application record each way over the established session. */
    const char* msg = "hello over TLS from d-os";
    mbedtls_ssl_write(&scli, (const unsigned char*)msg, strlen(msg));
    unsigned char got[64] = {0};
    int r = mbedtls_ssl_read(&ssrv, got, sizeof got - 1);
    int ok = (r == (int)strlen(msg)) && memcmp(got, msg, strlen(msg)) == 0;
    printf("ssltest: app data over TLS: \"%s\" (%s)\n", got, ok ? "match" : "MISMATCH");
    printf("ssltest: %s\n", ok ? "TLS PASS" : "TLS FAIL");
    return ok ? 0 : 1;
}
