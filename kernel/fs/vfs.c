/* =============================================================================
 * vfs.c — VFS core.
 *
 * Owns the root dentry, the registered-filesystem list, and path
 * resolution.  Concrete filesystems (ramfs, exfat, ...) plug in via
 * `vfs_register_fs` and provide their own `file_ops` per inode and
 * `inode_ops` per directory inode.
 *
 * Path resolution is a component-by-component walk through the dentry
 * tree.  Each step first looks in the cache (`parent->children`); on
 * miss, if the parent inode has `dir_ops->lookup`, the fs is asked to
 * resolve the name and (on success) the result is attached to the
 * cache.  Filesystems with eager dentry trees (ramfs, devfs, procfs)
 * never see a lookup call; lazy filesystems (exFAT) populate their
 * children incrementally.
 *
 * Pos ownership: `f->pos` lives in the VFS layer.  `vfs_read`/`vfs_write`
 * pass the current value to `file_ops.read/write` as the `off`
 * parameter and bump `f->pos` by the non-negative return value.  Fs
 * implementations are pure offset-addressed and never touch `f->pos`.
 *
 * Future revisions will add `..` / `.` resolution and non-root mount
 * points (the framework is here, but only "/" is exercised today).
 * ============================================================================= */

#include "vfs.h"
#include "pcache.h"
#include "block_cache.h"
#include "kmutex.h"
#include "cred.h"
#include "audit.h"
#include "shellcmd.h"
#include "block.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include <stddef.h>

/* ------------------------------------------------------------------- */
/* Module state.                                                        */
/* ------------------------------------------------------------------- */

static struct fs_type* fs_types = NULL;
static struct dentry*  root     = NULL;

struct dentry* vfs_root(void) { return root; }

/* §M73 — the dentry a path names, resolved from the CALLER's root (the
 * machine's for the shell, the container's for a container task).  NULL if it
 * does not exist.  Needed by whoever sets up a container: its root is a dentry,
 * and the only safe way to get one is the same resolution everything else
 * uses. */
static struct dentry* resolve_path(const char* path, struct dentry** out_parent,
                                   const char** out_last_name);
struct dentry* vfs_resolve(const char* path);
static struct dentry* resolve_path_ex(const char* path, struct dentry** out_parent,
                                      const char** out_last_name, int follow_last);
struct dentry* vfs_resolve_nofollow(const char* path) {
    return resolve_path_ex(path, NULL, NULL, 0);
}
struct dentry* vfs_resolve(const char* path) {
    return resolve_path(path, NULL, NULL);
}

/* §M73 — give a whole tree to one owner (in memory; a container's rootfs lives
 * on ramfs, which stores nothing).  Bounded depth: a tree deeper than this is
 * not a filesystem this kernel made. */
static void chown_walk(struct dentry* d, int uid, int gid, int depth) {
    if (!d || depth > 32) return;
    if (d->inode) { d->inode->owner_uid = uid; d->inode->owner_gid = gid; }
    for (struct dentry* c = d->children; c; c = c->sibling) chown_walk(c, uid, gid, depth + 1);
}
void vfs_chown_tree(struct dentry* d, int uid, int gid) { chown_walk(d, uid, gid, 0); }

/* ------------------------------------------------------------------- */
/* String helpers — no libc.                                            */
/* ------------------------------------------------------------------- */

/* THE NAMESPACE LOCK (2026-09-25).  The dentry tree — every parent's
 * `children` list, the mount table, attach/detach — was mutated by any task
 * with no lock, and `diskstorm` on 4 CPUs turned that into a livelock: four
 * tasks spinning in streq() over a sibling list that concurrent inserts and
 * removals had left circular.  Every public entry point that WALKS OR CHANGES
 * the tree takes this; it is recursive, so the VFS calling itself (vfs_copy ->
 * vfs_open, vfs_open -> vfs_create) is fine.  Deliberately NOT taken by
 * vfs_read / vfs_write: those only reach the file's own inode, and a blocking
 * device read (a terminal, /dev/dsp) must not stall every open on the machine.
 * Order: this, then a filesystem's own lock, then the block cache, then the
 * driver. */
static struct kmutex ns_lock = KMUTEX_INIT("vfs-namespace");
#define NS_LOCKED(T, call) ({ kmutex_lock(&ns_lock); T _r = (call); kmutex_unlock(&ns_lock); _r; })

static int streq_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 1;
    }
    return 1;
}
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t strlen_(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static void strcpy_n(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void memcpy_(void* dst, const void* src, size_t n) {
    char* d = (char*)dst; const char* s = (const char*)src;
    while (n--) *d++ = *s++;
}

/* ------------------------------------------------------------------- */
/* Init + fs registry.                                                  */
/* ------------------------------------------------------------------- */

void vfs_init(void) {
    if (root) return;                           /* idempotent */

    root = (struct dentry*)kcalloc(1, sizeof(struct dentry));
    if (!root) {
        kprintf("vfs: failed to allocate root dentry\n");
        return;
    }
    /* "/" by convention; parent NULL distinguishes the root. */
    root->name[0] = '/';
    root->name[1] = 0;
    root->parent  = NULL;
    root->inode   = NULL;       /* filled in by first mount */

    kprintf("vfs: initialized\n");
}

int vfs_register_fs(struct fs_type* fs) {
    if (!fs || !fs->name || !fs->mount) return -1;
    /* Refuse duplicate names. */
    for (struct fs_type* p = fs_types; p; p = p->next) {
        if (streq(p->name, fs->name)) return -2;
    }
    fs->next = fs_types;
    fs_types = fs;
    return 0;
}

/* ------------------------------------------------------------------- */
/* Path resolution.                                                     */
/* ------------------------------------------------------------------- */

/* Look up a single name component as a child of `parent`.  Returns the
 * matching dentry or NULL.  `name_len` excludes the trailing NUL/'/'.
 *
 * On cache miss, consults `parent->inode->dir_ops->lookup` (if any).
 * A successful lazy lookup is attached to the cache before returning,
 * so subsequent resolutions are O(1) over `parent->children`. */
static struct dentry* lookup_child(struct dentry* parent,
                                   const char* name, size_t name_len) {
    if (!parent) return NULL;
    /* Cache hit? */
    for (struct dentry* c = parent->children; c; c = c->sibling) {
        if (streq_n(c->name, name, name_len) && c->name[name_len] == 0) {
            return c;
        }
    }
    /* Cache miss — ask the fs to resolve, if it supports lazy lookup. */
    if (!parent->inode || !parent->inode->dir_ops ||
        !parent->inode->dir_ops->lookup) {
        return NULL;
    }
    /* Build a NUL-terminated copy of the component (lookup wants a C
     * string; the caller has only `name_len` bytes of `path` content). */
    char buf[VFS_NAME_MAX + 1];
    if (name_len > VFS_NAME_MAX) return NULL;
    for (size_t i = 0; i < name_len; i++) buf[i] = name[i];
    buf[name_len] = 0;

    struct inode* ino = NULL;
    if (parent->inode->dir_ops->lookup(parent->inode, buf, &ino) != 0) return NULL;
    if (!ino) return NULL;
    return vfs_attach_child(parent, buf, ino);
}

/* Walk a path, returning the dentry of the LAST component, or NULL on
 * any failure.  If `out_parent` is non-NULL, returns the parent dentry
 * of the last component (useful for create/mkdir). */
/* §M73 — a path made CANONICAL: a relative one joined to the task's working
 * directory, and "." / ".." applied LEXICALLY, with ".." at the root staying
 * at the root (chroot's rule — which is what makes a container's "/" a floor
 * and not a starting point).  Returns `path` itself when there is nothing to
 * do (the common case: absolute, no dot components), `buf` otherwise, NULL if
 * the result does not fit. */
/* §M90 — the dentry behind the CALLER's descriptor `fd` (a VFS file), or
 * NULL: what /proc/self/fd/N names.  usyscall.c owns the descriptor table. */
struct dentry* fd_vfs_dentry(int fd) __attribute__((weak));
struct dentry* fd_vfs_dentry(int fd) { (void)fd; return NULL; }
static int vfs_dentry_path_unlocked(struct dentry* d, char* out, size_t cap);

/* ------------------------------------------------------------------- */
/* §M90 — bind mounts and mount namespaces (see vfs.h).                 */
/* ------------------------------------------------------------------- */

/* The caller's mount namespace: task.c owns tasks, this file owns paths. */
int task_mntns_current(void) __attribute__((weak));
int task_mntns_current(void) { return 0; }

/* A namespace is (parent, the bind sequence number at its creation).  Ids are
 * never reused, so a bind's `bound_ns` can never come to mean another
 * namespace; 256 namespaces over a boot is far more than one container
 * runtime creates (one per container), and a full table REFUSES the unshare
 * rather than handing two containers one namespace. */
#define VFS_MAX_MNTNS 256
static struct { int parent; unsigned born; } g_ns[VFS_MAX_MNTNS] = { { -1, 0 } };
static int      g_ns_n = 1;                 /* id 0 = the machine's namespace */
static unsigned g_bind_seq;

#define VFS_MAX_BINDS 256
static struct { struct dentry* tgt; struct dentry* src; } g_binds[VFS_MAX_BINDS];
static int g_nbinds;

/* Is `d`'s bind visible to the caller?  Made in the caller's own namespace,
 * or in an ancestor of it BEFORE the descendant on the way down was created. */
static int bind_visible(const struct dentry* d) {
    int me = task_mntns_current();
    if (d->bound_ns == me) return 1;
    for (int n = me; n > 0 && n < g_ns_n; n = g_ns[n].parent) {
        if (g_ns[n].parent == d->bound_ns) return d->bound_seq < g_ns[n].born;
    }
    return 0;
}
/* Where a walk arriving at `d` really continues.  Bounded: a bind of a bind
 * of a bind is followed, a cycle is not (and cannot be built — vfs_bind
 * resolves its source through the binds first). */
static struct dentry* follow_bind(struct dentry* d) {
    for (int i = 0; d && d->bound && i < 8 && bind_visible(d); i++) d = d->bound;
    return d;
}

int vfs_mntns_new(int parent) {
    kmutex_lock(&ns_lock);
    int id = -1;
    if (g_ns_n < VFS_MAX_MNTNS && parent >= 0 && parent < g_ns_n) {
        id = g_ns_n++;
        g_ns[id].parent = parent;
        g_ns[id].born   = g_bind_seq;
    }
    kmutex_unlock(&ns_lock);
    return id;
}

static const char* vfs_canon_plain(const char* path, char* buf, size_t cap) {
    int dots = 0;
    for (const char* q = path; *q; q++)
        if (*q == '.' && (q == path || q[-1] == '/') &&
            (q[1] == 0 || q[1] == '/' || (q[1] == '.' && (q[2] == 0 || q[2] == '/'))))
            { dots = 1; break; }
    if (path[0] == '/' && !dots) return path;
    char tmp[256];
    size_t n = 0;
    if (path[0] != '/') {                       /* relative: from the working directory */
        const char* cwd = cred_fs_cwd();
        /* §M90 — a working directory that does not fit is a refusal, never a
         * truncation: a cut cwd joined to the name is ANOTHER path. */
        for (size_t i = 0; cwd[i]; i++) { if (n + 1 >= sizeof tmp) return NULL; tmp[n++] = cwd[i]; }
        if (n + 1 >= sizeof tmp) return NULL;
        tmp[n++] = '/';
    }
    for (size_t i = 0; path[i]; i++) { if (n + 1 >= sizeof tmp) return NULL; tmp[n++] = path[i]; }
    tmp[n] = 0;
    /* Walk the components into buf, popping on "..". */
    size_t o = 0;
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') { p = e; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 0 && buf[o - 1] != '/') o--;
            if (o > 0) o--;                     /* drop the slash too; at "/" it stays "/" */
            p = e; continue;
        }
        if (o + 1 + len + 1 > cap) return NULL;
        buf[o++] = '/';
        for (size_t i = 0; i < len; i++) buf[o++] = p[i];
        p = e;
    }
    if (o == 0) buf[o++] = '/';
    buf[o] = 0;
    return buf;
}

/* §M90 — /proc/self/fd/N[/rest] (and /proc/thread-self/…) is Linux's magic
 * link to descriptor N's file.  /proc has no per-process tree here, so it is
 * resolved in this one function every path passes through: replaced by that
 * file's path, `rest` kept.  It is applied to the CANONICAL path, after a
 * relative name has been joined to its directory — Go's os.Root opens
 * "/proc/self/fd" as a directory and then chmods "22" relative to it, so a
 * check on the raw argument alone never sees the link.  1 if `out` holds the
 * rewritten path. */
/* §M90 — the program behind /proc/self/exe (or the /proc/<pid> target's), as a
 * path; "" when unknown.  task.c; weak so a build without tasks links. */
const char* task_proc_exe(void) __attribute__((weak));
const char* task_proc_exe(void) { return ""; }
static int vfs_fd_magic(const char* path, char* out, size_t cap) {
    /* §M90 — /proc/self/exe OPENS the running program, as on Linux (readlink
     * of it was answered since §M89, opening it was ENOENT).  runc re-executes
     * itself through it to start a container's init. */
    {
        const char* e = "/proc/self/exe";
        size_t i = 0;
        while (e[i] && path[i] == e[i]) i++;
        if (!e[i] && !path[i]) {
            const char* x = task_proc_exe();
            if (!x || !x[0]) return 0;
            size_t o = 0;
            for (; x[o] && o + 1 < cap; o++) out[o] = x[o];
            out[o] = 0;
            return 1;
        }
    }
    static const char* const pre[2] = { "/proc/self/fd/", "/proc/thread-self/fd/" };
    for (int w = 0; w < 2; w++) {
        size_t i = 0;
        while (pre[w][i] && path[i] == pre[w][i]) i++;
        if (pre[w][i]) continue;
        int fd = 0, digits = 0;
        while (path[i] >= '0' && path[i] <= '9') { fd = fd * 10 + (path[i++] - '0'); digits++; }
        if (!digits || (path[i] && path[i] != '/')) return 0;
        struct dentry* d = fd_vfs_dentry(fd);
        if (!d) return 0;
        char m[256];
        if (vfs_dentry_path_unlocked(d, m, sizeof m) != 0) return 0;
        size_t o = 0;
        for (; m[o] && o + 1 < cap; o++) out[o] = m[o];
        if (o == 0 && o + 1 < cap) out[o++] = '/';
        for (; path[i] && o + 1 < cap; i++) out[o++] = path[i];
        out[o] = 0;
        return 1;
    }
    return 0;
}

/* §M90 — /proc has no per-process tree of dentries; every spelling maps onto
 * /proc/self, and for ANOTHER process the target is recorded so procfs
 * generates for it:
 *   /proc/thread-self/X               -> /proc/self/X
 *   /proc/self/task/<tid>/X           -> /proc/self/X
 *   /proc/<own pid>[/task/<tid>]/X    -> /proc/self/X  (Go names its own
 *                                        threads this way: netns.Get)
 *   /proc/<other pid>[/task/<tid>]/X  -> /proc/self/X, target = <other pid>,
 *                                        for ns/… and the per-process files in
 *                                        proc_foreign_ok (status, cgroup,
 *                                        oom_score_adj, oom_score, exe)
 * Another process's fd table or mountinfo stay unanswered — a wrong answer
 * (the caller's own, under another pid) is worse than a missing one.
 * 1 if `out` holds the rewrite. */
int task_tgid_current(void) __attribute__((weak));
int task_tgid_current(void) { return -1; }
/* §M90 — /proc/<another pid>/X: the alias below maps it onto /proc/self/X and
 * records WHICH process on the calling task (task.c), for procfs to generate
 * from at open; every /proc path resets it first, so it never outlives the
 * lookup that set it.  Weak defaults for a build without the task layer. */
void task_set_proc_target(int pid) __attribute__((weak));
void task_set_proc_target(int pid) { (void)pid; }
int  task_tgid_alive(int pid) __attribute__((weak));
int  task_tgid_alive(int pid) { (void)pid; return 0; }
/* The per-process files that answer for ANOTHER process.  The rest of
 * /proc/self (fd/, mountinfo, …) is the caller's own view and is not offered
 * under a foreign pid — better ENOENT than the caller's data under another
 * process's name. */
static int proc_foreign_ok(const char* rest) {
    static const char* const ok[] = { "oom_score_adj", "oom_score", "status", "cgroup", "exe" };
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        const char* a = ok[i]; const char* b = rest;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && (*b == 0 || *b == '/')) return 1;
    }
    return 0;
}
static int vfs_proc_alias(const char* c, char* out, size_t cap) {
    const char* pre = "/proc/";
    size_t i = 0;
    while (pre[i] && c[i] == pre[i]) i++;
    if (pre[i]) return 0;
    /* §M90 — the target is NOT reset here: a path is canonicalised more than
     * once on its way through the VFS, and the second pass sees this alias's
     * own output ("/proc/self/…") — resetting then erased the target the
     * first pass set.  Its lifetime is bounded by the call instead: every
     * Linux syscall starts with it cleared (abi_dispatch), and procfs clears
     * it when an open consumes it. */
    const char* rest = NULL;
    int own = 0;                                    /* the caller's own process */
    long fpid = 0;                                  /* a foreign process's pid */
    const char* ts = "thread-self/";
    size_t j = 0;
    while (ts[j] && c[i + j] == ts[j]) j++;
    if (!ts[j]) { rest = c + i + j; own = 1; }
    size_t k = i;
    if (!rest) {
        const char* sf = "self/";
        j = 0;
        while (sf[j] && c[i + j] == sf[j]) j++;
        if (!sf[j]) { k = i + j; own = 1; }
        else {
            long pid = 0; size_t d = 0;
            while (c[k] >= '0' && c[k] <= '9') { pid = pid * 10 + (c[k] - '0'); k++; d++; }
            if (!d || c[k] != '/') return 0;
            k++;
            own = pid == task_tgid_current();
            if (!own) fpid = pid;
            else task_set_proc_target(0);           /* /proc/<own pid>: the caller */
        }
        /* an optional "task/<tid>/" */
        const char* tk = "task/";
        j = 0;
        while (tk[j] && c[k + j] == tk[j]) j++;
        if (!tk[j]) {
            size_t m = k + j, d = 0;
            while (c[m] >= '0' && c[m] <= '9') { m++; d++; }
            if (d && c[m] == '/') k = m + 1;
            else if (d && c[m] == 0) return 0;      /* the thread directory itself */
        }
        if (k == i + 5 && own && c[k - 1] == '/' && c[i] == 's') return 0;   /* plain /proc/self/X */
        int is_ns = c[k] == 'n' && c[k + 1] == 's' && (c[k + 2] == '/' || c[k + 2] == 0);
        if (!own) {
            /* Another process's file: its handles (ns/…) or a per-process file
             * that answers for it — and then procfs must generate for THAT
             * process, not for the caller (a namespace handle of another pid
             * used to name the caller's own namespace). */
            if (!fpid || !task_tgid_alive((int)fpid)) return 0;
            if (!is_ns && !proc_foreign_ok(c + k)) return 0;
            task_set_proc_target((int)fpid);
        }
        rest = c + k;
    }
    const char* self = "/proc/self/";
    size_t o = 0;
    for (; self[o] && o + 1 < cap; o++) out[o] = self[o];
    for (; *rest && o + 1 < cap; rest++) out[o++] = *rest;
    while (o > 1 && out[o - 1] == '/') o--;
    out[o] = 0;
    return 1;
}

static const char* vfs_canon(const char* path, char* buf, size_t cap) {
    const char* c = vfs_canon_plain(path, buf, cap);
    if (!c) return NULL;
    /* §M90 — "dir/" names "dir" (POSIX).  The fast branch hands back an
     * absolute, dot-free path untouched, slash and all, and the walk then met
     * an EMPTY last component: stat("…/") failed, so Go's MkdirAll — stat,
     * else mkdir, else stat again — answered EEXIST for a directory that was
     * there all along (docker's container start). */
    {
        size_t n = 0;
        while (c[n]) n++;
        if (n > 1 && c[n - 1] == '/') {
            if (c != buf) {
                if (n + 1 > cap) return NULL;
                for (size_t i = 0; i <= n; i++) buf[i] = c[i];
                c = buf;
            }
            while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
        }
    }
    char m[256];
    {
        char a[256];
        if (vfs_proc_alias(c, a, sizeof a)) {
            size_t n = 0;
            while (a[n]) n++;
            if (n + 1 > cap) return NULL;
            for (size_t i = 0; i <= n; i++) buf[i] = a[i];
            c = buf;
        }
    }
    if (!vfs_fd_magic(c, m, sizeof m)) return c;
    c = vfs_canon_plain(m, buf, cap);
    if (!c) return NULL;
    if (c == m) {                                   /* already canonical: lives in m */
        size_t n = 0;
        while (m[n]) n++;
        if (n + 1 > cap) return NULL;
        for (size_t i = 0; i <= n; i++) buf[i] = m[i];
        c = buf;
    }
    return c;
}

/* §M90 — canonical WITHOUT resolving a /proc/self/fd/N magic link: for
 * readlink, whose whole question is what that link names.  The /proc aliases
 * still apply (thread-self, task/<tid>, <pid>/ns). */
int vfs_canonical_link(const char* path, char* out, size_t cap) {
    if (!path || !*path || !out) return -1;
    const char* c = vfs_canon_plain(path, out, cap);
    if (!c) return -1;
    char a[256];
    if (vfs_proc_alias(c, a, sizeof a)) c = a;
    if (c != out) {
        size_t n = strlen_(c);
        if (n + 1 > cap) return -1;
        memcpy_(out, c, n + 1);
    }
    return 0;
}

int vfs_canonical(const char* path, char* out, size_t cap) {
    if (!path || !*path || !out) return -1;
    const char* c = vfs_canon(path, out, cap);
    if (!c) return -1;
    if (c != out) { size_t n = strlen_(c); if (n + 1 > cap) return -1; memcpy_(out, c, n + 1); }
    return 0;
}

/* §M89 — the target a symbolic link holds, NUL-terminated.  -1 if `d` is not
 * a link or cannot be read. */
static int link_target(struct dentry* d, char* out, size_t cap) {
    if (!d || !d->inode || d->inode->type != INODE_SYMLINK || !d->inode->ops ||
        !d->inode->ops->read || cap < 2)
        return -1;
    struct file tmp = { d->inode, d, 0, 0, 0, 0 };   /* never closed: no magic */
    ssize_t n = d->inode->ops->read(&tmp, out, cap - 1, 0);
    if (n <= 0) return -1;
    out[n] = 0;
    return (int)n;
}

/* §M89 — rewrite a CANONICAL absolute path so that it contains no symbolic
 * link (the last component excepted when !follow_last).  Textual, one link at
 * a time, re-canonicalising after each splice: an absolute target restarts
 * from this task's "/" (so a link inside a container resolves inside the
 * container, as chroot requires), a relative one is taken from the link's own
 * directory.  A component that does not exist ends the walk — the rest cannot
 * contain a link, and a create needs exactly that path.  40 hops, then NULL:
 * a loop is refused, never followed forever. */
static const char* expand_links(const char* path, char* buf, size_t cap, int follow_last) {
    char a[256], t[256], b[512];
    size_t n = strlen_(path);
    if (n + 1 > sizeof a) return NULL;
    memcpy_(a, path, n + 1);
    struct dentry* base = cred_fs_root();
    if (!base) base = root;
    for (int hops = 0; hops <= 40; hops++) {
        struct dentry* cur = base;
        const char* p = a + 1;
        int spliced = 0;
        while (*p) {
            const char* slash = p;
            while (*slash && *slash != '/') slash++;
            size_t len = (size_t)(slash - p);
            int last = (*slash == 0);
            struct dentry* d = lookup_child(cur, p, len);
            if (!d) break;
            if (d->inode && d->inode->type == INODE_SYMLINK && (!last || follow_last)) {
                if (link_target(d, t, sizeof t) < 0) return NULL;
                size_t o = 0;
                if (t[0] != '/') {                    /* relative: the link's directory */
                    size_t pre = (size_t)(p - a);     /* "/dir/.../" including the slash */
                    memcpy_(b, a, pre); o = pre;
                }
                size_t tl = strlen_(t);
                memcpy_(b + o, t, tl); o += tl;
                size_t rl = strlen_(slash);
                if (o + rl + 1 > sizeof b) return NULL;
                memcpy_(b + o, slash, rl + 1);
                char c2[256];
                const char* cc = vfs_canon(b, c2, sizeof c2);
                if (!cc) return NULL;
                size_t cl = strlen_(cc);
                if (cl + 1 > sizeof a) return NULL;
                memcpy_(a, cc, cl + 1);
                spliced = 1;
                break;
            }
            if (last) break;
            if (!d->inode || d->inode->type != INODE_DIR) break;
            cur = d;
            p = slash + 1;
        }
        if (!spliced) {
            size_t al = strlen_(a);
            if (al + 1 > cap) return NULL;
            memcpy_(buf, a, al + 1);
            return buf;
        }
    }
    return NULL;                                      /* ELOOP */
}

static struct dentry* resolve_path_ex(const char* path, struct dentry** out_parent,
                                      const char** out_last_name, int follow_last);
static struct dentry* resolve_path(const char* path,
                                   struct dentry** out_parent,
                                   const char**    out_last_name) {
    return resolve_path_ex(path, out_parent, out_last_name, 1);
}

static struct dentry* resolve_path_ex(const char* path,
                                      struct dentry** out_parent,
                                      const char**    out_last_name,
                                      int             follow_last) {
    if (!path || !path[0]) return NULL;
    /* §M73 — relative paths and dot components, made canonical first.  When
     * that rewrote the path, *out_last_name would point into this frame's
     * buffer, so it is reported only as "there is a last name" (the one caller
     * that asks, vfs_open, uses it for exactly that). */
    char cb[256], eb[256];
    const char* orig = path;
    path = vfs_canon(path, cb, sizeof cb);
    if (!path || path[0] != '/') return NULL;
    /* §M89 — symbolic links, spliced out before the walk. */
    {
        const char* ex = expand_links(path, eb, sizeof eb, follow_last);
        if (!ex) return NULL;
        size_t a = strlen_(ex), b = strlen_(path);
        if (a != b || !streq_n(ex, path, a)) path = ex;
    }
    if (path != orig && out_last_name) {
        /* the canonical, link-free path takes the fast branch below when it
         * comes back here (expanding it again finds nothing to splice) */
        struct dentry* r = resolve_path_ex(path, out_parent, NULL, follow_last);
        *out_last_name = (out_parent && *out_parent) ? "" : NULL;
        return r;
    }
    /* §M73 — "/" is the calling task's root: the machine's, or its
     * container's.  The only place a path starts, so the only place this has
     * to be decided. */
    struct dentry* base = cred_fs_root();
    if (!base) base = root;
    if (path[1] == 0) {                         /* exactly "/" */
        if (out_parent)    *out_parent    = NULL;
        if (out_last_name) *out_last_name = NULL;
        return base;
    }

    struct dentry* cur = base;
    const char* p = path + 1;                   /* skip leading '/' */
    const char* last_start = p;

    for (;;) {
        /* Find the next '/' or end-of-string. */
        const char* slash = p;
        while (*slash && *slash != '/') slash++;
        size_t comp_len = (size_t)(slash - p);
        if (comp_len == 0 || comp_len > VFS_NAME_MAX) return NULL;
        last_start = p;

        if (*slash == 0) {
            /* Last component — return parent + name without descending. */
            if (out_parent)    *out_parent    = cur;
            if (out_last_name) *out_last_name = last_start;
            return follow_bind(lookup_child(cur, last_start, comp_len));
        }

        /* Intermediate component — must exist and be a directory. */
        struct dentry* nxt = follow_bind(lookup_child(cur, p, comp_len));
        if (!nxt || !nxt->inode || nxt->inode->type != INODE_DIR) return NULL;
        cur = nxt;
        p = slash + 1;
    }
}

/* ------------------------------------------------------------------- */
/* Tree manipulation — used by filesystems and create/mkdir helpers.    */
/* ------------------------------------------------------------------- */

static struct dentry* vfs_attach_child_unlocked(struct dentry* parent, const char* name,
                                struct inode* inode) {
    if (!parent || !name || !inode) return NULL;

    struct dentry* d = (struct dentry*)kcalloc(1, sizeof(struct dentry));
    if (!d) return NULL;
    strcpy_n(d->name, name, sizeof d->name);
    d->inode    = inode;
    d->parent   = parent;
    d->sibling  = parent->children;             /* push to head */
    parent->children = d;
    return d;
}
struct dentry* vfs_attach_child(struct dentry* parent, const char* name, struct inode* inode) {
    return NS_LOCKED(struct dentry*, vfs_attach_child_unlocked(parent, name, inode));
}

/* §M87 — take `name` out of `parent`'s directory listing WITHOUT freeing it.
 *
 * For a device that goes away (a removed RAM disk): its /dev entry must stop
 * being findable, but a task may still hold it open, and both `struct file`
 * fields point into it (`inode`, and `dentry`, which vfs_close walks to find
 * the mount).  Freeing either would turn that close into a use-after-free.
 * So the dentry is unlinked and ORPHANED: its parent pointer stays (the walk
 * still ends at the right mount), its memory stays, and the caller gets the
 * inode back to neuter.  A few hundred bytes per removed device, on purpose. */
struct inode* vfs_orphan_child(struct dentry* parent, const char* name) {
    struct inode* ino = NULL;
    kmutex_lock(&ns_lock);
    struct dentry** pp = parent ? &parent->children : NULL;
    while (pp && *pp) {
        if (streq((*pp)->name, name)) {
            struct dentry* d = *pp;
            *pp = d->sibling;
            d->sibling = NULL;
            ino = d->inode;
            break;
        }
        pp = &(*pp)->sibling;
    }
    kmutex_unlock(&ns_lock);
    return ino;
}

/* ------------------------------------------------------------------- */
/* Mount.                                                               */
/* ------------------------------------------------------------------- */

/* §M32 stage 5 — WHAT IS MOUNTED WHERE.
 *
 * The VFS had no mount record at all: `vfs_mount` filled in a dentry and
 * forgot everything else, so nothing could answer "which filesystem is this
 * path on" — which is exactly the question ownership-persistence asks.  A
 * small static table rather than a list: mounts are few, they are never
 * removed today (there is no umount), and an allocation here would put a
 * failure path in the middle of boot. */
#define VFS_MAX_MOUNTS 8
static struct vfs_mount g_mounts[VFS_MAX_MOUNTS];
static int g_nmounts;

/* §M87 — the mount a dentry lives under: the nearest ancestor (or itself)
 * that is a mountpoint.  Walked upward, so the innermost mount wins, which is
 * the right answer for a nested mount.  Caller holds ns_lock. */
static struct vfs_mount* mount_of_dentry(struct dentry* d) {
    for (; d; d = d->parent) {
        for (int i = 0; i < g_nmounts; i++)
            if (g_mounts[i].mp == d) return &g_mounts[i];
        if (d->parent == d) break;
    }
    return NULL;
}

static int vfs_mount_unlocked(const char* fs_name, const char* path, const char* dev_name) {
    if (!fs_name || !path) return -1;

    /* Find the fs implementation. */
    struct fs_type* fs = NULL;
    for (struct fs_type* p = fs_types; p; p = p->next) {
        if (streq(p->name, fs_name)) { fs = p; break; }
    }
    if (!fs) {
        kprintf("vfs_mount: fs %s not registered\n", fs_name);
        return -2;
    }

    /* Resolve the optional backing block device. */
    struct block_device* bdev = NULL;
    if (dev_name) {
        bdev = blk_find(dev_name);
        if (!bdev) {
            klog(KLOG_WARN, "vfs", "mount: block device %s not found\n", dev_name);
            return -5;
        }
    }

    /* Resolve the mountpoint dentry. */
    struct dentry* mp;
    if (streq(path, "/")) {
        mp = root;
    } else {
        mp = resolve_path(path, NULL, NULL);
        if (!mp) {
            kprintf("vfs_mount: mountpoint %s not found\n", path);
            return -3;
        }
    }

    /* Detach any placeholder inode on a nested mountpoint so the fs can
     * install its own root.  The root ("/") still hands the fs a blank
     * dentry (vfs_init left mp->inode NULL there).  The previous inode
     * is leaked for now — ramfs bootstrap directories carry no payload,
     * and a proper umount path is a later milestone. */
    /* §M87 — no longer leaked: remembered, and restored by umount.  Also
     * refuse a second mount on the SAME dentry — the first one's tree would
     * be silently shadowed and could then never be unmounted. */
    for (int k = 0; k < g_nmounts; k++)
        if (g_mounts[k].mp == mp && mp != root) {
            kprintf("vfs_mount: %s is already a mountpoint\n", path);
            return -6;
        }
    struct inode*  covered = (mp != root) ? mp->inode : NULL;
    struct dentry* covered_children = (mp != root) ? mp->children : NULL;
    if (mp != root) { mp->inode = NULL; mp->children = NULL; }

    /* Hand off to the fs to fill in the mountpoint. */
    int r = fs->mount(bdev, mp);
    if (r != 0) {
        kprintf("vfs_mount: %s->mount() failed: %d\n", fs_name, r);
        if (mp != root) { mp->inode = covered; mp->children = covered_children; }
        return r;
    }
    /* Record it.  The ownership declaration is COPIED from the fs_type rather
     * than looked up later, so a mount answers for the implementation it was
     * actually made with. */
    if (g_nmounts < VFS_MAX_MOUNTS) {
        struct vfs_mount* m = &g_mounts[g_nmounts++];
        int i = 0;
        for (; path[i] && i < (int)sizeof m->path - 1; i++) m->path[i] = path[i];
        m->path[i] = 0;
        m->fs_name         = fs->name;
        m->stores_ownership = fs->stores_ownership;
        /* §M87 — enough to undo it: the device, the dentry, and what the
         * mount covered (umount puts the placeholder directory back instead
         * of leaving a mountpoint with no inode, which every later lookup
         * would treat as a missing path). */
        i = 0;
        for (; dev_name && dev_name[i] && i < (int)sizeof m->dev_name - 1; i++)
            m->dev_name[i] = dev_name[i];
        m->dev_name[i]      = 0;
        m->open_files       = 0;
        m->hold             = NULL;
        m->mp               = mp;
        m->covered_inode    = covered;
        m->covered_children = covered_children;
        m->fs               = fs;
    }

    if (dev_name) kprintf("vfs: mounted %s (%s) at %s\n", fs_name, dev_name, path);
    else          kprintf("vfs: mounted %s at %s\n", fs_name, path);
    return 0;
}
int vfs_mount(const char* fs_name, const char* path, const char* dev_name) {
    return NS_LOCKED(int, vfs_mount_unlocked(fs_name, path, dev_name));
}

/* ------------------------------------------------------------------- */
/* Open / read / write / close / readdir / mkdir / create.              */
/* ------------------------------------------------------------------- */

static struct file* vfs_open_unlocked(const char* path, int flags) {
    struct dentry*  parent;
    const char*     last;
    struct dentry*  d = resolve_path(path, &parent, &last);

    if (!d) {
        if ((flags & VFS_CREATE) == 0) return NULL;
        if (!parent || !last)          return NULL;
        if (vfs_create(path) != 0)     return NULL;
        d = resolve_path(path, NULL, NULL);
        if (!d) return NULL;
    }

    if (!d->inode) return NULL;

    /* §M32 stage 5 — read and/or write against the FILE, derived from the
     * flags the caller actually asked for.  A caller that opens read-only must
     * not be refused because it lacks write, and one that opens for writing
     * must not sneak past on the read bits. */
    {
        int want = 0;
        if (flags & VFS_RDONLY) want |= VFS_PERM_READ;
        if (flags & VFS_WRONLY) want |= VFS_PERM_WRITE;
        if (flags & VFS_TRUNC)  want |= VFS_PERM_WRITE;
        /* VFS_RDWR is both bits, so it correctly demands both.  A caller that
         * passes no mode bits at all is reading — several in this tree do, and
         * silently granting them nothing would refuse every one of them. */
        if (want == 0) want = VFS_PERM_READ;
        if (!vfs_permitted(d->inode, want)) {
            /* A REFUSAL MUST NAME ITS REASON.  vfs_open can only return NULL,
             * so the caller prints "open failed" — which reads as a broken file
             * and sends the user looking for the wrong problem.  The reason
             * goes to klog rather than the console because an open is also how
             * code TESTS for existence, and a probe that printed a denial on
             * screen would be noise on a healthy machine. `dmesg` has it. */
            klog(KLOG_INFO, "vfs",
                 "%s: permission denied for uid %d (wanted %s%s, mode %d%d%d%d, owner uid %d)\n",
                 path, cred_uid(cred_current()),
                 (want & VFS_PERM_READ) ? "r" : "",
                 (want & VFS_PERM_WRITE) ? "w" : "",
                 (int)((d->inode->mode >> 9) & 7), (int)((d->inode->mode >> 6) & 7),
                 (int)((d->inode->mode >> 3) & 7), (int)(d->inode->mode & 7),
                 d->inode->owner_uid);
            return NULL;
        }
    }

    /* VFS_TRUNC: logically empty the file by zeroing its size.  The
     * underlying buffer (if any) is left allocated — subsequent writes
     * overwrite from the start, and the next save round-trip is clean. */
    if ((flags & VFS_TRUNC) && d->inode->type == INODE_FILE) {
        d->inode->size = 0;
        pcache_invalidate(d->inode);             /* §M74 — its pages are gone */
    }

    struct file* f = (struct file*)kcalloc(1, sizeof(struct file));
    if (!f) return NULL;
    f->inode  = d->inode;
    f->dentry = d;
    f->flags  = flags;
    f->pos    = 0;
    f->magic  = VFS_FILE_MAGIC;
    d->inode->opens++;                       /* §M90 — see vfs_close / unlink */
    struct vfs_mount* m = mount_of_dentry(d);
    if (m) m->open_files++;
    return f;
}
struct file* vfs_open(const char* path, int flags) {
    struct file* f = NS_LOCKED(struct file*, vfs_open_unlocked(path, flags));
    /* §M90 — the fs's open hook, outside the lock (a generator may walk the
     * namespace itself: /proc/self/mountinfo). */
    if (f && f->inode && f->inode->ops && f->inode->ops->open) f->inode->ops->open(f);
    return f;
}

int vfs_close(struct file* f) {
    if (!f) return -1;
    if (f->magic != VFS_FILE_MAGIC) {
        kprintf("!! vfs_close: %s struct file %p (magic %x inode %p) from %p\n",
                f->magic == VFS_FILE_DEAD ? "ALREADY CLOSED" : "CORRUPT",
                (void*)f, f->magic, (void*)f->inode, __builtin_return_address(0));
        return -1;                       /* never free what is not ours to free */
    }
    f->magic = VFS_FILE_DEAD;
    if (f->inode && f->inode->ops && f->inode->ops->close) f->inode->ops->close(f);
    /* §M87 — the mount is found again from the dentry rather than remembered
     * in the file: mount records move when one is removed, and a pointer kept
     * across that would decrement somebody else's count. */
    kmutex_lock(&ns_lock);
    /* §M90 — the dentry may be gone (the file was unlinked while open): the
     * mount is then not found from it, and its count is left as it was. */
    struct vfs_mount* m = f->dentry && f->inode && !f->inode->unlink_dir
                        ? mount_of_dentry(f->dentry) : NULL;
    if (m && m->open_files > 0) m->open_files--;
    /* §M90 — the LAST close of an unlinked file performs the removal that
     * vfs_unlink deferred: until now its inode (and, on ramfs, its pages)
     * had to stay valid for this open file. */
    struct inode* ino = f->inode;
    if (ino && ino->opens > 0 && --ino->opens == 0 && ino->unlink_dir) {
        struct inode* dir = ino->unlink_dir;
        char* nm = ino->unlink_name;
        ino->unlink_dir = NULL; ino->unlink_name = NULL;
        pcache_invalidate(ino);
        if (dir->dir_ops && dir->dir_ops->unlink) dir->dir_ops->unlink(dir, nm ? nm : "", ino);
        if (nm) kfree(nm);
    }
    kmutex_unlock(&ns_lock);
    kfree(f);
    return 0;
}

ssize_t vfs_read(struct file* f, void* buf, size_t n) {
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->read) return -1;
    ssize_t r = f->inode->ops->read(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    return r;
}

ssize_t vfs_write(struct file* f, const void* buf, size_t n) {
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->write) return -1;
    uint64_t at = f->pos;
    ssize_t r = f->inode->ops->write(f, buf, n, f->pos);
    if (r > 0) f->pos += (uint64_t)r;
    /* §M74/§M90 — the file changed: its cached pages are brought up to date in
     * place, so mappings of them see the write (pcache_update).  After the
     * write and outside the filesystem's lock (the cache reads through the
     * filesystem, so the order is always cache -> fs, never the reverse). */
    if (r > 0 && f->inode->pc_id) pcache_update(f->inode, at, buf, (size_t)r);
    return r;
}

static int vfs_readdir_unlocked(struct file* f, struct dirent* out) {
    if (!f || !f->inode || !f->inode->ops || !f->inode->ops->readdir) return -1;
    return f->inode->ops->readdir(f, out);
}
int vfs_readdir(struct file* f, struct dirent* out) {
    return NS_LOCKED(int, vfs_readdir_unlocked(f, out));
}

/* Split a path into "parent dir path" and "last component".  Caller
 * provides a buffer for the parent path (mutable copy). */
static int split_parent(const char* path, char* parent_buf, size_t cap,
                        const char** last_out) {
    /* §M73 — canonical first (relative → from the working directory; "." and
     * ".." applied), INTO parent_buf, which then holds "parent\0last". */
    if (cap < 4) return -1;
    const char* c = vfs_canon(path, parent_buf + 1, cap - 1);
    if (!c) return -1;
    size_t len = strlen_(c);
    if (len == 0 || len + 2 > cap) return -1;
    if (c != parent_buf + 1) memcpy_(parent_buf + 1, c, len + 1);
    char* s = parent_buf + 1;                   /* the canonical path */
    /* §M90 — "dir/" names the same entry as "dir" (POSIX): mkdir("a/b/") is
     * how Go's MkdirAll spells its last step, and the trailing slash used to
     * leave an empty last name — ENOENT for a directory that was about to be
     * created.  The canonicaliser returns an absolute, dot-free path as is,
     * so the slash survives to here. */
    while (len > 1 && s[len - 1] == '/') s[--len] = 0;

    int last_slash = -1;
    for (size_t i = 0; i < len; i++) if (s[i] == '/') last_slash = (int)i;
    if (last_slash < 0 || s[last_slash + 1] == 0) return -1;   /* "/" has no last name */

    if (last_slash == 0) {
        /* "/name": the parent is "/", so the layout is "/\0name" — which is
         * the canonical string shifted left by one with its slash kept. */
        parent_buf[0] = '/';
        parent_buf[1] = 0;
        *last_out = s + 1;                      /* name still sits after it */
    } else {
        for (int i = 0; i < last_slash; i++) parent_buf[i] = s[i];
        parent_buf[last_slash] = 0;
        /* the name: copy it just past the parent's terminator */
        const char* nm = s + last_slash + 1;
        size_t nl = strlen_(nm);
        char* dst = parent_buf + last_slash + 1;
        for (size_t i = 0; i <= nl; i++) dst[i] = nm[i];   /* forward copy: dst <= nm */
        *last_out = dst;
    }
    return 0;
}

/* Dispatch a namespace mutator to the parent inode's dir_ops.  Returns
 * 0 on success.  Walks the path, attaches the freshly-created child
 * inode (returned by the fs) under the parent dentry. */
/* `kind`: 0 a regular file, 1 a directory, 2 a FIFO (§M90 — made by the fs's
 * `create` and turned into INODE_FIFO here: the fs stores nothing for it). */
static int vfs_mutator_unlocked(const char* path, int kind);
static int vfs_mutator(const char* path, int kind) {
    return NS_LOCKED(int, vfs_mutator_unlocked(path, kind));
}
static int vfs_mutator_unlocked(const char* path, int kind) {
    int is_dir = kind == 1;
    char buf[256];
    const char* last;
    if (split_parent(path, buf, sizeof buf, &last) != 0) return -1;
    if (!*last) return -1;
    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return -1;
    if (!parent->inode->dir_ops) return -1;
    /* §M32 — creating a NAME changes the DIRECTORY, so the check is write on
     * the parent, not on the thing being created (which does not exist yet). */
    if (!vfs_permitted(parent->inode, VFS_PERM_WRITE)) return -5;

    int (*op)(struct inode*, const char*, struct inode**) =
        is_dir ? parent->inode->dir_ops->mkdir
               : parent->inode->dir_ops->create;
    if (!op) return -1;
    /* §M90 — a FIFO only where nodes live in memory.  A block-backed
     * filesystem would hand it back after a reboot as an empty regular file
     * (exFAT has no node type to store), so the honest answer is a refusal. */
    if (kind == 2) {
        struct vfs_mount* m = mount_of_dentry(parent);
        if (m && m->dev_name[0]) return -3;
    }

    /* Refuse a duplicate name up front.  The fs may also enforce this
     * (and should, for races once SMP lands), but checking here keeps
     * the error surface uniform regardless of fs. */
    for (struct dentry* c = parent->children; c; c = c->sibling) {
        if (streq(c->name, last)) return -2;
    }

    struct inode* ino = NULL;
    int r = op(parent->inode, last, &ino);
    if (r != 0 || !ino) return r ? r : -3;
    if (kind == 2) { ino->type = INODE_FIFO; ino->size = 0; }

    /* §M32 — A NEW FILE BELONGS TO WHOEVER MADE IT.
     *
     * `vfs_inode_defaults` stamps root:root, which is the right answer for an
     * inode a FILESYSTEM synthesises (a devfs node, procfs's synthetic files,
     * an exFAT entry read off a volume that stores no owner) and the wrong one
     * for an inode a USER just created.
     *
     * Found by driving it rather than by reading it: logged in as alice, a
     * `write /home/alice/mine` created the file — so the parent-directory check
     * had passed — and was then refused the WRITE to the thing it had just
     * made, because the new inode was root's.  *A user who cannot write inside
     * their own home is not a permission model, it is a broken one*, and no
     * amount of staring at the defaults would have shown it: every path in the
     * test suite ran as SYSTEM, which bypasses the check entirely. */
    {
        const struct cred* c = cred_current();
        if (c->owner == TASK_OWNER_USER) {
            ino->owner_uid = cred_uid(c);
            ino->owner_gid = cred_gid(c);
            /* ...and on a filesystem that stores ownership, the disk has to
             * hear it too, or the file is the user's until the next mount. */
            if (parent->inode->dir_ops && parent->inode->dir_ops->setattr &&
                parent->inode->dir_ops->setattr(parent->inode, last, ino) != 0)
                klog(KLOG_WARN, "vfs", "%s: owner not stored on the volume\n", last);
        }
    }

    if (!vfs_attach_child(parent, last, ino)) return -4;
    return 0;
}

int vfs_create(const char* path) { return vfs_mutator(path, 0); }
int vfs_mkdir (const char* path) { return vfs_mutator(path, 1); }
int vfs_mkfifo(const char* path) { return vfs_mutator(path, 2); }

/* Remove `path` (file or empty dir).  The fs op frees the inode; we
 * then detach + free the dentry.  Mount roots refuse removal because
 * their parent belongs to a different fs (dir_ops mismatch would
 * corrupt the foreign inode's accounting). */
static int vfs_unlink_unlocked(const char* path) {
    char buf[256];
    const char* last;
    if (split_parent(path, buf, sizeof buf, &last) != 0) return -1;
    if (!*last) return -1;

    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return -1;
    if (!parent->inode->dir_ops || !parent->inode->dir_ops->unlink)    return -1;
    /* Removing a name is a write to the DIRECTORY.  Which is why a file you
     * cannot write can still be deleted if you own the directory it is in —
     * surprising the first time, and exactly what POSIX specifies. */
    if (!vfs_permitted(parent->inode, VFS_PERM_WRITE)) return -5;

    /* Find the child dentry + keep the link BEFORE it for splicing. */
    struct dentry** link = &parent->children;
    struct dentry*  d    = parent->children;
    while (d && !streq(d->name, last)) { link = &d->sibling; d = d->sibling; }
    if (!d || !d->inode) return -1;
    if (d->bound) return -6;          /* §M90 — a bind's target: busy (EBUSY) */
    if (d->inode->type == INODE_DIR && d->children &&
        !(d->inode->vflags & VFS_IF_OWN_CHILDREN)) return -2;   /* not empty */
    if (d->inode->type == INODE_DEVICE) return -1;               /* devfs nodes */

    /* §M90 — STILL OPEN: the name goes now, the file when its last opener
     * closes it (vfs_close).  Freeing it here left every open descriptor
     * pointing at freed memory — a daemon that removes a file it still holds
     * (or renames a new one over it) then crashed the kernel at close. */
    if (d->inode->type != INODE_DIR && d->inode->opens > 0) {
        size_t nl = strlen_(last);
        char* nm = (char*)kmalloc(nl + 1);
        if (!nm) return -1;
        for (size_t i = 0; i <= nl; i++) nm[i] = last[i];
        d->inode->unlink_dir  = parent->inode;
        d->inode->unlink_name = nm;
        *link = d->sibling;
        kfree(d);
        return 0;
    }

    pcache_invalidate(d->inode);                 /* §M74 — before the inode goes */
    int r = parent->inode->dir_ops->unlink(parent->inode, last, d->inode);
    if (r != 0) return r;

    *link = d->sibling;                          /* splice out of the tree */
    /* §M90 — a VFS_IF_OWN_CHILDREN directory's remaining entries are the
     * fs's synthesised files: their inodes went with the fs's unlink, the
     * dentries are ours to free. */
    while (d->children) { struct dentry* c = d->children; d->children = c->sibling; kfree(c); }
    kfree(d);
    return 0;
}
/* §M90 — fsync(2): write back what the file's volume holds dirty.  The
 * block cache keeps no per-file dirty list, so the unit is the VOLUME — more
 * than asked, never less, which is the direction fsync may err in.  A file on
 * a volume with no device (ramfs) has nothing to write: done. */
int vfs_fsync_file(struct file* f) {
    if (!f || !f->dentry) return -1;
    char dev[16];
    dev[0] = 0;
    kmutex_lock(&ns_lock);
    struct vfs_mount* m = mount_of_dentry(f->dentry);
    if (m) for (int i = 0; i < (int)sizeof dev - 1 && (dev[i] = m->dev_name[i]); i++) dev[i + 1] = 0;
    kmutex_unlock(&ns_lock);
    if (!dev[0]) return 0;
    struct block_device* bd = blk_find(dev);
    return bd ? bcache_sync(bd) : 0;
}

int vfs_unlink(const char* path) {
    return NS_LOCKED(int, vfs_unlink_unlocked(path));
}

/* §M73 — a second name for an existing regular file, in a directory of the
 * SAME filesystem (a link cannot cross one: it is the same inode).  -2 if the
 * new name exists, -5 if the directory may not be written. */
static int vfs_link_unlocked(const char* oldpath, const char* newpath) {
    struct dentry* src = resolve_path(oldpath, NULL, NULL);
    if (!src || !src->inode || src->inode->type != INODE_FILE) return -1;
    char buf[256];
    const char* last;
    if (split_parent(newpath, buf, sizeof buf, &last) != 0 || !*last) return -1;
    if (strlen_(last) > VFS_NAME_MAX) return -1;
    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return -1;
    if (!parent->inode->dir_ops || !parent->inode->dir_ops->link) return -1;
    if (parent->inode->dir_ops != src->parent->inode->dir_ops) return -1;   /* same fs */
    if (!vfs_permitted(parent->inode, VFS_PERM_WRITE)) return -5;
    for (struct dentry* c = parent->children; c; c = c->sibling)
        if (streq(c->name, last)) return -2;
    if (parent->inode->dir_ops->link(parent->inode, last, src->inode) != 0) return -1;
    return vfs_attach_child_unlocked(parent, last, src->inode) ? 0 : -1;
}
int vfs_link(const char* oldpath, const char* newpath) {
    return NS_LOCKED(int, vfs_link_unlocked(oldpath, newpath));
}

/* ------------------------------------------------------------------- */
/* M22.5 — rename / copy / recursive delete.                            */
/* ------------------------------------------------------------------- */

/* §M90 — rename into ANOTHER directory of the same mount: the fs's `move`
 * op, then the dentry is unlinked from one parent's child list and linked
 * into the other's.  -3 = cannot be done here (another mount, or the fs has
 * no move): the caller decides what crossing means (EXDEV for rename(2)).
 * -2 = the target name exists.  -1 = not found / not allowed — including a
 * directory moved into its own subtree, which would detach it from the tree,
 * and a mount point, which is not the mounted filesystem's to move. */
static int vfs_move_unlocked(struct dentry* op, const char* olast,
                             struct dentry* np, const char* nlast) {
    if (!op->inode || op->inode->type != INODE_DIR) return -1;
    if (!np->inode || np->inode->type != INODE_DIR) return -1;
    if (strlen_(nlast) > VFS_NAME_MAX) return -1;
    if (mount_of_dentry(op) != mount_of_dentry(np)) return -3;
    if (!op->inode->dir_ops || !op->inode->dir_ops->move ||
        op->inode->dir_ops != np->inode->dir_ops) return -3;
    if (!vfs_permitted(op->inode, VFS_PERM_WRITE) ||
        !vfs_permitted(np->inode, VFS_PERM_WRITE)) return -5;
    struct dentry* d = NULL;
    for (struct dentry* c = op->children; c; c = c->sibling)
        if (streq(c->name, olast)) { d = c; break; }
    if (!d || !d->inode || d->inode->type == INODE_DEVICE) return -1;
    for (struct dentry* c = np->children; c; c = c->sibling)
        if (streq(c->name, nlast)) return -2;
    for (int i = 0; i < g_nmounts; i++) if (g_mounts[i].mp == d) return -1;
    for (struct dentry* a = np; a; a = a->parent) {          /* not into itself */
        if (a == d) return -1;
        if (a->parent == a) break;
    }
    int r = op->inode->dir_ops->move(op->inode, olast, np->inode, nlast, d->inode);
    if (r != 0) return r;
    /* Relink: out of the old parent's list, into the new one's. */
    struct dentry** pp = &op->children;
    while (*pp && *pp != d) pp = &(*pp)->sibling;
    if (*pp) *pp = d->sibling;
    d->parent  = np;
    d->sibling = np->children;
    np->children = d;
    size_t i = 0;
    for (; nlast[i] && i < sizeof(d->name) - 1; i++) d->name[i] = nlast[i];
    d->name[i] = 0;
    return 0;
}

static int vfs_rename_unlocked(const char* oldpath, const char* newpath) {
    char obuf[256], nbuf[256];
    const char *olast, *nlast;
    if (split_parent(oldpath, obuf, sizeof obuf, &olast) != 0) return -1;
    if (split_parent(newpath, nbuf, sizeof nbuf, &nlast) != 0) return -1;
    if (!*olast || !*nlast) return -1;

    struct dentry* oparent = resolve_path(obuf, NULL, NULL);
    struct dentry* nparent = resolve_path(nbuf, NULL, NULL);
    if (!oparent || !nparent) return -1;
    if (oparent != nparent) return vfs_move_unlocked(oparent, olast, nparent, nlast);
    if (!oparent->inode || oparent->inode->type != INODE_DIR) return -1;
    if (!oparent->inode->dir_ops || !oparent->inode->dir_ops->rename) return -1;
    if (strlen_(nlast) > VFS_NAME_MAX) return -1;

    struct dentry* d = NULL;
    for (struct dentry* c = oparent->children; c; c = c->sibling) {
        if (streq(c->name, nlast)) return -2;        /* target exists */
        if (streq(c->name, olast)) d = c;
    }
    if (!d || !d->inode) return -1;
    if (d->inode->type == INODE_DEVICE) return -1;

    int r = oparent->inode->dir_ops->rename(oparent->inode, olast, nlast,
                                            d->inode);
    if (r != 0) return r;

    /* The fs said yes — rewrite the dentry (the VFS owns names). */
    size_t i = 0;
    for (; nlast[i] && i < sizeof(d->name) - 1; i++) d->name[i] = nlast[i];
    d->name[i] = 0;
    return 0;
}
int vfs_rename(const char* oldpath, const char* newpath) {
    return NS_LOCKED(int, vfs_rename_unlocked(oldpath, newpath));
}

/* §M90 — rename(2)'s contract: an existing TARGET is replaced.  Programs
 * write a temporary file and rename it over the real one (containerd's
 * metadata, every atomic config save), and a rename that refuses an existing
 * target breaks exactly that.  Done under ONE hold of the namespace lock, so
 * nobody can observe the gap between the unlink and the rename.  A directory
 * target is replaced only by a directory and only when empty (-2: a file over
 * a directory, -4: not empty).  Across directories when the filesystem can
 * move (§M90); -3 says it cannot (another mount, or no `move` op) so the
 * caller can decide what crossing means for it. */
static int vfs_rename_replace_unlocked(const char* oldpath, const char* newpath) {
    char obuf[256], nbuf[256];
    const char *olast, *nlast;
    if (split_parent(oldpath, obuf, sizeof obuf, &olast) != 0) return -1;
    if (split_parent(newpath, nbuf, sizeof nbuf, &nlast) != 0) return -1;
    struct dentry* op = resolve_path(obuf, NULL, NULL);
    struct dentry* np = resolve_path(nbuf, NULL, NULL);
    if (!op || !np) return -1;
    int r = vfs_rename_unlocked(oldpath, newpath);
    if (r != -2) return r;
    struct dentry* t = resolve_path(newpath, NULL, NULL);
    if (!t || !t->inode) return -2;
    if (t->inode->type == INODE_DIR) {
        /* POSIX: a directory may replace an EMPTY directory (an image
         * store renames a finished tree over its empty placeholder). */
        struct dentry* s0 = resolve_path(oldpath, NULL, NULL);
        if (!s0 || !s0->inode || s0->inode->type != INODE_DIR) return -2;
        if (t->children) return -4;                      /* not empty */
    }
    if (vfs_unlink_unlocked(newpath) != 0) return -1;
    return vfs_rename_unlocked(oldpath, newpath);
}
int vfs_rename_replace(const char* oldpath, const char* newpath) {
    return NS_LOCKED(int, vfs_rename_replace_unlocked(oldpath, newpath));
}

/* §M90 — the path of a dentry AS THE CALLING TASK SEES IT (relative to its
 * §M73 root), for the *at calls' directory descriptors.  "" for the root, as
 * cred.cwd spells it.  -1 when it does not fit, or when the dentry is outside
 * the task's root (a descriptor carried into a container must not name what
 * lies above the container's "/"). */
static int vfs_dentry_path_unlocked(struct dentry* d, char* out, size_t cap) {
    struct dentry* top = cred_fs_root();
    if (!top) top = root;
    const char* parts[48];
    int np = 0;
    while (d && d != top) {
        if (d == root || !d->parent || d->parent == d) return -1;   /* above the task's root */
        if (np == 48) return -1;
        parts[np++] = d->name;
        d = d->parent;
    }
    if (!d) return -1;
    size_t o = 0;
    for (int i = np - 1; i >= 0; i--) {
        if (o + 1 >= cap) return -1;
        out[o++] = '/';
        for (const char* q = parts[i]; *q; q++) { if (o + 1 >= cap) return -1; out[o++] = *q; }
    }
    out[o] = 0;
    return 0;
}
int vfs_dentry_path(struct dentry* d, char* out, size_t cap) {
    if (!d || !out || cap < 2) return -1;
    return NS_LOCKED(int, vfs_dentry_path_unlocked(d, out, cap));
}

int vfs_copy(const char* src, const char* dst) {
    struct file* in = vfs_open(src, VFS_RDONLY);
    if (!in) return -1;
    if (!in->inode || in->inode->type != INODE_FILE) {
        vfs_close(in);
        return -1;                                   /* files only */
    }
    /* Self-copy guard: opening dst with TRUNC would empty the SOURCE
     * if both paths resolve to the same inode.  Probe without TRUNC
     * first and compare. */
    struct file* probe = vfs_open(dst, VFS_RDONLY);
    if (probe) {
        int same = (probe->inode == in->inode);
        vfs_close(probe);
        if (same) { vfs_close(in); return -1; }
    }

    struct file* out = vfs_open(dst, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!out) { vfs_close(in); return -1; }

    /* Bounce buffer on the heap — kernel task stacks are 4 KiB. */
    char* buf = (char*)kmalloc(4096);
    if (!buf) { vfs_close(in); vfs_close(out); return -1; }

    int err = 0;
    for (;;) {
        ssize_t r = vfs_read(in, buf, 4096);
        if (r < 0) { err = 1; break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = vfs_write(out, buf + off, (size_t)(r - off));
            if (w <= 0) { err = 1; break; }
            off += w;
        }
        if (err) break;
    }
    kfree(buf);
    vfs_close(in);
    vfs_close(out);
    return err ? -1 : 0;
}

/* Depth-limited tree delete.  The path buffer is SHARED across the
 * recursion (append child / recurse / truncate back) so the stack cost
 * per frame stays at a few dozen bytes — kernel stacks are 4 KiB.
 * Deleting while iterating: each round re-reads the directory's FIRST
 * entry (f->pos reset), so no iterator is held across an unlink. */
static int unlink_rec(char* path, size_t cap, int depth) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    int is_dir = f->inode && f->inode->type == INODE_DIR;

    if (is_dir) {
        if (depth >= 8) { vfs_close(f); return -1; }
        for (;;) {
            struct dirent de;
            f->pos = 0;                          /* rewind: first entry */
            int n = vfs_readdir(f, &de);
            if (n <= 0) break;                   /* empty (or error) */

            size_t plen = strlen_(path);
            size_t nlen = strlen_(de.name);
            /* "path" + "/" + name + NUL must fit. */
            if (plen + 1 + nlen + 1 > cap) { vfs_close(f); return -1; }
            size_t p = plen;
            if (!(plen == 1 && path[0] == '/')) path[p++] = '/';
            for (size_t i = 0; i <= nlen; i++) path[p + i] = de.name[i];

            int r = unlink_rec(path, cap, depth + 1);
            path[plen] = 0;                      /* truncate back */
            if (r != 0) { vfs_close(f); return r; }
        }
    }
    vfs_close(f);
    return vfs_unlink(path);
}

static int vfs_unlink_recursive_unlocked(const char* path) {
    char buf[256];
    size_t len = strlen_(path);
    if (len == 0 || len >= sizeof buf) return -1;
    memcpy_(buf, path, len + 1);
    return unlink_rec(buf, sizeof buf, 0);
}
int vfs_unlink_recursive(const char* path) {
    return NS_LOCKED(int, vfs_unlink_recursive_unlocked(path));
}

/* =============================================================================
 * §M32 stage 5 — ownership and permissions.
 *
 * WHERE THE CHECKS ARE, AND THE ONE THAT IS DELIBERATELY ELSEWHERE.
 *
 * `vfs_open` checks read and/or write against the FILE; the namespace mutators
 * (create, mkdir, unlink, rename) check WRITE against the PARENT DIRECTORY,
 * because creating and removing names changes the directory rather than the
 * file — which is also why a read-only file in a writable directory can still
 * be deleted, exactly as POSIX has it.
 *
 * **THE EXECUTE BIT IS NOT CHECKED HERE AND CANNOT BE.**  A program is opened
 * for READING in order to be run, so an open-time check has no way to tell
 * "may read this file" from "may run it".  It lives in the loader (proc.c,
 * §M32 stage 6) and nowhere else.
 * ============================================================================= */

void vfs_inode_defaults(struct inode* ino) {
    if (!ino) return;
    ino->owner_uid = CRED_UID_ROOT;
    ino->owner_gid = CRED_GID_ROOT;
    /* A directory needs x to be traversable at all, which is why the two
     * defaults differ by exactly that bit. */
    ino->mode = (ino->type == INODE_DIR) ? 0755u : 0644u;
}

int vfs_permitted(const struct inode* ino, int want) {
    if (!ino) return 0;
    const struct cred* c = cred_current();

    /* The kernel's own tasks and the system's services are not gated against
     * the machine they are (cred.h).  This is also what keeps the boot path,
     * every driver and every §M29 service working exactly as before §M32. */
    if (c->owner != TASK_OWNER_USER) return 1;

    if (cred_is_admin(c)) {
        /* Classic root override — with the classic exception: an administrator
         * may read and write anything, but may only EXECUTE something that is
         * executable by somebody.  Without the exception, every text file on
         * the machine would be a program to an admin, and "chmod -x" would
         * stop meaning anything for the account most likely to run it. */
        if (want != VFS_PERM_EXEC) return 1;
        return (ino->mode & 0111u) != 0;
    }

    /* An inode that never went through vfs_inode_defaults.  Refusing is the
     * safe answer AND a visible one — `audit inode-ownership` reports these by
     * name, so the seventh construction site shows up as a named row rather
     * than as a filesystem that mysteriously denies everything. */
    if (ino->mode == 0) return 0;

    uint32_t bits;
    if (cred_uid(c) == ino->owner_uid)        bits = (ino->mode >> 6) & 7u;
    else if (cred_in_group(c, ino->owner_gid)) bits = (ino->mode >> 3) & 7u;
    else                                       bits = ino->mode & 7u;

    return ((int)bits & want) == want;
}

/* §M32 — hand a changed owner/mode to the filesystem, if it keeps them.  A
 * failure is reported, and the callers put the old values back: enforcing a
 * mode the disk did not take would be true until the next mount and then
 * silently false. */
static int persist_attr(struct dentry* d) {
    if (!d->parent || !d->parent->inode) return 0;          /* a mount root */
    const struct inode_ops* ops = d->parent->inode->dir_ops;
    if (!ops || !ops->setattr) return 0;                    /* volatile fs */
    return ops->setattr(d->parent->inode, d->name, d->inode);
}

static int vfs_chmod_unlocked(const char* path, uint32_t mode) {
    struct dentry* d = resolve_path(path, NULL, NULL);
    if (!d || !d->inode) return -1;
    const struct cred* c = cred_current();
    /* The OWNER or an admin.  Not "anyone who may write the file": write
     * permission is something an owner grants, and letting it also grant the
     * power to change the grant makes the mode self-modifying. */
    if (c->owner == TASK_OWNER_USER && !cred_is_admin(c) &&
        cred_uid(c) != d->inode->owner_uid) return -2;
    uint32_t was = d->inode->mode;
    d->inode->mode = mode & 07777u;
    if (persist_attr(d) != 0) { d->inode->mode = was; return -3; }
    return 0;
}
int vfs_chmod(const char* path, uint32_t mode) {
    return NS_LOCKED(int, vfs_chmod_unlocked(path, mode));
}

static int vfs_chown_unlocked(const char* path, int uid, int gid) {
    struct dentry* d = resolve_path(path, NULL, NULL);
    if (!d || !d->inode) return -1;
    const struct cred* c = cred_current();
    /* ADMIN ONLY, including for a file you own.  Giving a file away is how an
     * ownership-based rule gets escaped from the inside, and no ordinary
     * workflow here needs it. */
    if (c->owner == TASK_OWNER_USER && !cred_is_admin(c)) return -2;
    int ou = d->inode->owner_uid, og = d->inode->owner_gid;
    if (uid != CRED_UID_NONE) d->inode->owner_uid = uid;
    if (gid != CRED_UID_NONE) d->inode->owner_gid = gid;
    if (persist_attr(d) != 0) { d->inode->owner_uid = ou; d->inode->owner_gid = og; return -3; }
    return 0;
}
/* §M32 — owner, mode, size and kind of `path`, as this kernel enforces them.
 * 0, or -1 if the path does not resolve. */
static int vfs_stat_unlocked(const char* path, struct vfs_stat* st) {
    struct dentry* d = resolve_path(path, NULL, NULL);
    if (!d || !d->inode) return -1;
    st->uid  = d->inode->owner_uid;
    st->gid  = d->inode->owner_gid;
    st->mode = d->inode->mode;
    st->size = d->inode->size;
    st->is_dir = d->inode->type == INODE_DIR;
    return 0;
}
int vfs_stat(const char* path, struct vfs_stat* st) {
    if (!st) return -1;
    st->is_link = 0;
    return NS_LOCKED(int, vfs_stat_unlocked(path, st));
}

/* ---- §M89 — symbolic links -------------------------------------------------- */

static int vfs_lstat_unlocked(const char* path, struct vfs_stat* st) {
    struct dentry* d = resolve_path_ex(path, NULL, NULL, 0);
    if (!d || !d->inode) return -1;
    st->uid  = d->inode->owner_uid;
    st->gid  = d->inode->owner_gid;
    st->mode = d->inode->mode;
    st->size = d->inode->size;
    st->is_dir  = d->inode->type == INODE_DIR;
    st->is_link = d->inode->type == INODE_SYMLINK;
    return 0;
}
int vfs_lstat(const char* path, struct vfs_stat* st) {
    if (!st) return -1;
    return NS_LOCKED(int, vfs_lstat_unlocked(path, st));
}

static int vfs_readlink_unlocked(const char* path, char* out, size_t cap) {
    struct dentry* d = resolve_path_ex(path, NULL, NULL, 0);
    if (!d || !d->inode) return -1;                         /* ENOENT */
    if (d->inode->type != INODE_SYMLINK) return -2;          /* EINVAL: not a link */
    return link_target(d, out, cap);
}
int vfs_readlink(const char* path, char* out, size_t cap) {
    return NS_LOCKED(int, vfs_readlink_unlocked(path, out, cap));
}

static int vfs_realpath_unlocked(const char* path, char* out, size_t cap) {
    char cb[256];
    const char* c = vfs_canon(path, cb, sizeof cb);
    if (!c) return -1;
    return expand_links(c, out, cap, 1) ? 0 : -1;
}
int vfs_realpath(const char* path, char* out, size_t cap) {
    if (!path || !*path || !out) return -1;
    return NS_LOCKED(int, vfs_realpath_unlocked(path, out, cap));
}

static int vfs_symlink_unlocked(const char* target, const char* linkpath) {
    if (!target || !*target || strlen_(target) >= 255) return -1;
    char buf[256];
    const char* last;
    if (split_parent(linkpath, buf, sizeof buf, &last) != 0 || !*last) return -1;
    if (strlen_(last) > VFS_NAME_MAX) return -1;
    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return -1;
    if (!parent->inode->dir_ops || !parent->inode->dir_ops->symlink) return -3;   /* EPERM: fs has none */
    if (!vfs_permitted(parent->inode, VFS_PERM_WRITE)) return -5;
    for (struct dentry* c = parent->children; c; c = c->sibling)
        if (streq(c->name, last)) return -2;                                     /* EEXIST */
    struct inode* ino = NULL;
    if (parent->inode->dir_ops->symlink(parent->inode, last, target, &ino) != 0 || !ino) return -1;
    const struct cred* cr = cred_current();
    if (cr->owner == TASK_OWNER_USER) { ino->owner_uid = cred_uid(cr); ino->owner_gid = cred_gid(cr); }
    ino->mode = 0777;                                      /* a link's own bits mean nothing */
    return vfs_attach_child_unlocked(parent, last, ino) ? 0 : -1;
}
int vfs_symlink(const char* target, const char* linkpath) {
    return NS_LOCKED(int, vfs_symlink_unlocked(target, linkpath));
}

int vfs_chown(const char* path, int uid, int gid) {
    return NS_LOCKED(int, vfs_chown_unlocked(path, uid, gid));
}

/* Which mount does `path` fall under?  Longest matching prefix wins, so
 * /mnt/foo answers with the exFAT mount rather than with the root. */
const struct vfs_mount* vfs_mount_for(const char* path) {
    const struct vfs_mount* best = NULL;
    int bestlen = -1;
    if (!path) return NULL;
    for (int i = 0; i < g_nmounts; i++) {
        const char* mp = g_mounts[i].path;
        int n = 0;
        while (mp[n]) n++;
        int match = 1;
        for (int k = 0; k < n; k++) if (path[k] != mp[k]) { match = 0; break; }
        /* "/mnt" must not match "/mnturbo": the character after the prefix has
         * to be a separator or the end of the path. */
        if (match && n > 1 && path[n] && path[n] != '/') match = 0;
        if (match && n > bestlen) { best = &g_mounts[i]; bestlen = n; }
    }
    return best;
}

int vfs_mount_count(void) { return g_nmounts; }
const struct vfs_mount* vfs_mount_at(int i) {
    return (i >= 0 && i < g_nmounts) ? &g_mounts[i] : NULL;
}

int vfs_ownership_is_persistent(const char* path) {
    const struct vfs_mount* m = vfs_mount_for(path);
    if (!m) return -1;
    return m->stores_ownership ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * §M87 — UMOUNT.
 *
 * There was none: mounts were made at boot and lived until power-off, which
 * is why the mount record could be a table that only ever grew.  A disk
 * manager needs the other half — a volume must be detachable before it is
 * formatted, and a removable one before it is removed.
 *
 * REFUSAL IS THE COMMON CASE AND EACH REASON IS NAMED.  An open file below the
 * mountpoint, a mount nested inside it, or a subsystem that depends on it (the
 * settings store lives on /mnt) — any of these makes an unmount a use-after-
 * free waiting to happen, so it is refused with the reason rather than forced.
 * There is no "lazy" unmount: a detach that finishes "later, when nobody is
 * looking" is a teardown with no owner.
 *
 * ORDER: the fs writes back and frees its volume state FIRST (while the tree
 * still exists, so a refusal leaves everything intact), then the VFS frees the
 * dentries and inodes, then the placeholder the mount covered is put back.
 * ------------------------------------------------------------------------- */

static void free_tree(struct dentry* d, struct fs_type* fs) {
    struct dentry* c = d->children;
    while (c) {
        struct dentry* next = c->sibling;
        free_tree(c, fs);
        if (c->inode) {
            if (fs && fs->evict) fs->evict(c->inode);
            kfree(c->inode);
        }
        kfree(c);
        c = next;
    }
    d->children = NULL;
}

static int path_is_below(const char* inner, const char* outer) {
    int n = 0;
    while (outer[n]) n++;
    for (int k = 0; k < n; k++) if (inner[k] != outer[k]) return 0;
    return inner[n] == '/' && inner[n + 1] != 0;
}

static int vfs_umount_unlocked(const char* path) {
    int idx = -1;
    for (int i = 0; i < g_nmounts; i++)
        if (streq(g_mounts[i].path, path)) idx = i;
    if (idx < 0 || streq(path, "/")) return -1;
    struct vfs_mount* m = &g_mounts[idx];
    if (m->hold) return -4;
    if (m->open_files > 0) return -2;
    for (int i = 0; i < g_nmounts; i++)
        if (i != idx && path_is_below(g_mounts[i].path, path)) return -3;
    if (!m->fs || !m->fs->umount) return -5;
    if (m->fs->umount(m->mp) != 0) return -5;

    struct dentry* mp = m->mp;
    free_tree(mp, m->fs);
    if (mp->inode) {
        if (m->fs->evict) m->fs->evict(mp->inode);
        kfree(mp->inode);
    }
    mp->inode    = m->covered_inode;
    mp->children = m->covered_children;

    /* The block cache still holds the volume's sectors.  Written back by the
     * fs's umount; DROPPED here, so a disk that is formatted or replaced next
     * is read fresh instead of through a cache of the old contents. */
    if (m->dev_name[0]) {
        struct block_device* bd = blk_find(m->dev_name);
        if (bd) { bcache_sync(bd); bcache_invalidate(bd); }
    }
    kprintf("vfs: unmounted %s%s%s\n", path, m->dev_name[0] ? " from " : "",
            m->dev_name);
    for (int i = idx; i + 1 < g_nmounts; i++) g_mounts[i] = g_mounts[i + 1];
    g_nmounts--;
    return 0;
}
int vfs_umount(const char* path) {
    int r = NS_LOCKED(int, vfs_umount_unlocked(path));
    /* §M74 — the volume's inodes are gone; nothing may still hit their pages.
     * The cache has no per-filesystem index, and an unmount is rare, so the
     * whole cache goes (mapped pages are detached, not freed). */
    if (r == 0) pcache_drop_all();
    return r;
}

int vfs_mount_hold(const char* path, const char* who) {
    int r = -1;
    kmutex_lock(&ns_lock);
    for (int i = 0; i < g_nmounts; i++)
        if (streq(g_mounts[i].path, path)) { g_mounts[i].hold = who; r = 0; }
    kmutex_unlock(&ns_lock);
    return r;
}

const struct vfs_mount* vfs_mount_of_dev(const char* dev_name) {
    if (!dev_name || !dev_name[0]) return NULL;
    for (int i = 0; i < g_nmounts; i++)
        if (streq(g_mounts[i].dev_name, dev_name)) return &g_mounts[i];
    return NULL;
}

int vfs_statfs(const char* path, uint64_t* total, uint64_t* free) {
    const struct vfs_mount* m = vfs_mount_for(path);
    if (!m || !m->fs || !m->fs->statfs) return -1;
    return m->fs->statfs(m->mp, total, free);
}

/* ---------------------------------------------------------------------------
 * §M71 — the invariant that keeps the mode field meaningful.
 *
 * `vfs_inode_defaults` is called from six places across four filesystems, and
 * a seventh that forgets produces an inode with mode 0 — which `vfs_permitted`
 * REFUSES, so the symptom is a file nobody can open for a reason nothing
 * explains.  This turns that into a named row.
 *
 * HOW TO MAKE IT FAIL: `inodetest`, below, clears the mode on a real inode in
 * the real tree — §M71 rule 1, detection rather than reporting.
 * ------------------------------------------------------------------------- */

static int walk_inodes(struct dentry* d, int depth, int verbose, int* rows) {
    if (!d || depth > 8) return 0;
    int bad = 0;
    if (d->inode) {
        (*rows)++;
        if (d->inode->mode == 0) {
            kprintf("  !! '%s' has mode 0 — it never went through "
                    "vfs_inode_defaults, and nothing can open it\n", d->name);
            bad++;
        } else if (verbose) {
            /* Octal BY HAND.  This kernel's printf has no %o and no width
             * specifiers — a documented trap in CLAUDE.md that this file walked
             * straight into: the first run printed a literal "%o" beside a
             * decimal 493, which is 0755 wearing a disguise. */
            char m[5];
            m[0] = (char)('0' + ((d->inode->mode >> 9) & 7));
            m[1] = (char)('0' + ((d->inode->mode >> 6) & 7));
            m[2] = (char)('0' + ((d->inode->mode >> 3) & 7));
            m[3] = (char)('0' + (d->inode->mode & 7));
            m[4] = 0;
            kprintf("  %s mode %s uid %d gid %d\n", d->name, m,
                    d->inode->owner_uid, d->inode->owner_gid);
        }
    }
    for (struct dentry* c = d->children; c; c = c->sibling)
        bad += walk_inodes(c, depth + 1, verbose, rows);
    return bad;
}

static int au_inode_ownership(int verbose) {
    int rows = 0;
    /* Only what is CACHED in the dentry tree.  exFAT is lazy, so an unvisited
     * directory has no inode yet and cannot be checked — and this reports the
     * count so a clean answer is not read as covering the whole disk. */
    int bad = walk_inodes(vfs_root(), 0, verbose, &rows);
    if (rows == 0) return AUDIT_SKIP;
    if (verbose) kprintf("  %d cached inode(s) checked (lazy filesystems are "
                         "only checked where they have been visited)\n", rows);
    return bad;
}

AUDIT(inode_ownership) = {
    "inode-ownership",
    "every cached inode carries an owner and a mode that was actually set",
    au_inode_ownership
};

static void cmd_inodetest(const char* args) {
    (void)args;
    struct dentry* d = resolve_path("/tmp", NULL, NULL);
    if (!d) { vfs_mkdir("/tmp"); d = resolve_path("/tmp", NULL, NULL); }
    if (!d || !d->inode) { kprintf("inodetest: could not reach /tmp\n"); return; }

    uint32_t saved = d->inode->mode;
    d->inode->mode = 0;
    kprintf("inodetest: cleared /tmp's mode — `audit inode-ownership` must FAIL\n");
    int v = audit_run_one("inode-ownership", 0);
    kprintf("inodetest: audit reported %d violation(s) — %s\n",
            v, v > 0 ? "DETECTED" : "NOT DETECTED (the check is broken)");
    d->inode->mode = saved;
    v = audit_run_one("inode-ownership", 0);
    kprintf("inodetest: restored; audit reports %d violation(s) — %s\n",
            v, v == 0 ? "clean" : "STILL DIRTY");
}

SHELL_CMD(inodetest) = { "inodetest", "", NULL, SHELL_G_TEST, cmd_inodetest, SHELL_P_ADMIN };

/* ------------------------------------------------------------------- */
/* §M90 — vfs_bind / vfs_unbind (see vfs.h and the table above).        */
/* ------------------------------------------------------------------- */

/* The TARGET dentry itself, not what a bind on it would redirect to. */
static struct dentry* bind_target_unlocked(const char* path) {
    char buf[256];
    const char* last;
    if (split_parent(path, buf, sizeof buf, &last) != 0 || !*last) return NULL;
    struct dentry* parent = resolve_path(buf, NULL, NULL);
    if (!parent || !parent->inode || parent->inode->type != INODE_DIR) return NULL;
    return lookup_child(parent, last, strlen_(last));
}

int vfs_bind(const char* src, const char* tgt) {
    kmutex_lock(&ns_lock);
    int r;
    struct dentry* S = resolve_path(src, NULL, NULL);
    struct dentry* T = bind_target_unlocked(tgt);
    if (!S || !S->inode || !T || !T->inode) r = -1;
    else if (T->bound && bind_visible(T)) r = -2;
    else if ((S->inode->type == INODE_DIR) != (T->inode->type == INODE_DIR)) r = -3;
    else if (g_nbinds >= VFS_MAX_BINDS) r = -4;
    else if (S == T) r = 0;                       /* onto itself: nothing changes */
    else {
        T->bound_seq = ++g_bind_seq;
        T->bound_ns  = task_mntns_current();
        T->bound     = S;
        g_binds[g_nbinds].tgt = T;
        g_binds[g_nbinds].src = S;
        g_nbinds++;
        r = 0;
    }
    kmutex_unlock(&ns_lock);
    return r;
}

int vfs_unbind(const char* tgt) {
    kmutex_lock(&ns_lock);
    int r = -1;
    struct dentry* T = bind_target_unlocked(tgt);
    if (T && T->bound && bind_visible(T)) {
        T->bound = NULL;
        for (int i = 0; i < g_nbinds; i++)
            if (g_binds[i].tgt == T) { g_binds[i] = g_binds[--g_nbinds]; break; }
        r = 0;
    }
    kmutex_unlock(&ns_lock);
    return r;
}

int vfs_bind_count(void) { return g_nbinds; }
int vfs_bind_nth(int i, char* src, size_t scap, char* tgt, size_t tcap) {
    kmutex_lock(&ns_lock);
    int r = -1;
    if (i >= 0 && i < g_nbinds && g_binds[i].tgt->bound && bind_visible(g_binds[i].tgt) &&
        vfs_dentry_path_unlocked(g_binds[i].src, src, scap) == 0 &&
        vfs_dentry_path_unlocked(g_binds[i].tgt, tgt, tcap) == 0) r = 0;
    kmutex_unlock(&ns_lock);
    return r;
}
