/* =============================================================================
 * container.c — an application arriving as an image, running against its own
 * filesystem (§M73 rungs 1-3).
 *
 * WHAT A CONTAINER IS HERE, AND WHAT IT IS NOT — said first because the second
 * half is the part a reader would otherwise assume:
 *
 *   ISOLATED   its FILESYSTEM VIEW: every task in it resolves "/" to the
 *              image's root (cred.root, inherited by every spawn, fork and
 *              clone — there is no path out, because the VFS resolves only
 *              absolute paths and has no ".." to climb with);
 *              its IDENTITY: it runs as a uid of its own (20000 + id) that owns
 *              its root and nothing outside it (§M32).
 *   NOT        the NETWORK — one stack, shared, visible;
 *              the KERNEL — one kernel, and a kernel bug is everyone's;
 *              the PROCESS LIST — `ps` in the host shows container tasks and a
 *              container task can signal only its own uid's tasks (§M32), but
 *              nothing hides the rest from it;
 *              DEVICES — /dev and /proc are not in the image, so there are none.
 * Saying less than that would be the isolation theatre §M33 refused by name.
 *
 * HOW A PROGRAM STARTS INSIDE ONE.  A kernel thread — the container's init —
 * puts ITSELF in the container first (root, a session, the container's uid),
 * and only then spawns the program, which inherits all of it.  So the ELF is
 * read through the container's root, and a program is never, even for an
 * instruction, running with the machine's "/" or the machine's identity.  The
 * init waits for the program and reports its exit status: that status is how
 * `ctrescapetest` tells "the host file was not reachable" from "it was".
 *
 * WHERE THE IMAGE COMES FROM.  A real multi-platform OCI archive (busybox:musl,
 * `docker save`) is embedded in the kernel as the validation target; `ctr
 * import` writes it to a file and ring-3 `ociunpack` (a program, not kernel
 * code — it parses untrusted input) unpacks the variant for this CPU.  An
 * archive already on disk can be imported by path the same way.
 * ============================================================================= */

#include "task.h"
#include "proc.h"
#include "vfs.h"
#include "cred.h"
#include "kmalloc.h"
#include "printf.h"
#include "shellcmd.h"
#include "audit.h"
#include <stdint.h>
#include <stddef.h>

#define CTR_MAX       8
#define CTR_UID_BASE  20000

struct container {
    int  id;                       /* 1.. ; 0 = free slot */
    char name[32];
    char dir[96];                  /* /containers/<name>            */
    char rootfs[112];              /* /containers/<name>/rootfs     */
    struct dentry* root;
    int  uid;
    char cmd[4][64];               /* the image's Entrypoint + Cmd  */
    int  ncmd;
};
static struct container g_ctr[CTR_MAX];

static void scopy(char* d, const char* s, unsigned cap) {
    unsigned i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}
static void scat(char* d, const char* s, unsigned cap) {
    unsigned n = 0;
    while (d[n]) n++;
    for (unsigned i = 0; s[i] && n + 1 < cap; i++) d[n++] = s[i];
    d[n] = 0;
}
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static struct container* ctr_by_name(const char* n) {
    for (int i = 0; i < CTR_MAX; i++) if (g_ctr[i].id && seq(g_ctr[i].name, n)) return &g_ctr[i];
    return NULL;
}
struct container* ctr_by_id(int id) {
    for (int i = 0; i < CTR_MAX; i++) if (g_ctr[i].id == id) return &g_ctr[i];
    return NULL;
}

/* ---- import ------------------------------------------------------------------ */

extern const unsigned char _binary_assets_images_busybox_musl_tar_start[] __attribute__((weak));
extern const unsigned char _binary_assets_images_busybox_musl_tar_end[]   __attribute__((weak));
extern const unsigned char _binary_user_ociunpack_elf_start[]         __attribute__((weak));
extern const unsigned char _binary_user_ociunpack_elf_end[]           __attribute__((weak));
extern const unsigned char _binary_user_ociunpack_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_ociunpack_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_ociunpack_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_ociunpack_aarch64_elf_end[]   __attribute__((weak));

static int write_blob(const char* path, const unsigned char* b, size_t n) {
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;
    size_t off = 0;
    while (off < n) {
        size_t c = n - off > 65536 ? 65536 : n - off;
        ssize_t w = vfs_write(f, b + off, c);
        if (w <= 0) { vfs_close(f); return -1; }
        off += (size_t)w;
    }
    vfs_close(f);
    return 0;
}

/* Run a program image and wait for it (the unpacker). */
static int run_and_wait(const char* name, const unsigned char* s, const unsigned char* e,
                        int argc, const char* const argv[]) {
    int pid = proc_spawn_argv(name, s, (size_t)(e - s), argc, argv, 0);
    if (pid < 0) return -1;
    struct task* t = task_find(pid);
    if (t) task_set_reap_owned(t, 1);         /* the exit status is ours to read */
    int code = -1;
    task_wait(pid, &code);
    return code;
}

/* image.conf: "entry=" and "cmd=" lines, in order. */
static void read_conf(struct container* c, const char* path) {
    c->ncmd = 0;
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return;
    char buf[2048];
    ssize_t n = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (n <= 0) return;
    buf[n] = 0;
    for (int pass = 0; pass < 2; pass++) {                /* entrypoint first, then cmd */
        const char* key = pass ? "cmd=" : "entry=";
        char* p = buf;
        while (*p) {
            char* line = p;
            while (*p && *p != '\n') p++;
            char save = *p; *p = 0;
            int k = 0; while (key[k] && line[k] == key[k]) k++;
            if (!key[k] && c->ncmd < 4) scopy(c->cmd[c->ncmd++], line + k, sizeof c->cmd[0]);
            *p = save;
            if (*p) p++;
        }
    }
}

/* §M89 — the unpacker for other subsystems (apps.c installs software with it):
 * `root` "-" writes only the image configuration to `conf`; a `subtree`
 * extracts only the entries under it, the prefix removed.  0 or the
 * unpacker's exit status. */
int ctr_unpack(const char* archive, const char* root, const char* conf, const char* subtree) {
    const unsigned char *us = 0, *ue = 0;
    if (_binary_user_ociunpack_elf_start)             { us = _binary_user_ociunpack_elf_start;         ue = _binary_user_ociunpack_elf_end; }
    else if (_binary_user_ociunpack_x86_64_elf_start) { us = _binary_user_ociunpack_x86_64_elf_start;  ue = _binary_user_ociunpack_x86_64_elf_end; }
    else if (_binary_user_ociunpack_aarch64_elf_start){ us = _binary_user_ociunpack_aarch64_elf_start; ue = _binary_user_ociunpack_aarch64_elf_end; }
    if (!us) return -1;
    const char* argv[5] = { "ociunpack", archive, root, conf, subtree };
    return run_and_wait("ociunpack", us, ue, subtree ? 5 : 4, argv);
}

static struct container* ctr_import(const char* name, const char* archive) {
    if (ctr_by_name(name)) return ctr_by_name(name);
    const unsigned char *us = 0, *ue = 0;
    if (_binary_user_ociunpack_elf_start)             { us = _binary_user_ociunpack_elf_start;         ue = _binary_user_ociunpack_elf_end; }
    else if (_binary_user_ociunpack_x86_64_elf_start) { us = _binary_user_ociunpack_x86_64_elf_start;  ue = _binary_user_ociunpack_x86_64_elf_end; }
    else if (_binary_user_ociunpack_aarch64_elf_start){ us = _binary_user_ociunpack_aarch64_elf_start; ue = _binary_user_ociunpack_aarch64_elf_end; }
    if (!us) { kprintf("ctr: no unpacker in this build\n"); return NULL; }
    struct container* c = NULL;
    for (int i = 0; i < CTR_MAX; i++) if (!g_ctr[i].id) { c = &g_ctr[i]; c->id = i + 1; break; }
    if (!c) { kprintf("ctr: all %d container slots are in use\n", CTR_MAX); return NULL; }
    scopy(c->name, name, sizeof c->name);
    c->dir[0] = 0;    scat(c->dir, "/containers/", sizeof c->dir); scat(c->dir, name, sizeof c->dir);
    c->rootfs[0] = 0; scat(c->rootfs, c->dir, sizeof c->rootfs); scat(c->rootfs, "/rootfs", sizeof c->rootfs);
    c->uid = CTR_UID_BASE + c->id;
    vfs_mkdir("/containers");
    vfs_mkdir(c->dir);

    char src[128];
    if (archive) scopy(src, archive, sizeof src);
    else {
        if (!_binary_assets_images_busybox_musl_tar_start) {
            kprintf("ctr: no image embedded in this build and none named\n");
            c->id = 0; return NULL;
        }
        vfs_mkdir("/images");
        scopy(src, "/images/busybox-musl.tar", sizeof src);
        struct file* probe = vfs_open(src, VFS_RDONLY);
        if (probe) vfs_close(probe);
        else if (write_blob(src, _binary_assets_images_busybox_musl_tar_start,
                            (size_t)(_binary_assets_images_busybox_musl_tar_end -
                                     _binary_assets_images_busybox_musl_tar_start)) != 0) {
            kprintf("ctr: could not write the image to %s\n", src);
            c->id = 0; return NULL;
        }
    }
    char conf[128]; conf[0] = 0; scat(conf, c->dir, sizeof conf); scat(conf, "/image.conf", sizeof conf);
    const char* argv[4] = { "ociunpack", src, c->rootfs, conf };
    int rc = run_and_wait("ociunpack", us, ue, 4, argv);
    if (rc != 0) {
        kprintf("ctr: unpacking %s failed (status %d) - see ociunpack's lines above\n", src, rc);
        c->id = 0; return NULL;
    }
    c->root = vfs_resolve(c->rootfs);
    if (!c->root) { kprintf("ctr: %s did not appear\n", c->rootfs); c->id = 0; return NULL; }
    vfs_chown_tree(c->root, c->uid, c->uid);    /* its root is its own, nothing else is */
    read_conf(c, conf);
    kprintf("ctr: '%s' imported as container %d - root %s, uid %d, command:",
            c->name, c->id, c->rootfs, c->uid);
    for (int i = 0; i < c->ncmd; i++) kprintf(" %s", c->cmd[i]);
    kprintf("\n");
    return c;
}

/* ---- run ----------------------------------------------------------------------- */

/* argv lives in ONE pool rather than fixed slots: the first version gave each
 * argument 96 bytes and cut the rest off without a word — so `sh -c "<a long
 * script>"` ran a script missing its tail, and its last command, now `cat`
 * with no argument, sat reading stdin forever.  A limit that truncates quietly
 * is a different program; this one refuses (ctr_argv_add). */
#define CTR_ARGV_POOL 2048
struct ctr_run {
    struct container* c;
    int  argc;
    char pool[CTR_ARGV_POOL];
    int  used;
    const char* argv[16];
    volatile int done;
    volatile int code;
};

static void ctr_init_main(void) {
    struct ctr_run* r = (struct ctr_run*)task_start_arg();
    struct task* me = task_current();
    /* INTO THE CONTAINER FIRST — root, then identity — and only then spawn. */
    me->cred.root = r->c->root;
    me->cred.container = r->c->id;
    if (cred_become_user(me->pid, r->c->uid, r->c->uid, NULL, 0, cred_session_alloc()) != 0) {
        kprintf("ctr: could not take on uid %d\n", r->c->uid);
        r->code = -1; r->done = 1; return;
    }
    /* The program is read THROUGH the container's root. */
    struct file* f = vfs_open(r->argv[0], VFS_RDONLY);
    if (!f) {
        kprintf("ctr: '%s' is not in container '%s'\n", r->argv[0], r->c->name);
        r->code = 127; r->done = 1; return;
    }
    size_t sz = f->inode ? (size_t)f->inode->size : 0;
    uint8_t* img = sz ? (uint8_t*)kmalloc(sz) : NULL;
    ssize_t got = img ? vfs_read(f, img, sz) : -1;
    vfs_close(f);
    if (!img || got != (ssize_t)sz) {
        if (img) kfree(img);
        kprintf("ctr: cannot read %s\n", r->argv[0]);
        r->code = 126; r->done = 1; return;
    }
    const char* name = r->argv[0];
    for (const char* p = r->argv[0]; *p; p++) if (*p == '/') name = p + 1;
    /* §M89 — /proc/self/exe for the program (the child inherits the cred). */
    if (vfs_realpath(r->argv[0], me->cred.exe, sizeof me->cred.exe) != 0) me->cred.exe[0] = 0;
    int pid = proc_spawn_argv(name, img, sz, r->argc, r->argv, /*linux_abi*/1);
    kfree(img);
    if (pid < 0) { kprintf("ctr: could not start %s\n", r->argv[0]); r->code = 126; r->done = 1; return; }
    struct task* t = task_find(pid);
    if (t) task_set_reap_owned(t, 1);
    int code = -1;
    task_wait(pid, &code);
    r->code = code;
    r->done = 1;
}

static int ctr_argv_add(struct ctr_run* r, const char* s) {
    int n = 0; while (s[n]) n++;
    if (r->argc >= 15 || r->used + n + 1 > CTR_ARGV_POOL) return -1;
    char* d = r->pool + r->used;
    for (int i = 0; i <= n; i++) d[i] = s[i];
    r->argv[r->argc++] = d;
    r->argv[r->argc] = NULL;
    r->used += n + 1;
    return 0;
}

/* Start `argv` in container `c` and wait.  argv[0] is resolved INSIDE it; a
 * bare name is looked for in /bin, /usr/bin, /sbin, /usr/sbin there.  Returns
 * the program's exit status (or 127/126 as a shell would). */
int ctr_run(struct container* c, int argc, const char* const argv[]) {
    struct ctr_run* r = (struct ctr_run*)kcalloc(1, sizeof *r);
    if (!r) return -1;
    r->c = c;
    /* argv[0]: an absolute path inside the container, or a bare name looked
     * up the way the image's own shell would (PATH). */
    char prog[160];
    scopy(prog, argv[0], sizeof prog);
    if (prog[0] != '/') {
        static const char* dirs[4] = { "/bin/", "/usr/bin/", "/sbin/", "/usr/sbin/" };
        char probe[160];
        for (int d = 0; d < 4; d++) {
            probe[0] = 0; scat(probe, c->rootfs, sizeof probe); scat(probe, dirs[d], sizeof probe);
            scat(probe, argv[0], sizeof probe);
            struct file* f = vfs_open(probe, VFS_RDONLY);
            if (f) {
                vfs_close(f);
                prog[0] = 0;
                scat(prog, dirs[d], sizeof prog);
                scat(prog, argv[0], sizeof prog);
                break;
            }
        }
    }
    int bad = ctr_argv_add(r, prog);
    for (int i = 1; i < argc && !bad; i++) bad = ctr_argv_add(r, argv[i]);
    if (bad) {
        kprintf("ctr: argument list too long (%d bytes, %d arguments at most) - refused, "
                "not shortened\n", CTR_ARGV_POOL, 15);
        kfree(r);
        return 126;
    }
    char tname[32] = "ctr:"; scat(tname, c->name, sizeof tname);
    if (!task_spawn_arg(tname, ctr_init_main, r)) { kfree(r); return -1; }
    while (!r->done) task_msleep(20);
    int code = r->code;
    kfree(r);
    return code;
}

/* ---- the invariant ------------------------------------------------------------- */

/* NO TASK'S ROOT ESCAPES ITS CONTAINER: a task tagged with a container must
 * resolve "/" to exactly that container's root.  A tag without the root would
 * be a container task seeing the whole machine — the one failure that makes
 * every other claim here false. */
struct ctr_audit { int bad; int n; };
static void ctr_audit_cb(const struct task* t, int cur, void* cx) {
    (void)cur;
    struct ctr_audit* a = (struct ctr_audit*)cx;
    if (!t->cred.container || t->state == TASK_DEAD) return;
    a->n++;
    struct container* c = ctr_by_id(t->cred.container);
    if (!c || t->cred.root != c->root || !c->root) {
        a->bad++;
        kprintf("audit container-root: pid %d '%s' is in container %d but its \"/\" is %s\n",
                t->pid, t->name, t->cred.container,
                !t->cred.root ? "the MACHINE's" : "somewhere else");
    }
}
static int ctr_audit(int verbose) {
    struct ctr_audit a = { 0, 0 };
    task_for_each(ctr_audit_cb, &a);
    if (verbose) kprintf("audit container-root: %d container task(s) checked\n", a.n);
    return a.n ? a.bad : AUDIT_SKIP;
}
AUDIT(ctr_root) = {
    .name = "container-root",
    .what = "every task in a container resolves \"/\" to that container's root",
    .run  = ctr_audit,
};

/* ---- commands -------------------------------------------------------------------- */

static int split_args(char* s, const char* argv[], int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '"') {
            s++; argv[n++] = s;
            while (*s && *s != '"') s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ') s++;
        }
        if (*s) *s++ = 0;
    }
    while (*s == ' ') s++;
    return *s ? -1 : n;                 /* words left over: too many, say so */
}

static void ctr_say_isolation(void) {
    kprintf("ctr: isolated - its filesystem view (\"/\" is the image) and its identity (its "
            "own uid).  NOT isolated - the network (shared), the kernel, the process list "
            "(visible from the host), devices (none inside)\n");
}

static void cmd_ctr(const char* args) {
    char buf[1024];
    {   /* refuse, never shorten: a cut command line is a different command */
        int n = 0; while (args && args[n]) n++;
        if (n >= (int)sizeof buf) { kprintf("ctr: command line too long (%d bytes at most)\n", (int)sizeof buf - 1); return; }
    }
    scopy(buf, args ? args : "", sizeof buf);
    const char* av[18];
    int ac = split_args(buf, av, 18);
    if (ac < 0) { kprintf("ctr: too many arguments (18 at most)\n"); return; }
    if (ac == 0 || seq(av[0], "list")) {
        int any = 0;
        for (int i = 0; i < CTR_MAX; i++) if (g_ctr[i].id) {
            any = 1;
            kprintf("ctr: %d '%s' root %s uid %d\n", g_ctr[i].id, g_ctr[i].name, g_ctr[i].rootfs, g_ctr[i].uid);
        }
        if (!any) kprintf("ctr: no containers - `ctr import <name> [archive]`\n");
        return;
    }
    if (seq(av[0], "import")) {
        ctr_import(ac > 1 ? av[1] : "busybox", ac > 2 ? av[2] : NULL);
        return;
    }
    if (seq(av[0], "run")) {
        if (ac < 2) { kprintf("ctr: usage: ctr run <name> [command args...]\n"); return; }
        struct container* c = ctr_by_name(av[1]);
        if (!c && seq(av[1], "busybox")) c = ctr_import("busybox", NULL);   /* first use */
        if (!c) { kprintf("ctr: no container '%s'\n", av[1]); return; }
        ctr_say_isolation();
        int rc;
        if (ac > 2) rc = ctr_run(c, ac - 2, av + 2);
        else {
            const char* dv[4];
            for (int i = 0; i < c->ncmd; i++) dv[i] = c->cmd[i];
            rc = c->ncmd ? ctr_run(c, c->ncmd, dv) : -1;
        }
        kprintf("ctr: '%s' exited with status %d\n", c->name, rc);
        return;
    }
    if (seq(av[0], "ps")) {
        kprintf("ctr ps: see `ps` - container tasks carry [container N]\n");
        return;
    }
    kprintf("ctr: usage: ctr [list | import <name> [archive] | run <name> [cmd...]]\n");
}
SHELL_CMD(ctr) = { "ctr", "[list|import|run] ...", "containers: an image as a root filesystem and a uid",
                   SHELL_G_TASK, cmd_ctr, SHELL_P_ADMIN };

/* `ctrescapetest` (hidden) — the claim, made to fail if it is false.
 *   control: the image's own /etc/passwd is readable from inside   → 0
 *   escape:  the host's settings file, by its host path             → non-zero
 *   escape:  the same, climbing with ".."                           → non-zero
 *   escape:  the host's /containers tree (where the image lives)    → non-zero
 * Plus the audit, clean while a container task runs, and caught when one is
 * given the machine's root on purpose. */
static void cmd_ctrescapetest(const char* args) {
    (void)args;
    struct container* c = ctr_by_name("busybox");
    if (!c) c = ctr_import("busybox", NULL);
    if (!c) { kprintf("ctrescapetest: no container\n"); return; }
    const char* ctl[3] = { "cat", "/etc/passwd", 0 };
    const char* e1[3]  = { "cat", "/mnt/d-os.conf", 0 };
    const char* e2[3]  = { "cat", "/../../mnt/d-os.conf", 0 };
    const char* e3[3]  = { "ls", "/containers", 0 };
    int r0 = ctr_run(c, 2, ctl), r1 = ctr_run(c, 2, e1), r2 = ctr_run(c, 2, e2), r3 = ctr_run(c, 2, e3);
    /* the audit, and its falsifier: a long-running task, given the machine's
     * root on purpose */
    const char* sl[3] = { "sleep", "3", 0 };
    char tname[32] = "ctr:"; scat(tname, c->name, sizeof tname);
    struct ctr_run* r = (struct ctr_run*)kcalloc(1, sizeof *r);
    int audit_clean = -1, audit_caught = -1;
    if (r) {
        r->c = c;
        ctr_argv_add(r, "/bin/sleep"); ctr_argv_add(r, sl[1]);
        if (task_spawn_arg(tname, ctr_init_main, r)) {
            task_msleep(800);
            audit_clean = ctr_audit(0);
            struct task* victim = NULL;
            /* any task of this container: give it the machine's root */
            for (int pid = 1; pid < 4096 && !victim; pid++) {
                struct task* t = task_find(pid);
                if (t && t->cred.container == c->id && t->state != TASK_DEAD) victim = t;
            }
            if (victim) {
                struct dentry* saved = victim->cred.root;
                victim->cred.root = NULL;          /* the violation */
                audit_caught = ctr_audit(0);
                victim->cred.root = saved;
            }
            while (!r->done) task_msleep(50);
        }
        kfree(r);
    }
    int ok = r0 == 0 && r1 != 0 && r2 != 0 && r3 != 0 && audit_clean == 0 && audit_caught >= 1;
    kprintf("ctrescapetest: image file %d (want 0); host file %d, via \"..\" %d, host tree %d "
            "(want non-zero); audit clean %d, audit caught a root swap %d -> %s\n",
            r0, r1, r2, r3, audit_clean, audit_caught, ok ? "PASS" : "FAIL");
}
SHELL_CMD(ctrescapetest) = { "ctrescapetest", "", 0, SHELL_G_TEST, cmd_ctrescapetest, SHELL_P_ADMIN };
