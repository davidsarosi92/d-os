/* =============================================================================
 * pkg.c — content-addressed package store (§M35.5).  See pkg.h.
 *
 * Nix/Guix-shaped: each package materialises into an immutable, hash-named,
 * per-version directory under /store, with a pinned dependency closure.
 * Versions coexist (distinct hashes → distinct paths); a symlink-free text
 * "profile" (/etc/pkg/profile) is the installed view; mark-sweep GC reclaims
 * any store path not reachable from the profile's closure.
 * ============================================================================= */

#include "pkg.h"
#include "vfs.h"
#include "printf.h"
#include <stdint.h>
#include <stddef.h>

/* ----------------------- small string helpers ----------------------------- */

static int sappend(char* buf, int cap, int pos, const char* s) {
    while (s && *s && pos < cap - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}
static void hex8(uint32_t v, char* out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[7 - i] = hx[(v >> (i * 4)) & 0xF];
    out[8] = '\0';
}
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ----------------------- recipe registry ---------------------------------- */

static struct pkg_recipe* g_recipes = NULL;

void pkg_register(struct pkg_recipe* r) { r->next = g_recipes; g_recipes = r; }

static struct pkg_recipe* find_recipe(const char* id) {
    for (struct pkg_recipe* r = g_recipes; r; r = r->next)
        if (streq(r->id, id)) return r;
    return NULL;
}

/* Iterate the space-separated dep ids of `deps`, calling fn(id, ctx). */
typedef void (*dep_fn)(const char* id, void* ctx);
static void for_each_dep(const char* deps, dep_fn fn, void* ctx) {
    char tok[64];
    int t = 0;
    for (const char* c = deps ? deps : ""; ; c++) {
        if (*c == ' ' || *c == '\0') {
            if (t > 0) { tok[t] = '\0'; fn(tok, ctx); t = 0; }
            if (*c == '\0') break;
        } else if (t < (int)sizeof(tok) - 1) {
            tok[t++] = *c;
        }
    }
}

/* ----------------------- content-addressed hash --------------------------- */

static uint32_t fnv(const char* s, uint32_t h) {
    while (s && *s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static uint32_t recipe_hash(const struct pkg_recipe* r);   /* fwd */

struct hashctx { uint32_t h; };
static void fold_dep_hash(const char* id, void* ctx) {
    struct hashctx* hc = (struct hashctx*)ctx;
    struct pkg_recipe* d = find_recipe(id);
    if (d) { char hb[9]; hex8(recipe_hash(d), hb); hc->h = fnv(hb, hc->h); }
}
/* Folds id + version + each dep's recursive hash, so any change to a transitive
 * dependency yields a new store path (real content-addressing). */
static uint32_t recipe_hash(const struct pkg_recipe* r) {
    uint32_t h = 2166136261u;
    h = fnv(r->id, h);       h = fnv(":", h);
    h = fnv(r->version, h);  h = fnv(":", h);
    struct hashctx hc = { h };
    for_each_dep(r->deps, fold_dep_hash, &hc);
    return hc.h;
}

/* store dir NAME ("<hex8>-<name>-<version>") + full PATH ("/store/<name>"). */
static void store_dirname_r(const struct pkg_recipe* r, char* buf, int cap) {
    char hb[9]; hex8(recipe_hash(r), hb);
    int p = sappend(buf, cap, 0, hb);
    p = sappend(buf, cap, p, "-");
    p = sappend(buf, cap, p, r->name);
    p = sappend(buf, cap, p, "-");
    sappend(buf, cap, p, r->version);
}
static void store_path_r(const struct pkg_recipe* r, char* buf, int cap) {
    int p = sappend(buf, cap, 0, "/store/");
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    sappend(buf, cap, p, dn);
}

/* ----------------------- tiny file I/O ------------------------------------ */

static int write_file(const char* path, const void* data, unsigned len) {
    vfs_unlink(path);
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) return -1;
    if (len) vfs_write(f, data, len);
    vfs_close(f);
    return 0;
}
static int read_file(const char* path, char* buf, int cap) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    int n = (f->inode && f->inode->size < (uint64_t)(cap - 1))
              ? (int)f->inode->size : cap - 1;
    ssize_t r = vfs_read(f, buf, (size_t)n);
    vfs_close(f);
    if (r < 0) return -1;
    buf[r] = '\0';
    return (int)r;
}
static int path_exists(const char* path) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return 0;
    vfs_close(f);
    return 1;
}

/* ----------------------- build -------------------------------------------- */

/* Closure-text builder: appends each direct dep's store dirname + '\n'. */
struct cloctx { char* buf; int cap; int pos; };
static void append_dep_dirname(const char* id, void* ctx) {
    struct cloctx* c = (struct cloctx*)ctx;
    struct pkg_recipe* d = find_recipe(id);
    if (!d) return;
    char dn[96]; store_dirname_r(d, dn, sizeof dn);
    c->pos = sappend(c->buf, c->cap, c->pos, dn);
    c->pos = sappend(c->buf, c->cap, c->pos, "\n");
}

static void build_dep(const char* id, void* ctx) { (void)ctx; pkg_build(id); }

int pkg_build(const char* id) {
    struct pkg_recipe* r = find_recipe(id);
    if (!r) { kprintf("pkg: no recipe '%s'\n", id); return -1; }

    for_each_dep(r->deps, build_dep, NULL);           /* deps first */

    char path[160]; store_path_r(r, path, sizeof path);
    if (path_exists(path)) return 0;                  /* content-addressed reuse */

    vfs_mkdir("/store");
    if (vfs_mkdir(path) != 0) { kprintf("pkg: mkdir %s failed\n", path); return -1; }

    /* Payload → <path>/bin/<name>. */
    char binp[224];
    int p = sappend(binp, sizeof binp, 0, path);
    p = sappend(binp, sizeof binp, p, "/bin");
    vfs_mkdir(binp);
    p = sappend(binp, sizeof binp, p, "/");
    sappend(binp, sizeof binp, p, r->name);
    write_file(binp, r->content, r->content_len);

    /* .recipe (text). */
    char meta[512]; int m = 0;
    m = sappend(meta, sizeof meta, m, "name=");       m = sappend(meta, sizeof meta, m, r->name);
    m = sappend(meta, sizeof meta, m, "\nversion=");  m = sappend(meta, sizeof meta, m, r->version);
    m = sappend(meta, sizeof meta, m, "\ndeps=");     m = sappend(meta, sizeof meta, m, r->deps);
    m = sappend(meta, sizeof meta, m, "\n");
    char rp[192]; int rpp = sappend(rp, sizeof rp, 0, path);
    sappend(rp, sizeof rp, rpp, "/.recipe");
    write_file(rp, meta, (unsigned)m);

    /* .closure (direct dep store dirnames). */
    char clo[1024]; struct cloctx cc = { clo, sizeof clo, 0 }; clo[0] = '\0';
    for_each_dep(r->deps, append_dep_dirname, &cc);
    char cp[192]; int cpp = sappend(cp, sizeof cp, 0, path);
    sappend(cp, sizeof cp, cpp, "/.closure");
    write_file(cp, clo, (unsigned)cc.pos);

    kprintf("pkg: built /store/%s\n", path + 7);   /* skip "/store/" for brevity */
    return 0;
}

/* ----------------------- profile (/etc/pkg/profile) ----------------------- */

#define PROFILE_PATH  "/etc/pkg/profile"
#define PROFILE_MAX   4096

/* Does the profile contain `dirname` on its own line? */
static int profile_has(const char* dirname) {
    char buf[PROFILE_MAX];
    if (read_file(PROFILE_PATH, buf, sizeof buf) < 0) return 0;
    /* line-by-line compare */
    int i = 0;
    while (buf[i]) {
        int s = i;
        while (buf[i] && buf[i] != '\n') i++;
        int len = i - s;
        int dl = 0; while (dirname[dl]) dl++;
        if (len == dl) {
            int k = 0; while (k < len && buf[s + k] == dirname[k]) k++;
            if (k == len) return 1;
        }
        if (buf[i] == '\n') i++;
    }
    return 0;
}
static void profile_add(const char* dirname) {
    if (profile_has(dirname)) return;
    char buf[PROFILE_MAX];
    int n = read_file(PROFILE_PATH, buf, sizeof buf);
    if (n < 0) n = 0;
    int p = n;
    p = sappend(buf, sizeof buf, p, dirname);
    p = sappend(buf, sizeof buf, p, "\n");
    vfs_mkdir("/etc"); vfs_mkdir("/etc/pkg");
    write_file(PROFILE_PATH, buf, (unsigned)p);
}
static void profile_remove(const char* dirname) {
    char buf[PROFILE_MAX], out[PROFILE_MAX];
    if (read_file(PROFILE_PATH, buf, sizeof buf) < 0) return;
    int i = 0, op = 0, dl = 0; while (dirname[dl]) dl++;
    while (buf[i]) {
        int s = i;
        while (buf[i] && buf[i] != '\n') i++;
        int len = i - s;
        int keep = 1;
        if (len == dl) { int k = 0; while (k < len && buf[s+k] == dirname[k]) k++; if (k == len) keep = 0; }
        if (keep) { for (int k = 0; k < len && op < PROFILE_MAX-2; k++) out[op++] = buf[s+k]; out[op++] = '\n'; }
        if (buf[i] == '\n') i++;
    }
    out[op] = '\0';
    write_file(PROFILE_PATH, out, (unsigned)op);
}

/* ----------------------- install / remove --------------------------------- */

int pkg_install(const char* id) {
    struct pkg_recipe* r = find_recipe(id);
    if (!r) { kprintf("pkg: no recipe '%s'\n", id); return -1; }
    if (pkg_build(id) != 0) return -1;
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    profile_add(dn);
    kprintf("pkg: installed %s (%s)\n", r->name, dn);
    return 0;
}

int pkg_remove(const char* id) {
    struct pkg_recipe* r = find_recipe(id);
    if (!r) return -1;
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    profile_remove(dn);
    kprintf("pkg: removed %s from profile (run 'pkg gc' to reclaim)\n", r->name);
    return 0;
}

/* ----------------------- GC (mark-sweep) ---------------------------------- */

/* Reachable-set: profile lines + their transitive .closure. */
struct reachset { char names[64][96]; int n; };
static int rs_has(struct reachset* rs, const char* dn) {
    for (int i = 0; i < rs->n; i++) if (streq(rs->names[i], dn)) return 1;
    return 0;
}
static void rs_add_closure(struct reachset* rs, const char* dn) {
    if (rs_has(rs, dn) || rs->n >= 64) return;
    int idx = rs->n++;
    sappend(rs->names[idx], 96, 0, dn);
    /* read /store/<dn>/.closure and recurse */
    char cp[224]; int p = sappend(cp, sizeof cp, 0, "/store/");
    p = sappend(cp, sizeof cp, p, dn);
    sappend(cp, sizeof cp, p, "/.closure");
    char buf[1024];
    if (read_file(cp, buf, sizeof buf) < 0) return;
    int i = 0;
    while (buf[i]) {
        char line[96]; int l = 0;
        while (buf[i] && buf[i] != '\n' && l < 95) line[l++] = buf[i++];
        line[l] = '\0';
        if (buf[i] == '\n') i++;
        if (l > 0) rs_add_closure(rs, line);
    }
}

int pkg_gc(void) {
    /* 1. Build the reachable set from the profile. */
    struct reachset rs; rs.n = 0;
    char prof[PROFILE_MAX];
    if (read_file(PROFILE_PATH, prof, sizeof prof) >= 0) {
        int i = 0;
        while (prof[i]) {
            char line[96]; int l = 0;
            while (prof[i] && prof[i] != '\n' && l < 95) line[l++] = prof[i++];
            line[l] = '\0';
            if (prof[i] == '\n') i++;
            if (l > 0) rs_add_closure(&rs, line);
        }
    }

    /* 2. Sweep /store: delete any entry not in the reachable set. */
    struct file* d = vfs_open("/store", VFS_RDONLY);
    if (!d) return 0;
    /* Collect dead entries first (deleting during readdir is unsafe). */
    char dead[64][96]; int nd = 0;
    struct dirent de;
    while (nd < 64 && vfs_readdir(d, &de) > 0) {
        if (de.name[0] == '.' && (de.name[1] == '\0' ||
            (de.name[1] == '.' && de.name[2] == '\0'))) continue;
        if (!rs_has(&rs, de.name)) sappend(dead[nd++], 96, 0, de.name);
    }
    vfs_close(d);

    for (int i = 0; i < nd; i++) {
        char p[224]; int pp = sappend(p, sizeof p, 0, "/store/");
        sappend(p, sizeof p, pp, dead[i]);
        if (vfs_unlink_recursive(p) == 0)
            kprintf("pkg: gc reclaimed %s\n", dead[i]);
    }
    kprintf("pkg: gc — %d path(s) reclaimed, %d kept\n", nd, rs.n);
    return nd;
}

/* ----------------------- diagnostics -------------------------------------- */

void pkg_list(void) {
    struct file* d = vfs_open("/store", VFS_RDONLY);
    if (!d) { kprintf("pkg: store is empty\n"); return; }
    struct dirent de;
    kprintf("store paths (* = installed in profile):\n");
    while (vfs_readdir(d, &de) > 0) {
        if (de.name[0] == '.') continue;
        kprintf("  %s /store/%s\n", profile_has(de.name) ? "*" : " ", de.name);
    }
    vfs_close(d);
}

void pkg_why(const char* id) {
    struct pkg_recipe* r = find_recipe(id);
    if (!r) { kprintf("pkg: no recipe '%s'\n", id); return; }
    struct reachset rs; rs.n = 0;
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    rs_add_closure(&rs, dn);
    kprintf("closure of %s (%d path(s)):\n", r->name, rs.n);
    for (int i = 0; i < rs.n; i++) kprintf("  /store/%s\n", rs.names[i]);
}

/* ----------------------- built-in recipes + init -------------------------- */

/* Payload for the demo packages = the embedded user ELFs (weak — present on
 * the i386 build).  Two "hello" versions demonstrate coexistence; "args"
 * depends on hello-2 to demonstrate a pinned closure. */
extern const unsigned char _binary_user_hello_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_hello_i386_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_args_i386_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_args_i386_elf_end[]    __attribute__((weak));

static struct pkg_recipe rc_hello1, rc_hello2, rc_args;

void pkg_init(void) {
    vfs_mkdir("/store");
    vfs_mkdir("/etc"); vfs_mkdir("/etc/pkg");

    unsigned hlen = _binary_user_hello_i386_elf_start
        ? (unsigned)(_binary_user_hello_i386_elf_end - _binary_user_hello_i386_elf_start) : 0;
    unsigned alen = _binary_user_args_i386_elf_start
        ? (unsigned)(_binary_user_args_i386_elf_end - _binary_user_args_i386_elf_start) : 0;

    rc_hello1 = (struct pkg_recipe){ "hello-1", "hello", "1.0", "",
        _binary_user_hello_i386_elf_start, hlen, NULL };
    rc_hello2 = (struct pkg_recipe){ "hello-2", "hello", "2.0", "",
        _binary_user_hello_i386_elf_start, hlen, NULL };
    rc_args   = (struct pkg_recipe){ "args", "args", "1.0", "hello-2",
        _binary_user_args_i386_elf_start, alen, NULL };

    pkg_register(&rc_hello1);
    pkg_register(&rc_hello2);
    pkg_register(&rc_args);
}
