/* =============================================================================
 * fetch_dos.c — the http/https fetcher for NetSurf on d-os.
 *
 * NetSurf was ported without one.  Its curated source list carries only the
 * data:, resource: and file: fetchers, and the curl fetcher is compiled out,
 * so the browser could render a local page and nothing else: typing an address
 * did nothing, and clicking a link to the web did nothing.  Not a bug in the
 * input path — there was simply no code that could speak HTTP.
 *
 * Rather than cross-build libcurl (and a TLS stack under it) this uses what
 * d-os already has and already proves in `wget`: ring-3 BSD sockets, musl's
 * getaddrinfo, and — for https — Mbed TLS.
 *
 * ---------------------------------------------------------------------------
 * How it attaches without touching the vendored tree
 * ---------------------------------------------------------------------------
 * `fetcher_init()` already calls `fetch_curl_register()`, guarded by
 * WITH_CURL.  So this file DEFINES that symbol and registers itself under the
 * http and https schemes; content/fetch.c is compiled with -DWITH_CURL and
 * fetchers/curl.c is not compiled at all.  NetSurf's own extension point,
 * upstream sources unmodified.
 *
 * (WITH_CURL is set for content/fetch.c ALONE.  utils/time.c also tests it,
 * and would switch to curl_getdate for HTTP date parsing — a symbol we do not
 * have.  Defining it globally turns a working date parser into a link error.)
 *
 * ---------------------------------------------------------------------------
 * Where the blocking went
 * ---------------------------------------------------------------------------
 * The obvious way to keep a UI responsive is a non-blocking socket and an
 * incremental poll.  That does not work here: d-os polls network RX from the
 * CALLING TASK rather than from an interrupt, so the blocking read is what
 * drives the NIC — a non-blocking one returns EAGAIN forever and the response
 * never arrives at all.  The blocking cannot be removed, only moved off the
 * thread that draws.
 *
 * So each transfer runs on its own pthread and `poll` becomes what it should
 * be: a check for finished work.  The split is strict — the worker touches
 * ONLY sockets, TLS and its own context, while every NetSurf callback happens
 * on the main thread inside poll.  NetSurf's core is not thread-safe, and a
 * fetcher calling back from a worker would corrupt the content cache in ways
 * that look nothing like a threading bug.
 * ============================================================================= */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>

#ifdef DOS_FETCH_TLS
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <psa/crypto.h>
#endif

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "content/fetch.h"
#include "content/fetchers.h"

/* Response cap.  A browser must not be talked into unbounded allocation by a
 * server that never closes; 8 MiB is far past any page we can lay out and far
 * below what d-os can spare. */
#define DOS_FETCH_MAX_BODY  (8u << 20)
#define DOS_FETCH_CHUNK     (16u << 10)
#define DOS_FETCH_CA_PATH   "/etc/ssl/cert.pem"

/* One transport, two backings, so the HTTP request/response code is written
 * once.  Same shape as `wget`'s — deliberately, because that one is proven on
 * this stack and divergence here would be divergence in the part that is
 * hardest to debug. */
struct conn {
    int fd;
    int tls;
#ifdef DOS_FETCH_TLS
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
#endif
};

#ifdef DOS_FETCH_TLS
/* BIO over the plain socket.  write/read, not send/recv — see send_all.
 *
 * The transport error code is mbedTLS's generic one rather than
 * MBEDTLS_ERR_NET_*: those live in net_sockets.h, which is the module we are
 * replacing, and pulling it in just for two constants would drag in the POSIX
 * socket layer we deliberately bypass. */
#define DOS_BIO_ERR  MBEDTLS_ERR_SSL_INTERNAL_ERROR

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    long w = write(*(int *)ctx, b, n);
    return w >= 0 ? (int)w : DOS_BIO_ERR;
}
static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    long r = read(*(int *)ctx, b, n);
    return r >= 0 ? (int)r : DOS_BIO_ERR;
}

/* Verify against the CA bundle AND the hostname.  A browser that skips either
 * is worse than one that cannot do https at all, because it looks like it can. */
static int tls_start(struct conn *c, const char *host)
{
    c->tls = 1;
    psa_crypto_init();
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)"d-os-netsurf", 12) != 0)
        return -1;

    mbedtls_x509_crt_init(&c->ca);
    FILE *cf = fopen(DOS_FETCH_CA_PATH, "rb");
    if (!cf) { fprintf(stderr, "fetch_dos: no %s\n", DOS_FETCH_CA_PATH); return -1; }
    fseek(cf, 0, SEEK_END); long clen = ftell(cf); fseek(cf, 0, SEEK_SET);
    unsigned char *cab = clen > 0 ? malloc((size_t)clen + 1) : NULL;
    if (!cab || fread(cab, 1, (size_t)clen, cf) != (size_t)clen) {
        fclose(cf); free(cab); return -1;
    }
    fclose(cf);
    cab[clen] = 0;
    int rc = mbedtls_x509_crt_parse(&c->ca, cab, (size_t)clen + 1);
    free(cab);
    if (rc < 0) return -1;

    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_init(&c->ssl);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) return -1;
    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

    int hs;
    while ((hs = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char e[128] = {0};
            mbedtls_strerror(hs, e, sizeof e);
            fprintf(stderr, "fetch_dos: TLS handshake failed -0x%x %s\n",
                    (unsigned)(-hs), e);
            return -1;
        }
    }
    return 0;
}

static void tls_end(struct conn *c)
{
    if (!c->tls) return;
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    c->tls = 0;
}
#endif /* DOS_FETCH_TLS */

static long c_write(struct conn *c, const void *b, size_t n)
{
#ifdef DOS_FETCH_TLS
    if (c->tls) return mbedtls_ssl_write(&c->ssl, b, n);
#endif
    return write(c->fd, b, n);
}
static long c_read(struct conn *c, void *b, size_t n)
{
#ifdef DOS_FETCH_TLS
    if (c->tls) {
        int r = mbedtls_ssl_read(&c->ssl, b, n);
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return r;
    }
#endif
    return read(c->fd, b, n);
}

struct dos_fetch {
    struct fetch  *parent;          /* NetSurf's handle for callbacks        */
    struct nsurl  *url;             /* owned reference                       */
    bool           only_2xx;
    bool           aborted;
    bool           started;

    /* The worker and what it hands back. */
    pthread_t      th;
    bool           th_live;         /* created and not yet joined            */
    volatile bool  done;            /* worker finished (success or failure)  */
    bool           delivered;       /* result already given to NetSurf       */
    char          *resp;            /* malloc'd response, or NULL            */
    size_t         resp_len;
    const char    *err;             /* failure reason, or NULL on success    */

    struct dos_fetch *next;
};

/* Fetches live here from `start` until `free`.  Only the main thread touches
 * the list — a worker writes solely into its own context. */
static struct dos_fetch *dos_ring;

/* musl's default thread stack is small and the TLS handshake is not. */
#define DOS_FETCH_STACK  (256u << 10)

/* ---------------------------------------------------------------------------
 * URL splitting.
 *
 * Done on the flat string from nsurl_access rather than through the nsurl
 * component API: we need exactly host / port / path, and the string form is
 * already normalised by the time a fetcher sees it.
 * --------------------------------------------------------------------------- */
struct urlparts {
    char host[256];
    char path[1024];
    int  port;
    bool tls;
};

static bool split_url(const char *u, struct urlparts *p)
{
    memset(p, 0, sizeof *p);
    const char *rest;
    if (strncmp(u, "https://", 8) == 0)     { p->tls = true;  p->port = 443; rest = u + 8; }
    else if (strncmp(u, "http://", 7) == 0) { p->tls = false; p->port = 80;  rest = u + 7; }
    else return false;

    /* Authority runs to the first '/', '?' or '#'. */
    size_t n = strcspn(rest, "/?#");
    if (n == 0 || n >= sizeof p->host) return false;
    memcpy(p->host, rest, n);
    p->host[n] = '\0';

    /* Strip any userinfo, then split off an explicit :port. */
    char *at = strrchr(p->host, '@');
    if (at) memmove(p->host, at + 1, strlen(at + 1) + 1);
    char *colon = strrchr(p->host, ':');
    if (colon && strchr(colon, ']') == NULL) {      /* not an IPv6 literal */
        *colon = '\0';
        int v = atoi(colon + 1);
        if (v > 0 && v < 65536) p->port = v;
    }
    if (p->host[0] == '\0') return false;

    /* Path (with query); the fragment is client-side and never sent. */
    const char *path = rest + n;
    if (*path == '\0' || *path == '#') {
        strcpy(p->path, "/");
    } else {
        size_t plen = strcspn(path, "#");
        if (plen >= sizeof p->path) plen = sizeof p->path - 1;
        memcpy(p->path, path, plen);
        p->path[plen] = '\0';
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Transport.  One connected socket, or -1.
 * --------------------------------------------------------------------------- */
static int tcp_connect(const char *host, int port)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;         /* d-os has no IPv6 stack yet */
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || res == NULL) {
        NSLOG(netsurf, INFO, "fetch_dos: cannot resolve %s", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        NSLOG(netsurf, INFO, "fetch_dos: connect to %s:%d failed", host, port);
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void conn_close(struct conn *c)
{
#ifdef DOS_FETCH_TLS
    tls_end(c);
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* write()/read(), NOT send()/recv().
 *
 * d-os routes ring-3 sockets through the Linux-ABI layer, and that layer wires
 * connect/read/write for a connected TCP socket — the send/recv entry points
 * are for the datagram paths.  Using them here connected successfully, appeared
 * to send, and then never received a byte, while `wget` fetched the very same
 * URL: the difference was exactly this pair of calls. */
static bool send_all(struct conn *c, const char *buf, size_t len)
{
    while (len) {
        long w = c_write(c, buf, len);
        if (w <= 0) return false;
        buf += w; len -= (size_t)w;
    }
    return true;
}

/* Read a whole HTTP response.
 *
 * NOT "read until EOF", which is the obvious implementation and does not work
 * here: d-os's TCP does not surface the peer's FIN as recv() == 0, so a reader
 * waiting for end-of-stream waits forever — the fetch connected, sent, and then
 * hung with the page never arriving.
 *
 * So the length is taken from the protocol instead of from the transport:
 * read until the header terminator, take Content-Length, then read exactly that
 * many body bytes, and stop.  Nothing ever waits for end-of-stream.
 *
 * The recv stays BLOCKING, which is not an oversight.  d-os polls network RX
 * from the calling task rather than from an interrupt, so a blocking recv is
 * what drives the NIC; a non-blocking one returns EAGAIN forever and the
 * response never arrives at all (first attempt at this read 0 bytes in 30
 * seconds from a server that had already answered).  Non-blocking is used only
 * for the fallback below, where terminating matters more than completeness.
 *
 * Returns a malloc'd buffer, or NULL. */
#define DOS_FETCH_IDLE_MS   4000        /* no data for this long = finished  */
#define DOS_FETCH_TOTAL_MS  30000       /* hard ceiling on one transfer      */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Offset just past the header terminator, or 0 if not seen yet. */
static size_t header_end(const char *b, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (b[i] == '\n' && b[i + 1] == '\n') return i + 2;
        if (i + 3 < len && b[i] == '\r' && b[i+1] == '\n' &&
            b[i+2] == '\r' && b[i+3] == '\n') return i + 4;
    }
    return 0;
}

/* Does the header block announce a chunked body? */
static bool is_chunked(const char *b, size_t hdr_len)
{
    static const char key[] = "transfer-encoding:";
    for (size_t i = 0; i + sizeof key - 1 < hdr_len; i++) {
        if (i && b[i - 1] != '\n') continue;
        size_t k = 0;
        while (k < sizeof key - 1 && (b[i + k] | 0x20) == key[k]) k++;
        if (k != sizeof key - 1) continue;
        /* Only "chunked" matters to us; any other coding we cannot undo. */
        for (size_t j = i + k; j + 7 <= hdr_len; j++) {
            if (b[j] == '\n') break;
            if ((b[j] | 0x20) == 'c' && strncasecmp(b + j, "chunked", 7) == 0)
                return true;
        }
        return false;
    }
    return false;
}

/* Content-Length within the header block, or -1 if absent/unparsable. */
static long content_length(const char *b, size_t hdr_len)
{
    static const char key[] = "content-length:";
    for (size_t i = 0; i + sizeof key - 1 < hdr_len; i++) {
        if (i && b[i - 1] != '\n') continue;           /* start of a line only */
        size_t k = 0;
        while (k < sizeof key - 1 &&
               (b[i + k] | 0x20) == key[k]) k++;        /* case-insensitive */
        if (k != sizeof key - 1) continue;
        const char *v = b + i + k;
        while (v < b + hdr_len && (*v == ' ' || *v == '\t')) v++;
        long n = 0; int digits = 0;
        while (v < b + hdr_len && *v >= '0' && *v <= '9') { n = n * 10 + (*v++ - '0'); digits++; }
        return digits ? n : -1;
    }
    return -1;
}

static char *read_all(struct conn *c, size_t *out_len)
{
    size_t cap = DOS_FETCH_CHUNK, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    long start = now_ms(), last = start;
    size_t hdr = 0;
    long clen = -1;
    bool chunked = false;
    int nonblock = 0;

    for (;;) {
        if (len + DOS_FETCH_CHUNK > cap) {
            if (cap >= DOS_FETCH_MAX_BODY) break;      /* refuse to grow further */
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); return NULL; }
            buf = nb; cap = ncap;
        }

        long r = c_read(c, buf + len, DOS_FETCH_CHUNK);
        if (r > 0) {
            len += (size_t)r;
            last = now_ms();
            if (!hdr) {
                hdr = header_end(buf, len);
                if (hdr) {
                    clen = content_length(buf, hdr);
                    chunked = is_chunked(buf, hdr);
                    if (clen < 0 && !chunked) {
                        /* No declared length: we cannot know when the body ends
                         * except by the stream ending, which this transport
                         * never reports.  Drop to non-blocking and take what
                         * arrives before it goes quiet — best effort, but it
                         * terminates. */
                        /* Only meaningful for the plain path; a TLS record
                         * layer cannot be drained by flipping the socket. */
                        int fl = c->tls ? -1 : fcntl(c->fd, F_GETFL, 0);
                        if (fl >= 0 && fcntl(c->fd, F_SETFL, fl | O_NONBLOCK) == 0)
                            nonblock = 1;
                    }
                }
            }
            if (hdr && clen >= 0 && len >= hdr + (size_t)clen) break;   /* complete */
            /* A chunked body ends with a zero-length chunk, which is the only
             * in-band signal we get — the transport never reports EOF. */
            if (hdr && chunked && len >= hdr + 5 &&
                memcmp(buf + len - 5, "0\r\n\r\n", 5) == 0) break;
            continue;
        }
        if (r == 0) break;                              /* peer closed (if ever) */
        if (errno == EINTR) continue;
        if (!nonblock) break;                           /* blocking error = give up */
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;

        long t = now_ms();
        if (len && t - last > DOS_FETCH_IDLE_MS) break; /* gone quiet = done */
        if (t - start > DOS_FETCH_TOTAL_MS)       break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };   /* 10 ms */
        nanosleep(&ts, NULL);
    }
    *out_len = len;
    return buf;
}

/* ---------------------------------------------------------------------------
 * Response delivery.
 * --------------------------------------------------------------------------- */
static void send_msg(struct dos_fetch *c, fetch_msg *msg)
{
    if (c->aborted) return;
    fetch_send_callback(msg, c->parent);
}

static void send_header(struct dos_fetch *c, const char *line, size_t len)
{
    fetch_msg msg;
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)line;
    msg.data.header_or_data.len = len;
    send_msg(c, &msg);
}

static void send_error(struct dos_fetch *c, const char *why)
{
    fetch_msg msg;
    msg.type = FETCH_ERROR;
    msg.data.error = why;
    send_msg(c, &msg);
}

/* Parse the status line + headers, hand them to NetSurf, then the body.
 * `resp` is the whole response as received. */
static void deliver(struct dos_fetch *c, char *resp, size_t len)
{
    /* Status line: "HTTP/1.x CODE reason". */
    if (len < 12 || strncmp(resp, "HTTP/", 5) != 0) {
        send_error(c, "Malformed HTTP response");
        return;
    }
    const char *sp = memchr(resp, ' ', len);
    int code = sp ? atoi(sp + 1) : 0;
    if (code == 0) { send_error(c, "Malformed HTTP status line"); return; }

    /* Header/body split.  Tolerate a bare LF separator as well as CRLF —
     * NetSurf never sees the raw stream, so being lenient here costs nothing
     * and a strict split silently yields an empty page. */
    char *body = NULL;
    size_t hdr_len = len;
    for (size_t i = 0; i + 1 < len; i++) {
        if (resp[i] == '\n' && resp[i + 1] == '\n') { hdr_len = i + 1; body = resp + i + 2; break; }
        if (i + 3 < len && resp[i] == '\r' && resp[i+1] == '\n' &&
            resp[i+2] == '\r' && resp[i+3] == '\n') {
            hdr_len = i + 2; body = resp + i + 4; break;
        }
    }
    size_t body_len = body ? len - (size_t)(body - resp) : 0;

    fprintf(stderr, "fetch_dos: HTTP %d, %u header bytes, %u body bytes\n",
            code, (unsigned)hdr_len, (unsigned)body_len);
    fetch_set_http_code(c->parent, (long)code);

    if (c->only_2xx && (code < 200 || code > 299)) {
        send_error(c, "Server returned a non-2xx status");
        return;
    }

    /* Feed each header line on its own.  NetSurf parses Content-Type from
     * these; without them it cannot decide what the body IS and renders
     * nothing however correct the bytes are. */
    const char *p = resp;
    const char *end = resp + hdr_len;
    /* skip the status line itself */
    while (p < end && *p != '\n') p++;
    if (p < end) p++;
    while (p < end && !c->aborted) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        while (llen && (p[llen - 1] == '\r')) llen--;
        if (llen) send_header(c, p, llen);
        if (!nl) break;
        p = nl + 1;
    }

    /* De-chunk in place if needed.  Without this the chunk sizes are handed to
     * the HTML parser as content — example.com arrived with a stray "22f" above
     * the heading and a "0" below it, which is the framing, not the page. */
    if (body_len && is_chunked(resp, hdr_len)) {
        char *out = body;
        const char *in = body, *bend = body + body_len;
        size_t total = 0;
        while (in < bend) {
            char *stop = NULL;
            unsigned long sz = strtoul(in, &stop, 16);
            if (stop == in) break;                       /* not a chunk header */
            while (stop < bend && *stop != '\n') stop++; /* skip any extension */
            if (stop >= bend) break;
            in = stop + 1;
            if (sz == 0) break;                          /* terminating chunk */
            if ((size_t)(bend - in) < sz) sz = (unsigned long)(bend - in);
            memmove(out + total, in, sz);
            total += sz;
            in += sz;
            while (in < bend && (*in == '\r' || *in == '\n')) in++;
        }
        body_len = total;
    }

    if (!c->aborted && body_len) {
        fetch_msg msg;
        msg.type = FETCH_DATA;
        msg.data.header_or_data.buf = (const uint8_t *)body;
        msg.data.header_or_data.len = body_len;
        send_msg(c, &msg);
    }
    if (!c->aborted) {
        fetch_msg msg;
        msg.type = FETCH_FINISHED;
        send_msg(c, &msg);
    }
}

/* Run one fetch to completion. */
static void do_fetch_work(struct dos_fetch *c);

/* Worker thread body.  Network only — no NetSurf call may be made from here. */
static void *fetch_worker(void *arg)
{
    struct dos_fetch *c = arg;
    do_fetch_work(c);
    c->done = true;                 /* published last: poll reads it as the gate */
    return NULL;
}

static void do_fetch_work(struct dos_fetch *c)
{
    struct urlparts u;
    const char *urlstr = nsurl_access(c->url);
    /* Progress on stderr, not NSLOG: NetSurf's logging is not routed to the
     * d-os console, and a fetcher that blocks silently is indistinguishable
     * from one that has hung. */
    fprintf(stderr, "fetch_dos: GET %s\n", urlstr ? urlstr : "(null)");
    if (!split_url(urlstr, &u)) { c->err = "Unsupported URL"; return; }
    fprintf(stderr, "fetch_dos: host=%s port=%d tls=%d path=%s\n",
            u.host, u.port, (int)u.tls, u.path);

#ifndef DOS_FETCH_TLS
    if (u.tls) {
        /* Explicit rather than obscure: Mbed TLS exists on d-os (`wget`,
         * `httpstest`), this build just was not linked against it. */
        c->err = "https is not available in this build";
        return;
    }
#endif

    /* On the HEAP, not the worker's stack: `struct conn` embeds mbedTLS's ssl
     * context, config, CA chain, entropy and DRBG state. */
    struct conn *cn = calloc(1, sizeof *cn);
    if (!cn) { c->err = "Out of memory"; return; }
    cn->fd = tcp_connect(u.host, u.port);
    fprintf(stderr, "fetch_dos: connect -> fd=%d\n", cn->fd);
    if (cn->fd < 0) { free(cn); c->err = "Could not connect to server"; return; }

#ifdef DOS_FETCH_TLS
    if (u.tls && tls_start(cn, u.host) != 0) {
        tls_end(cn); close(cn->fd); free(cn);
        c->err = "TLS handshake failed";
        return;
    }
#endif

    char req[2048];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: NetSurf/d-os\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"   /* we do not gunzip */
                     "Connection: close\r\n"
                     "\r\n",
                     u.path, u.host);
    if (n <= 0 || n >= (int)sizeof req) {
        conn_close(cn); free(cn); c->err = "Request too long"; return;
    }
    if (!send_all(cn, req, (size_t)n)) {
        conn_close(cn); free(cn); c->err = "Could not send request"; return;
    }

    size_t len = 0;
    char *resp = read_all(cn, &len);
    conn_close(cn);
    free(cn);
    fprintf(stderr, "fetch_dos: received %u bytes\n", (unsigned)len);

    if (!resp || len == 0) {
        free(resp);
        c->err = "Empty response from server";
        return;
    }
    c->resp = resp;                 /* poll delivers it; free releases it */
    c->resp_len = len;
}

/* ---------------------------------------------------------------------------
 * The fetcher_operation_table.
 * --------------------------------------------------------------------------- */
static bool dos_fetch_initialise(lwc_string *scheme) { (void)scheme; return true; }
static void dos_fetch_finalise(lwc_string *scheme)   { (void)scheme; }

static bool dos_fetch_can_fetch(const struct nsurl *url) { (void)url; return true; }

static void *dos_fetch_setup(struct fetch *parent, struct nsurl *url,
                             bool only_2xx, bool downgrade_tls,
                             const char *post_urlenc,
                             const struct fetch_multipart_data *post_multipart,
                             const char **headers)
{
    (void)downgrade_tls; (void)post_urlenc; (void)post_multipart; (void)headers;

    struct dos_fetch *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->parent   = parent;
    c->url      = nsurl_ref(url);
    c->only_2xx = only_2xx;
    return c;
}

static bool dos_fetch_start(void *v)
{
    struct dos_fetch *c = v;
    c->started = true;
    c->next = dos_ring;
    dos_ring = c;

    pthread_attr_t attr;
    int ok = -1;
    if (pthread_attr_init(&attr) == 0) {
        pthread_attr_setstacksize(&attr, DOS_FETCH_STACK);
        ok = pthread_create(&c->th, &attr, fetch_worker, c);
        pthread_attr_destroy(&attr);
    }
    if (ok != 0) {
        /* No thread: run inline rather than fail the fetch.  The UI stalls for
         * this one transfer, which beats a page that will not load. */
        fprintf(stderr, "fetch_dos: no worker thread, fetching inline\n");
        do_fetch_work(c);
        c->done = true;
        return true;
    }
    c->th_live = true;
    return true;
}

static void dos_fetch_abort(void *v)
{
    struct dos_fetch *c = v;
    c->aborted = true;
}

static void unlink_ctx(struct dos_fetch *c)
{
    struct dos_fetch **pp = &dos_ring;
    while (*pp) {
        if (*pp == c) { *pp = c->next; c->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

static void dos_fetch_free(void *v)
{
    struct dos_fetch *c = v;
    /* A running worker writes into this context, so it MUST be reaped before
     * the memory goes away — an abort during a slow transfer is exactly when
     * that happens.  Joining can block for the rest of that transfer;
     * detaching instead would trade a stall for a use-after-free. */
    if (c->th_live) { pthread_join(c->th, NULL); c->th_live = false; }
    unlink_ctx(c);
    if (c->url) nsurl_unref(c->url);
    free(c->resp);
    free(c);
}

static void dos_fetch_poll(lwc_string *scheme)
{
    (void)scheme;
    /* Deliver finished transfers.  A callback can start, abort or free fetches
     * — i.e. mutate the very list being walked — so the search restarts after
     * each delivery instead of continuing, and the list is only read here. */
    for (;;) {
        struct dos_fetch *c = NULL;
        for (struct dos_fetch *p = dos_ring; p; p = p->next)
            if (p->done && !p->delivered) { c = p; break; }
        if (!c) return;

        c->delivered = true;
        if (c->th_live) { pthread_join(c->th, NULL); c->th_live = false; }
        if (c->aborted) continue;

        if (c->err)       send_error(c, c->err);
        else if (c->resp) deliver(c, c->resp, c->resp_len);
        else              send_error(c, "Empty response from server");
        /* NetSurf calls ->free once it has seen FINISHED or ERROR; the context
         * must survive until then, so nothing is released here. */
    }
}

/* ---------------------------------------------------------------------------
 * Registration.  Named fetch_curl_register because that is the hook
 * content/fetch.c already calls under WITH_CURL — see the file header.
 * --------------------------------------------------------------------------- */
nserror fetch_curl_register(void);

nserror fetch_curl_register(void)
{
    static const struct fetcher_operation_table ops = {
        .initialise = dos_fetch_initialise,
        .acceptable = dos_fetch_can_fetch,
        .setup      = dos_fetch_setup,
        .start      = dos_fetch_start,
        .abort      = dos_fetch_abort,
        .free       = dos_fetch_free,
        .poll       = dos_fetch_poll,
        .finalise   = dos_fetch_finalise,
    };
    static const char *const schemes[] = { "http", "https" };
    nserror err = NSERROR_OK;

    for (unsigned i = 0; i < sizeof schemes / sizeof schemes[0]; i++) {
        lwc_string *s = NULL;
        if (lwc_intern_string(schemes[i], strlen(schemes[i]), &s) != lwc_error_ok)
            return NSERROR_NOMEM;
        err = fetcher_add(s, &ops);
        if (err != NSERROR_OK) return err;
    }
    return err;
}
