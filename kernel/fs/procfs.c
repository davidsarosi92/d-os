/* =============================================================================
 * procfs.c — synthetic /proc filesystem.
 *
 * Same overall mechanism as devfs (M9): each procfs_node hangs off the
 * existing /proc directory dentry as a child file with custom file_ops.
 * The difference is the read path: a procfs file produces its content
 * lazily (on first read after open) by calling the node's `gen`
 * function with a `procfs_writer`.  The buffer is cached in `f->private`
 * for subsequent reads from the same handle, and freed on close.  Re-
 * opening regenerates fresh content.
 *
 * All built-in nodes are listed and registered at the bottom.  When a
 * subsystem grows its own /proc-worthy state, the cleanest path is to
 * add a node here (centralized) and pull the data via the subsystem's
 * public iterator/accessor — see the pattern below for `gen_modules`,
 * `gen_tasks`, etc.
 * ============================================================================= */

#include "procfs.h"
#include "vfs.h"
#include "kmalloc.h"
#include "printf.h"
#include "module.h"
#include "driver.h"
#include "domain.h"     /* §M33 — the placement columns */
#include "drvuser.h"    /* §M33 Tier 2 — a placed driver's pid + restarts */
#include "drvguard.h"   /* §M33 Tier 0 — faults-contained */
#include "task.h"
#include "cred.h"
#include "console.h"
#include "config.h"
#include "timer.h"
#include "klog.h"
#include "pmm.h"
#include "memage.h"
#include "hal_api.h"     /* §M90 — hal_cpu_model */
#include "percpu.h"      /* §M90 — smp_ncpus */
#include "cgroupfs.h"
#include "nsproxy.h"
#include "fd.h"      /* §M90 — fd_nth_open */    /* §M90 — /proc/self/cgroup */
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* Writer.                                                                */
/* ---------------------------------------------------------------------- */

static void pw_grow(struct procfs_writer* w, size_t need_more) {
    if (w->len + need_more <= w->cap) return;
    size_t new_cap = w->cap ? w->cap * 2 : 256;
    while (new_cap < w->len + need_more) new_cap *= 2;
    char* nb = (char*)kmalloc(new_cap);
    if (!nb) return;                            /* silently truncate on OOM */
    if (w->buf) {
        for (size_t i = 0; i < w->len; i++) nb[i] = w->buf[i];
        kfree(w->buf);
    }
    w->buf = nb;
    w->cap = new_cap;
}

void pw_putc(struct procfs_writer* w, char c) {
    pw_grow(w, 1);
    if (w->len < w->cap) w->buf[w->len++] = c;
}

void pw_puts(struct procfs_writer* w, const char* s) {
    if (!s) { pw_puts(w, "(null)"); return; }
    while (*s) pw_putc(w, *s++);
}

void pw_put_uint(struct procfs_writer* w, unsigned int v) {
    char buf[16];
    int n = 0;
    if (v == 0) { pw_putc(w, '0'); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) pw_putc(w, buf[n]);
}

void pw_put_hex(struct procfs_writer* w, unsigned int v, int min_digits) {
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int n = 0;
    if (v == 0 && min_digits == 0) { pw_putc(w, '0'); return; }
    while (v || n < min_digits) { buf[n++] = hex[v & 0xF]; v >>= 4; }
    while (n--) pw_putc(w, buf[n]);
}

/* Format a 64-bit ms value as h:mm:ss.mmm.  Used by /proc/uptime. */
static void pw_put_uptime(struct procfs_writer* w, uint64_t total_ms) {
    uint32_t ms  = (uint32_t)(total_ms % 1000);
    uint32_t sec = (uint32_t)((total_ms / 1000) % 60);
    uint32_t min = (uint32_t)((total_ms / 60000) % 60);
    uint32_t hr  = (uint32_t)(total_ms / 3600000);
    pw_put_uint(w, hr);  pw_putc(w, ':');
    if (min < 10) { pw_putc(w, '0'); } pw_put_uint(w, min); pw_putc(w, ':');
    if (sec < 10) { pw_putc(w, '0'); } pw_put_uint(w, sec); pw_putc(w, '.');
    if (ms < 100) pw_putc(w, '0');
    if (ms < 10)  pw_putc(w, '0');
    pw_put_uint(w, ms);
}

/* ---------------------------------------------------------------------- */
/* file_ops adapter — lazy content generation, sliced reads.              */
/* ---------------------------------------------------------------------- */

struct file_state {
    char*  content;
    size_t size;
    int    pid;                     /* §M90 — the process the path named, 0 = opener */
    /* §M90 — a namespace HANDLE (/proc/<pid>/ns/X): the namespace it named
     * when it was opened, held by reference — what setns(fd) joins and what
     * fstat reports, even after every member has left. */
    int           ns_kind;          /* -1: not a handle */
    struct nsobj* ns_obj;           /* NULL = the initial one (or MNT) */
    int           ns_mnt;           /* NSK_MNT: the VFS mount namespace id */
    uint32_t      ns_ino;
};
static int ns_kind_of_node(const struct procfs_node* n);   /* below */

/* §M90 — see procfs.h.  The target is recorded on the opening task by the
 * path lookup (vfs_proc_alias) and consumed at open, in the same call. */
struct task* procfs_target(void) {
    struct task* t = task_current();
    if (t && t->proc_target_pid > 0) {
        struct task* x = task_find(t->proc_target_pid);
        if (x) return x;
    }
    return t;
}

/* Generate the node's content into a fresh state for `f`. */
static struct file_state* procfs_generate(struct file* f, struct procfs_node* node) {
    struct procfs_writer w = { 0 };
    if (node->gen) node->gen(&w);
    struct file_state* st = (struct file_state*)kcalloc(1, sizeof(*st));
    if (!st) { if (w.buf) kfree(w.buf); return NULL; }
    st->content = w.buf;
    st->size    = w.len;
    struct task* cur = task_current();
    st->pid = (cur && cur->proc_target_pid > 0) ? cur->proc_target_pid : 0;
    st->ns_kind = ns_kind_of_node(node);
    if (st->ns_kind >= 0) {
        struct task* tg = procfs_target();
        int k = st->ns_kind & 0xFF, child = st->ns_kind >> 8;
        st->ns_ino = child ? ns_child_ino_of(tg, k) : ns_ino_of(tg, k);
        st->ns_mnt = tg ? tg->mntns : 0;
        st->ns_obj = child ? ns_get_child(tg, k) : ns_get(tg, k);
        st->ns_kind = k;
    }
    f->private  = st;
    f->inode->size = st->size;                  /* update for stat */
    return st;
}

/* §M90 — content at OPEN (procfs.h always said so; it used to be the first
 * read): the target recorded by this open's lookup is consumed here and
 * cleared, so it can never leak into a later open. */
static int procfs_open(struct file* f) {
    struct task* cur = task_current();
    if (f && f->inode && f->inode->private && !f->private)
        procfs_generate(f, (struct procfs_node*)f->inode->private);
    if (cur) cur->proc_target_pid = 0;
    return 0;
}

/* §M90 — a write to a writable node (oom_score_adj).  Only the process
 * itself or an administrator may change another process's value. */
static ssize_t procfs_write(struct file* f, const void* buf, size_t n, uint64_t off) {
    (void)off;
    if (!f || !f->inode) return -1;
    struct procfs_node* node = (struct procfs_node*)f->inode->private;
    if (!node || !node->write) return -1;
    struct file_state* st = (struct file_state*)f->private;
    struct task* cur = task_current();
    struct task* t = (st && st->pid > 0) ? task_find(st->pid) : cur;
    if (!t || !cur) return -1;
    if (task_tgid(t) != task_tgid(cur) && !cred_is_admin(&cur->cred) && cred_uid(&cur->cred) != 0)
        return -1;
    return (ssize_t)node->write(t, (const char*)buf, n);
}

/* file_ops signature after the M12 VFS refactor: explicit byte offset.
 * The generated buffer is keyed to the open file handle (cached in
 * `f->private`), so we read from `content[off]` regardless of what the
 * VFS layer's `f->pos` is doing. */
static ssize_t procfs_read(struct file* f, void* buf, size_t n, uint64_t off) {
    if (!f || !f->inode) return -1;
    struct procfs_node* node = (struct procfs_node*)f->inode->private;
    if (!node) return -1;

    /* Normally generated at open (procfs_open); a file opened some other
     * way generates on its first read. */
    if (!f->private && !procfs_generate(f, node)) return -1;

    struct file_state* st = (struct file_state*)f->private;
    if (!st->content || off >= st->size) return 0;          /* EOF */

    size_t remain = st->size - (size_t)off;
    size_t take   = n < remain ? n : remain;
    char* b = (char*)buf;
    for (size_t i = 0; i < take; i++) b[i] = st->content[(size_t)off + i];
    return (ssize_t)take;
}

static int procfs_close(struct file* f) {
    if (f && f->private) {
        struct file_state* st = (struct file_state*)f->private;
        if (st->ns_kind >= 0) ns_put(st->ns_obj);
        if (st->content) kfree(st->content);
        kfree(st);
        f->private = NULL;
    }
    return 0;
}

static const struct file_ops procfs_file_ops = {
    .read    = procfs_read,
    .write   = procfs_write,                    /* §M90 — writable nodes only */
    .readdir = NULL,
    .close   = procfs_close,
    .open    = procfs_open,                     /* §M90 — generate at open */
};

/* ---------------------------------------------------------------------- */
/* Pending queue + cached /proc dentry — same dance as devfs.             */
/* ---------------------------------------------------------------------- */

static struct procfs_node* pending_head = NULL;
static struct dentry*      proc_dir     = NULL;

/* A registered name may contain ONE slash, which makes it a file inside a
 * subdirectory of /proc — `net/tcp` becomes /proc/net/tcp.
 *
 * Added for §M24: the network's diagnostics are several files about one
 * subsystem, and Linux's own answer to that — the /proc/net directory — is a
 * path everybody already knows.  Flattening them into proc_net_tcp would
 * have worked and
 * would have taught a reader a private convention for no reason.  One level is
 * all that is offered — anything deeper wants a real tree, and there is
 * nothing yet that needs one. */
static struct dentry* proc_subdir(const char* name, size_t n) {
    for (struct dentry* d = proc_dir ? proc_dir->children : NULL; d; d = d->sibling) {
        size_t i = 0;
        while (i < n && d->name[i] && d->name[i] == name[i]) i++;
        if (i == n && d->name[i] == '\0') return d;
    }
    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!ino) return NULL;
    ino->type = INODE_DIR;
    vfs_inode_defaults(ino);   /* §M32 — ONE initialiser, every site */
    /* Borrow /proc's OWN directory operations.  A directory inode with no ops
     * can be walked (lookup follows dentry children, so `cat /proc/net/tcp`
     * works) but cannot be LISTED — `ls /proc/net` answered "readdir failed",
     * which is the shape of bug that gets called a filesystem mystery: the
     * file is there and the directory is empty. */
    if (proc_dir && proc_dir->inode) ino->ops = proc_dir->inode->ops;
    char buf[32];
    size_t i = 0;
    for (; i < n && i < sizeof buf - 1; i++) buf[i] = name[i];
    buf[i] = '\0';
    struct dentry* d = vfs_attach_child(proc_dir, buf, ino);
    if (!d) { kfree(ino); return NULL; }
    return d;
}

static int attach_node(struct procfs_node* node) {
    struct dentry* parent = proc_dir;
    const char* leaf = node->name;
    for (const char* p = node->name; *p; p++) {
        if (*p == '/') {
            parent = proc_subdir(node->name, (size_t)(p - node->name));
            leaf   = p + 1;
            break;
        }
    }
    if (!parent || !*leaf) return -3;

    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!ino) return -1;
    ino->type    = INODE_FILE;
    vfs_inode_defaults(ino);   /* §M32 — ONE initialiser, every site */
    ino->ops     = &procfs_file_ops;
    ino->private = node;
    if (!vfs_attach_child(parent, leaf, ino)) {
        kfree(ino);
        return -2;
    }
    return 0;
}

/* §M90 — /proc/self/fd: one entry per open descriptor of the task that READS
 * the directory, generated at readdir time (the descriptor table is the
 * caller's, so nothing can be cached in the shared dentry tree — there is no
 * lookup, only the listing).  Go's os/exec and docker's CLI enumerate it to
 * find what to close; without it `docker load` stopped at the first step. */
static int fd_dir_readdir(struct file* f, struct dirent* out) {
    if (!f || !out) return -1;
    int fd = fd_nth_open((int)f->pos);
    if (fd < 0) return 0;
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + fd % 10); fd /= 10; } while (fd);
    int i = 0;
    while (n) out->name[i++] = tmp[--n];
    out->name[i] = 0;
    out->type = INODE_SYMLINK;
    out->size = 0;
    f->pos++;
    return 1;
}
static const struct file_ops procfs_fddir_ops = { .read = NULL, .write = NULL,
                                                  .readdir = fd_dir_readdir, .close = NULL };
static void attach_fd_dir(void) {
    struct dentry* self = proc_subdir("self", 4);
    if (!self) return;
    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!ino) return;
    ino->type = INODE_DIR;
    vfs_inode_defaults(ino);
    ino->ops = &procfs_fddir_ops;
    if (!vfs_attach_child(self, "fd", ino)) kfree(ino);
}

int procfs_register(struct procfs_node* node) {
    if (!node || !node->name) return -1;
    if (proc_dir) return attach_node(node);
    node->_next = pending_head;
    pending_head = node;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Built-in node generators.                                              */
/* ---------------------------------------------------------------------- */

static void gen_version(struct procfs_writer* w) {
    pw_puts(w, "d-os 0.0.1 (i386)\n");
}

static void gen_uptime(struct procfs_writer* w) {
    pw_put_uptime(w, timer_ticks_ms());
    pw_putc(w, '\n');
}

static void gen_meminfo(struct procfs_writer* w) {
    /* PMM */
    uint32_t mgr = pmm_managed_frames();
    uint32_t fr  = pmm_free_frames();
    uint32_t us  = pmm_used_frames();
    /* §M90 — Linux's own lines FIRST: programs written for Linux (dockerd, a
     * JVM sizing its heap) look for "MemTotal:" and friends by name; the
     * system's own detail follows and is skipped by them. */
    pw_puts(w, "MemTotal:       "); pw_put_uint(w, mgr * 4u); pw_puts(w, " kB\n");
    pw_puts(w, "MemFree:        "); pw_put_uint(w, fr * 4u);  pw_puts(w, " kB\n");
    pw_puts(w, "MemAvailable:   "); pw_put_uint(w, fr * 4u);  pw_puts(w, " kB\n");
    pw_puts(w, "SwapTotal:      0 kB\nSwapFree:       0 kB\n");
    pw_puts(w, "pmm.frames.managed: "); pw_put_uint(w, mgr); pw_putc(w, '\n');
    pw_puts(w, "pmm.frames.free:    "); pw_put_uint(w, fr);  pw_putc(w, '\n');
    pw_puts(w, "pmm.frames.used:    "); pw_put_uint(w, us);  pw_putc(w, '\n');
    pw_puts(w, "pmm.mib.total:      "); pw_put_uint(w, (mgr * 4) / 1024); pw_putc(w, '\n');
    pw_puts(w, "pmm.mib.free:       "); pw_put_uint(w, (fr  * 4) / 1024); pw_putc(w, '\n');
    {   /* §M72 — the reserve */
        uint32_t rkb, refused; int low;
        pmm_reserve_stats(&rkb, &refused, &low);
        pw_puts(w, "pmm.reserve.kib:    "); pw_put_uint(w, rkb);     pw_putc(w, '\n');
        pw_puts(w, "pmm.reserve.low:    "); pw_put_uint(w, (unsigned)low); pw_putc(w, '\n');
        pw_puts(w, "pmm.reserve.refused:"); pw_put_uint(w, refused); pw_putc(w, '\n');
    }
    {   /* §M74 rung 1 — how much of the resident user memory is IN USE */
        struct memage_stats ms;
        memage_stats(&ms);
        pw_puts(w, "age.sweeps:         "); pw_put_uint(w, ms.sweeps); pw_putc(w, '\n');
        pw_puts(w, "age.kib.hot:        "); pw_put_uint(w, (unsigned)(ms.hot_bytes >> 10));  pw_putc(w, '\n');
        pw_puts(w, "age.kib.warm:       "); pw_put_uint(w, (unsigned)(ms.warm_bytes >> 10)); pw_putc(w, '\n');
        pw_puts(w, "age.kib.cold:       "); pw_put_uint(w, (unsigned)(ms.cold_bytes >> 10)); pw_putc(w, '\n');
    }
    /* §M19.5.3 — per NUMA node: size, free, and how many allocations were
     * served from the asking CPU's own node vs had to fall back elsewhere. */
    pw_puts(w, "pmm.numa.nodes:     "); pw_put_uint(w, (unsigned)pmm_node_count()); pw_putc(w, '\n');
    for (int nd = 0; nd < pmm_node_count(); nd++) {
        struct pmm_node_info ni;
        pmm_node_stats(nd, &ni);
        pw_puts(w, "pmm.node"); pw_put_uint(w, (unsigned)nd);
        pw_puts(w, ".mib.total: "); pw_put_uint(w, (ni.managed * 4) / 1024);
        pw_puts(w, " free: ");      pw_put_uint(w, (ni.free * 4) / 1024);
        pw_puts(w, " local: ");     pw_put_uint(w, ni.hit);
        pw_puts(w, " fallback: ");  pw_put_uint(w, ni.miss); pw_putc(w, '\n');
    }

    /* Heap */
    struct kmstat ks;
    kmalloc_stats(&ks);
    pw_puts(w, "heap.bytes.total:   "); pw_put_uint(w, (unsigned)ks.total_bytes); pw_putc(w, '\n');
    pw_puts(w, "heap.bytes.used:    "); pw_put_uint(w, (unsigned)ks.used_bytes);  pw_putc(w, '\n');
    pw_puts(w, "heap.bytes.free:    "); pw_put_uint(w, (unsigned)ks.free_bytes);  pw_putc(w, '\n');
    pw_puts(w, "heap.chunks:        "); pw_put_uint(w, ks.chunk_count);           pw_putc(w, '\n');
    pw_puts(w, "heap.chunks.free:   "); pw_put_uint(w, ks.free_chunk_count);      pw_putc(w, '\n');
}

static void gen_modules(struct procfs_writer* w) {
    /* Iterate the linker section directly — module.h already exposes the
     * boundary symbols. */
    int n = (int)(__stop_modules - __start_modules);
    pw_puts(w, "# class                   name\n");
    for (int i = 0; i < n; i++) {
        struct module_def* m = &__start_modules[i];
        pw_puts(w, m->class ? m->class : "?");
        for (int p = 0; p < 24; p++) pw_putc(w, ' '); /* crude column padding */
        pw_puts(w, m->name);
        pw_putc(w, '\n');
    }
    pw_puts(w, "total: ");
    pw_put_uint(w, (unsigned)n);
    pw_putc(w, '\n');
}

/* §M33 — the placement view.
 *
 * IT WALKS THE SLOT TABLE, NOT THE LINKER SECTION.  This used to index
 * `__start_drivers` directly, which §M66 turned into only part of the truth and
 * §M67 made actively wrong: a driver loaded from a module is not in that array
 * at all, so `/proc/drivers` silently omitted exactly the drivers most likely
 * to be under investigation.
 *
 * Columns: what it is, where it runs, where it COULD run, and what isolation
 * that placement actually delivers — the last being the one not inferable from
 * the others, because a DMA driver in ring 3 with no IOMMU is placed but not
 * isolated. */
static void gen_drivers(struct procfs_writer* w) {
    int n = driver_count_all();
    pw_puts(w, "# class\tname\tstate\tdomain\tcan-be\tisolation\tflags\n");
    for (int i = 0; i < n; i++) {
        struct driver* d = driver_at(i);
        if (!d) continue;
        uint8_t st = driver_state(d);
        const char* state_str =
            (st & DRV_S_QUARANTINE) ? "QUARANTINED" :
            (st & DRV_S_ADMIN_DOWN) ? "stopped" :
            (st & DRV_S_INITED)     ? "OK" :
            (st & DRV_S_INIT_FAIL)  ? "init-fail" :
            (st & DRV_S_PROBE_FAIL) ? "absent" :
            (st & DRV_S_PROBED)     ? "probed" : "registered";
        char decl[40];
        domain_set_str(d->domains ? d->domains : DOMAIN_KERNEL, decl, sizeof decl);
        uint32_t at = driver_domain(d);
        int dma = (d->flags & DRVF_DMA) ? 1 : 0;

        pw_puts(w, d->class ? d->class : "?");  pw_putc(w, '\t');
        pw_puts(w, d->name);                    pw_putc(w, '\t');
        pw_puts(w, state_str);                  pw_putc(w, '\t');
        pw_puts(w, domain_name(at));            pw_putc(w, '\t');
        pw_puts(w, decl);                       pw_putc(w, '\t');
        pw_puts(w, domain_isolation_name(
                       domain_isolation_of(at, dma, drvuser_confined(d->name))));
        pw_putc(w, '\t');
        if (d->flags & DRVF_BOOT_CRITICAL) pw_puts(w, "boot-critical ");
        if (dma)                           pw_puts(w, "dma ");
        /* §M33 Tier 2 — the pid and the restart count, for a driver that has
         * one.  The pid is what makes "placed in ring 3" checkable from outside
         * rather than taken on trust, and the restart count is the only thing
         * that tells a driver which has been dying and coming back from one
         * that has simply been up: both look identical in every other column,
         * which is precisely the state worth noticing. */
        int pid = drvuser_pid(d->name);
        if (pid > 0) {
            pw_puts(w, "pid="); pw_put_uint(w, (unsigned)pid); pw_putc(w, ' ');
            int rs = drvuser_restarts(d->name);
            if (rs) { pw_puts(w, "restarts="); pw_put_uint(w, (unsigned)rs);
                      pw_putc(w, ' '); }
            int ev = drvuser_events(d->name);
            if (ev >= 0) { pw_puts(w, "events="); pw_put_uint(w, (unsigned)ev);
                           pw_putc(w, ' '); }
        } else if (at == DOMAIN_USER) {
            /* Config asks for ring 3 and the driver is still in the kernel: a
             * restart has not happened yet.  Named rather than left blank,
             * because the `domain` column already reads "user" and the two
             * together would otherwise claim a placement that has not taken
             * effect. */
            pw_puts(w, "pending-restart ");
        }
        pw_putc(w, '\n');
    }
    pw_puts(w, "total: ");
    pw_put_uint(w, (unsigned)n);
    /* §M33 Tier 0's counter.  Here rather than only in the log, because a
     * system that quietly contains a fault every few seconds looks healthy from
     * outside — §M29's crash-loop reasoning, one layer over. */
    pw_puts(w, "\nfaults-contained: ");
    pw_put_uint(w, drvguard_fault_count());
    pw_putc(w, '\n');
}

/* The console / task / config iterators land below — declared in their
 * own headers and added in this milestone for procfs's sake. */

static void cb_sink(const struct console_sink* s, void* ctx) {
    struct procfs_writer* w = (struct procfs_writer*)ctx;
    pw_puts(w, s->category ? s->category : "?"); pw_putc(w, '\t');
    pw_puts(w, s->name);                          pw_putc(w, '\t');
    pw_puts(w, s->active ? "active" : "inactive");
    pw_putc(w, '\n');
}
static void gen_console(struct procfs_writer* w) {
    pw_puts(w, "# category  name      state\n");
    console_for_each(cb_sink, w);
}

static const char* state_name(enum task_state s) {
    switch (s) {
        case TASK_RUNNABLE: return "RUN";
        case TASK_SLEEPING: return "SLP";
        case TASK_STOPPED:  return "STOP";
        case TASK_DEAD:     return "DEAD";
    }
    return "?";
}
static void cb_task(const struct task* t, int is_current, void* ctx) {
    struct procfs_writer* w = (struct procfs_writer*)ctx;
    char ob[24];
    pw_put_uint(w, (unsigned)t->pid);  pw_putc(w, '\t');
    pw_put_uint(w, (unsigned)t->ppid); pw_putc(w, '\t');   /* M27 */
    pw_puts(w, state_name(t->state));  pw_putc(w, '\t');
    /* §M32 — the owner, through cred.h's one formatter.  `ps`, this file and
     * §M75's Task Manager all ask the same function: three renderings of an
     * identity would be three chances for one of them to say `root` where the
     * others say `system`. */
    pw_puts(w, cred_owner_name(&t->cred, ob, sizeof ob)); pw_putc(w, '\t');
    pw_puts(w, t->name);
    if (is_current) pw_puts(w, " (running)");
    pw_putc(w, '\n');
}
static void gen_tasks(struct procfs_writer* w) {
    pw_puts(w, "# pid  ppid  state  user  name\n");
    task_for_each(cb_task, w);
}

static void cb_config(const char* key, const char* value, void* ctx) {
    struct procfs_writer* w = (struct procfs_writer*)ctx;
    pw_puts(w, key); pw_puts(w, " = "); pw_puts(w, value); pw_putc(w, '\n');
}
static void gen_config(struct procfs_writer* w) {
    config_for_each(cb_config, w);
}

/* M28: the klog ring, rendered dmesg-style: `[    sec.mmm] LEVEL tag: msg`.
 * One source of truth — the `dmesg` shell command formats the same ring. */
static void cb_kmsg(const struct klog_record* r, void* ctx) {
    struct procfs_writer* w = (struct procfs_writer*)ctx;
    unsigned sec = (unsigned)(r->t_ms / 1000);
    unsigned ms  = (unsigned)(r->t_ms % 1000);
    pw_putc(w, '[');
    pw_put_uint(w, sec); pw_putc(w, '.');
    if (ms < 100) pw_putc(w, '0');
    if (ms <  10) pw_putc(w, '0');
    pw_put_uint(w, ms);
    pw_puts(w, "] ");
    pw_puts(w, klog_level_name(r->level)); pw_putc(w, ' ');
    pw_puts(w, r->tag); pw_puts(w, ": ");
    pw_puts(w, r->msg); pw_putc(w, '\n');
}
static void gen_kmsg(struct procfs_writer* w) {
    klog_for_each(cb_kmsg, w);
}

/* ---------------------------------------------------------------------- */
/* Built-in node table — declared static so they live forever.            */
/* ---------------------------------------------------------------------- */

static struct procfs_node nd_version = { .name = "version", .gen = gen_version };
static struct procfs_node nd_uptime  = { .name = "uptime",  .gen = gen_uptime  };
static struct procfs_node nd_meminfo = { .name = "meminfo", .gen = gen_meminfo };

/* §M90 — /proc/cpuinfo, in Linux's shape: one block per CPU.  Read by Go
 * (dockerd's CPU-variant probe), by a JVM and by build tools; absent, they
 * warn or guess. */
static void gen_cpuinfo(struct procfs_writer* w) {
    char model[64];
    hal_cpu_model(model, sizeof model);
    int n = smp_ncpus();
    for (int i = 0; i < n; i++) {
        pw_puts(w, "processor\t: "); pw_put_uint(w, (uint32_t)i); pw_putc(w, '\n');
#if defined(__aarch64__)
        pw_puts(w, "BogoMIPS\t: 125.00\nFeatures\t: fp asimd evtstrm cpuid\n"
                   "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x0\n"
                   "CPU part\t: 0xd08\nCPU revision\t: 3\n");
        pw_puts(w, "model name\t: "); pw_puts(w, model); pw_putc(w, '\n');
#else
        pw_puts(w, "vendor_id\t: GenuineIntel\nmodel name\t: "); pw_puts(w, model);
        pw_puts(w, "\nflags\t\t: fpu tsc cx8 cmov mmx fxsr sse sse2\n");
#endif
        pw_putc(w, '\n');
    }
}
static struct procfs_node nd_cpuinfo = { .name = "cpuinfo", .gen = gen_cpuinfo };

/* §M90 — the mount table in Linux's two shapes.  dockerd reads
 * /proc/self/mountinfo to find the parent mount of its data root (to set
 * propagation), runc and containerd read it to decide what is already
 * mounted; with no file they warn and fall back, or refuse.
 *
 * There are no mount NAMESPACES here, so "self" is the same table for every
 * process — the file lives at /proc/self/mountinfo as a plain node (procfs has
 * no per-pid directories), which is exactly what a reader opening "self" gets
 * on Linux when it has never unshared.
 *
 * /proc and /dev are not entries in the VFS mount table — procfs and devfs
 * hang their nodes off ramfs directories (see the header).  To a Linux reader
 * they ARE filesystems of type proc / devtmpfs, and a tool that checks "is
 * /proc mounted" before trusting it would otherwise refuse, so they are listed
 * with those types.  Device numbers are synthetic (0:<id>): there is no dev_t
 * here, and nothing that reads this file compares them with stat(). */
static void mi_line(struct procfs_writer* w, int id, int parent, const char* mp,
                    const char* type, const char* src, int info) {
    if (info) {
        pw_put_uint(w, (uint32_t)id); pw_putc(w, ' ');
        pw_put_uint(w, (uint32_t)parent); pw_puts(w, " 0:");
        pw_put_uint(w, (uint32_t)id); pw_puts(w, " / ");
        pw_puts(w, mp); pw_puts(w, " rw,relatime shared:"); pw_put_uint(w, (uint32_t)id);
        pw_puts(w, " - "); pw_puts(w, type); pw_putc(w, ' ');
        pw_puts(w, src); pw_puts(w, " rw\n");
    } else {
        pw_puts(w, src); pw_putc(w, ' '); pw_puts(w, mp); pw_putc(w, ' ');
        pw_puts(w, type); pw_puts(w, " rw,relatime 0 0\n");
    }
}
static void gen_mounts_common(struct procfs_writer* w, int info) {
    int n = vfs_mount_count();
    int root_id = 1;
    for (int i = 0; i < n; i++) {
        const struct vfs_mount* m = vfs_mount_at(i);
        if (!m) continue;
        int id = i + 1;
        int is_root = (m->path[0] == '/' && m->path[1] == 0);
        if (is_root) root_id = id;
        mi_line(w, id, is_root ? 0 : root_id, m->path,
                m->fs_name ? m->fs_name : "none",
                m->dev_name[0] ? m->dev_name : (m->fs_name ? m->fs_name : "none"), info);
    }
    mi_line(w, n + 1, root_id, "/proc", "proc", "proc", info);
    mi_line(w, n + 2, root_id, "/dev", "devtmpfs", "devtmpfs", info);
}
static void gen_mountinfo(struct procfs_writer* w) { gen_mounts_common(w, 1); }
static void gen_mounts(struct procfs_writer* w)    { gen_mounts_common(w, 0); }
/* §M90 — /proc/self/cgroup in the unified-hierarchy form, "0::<path>": the
 * line runc, containerd and dockerd read to find the cgroup they run in. */
static void gen_selfcgroup(struct procfs_writer* w) {
    char path[256];
    char view[256];
    struct task* t = procfs_target();
    cgroup_path_of(t, path, sizeof path);
    ns_cgroup_view(t, path, view, sizeof view);          /* §M90 — its cgroup namespace */
    pw_puts(w, "0::"); pw_puts(w, view); pw_putc(w, '\n');
}
static struct procfs_node nd_selfcgroup = { .name = "self/cgroup",    .gen = gen_selfcgroup };

/* §M90 — /proc/self/status, the lines programs actually parse: the ids, the
 * thread count, the memory figure, and the CAPABILITY masks — dockerd and runc
 * read CapEff/CapBnd to know what they may grant a container.  This kernel has
 * no capability sets: an administrator holds every privilege and anyone else
 * none, which is exactly what all-ones / all-zeros say. */
struct st_count { int tgid; int threads; };
static void st_count_fn(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct st_count* c = (struct st_count*)ctx;
    if (t->state != TASK_DEAD && task_tgid(t) == c->tgid) c->threads++;
}
static void gen_selfstatus(struct procfs_writer* w) {
    struct task* t = procfs_target();
    if (!t) return;
    int uid = cred_uid(&t->cred);
    if (uid < 0) uid = 0;
    int admin = cred_is_admin(&t->cred) || uid == 0;
    struct st_count c = { task_tgid(t), 0 };
    task_for_each(st_count_fn, &c);
    uint64_t priv = 0;
    task_mem_bytes(t, &priv, NULL, NULL);
    pw_puts(w, "Name:\t"); pw_puts(w, t->name);
    /* §M90 — the numbers as the READER's pid namespace names them. */
    struct task* rd = task_current();
    struct task* lead = (t->tgid && t->tgid != t->pid) ? task_find(t->tgid) : t;
    struct task* par = task_find(t->ppid);
    pw_puts(w, "\nUmask:\t0022\nState:\tR (running)\nTgid:\t"); pw_put_uint(w, (unsigned)ns_vnr(rd, lead ? lead : t));
    pw_puts(w, "\nPid:\t"); pw_put_uint(w, (unsigned)ns_vnr(rd, t));
    pw_puts(w, "\nPPid:\t"); pw_put_uint(w, (unsigned)(par ? ns_vnr(rd, par) : 0));
    pw_puts(w, "\nUid:\t"); for (int i = 0; i < 4; i++) { pw_put_uint(w, (unsigned)uid); pw_putc(w, i < 3 ? '\t' : '\n'); }
    pw_puts(w, "Gid:\t"); for (int i = 0; i < 4; i++) { pw_put_uint(w, (unsigned)uid); pw_putc(w, i < 3 ? '\t' : '\n'); }
    pw_puts(w, "Groups:\t\nVmRSS:\t"); pw_put_uint(w, (unsigned)(priv / 1024)); pw_puts(w, " kB\n");
    pw_puts(w, "Threads:\t"); pw_put_uint(w, (unsigned)c.threads);
    const char* caps = admin ? "000001ffffffffff" : "0000000000000000";
    pw_puts(w, "\nCapInh:\t0000000000000000\nCapPrm:\t"); pw_puts(w, caps);
    pw_puts(w, "\nCapEff:\t"); pw_puts(w, caps);
    pw_puts(w, "\nCapBnd:\t"); pw_puts(w, caps);
    pw_puts(w, "\nCapAmb:\t0000000000000000\nNoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\n");
}
static struct procfs_node nd_selfstatus = { .name = "self/status", .gen = gen_selfstatus };

/* §M90 — /proc/<pid>/oom_score_adj (-1000..1000, read and written by
 * containerd, its shim and runc) and oom_score (what an OOM killer would rank
 * by: there is none — §M72 refuses allocations instead — so 0).  See
 * task.h's oom_score_adj. */
static void gen_oomadj(struct procfs_writer* w) {
    struct task* t = procfs_target();
    int v = t ? t->oom_score_adj : 0;
    if (v < 0) { pw_putc(w, '-'); v = -v; }
    pw_put_uint(w, (unsigned)v); pw_putc(w, '\n');
}
static long write_oomadj(struct task* t, const char* buf, size_t n) {
    long v = 0; int neg = 0; size_t i = 0, d = 0;
    size_t full = n;
    /* Linux parses the write as a C STRING (kstrtoint of the stripped
     * buffer): a NUL ends it.  runc's nsexec writes the value with its
     * terminating NUL included, and was refused EINVAL. */
    for (size_t q = 0; q < n; q++) if (buf[q] == 0) { n = q; break; }
    while (i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i < n && (buf[i] == '-' || buf[i] == '+')) { neg = buf[i] == '-'; i++; }
    while (i < n && buf[i] >= '0' && buf[i] <= '9' && d < 6) { v = v * 10 + (buf[i] - '0'); i++; d++; }
    while (i < n && (buf[i] == '\n' || buf[i] == ' ')) i++;
    if (!d || i != n) return -22;                                     /* EINVAL */
    if (neg) v = -v;
    if (v < -1000 || v > 1000) return -22;
    t->oom_score_adj = (int)v;
    return (long)full;                                  /* the whole write was consumed */
}
static void gen_oomscore(struct procfs_writer* w) { pw_puts(w, "0\n"); }
static struct procfs_node nd_oomadj   = { .name = "self/oom_score_adj", .gen = gen_oomadj,
                                          .write = write_oomadj };
static struct procfs_node nd_oomscore = { .name = "self/oom_score", .gen = gen_oomscore };

/* §M90 — /proc/<pid>/timens_offsets: the clock offsets of the time namespace
 * its children are born in.  Only zero offsets exist (nsproxy.c says why), so
 * this reads zeros and refuses anything else; runc checks the file to decide
 * whether time namespaces exist at all. */
static void gen_timens(struct procfs_writer* w) {
    pw_puts(w, "monotonic           0         0\nboottime            0         0\n");
}
static long write_timens(struct task* t, const char* buf, size_t n) {
    (void)t;
    /* Lines "<clock> <secs> <nsecs>"; accepted only when every number is 0. */
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c >= '1' && c <= '9') {
            /* a non-zero digit: allowed only inside the CLOCK field ("1"/"7" are
             * the numeric clock ids), i.e. as the first token of its line */
            size_t s = i;
            while (s > 0 && buf[s - 1] != '\n') s--;
            int first = 1;
            for (size_t q = s; q < i; q++) if (buf[q] == ' ' || buf[q] == '\t') { first = 0; break; }
            if (!first) return -22;          /* EINVAL: a non-zero offset */
        }
    }
    return (long)n;
}
static struct procfs_node nd_timens = { .name = "self/timens_offsets", .gen = gen_timens,
                                        .write = write_timens };
static struct procfs_node nd_mountinfo  = { .name = "self/mountinfo", .gen = gen_mountinfo };
static struct procfs_node nd_selfmounts = { .name = "self/mounts",    .gen = gen_mounts };
/* §M90 — /proc/self/ns/<kind>: namespace HANDLES.  A program opens one to
 * hold or join a namespace (dockerd bind-mounts its net handle to keep the
 * default sandbox's namespace; runc setns()es into configured ones).  This
 * machine has ONE namespace of every kind except mount (vfs_mntns_new), so a
 * handle is a name; what it identifies is answered by readlink ("net:[N]",
 * the Linux numbers of the initial namespaces) and by setns.  Reading one
 * gives that same line. */
static const char* const g_ns_kinds[] = { "net", "mnt", "pid", "uts", "ipc", "cgroup", "user", "time" };
/* §M90 — every handle answers for the namespace of the process the PATH
 * named (/proc/self, /proc/<pid>, a task directory), from nsproxy.c; the
 * *_for_children pair names where that process's next child will be born. */
#define NS_GEN(k, K) static void gen_ns_##k(struct procfs_writer* w) { \
        pw_puts(w, #k ":["); pw_put_uint(w, ns_ino_of(procfs_target(), K)); pw_puts(w, "]\n"); }
NS_GEN(net, NSK_NET) NS_GEN(mnt, NSK_MNT) NS_GEN(pid, NSK_PID) NS_GEN(uts, NSK_UTS)
NS_GEN(ipc, NSK_IPC) NS_GEN(cgroup, NSK_CGROUP) NS_GEN(user, NSK_USER) NS_GEN(time, NSK_TIME)
static void gen_ns_tfc(struct procfs_writer* w) {
    pw_puts(w, "time:["); pw_put_uint(w, ns_child_ino_of(procfs_target(), NSK_TIME)); pw_puts(w, "]\n");
}
static void gen_ns_pfc(struct procfs_writer* w) {
    pw_puts(w, "pid:["); pw_put_uint(w, ns_child_ino_of(procfs_target(), NSK_PID)); pw_puts(w, "]\n");
}
static struct procfs_node nd_ns[] = {
    { .name = "net", .gen = gen_ns_net }, { .name = "mnt", .gen = gen_ns_mnt },
    { .name = "pid", .gen = gen_ns_pid }, { .name = "uts", .gen = gen_ns_uts },
    { .name = "ipc", .gen = gen_ns_ipc }, { .name = "cgroup", .gen = gen_ns_cgroup },
    { .name = "user", .gen = gen_ns_user }, { .name = "time", .gen = gen_ns_time },
    { .name = "time_for_children", .gen = gen_ns_tfc },
    { .name = "pid_for_children",  .gen = gen_ns_pfc },
};
/* The kind a handle node stands for: the enum value, plus 0x100 for a
 * *_for_children node; -1 for anything that is not a handle. */
static int ns_kind_of_node(const struct procfs_node* n) {
    for (unsigned i = 0; i < sizeof nd_ns / sizeof nd_ns[0]; i++)
        if (n == &nd_ns[i]) return i < NSK_COUNT ? (int)i : (i == 8 ? (0x100 | NSK_TIME) : (0x100 | NSK_PID));
    return -1;
}
/* readlink of a handle: "<kind>:[<ino>]" for the process the path named. */
unsigned long procfs_ns_ino(const char* kind) {
    static const char* const tail[] = { "time_for_children", "pid_for_children" };
    for (unsigned i = 0; i < 2; i++) {
        const char* a = tail[i]; const char* b = kind;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return ns_child_ino_of(procfs_target(), i == 0 ? NSK_TIME : NSK_PID);
    }
    for (unsigned i = 0; i < sizeof g_ns_kinds / sizeof g_ns_kinds[0]; i++) {
        const char* a = g_ns_kinds[i]; const char* b = kind;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return ns_ino_of(procfs_target(), (int)i);
    }
    return 0;
}
/* §M90 — st_ino of a handle (stat by path): the namespace's number, which is
 * what a program compares.  1 and *ino set, or 0 for any other inode. */
int procfs_ns_stat_ino(const struct inode* in, uint64_t* ino) {
    /* §M90 — the procfs ROOT is inode 1 on Linux (PROC_ROOT_INO), and runc
     * (filepath-securejoin) refuses a /proc whose root says otherwise —
     * "unsafe procfs detected".  The number is what makes it /proc. */
    if (in && proc_dir && in == proc_dir->inode) { *ino = 1; return 1; }
    if (!in || in->ops != &procfs_file_ops || !in->private) return 0;
    int k = ns_kind_of_node((const struct procfs_node*)in->private);
    if (k < 0) return 0;
    *ino = (k & 0x100) ? ns_child_ino_of(procfs_target(), k & 0xFF) : ns_ino_of(procfs_target(), k);
    return 1;
}
/* §M90 — an OPEN handle: what it captured at open (fstat, setns).
 * 1 = it is one; 0 = not a handle. */
int procfs_ns_handle(struct file* f, int* kind, struct nsobj** obj, int* mnt, uint32_t* ino) {
    if (!f || !f->inode || f->inode->ops != &procfs_file_ops || !f->private) return 0;
    struct file_state* st = (struct file_state*)f->private;
    if (st->ns_kind < 0) return 0;
    if (kind) *kind = st->ns_kind;
    if (obj)  *obj  = st->ns_obj;
    if (mnt)  *mnt  = st->ns_mnt;
    if (ino)  *ino  = st->ns_ino;
    return 1;
}
static void attach_ns_dir(void) {
    struct dentry* self = proc_subdir("self", 4);
    if (!self) return;
    struct inode* dino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!dino) return;
    dino->type = INODE_DIR;
    vfs_inode_defaults(dino);
    if (proc_dir && proc_dir->inode) dino->ops = proc_dir->inode->ops;
    struct dentry* ns = vfs_attach_child(self, "ns", dino);
    if (!ns) { kfree(dino); return; }
    for (unsigned i = 0; i < sizeof nd_ns / sizeof nd_ns[0]; i++) {
        struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
        if (!ino) return;
        ino->type = INODE_FILE;
        vfs_inode_defaults(ino);
        ino->ops = &procfs_file_ops;
        ino->private = &nd_ns[i];
        if (!vfs_attach_child(ns, nd_ns[i].name, ino)) kfree(ino);
    }
}

static struct procfs_node nd_mounts     = { .name = "mounts",         .gen = gen_mounts };
static struct procfs_node nd_modules = { .name = "modules", .gen = gen_modules };
static struct procfs_node nd_drivers = { .name = "drivers", .gen = gen_drivers };
static struct procfs_node nd_console = { .name = "console", .gen = gen_console };
static struct procfs_node nd_tasks   = { .name = "tasks",   .gen = gen_tasks   };
static struct procfs_node nd_config  = { .name = "config",  .gen = gen_config  };
static struct procfs_node nd_kmsg    = { .name = "kmsg",    .gen = gen_kmsg    };

/* ---------------------------------------------------------------------- */
/* Init.                                                                  */
/* ---------------------------------------------------------------------- */

void procfs_init(void) {
    struct file* f = vfs_open("/proc", VFS_RDONLY);
    if (!f) {
        kprintf("procfs: /proc not found — ramfs mount missing it?\n");
        return;
    }
    proc_dir = f->dentry;
    vfs_close(f);

    attach_node(&nd_version);
    attach_node(&nd_uptime);
    attach_node(&nd_meminfo);
    attach_node(&nd_cpuinfo);
    attach_node(&nd_mountinfo);
    attach_node(&nd_selfcgroup);
    attach_node(&nd_selfstatus);
    attach_node(&nd_oomadj);
    attach_node(&nd_oomscore);
    attach_node(&nd_timens);
    attach_fd_dir();
    attach_ns_dir();
    attach_node(&nd_selfmounts);
    attach_node(&nd_mounts);
    attach_node(&nd_modules);
    attach_node(&nd_drivers);
    attach_node(&nd_console);
    attach_node(&nd_tasks);
    attach_node(&nd_config);
    attach_node(&nd_kmsg);

    int flushed = 0;
    while (pending_head) {
        struct procfs_node* n = pending_head;
        pending_head = n->_next;
        n->_next = NULL;
        if (attach_node(n) == 0) flushed++;
    }
    kprintf("procfs: ready, /proc populated (%d external + 9 built-ins)\n",
            flushed);
}
