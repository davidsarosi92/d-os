/* =============================================================================
 * cmd_pkg.c — package store, on-device compiler and program launch (§M70).
 *
 * Split out of shell.c.  `pkgrun` is where the "two brothers" seam is visible
 * from a console: the package declares its ABI and `pkg_run` maps that to a
 * personality in ONE place, so `pkgrun hello` takes the native syscall path
 * and `pkgrun echo` takes the Linux one — same store, two real backends,
 * chosen by DATA rather than by a hardcoded name.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "pkg.h"
#include "vfs.h"
#include "kmalloc.h"
#include "task.h"
#include "proc.h"
#include "elf.h"
#include "config.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

/* §M43 — the on-device C compiler: an embedded musl ELF run under the Linux
 * personality with the shell args as argv.  tcc reads the source and headers
 * off the VFS and writes a runnable ELF back to it. */
extern const unsigned char _binary_user_dostcc_start[] __attribute__((weak));
extern const unsigned char _binary_user_dostcc_end[]   __attribute__((weak));

/* §M35.5 + §M36 — `pkgrun <name> [args...]`: exec an INSTALLED package's binary
 * from the /store, with argv.  The package's declared .abi picks the exec
 * personality (pkg_run), so a musl/Linux coreutil and a native program run the
 * same way — the ABI is data, not a special case here. */
static void cmd_pkgrun(const char* line) {
    static char scratch[256];
    const char* argv[16];
    int argc = 0;

    int n = 0;
    while (line[n] && n < 255) { scratch[n] = line[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        char q = 0;
        if (scratch[i] == '"' || scratch[i] == '\'') { q = scratch[i]; i++; }
        argv[argc++] = &scratch[i];
        if (q) { while (scratch[i] && scratch[i] != q) i++; }   /* quoted arg */
        else   { while (scratch[i] && scratch[i] != ' ') i++; }
        if (scratch[i]) scratch[i++] = '\0';
    }
    if (argc == 0) { console_write("usage: pkgrun <name> [args...]\n"); return; }

    int rc = pkg_backend_active()->run(argc, (const char* const*)argv);
    kprintf("pkgrun: '%s' returned rc=%d\n", argv[0], rc);
}

static void cmd_tcc(const char* args) {
    /* §M62 follow-up — the compiler's headers/crt/libs are unpacked HERE, on
     * first use, instead of at every boot (see pkg.h). */
    pkg_ensure_tcc_rootfs();
    if (!_binary_user_dostcc_start) {
        console_write("tcc: not embedded — run `make tcc` then rebuild\n");
        return;
    }
    static char scratch[256];
    const char* argv[18];
    int argc = 0;
    argv[argc++] = "tcc";                        /* argv[0] */
    /* -B sets tcc's base dir explicitly: we run tcc from an embedded blob with
     * argv[0]="tcc" (no path), so tcc can't derive its dir → point it at where
     * pkg.c provisioned libtcc1.a + tcc's own headers. */
    argv[argc++] = "-B/usr/lib/tcc";
    int n = 0;
    while (args[n] && n < 255) { scratch[n] = args[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        argv[argc++] = &scratch[i];
        while (scratch[i] && scratch[i] != ' ') i++;
        if (scratch[i]) scratch[i++] = '\0';
    }
    size_t len = (size_t)(_binary_user_dostcc_end - _binary_user_dostcc_start);
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf_argv(_binary_user_dostcc_start, len, argc,
                                (const char* const*)argv);
    if (me) me->linux_abi = prev;
    kprintf("tcc: returned rc=%d\n", rc);
}

/* §M43 — `exec <path>`: load + run an ELF from the VFS (e.g. one tcc just
 * produced) under the Linux personality, as an excursion that returns to the
 * shell.  This is what closes the "compile → run" loop on d-os. */
static void cmd_exec(const char* path) {
    /* Run via the capturing engine (which opens/reads the ELF itself) so we can
     * also report the byte count — exercising the §M43 stdout-capture path the
     * editor's Output window uses.  Output still echoes to the console. */
    static char cap[4096];
    cap[0] = '\0';
    int rc = dos_run_elf_cap(path, cap, sizeof cap);
    if (rc == -1 && cap[0] == '\0') { kprintf("exec: '%s' not runnable\n", path); return; }
    int caplen = 0; while (cap[caplen]) caplen++;
    kprintf("exec: '%s' returned rc=%d [captured %d bytes]\n", path, rc, caplen);
}

/* §M43 — reusable compile+run engine (devtools.h), shared with the GUI editor's
 * "Compile & Run" button.  Runs the embedded tcc / loads a VFS ELF, both under
 * the Linux personality. */
int dos_tcc_available(void) { return _binary_user_dostcc_start != 0; }

int dos_tcc_compile(const char* src, const char* out) {
    if (!_binary_user_dostcc_start) return -1;
    const char* argv[5] = { "tcc", "-B/usr/lib/tcc", src, "-o", out };
    size_t len = (size_t)(_binary_user_dostcc_end - _binary_user_dostcc_start);
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    proc_exec_elf_argv(_binary_user_dostcc_start, len, 5, (const char* const*)argv);
    if (me) me->linux_abi = prev;
    /* Success proxy: tcc produced a non-empty output file. */
    struct file* f = vfs_open(out, VFS_RDONLY);
    if (!f) return -1;
    int ok = (f->inode && f->inode->size > 0) ? 0 : -1;
    vfs_close(f);
    return ok;
}

/* §M35.5 — package manager.  Dispatches through the ACTIVE, swappable backend
 * (pkg_backend_active) rather than the store functions directly, so a different
 * pkg-manager implementation transparently serves the same commands. */
static void cmd_pkg(const char* args) {
    const struct pkg_ops* b = pkg_backend_active();
    if (cmd_starts_with(args, "build "))   { b->build(args + 6);   return; }
    if (cmd_starts_with(args, "install ")) { b->install(args + 8); return; }
    if (cmd_starts_with(args, "remove "))  { b->remove(args + 7);  return; }
    if (cmd_starts_with(args, "why "))     { b->why(args + 4);     return; }
    if (cmd_streq(args, "gc"))             { b->gc();              return; }
    if (cmd_streq(args, "list") || !*args) { b->list();            return; }
    if (cmd_streq(args, "backend")) {
        kprintf("pkg: active backend '%s' v%s\n", b->name, b->version);
        return;
    }
    console_write("usage: pkg build|install|remove|why <id> | list | gc | backend\n");
}

/* §M35.5 — scripted demo: two hello versions coexist, install hello-2 + args
 * (deps hello-2), gc reclaims the unreferenced hello-1. */
static void cmd_pkgtest(void) {
    console_write("pkgtest: content-addressed store demo\n");
    pkg_build("hello-1");                /* hello 1.0 */
    pkg_build("hello-2");                /* hello 2.0 — coexists (distinct hash) */
    pkg_install("hello-2");
    pkg_install("args");                 /* deps hello-2 → pinned closure */
    console_write("--- store before gc ---\n");
    pkg_list();
    pkg_gc();                            /* reclaims hello-1 (not in any closure) */
    console_write("--- store after gc (hello-1 gone; hello-2 + args kept) ---\n");
    pkg_list();
}

/* --- registrations --------------------------------------------------------- */

static void pk_pkgtest(const char* a) { (void)a; cmd_pkgtest(); }

static void pk_tcc(const char* a) {
    if (!a[0]) { console_write("usage: tcc <src.c> -o <out> [args]\n"); return; }
    cmd_tcc(a);
}

SHELL_CMD(pkg)     = { "pkg", "[list|install <name>|gc|profile]", "the package store",
                       SHELL_G_PKG, cmd_pkg, SHELL_P_ADMIN };
SHELL_CMD(pkgrun)  = { "pkgrun", "<name> [args]", "run a program from the store",
                       SHELL_G_PKG, cmd_pkgrun, SHELL_P_ANY };
SHELL_CMD(exec)    = { "exec", "<path>", "run an ELF from the filesystem",
                       SHELL_G_PKG, cmd_exec, SHELL_P_ADMIN };
SHELL_CMD(tcc)     = { "tcc", "<src.c> -o <out> [args]", "compile C on the machine itself",
                       SHELL_G_PKG, pk_tcc, SHELL_P_ADMIN };
SHELL_CMD(pkgtest) = { "pkgtest", "", "store round trip: install, run, GC",
                       SHELL_G_TEST, pk_pkgtest, SHELL_P_ADMIN };

/* ---- §M89 — programs on the PATH ------------------------------------------
 *
 * The shell's built-ins are a registry; everything else a user wants to run is
 * a FILE — an installed application's link in /bin, a Linux program.  This is
 * the fallback the dispatcher takes for a verb it does not know: split the
 * line into arguments (double quotes group words), search the `env.PATH`
 * setting for the verb (or take a path containing '/'), and run it in the
 * foreground until it exits.
 *
 * WHICH PERSONALITY: a dynamically linked program (PT_INTERP) is a Linux one —
 * everything installed from an image is — and so is anything that resolves
 * into /mnt/apps; the rest are this system's own programs.  /proc/self/exe is
 * the resolved path, which is how a JDK started as `java` finds its home. */
#include "config.h"
#include "cred.h"

static int is_linux_elf(const uint8_t* img, size_t len, const char* real) {
    const char* a = "/mnt/apps/";
    int i = 0;
    while (a[i] && real[i] == a[i]) i++;
    if (!a[i]) return 1;
    if (len < 64 || img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F') return 0;
    int is64 = img[4] == 2;
    uint64_t phoff = is64 ? *(const uint64_t*)(img + 32) : *(const uint32_t*)(img + 28);
    uint16_t phent = *(const uint16_t*)(img + (is64 ? 54 : 42));
    uint16_t phnum = *(const uint16_t*)(img + (is64 ? 56 : 44));
    for (uint16_t k = 0; k < phnum; k++) {
        uint64_t o = phoff + (uint64_t)k * phent;
        if (o + 4 > len) break;
        if (*(const uint32_t*)(img + o) == 3) return 1;          /* PT_INTERP */
    }
    return 0;
}

int shell_run_from_path(const char* line) {
    /* Arguments, with "double quoted" words kept together. */
    static char pool[1024];
    const char* argv[32];
    int argc = 0, used = 0;
    const char* p = line;
    while (*p && argc < 31) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = pool + used;
        int q = 0;
        while (*p && (q || *p != ' ')) {
            if (*p == '"') { q = !q; p++; continue; }
            if (used < (int)sizeof pool - 2) pool[used++] = *p;
            p++;
        }
        pool[used++] = 0;
    }
    argv[argc] = NULL;
    if (!argc) return 0;

    /* The program: a path as given, else the first PATH entry that has it. */
    char path[256], real[256];
    path[0] = 0;
    int has_slash = 0;
    for (const char* v = argv[0]; *v; v++) if (*v == '/') has_slash = 1;
    struct vfs_stat st;
    if (has_slash) {
        int k = 0; for (; argv[0][k] && k < 255; k++) path[k] = argv[0][k]; path[k] = 0;
        if (vfs_stat(path, &st) != 0 || st.is_dir) return 0;
    } else {
        const char* pv = config_get("env.PATH", "/bin:/usr/bin");
        while (*pv) {
            int k = 0;
            while (*pv && *pv != ':' && k < 200) path[k++] = *pv++;
            if (*pv == ':') pv++;
            if (!k) continue;
            path[k++] = '/';
            for (const char* v = argv[0]; *v && k < 255; v++) path[k++] = *v;
            path[k] = 0;
            if (vfs_stat(path, &st) == 0 && !st.is_dir) break;
            path[0] = 0;
        }
        if (!path[0]) return 0;
    }
    if (vfs_realpath(path, real, sizeof real) != 0) { int k = 0; for (; path[k]; k++) real[k] = path[k]; real[k] = 0; }

    struct file* f = vfs_open(real, VFS_RDONLY);
    if (!f) return 0;
    size_t sz = f->inode ? (size_t)f->inode->size : 0;
    uint8_t* img = sz ? (uint8_t*)kmalloc(sz) : NULL;
    ssize_t got = img ? vfs_read(f, img, sz) : -1;
    vfs_close(f);
    if (!img || got != (ssize_t)sz) { if (img) kfree(img); kprintf("%s: cannot read %s\n", argv[0], real); return 1; }
    int linux_abi = is_linux_elf(img, sz, real);

    /* The child inherits the cred, and with it /proc/self/exe. */
    struct task* me = task_current();
    char saved[sizeof me->cred.exe];
    for (unsigned i = 0; i < sizeof saved; i++) saved[i] = me->cred.exe[i];
    int k = 0; for (; real[k] && k < (int)sizeof me->cred.exe - 1; k++) me->cred.exe[k] = real[k];
    me->cred.exe[k] = 0;
    const char* name = argv[0];
    for (const char* v = argv[0]; *v; v++) if (*v == '/') name = v + 1;
    int pid = proc_spawn_argv(name, img, sz, argc, argv, linux_abi);
    for (unsigned i = 0; i < sizeof saved; i++) me->cred.exe[i] = saved[i];
    kfree(img);
    if (pid < 0) { kprintf("%s: could not start %s\n", argv[0], real); return 1; }
    struct task* t = task_find(pid);
    if (t) task_set_reap_owned(t, 1);
    int code = 0;
    task_wait(pid, &code);
    if (code) kprintf("[%s exited with status %d]\n", name, code);
    return 1;
}
