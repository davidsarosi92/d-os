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
#include "kmalloc.h"
#include "proc.h"
#include "task.h"
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
static unsigned strlen_local(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }

/* Cap for reading a package payload back out of the store (bin exec image or a
 * shared-library profile-expose).  Must fit the largest payload we copy this
 * way — the x86_64 musl libc.so (== the dynamic linker) is ~750 KiB, so 256 KiB
 * would silently TRUNCATE it into a broken ld.so.  2 MiB gives headroom. */
#define PKG_MAX_IMAGE  (2 * 1024 * 1024)

/* The canonical PT_INTERP path the arch's musl dynamic binaries carry (matches
 * the Makefile's $(DOS_LDSO) + the -dynamic-linker the .dynelf/.cxxelf rules
 * stamp in).  The active libc package's profile view writes its libc.so here. */
#if defined(__x86_64__)
#define DOS_LDSO_PATH "/lib/ld-musl-x86_64.so.1"
#else
#define DOS_LDSO_PATH "/lib/ld-musl-i386.so.1"
#endif

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

    /* Payload → <path>/bin/<name> for a program, or <path>/lib/<soname> for a
     * shared-library package (the runtime libc goes this route). */
    char binp[224];
    if (r->soname) {
        int p = sappend(binp, sizeof binp, 0, path);
        p = sappend(binp, sizeof binp, p, "/lib");
        vfs_mkdir(binp);
        p = sappend(binp, sizeof binp, p, "/");
        sappend(binp, sizeof binp, p, r->soname);
        write_file(binp, r->content, r->content_len);
    } else {
        int p = sappend(binp, sizeof binp, 0, path);
        p = sappend(binp, sizeof binp, p, "/bin");
        vfs_mkdir(binp);
        p = sappend(binp, sizeof binp, p, "/");
        sappend(binp, sizeof binp, p, r->name);
        write_file(binp, r->content, r->content_len);
    }

    /* .recipe (text). */
    char meta[512]; int m = 0;
    m = sappend(meta, sizeof meta, m, "name=");       m = sappend(meta, sizeof meta, m, r->name);
    m = sappend(meta, sizeof meta, m, "\nversion=");  m = sappend(meta, sizeof meta, m, r->version);
    m = sappend(meta, sizeof meta, m, "\ndeps=");     m = sappend(meta, sizeof meta, m, r->deps);
    m = sappend(meta, sizeof meta, m, "\n");
    char rp[192]; int rpp = sappend(rp, sizeof rp, 0, path);
    sappend(rp, sizeof rp, rpp, "/.recipe");
    write_file(rp, meta, (unsigned)m);

    /* .abi — the declared ABI/personality (defaults to "native").  The runner
     * (pkg_run) reads this to pick the exec personality, so the store path is
     * self-describing and the choice stays data-driven. */
    const char* abi = r->abi ? r->abi : "native";
    char ap[192]; int app = sappend(ap, sizeof ap, 0, path);
    sappend(ap, sizeof ap, app, "/.abi");
    write_file(ap, abi, (unsigned)strlen_local(abi));

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

/* Expose the installed binary by name under /bin (the "profile view") so it is
 * runnable via a PATH lookup (a musl `sh`'s execvp).  d-os ramfs has no
 * symlinks yet, so this copies the payload — the moral equivalent of Nix's
 * profile symlink; a symlink-based view is a follow-up. */
static void profile_bin_expose(struct pkg_recipe* r) {
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    char src[224]; int p = sappend(src, sizeof src, 0, "/store/");
    p = sappend(src, sizeof src, p, dn);
    p = sappend(src, sizeof src, p, "/bin/");
    sappend(src, sizeof src, p, r->name);

    char* buf = (char*)kmalloc(PKG_MAX_IMAGE);
    if (!buf) return;
    int n = read_file(src, buf, PKG_MAX_IMAGE);
    if (n > 0) {
        vfs_mkdir("/bin");
        char dst[96]; int dp = sappend(dst, sizeof dst, 0, "/bin/");
        sappend(dst, sizeof dst, dp, r->name);
        write_file(dst, buf, (unsigned)n);
    }
    kfree(buf);
}

/* Expose a shared-library package into /lib (the "profile view" for libraries,
 * analogous to profile_bin_expose for programs).  Copies <store>/lib/<soname>
 * → /lib/<soname>; for the C library it also writes the fixed PT_INTERP alias
 * (/lib/ld-musl-<arch>.so.1) so a dynamic binary's interpreter resolves.  ramfs
 * has no symlinks, so this copies — the moral equivalent of a Nix profile link;
 * "switching" libc versions = re-run this from a different store path. */
/* Stream-copy src → dst in fixed chunks (no whole-file buffer), so provisioning
 * scales to a 17 MiB libstdc++ without a giant contiguous kmalloc.  Returns
 * total bytes copied, or -1. */
static long copy_file(const char* src, const char* dst) {
    struct file* in = vfs_open(src, VFS_RDONLY);
    if (!in) return -1;
    vfs_unlink(dst);
    struct file* out = vfs_open(dst, VFS_WRONLY | VFS_CREATE);
    if (!out) { vfs_close(in); return -1; }
    enum { CHUNK = 65536 };
    char* buf = (char*)kmalloc(CHUNK);
    long total = 0; int ok = (buf != 0);
    while (ok) {
        ssize_t r = vfs_read(in, buf, CHUNK);
        if (r <= 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = vfs_write(out, buf + off, (size_t)(r - off));
            if (w <= 0) { ok = 0; break; }
            off += w;
        }
        total += off;
    }
    if (buf) kfree(buf);
    vfs_close(in); vfs_close(out);
    return ok ? total : -1;
}

static void profile_lib_expose(struct pkg_recipe* r) {
    if (!r->soname) return;
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    char src[224]; int p = sappend(src, sizeof src, 0, "/store/");
    p = sappend(src, sizeof src, p, dn);
    p = sappend(src, sizeof src, p, "/lib/");
    sappend(src, sizeof src, p, r->soname);

    vfs_mkdir("/lib");
    char dst[96]; int dp = sappend(dst, sizeof dst, 0, "/lib/");
    sappend(dst, sizeof dst, dp, r->soname);
    long n = copy_file(src, dst);                     /* streamed → scales big */
    if (n > 0) {
        if (r->is_libc) {
            copy_file(src, DOS_LDSO_PATH);            /* PT_INTERP alias */
            kprintf("libc: active = %s %s (%s) → /lib/%s + %s\n",
                    r->name, r->version, dn, r->soname, DOS_LDSO_PATH);
        }
    }
}

int pkg_install(const char* id) {
    struct pkg_recipe* r = find_recipe(id);
    if (!r) { kprintf("pkg: no recipe '%s'\n", id); return -1; }
    if (pkg_build(id) != 0) return -1;
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    profile_add(dn);
    if (r->soname) profile_lib_expose(r);        /* library → /lib/<soname> */
    else           profile_bin_expose(r);        /* program → runnable by name */
    kprintf("pkg: installed %s (%s)\n", r->name, dn);
    return 0;
}

/* Switch the active C library (see pkg.h): (re)build+install the named libc
 * package and re-point /lib at it.  With two musl versions registered + built,
 * this flips /lib between them — they coexist in the store untouched. */
int pkg_libc_use(const char* id) {
    struct pkg_recipe* r = find_recipe(id);
    if (!r || !r->soname || !r->is_libc) {
        kprintf("pkg: '%s' is not a C-library package\n", id ? id : "(null)");
        return -1;
    }
    if (pkg_build(id) != 0) return -1;
    char dn[96]; store_dirname_r(r, dn, sizeof dn);
    profile_add(dn);
    profile_lib_expose(r);
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

/* ----------------------- run (exec from the store) ------------------------ */

/* THE swappable seam: map a declared ABI name to a kernel personality.  Every
 * future backend (a BSD personality, a native musl-fork, …) is added HERE and
 * only here — call sites never hardcode "musl"/"linux".  Returns the value for
 * task->linux_abi (0 = d-os native ABI, 1 = Linux ABI). */
static int abi_to_personality(const char* abi) {
    if (abi && streq(abi, "linux")) return 1;
    return 0;                                   /* "native"/unknown → d-os ABI */
}

/* Find an INSTALLED recipe by program name (first match whose store dir is in
 * the profile).  Returns NULL if not installed. */
static struct pkg_recipe* find_installed_by_name(const char* name) {
    for (struct pkg_recipe* r = g_recipes; r; r = r->next) {
        if (!streq(r->name, name)) continue;
        char dn[96]; store_dirname_r(r, dn, sizeof dn);
        if (profile_has(dn)) return r;
    }
    return NULL;
}

int pkg_run(int argc, const char* const argv[]) {
    if (argc < 1 || !argv || !argv[0]) return -1;
    const char* name = argv[0];

    struct pkg_recipe* r = find_installed_by_name(name);
    if (!r) { kprintf("pkgrun: '%s' is not installed\n", name); return -1; }

    char dn[96]; store_dirname_r(r, dn, sizeof dn);

    /* <store>/<dn>/bin/<name> — the immutable payload we exec from the store. */
    char binp[224]; int bp = sappend(binp, sizeof binp, 0, "/store/");
    bp = sappend(binp, sizeof binp, bp, dn);
    bp = sappend(binp, sizeof binp, bp, "/bin/");
    sappend(binp, sizeof binp, bp, name);

    /* <store>/<dn>/.abi → personality (data-driven, self-describing store). */
    char abip[224]; int ap = sappend(abip, sizeof abip, 0, "/store/");
    ap = sappend(abip, sizeof abip, ap, dn);
    sappend(abip, sizeof abip, ap, "/.abi");
    char abi[32];
    if (read_file(abip, abi, sizeof abi) < 0) abi[0] = '\0';
    for (int i = 0; i < (int)sizeof abi && abi[i]; i++)   /* strip trailing newline */
        if (abi[i] == '\n') { abi[i] = '\0'; break; }
    if (!abi[0]) { abi[0] = 'n'; abi[1] = 'a'; abi[2] = 't'; abi[3] = 'i';
                   abi[4] = 'v'; abi[5] = 'e'; abi[6] = '\0'; }   /* default */
    int personality = abi_to_personality(abi);

    /* Show which ABI backend the seam selected — the "two brothers" made
     * visible: abi="linux" → the linux_abi translator; abi="native" → the
     * d-os native syscall path (linux_abi bypassed). */
    kprintf("pkgrun: %s  [abi=%s → %s backend]\n", name, abi,
            personality ? "linux-abi" : "d-os native");

    /* Read the ELF from the store into a kernel buffer. */
    char* img = (char*)kmalloc(PKG_MAX_IMAGE);
    if (!img) { kprintf("pkgrun: out of memory\n"); return -1; }
    int len = read_file(binp, img, PKG_MAX_IMAGE);
    if (len <= 0) { kprintf("pkgrun: cannot read %s\n", binp); kfree(img); return -1; }

    /* Run it as a synchronous excursion with the declared personality. */
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = personality;
    int rc = proc_exec_elf_argv(img, (size_t)len, argc, argv);
    if (me) me->linux_abi = prev;

    kfree(img);
    return rc;
}

/* ----------------------- built-in recipes + init -------------------------- */

/* Payload for the demo packages = the embedded user ELFs (weak — present on
 * the i386 build).  Two "hello" versions demonstrate coexistence; "args"
 * depends on hello-2 to demonstrate a pinned closure. */
extern const unsigned char _binary_user_hello_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_hello_i386_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_args_i386_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_args_i386_elf_end[]    __attribute__((weak));

/* musl-linked coreutils (Linux personality) — present only when musl was built
 * (`make musl`); weak so the kernel links without them otherwise. */
extern const unsigned char _binary_user_echo_muslelf_start[]   __attribute__((weak));
extern const unsigned char _binary_user_echo_muslelf_end[]     __attribute__((weak));
extern const unsigned char _binary_user_cat_muslelf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_cat_muslelf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_ls_muslelf_start[]     __attribute__((weak));
extern const unsigned char _binary_user_ls_muslelf_end[]       __attribute__((weak));
extern const unsigned char _binary_user_env_muslelf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_env_muslelf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_sh_muslelf_start[]     __attribute__((weak));
extern const unsigned char _binary_user_sh_muslelf_end[]       __attribute__((weak));

/* §M37: the musl dynamic linker (== the shared libc.so, embedded as a blob when
 * `make musl` built the shared library).  Weak so the kernel links without it. */
extern const unsigned char _binary_user_ldmusl_so_start[]      __attribute__((weak));
extern const unsigned char _binary_user_ldmusl_so_end[]        __attribute__((weak));

/* §M37 stage 5: a separate shared library, provisioned at /lib/libgreet.so so
 * ld.so can resolve a program's DT_NEEDED "libgreet.so" via the search path. */
extern const unsigned char _binary_user_libgreet_so_start[]    __attribute__((weak));
extern const unsigned char _binary_user_libgreet_so_end[]      __attribute__((weak));

/* §M38 support libs (toward NetSurf): zlib, cross-built + embedded, installed
 * as a versioned store package (soname libz.so.1). */
extern const unsigned char _binary_user_libz_so_1_start[]      __attribute__((weak));
extern const unsigned char _binary_user_libz_so_1_end[]        __attribute__((weak));
extern const unsigned char _binary_user_libpng16_so_16_start[] __attribute__((weak));
extern const unsigned char _binary_user_libpng16_so_16_end[]   __attribute__((weak));
extern const unsigned char _binary_user_libfreetype_so_6_start[] __attribute__((weak));
extern const unsigned char _binary_user_libfreetype_so_6_end[]   __attribute__((weak));
extern const unsigned char _binary_user_libharfbuzz_so_0_start[] __attribute__((weak));
extern const unsigned char _binary_user_libharfbuzz_so_0_end[]   __attribute__((weak));
extern const unsigned char _binary_user_libwapcaplet_so_0_start[] __attribute__((weak));
extern const unsigned char _binary_user_libwapcaplet_so_0_end[]   __attribute__((weak));
extern const unsigned char _binary_user_libparserutils_so_0_start[] __attribute__((weak));
extern const unsigned char _binary_user_libparserutils_so_0_end[]   __attribute__((weak));
extern const unsigned char _binary_user_libhubbub_so_0_start[]      __attribute__((weak));
extern const unsigned char _binary_user_libhubbub_so_0_end[]        __attribute__((weak));
extern const unsigned char _binary_user_libnsgif_so_0_start[]       __attribute__((weak));
extern const unsigned char _binary_user_libnsgif_so_0_end[]         __attribute__((weak));
extern const unsigned char _binary_user_libcss_so_0_start[]         __attribute__((weak));
extern const unsigned char _binary_user_libcss_so_0_end[]           __attribute__((weak));
extern const unsigned char _binary_user_libdom_so_0_start[]         __attribute__((weak));
extern const unsigned char _binary_user_libdom_so_0_end[]           __attribute__((weak));

/* §M38: the C++ runtime .so's (from the musl C++ toolchain) + the demo C++
 * library, provisioned into /lib so ld.so resolves a C++ program's DT_NEEDED
 * libstdc++.so.6 / libgcc_s.so.1 / libcpplib.so.  Weak — present only when the
 * toolchain was built. */
extern const unsigned char _binary_user_libstdcxx_so_start[]   __attribute__((weak));
extern const unsigned char _binary_user_libstdcxx_so_end[]     __attribute__((weak));
extern const unsigned char _binary_user_libgccs_so_start[]     __attribute__((weak));
extern const unsigned char _binary_user_libgccs_so_end[]       __attribute__((weak));
extern const unsigned char _binary_user_libcpplib_so_start[]   __attribute__((weak));
extern const unsigned char _binary_user_libcpplib_so_end[]     __attribute__((weak));

static struct pkg_recipe rc_hello1, rc_hello2, rc_args, rc_echo, rc_cat, rc_ls, rc_env, rc_sh;
static struct pkg_recipe rc_musl;
static struct pkg_recipe rc_zlib;
static struct pkg_recipe rc_libpng;
static struct pkg_recipe rc_freetype;
static struct pkg_recipe rc_harfbuzz;
static struct pkg_recipe rc_libgcc;
static struct pkg_recipe rc_wapcaplet;
static struct pkg_recipe rc_parserutils;
static struct pkg_recipe rc_hubbub;
static struct pkg_recipe rc_nsgif;
static struct pkg_recipe rc_libcss;
static struct pkg_recipe rc_libdom;

/* The version string of the embedded runtime musl — arch-specific (the i386
 * build fetches+builds musl 1.2.5; the x86_64 prebuilt musl.cc sysroot ships
 * 1.2.2).  It names the musl STORE PACKAGE, so a newer musl = a new recipe with
 * a new version = a new /store path that coexists with the old one. */
#if defined(__x86_64__)
#define DOS_MUSL_VERSION "1.2.2"
#else
#define DOS_MUSL_VERSION "1.2.5"
#endif

static unsigned blob_len(const unsigned char* s, const unsigned char* e) {
    return s ? (unsigned)(e - s) : 0;
}

/* Register one musl coreutil recipe if its blob is embedded (abi="linux"). */
static void register_coreutil(struct pkg_recipe* rc, const char* name,
                              const unsigned char* s, const unsigned char* e) {
    if (!s) return;
    *rc = (struct pkg_recipe){ .id = name, .name = name, .version = "1.0",
        .deps = "", .content = s, .content_len = blob_len(s, e), .abi = "linux" };
    pkg_register(rc);
}

/* §M43 — a flat rootfs archive (scripts/pack-rootfs.py) unpacked into the VFS
 * at boot: the on-device compiler's headers/crt/libs (too many files to embed
 * one-by-one).  Format: repeated [u32 pathlen|path|u32 datalen|data], 0-len
 * pathlen terminates.  Weak — present only when `make tcc` produced it. */
extern const unsigned char _binary_user_rootfs_bin_start[] __attribute__((weak));
extern const unsigned char _binary_user_rootfs_bin_end[]   __attribute__((weak));

static uint32_t rf_rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* mkdir -p every parent directory of `path`. */
static void mkdir_parents(const char* path) {
    char buf[256];
    int n = 0;
    for (int i = 0; path[i] && n < (int)sizeof buf - 1; i++) {
        buf[n++] = path[i];
        if (path[i] == '/' && n > 1) { buf[n - 1] = '\0'; vfs_mkdir(buf); buf[n - 1] = '/'; }
    }
}

static void rootfs_unpack(void) {
    const unsigned char* p   = _binary_user_rootfs_bin_start;
    const unsigned char* end = _binary_user_rootfs_bin_end;
    if (!p) return;
    while (p + 4 <= end) {
        uint32_t plen = rf_rd32(p); p += 4;
        if (plen == 0 || p + plen + 4 > end) break;
        char path[256];
        uint32_t pl = plen < sizeof path - 1 ? plen : sizeof path - 1;
        for (uint32_t i = 0; i < pl; i++) path[i] = (char)p[i];
        path[pl] = '\0';
        p += plen;
        uint32_t dlen = rf_rd32(p); p += 4;
        if (p + dlen > end) break;
        mkdir_parents(path);
        write_file(path, p, dlen);
        p += dlen;
    }
}

static void ldso_provision(void) {
    if (!_binary_user_ldmusl_so_start) return;      /* musl shared not built */
    unsigned len = blob_len(_binary_user_ldmusl_so_start, _binary_user_ldmusl_so_end);

    /* The runtime C library is a first-class STORE PACKAGE, not a hardcoded
     * global-/lib blob: register musl as a versioned, content-addressed recipe,
     * build+install it into /store, and let the profile view expose it to /lib
     * (libc.so + the PT_INTERP alias).  This makes musl swappable + updatable
     * via the package manager and lets multiple libc versions coexist — swap =
     * `pkg_libc_use("musl-<other>")`, update = `pkg install` a newer recipe.
     * (The interp path stays the fixed musl soname; the STORE is where the
     * version lives.)  See [[feedback-dos-swappable-layers]] + pkg_libc_use. */
    rc_musl = (struct pkg_recipe){ .id="musl", .name="musl", .version=DOS_MUSL_VERSION,
        .deps="", .content=_binary_user_ldmusl_so_start, .content_len=len,
        .abi="native", .soname="libc.so", .is_libc=1 };
    pkg_register(&rc_musl);
    vfs_mkdir("/lib");
    if (pkg_install("musl") != 0) {
        /* Store path failed — fall back to a direct /lib write so a dynamic
         * program can still run (degraded: no version management). */
        write_file(DOS_LDSO_PATH,  _binary_user_ldmusl_so_start, len);
        write_file("/lib/libc.so", _binary_user_ldmusl_so_start, len);
    }

    /* Aux runtime libraries (the DT_NEEDED search-path test lib + the C++
     * runtime) are still provisioned directly to /lib for now; folding them
     * into store packages the same way is a follow-up. */
    /* A separate shared library for the DT_NEEDED search-path test (stage 5). */
    if (_binary_user_libgreet_so_start)
        write_file("/lib/libgreet.so", _binary_user_libgreet_so_start,
                   blob_len(_binary_user_libgreet_so_start,
                            _binary_user_libgreet_so_end));

    /* §M38 — the C++ runtime.  libgcc_s.so.1 (~100 KiB) becomes a proper
     * versioned store package.  libstdc++.so.6 is ~17 MiB, though: making it a
     * store package would keep TWO copies in the ramfs (the immutable /store
     * copy + the /lib profile-view copy), and ramfs backs each file with one
     * contiguous, doubling-on-grow buffer — 2×~32 MiB overwhelms i386's memory
     * (it faulted).  Until the /lib view can POINT at the store path (ramfs
     * symlinks, or a store-aware /lib search — a follow-up) rather than copy,
     * libstdc++ stays a direct single /lib write (no duplication).  Same on
     * both arches for consistency.  (libcpplib is a test artifact — direct.) */
    if (_binary_user_libgccs_so_start) {
        rc_libgcc = (struct pkg_recipe){ .id="libgcc", .name="libgcc", .version="11.2.0",
            .deps="", .content=_binary_user_libgccs_so_start,
            .content_len=blob_len(_binary_user_libgccs_so_start, _binary_user_libgccs_so_end),
            .abi="native", .soname="libgcc_s.so.1", .is_libc=0 };
        pkg_register(&rc_libgcc);
        pkg_install("libgcc");
    }
    if (_binary_user_libstdcxx_so_start)
        write_file("/lib/libstdc++.so.6", _binary_user_libstdcxx_so_start,
                   blob_len(_binary_user_libstdcxx_so_start, _binary_user_libstdcxx_so_end));
    if (_binary_user_libcpplib_so_start)
        write_file("/lib/libcpplib.so", _binary_user_libcpplib_so_start,
                   blob_len(_binary_user_libcpplib_so_start, _binary_user_libcpplib_so_end));

    /* §M38 support libs — zlib as a proper versioned store package (same path
     * as musl: a `soname` lib recipe → /store → profile view → /lib/libz.so.1).
     * The first of the NetSurf support-lib closure; freetype/harfbuzz/… follow
     * the identical pattern. */
    if (_binary_user_libz_so_1_start) {
        rc_zlib = (struct pkg_recipe){ .id="zlib", .name="zlib", .version="1.3.1",
            .deps="", .content=_binary_user_libz_so_1_start,
            .content_len=blob_len(_binary_user_libz_so_1_start, _binary_user_libz_so_1_end),
            .abi="native", .soname="libz.so.1", .is_libc=0 };
        pkg_register(&rc_zlib);
        pkg_install("zlib");
    }

    /* libpng — depends on zlib; deps="zlib" makes the store closure pin it
     * (the recipe hash folds in zlib's hash → real content-addressing). */
    if (_binary_user_libpng16_so_16_start) {
        rc_libpng = (struct pkg_recipe){ .id="libpng", .name="libpng", .version="1.6.43",
            .deps="zlib", .content=_binary_user_libpng16_so_16_start,
            .content_len=blob_len(_binary_user_libpng16_so_16_start,
                                  _binary_user_libpng16_so_16_end),
            .abi="native", .soname="libpng16.so.16", .is_libc=0 };
        pkg_register(&rc_libpng);
        pkg_install("libpng");
    }

    /* freetype — the font rasteriser; depends on zlib too. */
    if (_binary_user_libfreetype_so_6_start) {
        rc_freetype = (struct pkg_recipe){ .id="freetype", .name="freetype",
            .version="2.13.2", .deps="zlib",
            .content=_binary_user_libfreetype_so_6_start,
            .content_len=blob_len(_binary_user_libfreetype_so_6_start,
                                  _binary_user_libfreetype_so_6_end),
            .abi="native", .soname="libfreetype.so.6", .is_libc=0 };
        pkg_register(&rc_freetype);
        pkg_install("freetype");
    }

    /* harfbuzz — text shaping (a C++ .so; its DT_NEEDED pulls libstdc++.so.6,
     * still direct-provisioned, so deps="" for the store closure today; the
     * logical harfbuzz→freetype+libstdc++ deps become real store deps once
     * hb-ft + a libstdc++ package land). */
    if (_binary_user_libharfbuzz_so_0_start) {
        rc_harfbuzz = (struct pkg_recipe){ .id="harfbuzz", .name="harfbuzz",
            .version="8.5.0", .deps="",
            .content=_binary_user_libharfbuzz_so_0_start,
            .content_len=blob_len(_binary_user_libharfbuzz_so_0_start,
                                  _binary_user_libharfbuzz_so_0_end),
            .abi="native", .soname="libharfbuzz.so.0", .is_libc=0 };
        pkg_register(&rc_harfbuzz);
        pkg_install("harfbuzz");
    }

    /* §M42 — NetSurf's own component libraries (store packages). */
    if (_binary_user_libwapcaplet_so_0_start) {
        rc_wapcaplet = (struct pkg_recipe){ .id="libwapcaplet", .name="libwapcaplet",
            .version="0.4.3", .deps="",
            .content=_binary_user_libwapcaplet_so_0_start,
            .content_len=blob_len(_binary_user_libwapcaplet_so_0_start,
                                  _binary_user_libwapcaplet_so_0_end),
            .abi="native", .soname="libwapcaplet.so.0", .is_libc=0 };
        pkg_register(&rc_wapcaplet);
        pkg_install("libwapcaplet");
    }
    if (_binary_user_libparserutils_so_0_start) {
        rc_parserutils = (struct pkg_recipe){ .id="libparserutils", .name="libparserutils",
            .version="0.2.5", .deps="",
            .content=_binary_user_libparserutils_so_0_start,
            .content_len=blob_len(_binary_user_libparserutils_so_0_start,
                                  _binary_user_libparserutils_so_0_end),
            .abi="native", .soname="libparserutils.so.0", .is_libc=0 };
        pkg_register(&rc_parserutils);
        pkg_install("libparserutils");
    }
    if (_binary_user_libhubbub_so_0_start) {
        rc_hubbub = (struct pkg_recipe){ .id="libhubbub", .name="libhubbub",
            .version="0.3.8", .deps="libparserutils",
            .content=_binary_user_libhubbub_so_0_start,
            .content_len=blob_len(_binary_user_libhubbub_so_0_start,
                                  _binary_user_libhubbub_so_0_end),
            .abi="native", .soname="libhubbub.so.0", .is_libc=0 };
        pkg_register(&rc_hubbub);
        pkg_install("libhubbub");
    }
    if (_binary_user_libnsgif_so_0_start) {
        rc_nsgif = (struct pkg_recipe){ .id="libnsgif", .name="libnsgif",
            .version="1.0.0", .deps="",
            .content=_binary_user_libnsgif_so_0_start,
            .content_len=blob_len(_binary_user_libnsgif_so_0_start,
                                  _binary_user_libnsgif_so_0_end),
            .abi="native", .soname="libnsgif.so.0", .is_libc=0 };
        pkg_register(&rc_nsgif);
        pkg_install("libnsgif");
    }
    if (_binary_user_libcss_so_0_start) {
        rc_libcss = (struct pkg_recipe){ .id="libcss", .name="libcss",
            .version="0.9.2", .deps="libwapcaplet libparserutils",
            .content=_binary_user_libcss_so_0_start,
            .content_len=blob_len(_binary_user_libcss_so_0_start,
                                  _binary_user_libcss_so_0_end),
            .abi="native", .soname="libcss.so.0", .is_libc=0 };
        pkg_register(&rc_libcss);
        pkg_install("libcss");
    }
    if (_binary_user_libdom_so_0_start) {
        rc_libdom = (struct pkg_recipe){ .id="libdom", .name="libdom",
            .version="0.4.2", .deps="libwapcaplet libhubbub libparserutils",
            .content=_binary_user_libdom_so_0_start,
            .content_len=blob_len(_binary_user_libdom_so_0_start,
                                  _binary_user_libdom_so_0_end),
            .abi="native", .soname="libdom.so.0", .is_libc=0 };
        pkg_register(&rc_libdom);
        pkg_install("libdom");
    }
}

/* ----------------------- pluggable backend (see pkg.h) -------------------- */

/* The content-addressed store IS the default backend: its ops just point at the
 * store_* implementations above (their public names, kept for direct/bootstrap
 * use).  A different pkg manager registers its own struct pkg_ops. */
static const struct pkg_ops store_ops = {
    .name = "store", .version = "1",
    .build = pkg_build, .install = pkg_install, .remove = pkg_remove,
    .run = pkg_run, .gc = pkg_gc, .list = pkg_list, .why = pkg_why,
    .libc_use = pkg_libc_use,
};
static const struct pkg_ops* g_pkg_backend = &store_ops;

void pkg_backend_register(const struct pkg_ops* ops) { if (ops) g_pkg_backend = ops; }
const struct pkg_ops* pkg_backend_active(void) { return g_pkg_backend; }

void pkg_init(void) {
    /* Idempotent: pkg_init may be reached from more than one path (the x86_64
     * boot self-test provisions early; shell_run also calls it lazily).  The
     * built-in recipes are file-scope statics linked into the registry, so a
     * second run would re-register the SAME structs and cycle the list.  Guard
     * against that. */
    static int inited = 0;
    if (inited) return;
    inited = 1;

    /* Announce the active package-manager backend (swappable component). */
    pkg_backend_register(&store_ops);
    kprintf("pkg: backend '%s' v%s active\n",
            g_pkg_backend->name, g_pkg_backend->version);

    vfs_mkdir("/store");
    vfs_mkdir("/etc"); vfs_mkdir("/etc/pkg");
    ldso_provision();
    rootfs_unpack();                 /* §M43 — tcc headers/crt/libs into the VFS */

    /* Native (d-os libc) demo packages. */
    rc_hello1 = (struct pkg_recipe){ .id="hello-1", .name="hello", .version="1.0",
        .deps="", .content=_binary_user_hello_i386_elf_start,
        .content_len=blob_len(_binary_user_hello_i386_elf_start, _binary_user_hello_i386_elf_end),
        .abi="native" };
    rc_hello2 = (struct pkg_recipe){ .id="hello-2", .name="hello", .version="2.0",
        .deps="", .content=_binary_user_hello_i386_elf_start,
        .content_len=blob_len(_binary_user_hello_i386_elf_start, _binary_user_hello_i386_elf_end),
        .abi="native" };
    rc_args   = (struct pkg_recipe){ .id="args", .name="args", .version="1.0",
        .deps="hello-2", .content=_binary_user_args_i386_elf_start,
        .content_len=blob_len(_binary_user_args_i386_elf_start, _binary_user_args_i386_elf_end),
        .abi="native" };
    pkg_register(&rc_hello1);
    pkg_register(&rc_hello2);
    pkg_register(&rc_args);

    /* musl-linked coreutils (Linux ABI) — registered only when embedded. */
    register_coreutil(&rc_echo, "echo", _binary_user_echo_muslelf_start, _binary_user_echo_muslelf_end);
    register_coreutil(&rc_cat,  "cat",  _binary_user_cat_muslelf_start,  _binary_user_cat_muslelf_end);
    register_coreutil(&rc_ls,   "ls",   _binary_user_ls_muslelf_start,   _binary_user_ls_muslelf_end);
    register_coreutil(&rc_env,  "env",  _binary_user_env_muslelf_start,  _binary_user_env_muslelf_end);
    register_coreutil(&rc_sh,   "sh",   _binary_user_sh_muslelf_start,   _binary_user_sh_muslelf_end);
}
