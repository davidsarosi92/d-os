/* =============================================================================
 * cgroupfs.c — the cgroup v2 filesystem ("cgroup2"), mounted at /sys/fs/cgroup.
 * §M90 rung 2, 2026-10-02.
 *
 * WHY.  dockerd, containerd and runc decide what the kernel can do by looking
 * at /sys/fs/cgroup: statfs's f_type says whether it is the unified (v2)
 * hierarchy, cgroup.controllers lists what may be delegated, and every
 * container is a directory there whose cgroup.procs receives the container's
 * first process.  With nothing mounted dockerd fell back to v1 probing and
 * stopped at "Devices cgroup isn't mounted".
 *
 * WHAT A CGROUP IS HERE.  A node in a tree (one directory each), a set of
 * member tasks (`task->cgroup`, inherited at spawn — a child starts where its
 * parent is, as on Linux), and its control files.  Membership is real:
 * cgroup.procs lists and moves PROCESSES (every thread of the group goes),
 * /proc/self/cgroup names the task's node, pids.current and memory.current
 * count the members, cgroup.kill kills them and cgroup.freeze stops and
 * resumes them through §M72's TASK_STOPPED.
 *
 * WHAT IT IS NOT YET — said here so a pass is not read as more than it is:
 * the LIMITS (memory.max, pids.max, cpu.max, cpu.weight, io.*, cpuset.*) are
 * stored and read back but not enforced; enforcement is §M90 rung 3 ("cgroup
 * limits enforced"), where memory.max is to become a §M72 reserve per node.
 * The "no internal processes" rule and the threaded mode are not modelled
 * (cgroup.type answers "domain").  rmdir of a node whose control file is
 * still OPEN frees the node under the open file — Linux keeps it alive; this
 * has no per-inode open count yet.
 *
 * SHAPE.  Each node owns its inodes: one directory inode and one inode per
 * control file, embedded in the node, so the VFS's dentries (which it frees)
 * never own anything the node frees.  Control files are found by `lookup`
 * (lazily, like any fs that does not build its tree eagerly) and listed by
 * `readdir`; child nodes are eager dentries made by mkdir.  The directory is
 * marked VFS_IF_OWN_CHILDREN: rmdir is the node's decision (no members, no
 * child nodes), not "the directory has entries", and the VFS frees the
 * file dentries after the node says yes.
 * ============================================================================= */

#include "cgroupfs.h"
#include "bpf.h"
#include "vfs.h"
#include "task.h"
#include "kmalloc.h"
#include "printf.h"
#include "percpu.h"
#include "module.h"
#include <stddef.h>
#include <stdint.h>

#define CG_NAME   VFS_NAME_MAX
#define CG_VAL    48

enum cgk {
    K_CONTROLLERS, K_SUBTREE, K_PROCS, K_THREADS, K_TYPE, K_EVENTS, K_STAT,
    K_MAXDEPTH, K_MAXDESC, K_FREEZE, K_KILL,
    K_CPU_MAX, K_CPU_WEIGHT, K_CPU_STAT,
    K_MEM_MAX, K_MEM_HIGH, K_MEM_LOW, K_MEM_MIN, K_MEM_CURRENT, K_MEM_PEAK,
    K_MEM_SWAP_MAX, K_MEM_SWAP_CUR, K_MEM_EVENTS, K_MEM_STAT, K_MEM_OOM_GROUP,
    K_PIDS_MAX, K_PIDS_CURRENT,
    K_IO_MAX, K_IO_WEIGHT, K_IO_STAT,
    K_CPUSET_CPUS, K_CPUSET_MEMS, K_CPUSET_CPUS_EFF, K_CPUSET_MEMS_EFF,
    K_N
};

/* name, present on the root too, writable, default text (stored kinds) */
static const struct { const char* name; char root; char rw; const char* def; } g_kind[K_N] = {
    [K_CONTROLLERS]   = { "cgroup.controllers",      1, 0, NULL },
    [K_SUBTREE]       = { "cgroup.subtree_control",  1, 1, NULL },
    [K_PROCS]         = { "cgroup.procs",            1, 1, NULL },
    [K_THREADS]       = { "cgroup.threads",          1, 1, NULL },
    [K_TYPE]          = { "cgroup.type",             0, 1, NULL },
    [K_EVENTS]        = { "cgroup.events",           0, 0, NULL },
    [K_STAT]          = { "cgroup.stat",             1, 0, NULL },
    [K_MAXDEPTH]      = { "cgroup.max.depth",        1, 1, "max" },
    [K_MAXDESC]       = { "cgroup.max.descendants",  1, 1, "max" },
    [K_FREEZE]        = { "cgroup.freeze",           0, 1, NULL },
    [K_KILL]          = { "cgroup.kill",             0, 1, NULL },
    [K_CPU_MAX]       = { "cpu.max",                 0, 1, "max 100000" },
    [K_CPU_WEIGHT]    = { "cpu.weight",              0, 1, "100" },
    [K_CPU_STAT]      = { "cpu.stat",                1, 0, NULL },
    [K_MEM_MAX]       = { "memory.max",              0, 1, "max" },
    [K_MEM_HIGH]      = { "memory.high",             0, 1, "max" },
    [K_MEM_LOW]       = { "memory.low",              0, 1, "0" },
    [K_MEM_MIN]       = { "memory.min",              0, 1, "0" },
    [K_MEM_CURRENT]   = { "memory.current",          0, 0, NULL },
    [K_MEM_PEAK]      = { "memory.peak",             0, 0, NULL },
    [K_MEM_SWAP_MAX]  = { "memory.swap.max",         0, 1, "max" },
    [K_MEM_SWAP_CUR]  = { "memory.swap.current",     0, 0, NULL },
    [K_MEM_EVENTS]    = { "memory.events",           0, 0, NULL },
    [K_MEM_STAT]      = { "memory.stat",             1, 0, NULL },
    [K_MEM_OOM_GROUP] = { "memory.oom.group",        0, 1, "0" },
    [K_PIDS_MAX]      = { "pids.max",                0, 1, "max" },
    [K_PIDS_CURRENT]  = { "pids.current",            0, 0, NULL },
    [K_IO_MAX]        = { "io.max",                  0, 1, "" },
    [K_IO_WEIGHT]     = { "io.weight",               0, 1, "default 100" },
    [K_IO_STAT]       = { "io.stat",                 1, 0, NULL },
    [K_CPUSET_CPUS]   = { "cpuset.cpus",             0, 1, "" },
    [K_CPUSET_MEMS]   = { "cpuset.mems",             0, 1, "" },
    [K_CPUSET_CPUS_EFF] = { "cpuset.cpus.effective", 1, 0, NULL },
    [K_CPUSET_MEMS_EFF] = { "cpuset.mems.effective", 1, 0, NULL },
};

/* The controllers this kernel offers, in Linux's order. */
static const char* const g_ctl[] = { "cpuset", "cpu", "io", "memory", "pids" };
#define CTL_N 5
#define CTL_ALL ((1u << CTL_N) - 1)

struct cgroup;
struct cgfile { struct cgroup* cg; int kind; };

struct cgroup {
    struct inode   dir;                  /* FIRST: container_of is a cast */
    struct inode   files[K_N];
    struct cgfile  fp[K_N];
    struct cgroup* parent;
    char           name[CG_NAME + 1];
    uint32_t       subtree;              /* controllers enabled for children */
    int            nchildren;
    int            frozen;
    uint64_t       mem_peak;
    char           val[K_N][CG_VAL];     /* stored text of the limit files */
};

static struct cgroup g_root;
static int g_root_ready;

static const struct file_ops  cg_file_ops;
static const struct file_ops  cg_dir_ops;
static const struct inode_ops cg_inode_ops;

/* ---- small string helpers (no libc) ------------------------------------- */

static int s_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static size_t s_len(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void s_cpy(char* d, const char* s, size_t cap) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

struct wbuf { char* p; size_t n, cap; };
static void w_s(struct wbuf* w, const char* s) { while (*s && w->n + 1 < w->cap) w->p[w->n++] = *s++; w->p[w->n] = 0; }
static void w_u(struct wbuf* w, uint64_t v) {
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) { char c[2] = { t[--k], 0 }; w_s(w, c); }
}

/* ---- the tree ------------------------------------------------------------ */

static int cg_is_root(const struct cgroup* cg) { return cg == &g_root; }

static void cg_init(struct cgroup* cg, struct cgroup* parent, const char* name) {
    cg->parent = parent;
    s_cpy(cg->name, name, sizeof cg->name);
    cg->dir.type    = INODE_DIR;
    vfs_inode_defaults(&cg->dir);
    cg->dir.ops     = &cg_dir_ops;
    cg->dir.dir_ops = &cg_inode_ops;
    cg->dir.vflags  = VFS_IF_OWN_CHILDREN;
    cg->dir.private = cg;
    for (int k = 0; k < K_N; k++) {
        cg->files[k].type = INODE_FILE;
        vfs_inode_defaults(&cg->files[k]);
        if (!g_kind[k].rw) cg->files[k].mode = 0444u;
        cg->files[k].ops = &cg_file_ops;
        cg->fp[k].cg = cg;
        cg->fp[k].kind = k;
        cg->files[k].private = &cg->fp[k];
        if (g_kind[k].def) s_cpy(cg->val[k], g_kind[k].def, CG_VAL);
    }
}

static uint32_t cg_available(const struct cgroup* cg) {
    return cg->parent ? cg->parent->subtree : CTL_ALL;
}

/* The path of a node from the root, as /proc/<pid>/cgroup spells it. */
static void cg_path(const struct cgroup* cg, char* out, size_t cap) {
    const struct cgroup* chain[32];
    int n = 0;
    for (const struct cgroup* c = cg; c && !cg_is_root(c) && n < 32; c = c->parent) chain[n++] = c;
    size_t o = 0;
    if (n == 0 && cap > 1) out[o++] = '/';
    for (int i = n - 1; i >= 0; i--) {
        if (o + 1 < cap) out[o++] = '/';
        for (const char* q = chain[i]->name; *q && o + 1 < cap; q++) out[o++] = *q;
    }
    out[o] = 0;
}

static struct cgroup* task_cg(const struct task* t) {
    struct cgroup* cg = (struct cgroup*)t->cgroup;
    return cg ? cg : &g_root;
}

/* ---- membership ------------------------------------------------------------ */

struct scan { struct cgroup* cg; int procs_only; struct wbuf* w;
              uint64_t tasks, mem, cpu_ms; int alive;
              int pids[64]; int npids;            /* processes (leaders) */
              int all[256]; int nall; };          /* every task, threads too */

static void scan_fn(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct scan* s = (struct scan*)ctx;
    if (t->state == TASK_DEAD || task_cg(t) != s->cg) return;
    s->tasks++;
    if (s->nall < 256) s->all[s->nall++] = t->pid;
    s->cpu_ms += t->cpu_ms;
    uint64_t priv = 0; int owns = 0;
    task_mem_bytes(t, &priv, NULL, &owns);
    s->mem += priv;
    int is_leader = !(t->tgid && t->tgid != t->pid);
    if (s->w && (!s->procs_only || is_leader)) { w_u(s->w, (uint64_t)t->pid); w_s(s->w, "\n"); }
    if (is_leader && s->npids < 64) s->pids[s->npids++] = t->pid;
    s->alive = 1;
}

static void cg_scan(struct cgroup* cg, struct scan* s, struct wbuf* w, int procs_only) {
    s->cg = cg; s->w = w; s->procs_only = procs_only;
    s->tasks = s->mem = s->cpu_ms = 0; s->alive = 0; s->npids = 0; s->nall = 0;
    task_for_each(scan_fn, s);
}

struct move { int tgid; int tid; struct cgroup* to; int moved; };
static void move_fn(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct move* m = (struct move*)ctx;
    if (t->state == TASK_DEAD) return;
    if (m->tid ? t->pid == m->tid : task_tgid(t) == m->tgid) {
        ((struct task*)t)->cgroup = cg_is_root(m->to) ? NULL : m->to;
        m->moved++;
    }
}

/* ---- control files: read -------------------------------------------------- */

static void gen(struct cgroup* cg, int k, struct wbuf* w) {
    struct scan s;
    switch (k) {
    case K_CONTROLLERS: case K_SUBTREE: {
        uint32_t m = (k == K_CONTROLLERS) ? cg_available(cg) : cg->subtree;
        int first = 1;
        for (int i = 0; i < CTL_N; i++)
            if (m & (1u << i)) { if (!first) w_s(w, " "); w_s(w, g_ctl[i]); first = 0; }
        w_s(w, "\n");
        return;
    }
    case K_PROCS:   cg_scan(cg, &s, w, 1); return;
    case K_THREADS: cg_scan(cg, &s, w, 0); return;
    case K_TYPE:    w_s(w, "domain\n"); return;
    case K_EVENTS:
        cg_scan(cg, &s, NULL, 0);
        w_s(w, "populated "); w_u(w, s.alive ? 1 : 0);
        w_s(w, "\nfrozen "); w_u(w, (uint64_t)cg->frozen); w_s(w, "\n");
        return;
    case K_STAT:
        w_s(w, "nr_descendants "); w_u(w, (uint64_t)cg->nchildren);
        w_s(w, "\nnr_dying_descendants 0\n");
        return;
    case K_FREEZE: w_u(w, (uint64_t)cg->frozen); w_s(w, "\n"); return;
    case K_CPU_STAT:
        cg_scan(cg, &s, NULL, 0);
        w_s(w, "usage_usec "); w_u(w, s.cpu_ms * 1000);
        w_s(w, "\nuser_usec "); w_u(w, s.cpu_ms * 1000);
        w_s(w, "\nsystem_usec 0\nnr_periods 0\nnr_throttled 0\nthrottled_usec 0\n");
        return;
    case K_MEM_CURRENT: case K_MEM_PEAK:
        cg_scan(cg, &s, NULL, 0);
        if (s.mem > cg->mem_peak) cg->mem_peak = s.mem;
        w_u(w, k == K_MEM_CURRENT ? s.mem : cg->mem_peak); w_s(w, "\n");
        return;
    case K_MEM_SWAP_CUR: w_s(w, "0\n"); return;
    case K_MEM_EVENTS:  w_s(w, "low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n"); return;
    case K_MEM_STAT:
        cg_scan(cg, &s, NULL, 0);
        w_s(w, "anon "); w_u(w, s.mem); w_s(w, "\nfile 0\nkernel 0\nsock 0\nshmem 0\n");
        return;
    case K_PIDS_CURRENT: cg_scan(cg, &s, NULL, 0); w_u(w, s.tasks); w_s(w, "\n"); return;
    case K_IO_STAT: return;
    case K_CPUSET_CPUS_EFF: {
        int n = smp_ncpus();
        w_s(w, "0"); if (n > 1) { w_s(w, "-"); w_u(w, (uint64_t)(n - 1)); } w_s(w, "\n");
        return;
    }
    case K_CPUSET_MEMS_EFF: w_s(w, "0\n"); return;
    case K_KILL: return;                         /* write-only on Linux too */
    default:
        /* a stored value; an empty io.max is an empty file, as on Linux */
        if (cg->val[k][0]) { w_s(w, cg->val[k]); w_s(w, "\n"); }
        else if (k != K_IO_MAX) w_s(w, "\n");
        return;
    }
}

static ssize_t cg_read(struct file* f, void* buf, size_t n, uint64_t off) {
    struct cgfile* cf = (struct cgfile*)f->inode->private;
    if (!cf) return -1;
    struct wbuf w = { (char*)kmalloc(4096), 0, 4096 };
    if (!w.p) return -1;
    w.p[0] = 0;
    gen(cf->cg, cf->kind, &w);
    ssize_t r = 0;
    if (off < w.n) {
        size_t take = w.n - (size_t)off;
        if (take > n) take = n;
        for (size_t i = 0; i < take; i++) ((char*)buf)[i] = w.p[off + i];
        r = (ssize_t)take;
    }
    kfree(w.p);
    return r;
}

/* ---- control files: write ------------------------------------------------- */

static int parse_int(const char* s, long* out) {
    long v = 0; int any = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    *out = v;
    return any ? 0 : -1;
}

static int set_subtree(struct cgroup* cg, const char* s) {
    uint32_t m = cg->subtree, avail = cg_available(cg);
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        char sign = *s++;
        if (sign != '+' && sign != '-') return -22;            /* EINVAL */
        char word[16]; int i = 0;
        while (*s && *s != ' ' && *s != '\n' && i < 15) word[i++] = *s++;
        word[i] = 0;
        int c = -1;
        for (int j = 0; j < CTL_N; j++) if (s_eq(word, g_ctl[j])) c = j;
        if (c < 0) return -22;
        if (sign == '+') {
            if (!(avail & (1u << c))) return -2;               /* ENOENT, as Linux */
            m |= 1u << c;
        } else {
            m &= ~(1u << c);
        }
    }
    cg->subtree = m;
    return 0;
}

static void freeze(struct cgroup* cg, int on) {
    struct scan s;
    cg_scan(cg, &s, NULL, 0);
    cg->frozen = on;
    /* Every task, threads included: stop/cont act on one task each. */
    for (int i = 0; i < s.nall; i++) {
        if (on) task_stop(s.all[i]); else task_cont(s.all[i]);
    }
}

static ssize_t cg_write(struct file* f, const void* buf, size_t n, uint64_t off) {
    (void)off;
    struct cgfile* cf = (struct cgfile*)f->inode->private;
    if (!cf || !g_kind[cf->kind].rw) return -1;
    struct cgroup* cg = cf->cg;
    char s[CG_VAL + 16];
    size_t m = n < sizeof s - 1 ? n : sizeof s - 1;
    for (size_t i = 0; i < m; i++) s[i] = ((const char*)buf)[i];
    s[m] = 0;
    while (m && (s[m - 1] == '\n' || s[m - 1] == ' ')) s[--m] = 0;

    switch (cf->kind) {
    case K_SUBTREE:
        return set_subtree(cg, s) == 0 ? (ssize_t)n : -1;
    case K_PROCS: case K_THREADS: {
        long pid;
        if (parse_int(s, &pid) != 0) return -1;
        struct task* me = task_current();
        if (pid == 0 && me) pid = cf->kind == K_PROCS ? task_tgid(me) : me->pid;
        struct task* t = task_find((int)pid);
        if (!t) return -1;
        struct move mv = { cf->kind == K_PROCS ? task_tgid(t) : 0,
                           cf->kind == K_THREADS ? (int)pid : 0, cg, 0 };
        task_for_each(move_fn, &mv);
        return mv.moved ? (ssize_t)n : -1;
    }
    case K_KILL: {
        if (!s_eq(s, "1")) return -1;
        struct scan sc;
        cg_scan(cg, &sc, NULL, 0);
        for (int i = 0; i < sc.npids; i++) task_kill(sc.pids[i]);
        return (ssize_t)n;
    }
    case K_FREEZE:
        if (s_eq(s, "1")) { if (!cg->frozen) freeze(cg, 1); return (ssize_t)n; }
        if (s_eq(s, "0")) { if (cg->frozen) freeze(cg, 0); return (ssize_t)n; }
        return -1;
    case K_TYPE:
        return s_eq(s, "domain") ? (ssize_t)n : -1;   /* threaded mode: not modelled */
    default:
        s_cpy(cg->val[cf->kind], s, CG_VAL);
        return (ssize_t)n;
    }
}

static int cg_close(struct file* f) { (void)f; return 0; }

static const struct file_ops cg_file_ops = { .read = cg_read, .write = cg_write, .readdir = NULL, .close = cg_close };

/* ---- directories ----------------------------------------------------------- */

static int cg_readdir(struct file* f, struct dirent* out) {
    struct cgroup* cg = (struct cgroup*)f->inode->private;
    if (!cg || !out) return -1;
    uint64_t i = f->pos;
    /* control files first (those present on this node), then child nodes */
    for (int k = 0; k < K_N; k++) {
        if (cg_is_root(cg) && !g_kind[k].root) continue;
        if (i-- == 0) {
            s_cpy(out->name, g_kind[k].name, sizeof out->name);
            out->type = INODE_FILE; out->size = 0;
            f->pos++;
            return 1;
        }
    }
    for (struct dentry* c = f->dentry ? f->dentry->children : NULL; c; c = c->sibling) {
        if (!c->inode || c->inode->type != INODE_DIR) continue;
        if (i-- == 0) {
            s_cpy(out->name, c->name, sizeof out->name);
            out->type = INODE_DIR; out->size = 0;
            f->pos++;
            return 1;
        }
    }
    return 0;
}

static const struct file_ops cg_dir_ops = { .read = NULL, .write = NULL, .readdir = cg_readdir, .close = cg_close };

static int cg_lookup(struct inode* dir, const char* name, struct inode** out) {
    struct cgroup* cg = (struct cgroup*)dir->private;
    for (int k = 0; k < K_N; k++) {
        if (cg_is_root(cg) && !g_kind[k].root) continue;
        if (s_eq(name, g_kind[k].name)) { *out = &cg->files[k]; return 0; }
    }
    return -1;
}

static int cg_mkdir(struct inode* dir, const char* name, struct inode** out) {
    struct cgroup* parent = (struct cgroup*)dir->private;
    if (!parent || s_len(name) > CG_NAME) return -1;
    for (int k = 0; k < K_N; k++) if (s_eq(name, g_kind[k].name)) return -2;
    struct cgroup* cg = (struct cgroup*)kcalloc(1, sizeof *cg);
    if (!cg) return -1;
    cg_init(cg, parent, name);
    parent->nchildren++;
    *out = &cg->dir;
    return 0;
}

static int cg_unlink(struct inode* dir, const char* name, struct inode* child) {
    (void)name;
    struct cgroup* parent = (struct cgroup*)dir->private;
    if (!child || child->type != INODE_DIR) return -1;      /* control files stay */
    struct cgroup* cg = (struct cgroup*)child->private;
    if (!cg || cg_is_root(cg) || cg->nchildren > 0) return -2;
    for (int k = 0; k < K_N; k++) if (cg->files[k].opens > 0) return -2;   /* a control file is open */
    struct scan s;
    cg_scan(cg, &s, NULL, 0);
    if (s.alive) return -2;                                 /* EBUSY: members */
    parent->nchildren--;
    bpf_cgroup_gone(cg);                        /* §M90 — its device programs go too */
    kfree(cg);
    return 0;
}

static const struct inode_ops cg_inode_ops = {
    .lookup = cg_lookup,
    .create = NULL,                 /* only the kernel's control files exist */
    .mkdir  = cg_mkdir,
    .unlink = cg_unlink,
};

/* ---- outside API -------------------------------------------------------- */

void cgroup_path_of(const struct task* t, char* out, size_t cap) {
    if (!out || !cap) return;
    if (!t) { s_cpy(out, "/", cap); return; }
    cg_path(task_cg(t), out, cap);
}

static int cgroupfs_mount(struct block_device* dev, struct dentry* mp) {
    (void)dev;
    if (mp->inode) return -1;
    /* The root delegates every controller, as a systemd host's does: runc and
     * dockerd create their nodes below it and expect to enable controllers
     * there without first writing the root's subtree_control. */
    if (!g_root_ready) { cg_init(&g_root, NULL, ""); g_root.subtree = CTL_ALL; g_root_ready = 1; }
    mp->inode = &g_root.dir;
    return 0;
}

static struct fs_type cgroupfs_type = {
    .name  = "cgroup2",
    .mount = cgroupfs_mount,
    .next  = NULL,
};

/* After ramfs (link order: cgroupfs.c follows ramfs.c in the Makefile). */
static int cgroupfs_module_init(void) {
    if (vfs_register_fs(&cgroupfs_type) != 0) return -1;
    vfs_mkdir("/sys");
    vfs_mkdir("/sys/fs");
    vfs_mkdir("/sys/fs/cgroup");
    return vfs_mount("cgroup2", "/sys/fs/cgroup", NULL);
}
MODULE("cgroupfs", "fs", cgroupfs_module_init);

/* ---- `cgrouptest` — the contract above, each step able to fail ------------- */
#include "shellcmd.h"
static int ct_write(const char* path, const char* text) {
    struct file* f = vfs_open(path, VFS_WRONLY);
    if (!f) return -1;
    size_t n = s_len(text);
    ssize_t r = vfs_write(f, text, n);
    vfs_close(f);
    return r == (ssize_t)n ? 0 : -1;
}
static int ct_read(const char* path, char* out, size_t cap) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    ssize_t r = vfs_read(f, out, cap - 1);
    vfs_close(f);
    out[r > 0 ? r : 0] = 0;
    return r < 0 ? -1 : 0;
}
static void cmd_cgrouptest(const char* args) {
    (void)args;
    struct task* me = task_current();
    char pid[16], buf[256], path[128];
    struct wbuf w = { pid, 0, sizeof pid };
    pid[0] = 0;
    w_u(&w, (uint64_t)task_tgid(me));
    int ok = 1;
    #define CT(cond, what) do { int _c = (cond); kprintf("cgroup: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    vfs_unlink("/sys/fs/cgroup/cgtest/sub");
    vfs_unlink("/sys/fs/cgroup/cgtest");
    CT(ct_read("/sys/fs/cgroup/cgroup.controllers", buf, sizeof buf) == 0 &&
       s_eq(buf, "cpuset cpu io memory pids\n"), "root controllers");
    CT(vfs_mkdir("/sys/fs/cgroup/cgtest") == 0, "mkdir cgtest");
    CT(ct_read("/sys/fs/cgroup/cgtest/memory.max", buf, sizeof buf) == 0 && s_eq(buf, "max\n"),
       "memory.max defaults to max");
    CT(ct_write("/sys/fs/cgroup/cgtest/memory.max", "104857600") == 0 &&
       ct_read("/sys/fs/cgroup/cgtest/memory.max", buf, sizeof buf) == 0 && s_eq(buf, "104857600\n"),
       "memory.max stored");
    CT(ct_write("/sys/fs/cgroup/cgtest/cgroup.procs", pid) == 0, "move self into cgtest");
    cgroup_path_of(me, path, sizeof path);
    CT(s_eq(path, "/cgtest"), "task's cgroup is /cgtest");
    CT(ct_read("/sys/fs/cgroup/cgtest/pids.current", buf, sizeof buf) == 0 && buf[0] >= '1',
       "pids.current counts the member");
    CT(vfs_unlink("/sys/fs/cgroup/cgtest") != 0, "rmdir refused while a member is inside");
    CT(ct_write("/sys/fs/cgroup/cgtest/cgroup.subtree_control", "+memory +pids") == 0 &&
       vfs_mkdir("/sys/fs/cgroup/cgtest/sub") == 0 &&
       ct_read("/sys/fs/cgroup/cgtest/sub/cgroup.controllers", buf, sizeof buf) == 0 &&
       s_eq(buf, "memory pids\n"), "subtree_control delegates to a child");
    CT(ct_write("/sys/fs/cgroup/cgtest/cgroup.subtree_control", "+bogus") != 0, "unknown controller refused");
    CT(ct_write("/sys/fs/cgroup/cgroup.procs", pid) == 0, "move self back to the root");
    cgroup_path_of(me, path, sizeof path);
    CT(s_eq(path, "/"), "task's cgroup is / again");
    CT(vfs_unlink("/sys/fs/cgroup/cgtest") != 0, "rmdir refused while a child node exists");
    CT(vfs_unlink("/sys/fs/cgroup/cgtest/sub") == 0 && vfs_unlink("/sys/fs/cgroup/cgtest") == 0,
       "rmdir of empty nodes");
    #undef CT
    kprintf("cgroup: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(cgrouptest) = { "cgrouptest", "", "cgroup v2: nodes, membership, delegation, rmdir rules",
                          SHELL_G_TEST, cmd_cgrouptest, SHELL_P_ANY };

/* §M90 — for bpf.c: the cgroup a directory inode IS (NULL for any other
 * inode), a cgroup's parent (NULL at the root), and a task's cgroup. */
void* cgroupfs_of_inode(struct inode* in) {
    return (in && in->ops == &cg_dir_ops) ? in->private : NULL;
}
void* cgroupfs_parent(void* cg) {
    struct cgroup* c = (struct cgroup*)cg;
    return (c && !cg_is_root(c)) ? c->parent : NULL;
}
void* cgroupfs_task_node(const struct task* t) { return task_cg(t); }
