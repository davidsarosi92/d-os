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
};

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
/* Spawn / fork: the child inherits (time: the parent's time_for_children). */
void ns_inherit(const struct task* parent, struct task* child);
/* Reap: drop every reference. */
void ns_release(struct task* t);

/* UTS: the names `t` sees, and setting them in its namespace (0 / -22). */
const char* ns_hostname(const struct task* t);
const char* ns_domainname(const struct task* t);
int  ns_set_hostname(struct task* t, const char* name, size_t len);
int  ns_set_domainname(struct task* t, const char* name, size_t len);

/* CGROUP: `full` (a cgroup path) as `t` sees it, written to `out`. */
void ns_cgroup_view(const struct task* t, const char* full, char* out, size_t cap);
#endif
