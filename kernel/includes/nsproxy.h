/* nsproxy.h — namespaces as objects (§M90); see kernel/core/nsproxy.c. */
#ifndef DOS_NSPROXY_H
#define DOS_NSPROXY_H
#include <stdint.h>
#include <stddef.h>

struct task;

/* The kinds, in /proc/self/ns order.  NET and USER have exactly one namespace
 * here (there is no network or user-id separation yet); MNT is the VFS's
 * (task->mntns); the rest are objects below. */
enum ns_kind { NSK_NET, NSK_MNT, NSK_PID, NSK_UTS, NSK_IPC, NSK_CGROUP, NSK_USER, NSK_TIME,
               NSK_COUNT };

#define NS_HOST_MAX 64

struct nsobj {
    int       kind;
    uint32_t  ino;                       /* what readlink and stat report     */
    int       refs;
    /* NSK_UTS */
    char      hostname[NS_HOST_MAX + 1];
    char      domainname[NS_HOST_MAX + 1];
    /* NSK_CGROUP — the cgroup that is "/" inside, as a PATH (a node can be
     * removed while a namespace still names it; a path then merely stops
     * matching, a pointer would dangle). */
    char      cg_root[128];
    /* NSK_PID — a pid namespace: its depth (the initial one is 0 and has no
     * object), its parent, the next number to hand out inside it, and the
     * GLOBAL pid of its init (the task numbered 1; 0 once it has died). */
    int       level;
    struct nsobj* parent;
    int       next_nr;
    int       init_pid;
};
#define PIDNS_MAX_LEVEL 4                /* task->upid[] entries: levels 0..3 */

/* Linux's CLONE_NEW* bits. */
#define NS_CLONE_NEWTIME   0x00000080ul
#define NS_CLONE_NEWNS     0x00020000ul
#define NS_CLONE_NEWCGROUP 0x02000000ul
#define NS_CLONE_NEWUTS    0x04000000ul
#define NS_CLONE_NEWIPC    0x08000000ul
#define NS_CLONE_NEWUSER   0x10000000ul
#define NS_CLONE_NEWPID    0x20000000ul
#define NS_CLONE_NEWNET    0x40000000ul

/* The inode number of `t`'s namespace of `kind` (the initial ones are
 * Linux's well-known numbers). */
uint32_t ns_ino_of(const struct task* t, int kind);
/* time_for_children / pid_for_children: the namespace a child will be born in. */
uint32_t ns_child_ino_of(const struct task* t, int kind);
/* `t`'s namespace object of `kind`, with a reference (NULL = the initial one,
 * which needs none).  ns_put drops one. */
struct nsobj* ns_get(const struct task* t, int kind);
void          ns_put(struct nsobj* o);
/* The same for the namespace `t`'s next child is born in (time_for_children). */
struct nsobj* ns_get_child(const struct task* t, int kind);

/* unshare(2) for the object kinds in `flags` (NEWUTS/NEWIPC/NEWCGROUP/NEWTIME).
 * 0, or -12 when out of memory (nothing changed then). */
int  ns_unshare(struct task* t, unsigned long flags);
/* setns(2): make `o` (of `kind`, NULL = initial) `t`'s namespace. */
void ns_enter(struct task* t, int kind, struct nsobj* o);
void ns_enter_pid_children(struct task* t, struct nsobj* o);
/* Spawn / fork: the child inherits (time: the parent's time_for_children). */
void ns_inherit(const struct task* parent, struct task* child);
/* Reap: drop every reference. */
void ns_release(struct task* t);

/* UTS: the names `t` sees, and setting them in its namespace (0 / -22). */
const char* ns_hostname(const struct task* t);
const char* ns_domainname(const struct task* t);
int  ns_set_hostname(struct task* t, const char* name, size_t len);
int  ns_set_domainname(struct task* t, const char* name, size_t len);

/* PID namespaces.  Every task has a number in its own pid namespace and in
 * each ancestor (task->upid[level]; level 0 is the global pid).  A process
 * sees another by the number at ITS OWN level, and one outside its namespace
 * not at all (0).  In the initial namespace every answer is the global pid, so
 * nothing outside a container changes. */
int  ns_pid_level(const struct task* t);
/* A new pid namespace below where t's children are born (one reference). */
struct nsobj* ns_pid_new(const struct task* t, int* err);
/* At spawn, after ns_inherit: number `child` in every namespace it is in. */
void ns_pid_assign(struct task* child);
/* `t` as `viewer` sees it: its number, or 0 when it is outside viewer's ns. */
int  ns_vnr(const struct task* viewer, const struct task* t);
/* The GLOBAL pid of the task `viewer` calls `nr`, or -1.  Takes the task list
 * lock: never call with it held. */
int  ns_pid_resolve(const struct task* viewer, int nr);
/* §M90 — may `t` make a THREAD?  Not after unshare(CLONE_NEWPID) has moved
 * where its children are born: a thread lives in its process's namespace, and
 * Linux answers that clone with EINVAL rather than split a process across
 * two.  1 = yes. */
int  ns_pid_thread_ok(const struct task* t);
/* Is `t` the init (number 1) of its own pid namespace? */
int  ns_pid_is_init(const struct task* t);
/* `t` (an init) has died: kill every task in its namespace and below. */
void ns_pid_init_died(struct task* t);
/* Is `t` inside pid namespace `ns` or one below it? */
int  ns_pid_within(const struct task* t, const struct nsobj* ns);

/* CGROUP: `full` (a cgroup path) as `t` sees it, written to `out`. */
void ns_cgroup_view(const struct task* t, const char* full, char* out, size_t cap);
#endif
