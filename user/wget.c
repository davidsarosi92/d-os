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
 *     wget https://download.docker.com/linux/static/stable/x86_64/docker-29.9.1.tgz /mnt
 *                                              # → /mnt/docker-29.9.1.tgz (§M93:
 *                                              # redirects followed, status and
 *                                              # length checked, chunked decoded)
 *
 * Everything rides the Linux-ABI socket path (getaddrinfo/sendto/recvmsg/poll +
 * connect/read/write → linux_abi.c → the M24 stack).
 * ============================================================================= */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
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
static int g_last_errno;
static int bio_recv(void *ctx, unsigned char *b, size_t n) {
    long r = read(*(int *)ctx, b, n);
    if (r < 0) g_last_errno = errno;
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

/* §M93 (2026-10-16) — what a download of a vendor's release needs: REDIRECTS
 * (GitHub's and Adoptium's download links answer 302 to a CDN), the STATUS
 * checked (a 404 page saved as "docker.tgz" is the worst kind of success), the
 * length checked (a connection cut short is an error, not a smaller file),
 * chunked transfer decoded, a progress line every 4 MiB, and an output
 * DIRECTORY taking the file name from the URL (`wget <url> /mnt`).
 *
 * fetch(): 0 = the body was written, 1 = a redirect (`loc` holds the new URL),
 * -1 = a failure (said). */
static long g_total;
static long long g_have;          /* bytes already in the file (a resumed download) */
static long long g_full = -1;     /* the whole length, once a server has said it */
/* fetch(): 0 done, 1 redirect, -1 failure, -2 INCOMPLETE (worth resuming) */
static int fetch(const char *url, int ofd, char *loc, size_t loccap) {
    int https = 0;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else { printf("wget: only http:// and https:// URLs: '%s'\n", url); return -1; }
    char host[256]; int hi = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && hi < 255) host[hi++] = *p++;
    host[hi] = 0;
    int port = https ? 443 : 80;
    /* A port is 1..65535.  The parse used to accumulate any run of digits,
     * so ":99999999999" overflowed into whatever number came out and was
     * then dialled; it is refused now, with the URL named. */
    if (*p == ':') {
        p++; port = 0;
        while (*p >= '0' && *p <= '9' && port <= 65535) port = port*10 + (*p++ - '0');
        if (port < 1 || port > 65535 || (*p >= '0' && *p <= '9')) {
            printf("wget: bad port in URL '%s'\n", url); return -1;
        }
    }
    const char *path = (*p == '/' || *p == '?') ? p : "/";
    if (hi == 0) { printf("wget: bad URL '%s'\n", url); return -1; }

    char portstr[8]; snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) { printf("wget: cannot resolve %s (%d)\n", host, gai); return -1; }
    unsigned ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    printf("wget: %s = %u.%u.%u.%u, %s :%d\n", host,
           (ip>>24)&255, (ip>>16)&255, (ip>>8)&255, ip&255, https ? "https" : "http", port);

    static struct conn c;
    memset(&c, 0, sizeof c);
    c.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c.fd < 0) { printf("wget: socket() failed\n"); freeaddrinfo(res); return -1; }
    if (connect(c.fd, res->ai_addr, res->ai_addrlen) != 0) {
        printf("wget: connect failed\n"); close(c.fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    if (https && tls_start(&c, host) != 0) { close(c.fd); return -1; }

    char req[2048], range[64];
    range[0] = 0;
    if (g_have > 0) snprintf(range, sizeof range, "Range: bytes=%lld-\r\n", g_have);
    int rq = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: d-os-wget\r\nAccept: */*\r\n"
        "%sConnection: close\r\n\r\n", path, host, range);
    if (rq >= (int)sizeof req) { printf("wget: URL too long\n"); close(c.fd); return -1; }
    if (c_write(&c, req, (size_t)rq) < 0) { printf("wget: send failed\n"); close(c.fd); return -1; }

    /* the header, whole */
    static char hb[16384];
    size_t hl = 0;
    unsigned char buf[16384];
    long r = 0, body_off = -1;
    while (body_off < 0) {
        r = c_read(&c, buf, sizeof buf);
        if (r <= 0) { printf("wget: the connection closed before the response header\n"); close(c.fd); return -1; }
        for (long i = 0; i < r; i++) {
            if (hl < sizeof hb - 1) hb[hl++] = (char)buf[i];
            if (hl >= 4 && !memcmp(hb + hl - 4, "\r\n\r\n", 4)) { body_off = i + 1; break; }
        }
        if (body_off < 0 && hl >= sizeof hb - 1) { printf("wget: response header too large\n"); close(c.fd); return -1; }
    }
    hb[hl] = 0;
    int status = 0;
    { const char *sp = strchr(hb, ' '); if (sp) status = atoi(sp + 1); }
    long long clen = -1; int chunked = 0;
    loc[0] = 0;
    for (char *line = hb; *line; ) {
        char *e = strstr(line, "\r\n"); if (!e) break;
        *e = 0;
        if (!strncasecmp(line, "content-length:", 15)) clen = atoll(line + 15);
        if (!strncasecmp(line, "transfer-encoding:", 18) && strstr(line + 18, "chunked")) chunked = 1;
        if (!strncasecmp(line, "location:", 9)) {
            const char *v = line + 9; while (*v == ' ') v++;
            if (*v == '/') snprintf(loc, loccap, "%s://%s%s", https ? "https" : "http", host, v);
            else snprintf(loc, loccap, "%s", v);
        }
        line = e + 2;
    }
    if (status >= 300 && status < 400 && loc[0]) {
        printf("wget: %d - redirected to %.120s%s\n", status, loc, strlen(loc) > 120 ? "..." : "");
        if (c.tls) mbedtls_ssl_close_notify(&c.ssl);
        close(c.fd);
        return 1;
    }
    if (status == 206 && g_have > 0) {
        printf("wget: resuming at byte %lld\n", g_have);
    } else if (status == 200 && g_have > 0) {
        printf("wget: the server does not resume - starting again\n");
        if (ftruncate(ofd, 0) != 0) {}
        lseek(ofd, 0, SEEK_SET);
        g_have = 0;
    } else if (status != 200) {
        printf("wget: the server answered %d - nothing saved\n", status);
        if (c.tls) mbedtls_ssl_close_notify(&c.ssl);
        close(c.fd);
        return -1;
    }
    if (clen >= 0 && status == 200) { g_full = clen; printf("wget: %lld bytes (%lld MiB)\n", clen, clen >> 20); }

    /* the body: chunked or not */
    long long got = 0, next_report = ((g_have >> 22) + 1) << 22;
    long long chunk_left = 0; int cstate = 0;     /* 0 size line, 1 data, 2 CRLF after data */
    char cline[32]; int cl = 0;
    int done = 0;
    for (;;) {
        long i = body_off >= 0 ? body_off : 0;
        body_off = -1;
        while (i < r && !done) {
            if (!chunked) {
                long n = r - i;
                if (write(ofd, buf + i, (size_t)n) != n) { printf("wget: write failed (disk full?)\n"); close(c.fd); return -1; }
                got += n; i = r;
            } else if (cstate == 0) {
                char ch = (char)buf[i++];
                if (ch == '\n') {
                    cline[cl] = 0; cl = 0;
                    chunk_left = strtoll(cline, NULL, 16);
                    if (chunk_left == 0) done = 1; else cstate = 1;
                } else if (ch != '\r' && cl < 31) cline[cl++] = ch;
            } else if (cstate == 1) {
                long n = r - i;
                if (n > chunk_left) n = (long)chunk_left;
                if (write(ofd, buf + i, (size_t)n) != n) { printf("wget: write failed (disk full?)\n"); close(c.fd); return -1; }
                got += n; i += n; chunk_left -= n;
                if (!chunk_left) cstate = 2;
            } else {
                if (buf[i++] == '\n') cstate = 0;
            }
        }
        if (g_have + got >= next_report) { printf("wget: %lld MiB\n", (g_have + got) >> 20); next_report += 4 << 20; }
        if (done || (clen >= 0 && got >= clen)) break;
        r = c_read(&c, buf, sizeof buf);
        if (r <= 0) {
            if (r < 0) {
                char e[128] = {0};
                if (c.tls) mbedtls_strerror((int)r, e, sizeof e);
                printf("wget: the read failed after %lld bytes: %ld %s (errno %d)\n", got, r, e, g_last_errno);
            } else if (clen >= 0 && got < clen) {
                printf("wget: the peer closed the connection after %lld bytes\n", got);
            }
            break;
        }
    }
    if (c.tls) mbedtls_ssl_close_notify(&c.ssl);
    close(c.fd);
    g_have += got;
    g_total = (long)g_have;
    if (clen >= 0 && got != clen) {
        printf("wget: the connection ended after %lld of %lld bytes - incomplete\n",
               g_have, g_full >= 0 ? g_full : clen);
        return got > 0 ? -2 : -1;
    }
    if (chunked && !done) { printf("wget: the chunked body ended early - incomplete\n"); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: wget <url> [outfile | directory]\n"); return 2; }
    const char *url = argv[1];
    const char *outarg = (argc >= 3) ? argv[2] : NULL;

    /* an output DIRECTORY: the file is named after the URL's last component */
    char outpath[512]; outpath[0] = 0;
    if (outarg) {
        struct stat st;
        size_t ol = strlen(outarg);
        if ((stat(outarg, &st) == 0 && S_ISDIR(st.st_mode)) || (ol && outarg[ol - 1] == '/')) {
            const char *b = url, *q = url;
            for (; *q && *q != '?' && *q != '#'; q++) if (*q == '/') b = q + 1;
            if (q == b) { printf("wget: the URL names no file - give an output file name\n"); return 2; }
            snprintf(outpath, sizeof outpath, "%s%s%.*s", outarg, outarg[ol - 1] == '/' ? "" : "/",
                     (int)(q - b), b);
        } else snprintf(outpath, sizeof outpath, "%s", outarg);
    }
    int ofd = 1;
    if (outpath[0]) {
        ofd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { printf("wget: cannot create %s\n", outpath); return 1; }
    }
    static char cur[4096], loc[4096];
    snprintf(cur, sizeof cur, "%s", url);
    int rc = -1;
    int resumes = 0;
    for (int hop = 0; hop <= 10; hop++) {
        rc = fetch(cur, ofd, loc, sizeof loc);
        /* §M93 — a connection that broke part-way RESUMES (HTTP Range) from
         * the bytes already written, up to five times: a large release over
         * a slow or flaky link should not have to start again from zero. */
        if (rc == -2 && outpath[0] && resumes < 5) {
            resumes++;
            printf("wget: resuming (attempt %d of 5)\n", resumes);
            hop--;
            continue;
        }
        if (rc != 1) break;
        if (outpath[0]) { if (ftruncate(ofd, 0) != 0) {} lseek(ofd, 0, SEEK_SET); g_have = 0; }
        snprintf(cur, sizeof cur, "%s", loc);
        if (hop == 10) { printf("wget: too many redirects\n"); rc = -1; }
    }
    if (outpath[0]) close(ofd);
    if (rc != 0) {
        if (outpath[0]) unlink(outpath);                    /* no half file left behind */
        return 1;
    }
    printf("\nwget: %ld body bytes%s%s\n", g_total, outpath[0] ? " → " : "", outpath);
    return 0;
}
