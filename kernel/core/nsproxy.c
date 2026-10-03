/* =============================================================================
 * nsproxy.c — namespaces as objects, §M90.
 *
 * WHY.  runc builds a container out of namespaces: it unshares UTS, IPC,
 * cgroup, time, PID and mount, sets the container's hostname, and checks each
 * one's handle under /proc/self/ns.  Until now this kernel had exactly ONE
 * namespace of every kind except mount, so `unshare` of the others had to be
 * refused; this file makes them real objects a task can be moved into.
 *
 * WHAT EACH KIND SEPARATES HERE — said per kind, because a namespace that
 * separates nothing must say so rather than look like isolation:
 *   UTS     the hostname and domain name (uname, sethostname): REAL.
 *   CGROUP  the cgroup that reads as "/" in /proc/self/cgroup: REAL for that
 *           view (a cgroup2 mount inside it showing only the subtree is the
 *           mount layer's, not yet done).
 *   TIME    clock offsets for CLOCK_MONOTONIC / CLOCK_BOOTTIME.  Only ZERO
 *           offsets exist (/proc/self/timens_offsets refuses others), and with
 *           zero offsets a time namespace is indistinguishable from the host's
 *           — so it is real exactly as far as it goes.  Linux's rule is kept:
 *           unshare(CLONE_NEWTIME) does not move the caller, its CHILDREN are
 *           born in the new one (time_for_children).
 *   IPC     System V IPC and POSIX message queues — neither exists in this
 *           kernel, so a new IPC namespace has nothing to separate and is
 *           exactly as isolated as Linux's would be with no objects in it.
 *   NET, USER   one namespace each; unshare of them stays refused (net: the
 *           stack is single-instance; user: no id mapping).
 *   MNT     the VFS's own (task->mntns, vfs_mntns_new) — only its handle's
 *           number is produced here.
 *   PID     REAL: a task has a number in its namespace and in each ancestor;
 *           every pid crossing the system call boundary is translated to the
 *           caller's view (getpid, kill, wait, siginfo, /proc…), the first
 *           process of a namespace is its init (1), orphans inside go to it,
 *           and its death takes the whole namespace down.  unshare and setns
 *           move pid_for_children only, as on Linux.
 *
 * IDENTITY.  Each object has an inode number, which is what a program
 * compares (readlink of /proc/self/ns/X, or st_ino of the handle) to decide
 * whether two processes share a namespace.  The initial namespaces use Linux's
 * well-known numbers; new ones are numbered upward from 4026532000, as Linux
 * numbers its own, and never reused.
 *
 * LIFETIME.  Reference counted: a task holds one per kind it is in (NULL =
 * the initial namespace, which is never freed and needs none), an open
 * handle (/proc/self/ns/X) holds one — so setns(fd) can join a namespace whose
 * last member has gone, as on Linux.
 * ============================================================================= */

#include "nsproxy.h"
#include "task.h"
#include "kmalloc.h"
#include "lock.h"
#include <stdint.h>
#include <stddef.h>

static const uint32_t g_initial_ino[NSK_COUNT] = {
    4026531840u /* net */, 4026531841u /* mnt */, 4026531836u /* pid */, 4026531838u /* uts */,
    4026531839u /* ipc */, 4026531835u /* cgroup */, 4026531837u /* user */, 4026531834u /* time */,
};
static uint32_t   g_next_ino = 4026532000u;
static spinlock_t g_ns_lock = SPINLOCK_INIT;      /* refs + the counter */

static void s_cpy(char* d, const char* s, size_t cap) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    if (cap) d[i] = 0;
}

static struct nsobj* obj_of(const struct task* t, int kind) {
    return (t && kind >= 0 && kind < NSK_COUNT) ? t->ns[kind] : NULL;
}

uint32_t ns_ino_of(const struct task* t, int kind) {
    if (kind < 0 || kind >= NSK_COUNT) return 0;
    if (kind == NSK_MNT && t && t->mntns)               /* the VFS's mount namespaces */
        return 4026533000u + (uint32_t)t->mntns;
    struct nsobj* o = obj_of(t, kind);
    return o ? o->ino : g_initial_ino[kind];
}
uint32_t ns_child_ino_of(const struct task* t, int kind) {
    if (kind == NSK_TIME && t && t->ns_time_children) return t->ns_time_children->ino;
    if (kind == NSK_PID && t && t->ns_pid_children) return t->ns_pid_children->ino;
    return ns_ino_of(t, kind);
}

static struct nsobj* ns_ref(struct nsobj* o) {
    if (!o) return NULL;
    uint32_t f = spin_lock_irqsave(&g_ns_lock);
    o->refs++;
    spin_unlock_irqrestore(&g_ns_lock, f);
    return o;
}
struct nsobj* ns_get(const struct task* t, int kind) { return ns_ref(obj_of(t, kind)); }
struct nsobj* ns_get_child(const struct task* t, int kind) {
    if (kind == NSK_TIME && t && t->ns_time_children) return ns_ref(t->ns_time_children);
    if (kind == NSK_PID && t && t->ns_pid_children) return ns_ref(t->ns_pid_children);
    return ns_get(t, kind);
}
void ns_put(struct nsobj* o) {
    if (!o) return;
    uint32_t f = spin_lock_irqsave(&g_ns_lock);
    int last = --o->refs == 0;
    spin_unlock_irqrestore(&g_ns_lock, f);
    if (last) {
        struct nsobj* par = o->kind == NSK_PID ? o->parent : NULL;
        kfree(o);
        ns_put(par);
    }
}

/* A new namespace of `kind`, starting as a copy of what `t` sees now. */
static struct nsobj* ns_new(const struct task* t, int kind) {
    struct nsobj* o = (struct nsobj*)kcalloc(1, sizeof *o);
    if (!o) return NULL;
    o->kind = kind;
    o->refs = 1;
    uint32_t f = spin_lock_irqsave(&g_ns_lock);
    o->ino = g_next_ino++;
    spin_unlock_irqrestore(&g_ns_lock, f);
    if (kind == NSK_UTS) {
        s_cpy(o->hostname, ns_hostname(t), sizeof o->hostname);
        s_cpy(o->domainname, ns_domainname(t), sizeof o->domainname);
    }
    if (kind == NSK_CGROUP) {
        /* "/" inside is the cgroup the creator is in now (Linux's rule). */
        extern void cgroup_path_of(const struct task* t, char* out, size_t cap);
        cgroup_path_of(t, o->cg_root, sizeof o->cg_root);
    }
    return o;
}

struct nsobj* ns_pid_new(const struct task* t, int* err);
static void set_slot(struct nsobj** slot, struct nsobj* o) {
    struct nsobj* old = *slot;
    *slot = o;
    ns_put(old);
}

int ns_unshare(struct task* t, unsigned long flags) {
    if (!t) return -22;
    /* Build every new object first, so an allocation failure changes nothing. */
    struct nsobj *uts = NULL, *ipc = NULL, *cg = NULL, *tm = NULL, *pd = NULL;
    if (flags & NS_CLONE_NEWPID) {
        int e = 0;
        if (!(pd = ns_pid_new(t, &e))) return e;
    }
    if ((flags & NS_CLONE_NEWUTS)    && !(uts = ns_new(t, NSK_UTS)))    goto oom;
    if ((flags & NS_CLONE_NEWIPC)    && !(ipc = ns_new(t, NSK_IPC)))    goto oom;
    if ((flags & NS_CLONE_NEWCGROUP) && !(cg  = ns_new(t, NSK_CGROUP))) goto oom;
    if ((flags & NS_CLONE_NEWTIME)   && !(tm  = ns_new(t, NSK_TIME)))   goto oom;
    if (uts) set_slot(&t->ns[NSK_UTS], uts);
    if (ipc) set_slot(&t->ns[NSK_IPC], ipc);
    if (cg)  set_slot(&t->ns[NSK_CGROUP], cg);
    if (tm)  set_slot(&t->ns_time_children, tm);   /* children only — see the header */
    if (pd)  set_slot(&t->ns_pid_children, pd);    /* likewise */
    return 0;
oom:
    ns_put(uts); ns_put(ipc); ns_put(cg); ns_put(tm); ns_put(pd);
    return -12;
}

void ns_enter(struct task* t, int kind, struct nsobj* o) {
    if (!t || kind < 0 || kind >= NSK_COUNT) return;
    set_slot(&t->ns[kind], ns_ref(o));
    /* Joining a time namespace moves the children's too (Linux: setns of a
     * time namespace sets both). */
    if (kind == NSK_TIME) set_slot(&t->ns_time_children, ns_ref(o));
}
/* setns of a PID namespace moves only where children are born (Linux). */
void ns_enter_pid_children(struct task* t, struct nsobj* o) {
    if (t) set_slot(&t->ns_pid_children, ns_ref(o));
}

void ns_inherit(const struct task* parent, struct task* child) {
    for (int k = 0; k < NSK_COUNT; k++) child->ns[k] = ns_get(parent, k);
    if (parent && parent->ns_time_children) {
        ns_put(child->ns[NSK_TIME]);
        child->ns[NSK_TIME] = ns_ref(parent->ns_time_children);
    }
    child->ns_time_children = ns_ref(child->ns[NSK_TIME]);
    if (parent && parent->ns_pid_children) {
        ns_put(child->ns[NSK_PID]);
        child->ns[NSK_PID] = ns_ref(parent->ns_pid_children);
    }
    child->ns_pid_children = ns_ref(child->ns[NSK_PID]);
}

void ns_release(struct task* t) {
    if (!t) return;
    for (int k = 0; k < NSK_COUNT; k++) { ns_put(t->ns[k]); t->ns[k] = NULL; }
    ns_put(t->ns_time_children);
    t->ns_time_children = NULL;
    ns_put(t->ns_pid_children);
    t->ns_pid_children = NULL;
}

/* ---- UTS ------------------------------------------------------------------- */

static char g_hostname[NS_HOST_MAX + 1]   = "d-os";
static char g_domainname[NS_HOST_MAX + 1] = "(none)";

const char* ns_hostname(const struct task* t) {
    struct nsobj* o = obj_of(t, NSK_UTS);
    return o ? o->hostname : g_hostname;
}
const char* ns_domainname(const struct task* t) {
    struct nsobj* o = obj_of(t, NSK_UTS);
    return o ? o->domainname : g_domainname;
}
static int set_name(char* dst, const char* name, size_t len) {
    if (len > NS_HOST_MAX) return -22;
    for (size_t i = 0; i < len; i++) dst[i] = name[i];
    dst[len] = 0;
    return 0;
}
int ns_set_hostname(struct task* t, const char* name, size_t len) {
    struct nsobj* o = obj_of(t, NSK_UTS);
    return set_name(o ? o->hostname : g_hostname, name, len);
}
int ns_set_domainname(struct task* t, const char* name, size_t len) {
    struct nsobj* o = obj_of(t, NSK_UTS);
    return set_name(o ? o->domainname : g_domainname, name, len);
}

/* ---- CGROUP ---------------------------------------------------------------- */

void ns_cgroup_view(const struct task* t, const char* full, char* out, size_t cap) {
    struct nsobj* o = obj_of(t, NSK_CGROUP);
    const char* root = o ? o->cg_root : "/";
    size_t n = 0;
    while (root[n]) n++;
    /* Below the namespace's root: the rest of the path ("/" if it IS the root).
     * Elsewhere (a process outside the subtree, as Linux shows with "/.."):
     * the full path, which is at least not a lie about where it is. */
    int under = 1;
    for (size_t i = 0; i < n; i++) if (full[i] != root[i]) { under = 0; break; }
    if (n == 1) under = 1;                                   /* root "/" */
    if (under && n > 1 && full[n] != 0 && full[n] != '/') under = 0;
    if (!under || n == 1) { s_cpy(out, full, cap); return; }
    s_cpy(out, full[n] ? full + n : "/", cap);
}

/* ---- PID ------------------------------------------------------------------- */

/* A new pid namespace below the one `t`'s children are born in now (one
 * reference), or NULL with *err = -12 ENOMEM / -28 ENOSPC (too deep, as
 * Linux answers past its 32 levels — ours stops at PIDNS_MAX_LEVEL). */
struct nsobj* ns_pid_new(const struct task* t, int* err) {
    struct nsobj* par = t ? (t->ns_pid_children ? t->ns_pid_children : t->ns[NSK_PID]) : NULL;
    int lvl = par ? par->level + 1 : 1;
    if (lvl >= PIDNS_MAX_LEVEL) { if (err) *err = -28; return NULL; }
    struct nsobj* o = ns_new(t, NSK_PID);
    if (!o) { if (err) *err = -12; return NULL; }
    o->level = lvl;
    o->parent = ns_ref(par);
    o->next_nr = 1;
    return o;
}

int ns_pid_level(const struct task* t) {
    struct nsobj* o = obj_of(t, NSK_PID);
    return o ? o->level : 0;
}

void ns_pid_assign(struct task* child) {
    if (!child) return;
    for (int l = 0; l < PIDNS_MAX_LEVEL; l++) child->upid[l] = 0;
    child->upid[0] = child->pid;
    for (struct nsobj* o = obj_of(child, NSK_PID); o && o->level > 0; o = o->parent) {
        uint32_t f = spin_lock_irqsave(&g_ns_lock);
        int nr = o->next_nr++;
        if (nr == 1) o->init_pid = child->pid;       /* the first is the init */
        spin_unlock_irqrestore(&g_ns_lock, f);
        if (o->level < PIDNS_MAX_LEVEL) child->upid[o->level] = nr;
    }
}

int ns_pid_within(const struct task* t, const struct nsobj* ns) {
    if (!ns) return 1;                                /* everything is in the initial one */
    for (struct nsobj* o = obj_of(t, NSK_PID); o; o = o->parent)
        if (o == ns) return 1;
    return 0;
}

int ns_vnr(const struct task* viewer, const struct task* t) {
    if (!t) return 0;
    struct nsobj* v = obj_of(viewer, NSK_PID);
    if (!v) return t->pid;                            /* the initial namespace: global */
    if (!ns_pid_within(t, v)) return 0;               /* outside: invisible */
    return v->level < PIDNS_MAX_LEVEL ? t->upid[v->level] : 0;
}

struct pid_find { const struct task* viewer; int nr; int found; };
static void pid_find_cb(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct pid_find* f = (struct pid_find*)ctx;
    if (f->found < 0 && t->state != TASK_DEAD && ns_vnr(f->viewer, t) == f->nr) f->found = t->pid;
}
int ns_pid_resolve(const struct task* viewer, int nr) {
    if (nr <= 0) return -1;
    if (!obj_of(viewer, NSK_PID)) {
        struct task* t = task_find(nr);
        return t ? t->pid : -1;
    }
    struct pid_find f = { viewer, nr, -1 };
    task_for_each(pid_find_cb, &f);
    return f.found;
}

int ns_pid_is_init(const struct task* t) {
    struct nsobj* o = obj_of(t, NSK_PID);
    return o && o->init_pid == t->pid;
}

struct pid_zap { const struct nsobj* ns; int self; int pids[128]; int n; };
static void pid_zap_cb(const struct task* t, int is_current, void* ctx) {
    (void)is_current;
    struct pid_zap* z = (struct pid_zap*)ctx;
    if (t->pid != z->self && t->state != TASK_DEAD && obj_of(t, NSK_PID) &&
        ns_pid_within(t, z->ns) && z->n < 128) z->pids[z->n++] = t->pid;
}
void ns_pid_init_died(struct task* t) {
    struct nsobj* o = obj_of(t, NSK_PID);
    if (!o || o->init_pid != t->pid) return;
    o->init_pid = 0;                                 /* no new process may join */
    /* Linux: when a pid namespace's init dies, every other process in it (and
     * below) is killed — the namespace cannot outlive its reaper. */
    struct pid_zap z = { o, t->pid, { 0 }, 0 };
    task_for_each(pid_zap_cb, &z);
    for (int i = 0; i < z.n; i++) task_kill(z.pids[i]);
}
