/* =============================================================================
 * ociunpack.c — an OCI image archive becomes a root filesystem (§M73).
 *
 *   ociunpack <archive.tar> <rootfs-dir> <config-out>
 *
 * The input is what `docker save` writes today: an OCI image LAYOUT packed in
 * a tar — index.json → (a nested multi-platform index) → one image manifest
 * per platform → a config blob and gzip-compressed tar layers, every blob
 * named by its SHA-256.
 *
 * WHY THIS IS A RING-3 PROGRAM.  Everything in the file is untrusted: sizes,
 * offsets, names, a decompressor's input.  Parsing that in ring 0 would put
 * an attack surface under the kernel for no benefit — the tree's standing rule
 * (§M42's image codecs, §M60's wallpaper decoder) is that formats are decoded
 * in ring 3, and only the RESULT (files, created through the VFS with the
 * caller's own rights) reaches the kernel.  So: every length is checked
 * against the buffer it indexes, every blob's SHA-256 is checked against the
 * name it was fetched by (which is what makes the store content-addressed
 * rather than merely hash-named), and every gzip member's CRC-32 and size are
 * checked against its trailer.
 *
 * WHAT A LAYER MAY HOLD, AND WHAT BECOMES OF IT:
 *   directory          → mkdir -p, with its mode
 *   regular file       → created, written, mode set
 *   hard link          → a hard link (the VFS has them since §M73 — busybox's
 *                        /bin is one binary and 400 links to it)
 *   symbolic link      → this VFS has none: a link to a regular file that is
 *                        already in the image becomes a HARD link to it, and
 *                        anything else is skipped and COUNTED — the summary
 *                        says how many, rather than a shell later saying "not
 *                        found" for a reason nobody can see
 *   whiteout .wh.NAME  → NAME is removed (a later layer deleting a file)
 *   device, fifo       → skipped and counted (a container has no /dev here)
 * PAX ('x') records carry long names and are honoured; global ('g') ones are
 * skipped.
 *
 * Built on the in-tree libc so it runs unchanged on all three arches; the
 * platform it picks is the one it was compiled for.
 * ============================================================================= */

#include "libc.h"

#if defined(__x86_64__)
#define MY_ARCH "amd64"
#elif defined(__i386__)
#define MY_ARCH "386"
#elif defined(__aarch64__)
#define MY_ARCH "arm64"
#else
#define MY_ARCH "unknown"
#endif

#define O_RD     0x01
#define O_WR     0x02
#define O_CREAT_ 0x04
#define O_TRUNC_ 0x08

static int k_mkdir(const char* p, int mode)        { return (int)dos_syscall3(48, (long)p, mode, 0); }
static int k_link (const char* o, const char* n)   { return (int)dos_syscall3(49, (long)o, (long)n, 0); }
static int k_chmod(const char* p, int mode)        { return (int)dos_syscall3(50, (long)p, mode, 0); }
static int k_unlink(const char* p)                 { return (int)dos_syscall3(51, (long)p, 0, 0); }
static int k_symlink(const char* t, const char* p) { return (int)dos_syscall3(52, (long)t, (long)p, 0); }

/* ---- small string helpers ------------------------------------------------- */
static int memcmp_(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i]) return 1;
    return 0;
}
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static void scat(char* d, const char* s, unsigned cap) {
    unsigned n = (unsigned)strlen(d);
    while (*s && n + 1 < cap) d[n++] = *s++;
    d[n] = 0;
}
static void die(const char* m) { printf("ociunpack: %s\n", m); exit(1); }

/* ---- read a whole file ---------------------------------------------------- */
static unsigned char* slurp(const char* path, unsigned* len) {
    struct stat st;
    if (stat(path, &st) != 0 || st.type != 0) return 0;
    unsigned char* b = (unsigned char*)malloc(st.size + 1);
    if (!b) return 0;
    int fd = open(path, O_RD);
    if (fd < 0) return 0;
    unsigned got = 0;
    while (got < st.size) {
        long r = read(fd, b + got, st.size - got);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    close(fd);
    if (got != st.size) return 0;
    b[got] = 0;
    *len = got;
    return b;
}

/* ---- tar ------------------------------------------------------------------- */
static unsigned long octal(const unsigned char* p, int n) {
    unsigned long v = 0;
    for (int i = 0; i < n && p[i]; i++) {
        if (p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        v = v * 8 + (unsigned long)(p[i] - '0');
    }
    return v;
}
/* Find member `name` in an uncompressed tar; its data and size. */
static const unsigned char* tar_find(const unsigned char* t, unsigned len, const char* name,
                                     unsigned* size) {
    unsigned off = 0;
    while (off + 512 <= len) {
        const unsigned char* h = t + off;
        if (!h[0]) break;
        unsigned long sz = octal(h + 124, 12);
        char nm[256];
        unsigned k = 0;
        if (h[345]) {                                     /* ustar prefix */
            for (int i = 0; i < 155 && h[345 + i] && k < 200; i++) nm[k++] = (char)h[345 + i];
            nm[k++] = '/';
        }
        for (int i = 0; i < 100 && h[i] && k < 250; i++) nm[k++] = (char)h[i];
        nm[k] = 0;
        if (sz > len - off - 512) return 0;               /* truncated archive */
        if (streq(nm, name)) { *size = (unsigned)sz; return h + 512; }
        off += 512 + (unsigned)((sz + 511) & ~511ul);
    }
    return 0;
}

/* ---- SHA-256 ---------------------------------------------------------------- */
static const unsigned K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(unsigned h[8], const unsigned char* p) {
    unsigned w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)p[4*i] << 24) | ((unsigned)p[4*i+1] << 16) | ((unsigned)p[4*i+2] << 8) | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        unsigned s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        unsigned s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
        unsigned t1 = hh + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        unsigned t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}
static void sha256_hex(const unsigned char* p, unsigned len, char out[65]) {
    unsigned h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    unsigned i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, p + i);
    unsigned char tail[128];
    unsigned r = len - i;
    memset(tail, 0, sizeof tail);
    memcpy(tail, p + i, r);
    tail[r] = 0x80;
    unsigned tl = (r < 56) ? 64 : 128;
    unsigned long long bits = (unsigned long long)len * 8u;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (unsigned char)(bits >> (8 * k));
    sha256_block(h, tail);
    if (tl == 128) sha256_block(h, tail + 64);
    static const char hx[] = "0123456789abcdef";
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 4; j++) {
            unsigned char byte = (unsigned char)(h[k] >> (24 - 8 * j));
            out[k*8 + j*2]     = hx[byte >> 4];
            out[k*8 + j*2 + 1] = hx[byte & 15];
        }
    out[64] = 0;
}

/* A blob by digest ("sha256:<hex>"), refused if its content does not hash to
 * its name. */
static const unsigned char* g_tar; static unsigned g_tarlen;
static const unsigned char* blob(const char* digest, unsigned* size) {
    if (!starts(digest, "sha256:")) die("a digest that is not sha256");
    char path[96] = "blobs/sha256/";
    scat(path, digest + 7, sizeof path);
    const unsigned char* b = tar_find(g_tar, g_tarlen, path, size);
    if (!b) { printf("ociunpack: blob %s is not in the archive\n", digest); exit(1); }
    char hex[65];
    sha256_hex(b, *size, hex);
    if (!streq(hex, digest + 7)) { printf("ociunpack: blob %s does NOT match its digest - refused\n", digest); exit(1); }
    return b;
}

/* ---- JSON, only as much as an OCI index/manifest/config needs --------------
 * Values are found by key inside a span; objects are delimited by brace depth
 * (strings skipped), so a nested "digest" in "config" and one in "layers" are
 * told apart by the span searched. */
static const char* skip_str(const char* p, const char* e) {    /* p at '"' */
    p++;
    while (p < e && *p != '"') { if (*p == '\\' && p + 1 < e) p++; p++; }
    return p < e ? p + 1 : e;
}
static const char* match_close(const char* p, const char* e) { /* p at '{' or '[' */
    int depth = 0;
    while (p < e) {
        if (*p == '"') { p = skip_str(p, e); continue; }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
        p++;
    }
    return e;
}
/* The value after "key": in [p,e) at the TOP level of that span. */
static const char* find_key(const char* p, const char* e, const char* key) {
    int depth = 0;
    unsigned kl = (unsigned)strlen(key);
    while (p < e) {
        if (*p == '"') {
            const char* s = p + 1;
            const char* q = skip_str(p, e);
            if (depth == 1 && (unsigned)(q - 1 - s) == kl && !memcmp_(s, key, kl)) {
                while (q < e && (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
                return q;
            }
            p = q; continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return 0;
}
static int str_val(const char* v, const char* e, char* out, unsigned cap) {
    if (!v || v >= e || *v != '"') return -1;
    unsigned n = 0;
    v++;
    while (v < e && *v != '"' && n + 1 < cap) { if (*v == '\\' && v + 1 < e) v++; out[n++] = *v++; }
    out[n] = 0;
    return 0;
}

/* ---- inflate (RFC 1951), canonical-Huffman, bit at a time ------------------ */
struct huff { short count[16]; short sym[320]; };
struct bits { const unsigned char* p; unsigned len, pos; unsigned bitbuf, bitcnt; };
static int getbit(struct bits* s) {
    if (!s->bitcnt) {
        if (s->pos >= s->len) return -1;
        s->bitbuf = s->p[s->pos++];
        s->bitcnt = 8;
    }
    int b = (int)(s->bitbuf & 1);
    s->bitbuf >>= 1; s->bitcnt--;
    return b;
}
static int getbits(struct bits* s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) { int b = getbit(s); if (b < 0) return -1; v |= b << i; }
    return v;
}
static int build(struct huff* h, const short* lens, int n) {
    short offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (short)i;
    return 0;
}
static int decode(struct bits* s, const struct huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = getbit(s);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
    }
    return -1;
}
static const short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static int codes(struct bits* s, unsigned char* out, unsigned cap, unsigned* op,
                 const struct huff* lc, const struct huff* dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (*op >= cap) return -1; out[(*op)++] = (unsigned char)sym; continue; }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        int e = getbits(s, LEXT[sym]); if (e < 0) return -1;
        unsigned len = (unsigned)(LBASE[sym] + e);
        int ds = decode(s, dc);
        if (ds < 0 || ds >= 30) return -1;
        e = getbits(s, DEXT[ds]); if (e < 0) return -1;
        unsigned dist = (unsigned)(DBASE[ds] + e);
        if (dist > *op || *op + len > cap) return -1;       /* bad back-reference */
        for (unsigned i = 0; i < len; i++, (*op)++) out[*op] = out[*op - dist];
    }
}
static int inflate(const unsigned char* in, unsigned inlen, unsigned char* out, unsigned cap,
                   unsigned* outlen) {
    struct bits s = { in, inlen, 0, 0, 0 };
    static struct huff lc, dc;
    unsigned op = 0;
    int last;
    do {
        last = getbit(&s);
        int type = getbits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {                                   /* stored */
            s.bitcnt = 0;
            if (s.pos + 4 > s.len) return -1;
            unsigned len = s.p[s.pos] | (s.p[s.pos + 1] << 8);
            unsigned nlen = s.p[s.pos + 2] | (s.p[s.pos + 3] << 8);
            s.pos += 4;
            if ((len ^ 0xFFFF) != nlen || s.pos + len > s.len || op + len > cap) return -1;
            memcpy(out + op, s.p + s.pos, len);
            s.pos += len; op += len;
        } else if (type == 1) {                            /* fixed */
            static short l[288], d[30];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            for (int i = 0; i < 30; i++) d[i] = 5;
            build(&lc, l, 288); build(&dc, d, 30);
            if (codes(&s, out, cap, &op, &lc, &dc) != 0) return -1;
        } else if (type == 2) {                            /* dynamic */
            static const short ORD[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            int nlen = getbits(&s, 5) + 257, ndist = getbits(&s, 5) + 1, ncode = getbits(&s, 4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            short lens[320];
            for (int i = 0; i < 19; i++) lens[i] = 0;
            for (int i = 0; i < ncode; i++) { int v = getbits(&s, 3); if (v < 0) return -1; lens[ORD[i]] = (short)v; }
            struct huff cl;
            build(&cl, lens, 19);
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode(&s, &cl);
                if (sym < 0) return -1;
                if (sym < 16) { lens[idx++] = (short)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (!idx) return -1; val = lens[idx - 1]; rep = 3 + getbits(&s, 2); }
                else if (sym == 17) rep = 3 + getbits(&s, 3);
                else rep = 11 + getbits(&s, 7);
                if (idx + rep > nlen + ndist) return -1;
                while (rep--) lens[idx++] = (short)val;
            }
            build(&lc, lens, nlen); build(&dc, lens + nlen, ndist);
            if (codes(&s, out, cap, &op, &lc, &dc) != 0) return -1;
        } else return -1;
    } while (!last);
    *outlen = op;
    return 0;
}
static unsigned crc32(const unsigned char* p, unsigned n) {
    unsigned c = 0xFFFFFFFFu;
    for (unsigned i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
/* A gzip member → its decompressed bytes (checked against its trailer). */
static unsigned char* gunzip(const unsigned char* g, unsigned glen, unsigned* outlen) {
    if (glen < 18 || g[0] != 0x1f || g[1] != 0x8b || g[2] != 8) return 0;
    unsigned flg = g[3], p = 10;
    if (flg & 4) { if (p + 2 > glen) return 0; p += 2 + (g[p] | (g[p + 1] << 8)); }
    if (flg & 8)  { while (p < glen && g[p]) p++; p++; }
    if (flg & 16) { while (p < glen && g[p]) p++; p++; }
    if (flg & 2) p += 2;
    if (p >= glen - 8) return 0;
    unsigned want_crc = g[glen-8] | (g[glen-7] << 8) | (g[glen-6] << 16) | ((unsigned)g[glen-5] << 24);
    unsigned isize    = g[glen-4] | (g[glen-3] << 8) | (g[glen-2] << 16) | ((unsigned)g[glen-1] << 24);
    /* Refuse a gzip BOMB by its RATIO, not its size (§M89).  The first rule
     * was an absolute 192 MB, which refused the one thing this tool exists
     * for once the thing was a JDK (a 158 MB layer that is 307 MB unpacked),
     * and bounded nothing about a bomb that stays under it.  A bomb is
     * characterised by an absurd ratio (1000:1 and up); real layers are 2-3:1.
     * Small outputs are always fine; above 64 MB the ratio must stay under
     * 20:1; 1 GiB is the ceiling whatever the ratio. */
    if (isize > (64u << 20) && (isize / 20u > glen || isize > (1024u << 20)))
        return 0;
    /* A mapping of its own, not the bump heap (which never frees): the caller
     * gives it back after extracting, so a JDK's five layers are not all
     * resident at once (§M89). */
    unsigned char* out = (unsigned char*)mmap(isize + 1, -1);
    if (!out || (long)out == -1) return 0;
    unsigned got = 0;
    if (inflate(g + p, glen - 8 - p, out, isize, &got) != 0 || got != isize) return 0;
    if (crc32(out, got) != want_crc) return 0;
    *outlen = got;
    return out;
}

/* ---- extraction ------------------------------------------------------------ */
static char g_root[128];
/* §M89 — extract only the entries under this prefix (no leading "/", no
 * trailing one), with the prefix REMOVED: "opt/java/openjdk/bin/java" lands
 * at <root>/bin/java.  Empty = the whole image.  How an application is
 * installed as software rather than run as a container: only its own tree
 * is wanted, straight onto the persistent disk. */
static char g_prefix[128];

/* `name` with a leading "./" or "/" dropped; with a prefix set, the rest
 * after it, "" for the prefix directory itself, NULL when outside it. */
static const char* in_prefix(const char* name) {
    while (name[0] == '.' && name[1] == '/') name += 2;
    while (name[0] == '/') name++;
    if (!g_prefix[0]) return name;
    unsigned n = 0;
    while (g_prefix[n]) { if (name[n] != g_prefix[n]) return 0; n++; }
    if (name[n] == 0) return "";
    if (name[n] == '/') return name + n + 1;
    return 0;
}
static unsigned n_files, n_dirs, n_links, n_symlinks, n_symlink_skipped, n_other, n_whiteout;

static void join(char* out, unsigned cap, const char* rel) {
    out[0] = 0;
    scat(out, g_root, cap);
    while (*rel == '.' && rel[1] == '/') rel += 2;     /* "./x" */
    while (*rel == '/') rel++;
    if (*rel) { scat(out, "/", cap); scat(out, rel, cap); }
    unsigned n = (unsigned)strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}
static void mkdir_p(const char* path) {                 /* every parent of path */
    char buf[256];
    unsigned n = (unsigned)strlen(path);
    if (n >= sizeof buf) return;
    memcpy(buf, path, n + 1);
    for (unsigned i = 1; i < n; i++)
        if (buf[i] == '/') { buf[i] = 0; k_mkdir(buf, 0755); buf[i] = '/'; }
}
static int write_file(const char* path, const unsigned char* d, unsigned n, int mode) {
    k_unlink(path);                                     /* a later layer replaces */
    int fd = open(path, O_WR | O_CREAT_ | O_TRUNC_);
    if (fd < 0) return -1;
    unsigned off = 0;
    while (off < n) {
        long w = write(fd, d + off, n - off > 65536 ? 65536 : n - off);
        if (w <= 0) { close(fd); return -1; }
        off += (unsigned)w;
    }
    close(fd);
    k_chmod(path, mode & 0777);
    return 0;
}
/* "../../bin/env" relative to the link's own directory, confined to the image. */
static void resolve_link(const char* linkpath_rel, const char* target, char* out, unsigned cap) {
    char rel[256];
    if (target[0] == '/') { rel[0] = 0; scat(rel, target + 1, sizeof rel); }
    else {
        rel[0] = 0; scat(rel, linkpath_rel, sizeof rel);
        unsigned n = (unsigned)strlen(rel);
        while (n && rel[n - 1] != '/') n--;
        rel[n] = 0;
        scat(rel, target, sizeof rel);
    }
    /* normalise . and .. — a ".." past the image root stays at the root */
    char norm[256]; unsigned nn = 0;
    const char* p = rel;
    while (*p) {
        const char* s = p; while (*p && *p != '/') p++;
        unsigned L = (unsigned)(p - s);
        if (*p) p++;
        if (!L || (L == 1 && s[0] == '.')) continue;
        if (L == 2 && s[0] == '.' && s[1] == '.') {
            while (nn && norm[nn - 1] != '/') nn--;
            if (nn) nn--;
            continue;
        }
        if (nn) norm[nn++] = '/';
        for (unsigned i = 0; i < L && nn + 1 < sizeof norm; i++) norm[nn++] = s[i];
    }
    norm[nn] = 0;
    join(out, cap, norm);
}
static void extract_layer(const unsigned char* t, unsigned len) {
    unsigned off = 0;
    char pax_path[256]; pax_path[0] = 0;
    while (off + 512 <= len) {
        const unsigned char* h = t + off;
        if (!h[0]) break;
        unsigned long sz = octal(h + 124, 12);
        if (sz > len - off - 512) die("a layer entry runs past the end of its layer");
        const unsigned char* data = h + 512;
        char type = (char)h[156];
        char name[256]; unsigned k = 0;
        if (pax_path[0]) { name[0] = 0; scat(name, pax_path, sizeof name); pax_path[0] = 0; }
        else {
            if (h[345]) { for (int i = 0; i < 155 && h[345+i] && k < 200; i++) name[k++] = (char)h[345+i]; name[k++] = '/'; }
            for (int i = 0; i < 100 && h[i] && k < 250; i++) name[k++] = (char)h[i];
            name[k] = 0;
        }
        int mode = (int)octal(h + 100, 8);
        char link[101]; for (int i = 0; i < 100; i++) link[i] = (char)h[157 + i]; link[100] = 0;
        off += 512 + (unsigned)((sz + 511) & ~511ul);

        if (type == 'x') {                                  /* PAX: path=... */
            unsigned q = 0;
            while (q < sz) {
                unsigned rl = 0, r0 = q;
                while (q < sz && data[q] >= '0' && data[q] <= '9') rl = rl * 10 + (data[q++] - '0');
                if (!rl || r0 + rl > sz) break;
                if (q + 6 < r0 + rl && !memcmp_((const char*)data + q + 1, "path=", 5)) {
                    unsigned n = 0;
                    for (unsigned i = q + 6; i < r0 + rl - 1 && n + 1 < sizeof pax_path; i++) pax_path[n++] = (char)data[i];
                    pax_path[n] = 0;
                }
                q = r0 + rl;
            }
            continue;
        }
        if (type == 'g') continue;

        /* §M89 — only the chosen subtree, with its prefix removed. */
        if (g_prefix[0]) {
            const char* r = in_prefix(name);
            if (!r) continue;
            char tmp[256]; tmp[0] = 0; scat(tmp, r, sizeof tmp);
            name[0] = 0; scat(name, tmp[0] ? tmp : ".", sizeof name);
            if (type == '1') {                         /* a hard link's target moves too */
                const char* lr = in_prefix(link);
                if (!lr) { n_other++; continue; }      /* points outside the subtree */
                char lt[101]; lt[0] = 0; scat(lt, lr, sizeof lt);
                link[0] = 0; scat(link, lt, sizeof link);
            } else if (type == '2' && link[0] == '/') {
                const char* lr = in_prefix(link);      /* an absolute link INTO the tree */
                if (lr) { char lt[101]; lt[0] = 0; scat(lt, g_root, sizeof lt); scat(lt, "/", sizeof lt);
                          scat(lt, lr, sizeof lt); link[0] = 0; scat(link, lt, sizeof link); }
            }
        }

        /* whiteouts */
        const char* base = name; for (const char* c = name; *c; c++) if (*c == '/' && c[1]) base = c + 1;
        if (starts(base, ".wh.")) {
            if (!streq(base, ".wh..wh..opq")) {
                char victim[256]; name[base - name] = 0;
                char rel[256]; rel[0] = 0; scat(rel, name, sizeof rel); scat(rel, base + 4, sizeof rel);
                join(victim, sizeof victim, rel);
                k_unlink(victim);
            }
            n_whiteout++;
            continue;
        }

        char path[256];
        join(path, sizeof path, name);
        if (streq(path, g_root)) continue;                  /* "./" itself */
        mkdir_p(path);
        if (type == '5') { k_mkdir(path, mode); k_chmod(path, mode & 0777); n_dirs++; }
        else if (type == '0' || type == 0) {
            if (write_file(path, data, (unsigned)sz, mode) != 0) printf("ociunpack: could not write %s\n", path);
            else n_files++;
        } else if (type == '1') {
            char tgt[256]; join(tgt, sizeof tgt, link);
            k_unlink(path);
            if (k_link(tgt, path) == 0) n_links++;
            else printf("ociunpack: could not link %s -> %s\n", path, tgt);
        } else if (type == '2') {
            k_unlink(path);
            /* §M89 — a REAL symbolic link, holding the image's own text: an
             * absolute target resolves against the container's "/" when used
             * from inside it, exactly as in the image.  Only a filesystem that
             * stores no links (exFAT) gets the old approximation: a hard link
             * when the target is an existing file, otherwise skipped. */
            if (k_symlink(link, path) == 0) { n_symlinks++; continue; }
            char tgt[256]; resolve_link(name, link, tgt, sizeof tgt);
            struct stat st;
            if (stat(tgt, &st) == 0 && st.type == 0 && k_link(tgt, path) == 0) n_symlinks++;
            else n_symlink_skipped++;
        } else n_other++;
    }
}

/* ---- the config: what to run ------------------------------------------------ */
static void write_config(const char* out, const char* cfg, unsigned n) {
    const char* e = cfg + n;
    const char* c = find_key(cfg, e, "config");
    if (!c) return;
    const char* ce = match_close(c, e);
    char text[2048]; text[0] = 0;
    const char* env = find_key(c, ce, "Env");
    if (env && *env == '[') {
        const char* ee = match_close(env, ce);
        for (const char* p = env + 1; p < ee; ) {
            if (*p == '"') {
                char v[512]; str_val(p, ee, v, sizeof v);
                scat(text, "env=", sizeof text); scat(text, v, sizeof text); scat(text, "\n", sizeof text);
                p = skip_str(p, ee);
            } else p++;
        }
    }
    const char* keys[2] = { "Entrypoint", "Cmd" };
    for (int k = 0; k < 2; k++) {
        const char* v = find_key(c, ce, keys[k]);
        if (!v || *v != '[') continue;
        const char* ve = match_close(v, ce);
        for (const char* p = v + 1; p < ve; ) {
            if (*p == '"') {
                char a[256]; str_val(p, ve, a, sizeof a);
                scat(text, k ? "cmd=" : "entry=", sizeof text); scat(text, a, sizeof text); scat(text, "\n", sizeof text);
                p = skip_str(p, ve);
            } else p++;
        }
    }
    write_file(out, (const unsigned char*)text, (unsigned)strlen(text), 0644);
}

int main(int argc, char** argv) {
    if (argc < 4) die("usage: ociunpack <archive.tar> <rootfs-dir|-> <config-out> [subtree]");
    g_tar = slurp(argv[1], &g_tarlen);
    if (!g_tar) die("cannot read the archive");
    int config_only = streq(argv[2], "-");            /* §M89 — only image.conf */
    g_root[0] = 0; scat(g_root, argv[2], sizeof g_root);
    if (argc > 4) {
        const char* pf = argv[4];
        while (*pf == '/') pf++;
        g_prefix[0] = 0; scat(g_prefix, pf, sizeof g_prefix);
        unsigned pl = (unsigned)strlen(g_prefix);
        while (pl && g_prefix[pl - 1] == '/') g_prefix[--pl] = 0;
    }
    if (!config_only) k_mkdir(g_root, 0755);

    unsigned n;
    const char* idx = (const char*)tar_find(g_tar, g_tarlen, "index.json", &n);
    if (!idx) die("no index.json - not an OCI image layout");
    char dg[80];
    const char* man = find_key(idx, idx + n, "manifests");
    if (!man) die("index.json has no manifests");
    const char* obj = man + 1;
    while (*obj && *obj != '{') obj++;
    if (str_val(find_key(obj, match_close(obj, idx + n), "digest"), idx + n, dg, sizeof dg) != 0) die("no digest in index.json");

    /* Walk indexes until an image manifest for this platform appears. */
    unsigned bl;
    const char* b = (const char*)blob(dg, &bl);
    for (int hop = 0; hop < 4; hop++) {
        const char* be = b + bl;
        const char* mf = find_key(b, be, "manifests");
        if (!mf) break;                                     /* an image manifest */
        const char* me = match_close(mf, be);
        int found = 0;
        for (const char* p = mf + 1; p < me; ) {
            while (p < me && *p != '{') p++;
            if (p >= me) break;
            const char* oe = match_close(p, me);
            const char* plat = find_key(p, oe, "platform");
            char arch[16] = "", os[16] = "";
            if (plat && *plat == '{') {
                const char* pe = match_close(plat, oe);
                str_val(find_key(plat, pe, "architecture"), pe, arch, sizeof arch);
                str_val(find_key(plat, pe, "os"), pe, os, sizeof os);
            }
            if (streq(arch, MY_ARCH) && streq(os, "linux")) {
                str_val(find_key(p, oe, "digest"), oe, dg, sizeof dg);
                found = 1; break;
            }
            p = oe;
        }
        if (!found) { printf("ociunpack: the image has no linux/%s variant\n", MY_ARCH); exit(1); }
        b = (const char*)blob(dg, &bl);
    }
    const char* be = b + bl;
    char cfgd[80];
    const char* cf = find_key(b, be, "config");
    if (!cf || str_val(find_key(cf, match_close(cf, be), "digest"), be, cfgd, sizeof cfgd) != 0) die("no config in the manifest");
    unsigned cl;
    const char* cfg = (const char*)blob(cfgd, &cl);
    const char* ly = find_key(b, be, "layers");
    if (!ly || *ly != '[') die("no layers in the manifest");
    const char* le = match_close(ly, be);
    int nl = 0;
    for (const char* p = ly + 1; !config_only && p < le; ) {
        while (p < le && *p != '{') p++;
        if (p >= le) break;
        const char* oe = match_close(p, le);
        char ld[80], mt[96] = "";
        str_val(find_key(p, oe, "digest"), oe, ld, sizeof ld);
        str_val(find_key(p, oe, "mediaType"), oe, mt, sizeof mt);
        unsigned zl, tl;
        const unsigned char* z = blob(ld, &zl);
        const unsigned char* t = z;
        tl = zl;
        if (z[0] == 0x1f && z[1] == 0x8b) {
            t = gunzip(z, zl, &tl);
            if (!t) { printf("ociunpack: layer %s does not decompress cleanly - refused\n", ld); exit(1); }
        }
        extract_layer(t, tl);
        if (t != z) munmap((void*)t, tl + 1);   /* one layer at a time, not all of them at once */
        nl++;
        p = oe;
    }
    write_config(argv[3], cfg, cl);
    printf("ociunpack: linux/%s, %d layer(s): %u files, %u directories, %u hard links, "
           "%u symlinks as links (%u skipped), %u whiteouts, %u other entries skipped - "
           "every blob matched its sha256\n", MY_ARCH, nl, n_files, n_dirs, n_links,
           n_symlinks, n_symlink_skipped, n_whiteout, n_other);
    return 0;
}
