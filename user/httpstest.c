/* =============================================================================
 * httpstest.c — REAL HTTPS from an unmodified musl binary (§M39 stage 3b).
 *
 * The payoff of the whole §M39 arc: fetch https://example.com/ over the network
 * with genuine certificate verification.  Everything is real:
 *   - DNS over a UDP socket (SLIRP resolver), then a TCP connect to :443;
 *   - mbedTLS runs its SSL state machine with the BIO wired to that live socket
 *     (send/recv → the M24 stack, via musl's socketcall path → linux_abi.c);
 *   - the trust store is the provisioned CA bundle at /etc/ssl/cert.pem, and
 *     authmode is VERIFY_REQUIRED — the server's chain is validated against the
 *     Mozilla roots + the hostname is checked (SNI + CN/SAN), NOT disabled;
 *   - an HTTP/1.1 GET is written over TLS and the decrypted status line printed.
 *
 * The in-memory ssltest proved the TLS integration; this proves it over a real
 * socket against a real internet server.  Needs QEMU user networking.
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>

#define HOST "example.com"
#define PORT 443
#define CA_PATH "/etc/ssl/cert.pem"

/* ---- a blocking BIO over an M24 TCP socket ------------------------------- */
/* net_sockets.h (with its MBEDTLS_ERR_NET_*) is not part of this build; any
 * negative return from a BIO callback is treated as a fatal transport error by
 * mbedTLS, so a plain sentinel suffices. */
#define BIO_XPORT_ERR (-0x004C)
static int bio_send(void *ctx, const unsigned char *b, size_t n) {
    long w = write(*(int *)ctx, b, n);
    return w >= 0 ? (int)w : BIO_XPORT_ERR;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    long r = read(*(int *)ctx, b, n);            /* net_tcp_recv block-polls */
    return r >= 0 ? (int)r : BIO_XPORT_ERR;      /* 0 => EOF */
}

int main(void) {
    /* Resolve the hostname through musl's OWN getaddrinfo() — the resolver
     * reads /etc/resolv.conf (provisioned to nameserver 10.0.2.3, the SLIRP
     * stub), sends its A query over a UDP socket and parses the reply, all via
     * the Linux-ABI socket path (sendto + recvmsg + poll → linux_abi.c).  No
     * hand-rolled DNS anymore.  We pin AF_INET (no AAAA) since the stack is
     * IPv4-only. */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(HOST, "443", &hints, &res);
    if (gai != 0 || !res) {
        printf("https: getaddrinfo(%s) failed (%d)\n", HOST, gai); return 1;
    }
    struct sockaddr_in *ra = (struct sockaddr_in *)res->ai_addr;
    unsigned ip = ntohl(ra->sin_addr.s_addr);
    printf("https: %s = %u.%u.%u.%u\n", HOST,
           (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { printf("https: socket() failed\n"); freeaddrinfo(res); return 1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        printf("https: connect(:%d) failed\n", PORT); close(fd); freeaddrinfo(res); return 1;
    }
    freeaddrinfo(res);
    printf("https: TCP connected to :%d\n", PORT);

    psa_crypto_init();
    mbedtls_entropy_context entropy;  mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_context drbg;    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)"d-os-https", 10) != 0) {
        printf("https: RNG seed failed\n"); close(fd); return 1;
    }

    /* Load the CA trust bundle (provisioned at /etc/ssl/cert.pem).  Read it
     * into a NUL-terminated buffer and parse — real trust anchors. */
    mbedtls_x509_crt ca;  mbedtls_x509_crt_init(&ca);
    FILE *cf = fopen(CA_PATH, "rb");
    if (!cf) { printf("https: cannot open %s\n", CA_PATH); close(fd); return 1; }
    fseek(cf, 0, SEEK_END); long clen = ftell(cf); fseek(cf, 0, SEEK_SET);
    unsigned char *cabuf = malloc((size_t)clen + 1);
    if (!cabuf || fread(cabuf, 1, (size_t)clen, cf) != (size_t)clen) {
        printf("https: CA read failed\n"); fclose(cf); close(fd); return 1;
    }
    fclose(cf);
    cabuf[clen] = 0;                                    /* PEM needs the NUL */
    int nca = mbedtls_x509_crt_parse(&ca, cabuf, (size_t)clen + 1);
    printf("https: CA bundle parsed (%d certs rejected)\n", nca);
    if (nca < 0) { printf("https: CA parse failed\n"); close(fd); return 1; }

    mbedtls_ssl_config conf;  mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);  /* REAL */

    mbedtls_ssl_context ssl;  mbedtls_ssl_init(&ssl);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) { printf("https: ssl_setup failed\n"); close(fd); return 1; }
    mbedtls_ssl_set_hostname(&ssl, HOST);       /* SNI + cert name check */
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);

    int hs;
    while ((hs = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char e[128] = {0}; mbedtls_strerror(hs, e, sizeof e);
            printf("https: handshake FAILED -0x%x %s\n", (unsigned)(-hs), e);
            printf("https: verify flags = 0x%x\n",
                   (unsigned)mbedtls_ssl_get_verify_result(&ssl));
            return 1;
        }
    }
    uint32_t vf = mbedtls_ssl_get_verify_result(&ssl);
    printf("https: handshake OK — %s / %s\n",
           mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
    printf("https: cert verify flags = 0x%x (0 = chain + hostname trusted)\n",
           (unsigned)vf);

    const char *req = "GET / HTTP/1.1\r\nHost: " HOST "\r\nConnection: close\r\n\r\n";
    mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req));

    unsigned char buf[2048]; int r = mbedtls_ssl_read(&ssl, buf, sizeof buf - 1);
    if (r <= 0) { printf("https: no TLS response (r=%d)\n", r); return 1; }
    buf[r] = 0;
    char *nl = strchr((char *)buf, '\r'); if (nl) *nl = 0;
    printf("https: HTTP status over TLS: %s\n", (char *)buf);

    mbedtls_ssl_close_notify(&ssl);
    close(fd);
    int ok = (vf == 0) && (strncmp((char *)buf, "HTTP/1.", 7) == 0);
    printf("https: %s\n", ok ? "HTTPS PASS" : "HTTPS FAIL");
    return ok ? 0 : 1;
}
