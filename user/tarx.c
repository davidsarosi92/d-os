/* =============================================================================
 * tarx.c — unpack a plain .tar / .tar.gz / .tgz release into a directory
 * (§M93: software as the vendor ships it — Docker's docker-<ver>.tgz, a JDK
 * tarball).
 *
 *   tarx <archive> <dest-dir> [--strip]
 *
 *   --strip   if every entry lives under ONE top-level directory ("docker/",
 *             "jdk-21.0.5+11/"), that directory is removed from the names, so
 *             <dest-dir> holds the release itself.  Decided on a first pass
 *             over the headers (no data is written by that pass).
 *
 * Prints, on its LAST line, `tarx: ok <files> <dirs> <links> <symlinks>
 * <symlinks-copied> <skipped> top=<dir>` or `tarx: FAIL <reason>`; exit 0/1.
 * On success the same facts are written to <dest-dir>/.tarx (key=value lines)
 * for the installer (apps.c), which has no way to read this program's output.
 *
 * WHY A STREAM: a release is large (Docker's tgz is 86 MB and 280 MB
 * unpacked, a JDK more), so nothing is held whole: the gzip stream is inflated
 * through a 32 KiB window and the tar is consumed 512-byte block by block, so
 * memory is constant whatever the archive's size.  §M73's ociunpack reads a
 * whole OCI archive into memory, which is right for an image of a few MB and
 * wrong here.
 *
 * WHY RING 3: everything in the file is untrusted (§M73's rule): sizes, names
 * and the compressed stream are parsed here, and only the RESULT reaches the
 * kernel, as files made through ordinary system calls with the caller's
 * rights.  Checked: every header's checksum, the gzip trailer's CRC-32 and
 * size, and every name — an absolute path or a ".." component is REFUSED
 * (a release that writes outside its directory is not unpacked at all).
 *
 * WHAT AN ENTRY BECOMES:
 *   regular file  created with its mode (the execute bits matter)
 *   directory     created with its mode
 *   hard link     a hard link to the earlier entry; where the filesystem has
 *                 none (exFAT), a COPY of it
 *   symlink       a symbolic link; where the filesystem has none (exFAT — the
 *                 persistent disk), a COPY of its target when that is a
 *                 regular file already unpacked, else skipped and counted
 *   anything else (device, fifo) skipped and counted
 * PAX ('x') records and GNU long names ('L', 'K') are honoured.
 * ============================================================================= */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static void fail(const char* m) { printf("tarx: FAIL %s\n", m); exit(1); }

/* ---- input: a buffered file -------------------------------------------------- */
static int g_in;
static unsigned char g_ib[65536];
static unsigned g_ipos, g_ilen;
static int g_ieof;
static int in_byte(void) {
    if (g_ipos == g_ilen) {
        if (g_ieof) return -1;
        ssize_t n = read(g_in, g_ib, sizeof g_ib);
        if (n <= 0) { g_ieof = 1; return -1; }
        g_ilen = (unsigned)n; g_ipos = 0;
    }
    return g_ib[g_ipos++];
}

/* ---- the tar consumer (fed bytes in order) ---------------------------------- */
static void tar_feed(const unsigned char* p, unsigned n);

/* ---- gzip + inflate, streaming ------------------------------------------- */
static unsigned g_crc_tab[256];
static void crc_init(void) {
    for (unsigned i = 0; i < 256; i++) {
        unsigned c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_tab[i] = c;
    }
}
static unsigned g_crc = 0xFFFFFFFFu, g_osize;
static unsigned char g_win[32768];
static unsigned g_wpos;
static unsigned char g_ob[16384];
static unsigned g_olen;
static void out_flush(void) { if (g_olen) { tar_feed(g_ob, g_olen); g_olen = 0; } }
static inline void out_byte(unsigned char c) {
    g_win[g_wpos++ & 32767] = c;
    g_crc = g_crc_tab[(g_crc ^ c) & 0xFF] ^ (g_crc >> 8);
    g_osize++;
    g_ob[g_olen++] = c;
    if (g_olen == sizeof g_ob) out_flush();
}

static unsigned g_bitbuf, g_bitcnt;
static int bits(int n) {
    while (g_bitcnt < (unsigned)n) {
        int b = in_byte();
        if (b < 0) fail("the compressed stream ends early");
        g_bitbuf |= (unsigned)b << g_bitcnt;
        g_bitcnt += 8;
    }
    int v = (int)(g_bitbuf & ((1u << n) - 1));
    g_bitbuf >>= n; g_bitcnt -= (unsigned)n;
    return v;
}
struct huff { short count[16], sym[320]; };
static int hbuild(struct huff* h, const short* len, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    short offs[16]; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (short)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return left;
}
static int hdecode(const struct huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(1);
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    fail("a bad Huffman code in the compressed stream");
    return -1;
}
static const short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static void codes(const struct huff* lc, const struct huff* dc) {
    for (;;) {
        int s = hdecode(lc);
        if (s < 256) { out_byte((unsigned char)s); continue; }
        if (s == 256) return;
        s -= 257;
        if (s >= 29) fail("a bad length code");
        int len = LBASE[s] + bits(LEXT[s]);
        int d = hdecode(dc);
        if (d >= 30) fail("a bad distance code");
        unsigned dist = (unsigned)(DBASE[d] + bits(DEXT[d]));
        if (dist > g_osize || dist > 32768) fail("a distance before the start of the stream");
        while (len--) out_byte(g_win[(g_wpos - dist) & 32767]);
    }
}
static void inflate_stream(void) {
    static struct huff lc, dc;
    int last;
    do {
        last = bits(1);
        int type = bits(2);
        if (type == 0) {
            g_bitbuf = 0; g_bitcnt = 0;                    /* to a byte boundary */
            int a = in_byte(), b = in_byte(), c = in_byte(), d = in_byte();
            if (d < 0) fail("a stored block ends early");
            unsigned len = (unsigned)a | ((unsigned)b << 8);
            if ((len ^ 0xFFFFu) != ((unsigned)c | ((unsigned)d << 8))) fail("a stored block's length is corrupt");
            while (len--) { int x = in_byte(); if (x < 0) fail("a stored block ends early"); out_byte((unsigned char)x); }
        } else if (type == 1) {
            static int fixed_ready;
            static struct huff fl, fd;
            if (!fixed_ready) {
                short l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                hbuild(&fl, l, 288);
                for (int i = 0; i < 30; i++) l[i] = 5;
                hbuild(&fd, l, 30);
                fixed_ready = 1;
            }
            codes(&fl, &fd);
        } else if (type == 2) {
            static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
            if (nlen > 286 || ndist > 30) fail("bad dynamic block counts");
            short len[320];
            for (int i = 0; i < 19; i++) len[order[i]] = 0;
            for (int i = 0; i < ncode; i++) len[order[i]] = (short)bits(3);
            struct huff cc;
            if (hbuild(&cc, len, 19) != 0) fail("an incomplete code-length code");
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = hdecode(&cc);
                if (sym < 16) { len[idx++] = (short)sym; continue; }
                int rep, val = 0;
                if (sym == 16) { if (!idx) fail("a repeat with nothing before it"); val = len[idx - 1]; rep = 3 + bits(2); }
                else if (sym == 17) rep = 3 + bits(3);
                else rep = 11 + bits(7);
                if (idx + rep > nlen + ndist) fail("too many code lengths");
                while (rep--) len[idx++] = (short)val;
            }
            if (hbuild(&lc, len, nlen) < 0) fail("an over-subscribed literal code");
            if (hbuild(&dc, len + nlen, ndist) < 0) fail("an over-subscribed distance code");
            codes(&lc, &dc);
        } else fail("an invalid block type");
    } while (!last);
}
static void gunzip_stream(void) {
    for (;;) {                                          /* gzip MEMBERS, one after another */
        int id1 = in_byte();
        if (id1 < 0) return;
        int id2 = in_byte(), cm = in_byte(), flg = in_byte();
        if (id1 != 0x1f || id2 != 0x8b || cm != 8) fail("not a gzip member");
        for (int i = 0; i < 6; i++) in_byte();          /* mtime, xfl, os */
        if (flg & 4) { int a = in_byte(), b = in_byte(); unsigned x = (unsigned)a | ((unsigned)b << 8); while (x--) in_byte(); }
        if (flg & 8)  { int c; while ((c = in_byte()) > 0) ; }
        if (flg & 16) { int c; while ((c = in_byte()) > 0) ; }
        if (flg & 2)  { in_byte(); in_byte(); }
        g_crc = 0xFFFFFFFFu; g_osize = 0; g_bitbuf = 0; g_bitcnt = 0;
        inflate_stream();
        out_flush();
        g_bitbuf = 0; g_bitcnt = 0;
        unsigned crc = 0, size = 0;
        for (int i = 0; i < 4; i++) crc  |= (unsigned)in_byte() << (8 * i);
        for (int i = 0; i < 4; i++) size |= (unsigned)in_byte() << (8 * i);
        if (crc != (g_crc ^ 0xFFFFFFFFu)) fail("the CRC-32 of the unpacked data does not match (corrupt download?)");
        if (size != g_osize) fail("the unpacked size does not match the gzip trailer");
    }
}

/* ---- tar ---------------------------------------------------------------------- */
static const char* g_dest;
static int g_strip, g_scan;                 /* g_scan: the first pass, names only */
static char g_top[256];                     /* the common top-level directory, or "" */
static int g_top_mixed;
static unsigned n_files, n_dirs, n_links, n_syms, n_symcopy, n_skip;

static unsigned char g_hdr[512];
static unsigned g_hfill;
static unsigned long long g_left;           /* data bytes of the current entry */
static unsigned g_pad;                      /* padding after it */
static int g_fd = -1;
static char g_name[1024], g_link[1024];
static char g_pax_path[1024], g_pax_link[1024], g_long_name[1024], g_long_link[1024];
static char* g_meta; static unsigned g_meta_len, g_meta_cap; static char g_meta_kind;
static int g_zero_blocks;

static unsigned long long octal(const unsigned char* p, int n) {
    if (p[0] & 0x80) {                      /* base-256 (GNU, for large sizes) */
        unsigned long long v = p[0] & 0x7F;
        for (int i = 1; i < n; i++) v = (v << 8) | p[i];
        return v;
    }
    unsigned long long v = 0;
    for (int i = 0; i < n && p[i]; i++) if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (unsigned)(p[i] - '0');
    return v;
}
static int safe_name(const char* n) {
    if (n[0] == '/') return 0;
    const char* p = n;
    while (*p) {
        const char* s = p;
        while (*p && *p != '/') p++;
        if (p - s == 2 && s[0] == '.' && s[1] == '.') return 0;
        if (*p) p++;
    }
    return 1;
}
/* "./a/b" -> "a/b"; with --strip, the common top directory goes too. */
static const char* rel_name(const char* n) {
    while (n[0] == '.' && n[1] == '/') n += 2;
    if (g_strip && g_top[0]) {
        size_t tl = strlen(g_top);
        if (strncmp(n, g_top, tl) == 0 && (n[tl] == '/' || n[tl] == 0)) {
            n += tl;
            while (*n == '/') n++;
        }
    }
    return n;
}
static void dest_path(char* out, size_t cap, const char* rel) {
    snprintf(out, cap, "%s/%s", g_dest, rel);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = 0;
}
static void mkdir_p(const char* path) {
    char b[1100];
    snprintf(b, sizeof b, "%s", path);
    for (char* p = b + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(b, 0755); *p = '/'; }
}
static int copy_file(const char* from, const char* to, int mode) {
    int a = open(from, O_RDONLY);
    if (a < 0) return -1;
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (b < 0) { close(a); return -1; }
    char buf[16384];
    ssize_t n;
    int ok = 0;
    while ((n = read(a, buf, sizeof buf)) > 0) if (write(b, buf, (size_t)n) != n) { ok = -1; break; }
    close(a); close(b);
    chmod(to, (mode_t)mode);
    return ok;
}
static void note_top(const char* n) {
    while (n[0] == '.' && n[1] == '/') n += 2;
    if (!n[0]) return;
    char t[256]; size_t i = 0;
    while (n[i] && n[i] != '/' && i < sizeof t - 1) { t[i] = n[i]; i++; }
    t[i] = 0;
    int is_dir_part = n[i] == '/';
    if (!is_dir_part) { g_top_mixed = 1; return; }     /* a file at the top level */
    if (!g_top[0]) snprintf(g_top, sizeof g_top, "%s", t);
    else if (strcmp(g_top, t) != 0) g_top_mixed = 1;
}

static void pax_parse(void) {
    /* "<len> key=value\n" records */
    unsigned i = 0;
    while (i < g_meta_len) {
        unsigned len = 0, j = i;
        while (j < g_meta_len && g_meta[j] >= '0' && g_meta[j] <= '9') len = len * 10 + (unsigned)(g_meta[j++] - '0');
        if (!len || i + len > g_meta_len) break;
        char* kv = g_meta + j + 1;
        char* end = g_meta + i + len - 1;                  /* the '\n' */
        char* eq = memchr(kv, '=', (size_t)(end - kv));
        if (eq) {
            size_t vl = (size_t)(end - eq - 1);
            if (vl >= sizeof g_pax_path) vl = sizeof g_pax_path - 1;
            if ((size_t)(eq - kv) == 4 && !strncmp(kv, "path", 4))     { memcpy(g_pax_path, eq + 1, vl); g_pax_path[vl] = 0; }
            if ((size_t)(eq - kv) == 8 && !strncmp(kv, "linkpath", 8)) { memcpy(g_pax_link, eq + 1, vl); g_pax_link[vl] = 0; }
        }
        i += len;
    }
}

static void entry_begin(void) {
    const unsigned char* h = g_hdr;
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != (unsigned)octal(h + 148, 8)) fail("a tar header's checksum is wrong (not a tar, or corrupt)");
    char type = (char)h[156];
    g_left = octal(h + 124, 12);
    g_pad = (unsigned)((512 - (g_left % 512)) % 512);
    /* the name: PAX / GNU long name, else prefix + name */
    if (g_pax_path[0]) snprintf(g_name, sizeof g_name, "%s", g_pax_path);
    else if (g_long_name[0]) snprintf(g_name, sizeof g_name, "%s", g_long_name);
    else if (!memcmp(h + 257, "ustar", 5) && h[345])
        snprintf(g_name, sizeof g_name, "%.155s/%.100s", (const char*)h + 345, (const char*)h + 0);
    else snprintf(g_name, sizeof g_name, "%.100s", (const char*)h);
    if (g_pax_link[0]) snprintf(g_link, sizeof g_link, "%s", g_pax_link);
    else if (g_long_link[0]) snprintf(g_link, sizeof g_link, "%s", g_long_link);
    else snprintf(g_link, sizeof g_link, "%.100s", (const char*)h + 157);
    if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
        g_meta_kind = type;
        g_meta_len = 0;
        if (g_left > (1u << 20)) fail("a metadata record over 1 MiB");
        if (g_left + 1 > g_meta_cap) { g_meta_cap = (unsigned)g_left + 1; g_meta = realloc(g_meta, g_meta_cap); }
        return;
    }
    g_meta_kind = 0;
    int mode = (int)(octal(h + 100, 8) & 07777);
    if (!safe_name(g_name)) { printf("tarx: FAIL the archive names '%s' - outside its directory\n", g_name); exit(1); }
    if (g_scan) { note_top(g_name); goto done; }
    const char* rel = rel_name(g_name);
    char path[1100];
    if (!rel[0]) goto done;                                /* the top directory itself */
    dest_path(path, sizeof path, rel);
    mkdir_p(path);
    if (type == '5') {
        if (mkdir(path, (mode_t)(mode ? mode : 0755)) != 0 && errno != EEXIST) n_skip++;
        else n_dirs++;
    } else if (type == '0' || type == 0 || type == '7') {
        g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
        if (g_fd < 0) { printf("tarx: FAIL cannot create %s (errno %d)\n", path, errno); exit(1); }
        if (mode) chmod(path, (mode_t)mode);
        n_files++;
    } else if (type == '1') {
        if (!safe_name(g_link)) fail("a hard link pointing outside the archive");
        char tgt[1100];
        dest_path(tgt, sizeof tgt, rel_name(g_link));
        if (link(tgt, path) == 0) n_links++;
        else if (copy_file(tgt, path, 0755) == 0) { struct stat st; if (!stat(tgt, &st)) chmod(path, st.st_mode & 07777); n_links++; }
        else n_skip++;
    } else if (type == '2') {
        unlink(path);
        if (symlink(g_link, path) == 0) n_syms++;
        else {
            /* no symlinks on this filesystem: copy a regular-file target that
             * is already unpacked (relative to the link's own directory) */
            char tgt[2200];
            if (g_link[0] == '/') { n_skip++; goto done; }
            snprintf(tgt, sizeof tgt, "%s", path);
            char* sl = strrchr(tgt, '/');
            if (sl) { sl[1] = 0; strncat(tgt, g_link, sizeof tgt - strlen(tgt) - 1); }
            struct stat st;
            if (stat(tgt, &st) == 0 && S_ISREG(st.st_mode) && copy_file(tgt, path, (int)(st.st_mode & 07777)) == 0) n_symcopy++;
            else n_skip++;
        }
    } else n_skip++;
done:
    g_pax_path[0] = g_pax_link[0] = g_long_name[0] = g_long_link[0] = 0;
}

static void tar_feed(const unsigned char* p, unsigned n) {
    while (n) {
        if (g_left) {
            unsigned k = g_left < n ? (unsigned)g_left : n;
            if (g_meta_kind) { memcpy(g_meta + g_meta_len, p, k); g_meta_len += k; }
            else if (g_fd >= 0 && write(g_fd, p, k) != (ssize_t)k) fail("a write failed (disk full?)");
            p += k; n -= k; g_left -= k;
            if (!g_left) {
                if (g_fd >= 0) { close(g_fd); g_fd = -1; }
                if (g_meta_kind == 'x') { pax_parse(); }
                else if (g_meta_kind == 'L' || g_meta_kind == 'K') {
                    g_meta[g_meta_len] = 0;
                    snprintf(g_meta_kind == 'L' ? g_long_name : g_long_link, sizeof g_long_name, "%s", g_meta);
                }
                g_meta_kind = 0;
            }
            continue;
        }
        if (g_pad) { unsigned k = g_pad < n ? g_pad : n; p += k; n -= k; g_pad -= k; continue; }
        unsigned k = 512 - g_hfill;
        if (k > n) k = n;
        memcpy(g_hdr + g_hfill, p, k);
        g_hfill += k; p += k; n -= k;
        if (g_hfill < 512) continue;
        g_hfill = 0;
        int zero = 1;
        for (int i = 0; i < 512; i++) if (g_hdr[i]) { zero = 0; break; }
        if (zero) { g_zero_blocks++; continue; }
        if (g_zero_blocks >= 2) continue;                      /* past the end marker */
        g_zero_blocks = 0;
        entry_begin();
    }
}

static int is_gzip(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char m[2] = {0, 0};
    ssize_t n = read(fd, m, 2);
    close(fd);
    return n == 2 && m[0] == 0x1f && m[1] == 0x8b;
}
static void pass(const char* path, int gz) {
    g_in = open(path, O_RDONLY);
    if (g_in < 0) fail("cannot open the archive");
    g_ipos = g_ilen = 0; g_ieof = 0;
    g_hfill = 0; g_left = 0; g_pad = 0; g_zero_blocks = 0; g_meta_kind = 0;
    g_pax_path[0] = g_pax_link[0] = g_long_name[0] = g_long_link[0] = 0;
    if (gz) gunzip_stream();
    else {
        int c;
        unsigned char b[4096]; unsigned k = 0;
        while ((c = in_byte()) >= 0) { b[k++] = (unsigned char)c; if (k == sizeof b) { tar_feed(b, k); k = 0; } }
        if (k) tar_feed(b, k);
    }
    close(g_in);
    if (g_left || g_hfill) fail("the archive ends in the middle of an entry");
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: tarx <archive> <dest-dir> [--strip]\n"); return 2; }
    g_dest = argv[2];
    g_strip = argc > 3 && !strcmp(argv[3], "--strip");
    crc_init();
    int gz = is_gzip(argv[1]);
    if (gz < 0) fail("cannot open the archive");
    mkdir(g_dest, 0755);
    if (g_strip) { g_scan = 1; pass(argv[1], gz); g_scan = 0; if (g_top_mixed) g_top[0] = 0; }
    pass(argv[1], gz);
    printf("tarx: ok %u %u %u %u %u %u top=%s\n", n_files, n_dirs, n_links, n_syms, n_symcopy, n_skip,
           g_top[0] ? g_top : "-");
    /* the same, for the installer to read: <dest>/.tarx */
    char rp[1100];
    snprintf(rp, sizeof rp, "%s/.tarx", g_dest);
    FILE* r = fopen(rp, "w");
    if (r) {
        fprintf(r, "top=%s\nfiles=%u\ndirs=%u\nlinks=%u\nsymlinks=%u\nsymlinks_copied=%u\nskipped=%u\n",
                g_top[0] ? g_top : "", n_files, n_dirs, n_links, n_syms, n_symcopy, n_skip);
        fclose(r);
    }
    return 0;
}
