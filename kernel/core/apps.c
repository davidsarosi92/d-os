/* =============================================================================
 * apps.c — installed applications from OCI images (§M89).
 *
 * Asked for in these words: "install it once from somewhere and it stays, as
 * installed software — listed among installed software, updated the same way,
 * reachable from /bin, in a non-OS folder".  So an application is:
 *
 *   /mnt/apps/<name>/<version>/     its own tree, on the PERSISTENT disk
 *   /mnt/apps/<name>/current        a one-line file naming the active version
 *                                   (a file, not a link: exFAT stores none)
 *   /bin/<exe> -> /mnt/apps/<name>/<version>/bin/<exe>
 *                                   symbolic links on ramfs, rebuilt at boot
 *
 * WHY THE LINKS AND NOT COPIES: a JDK finds its home through /proc/self/exe,
 * which reports the RESOLVED path — a copy in /bin would look for its
 * libraries under /bin/../lib.
 *
 * WHAT IS INSTALLED is the application's own subtree, not the image's
 * operating system: the image says where that is (<NAME>_HOME in its
 * environment, JAVA_HOME for a JDK), and the unpacker extracts only that,
 * prefix removed.  The one thing a musl program needs from outside its tree
 * is the C library under the name `libc.musl-<arch>.so.1`, which Alpine makes
 * a link to the dynamic linker; the same link is made here.
 *
 * UPDATES install BESIDE the old version and move `current`; the old one
 * stays until removed, so a rollback is `app use <name> <old version>`.
 *
 * DELIVERY, for now: `run_qemu.sh` copies images from the host's build/apps/
 * onto the disk's /incoming, and the boot service below installs anything
 * there that is not installed yet.  A download over TLS replaces that later.
 *
 * §M93 (2026-10-16) — SOFTWARE AS THE VENDOR SHIPS IT.  `app install <file>
 * [name]` also takes what one downloads from a vendor's site with `wget`:
 *
 *   a .tar.gz / .tgz / .tar RELEASE   Docker's docker-<ver>.tgz, a JDK
 *                                     tarball — unpacked by ring 3's tarx
 *                                     (streaming, constant memory), the
 *                                     release's one top directory removed
 *   a single EXECUTABLE               a plugin or a tool shipped as one file
 *                                     (docker-buildx)
 *   an OCI image archive              as before (§M89)
 *
 * BEFORE anything is linked, every program in the release is ASKED whether it
 * can run here: its ELF machine against this one (an x86_64 Docker on aarch64
 * is refused by name), and its loader — a static program needs none, a musl
 * one finds /lib/ld-musl-*, a glibc one needs the glibc package (its loader
 * /lib64/ld-linux-x86-64.so.2 or /lib/ld-linux-aarch64.so.1).  A release that
 * cannot start is not installed: the tree is removed and the reason said.
 *
 * NAME AND VERSION come from the file name (docker-29.9.1.tgz → docker
 * 29.9.1), else from the release's top directory (jdk-21.0.12+7 → 21.0.12+7),
 * else "unversioned"; the name may be given (`app install <file> java`).
 *
 * A MANIFEST says what a release needs beyond its programs on PATH: the
 * release's own `.dos-app`, else the one this system knows for that name
 * (Docker: its two daemons as services, /run, IPv4 forwarding; buildx: the
 * CLI plugin link).  Lines, applied at install and again at every boot:
 *   dir=<path>                 make a directory (ramfs is empty at boot)
 *   sysctl=<path> <value>      write a /proc/sys setting
 *   link=<file> <path>         a symbolic link to a file of the release
 *   program=<name>             (single file) the name it is installed as
 *   service=<command line>     start it in the background, in this order
 *   wait=<path> <seconds>      before the next line, wait for <path> to exist
 * ============================================================================= */

#include "vfs.h"
#include "task.h"
#include "printf.h"
#include "kmalloc.h"
#include "shellcmd.h"
#include "service.h"
#include <stdint.h>
#include <stddef.h>

#define APPS_ROOT  "/mnt/apps"
#define INCOMING   "/mnt/incoming"

#if defined(__x86_64__)
#define MUSL_ARCH "x86_64"
#elif defined(__aarch64__)
#define MUSL_ARCH "aarch64"
#else
#define MUSL_ARCH "i386"
#endif

int ctr_unpack(const char* archive, const char* root, const char* conf, const char* subtree);

/* The application being installed right now ("" = none), so the shell can say
 * "java is being installed" instead of "unknown command" — reported from use:
 * the first boot's install takes minutes, and a command typed meanwhile was
 * simply "unknown", which reads as "the install did not work". */
static char g_installing[32];
const char* apps_installing(void) { return g_installing; }

static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void scpy(char* d, const char* s, unsigned cap) {
    unsigned i = 0; for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0;
}
static void scat(char* d, const char* s, unsigned cap) {
    unsigned n = slen(d); for (unsigned i = 0; s && s[i] && n + 1 < cap; i++) d[n++] = s[i]; d[n] = 0;
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static int exists(const char* p) { struct vfs_stat st; return vfs_stat(p, &st) == 0; }

/* Read a whole small file into `out` (NUL-terminated, trailing newline cut). */
static int read_small(const char* path, char* out, unsigned cap) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    ssize_t n = vfs_read(f, out, cap - 1);
    vfs_close(f);
    if (n < 0) return -1;
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    return (int)n;
}
static int write_small(const char* path, const char* text) {
    vfs_unlink(path);
    if (vfs_create(path) != 0) return -1;
    struct file* f = vfs_open(path, VFS_WRONLY);
    if (!f) return -1;
    ssize_t n = vfs_write(f, text, slen(text));
    vfs_close(f);
    return n == (ssize_t)slen(text) ? 0 : -1;
}

/* `env=KEY=value` from the configuration the unpacker wrote. */
static int conf_env(const char* conf, const char* key, char* out, unsigned cap) {
    char* buf = (char*)kmalloc(4096);
    if (!buf) return -1;
    int n = read_small(conf, buf, 4096);
    int rc = -1;
    if (n > 0) {
        char want[80]; want[0] = 0;
        scat(want, "env=", sizeof want); scat(want, key, sizeof want); scat(want, "=", sizeof want);
        for (char* p = buf; *p; ) {
            char* e = p; while (*e && *e != '\n') e++;
            char save = *e; *e = 0;
            if (starts(p, want)) { scpy(out, p + slen(want), cap); rc = 0; }
            *e = save;
            if (!rc) break;
            p = *e ? e + 1 : e;
        }
    }
    kfree(buf);
    return rc;
}

static void mkdir_p(const char* path) {
    char b[256]; scpy(b, path, sizeof b);
    for (unsigned i = 1; b[i]; i++)
        if (b[i] == '/') { b[i] = 0; vfs_mkdir(b); b[i] = '/'; }
    vfs_mkdir(b);
}

/* The one library a musl program needs from outside its own tree. */
static void libc_link(void) {
    const char* ld  = "/lib/ld-musl-" MUSL_ARCH ".so.1";
    const char* lnk = "/lib/libc.musl-" MUSL_ARCH ".so.1";
    if (exists(ld) && !exists(lnk)) vfs_symlink(ld, lnk);
}

static void prog_dir(const char* tree, char* out, unsigned cap);   /* §M93, below */
/* /bin links for the active version of `name`.  An existing /bin entry is
 * replaced only when it is a link into /mnt/apps — a system program is never
 * shadowed by an application, whatever it is called. */
static int app_link(const char* name, int quiet) {
    char cur[64], bin[160];
    char p[160]; p[0] = 0; scat(p, APPS_ROOT "/", sizeof p); scat(p, name, sizeof p); scat(p, "/current", sizeof p);
    if (read_small(p, cur, sizeof cur) <= 0) return -1;
    bin[0] = 0; scat(bin, APPS_ROOT "/", sizeof bin); scat(bin, name, sizeof bin);
    scat(bin, "/", sizeof bin); scat(bin, cur, sizeof bin);
    {   /* §M93 — <version>/bin, or the release's top level (Docker's tgz) */
        char t[160]; scpy(t, bin, sizeof t);
        prog_dir(t, bin, sizeof bin);
    }
    struct file* d = vfs_open(bin, VFS_RDONLY);
    if (!d) return -1;
    /* /bin may not exist yet: this runs from a boot service, and the kernel
     * and the shell create /bin later.  Reported from use — the service said
     * "linked" while every symlink had failed for want of its directory, so
     * after a reboot `java` was unknown although `app list` showed it. */
    vfs_mkdir("/bin");
    vfs_mkdir("/lib");
    libc_link();
    struct dirent de;
    int made = 0;
    while (vfs_readdir(d, &de) > 0) {
        if (de.type == INODE_DIR) continue;
        char tgt[256], lnk[160];
        tgt[0] = 0; scat(tgt, bin, sizeof tgt); scat(tgt, "/", sizeof tgt); scat(tgt, de.name, sizeof tgt);
        lnk[0] = 0; scat(lnk, "/bin/", sizeof lnk); scat(lnk, de.name, sizeof lnk);
        struct vfs_stat st;
        if (vfs_lstat(lnk, &st) == 0) {
            char old[256];
            if (!st.is_link || vfs_readlink(lnk, old, sizeof old) < 0 || !starts(old, APPS_ROOT "/")) {
                if (!quiet) kprintf("app: not replacing %s - it is not an application's link\n", lnk);
                continue;
            }
            vfs_unlink(lnk);
        }
        if (vfs_symlink(tgt, lnk) == 0) made++;
    }
    vfs_close(d);
    return made;
}

/* Remove every /bin link that points into /mnt/apps/<name>/. */
static void app_unlink(const char* name) {
    char pre[96]; pre[0] = 0; scat(pre, APPS_ROOT "/", sizeof pre); scat(pre, name, sizeof pre); scat(pre, "/", sizeof pre);
    struct file* d = vfs_open("/bin", VFS_RDONLY);
    if (!d) return;
    char victims[64][32]; int nv = 0;
    struct dirent de;
    while (vfs_readdir(d, &de) > 0 && nv < 64) {
        if (de.type != INODE_SYMLINK) continue;
        char lnk[96], t[256]; lnk[0] = 0; scat(lnk, "/bin/", sizeof lnk); scat(lnk, de.name, sizeof lnk);
        if (vfs_readlink(lnk, t, sizeof t) >= 0 && starts(t, pre)) scpy(victims[nv++], de.name, 32);
    }
    vfs_close(d);
    for (int i = 0; i < nv; i++) {
        char lnk[96]; lnk[0] = 0; scat(lnk, "/bin/", sizeof lnk); scat(lnk, victims[i], sizeof lnk);
        vfs_unlink(lnk);
    }
}

/* A version string made safe as a directory name. */
static void sanitize(char* v) {
    for (char* p = v; *p; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '-' || c == '_' || c == '+';
        if (!ok) *p = '_';
    }
}

/* ===========================================================================
 * §M93 — releases (tarballs, single executables) and their manifests.
 * ========================================================================= */
#include "proc.h"
extern const unsigned char _binary_user_tarx_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_tarx_muslelf_end[]   __attribute__((weak));
int shell_run_from_path(const char* line);

#if defined(__x86_64__)
#define MY_EM 62
#define GLIBC_LD "/lib64/ld-linux-x86-64.so.2"
#elif defined(__aarch64__)
#define MY_EM 183
#define GLIBC_LD "/lib/ld-linux-aarch64.so.1"
#else
#define MY_EM 3
#define GLIBC_LD "/lib/ld-linux.so.2"
#endif
static const char* em_name(unsigned em) {
    return em == 62 ? "x86_64" : em == 183 ? "aarch64" : em == 3 ? "i386" : em == 40 ? "arm (32-bit)" :
           em == 243 ? "riscv64" : em == 21 ? "ppc64" : em == 22 ? "s390x" : "another machine";
}

/* Read `n` bytes at `off` of a file. */
static int read_at(const char* path, uint64_t off, void* buf, unsigned n) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    f->pos = off;
    ssize_t r = vfs_read(f, buf, n);
    vfs_close(f);
    return r == (ssize_t)n ? 0 : -1;
}

/* Can the program at `path` run on this machine?  0 = yes, 1 = not an ELF
 * (a script, data — not judged), <0 = no, with the reason in `why`. */
static int elf_can_run(const char* path, char* why, unsigned cap) {
    uint8_t h[64];
    if (read_at(path, 0, h, sizeof h) != 0) return 1;
    if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return 1;
    int is64 = h[4] == 2;
    unsigned em = (unsigned)h[18] | ((unsigned)h[19] << 8);
    if (em != MY_EM) {
        why[0] = 0;
        scat(why, "built for ", cap); scat(why, em_name(em), cap);
        scat(why, " - this machine is ", cap); scat(why, em_name(MY_EM), cap);
        return -1;
    }
    uint64_t phoff = is64 ? *(const uint64_t*)(h + 32) : *(const uint32_t*)(h + 28);
    unsigned phent = *(const uint16_t*)(h + (is64 ? 54 : 42));
    unsigned phnum = *(const uint16_t*)(h + (is64 ? 56 : 44));
    if (phnum > 64 || phent > 64 || phent < 32) return 0;
    uint8_t ph[64 * 64];
    if (read_at(path, phoff, ph, phent * phnum) != 0) return 0;
    for (unsigned k = 0; k < phnum; k++) {
        const uint8_t* e = ph + k * phent;
        if (*(const uint32_t*)e != 3) continue;                      /* PT_INTERP */
        uint64_t off = is64 ? *(const uint64_t*)(e + 8)  : *(const uint32_t*)(e + 4);
        uint64_t sz  = is64 ? *(const uint64_t*)(e + 32) : *(const uint32_t*)(e + 16);
        char interp[128];
        if (sz == 0 || sz >= sizeof interp || read_at(path, off, interp, (unsigned)sz) != 0) return 0;
        interp[sz] = 0;
        if (exists(interp)) return 0;
        why[0] = 0;
        int glibc = 0;
        for (const char* q = interp; *q; q++) if (starts(q, "ld-linux") || starts(q, "ld64.so")) glibc = 1;
        if (glibc) {
            scat(why, "linked against GLIBC (it asks for ", cap); scat(why, interp, cap);
            scat(why, ") - install the glibc package first (see the guide), or download "
                      "the musl (\"alpine\") build of this software", cap);
        } else {
            scat(why, "its loader ", cap); scat(why, interp, cap); scat(why, " is not on this machine", cap);
        }
        return -2;
    }
    return 0;
}

/* The directory a release keeps its programs in: <tree>/bin, else the top. */
static void prog_dir(const char* tree, char* out, unsigned cap) {
    out[0] = 0; scat(out, tree, cap); scat(out, "/bin", cap);
    struct vfs_stat st;
    if (vfs_stat(out, &st) != 0 || !st.is_dir) { out[0] = 0; scat(out, tree, cap); }
}

/* Every regular file of the program directory must be runnable here. */
static int release_check(const char* tree) {
    char dir[192];
    prog_dir(tree, dir, sizeof dir);
    struct file* d = vfs_open(dir, VFS_RDONLY);
    if (!d) return 0;
    struct dirent de;
    int bad = 0, progs = 0;
    while (vfs_readdir(d, &de) > 0) {
        if (de.type != INODE_FILE) continue;
        char p[256]; p[0] = 0; scat(p, dir, sizeof p); scat(p, "/", sizeof p); scat(p, de.name, sizeof p);
        char why[200];
        int r = elf_can_run(p, why, sizeof why);
        if (r == 0) progs++;
        if (r < 0) {
            kprintf("app: %s cannot run here: %s\n", de.name, why);
            bad++;
            if (bad >= 3) break;                    /* one reason is enough; say three */
        }
    }
    vfs_close(d);
    if (bad) return -1;
    return progs;
}

/* gzip, an OCI image tar, a plain tar, or a single ELF? */
enum { AK_NONE, AK_OCI, AK_TARBALL, AK_EXEC };
static int archive_kind(const char* path) {
    uint8_t h[512];
    if (read_at(path, 0, h, 4) != 0) return AK_NONE;
    if (h[0] == 0x1f && h[1] == 0x8b) return AK_TARBALL;
    if (h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return AK_EXEC;
    /* an uncompressed tar: an OCI layout names index.json / oci-layout /
     * manifest.json among its first entries (docker save) */
    uint64_t off = 0;
    for (int i = 0; i < 64; i++) {
        if (read_at(path, off, h, 512) != 0) break;
        if (!h[0]) break;
        if (h[257] != 'u' || h[258] != 's') return AK_NONE;  /* not ustar */
        char nm[101]; for (int k = 0; k < 100; k++) nm[k] = (char)h[k]; nm[100] = 0;
        if (seq(nm, "index.json") || seq(nm, "oci-layout") || seq(nm, "manifest.json") ||
            seq(nm, "./index.json") || seq(nm, "./oci-layout") || seq(nm, "./manifest.json")) return AK_OCI;
        uint64_t sz = 0;
        for (int k = 124; k < 136 && h[k]; k++) if (h[k] >= '0' && h[k] <= '7') sz = sz * 8 + (uint64_t)(h[k] - '0');
        off += 512 + ((sz + 511) / 512) * 512;
    }
    return AK_TARBALL;
}

/* docker-29.9.1.tgz -> name "docker", version "29.9.1"; the version split is
 * at the first "-<digit>" or "-v<digit>". */
static void name_ver_from_file(const char* path, char* name, unsigned ncap, char* ver, unsigned vcap) {
    const char* b = path;
    for (const char* q = path; *q; q++) if (*q == '/') b = q + 1;
    char base[96]; scpy(base, b, sizeof base);
    unsigned n = slen(base);
    const char* ext[4] = { ".tar.gz", ".tgz", ".tar", ".gz" };
    for (int i = 0; i < 4; i++) {
        unsigned el = slen(ext[i]);
        if (n > el && seq(base + n - el, ext[i])) { base[n - el] = 0; break; }
    }
    name[0] = ver[0] = 0;
    for (unsigned i = 0; base[i]; i++) {
        if (base[i] == '-' && ((base[i + 1] >= '0' && base[i + 1] <= '9') ||
                               (base[i + 1] == 'v' && base[i + 2] >= '0' && base[i + 2] <= '9'))) {
            base[i] = 0;
            scpy(name, base, ncap);
            scpy(ver, base + i + 1 + (base[i + 1] == 'v' ? 1 : 0), vcap);
            return;
        }
    }
    scpy(name, base, ncap);
}

/* The manifests this system knows, for releases that carry none. */
static const char* const known_manifests[][2] = {
    { "docker",
      "# Docker's static release: the CLI, dockerd, containerd and runc.\n"
      "dir=/run\n"
      "dir=/var/lib/docker\n"
      "sysctl=/proc/sys/net/ipv4/ip_forward 1\n"
      "service=containerd\n"
      "wait=/run/containerd/containerd.sock 180\n"
      "service=dockerd --containerd=/run/containerd/containerd.sock --storage-driver=overlay2 "
      "--firewall-backend=nftables --userland-proxy=false --ip6tables=false\n" },
    { "buildx",
      "# docker-buildx: one executable, the Docker CLI's build plugin.\n"
      "program=docker-buildx\n"
      "dir=/usr/local/lib/docker/cli-plugins\n"
      "link=bin/docker-buildx /usr/local/lib/docker/cli-plugins/docker-buildx\n" },
};
static const char* known_manifest(const char* name) {
    for (unsigned i = 0; i < sizeof known_manifests / sizeof known_manifests[0]; i++)
        if (seq(name, known_manifests[i][0])) return known_manifests[i][1];
    return NULL;
}
static void manifest_value(const char* m, const char* key, char* out, unsigned cap) {
    out[0] = 0;
    unsigned kl = slen(key);
    for (const char* p = m; *p; ) {
        const char* e = p; while (*e && *e != '\n') e++;
        if ((unsigned)(e - p) > kl && starts(p, key) && p[kl] == '=') {
            unsigned i = 0;
            for (const char* q = p + kl + 1; q < e && i + 1 < cap; q++) out[i++] = *q;
            out[i] = 0;
            return;
        }
        p = *e ? e + 1 : e;
    }
}

/* Apply a manifest: directories, settings, links, then the services in order.
 * `tree` is the installed version's directory (link targets are inside it). */
static void manifest_apply(const char* name, const char* tree, const char* m) {
    for (const char* p = m; *p; ) {
        const char* e = p; while (*e && *e != '\n') e++;
        char line[400]; unsigned n = 0;
        for (const char* q = p; q < e && n + 1 < sizeof line; q++) line[n++] = *q;
        line[n] = 0;
        p = *e ? e + 1 : e;
        if (!line[0] || line[0] == '#') continue;
        if (starts(line, "dir=")) { mkdir_p(line + 4); continue; }
        if (starts(line, "sysctl=") || starts(line, "link=") || starts(line, "wait=")) {
            char* a = line + (line[0] == 's' ? 7 : line[0] == 'l' ? 5 : 5);
            char* b = a; while (*b && *b != ' ') b++;
            if (!*b) continue;
            *b++ = 0;
            if (line[0] == 's') {
                struct file* f = vfs_open(a, VFS_WRONLY);
                if (f) { vfs_write(f, b, slen(b)); vfs_close(f); }
                else kprintf("app: %s: cannot set %s\n", name, a);
            } else if (line[0] == 'l') {
                char tgt[256]; tgt[0] = 0; scat(tgt, tree, sizeof tgt); scat(tgt, "/", sizeof tgt); scat(tgt, a, sizeof tgt);
                vfs_unlink(b);
                if (vfs_symlink(tgt, b) != 0) kprintf("app: %s: cannot link %s\n", name, b);
            } else {
                unsigned secs = 0;
                for (const char* q = b; *q >= '0' && *q <= '9'; q++) secs = secs * 10 + (unsigned)(*q - '0');
                unsigned i = 0;
                for (; i < secs * 10 && !exists(a); i++) task_msleep(100);
                if (!exists(a)) kprintf("app: %s: %s did not appear within %u s\n", name, a, secs);
            }
            continue;
        }
        if (starts(line, "service=")) {
            char cmd[420]; cmd[0] = 0; scat(cmd, line + 8, sizeof cmd); scat(cmd, " &", sizeof cmd);
            kprintf("app: %s: starting %s\n", name, line + 8);
            shell_run_from_path(cmd);
            continue;
        }
    }
}

/* The installed version's manifest (its .dos-app), applied — at boot and
 * right after an install. */
static void app_start(const char* name, int quiet) {
    char cur[64], tree[192], mp[224];
    char p[160]; p[0] = 0; scat(p, APPS_ROOT "/", sizeof p); scat(p, name, sizeof p); scat(p, "/current", sizeof p);
    if (read_small(p, cur, sizeof cur) <= 0) return;
    tree[0] = 0; scat(tree, APPS_ROOT "/", sizeof tree); scat(tree, name, sizeof tree);
    scat(tree, "/", sizeof tree); scat(tree, cur, sizeof tree);
    mp[0] = 0; scat(mp, tree, sizeof mp); scat(mp, "/.dos-app", sizeof mp);
    char* m = (char*)kmalloc(4096);
    if (!m) return;
    if (read_small(mp, m, 4096) > 0) {
        if (!quiet) kprintf("app: %s: applying its manifest\n", name);
        manifest_apply(name, tree, m);
    }
    kfree(m);
}

static int run_tarx(const char* archive, const char* dest) {
    if (!_binary_user_tarx_muslelf_start) { kprintf("app: no tarball unpacker in this build\n"); return -1; }
    const char* argv[4] = { "tarx", archive, dest, "--strip" };
    int pid = proc_spawn_argv("tarx", _binary_user_tarx_muslelf_start,
                              (size_t)(_binary_user_tarx_muslelf_end - _binary_user_tarx_muslelf_start),
                              4, argv, 1);
    if (pid < 0) return -1;
    struct task* t = task_find(pid);
    if (t) task_set_reap_owned(t, 1);
    int code = -1;
    task_wait(pid, &code);
    return code;
}

/* A release or a single executable.  `name_opt` may be NULL. */
static int app_install_release(const char* archive, const char* name_opt, int kind) {
    char name[32], ver[64];
    name_ver_from_file(archive, name, sizeof name, ver, sizeof ver);
    if (name_opt && *name_opt) scpy(name, name_opt, sizeof name);
    if (!name[0] || slen(name) > 24) { kprintf("app: give the application a name of 1..24 characters: app install <file> <name>\n"); return -1; }
    const char* man = known_manifest(name);
    char base[128], tmp[160];
    base[0] = 0; scat(base, APPS_ROOT "/", sizeof base); scat(base, name, sizeof base);
    tmp[0] = 0;  scat(tmp, base, sizeof tmp); scat(tmp, "/.incoming", sizeof tmp);
    mkdir_p(base);
    if (exists(tmp)) vfs_unlink_recursive(tmp);
    scpy(g_installing, name, sizeof g_installing);
    if (kind == AK_TARBALL) {
        kprintf("app: unpacking %s ...\n", archive);
        int rc = run_tarx(archive, tmp);
        if (rc != 0) {
            g_installing[0] = 0;
            kprintf("app: %s could not be unpacked (%d) - nothing installed\n", archive, rc);
            vfs_unlink_recursive(tmp);
            return -5;
        }
        if (!ver[0]) {                       /* the version from the top directory */
            char info[512], top[96];
            char ip[192]; ip[0] = 0; scat(ip, tmp, sizeof ip); scat(ip, "/.tarx", sizeof ip);
            if (read_small(ip, info, sizeof info) > 0) {
                manifest_value(info, "top", top, sizeof top);
                for (unsigned i = 0; top[i]; i++)
                    if (top[i] >= '0' && top[i] <= '9' && (i == 0 || top[i - 1] == '-')) { scpy(ver, top + i, sizeof ver); break; }
            }
            vfs_unlink(ip);
        } else {
            char ip[192]; ip[0] = 0; scat(ip, tmp, sizeof ip); scat(ip, "/.tarx", sizeof ip);
            vfs_unlink(ip);
        }
    } else {                                  /* one executable */
        char prog[64];
        man ? manifest_value(man, "program", prog, sizeof prog) : (void)(prog[0] = 0);
        if (!prog[0]) scpy(prog, name, sizeof prog);
        char bin[192]; bin[0] = 0; scat(bin, tmp, sizeof bin); scat(bin, "/bin", sizeof bin);
        mkdir_p(bin);
        char to[256]; to[0] = 0; scat(to, bin, sizeof to); scat(to, "/", sizeof to); scat(to, prog, sizeof to);
        if (vfs_copy(archive, to) != 0) {
            g_installing[0] = 0;
            kprintf("app: cannot copy %s - nothing installed\n", archive);
            vfs_unlink_recursive(tmp);
            return -5;
        }
        vfs_chmod(to, 0755);
    }
    if (!ver[0]) scpy(ver, "unversioned", sizeof ver);
    sanitize(ver);
    /* can it run here? */
    int progs = release_check(tmp);
    if (progs < 0) {
        g_installing[0] = 0;
        kprintf("app: %s was NOT installed - it cannot run on this machine (reasons above)\n", name);
        vfs_unlink_recursive(tmp);
        return -6;
    }
    char dest[192]; dest[0] = 0; scat(dest, base, sizeof dest); scat(dest, "/", sizeof dest); scat(dest, ver, sizeof dest);
    char rec[224]; rec[0] = 0; scat(rec, dest, sizeof rec); scat(rec, "/.app", sizeof rec);
    if (exists(dest) && exists(rec)) {
        g_installing[0] = 0;
        kprintf("app: %s %s is already installed at %s\n", name, ver, dest);
        vfs_unlink_recursive(tmp);
        return 1;
    }
    if (exists(dest)) vfs_unlink_recursive(dest);          /* a half install */
    if (vfs_rename(tmp, dest) != 0) {
        g_installing[0] = 0;
        kprintf("app: cannot move the unpacked tree to %s\n", dest);
        vfs_unlink_recursive(tmp);
        return -7;
    }
    /* its manifest: the release's own, else the one this system knows */
    char mp[224]; mp[0] = 0; scat(mp, dest, sizeof mp); scat(mp, "/.dos-app", sizeof mp);
    if (!exists(mp) && man) write_small(mp, man);
    {
        char m[512]; m[0] = 0;
        scat(m, "name=", sizeof m); scat(m, name, sizeof m);
        scat(m, "\nversion=", sizeof m); scat(m, ver, sizeof m);
        scat(m, "\nsource=", sizeof m); scat(m, archive, sizeof m);
        scat(m, "\nkind=", sizeof m); scat(m, kind == AK_EXEC ? "executable" : "release", sizeof m);
        scat(m, "\n", sizeof m);
        write_small(rec, m);                                /* written LAST: "installed" */
    }
    char cp[160]; cp[0] = 0; scat(cp, base, sizeof cp); scat(cp, "/current", sizeof cp);
    char old[64]; old[0] = 0;
    read_small(cp, old, sizeof old);
    if (old[0]) app_unlink(name);
    write_small(cp, ver);
    int n = app_link(name, 0);
    g_installing[0] = 0;
    kprintf("app: %s %s installed%s%s; %d program(s) linked into /bin\n", name, ver,
            old[0] && !seq(old, ver) ? " - previous version kept: " : "", old[0] && !seq(old, ver) ? old : "",
            n < 0 ? 0 : n);
    app_start(name, 0);
    return 0;
}

/* Install `archive` as application `name`.  0 installed, 1 already there,
 * <0 refused (with the reason printed). */
static int app_install_body(const char* archive, const char* name);
int app_install(const char* archive, const char* name) {
    if (!exists(archive)) { kprintf("app: %s does not exist\n", archive); return -1; }
    if (!exists(APPS_ROOT) && vfs_mkdir(APPS_ROOT) != 0) {
        kprintf("app: %s cannot be created - installed software needs the persistent disk\n", APPS_ROOT);
        return -2;
    }
    /* §M93 — what kind of file is it? */
    int kind = archive_kind(archive);
    if (kind == AK_NONE) {
        kprintf("app: %s is not something this system can install (not a .tar.gz/.tgz/.tar "
                "release, an executable or an image archive)\n", archive);
        return -1;
    }
    if (kind != AK_OCI) return app_install_release(archive, name, kind);
    if (!name || !*name || slen(name) > 24) { kprintf("app: an image needs a name of 1..24 characters: app install <image.tar> <name>\n"); return -1; }
    /* Raised FIRST: even reading the image's configuration means reading the
     * whole archive, which takes minutes for a JDK. */
    scpy(g_installing, name, sizeof g_installing);
    int result = app_install_body(archive, name);
    g_installing[0] = 0;
    return result;
}

static int app_install_body(const char* archive, const char* name) {
    char conf[96]; conf[0] = 0; scat(conf, "/tmp/.app-", sizeof conf); scat(conf, name, sizeof conf); scat(conf, ".conf", sizeof conf);
    vfs_mkdir("/tmp");
    if (ctr_unpack(archive, "-", conf, NULL) != 0) { kprintf("app: %s could not be read as an image\n", archive); return -3; }

    /* Where the application lives inside the image, and which version it is:
     * the image says so itself (JAVA_HOME / JAVA_VERSION for a JDK). */
    char key[40], home[160], ver[64];
    unsigned k = 0;
    for (; name[k] && k < 24; k++) key[k] = (name[k] >= 'a' && name[k] <= 'z') ? (char)(name[k] - 32) : name[k];
    key[k] = 0;
    char hk[48]; hk[0] = 0; scat(hk, key, sizeof hk); scat(hk, "_HOME", sizeof hk);
    char vk[48]; vk[0] = 0; scat(vk, key, sizeof vk); scat(vk, "_VERSION", sizeof vk);
    if (conf_env(conf, hk, home, sizeof home) != 0 || home[0] != '/') {
        kprintf("app: the image does not say where %s lives (%s in its environment) - not installed\n", name, hk);
        vfs_unlink(conf);
        return -4;
    }
    if (conf_env(conf, vk, ver, sizeof ver) != 0) scpy(ver, "unversioned", sizeof ver);
    sanitize(ver);

    char base[128], dest[192];
    base[0] = 0; scat(base, APPS_ROOT "/", sizeof base); scat(base, name, sizeof base);
    dest[0] = 0; scat(dest, base, sizeof dest); scat(dest, "/", sizeof dest); scat(dest, ver, sizeof dest);
    /* INSTALLED means the record written LAST exists — not merely the
     * directory.  A machine switched off half way through leaves a partial
     * tree, and taking that for an installed program would make the next
     * boot report success over a JDK with holes in it.  So: no record, the
     * tree goes and the install starts again. */
    char rec[224]; rec[0] = 0; scat(rec, dest, sizeof rec); scat(rec, "/.app", sizeof rec);
    if (exists(dest) && !exists(rec)) {
        kprintf("app: %s %s was left half-installed (interrupted?) - removing it and installing again\n", name, ver);
        vfs_unlink_recursive(dest);
    }
    if (exists(dest)) {
        kprintf("app: %s %s is already installed at %s\n", name, ver, dest);
        vfs_unlink(conf);
        return 1;
    }
    mkdir_p(base);
    kprintf("app: installing %s %s from %s (%s) into %s ...\n", name, ver, archive, home, dest);
    char conf2[96]; conf2[0] = 0; scat(conf2, conf, sizeof conf2); scat(conf2, "2", sizeof conf2);
    int rc = ctr_unpack(archive, dest, conf2, home);
    vfs_unlink(conf2);
    vfs_unlink(conf);
    if (rc != 0) {
        kprintf("app: unpacking failed (%d) - the partial tree is removed\n", rc);
        vfs_unlink_recursive(dest);
        return -5;
    }
    /* A record of what this is, beside it. */
    {
        char m[512]; m[0] = 0;
        scat(m, "name=", sizeof m); scat(m, name, sizeof m);
        scat(m, "\nversion=", sizeof m); scat(m, ver, sizeof m);
        scat(m, "\nsource=", sizeof m); scat(m, archive, sizeof m);
        scat(m, "\nhome-in-image=", sizeof m); scat(m, home, sizeof m); scat(m, "\n", sizeof m);
        char mp[224]; mp[0] = 0; scat(mp, dest, sizeof mp); scat(mp, "/.app", sizeof mp);
        write_small(mp, m);
    }
    char cp[160]; cp[0] = 0; scat(cp, base, sizeof cp); scat(cp, "/current", sizeof cp);
    char old[64]; old[0] = 0;
    read_small(cp, old, sizeof old);
    write_small(cp, ver);
    int n = app_link(name, 0);
    kprintf("app: %s %s installed%s%s; %d program(s) linked into /bin\n", name, ver,
            old[0] ? " - previous version kept: " : "", old, n < 0 ? 0 : n);
    return 0;
}

static void app_list(void) {
    if (g_installing[0]) kprintf("  %s: INSTALLING NOW - it appears in /bin when this finishes\n", g_installing);
    struct file* d = vfs_open(APPS_ROOT, VFS_RDONLY);
    if (!d) { kprintf("no installed applications (%s does not exist)\n", APPS_ROOT); return; }
    struct dirent de;
    int any = 0;
    while (vfs_readdir(d, &de) > 0) {
        if (de.type != INODE_DIR) continue;
        char base[128], cur[64];
        base[0] = 0; scat(base, APPS_ROOT "/", sizeof base); scat(base, de.name, sizeof base);
        char cp[160]; cp[0] = 0; scat(cp, base, sizeof cp); scat(cp, "/current", sizeof cp);
        if (read_small(cp, cur, sizeof cur) <= 0) cur[0] = 0;
        kprintf("  %s\n", de.name);
        struct file* v = vfs_open(base, VFS_RDONLY);
        if (!v) continue;
        struct dirent ve;
        while (vfs_readdir(v, &ve) > 0)
            if (ve.type == INODE_DIR)
                kprintf("    %s %s\n", seq(ve.name, cur) ? "*" : " ", ve.name);
        vfs_close(v);
        any = 1;
    }
    vfs_close(d);
    if (!any) kprintf("no installed applications\n");
    else kprintf("(* = the version /bin points at)\n");
}

static int app_use(const char* name, const char* ver) {
    char dest[192]; dest[0] = 0;
    scat(dest, APPS_ROOT "/", sizeof dest); scat(dest, name, sizeof dest); scat(dest, "/", sizeof dest); scat(dest, ver, sizeof dest);
    if (!exists(dest)) { kprintf("app: %s %s is not installed\n", name, ver); return -1; }
    char cp[160]; cp[0] = 0; scat(cp, APPS_ROOT "/", sizeof cp); scat(cp, name, sizeof cp); scat(cp, "/current", sizeof cp);
    app_unlink(name);
    write_small(cp, ver);
    int n = app_link(name, 0);
    kprintf("app: %s now %s (%d program(s) in /bin)\n", name, ver, n < 0 ? 0 : n);
    return 0;
}

static int app_remove(const char* name, const char* ver) {
    char base[128]; base[0] = 0; scat(base, APPS_ROOT "/", sizeof base); scat(base, name, sizeof base);
    char cp[160], cur[64]; cp[0] = 0; scat(cp, base, sizeof cp); scat(cp, "/current", sizeof cp);
    if (read_small(cp, cur, sizeof cur) <= 0) cur[0] = 0;
    if (!ver || !*ver) {                                   /* the whole application */
        app_unlink(name);
        int r = vfs_unlink_recursive(base);
        kprintf(r == 0 ? "app: %s removed\n" : "app: %s could not be removed\n", name);
        return r;
    }
    char dest[192]; dest[0] = 0; scat(dest, base, sizeof dest); scat(dest, "/", sizeof dest); scat(dest, ver, sizeof dest);
    if (!exists(dest)) { kprintf("app: %s %s is not installed\n", name, ver); return -1; }
    if (seq(cur, ver)) { app_unlink(name); vfs_unlink(cp); }
    int r = vfs_unlink_recursive(dest);
    kprintf(r == 0 ? "app: %s %s removed%s\n" : "app: %s %s could not be removed%s\n", name, ver,
            seq(cur, ver) ? " (it was active: /bin no longer has it; `app use` another version)" : "");
    return r;
}

/* ---- boot: relink, and install what was delivered ----------------------------- */

/* §M93 — `nft` is part of the system (built by `make nftables`, embedded):
 * Docker's nftables firewall backend runs it, and a person reads rules with it. */
extern const unsigned char _binary_user_nft_start[] __attribute__((weak));
extern const unsigned char _binary_user_nft_end[]   __attribute__((weak));
static void provision_nft(void) {
    if (!_binary_user_nft_start || exists("/bin/nft")) return;
    vfs_mkdir("/bin");
    if (vfs_create("/bin/nft") != 0) return;
    struct file* f = vfs_open("/bin/nft", VFS_WRONLY);
    if (!f) return;
    size_t n = (size_t)(_binary_user_nft_end - _binary_user_nft_start);
    ssize_t w = vfs_write(f, _binary_user_nft_start, n);
    vfs_close(f);
    vfs_chmod("/bin/nft", 0755);
    if (w != (ssize_t)n) { vfs_unlink("/bin/nft"); kprintf("apps: /bin/nft could not be written\n"); }
}

static void apps_boot_entry(void) {
    provision_nft();
    /* The persistent disk is mounted by the boot path; this service may start
     * before that.  Wait a bounded while for it rather than race it. */
    for (int i = 0; i < 300 && !exists("/mnt/d-os.conf") && !exists(APPS_ROOT) && !exists(INCOMING); i++)
        task_msleep(100);
    int relinked = 0, programs = 0;
    struct file* d = vfs_open(APPS_ROOT, VFS_RDONLY);
    if (d) {
        struct dirent de;
        char names[16][32]; int nn = 0;
        while (vfs_readdir(d, &de) > 0 && nn < 16) if (de.type == INODE_DIR) scpy(names[nn++], de.name, 32);
        vfs_close(d);
        for (int i = 0; i < nn; i++) {
            int m = app_link(names[i], 1);
            if (m > 0) { relinked++; programs += m; }
            else kprintf("apps: %s is installed but NONE of its programs could be linked into /bin\n", names[i]);
        }
    }
    if (relinked) kprintf("apps: %d installed application(s), %d program(s) linked into /bin\n", relinked, programs);
    /* §M93 — what each needs to run (its manifest: services, directories). */
    {
        struct file* d2 = vfs_open(APPS_ROOT, VFS_RDONLY);
        if (d2) {
            struct dirent de2;
            char nm[16][32]; int k = 0;
            while (vfs_readdir(d2, &de2) > 0 && k < 16) if (de2.type == INODE_DIR) scpy(nm[k++], de2.name, 32);
            vfs_close(d2);
            for (int i = 0; i < k; i++) app_start(nm[i], 1);
        }
    }

    struct file* in = vfs_open(INCOMING, VFS_RDONLY);
    if (!in) return;
    char files[8][64]; int nf = 0;
    struct dirent de;
    while (vfs_readdir(in, &de) > 0 && nf < 8) {
        unsigned n = slen(de.name);
        if (de.type == INODE_FILE && ((n > 4 && seq(de.name + n - 4, ".tar")) ||
                                      (n > 4 && seq(de.name + n - 4, ".tgz")) ||
                                      (n > 7 && seq(de.name + n - 7, ".tar.gz"))))
            scpy(files[nf++], de.name, 64);
    }
    vfs_close(in);
    for (int i = 0; i < nf; i++) {
        char path[128], mark[140], name[64];
        path[0] = 0; scat(path, INCOMING "/", sizeof path); scat(path, files[i], sizeof path);
        mark[0] = 0; scat(mark, path, sizeof mark); scat(mark, ".installed", sizeof mark);
        if (exists(mark)) continue;
        scpy(name, files[i], sizeof name);
        unsigned nl = slen(name);
        int image = nl > 4 && seq(name + nl - 4, ".tar") && archive_kind(path) == AK_OCI;
        if (image) name[nl - 4] = 0;                       /* "java.tar" -> "java" */
        kprintf("apps: %s was delivered and is not installed yet - installing it now "
                "(this takes a few minutes on first boot; `app list` shows when it is ready)\n", path);
        int r = app_install(path, image ? name : NULL);
        if (r >= 0) write_small(mark, "installed\n");
    }
}
SERVICE("apps", apps_boot_entry, 1, SVC_RESTART_NO);

/* ---- the command ---------------------------------------------------------------- */

static void cmd_app(const char* args) {
    char w[4][128]; int n = 0;
    while (*args && n < 4) {
        while (*args == ' ') args++;
        if (!*args) break;
        int i = 0;
        while (*args && *args != ' ' && i < 127) w[n][i++] = *args++;
        w[n][i] = 0; n++;
    }
    if (n >= 2 && seq(w[0], "install")) { app_install(w[1], n >= 3 ? w[2] : NULL); return; }
    if (n >= 2 && seq(w[0], "start"))   { app_start(w[1], 0); return; }
    if (n >= 1 && seq(w[0], "list"))    { app_list(); return; }
    if (n >= 3 && seq(w[0], "use"))     { app_use(w[1], w[2]); return; }
    if (n >= 2 && seq(w[0], "remove"))  { app_remove(w[1], n >= 3 ? w[2] : NULL); return; }
    kprintf("usage: app install <file> [name] | list | use <name> <version> | remove <name> [version]\n"
            "       | start <name>\n"
            "       <file>: a .tar.gz/.tgz/.tar release, one executable, or an image archive;\n"
            "       installs into " APPS_ROOT "/<name>/<version>, programs appear in /bin\n");
}
SHELL_CMD(app) = { "app", "install|list|use|remove ...", "installed applications from images (§M89)",
                   SHELL_G_PKG, cmd_app, SHELL_P_ADMIN };
