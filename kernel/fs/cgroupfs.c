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
 * WHAT IS ENFORCED (§M90 rung 3, 2026-10-08) — see "ENFORCEMENT" below:
 * pids.max (fork/clone refused with EAGAIN), cpu.max (quota per period: a
 * node that spent its quota is not scheduled until the period ends) and
 * memory.max (a Linux-style CHARGE MODEL — charged at allocation, uncharged
 * at free — and an OOM kill inside the node, SIGKILL = exit 137, counted in
 * memory.events).  Limits are hierarchical; current/peak/stat figures count
 * the whole subtree, as on Linux.
 *
 * WHAT IT IS NOT YET — said here so a pass is not read as more than it is:
 * cpu.weight, memory.high/low/min, memory.swap.max, io.* and cpuset.* are
 * stored and read back but NOT enforced (a write is accepted so runc and
 * dockerd can configure a container; nothing throttles on them).  memory.max
 * has no reclaim stage: Linux first shrinks the page cache and swaps, here
 * the first answer to "over the limit" is the OOM kill.  Lowering memory.max
 * below the current charge kills at the next charge, not at the write.
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
#include "fd.h"
#include "task.h"
#include "kmalloc.h"
#include "printf.h"
#include "percpu.h"
#include "module.h"
#include "lock.h"
#include "timer.h"
#include "klog.h"
#include "fsnotify.h"
#include "pmm.h"
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
    K_PIDS_MAX, K_PIDS_CURRENT, K_PIDS_EVENTS,
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
    [K_PIDS_EVENTS]   = { "pids.events",             0, 0, NULL },
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

    /* §M90 rung 3 — the parsed limits and their enforcement state.  -1 =
     * "max" (no limit).  The text in val[] is what reads back; these are what
     * the hooks consult, so a hot path never parses. */
    int64_t        pids_max;
    uint64_t       pids_ev_max;          /* pids.events "max": forks refused  */
    int64_t        mem_max;              /* bytes, page-aligned               */
    uint64_t       mem_charged;          /* bytes charged here + below (exact) */
    uint16_t       memcg_id;             /* this node's charge id (0 = none)   */
    int            oom_tgid;             /* an OOM kill under way, or 0       */
    uint64_t       mem_ev_max, mem_ev_oom, mem_ev_oom_kill;
    spinlock_t     cpu_lock;             /* the cpu.max state below           */
    int64_t        cpu_quota_us;
    uint64_t       cpu_period_us;
    uint64_t       cpu_period_start_ms;
    uint64_t       cpu_used_us;          /* in the current period             */
    int            cpu_throttled;
    uint64_t       cpu_throttle_start_ms;
    uint64_t       cpu_nr_periods, cpu_nr_throttled, cpu_throttled_us;
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
    cg->pids_max = -1;
    cg->mem_max = -1;
    cg->cpu_quota_us = -1;
    cg->cpu_period_us = 100000;
    spinlock_t z = SPINLOCK_INIT;
    cg->cpu_lock = z;
}

/* Is node `c` inside the subtree rooted at `anc` (itself included)? */
static int cg_within(const struct cgroup* c, const struct cgroup* anc) {
    for (; c; c = c->parent) if (c == anc) return 1;
    return 0;
}

/* A control file's content changed by itself (an event counter, populated):
 * tell anyone watching it with inotify — containerd watches memory.events to
 * learn of an OOM kill, which is how `docker inspect` comes to say OOMKilled. */
static void cg_changed(struct cgroup* cg, int k) {
    fsnotify_inode(&cg->files[k], FSN_MODIFY, NULL);
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

/* §M90 rung 3 — the SUBTREE totals (Linux's pids.current, memory.current,
 * cpu.stat and cgroup.events count descendants too), small enough for the
 * stack of a fault path, plus the OOM killer's candidate: the process holding
 * the most private memory, skipping oom_score_adj -1000 ("never kill me"). */
struct tally { struct cgroup* cg; uint64_t tasks, mem, cpu_ms;
               int best_tgid; uint64_t best_mem; int best_adj; };

static void tally_fn(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct tally* a = (struct tally*)ctx;
    if (t->state == TASK_DEAD || !cg_within(task_cg(t), a->cg)) return;
    a->tasks++;
    a->cpu_ms += t->cpu_ms;
    uint64_t priv = 0; int owns = 0;
    task_mem_bytes(t, &priv, NULL, &owns);
    a->mem += priv;
    if (owns && t->oom_score_adj > -1000 && !t->kill_pending) {
        /* Linux's badness: usage plus oom_score_adj thousandths of the limit;
         * here the adj only breaks ties toward the more willing victim. */
        if (!a->best_tgid || priv > a->best_mem ||
            (priv == a->best_mem && t->oom_score_adj > a->best_adj)) {
            a->best_tgid = task_tgid(t); a->best_mem = priv; a->best_adj = t->oom_score_adj;
        }
    }
}

static void cg_tally(struct cgroup* cg, struct tally* a) {
    a->cg = cg; a->tasks = a->mem = a->cpu_ms = 0;
    a->best_tgid = 0; a->best_mem = 0; a->best_adj = -1000;
    task_for_each(tally_fn, a);
}

struct move { int tgid; int tid; struct cgroup* to; int moved; };
static void move_fn(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct move* m = (struct move*)ctx;
    if (t->state == TASK_DEAD) return;
    if (m->tid ? t->pid == m->tid : task_tgid(t) == m->tgid) {
        ((struct task*)t)->cgroup = cg_is_root(m->to) ? NULL : m->to;
        m->moved++;
        /* Its charges stay where they were made (cgroup v2 does not move
         * charges with a task; new allocations charge the new node). */
    }
}

/* ---- control files: read -------------------------------------------------- */

static void cpu_period_roll(struct cgroup* c, uint64_t now);

static void gen(struct cgroup* cg, int k, struct wbuf* w) {
    struct scan s;
    struct tally a;
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
        cg_tally(cg, &a);
        w_s(w, "populated "); w_u(w, a.tasks ? 1 : 0);
        w_s(w, "\nfrozen "); w_u(w, (uint64_t)cg->frozen); w_s(w, "\n");
        return;
    case K_STAT:
        w_s(w, "nr_descendants "); w_u(w, (uint64_t)cg->nchildren);
        w_s(w, "\nnr_dying_descendants 0\n");
        return;
    case K_FREEZE: w_u(w, (uint64_t)cg->frozen); w_s(w, "\n"); return;
    case K_CPU_STAT: {
        cg_tally(cg, &a);
        uint64_t np, nt, tu;
        uint32_t fl = spin_lock_irqsave(&cg->cpu_lock);
        if (cg->cpu_quota_us >= 0) cpu_period_roll(cg, timer_ticks_ms());
        np = cg->cpu_nr_periods; nt = cg->cpu_nr_throttled; tu = cg->cpu_throttled_us;
        if (cg->cpu_throttled) tu += (timer_ticks_ms() - cg->cpu_throttle_start_ms) * 1000;
        spin_unlock_irqrestore(&cg->cpu_lock, fl);
        /* This kernel does not split user from system time per task: all of
         * it is reported as user, and system_usec as 0 — stated, not hidden. */
        w_s(w, "usage_usec "); w_u(w, a.cpu_ms * 1000);
        w_s(w, "\nuser_usec "); w_u(w, a.cpu_ms * 1000);
        w_s(w, "\nsystem_usec 0\nnr_periods "); w_u(w, np);
        w_s(w, "\nnr_throttled "); w_u(w, nt);
        w_s(w, "\nthrottled_usec "); w_u(w, tu);
        w_s(w, "\nnr_bursts 0\nburst_usec 0\n");
        return;
    }
    case K_MEM_CURRENT: case K_MEM_PEAK:
        w_u(w, k == K_MEM_CURRENT ? __atomic_load_n(&cg->mem_charged, __ATOMIC_RELAXED)
                                  : cg->mem_peak);
        w_s(w, "\n");
        return;
    case K_MEM_SWAP_CUR: w_s(w, "0\n"); return;
    case K_MEM_EVENTS:
        w_s(w, "low 0\nhigh 0\nmax "); w_u(w, cg->mem_ev_max);
        w_s(w, "\noom "); w_u(w, cg->mem_ev_oom);
        w_s(w, "\noom_kill "); w_u(w, cg->mem_ev_oom_kill);
        w_s(w, "\noom_group_kill 0\n");
        return;
    case K_MEM_STAT: {
        /* anon = what the members' address spaces privately hold (walked);
         * file = the rest of the charge (page cache, memfd pages, frames
         * shared copy-on-write).  The split is a measurement beside an
         * exact total, so it is a sample, not a ledger. */
        cg_tally(cg, &a);
        uint64_t ch = cg_is_root(cg) ? a.mem : __atomic_load_n(&cg->mem_charged, __ATOMIC_RELAXED);
        uint64_t anon = a.mem < ch ? a.mem : ch;
        w_s(w, "anon "); w_u(w, anon); w_s(w, "\nfile "); w_u(w, ch - anon);
        w_s(w, "\nkernel 0\nsock 0\nshmem 0\n");
        return;
    }
    case K_PIDS_CURRENT: cg_tally(cg, &a); w_u(w, a.tasks); w_s(w, "\n"); return;
    case K_PIDS_EVENTS: w_s(w, "max "); w_u(w, cg->pids_ev_max); w_s(w, "\n"); return;
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

/* "max" -> -1; else a non-negative integer, with memparse's K/M/G/T suffix
 * when `suffix` (memory.max accepts "512M", as on Linux).  -2 = malformed. */
static int64_t parse_limit(const char* s, int suffix) {
    while (*s == ' ' || *s == '\t') s++;
    if (s_eq(s, "max")) return -1;
    int64_t v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); any = 1; }
    if (!any) return -2;
    if (suffix) {
        int sh = 0;
        switch (*s) {
        case 'k': case 'K': sh = 10; s++; break;
        case 'm': case 'M': sh = 20; s++; break;
        case 'g': case 'G': sh = 30; s++; break;
        case 't': case 'T': sh = 40; s++; break;
        }
        v <<= sh;
    }
    return *s ? -2 : v;
}

static void put_limit(char* out, int64_t v) {
    if (v < 0) { s_cpy(out, "max", CG_VAL); return; }
    struct wbuf w = { out, 0, CG_VAL };
    out[0] = 0;
    w_u(&w, (uint64_t)v);
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
    case K_PIDS_MAX: {
        int64_t v = parse_limit(s, 0);
        if (v == -2) return -1;
        cg->pids_max = v;
        put_limit(cg->val[K_PIDS_MAX], v);
        return (ssize_t)n;
    }
    case K_MEM_MAX: {
        int64_t v = parse_limit(s, 1);
        if (v == -2) return -1;
        if (v >= 0) v &= ~(int64_t)4095;          /* Linux rounds down to a page */
        cg->mem_max = v;          /* a node already above it OOMs at its next charge */
        put_limit(cg->val[K_MEM_MAX], v);
        return (ssize_t)n;
    }
    case K_CPU_MAX: {
        /* "$MAX $PERIOD", "$MAX" or "max [$PERIOD]"; period 1 ms .. 1 s and
         * a quota of at least 1 ms, Linux's bounds. */
        char q[24]; int i = 0;
        const char* p = s;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && i < 23) q[i++] = *p++;
        q[i] = 0;
        int64_t quota = parse_limit(q, 0);
        if (quota == -2 || (quota >= 0 && quota < 1000)) return -1;
        uint64_t period = cg->cpu_period_us;
        while (*p == ' ') p++;
        if (*p) {
            int64_t pv = parse_limit(p, 0);
            if (pv < 1000 || pv > 1000000) return -1;
            period = (uint64_t)pv;
        }
        uint32_t fl = spin_lock_irqsave(&cg->cpu_lock);
        cg->cpu_quota_us = quota;
        cg->cpu_period_us = period;
        cg->cpu_period_start_ms = 0;              /* a fresh period from the next charge */
        cg->cpu_used_us = 0;
        cg->cpu_throttled = 0;
        spin_unlock_irqrestore(&cg->cpu_lock, fl);
        put_limit(cg->val[K_CPU_MAX], quota);
        size_t o = s_len(cg->val[K_CPU_MAX]);
        struct wbuf w = { cg->val[K_CPU_MAX], o, CG_VAL };
        w_s(&w, " "); w_u(&w, period);
        return (ssize_t)n;
    }
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

static void memcg_mkdir(struct cgroup* cg);
static void memcg_retire(struct cgroup* cg);
static int cg_mkdir(struct inode* dir, const char* name, struct inode** out) {
    struct cgroup* parent = (struct cgroup*)dir->private;
    if (!parent || s_len(name) > CG_NAME) return -1;
    for (int k = 0; k < K_N; k++) if (s_eq(name, g_kind[k].name)) return -2;
    struct cgroup* cg = (struct cgroup*)kcalloc(1, sizeof *cg);
    if (!cg) return -1;
    cg_init(cg, parent, name);
    memcg_mkdir(cg);                            /* §M90 — its charge id */
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
    memcg_retire(cg);                           /* §M90 — its charges stay in the parent */
    /* §M90 rung 3 — the control files are freed with the node: their
     * inotify watches end now (containerd watches memory.events). */
    for (int k = 0; k < K_N; k++) fsnotify_inode_gone(&cg->files[k]);
    kfree(cg);
    return 0;
}

static const struct inode_ops cg_inode_ops = {
    .lookup = cg_lookup,
    .create = NULL,                 /* only the kernel's control files exist */
    .mkdir  = cg_mkdir,
    .unlink = cg_unlink,
};

/* ---- ENFORCEMENT (§M90 rung 3) ---------------------------------------------
 *
 * Each hook starts at the task's own node and walks up to (not including) the
 * root, so a container inside a parent that runc gave a limit is bounded by
 * both.  A task in the root costs one pointer test.
 *
 * KNOWN RACE, stated: the scheduler reads `t->cgroup` without the cgroup
 * tree's lock.  A task moved out of a node at the very moment that node is
 * removed (rmdir requires it EMPTY, so the move must come first) could be
 * read with the old pointer for one pick.  Nodes are removed by runc after
 * the container is gone, i.e. long after its tasks were last scheduled. */

/* -- pids.max ----------------------------------------------------------------- */

int cgroup_fork_allowed(const struct task* t) {
    if (!t || !t->cgroup) return 1;
    for (struct cgroup* c = (struct cgroup*)t->cgroup; c && !cg_is_root(c); c = c->parent) {
        if (c->pids_max < 0) continue;
        struct tally a;
        cg_tally(c, &a);
        if ((int64_t)a.tasks + 1 > c->pids_max) {
            c->pids_ev_max++;
            cg_changed(c, K_PIDS_EVENTS);
            return 0;
        }
    }
    return 1;
}

/* -- cpu.max -----------------------------------------------------------------
 *
 * CFS bandwidth control, reduced to its contract: in every period of
 * `cpu_period_us` the node's tasks together may run `cpu_quota_us`; once that
 * is spent the node is THROTTLED and none of its tasks is picked until the
 * period ends.  Time is charged in milliseconds of the monotonic clock — the
 * tick's granularity (1 ms on x86, 10 ms on aarch64), so a quota below one
 * tick overshoots by up to a tick per period, and "max 100000" style
 * accounting is exact only to that grain.  An overrun is not carried into the
 * next period (Linux's cpu.max.burst is 0 by default, i.e. the same).
 *
 * A task is throttled wherever it is — including inside a system call holding
 * a sleeping lock.  Linux defers the throttle to the return to user mode; this
 * kernel has no such hook in the scheduler, so a throttled lock holder can
 * delay others by up to one period.  Spinlock holders are never preempted. */

static void cpu_period_roll(struct cgroup* c, uint64_t now) {
    uint64_t per = c->cpu_period_us / 1000;
    if (!per) per = 1;
    if (!c->cpu_period_start_ms) { c->cpu_period_start_ms = now; return; }
    if (now < c->cpu_period_start_ms + per) return;
    uint64_t n = (now - c->cpu_period_start_ms) / per;
    c->cpu_period_start_ms += n * per;
    c->cpu_nr_periods += n;
    c->cpu_used_us = 0;
    if (c->cpu_throttled) {
        c->cpu_throttled_us += (now - c->cpu_throttle_start_ms) * 1000;
        c->cpu_throttled = 0;
    }
}

void cgroup_cpu_charge(struct task* t, uint64_t now) {
    if (!t) return;
    uint64_t from = t->cg_cpu_mark_ms > t->sched_in_ms ? t->cg_cpu_mark_ms : t->sched_in_ms;
    t->cg_cpu_mark_ms = now;
    if (!t->cgroup || now <= from) return;
    uint64_t us = (now - from) * 1000;
    for (struct cgroup* c = (struct cgroup*)t->cgroup; c && !cg_is_root(c); c = c->parent) {
        if (c->cpu_quota_us < 0) continue;
        uint32_t fl = spin_lock_irqsave(&c->cpu_lock);
        cpu_period_roll(c, now);
        c->cpu_used_us += us;
        if (!c->cpu_throttled && c->cpu_used_us >= (uint64_t)c->cpu_quota_us) {
            c->cpu_throttled = 1;
            c->cpu_throttle_start_ms = now;
            c->cpu_nr_throttled++;
        }
        spin_unlock_irqrestore(&c->cpu_lock, fl);
    }
}

int cgroup_cpu_throttled(const struct task* t) {
    if (!t || !t->cgroup) return 0;
    uint64_t now = 0;
    for (struct cgroup* c = (struct cgroup*)t->cgroup; c && !cg_is_root(c); c = c->parent) {
        if (c->cpu_quota_us < 0) continue;
        if (!now) now = timer_ticks_ms();
        uint32_t fl = spin_lock_irqsave(&c->cpu_lock);
        cpu_period_roll(c, now);
        int thr = c->cpu_throttled;
        spin_unlock_irqrestore(&c->cpu_lock, fl);
        if (thr) return 1;
    }
    return 0;
}

/* -- memory.max: THE CHARGE MODEL ----------------------------------------------
 *
 * Linux's memory controller, reduced to its contract, as asked for after the
 * first cut (an estimate measured by walking address spaces) OOM-killed
 * `runc init` in an 8 MiB container (NEXT #18):
 *
 *   - A FRAME IS CHARGED ONCE, WHEN IT IS ALLOCATED, to the cgroup of the task
 *     that allocated it, and UNCHARGED when it is freed — wherever it is
 *     mapped in between.  pmm_alloc_frame_user*() charge; pmm_free_frame()
 *     uncharges.  A per-frame OWNER TABLE (2 bytes a frame, a charge id) is
 *     what makes the uncharge find its cgroup; it is allocated when the first
 *     node is created, so a machine that never makes a cgroup pays nothing.
 *   - A COPY-ON-WRITE share is charged to whoever allocated the frame, the
 *     copy to whoever wrote.  A page-cache page is charged to the task whose
 *     read brought it in — EXCEPT a page read from a memfd: on Linux a
 *     memfd's pages ARE its page cache and mapping one allocates nothing, so
 *     here the cached copy is charged to the memfd's CREATOR (shm.memcg_id,
 *     inode.memcg_as).  That is what lets runc exec its init from a memfd
 *     made outside the container without the container paying for runc.
 *   - Charges do not move with a task (cgroup v2 semantics).  A removed node's
 *     remaining charges stay counted in its parent: its id becomes a
 *     TOMBSTONE that forwards to the parent's id and remembers the residual,
 *     and the id is reused only when the residual is gone and nothing refers
 *     to it any more.
 *   - The counters are HIERARCHICAL: a charge adds to the node and every
 *     ancestor, so memory.current is exact and memory.max is checked with one
 *     comparison per ancestor.
 *
 * Over the limit = OOM: the process with the most private memory in the
 * over-limit node's subtree is killed with SIGKILL (exit 137) — every process
 * of the subtree when memory.oom.group is 1 — and memory.events counts
 * max/oom/oom_kill.  While that kill is under way further charges are allowed
 * (the victim's frames are about to come back).  A victim never gets another
 * frame.  A charge made ON BEHALF of another node (a memfd's page cache) that
 * finds nobody to kill is allowed rather than refused — the reader is not the
 * one over the limit.  When every candidate has oom_score_adj -1000 and the
 * allocating task is inside the node, nothing is killed and the charge fails.
 *
 * What is NOT charged, stated: kernel memory (page tables, the heap, socket
 * buffers — Linux charges kmem too), and swap (memory.swap.current reads 0: an
 * evicted page is uncharged when its frame is freed). */

#define MEMCG_IDS 4096
struct memcg_slot { struct cgroup* cg; uint16_t fwd; uint8_t used; int refs; uint64_t residual; };
static struct memcg_slot g_ids[MEMCG_IDS];       /* [0] = the root: never charged */
static uint16_t* volatile g_owner;               /* charge id per frame, by pfn - base */
static uint32_t g_owner_n;
static int g_owner_failed, g_ids_full_said;
static spinlock_t g_mc_lock = SPINLOCK_INIT;

/* Called from mkdir (task context): the owner table, once. */
static void memcg_table_init(void) {
    if (g_owner || g_owner_failed) return;
    uint32_t n = pmm_nr_frames - pmm_pfn_base;
    uint16_t* t = (uint16_t*)kcalloc(n, sizeof *t);
    if (!t) {
        g_owner_failed = 1;
        klog(KLOG_WARN, "cgroup", "memory accounting OFF: no room for the owner table "
             "(%u frames) - memory.max will not be enforced\n", (unsigned)n);
        return;
    }
    g_owner_n = n;
    __atomic_store_n(&g_owner, t, __ATOMIC_RELEASE);
}

/* A free charge id for `cg`, or 0.  Caller holds g_mc_lock. */
static uint16_t memcg_id_new_locked(struct cgroup* cg) {
    for (int i = 1; i < MEMCG_IDS; i++) {
        if (g_ids[i].used) continue;
        g_ids[i].used = 1; g_ids[i].cg = cg; g_ids[i].fwd = 0;
        g_ids[i].refs = 0; g_ids[i].residual = 0;
        return (uint16_t)i;
    }
    return 0;
}

/* Free tombstones that nothing needs any more, up the forward chain.  A
 * tombstone holds a reference on the id it forwards to, so a parent's id is
 * never reused while a child's tombstone still points at it. */
static void memcg_try_free_locked(uint16_t id) {
    while (id && g_ids[id].used && !g_ids[id].cg && !g_ids[id].residual && !g_ids[id].refs) {
        uint16_t nx = g_ids[id].fwd;
        g_ids[id].used = 0;
        if (nx && g_ids[nx].refs > 0) g_ids[nx].refs--;
        id = nx;
    }
}

/* The live node an id charges to now (a tombstone's nearest live ancestor),
 * or NULL for the root.  Caller holds g_mc_lock. */
static struct cgroup* memcg_live_locked(uint16_t id) {
    for (int guard = 0; id && guard < MEMCG_IDS; guard++) {
        if (!g_ids[id].used) return NULL;
        if (g_ids[id].cg) return g_ids[id].cg;
        id = g_ids[id].fwd;
    }
    return NULL;
}

static void charge_chain_locked(struct cgroup* cg, int64_t delta) {
    for (struct cgroup* c = cg; c && !cg_is_root(c); c = c->parent) {
        if (delta < 0 && c->mem_charged < (uint64_t)-delta) c->mem_charged = 0;
        else c->mem_charged += (uint64_t)delta;
        if (delta > 0 && c->mem_charged > c->mem_peak) c->mem_peak = c->mem_charged;
    }
}

int cgroup_memcg_hold_current(void) {
    struct task* me = task_current();
    struct cgroup* cg = me ? (struct cgroup*)me->cgroup : NULL;
    if (!cg || !cg->memcg_id) return 0;
    uint32_t fl = spin_lock_irqsave(&g_mc_lock);
    uint16_t id = cg->memcg_id;
    g_ids[id].refs++;
    spin_unlock_irqrestore(&g_mc_lock, fl);
    return id;
}

void cgroup_memcg_put(int id) {
    if (id <= 0 || id >= MEMCG_IDS) return;
    uint32_t fl = spin_lock_irqsave(&g_mc_lock);
    if (g_ids[id].refs > 0) g_ids[id].refs--;
    memcg_try_free_locked((uint16_t)id);
    spin_unlock_irqrestore(&g_mc_lock, fl);
}

void cgroup_mem_set_owner(pmm_phys_t f, int id) {
    uint16_t* t = g_owner;
    uint32_t pfn = (uint32_t)(f >> 12);
    if (!t || id <= 0 || pfn < pmm_pfn_base || pfn - pmm_pfn_base >= g_owner_n) return;
    __atomic_store_n(&t[pfn - pmm_pfn_base], (uint16_t)id, __ATOMIC_RELAXED);
}

void cgroup_mem_uncharge(int id) {
    if (id <= 0) return;
    uint32_t fl = spin_lock_irqsave(&g_mc_lock);
    uint16_t i = (uint16_t)id;
    for (int guard = 0; i && guard < MEMCG_IDS; guard++) {
        struct memcg_slot* sl = &g_ids[i];
        if (!sl->used) break;
        if (sl->cg) { charge_chain_locked(sl->cg, -4096); break; }
        /* A tombstone: its residual, then up to whoever counts it now. */
        if (sl->residual >= 4096) sl->residual -= 4096; else sl->residual = 0;
        uint16_t nx = sl->fwd;
        memcg_try_free_locked(i);
        i = nx;
    }
    spin_unlock_irqrestore(&g_mc_lock, fl);
}

void cgroup_mem_free_frame(pmm_phys_t f) {
    uint16_t* t = g_owner;
    if (!t) return;
    uint32_t pfn = (uint32_t)(f >> 12);
    if (pfn < pmm_pfn_base || pfn - pmm_pfn_base >= g_owner_n) return;
    uint16_t id = __atomic_exchange_n(&t[pfn - pmm_pfn_base], (uint16_t)0, __ATOMIC_ACQ_REL);
    if (id) cgroup_mem_uncharge(id);
}

/* A new node: the owner table on first use, and its charge id. */
static void memcg_mkdir(struct cgroup* cg) {
    memcg_table_init();
    if (!g_owner) return;
    uint32_t fl = spin_lock_irqsave(&g_mc_lock);
    cg->memcg_id = memcg_id_new_locked(cg);
    spin_unlock_irqrestore(&g_mc_lock, fl);
    if (!cg->memcg_id && !g_ids_full_said) {
        g_ids_full_said = 1;
        klog(KLOG_WARN, "cgroup", "%d charge ids in use - new nodes are not memory-accounted\n",
             MEMCG_IDS - 1);
    }
}

/* A node retires its id (rmdir): its residual stays counted in the parent. */
static void memcg_retire(struct cgroup* cg) {
    if (!cg->memcg_id) return;
    uint32_t fl = spin_lock_irqsave(&g_mc_lock);
    struct memcg_slot* sl = &g_ids[cg->memcg_id];
    sl->cg = NULL;
    sl->fwd = (cg->parent && !cg_is_root(cg->parent)) ? cg->parent->memcg_id : 0;
    if (sl->fwd) g_ids[sl->fwd].refs++;
    sl->residual = cg->mem_charged;
    memcg_try_free_locked(cg->memcg_id);
    spin_unlock_irqrestore(&g_mc_lock, fl);
    cg->memcg_id = 0;
}

int cgroup_oom_victim_self(void) {
    struct task* me = task_current();
    return me && me->kill_pending && me->group_exit_plus1 == 137 + 1;
}

struct oomall { struct cgroup* cg; int tg[64]; int n; };
static void oomall_fn(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct oomall* o = (struct oomall*)ctx;
    if (t->state == TASK_DEAD || !cg_within(task_cg(t), o->cg)) return;
    if (t->tgid && t->tgid != t->pid) return;          /* threads go with their group */
    if (t->oom_score_adj <= -1000 || o->n >= 64) return;
    o->tg[o->n++] = t->pid;
}

/* Over the limit in `c`: kill.  Returns 1 when the CURRENT process is a
 * victim (its charge must fail), 0 when it is not, -1 when nothing could be
 * killed. */
static int oom_kill_in(struct cgroup* c) {
    struct task* me = task_current();
    struct tally a;
    cg_tally(c, &a);
    char path[96];
    cg_path(c, path, sizeof path);
    int group = s_eq(c->val[K_MEM_OOM_GROUP], "1");
    if (!group && !a.best_tgid) {
        klog(KLOG_WARN, "cgroup", "%s: memory.max %lu reached, nobody to kill "
             "(no member, or every one oom_score_adj -1000)\n", path, (unsigned long)c->mem_max);
        cg_changed(c, K_MEM_EVENTS);
        return -1;
    }
    c->mem_ev_oom++;
    int self_dies = 0;
    if (group) {
        struct oomall* o = (struct oomall*)kmalloc(sizeof *o);
        if (o) {
            o->cg = c; o->n = 0;
            task_for_each(oomall_fn, o);
            for (int i = 0; i < o->n; i++) {
                if (me && o->tg[i] == task_tgid(me)) self_dies = 1;
                task_kill_group_oom(o->tg[i], 137);
                c->mem_ev_oom_kill++;
            }
            if (o->n) c->oom_tgid = o->tg[0];
            kfree(o);
        }
    } else {
        struct task* v = task_find(a.best_tgid);
        klog(KLOG_WARN, "cgroup", "%s: memory.max %lu reached (%lu charged) - OOM kill "
             "pid %d '%s' (%lu KiB private)\n", path, (unsigned long)c->mem_max,
             (unsigned long)c->mem_charged, a.best_tgid, v ? v->name : "?",
             (unsigned long)(a.best_mem / 1024));
        task_kill_group_oom(a.best_tgid, 137);
        c->mem_ev_oom_kill++;
        c->oom_tgid = a.best_tgid;
        self_dies = me && a.best_tgid == task_tgid(me);
    }
    /* Every ancestor's memory.events counts what happened below it. */
    for (struct cgroup* p = c; p && !cg_is_root(p); p = p->parent) {
        if (p != c) { p->mem_ev_oom++; p->mem_ev_oom_kill++; }
        cg_changed(p, K_MEM_EVENTS);
    }
    return self_dies;
}

int cgroup_mem_charge(int as, int* out_id) {
    *out_id = 0;
    if (!g_owner) return 1;                          /* no node exists: nothing to charge */
    struct task* me = task_current();
    uint16_t id;
    if (as >= 0) {
        id = (uint16_t)as;                           /* on behalf of a memfd's creator */
    } else {
        if (!me || !me->cgroup) return 1;
        if (cgroup_oom_victim_self()) return 0;
        id = ((struct cgroup*)me->cgroup)->memcg_id;
    }
    if (!id) return 1;
    for (int round = 0; round < 3; round++) {
        uint32_t fl = spin_lock_irqsave(&g_mc_lock);
        struct cgroup* cg = memcg_live_locked(id);
        if (!cg) { spin_unlock_irqrestore(&g_mc_lock, fl); return 1; }   /* the root: unaccounted */
        struct cgroup* over = NULL;
        if (round == 0)
            for (struct cgroup* c = cg; c && !cg_is_root(c); c = c->parent)
                if (c->mem_max >= 0 && c->mem_charged + 4096 > (uint64_t)c->mem_max) { over = c; break; }
        if (!over) {
            charge_chain_locked(cg, 4096);
            *out_id = cg->memcg_id;
            spin_unlock_irqrestore(&g_mc_lock, fl);
            return 1;
        }
        spin_unlock_irqrestore(&g_mc_lock, fl);
        /* Over the limit.  Outside the lock: finding and killing a victim
         * walks the task list and the address spaces. */
        over->mem_ev_max++;
        if (over->oom_tgid) {
            struct task* v = task_find(over->oom_tgid);
            if (v && v->state != TASK_DEAD) continue;      /* a kill under way: charge */
            over->oom_tgid = 0;
        }
        int r = oom_kill_in(over);
        if (r == 1) return 0;                               /* we are the victim */
        if (r < 0 && as < 0 && me && cg_within(task_cg(me), over)) return 0;
        /* someone else dies, or a charge on another node's behalf: charge */
    }
    return 1;
}

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
/* §M90 — clone3(CLONE_INTO_CGROUP): the cgroup a DIRECTORY descriptor names
 * (NULL when it is not one), and a new task placed there before it runs. */
void* cgroupfs_of_fd(int fd) {
    struct ofile* o = fd_get(fd);
    if (!o) return NULL;
    void* cg = (o->kind == FD_VFS && o->file) ? cgroupfs_of_inode(o->file->inode) : NULL;
    ofile_unref(o);
    return cg;
}
void cgroupfs_place(struct task* t, void* cg) {
    if (!t || !cg) return;
    t->cgroup = cg_is_root((struct cgroup*)cg) ? NULL : cg;
}

/* ---- `cglimittest` — §M90 rung 3: each limit made to BITE ------------------
 *
 * A limit that is stored and read back proves nothing about enforcement (that
 * was exactly the state before rung 3), so each check here drives real load
 * into a node and measures what the kernel did to it:
 *   pids.max   — with the node full, cgroup_fork_allowed says no and
 *                pids.events counts it; lifting the limit says yes again.
 *   memory.max — memhog `fill` grows 4 MiB at a time inside an 8 MiB node and
 *                must end with exit code 137, memory.events oom_kill 1.
 *   cpu.max    — a kernel spinner in a node allowed 20 ms per 100 ms must get
 *                about a fifth of the CPU time an unlimited one gets in the
 *                same wall time, and the node must report throttling. */
#include "proc.h"
#include "crash.h"
extern const unsigned char _binary_user_memhog_elf_start[]         __attribute__((weak));
extern const unsigned char _binary_user_memhog_elf_end[]           __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_memhog_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_memhog_aarch64_elf_end[]   __attribute__((weak));

static volatile int g_hog_stop;
static void cg_hog(void) {
    while (!__atomic_load_n(&g_hog_stop, __ATOMIC_ACQUIRE)) { }
}

static int ct_contains(const char* hay, const char* needle) {
    for (; *hay; hay++) {
        const char* a = hay; const char* b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* CPU ms a spinner gets in `ms` of wall time inside node `node` ("" = root). */
static uint64_t hog_cpu(const char* node_procs, unsigned ms) {
    __atomic_store_n(&g_hog_stop, 0, __ATOMIC_RELEASE);
    struct task* h = task_spawn("cg-hog", cg_hog);
    if (!h) return 0;
    int pid = h->pid;
    char p[16]; struct wbuf w = { p, 0, sizeof p }; p[0] = 0; w_u(&w, (uint64_t)pid);
    if (node_procs) ct_write(node_procs, p);
    struct task* t = task_find(pid);
    uint64_t c0 = t ? task_cpu_ms_now(t) : 0;
    task_msleep(ms);
    t = task_find(pid);
    uint64_t c1 = t ? task_cpu_ms_now(t) : 0;
    __atomic_store_n(&g_hog_stop, 1, __ATOMIC_RELEASE);
    task_msleep(50);
    return c1 - c0;
}

static void cmd_cglimittest(const char* args) {
    (void)args;
    struct task* me = task_current();
    char mypid[16], buf[256];
    struct wbuf w = { mypid, 0, sizeof mypid };
    mypid[0] = 0;
    w_u(&w, (uint64_t)task_tgid(me));
    int ok = 1;
    #define CT(cond, what) do { int _c = (cond); kprintf("cglimit: %s %s\n", what, _c ? "ok" : "FAIL"); if (!_c) ok = 0; } while (0)
    vfs_unlink("/sys/fs/cgroup/lim");
    CT(vfs_mkdir("/sys/fs/cgroup/lim") == 0, "mkdir lim");

    /* -- pids.max -- */
    CT(ct_write("/sys/fs/cgroup/lim/cgroup.procs", mypid) == 0, "move self into lim");
    CT(cgroup_fork_allowed(me), "no limit: a fork is allowed");
    CT(ct_write("/sys/fs/cgroup/lim/pids.max", "1") == 0, "pids.max = 1");
    CT(!cgroup_fork_allowed(me), "full node: a fork is refused");
    CT(ct_read("/sys/fs/cgroup/lim/pids.events", buf, sizeof buf) == 0 && s_eq(buf, "max 1\n"),
       "pids.events counts the refusal");
    CT(ct_write("/sys/fs/cgroup/lim/pids.max", "max") == 0 && cgroup_fork_allowed(me),
       "pids.max = max: allowed again");
    CT(ct_write("/sys/fs/cgroup/lim/pids.max", "bogus") != 0, "a malformed pids.max is refused");

    /* -- memory.max -- */
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_memhog_elf_start)             { s = _binary_user_memhog_elf_start;         e = _binary_user_memhog_elf_end; }
    else if (_binary_user_memhog_x86_64_elf_start) { s = _binary_user_memhog_x86_64_elf_start;  e = _binary_user_memhog_x86_64_elf_end; }
    else if (_binary_user_memhog_aarch64_elf_start){ s = _binary_user_memhog_aarch64_elf_start; e = _binary_user_memhog_aarch64_elf_end; }
    CT(ct_write("/sys/fs/cgroup/lim/memory.max", "8M") == 0 &&
       ct_read("/sys/fs/cgroup/lim/memory.max", buf, sizeof buf) == 0 && s_eq(buf, "8388608\n"),
       "memory.max = 8M reads back as 8388608");
    if (s) {
        const char* argv[2] = { "memhog", "fill" };
        int crashes0 = crash_count();
        int pid = proc_spawn_argv("cg-memhog", s, (size_t)(e - s), 2, argv, 0);   /* born in lim */
        ct_write("/sys/fs/cgroup/cgroup.procs", mypid);                             /* we leave */
        int code = -1;
        int r = pid > 0 ? task_wait(pid, &code) : -1;
        kprintf("cglimit: memhog pid %d ended, wait %d, code %d\n", pid, r, code);
        CT(pid > 0 && r >= 0 && code == 137, "memhog over 8M ends with 137 (SIGKILL)");
        /* An OOM kill is a policy, not a crash: no record, no Crash Reports
         * window (the first version filed "unresponsive task reclaimed by
         * force" for it, because it used the forced-kill path). */
        CT(crash_count() == crashes0, "the OOM kill filed no crash record");
        CT(ct_read("/sys/fs/cgroup/lim/memory.events", buf, sizeof buf) == 0 &&
           ct_contains(buf, "oom_kill 1\n") && ct_contains(buf, "\noom 1\n"),
           "memory.events: oom 1, oom_kill 1");
        CT(ct_read("/sys/fs/cgroup/lim/memory.peak", buf, sizeof buf) == 0 &&
           buf[0] >= '1' && buf[0] <= '9', "memory.peak readable");
        kprintf("cglimit: memory.peak %s", buf);
        /* The CHARGE MODEL's other half: every frame the victim held went
         * back through pmm_free_frame and uncharged the node.  A leak here
         * would leave the container "full" after its processes are gone. */
        task_msleep(200);
        long cur = -1;
        if (ct_read("/sys/fs/cgroup/lim/memory.current", buf, sizeof buf) == 0) parse_int(buf, &cur);
        kprintf("cglimit: memory.current after the kill %ld\n", cur);
        CT(cur >= 0 && cur < 262144, "the victim's frames were uncharged (memory.current < 256 KiB)");
    } else {
        kprintf("cglimit: no memhog embedded - memory.max not driven\n");
        ct_write("/sys/fs/cgroup/cgroup.procs", mypid);
    }

    /* -- cpu.max -- */
    CT(ct_write("/sys/fs/cgroup/lim/cpu.max", "20000 100000") == 0 &&
       ct_read("/sys/fs/cgroup/lim/cpu.max", buf, sizeof buf) == 0 && s_eq(buf, "20000 100000\n"),
       "cpu.max = 20000 100000");
    CT(ct_write("/sys/fs/cgroup/lim/cpu.max", "500 100000") != 0, "a quota below 1 ms is refused");
    uint64_t free_ms = hog_cpu(NULL, 1000);
    uint64_t lim_ms  = hog_cpu("/sys/fs/cgroup/lim/cgroup.procs", 1000);
    kprintf("cglimit: spinner got %lu ms unlimited, %lu ms at 20%% (of 1000 ms)\n",
            (unsigned long)free_ms, (unsigned long)lim_ms);
    CT(free_ms >= 500, "the unlimited spinner ran (control)");
    CT(lim_ms >= 120 && lim_ms <= 320, "the limited spinner got about 20%");
    CT(ct_read("/sys/fs/cgroup/lim/cpu.stat", buf, sizeof buf) == 0 && !ct_contains(buf, "nr_throttled 0\n"),
       "cpu.stat reports throttling");
    task_msleep(100);
    CT(vfs_unlink("/sys/fs/cgroup/lim") == 0, "rmdir lim");
    #undef CT
    kprintf("cglimit: %s\n", ok ? "ALL ok" : "FAIL");
}
SHELL_CMD(cglimittest) = { "cglimittest", "", "cgroup v2 limits enforced: pids.max, memory.max (OOM), cpu.max",
                           SHELL_G_TEST, cmd_cglimittest, SHELL_P_ANY };
