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
#include "task.h"
#include "console.h"
#include "config.h"
#include "timer.h"
#include "klog.h"
#include "pmm.h"
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
    if (min < 10) pw_putc(w, '0'); pw_put_uint(w, min); pw_putc(w, ':');
    if (sec < 10) pw_putc(w, '0'); pw_put_uint(w, sec); pw_putc(w, '.');
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
};

/* file_ops signature after the M12 VFS refactor: explicit byte offset.
 * The generated buffer is keyed to the open file handle (cached in
 * `f->private`), so we read from `content[off]` regardless of what the
 * VFS layer's `f->pos` is doing. */
static ssize_t procfs_read(struct file* f, void* buf, size_t n, uint64_t off) {
    if (!f || !f->inode) return -1;
    struct procfs_node* node = (struct procfs_node*)f->inode->private;
    if (!node) return -1;

    /* First read of this open instance — generate content into a fresh
     * writer and stash for subsequent slices. */
    if (!f->private) {
        struct procfs_writer w = { 0 };
        if (node->gen) node->gen(&w);

        struct file_state* st = (struct file_state*)kcalloc(1, sizeof(*st));
        if (!st) {
            if (w.buf) kfree(w.buf);
            return -1;
        }
        st->content = w.buf;
        st->size    = w.len;
        f->private  = st;
        f->inode->size = st->size;              /* update for stat */
    }

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
        if (st->content) kfree(st->content);
        kfree(st);
        f->private = NULL;
    }
    return 0;
}

static const struct file_ops procfs_file_ops = {
    .read    = procfs_read,
    .write   = NULL,                            /* read-only for now */
    .readdir = NULL,
    .close   = procfs_close,
};

/* ---------------------------------------------------------------------- */
/* Pending queue + cached /proc dentry — same dance as devfs.             */
/* ---------------------------------------------------------------------- */

static struct procfs_node* pending_head = NULL;
static struct dentry*      proc_dir     = NULL;

static int attach_node(struct procfs_node* node) {
    struct inode* ino = (struct inode*)kcalloc(1, sizeof(struct inode));
    if (!ino) return -1;
    ino->type    = INODE_FILE;
    ino->ops     = &procfs_file_ops;
    ino->private = node;
    if (!vfs_attach_child(proc_dir, node->name, ino)) {
        kfree(ino);
        return -2;
    }
    return 0;
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
    pw_puts(w, "pmm.frames.managed: "); pw_put_uint(w, mgr); pw_putc(w, '\n');
    pw_puts(w, "pmm.frames.free:    "); pw_put_uint(w, fr);  pw_putc(w, '\n');
    pw_puts(w, "pmm.frames.used:    "); pw_put_uint(w, us);  pw_putc(w, '\n');
    pw_puts(w, "pmm.mib.total:      "); pw_put_uint(w, (mgr * 4) / 1024); pw_putc(w, '\n');
    pw_puts(w, "pmm.mib.free:       "); pw_put_uint(w, (fr  * 4) / 1024); pw_putc(w, '\n');

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

static void gen_drivers(struct procfs_writer* w) {
    int n = (int)(__stop_drivers - __start_drivers);
    pw_puts(w, "# class      name        state\n");
    for (int i = 0; i < n; i++) {
        struct driver* d = &__start_drivers[i];
        uint8_t st = driver_state(d);
        const char* state_str =
            (st & DRV_S_INITED)     ? "OK" :
            (st & DRV_S_INIT_FAIL)  ? "init-fail" :
            (st & DRV_S_PROBE_FAIL) ? "absent" :
            (st & DRV_S_PROBED)     ? "probed" : "registered";
        pw_puts(w, d->class); pw_putc(w, '\t');
        pw_puts(w, d->name);  pw_putc(w, '\t');
        pw_puts(w, state_str); pw_putc(w, '\n');
    }
    pw_puts(w, "total: ");
    pw_put_uint(w, (unsigned)n);
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
        case TASK_DEAD:     return "DEAD";
    }
    return "?";
}
static void cb_task(const struct task* t, int is_current, void* ctx) {
    struct procfs_writer* w = (struct procfs_writer*)ctx;
    pw_put_uint(w, (unsigned)t->pid);  pw_putc(w, '\t');
    pw_put_uint(w, (unsigned)t->ppid); pw_putc(w, '\t');   /* M27 */
    pw_puts(w, state_name(t->state));  pw_putc(w, '\t');
    pw_puts(w, t->name);
    if (is_current) pw_puts(w, " (running)");
    pw_putc(w, '\n');
}
static void gen_tasks(struct procfs_writer* w) {
    pw_puts(w, "# pid  ppid  state  name\n");
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
