/* =============================================================================
 * wget.c — a real HTTP/HTTPS download front-end for d-os (§M39).
 *
 * An unmodified musl userland program: it takes a URL on argv, resolves the host
 * with musl's own getaddrinfo() (→ /etc/resolv.conf → the M24 UDP/DNS path), and
 * fetches the resource.  For https:// it runs a full mbedTLS 1.3 handshake with
 * genuine certificate verification against the provisioned CA bundle
 * (/etc/ssl/cert.pem) — the same proven path as httpstest, generalized.  The
 * response body (headers stripped) is streamed to stdout, or to a file when a
 * second argument is given:
 *
 *     wget http://example.com/                 # body → stdout (→ serial/pane)
 *     wget https://example.com/ /tmp/page.html # body → a VFS file
 *
 * Everything rides the Linux-ABI socket path (getaddrinfo/sendto/recvmsg/poll +
 * connect/read/write → linux_abi.c → the M24 stack).
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
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

#define CA_PATH "/etc/ssl/cert.pem"

/* A tiny transport abstraction so the HTTP request/response loop is written once
 * and works over both a plain socket and a TLS session. */
struct conn {
    int   fd;
    int   tls;                  /* 1 => use the mbedtls_ssl_context below */
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    ca;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
};

static long c_write(struct conn *c, const void *b, size_t n) {
    if (c->tls) return mbedtls_ssl_write(&c->ssl, b, n);
    return write(c->fd, b, n);
}
static long c_read(struct conn *c, void *b, size_t n) {
    if (c->tls) {
        int r = mbedtls_ssl_read(&c->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;   /* clean EOF */
        return r;
    }
    return read(c->fd, b, n);
}

/* ---- BIO over the M24 socket (TLS transport), same shape as httpstest ----- */
#define BIO_XPORT_ERR (-0x004C)
static int bio_send(void *ctx, const unsigned char *b, size_t n) {
    long w = write(*(int *)ctx, b, n);
    return w >= 0 ? (int)w : BIO_XPORT_ERR;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    long r = read(*(int *)ctx, b, n);
    return r >= 0 ? (int)r : BIO_XPORT_ERR;
}

/* Bring up TLS on an already-connected socket, verifying the peer against the
 * CA bundle + hostname.  Returns 0 on success. */
static int tls_start(struct conn *c, const char *host) {
    c->tls = 1;
    psa_crypto_init();
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)"d-os-wget", 9) != 0) {
        printf("wget: RNG seed failed\n"); return -1;
    }
    mbedtls_x509_crt_init(&c->ca);
    FILE *cf = fopen(CA_PATH, "rb");
    if (!cf) { printf("wget: cannot open %s\n", CA_PATH); return -1; }
    fseek(cf, 0, SEEK_END); long clen = ftell(cf); fseek(cf, 0, SEEK_SET);
    unsigned char *cabuf = malloc((size_t)clen + 1);
    if (!cabuf || fread(cabuf, 1, (size_t)clen, cf) != (size_t)clen) {
        printf("wget: CA read failed\n"); fclose(cf); return -1;
    }
    fclose(cf);
    cabuf[clen] = 0;
    if (mbedtls_x509_crt_parse(&c->ca, cabuf, (size_t)clen + 1) < 0) {
        printf("wget: CA parse failed\n"); return -1;
    }
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) { printf("wget: ssl_setup failed\n"); return -1; }
    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
    int hs;
    while ((hs = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char e[128] = {0}; mbedtls_strerror(hs, e, sizeof e);
            printf("wget: TLS handshake FAILED -0x%x %s\n", (unsigned)(-hs), e);
            return -1;
        }
    }
    if (mbedtls_ssl_get_verify_result(&c->ssl) != 0) {
        printf("wget: certificate verification FAILED\n"); return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: wget <url> [outfile]\n"); return 2; }
    const char *url = argv[1];
    const char *outpath = (argc >= 3) ? argv[2] : NULL;

    /* ---- parse the URL: scheme://host[:port][/path] ---- */
    int https = 0;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    char host[256]; int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 255) host[hi++] = *p++;
    host[hi] = 0;
    int port = https ? 443 : 80;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port*10 + (*p++ - '0'); }
    const char *path = (*p == '/') ? p : "/";
    if (hi == 0) { printf("wget: bad URL '%s'\n", url); return 2; }

    /* ---- resolve via musl getaddrinfo (AF_INET; the stack is IPv4-only) ---- */
    char portstr[8]; snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) { printf("wget: cannot resolve %s (%d)\n", host, gai); return 1; }
    unsigned ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    printf("wget: %s = %u.%u.%u.%u, %s :%d\n", host,
           (ip>>24)&255, (ip>>16)&255, (ip>>8)&255, ip&255,
           https ? "https" : "http", port);

    struct conn c;
    memset(&c, 0, sizeof c);
    c.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c.fd < 0) { printf("wget: socket() failed\n"); freeaddrinfo(res); return 1; }
    if (connect(c.fd, res->ai_addr, res->ai_addrlen) != 0) {
        printf("wget: connect failed\n"); close(c.fd); freeaddrinfo(res); return 1;
    }
    freeaddrinfo(res);

    if (https && tls_start(&c, host) != 0) { close(c.fd); return 1; }

    /* ---- send the request ---- */
    char req[512];
    int rq = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: d-os-wget\r\n"
        "Connection: close\r\n\r\n", path, host);
    if (c_write(&c, req, (size_t)rq) < 0) { printf("wget: send failed\n"); return 1; }

    /* ---- open the output sink ---- */
    int ofd = 1;                                     /* default: stdout */
    if (outpath) {
        ofd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { printf("wget: cannot create %s\n", outpath); return 1; }
    }

    /* ---- read the response, strip headers, stream the body ---- */
    unsigned char buf[4096];
    int in_body = 0, match = 0;      /* CRLFCRLF state machine over the header */
    long total_body = 0, r;
    while ((r = c_read(&c, buf, sizeof buf)) > 0) {
        long i = 0;
        if (!in_body) {
            for (; i < r && !in_body; i++) {
                char ch = (char)buf[i];
                if ((match == 0 || match == 2) && ch == '\r') match++;
                else if ((match == 1 || match == 3) && ch == '\n') { match++; if (match == 4) { in_body = 1; } }
                else match = 0;
            }
        }
        if (in_body && i < r) {
            long bn = r - i;
            write(ofd, buf + i, (size_t)bn);
            total_body += bn;
        }
    }

    if (c.tls) mbedtls_ssl_close_notify(&c.ssl);
    close(c.fd);
    if (outpath && ofd >= 0) close(ofd);
    printf("\nwget: %ld body bytes%s%s\n", total_body,
           outpath ? " → " : "", outpath ? outpath : "");
    return 0;
}
