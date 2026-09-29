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

/* /bin links for the active version of `name`.  An existing /bin entry is
 * replaced only when it is a link into /mnt/apps — a system program is never
 * shadowed by an application, whatever it is called. */
static int app_link(const char* name, int quiet) {
    char cur[64], bin[160];
    char p[160]; p[0] = 0; scat(p, APPS_ROOT "/", sizeof p); scat(p, name, sizeof p); scat(p, "/current", sizeof p);
    if (read_small(p, cur, sizeof cur) <= 0) return -1;
    bin[0] = 0; scat(bin, APPS_ROOT "/", sizeof bin); scat(bin, name, sizeof bin);
    scat(bin, "/", sizeof bin); scat(bin, cur, sizeof bin); scat(bin, "/bin", sizeof bin);
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

/* Install `archive` as application `name`.  0 installed, 1 already there,
 * <0 refused (with the reason printed). */
static int app_install_body(const char* archive, const char* name);
int app_install(const char* archive, const char* name) {
    if (!name || !*name || slen(name) > 24) { kprintf("app: a name of 1..24 characters is needed\n"); return -1; }
    if (!exists(APPS_ROOT) && vfs_mkdir(APPS_ROOT) != 0) {
        kprintf("app: %s cannot be created - installed software needs the persistent disk\n", APPS_ROOT);
        return -2;
    }
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

static void apps_boot_entry(void) {
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

    struct file* in = vfs_open(INCOMING, VFS_RDONLY);
    if (!in) return;
    char files[8][64]; int nf = 0;
    struct dirent de;
    while (vfs_readdir(in, &de) > 0 && nf < 8) {
        unsigned n = slen(de.name);
        if (de.type == INODE_FILE && n > 4 && seq(de.name + n - 4, ".tar")) scpy(files[nf++], de.name, 64);
    }
    vfs_close(in);
    for (int i = 0; i < nf; i++) {
        char path[128], mark[140], name[64];
        path[0] = 0; scat(path, INCOMING "/", sizeof path); scat(path, files[i], sizeof path);
        mark[0] = 0; scat(mark, path, sizeof mark); scat(mark, ".installed", sizeof mark);
        if (exists(mark)) continue;
        scpy(name, files[i], sizeof name);
        name[slen(name) - 4] = 0;                          /* "java.tar" -> "java" */
        kprintf("apps: %s was delivered and is not installed yet - installing it now "
                "(this takes a few minutes on first boot; `app list` shows when it is ready)\n", path);
        int r = app_install(path, name);
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
    if (n >= 3 && seq(w[0], "install")) { app_install(w[1], w[2]); return; }
    if (n >= 1 && seq(w[0], "list"))    { app_list(); return; }
    if (n >= 3 && seq(w[0], "use"))     { app_use(w[1], w[2]); return; }
    if (n >= 2 && seq(w[0], "remove"))  { app_remove(w[1], n >= 3 ? w[2] : NULL); return; }
    kprintf("usage: app install <image.tar> <name> | list | use <name> <version> | remove <name> [version]\n"
            "       installs into " APPS_ROOT "/<name>/<version>, programs appear in /bin\n");
}
SHELL_CMD(app) = { "app", "install|list|use|remove ...", "installed applications from images (§M89)",
                   SHELL_G_PKG, cmd_app, SHELL_P_ADMIN };
