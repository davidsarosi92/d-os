/* =============================================================================
 * cred.c — task identity: the owner tag, the predicates, and the audit (§M32).
 *
 * See cred.h for the model.  This file holds three things and nothing else:
 * the ONE inheritance route, the predicates every privilege gate asks through,
 * and the §M71 invariant that makes the whole arrangement checkable on a
 * running machine rather than by reading it.
 * ============================================================================= */

#include "cred.h"
#include "task.h"
#include "audit.h"
#include "printf.h"
#include "shellcmd.h"
#include "users.h"   /* §M32 stage 3 — a uid's display name */
#include "console.h"
#include <stddef.h>

/* Local copy, exactly as task.c keeps its own: four lines that two files need
 * is not a reason for a shared string library, and this tree has none. */
static void cr_copy_n(char* dst, const char* src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Unsigned decimal into a caller buffer, returning the buffer.  Used only for
 * the "uid<N>" fallback below. */
static void cr_utoa(int v, char* dst, int cap) {
    char tmp[12];
    int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof tmp) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int o = 0;
    if (neg && o < cap - 1) dst[o++] = '-';
    while (n > 0 && o < cap - 1) dst[o++] = tmp[--n];
    dst[o] = 0;
}

/* ---------------------------------------------------------------------------
 * Construction and inheritance.
 * ------------------------------------------------------------------------- */

void cred_init_kernel(struct cred* c) {
    if (!c) return;
    c->owner   = TASK_OWNER_KERNEL;
    c->uid     = CRED_UID_NONE;
    c->gid     = CRED_UID_NONE;
    c->ngroups = 0;
    c->session = CRED_SESSION_NONE;
    for (int i = 0; i < CRED_MAX_GROUPS; i++) c->groups[i] = CRED_UID_NONE;
    c->root = NULL;
    c->container = 0;
    c->cwd[0] = 0;
}

/* §M73 — the filesystem root the CURRENT task resolves "/" against; NULL
 * means the machine's.  The VFS asks this once per path. */
struct dentry* cred_fs_root(void) {
    struct task* t = task_current();
    return t ? t->cred.root : NULL;
}
const char* cred_fs_cwd(void) {
    struct task* t = task_current();
    if (t && t->at_dir) return t->at_dir;    /* §M90 — an *at call's directory */
    return t ? t->cred.cwd : "";
}

void cred_inherit(struct cred* child, const struct cred* parent) {
    if (!child) return;

    /* No parent at all — the boot path before pid 0 exists.  The kernel
     * identity is the only honest answer: there is nobody to inherit from, and
     * inventing a uid here is how a system task would come to look like a
     * person's. */
    if (!parent) { cred_init_kernel(child); return; }

    *child = *parent;

    /* A KERNEL parent does not pass its tag on.  pid 0 spawns init, and init is
     * not the scheduler's scaffolding — the kernel's own tasks are not an
     * inheritable identity.  Without this rule every service on the machine
     * would be tagged KERNEL and the tag would say nothing. */
    if (parent->owner == TASK_OWNER_KERNEL) {
        child->owner   = TASK_OWNER_SYSTEM;
        child->uid     = CRED_UID_NONE;
        child->gid     = CRED_UID_NONE;
        child->ngroups = 0;
        child->session = CRED_SESSION_NONE;
    }
}

/* Count a task's children.  Used only by cred_become_user, which must refuse a
 * transition that would change the identity of a task that has already handed
 * its old one to somebody. */
struct child_count_ctx { int pid; int n; };
static void count_children(const struct task* t, int is_current, void* vctx) {
    (void)is_current;
    struct child_count_ctx* c = (struct child_count_ctx*)vctx;
    if (t->ppid == c->pid && t->pid != c->pid) c->n++;
}

int cred_become_user(int pid, int uid, int gid,
                     const int* groups, int ngroups, int session) {
    struct task* t = task_find(pid);
    if (!t) return -1;

    /* ONCE.  A second transition is not a login, it is an identity change on a
     * running task — which is the shape the whole model exists to forbid.  The
     * counter is also what the audit reads, so refusing here and counting there
     * are two views of one rule. */
    if (t->cred_seq != 0) {
        kprintf("cred: pid %d has already adopted an identity — refused\n", pid);
        return -1;
    }

    /* AND BEFORE ANY CHILD EXISTS.  A task that has spawned children has
     * already given them the identity it is about to leave; changing it now
     * would leave a subtree whose owner matches nothing that produced it, and
     * the children would be the evidence that something laundered. */
    struct child_count_ctx cc = { pid, 0 };
    task_for_each(count_children, &cc);
    if (cc.n != 0) {
        kprintf("cred: pid %d already has %d child(ren) — identity is fixed\n",
                pid, cc.n);
        return -1;
    }

    if (session == CRED_SESSION_NONE) {
        kprintf("cred: refusing a user identity with no session\n");
        return -1;
    }

    struct cred nc;
    cred_init_kernel(&nc);
    nc.owner   = TASK_OWNER_USER;
    nc.uid     = uid;
    nc.gid     = gid;
    nc.session = session;
    nc.ngroups = 0;
    for (int i = 0; i < ngroups && groups; i++) {
        if (nc.ngroups >= CRED_MAX_GROUPS) {
            /* Loudly.  A dropped group is a permission that quietly is not
             * there, and the symptom appears later as a refused operation with
             * no explanation anywhere near the cause. */
            kprintf("cred: uid %d has more than %d groups — %d dropped\n",
                    uid, CRED_MAX_GROUPS, ngroups - CRED_MAX_GROUPS);
            break;
        }
        nc.groups[nc.ngroups++] = groups[i];
    }

    /* §M73 — becoming a user does not leave a container: the root and the
     * container id belong to where the task runs, not to who it is. */
    nc.root = t->cred.root;
    nc.container = t->cred.container;
    for (unsigned i = 0; i < sizeof nc.cwd; i++) nc.cwd[i] = t->cred.cwd[i];
    t->cred = nc;
    t->cred_seq++;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Predicates.
 * ------------------------------------------------------------------------- */

int cred_uid(const struct cred* c) {
    if (!c || c->owner != TASK_OWNER_USER) return CRED_UID_NONE;
    return c->uid;
}

int cred_gid(const struct cred* c) {
    if (!c || c->owner != TASK_OWNER_USER) return CRED_UID_NONE;
    return c->gid;
}

int cred_in_group(const struct cred* c, int gid) {
    if (!c || c->owner != TASK_OWNER_USER) return 0;
    if (c->gid == gid) return 1;
    for (int i = 0; i < c->ngroups && i < CRED_MAX_GROUPS; i++)
        if (c->groups[i] == gid) return 1;
    return 0;
}

int cred_is_root(const struct cred* c) {
    return cred_uid(c) == CRED_UID_ROOT;
}

int cred_is_admin(const struct cred* c) {
    if (!c) return 0;
    /* The kernel's own tasks and the system's services are not gated against
     * the machine they ARE.  Saying otherwise would mean cron had to
     * authenticate to run a job, which is a boundary with nothing on the other
     * side of it — §M33's "isolation theatre" in a smaller costume. */
    if (c->owner != TASK_OWNER_USER) return 1;
    if (c->uid == CRED_UID_ROOT) return 1;
    return cred_in_group(c, CRED_GID_ADMIN);
}

int cred_may_act_on_uid(const struct cred* c, int uid) {
    if (!c) return 0;
    if (c->owner != TASK_OWNER_USER) return 1;    /* the system may */
    if (cred_is_admin(c)) return 1;
    return c->uid == uid;
}

/* ---------------------------------------------------------------------------
 * Display.
 * ------------------------------------------------------------------------- */

const char* cred_owner_kind_name(int owner) {
    switch (owner) {
        case TASK_OWNER_KERNEL: return "kernel";
        case TASK_OWNER_SYSTEM: return "system";
        case TASK_OWNER_USER:   return "user";
        default:                return "?";
    }
}

const char* cred_owner_name(const struct cred* c, char* buf, int cap) {
    if (!c) return "?";
    if (c->owner == TASK_OWNER_KERNEL) return "kernel";
    if (c->owner == TASK_OWNER_SYSTEM) return "system";
    if (c->uid == CRED_UID_ROOT)       return "root";

    /* The account database has the name.  For a uid with NO account the NUMBER
     * is the answer — a uid nothing claims is a fact worth seeing on the
     * screen, and rendering it as an empty column or as "unknown" hides
     * exactly the case somebody needs to investigate.  It is also a state this
     * system deliberately produces: a deleted account's uid is never reused, so
     * its files keep showing a number that belongs to nobody. */
    {
        const char* n = user_name_of(c->uid);
        if (n) return n;
    }
    if (cap < 5) return "uid?";
    cr_copy_n(buf, "uid", cap);
    cr_utoa(c->uid, buf + 3, cap - 3);
    return buf;
}

int cred_session_alloc(void) {
    static int next = 0;
    return __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
}

const struct cred* cred_current(void) {
    static const struct cred kernel_identity = {
        TASK_OWNER_KERNEL, CRED_UID_NONE, CRED_UID_NONE, 0,
        { CRED_UID_NONE, CRED_UID_NONE, CRED_UID_NONE, CRED_UID_NONE,
          CRED_UID_NONE, CRED_UID_NONE, CRED_UID_NONE, CRED_UID_NONE },
        CRED_SESSION_NONE,
        NULL, 0, "", "", 0               /* §M73 — the machine's root, no container, cwd "/"; §M89 no exe; §M90 umask 022 */
    };
    struct task* t = task_current();
    /* Before task_init, or on a CPU that has not taken a task yet.  "There is
     * no current task" and "the current task is unprivileged" are different
     * answers and only one of them is true here. */
    if (!t) return &kernel_identity;
    return &t->cred;
}

/* ---------------------------------------------------------------------------
 * §M71 — the invariant.
 *
 * Two checks that catch different things, which is why both are here:
 *
 *   1. OWNERSHIP DID NOT CHANGE AFTER CREATION.  With cred_seq == 0 the live
 *      owner/uid must equal what they were at spawn.  This is what catches a
 *      direct field write — the re-parenting laundering bug, whose code looks
 *      like tidying up and is therefore invisible to reading.
 *   2. AT MOST ONE SANCTIONED TRANSITION.  cred_seq > 1 means a task changed
 *      identity twice; the login route refuses that, so a task holding it got
 *      there some other way.
 *
 * Plus the structural rule that gives the tag its meaning: KERNEL is exactly
 * pid 0 and the idle tasks.  If anything else is tagged KERNEL, inheritance
 * has leaked the scaffolding's identity into the machine.
 *
 * HOW TO MAKE IT FAIL: `credtest` below, which writes a bad owner into a real
 * task in the real table — §M71 rule 1.  It exercises DETECTION, not
 * reporting.
 * ------------------------------------------------------------------------- */

struct audit_ctx { int verbose; int violations; int rows; };

static void audit_one(const struct task* t, int is_current, void* vctx) {
    (void)is_current;
    struct audit_ctx* a = (struct audit_ctx*)vctx;
    char nb[24];
    a->rows++;

    if (t->cred.owner < TASK_OWNER_KERNEL || t->cred.owner > TASK_OWNER_USER) {
        kprintf("  !! pid %d ('%s') has owner tag %d, which is not a kind\n",
                t->pid, t->name, t->cred.owner);
        a->violations++;
        return;
    }

    if (t->cred_seq == 0 &&
        (t->cred.owner != t->owner_birth || t->cred.uid != t->uid_birth)) {
        kprintf("  !! pid %d ('%s') owner changed with no sanctioned "
                "transition: born %s uid %d, now %s uid %d\n",
                t->pid, t->name,
                cred_owner_kind_name(t->owner_birth), t->uid_birth,
                cred_owner_kind_name(t->cred.owner), t->cred.uid);
        a->violations++;
    }

    if (t->cred_seq > 1) {
        kprintf("  !! pid %d ('%s') changed identity %u times; login allows one\n",
                t->pid, t->name, t->cred_seq);
        a->violations++;
    }

    /* KERNEL is exactly the scaffolding.  `is_idle` and pid 0 are the three
     * hand-built sites; anything else wearing that tag means a spawn inherited
     * it, which cred_inherit exists to prevent. */
    int is_scaffolding = (t->pid == 0) || t->is_idle;
    if (t->cred.owner == TASK_OWNER_KERNEL && !is_scaffolding) {
        kprintf("  !! pid %d ('%s') is tagged kernel but is not pid 0 or an "
                "idle task\n", t->pid, t->name);
        a->violations++;
    }
    if (is_scaffolding && t->cred.owner != TASK_OWNER_KERNEL) {
        kprintf("  !! pid %d ('%s') is scaffolding but is tagged %s\n",
                t->pid, t->name, cred_owner_kind_name(t->cred.owner));
        a->violations++;
    }

    /* A USER task without a session is not a user's task — the session is what
     * makes it one (cred.h).  This catches a partially-built identity, which
     * is what a login interrupted halfway would leave. */
    if (t->cred.owner == TASK_OWNER_USER && t->cred.session == CRED_SESSION_NONE) {
        kprintf("  !! pid %d ('%s') is owned by uid %d with no session\n",
                t->pid, t->name, t->cred.uid);
        a->violations++;
    }

    if (a->verbose)
        kprintf("  pid %d '%s' owner=%s seq=%u\n", t->pid, t->name,
                cred_owner_name(&t->cred, nb, sizeof nb), t->cred_seq);
}

static int au_identity(int verbose) {
    struct audit_ctx a = { verbose, 0, 0 };
    task_for_each(audit_one, &a);
    if (a.rows == 0) return AUDIT_SKIP;     /* rule 3: nothing to check */
    if (verbose) kprintf("  %d task(s) checked\n", a.rows);
    return a.violations;
}

AUDIT(identity) = {
    "task-identity",
    "every task's owner was set at creation and has not changed since",
    au_identity
};

/* ---------------------------------------------------------------------------
 * The falsifier.  Hidden from `help` like `hardlock`, `leaktest` and
 * `boundarytest`: reachable so the check can be proven to fail, not
 * advertised, because it corrupts a real row on purpose.
 * ------------------------------------------------------------------------- */

static void cmd_credtest(const char* args) {
    (void)args;
    struct task* me = task_current();
    if (!me) { console_write("credtest: no current task\n"); return; }

    kprintf("credtest: pid %d born %s uid %d, live %s uid %d, seq %u\n",
            me->pid, cred_owner_kind_name(me->owner_birth), me->uid_birth,
            cred_owner_kind_name(me->cred.owner), me->cred.uid, me->cred_seq);

    /* Write a bad identity DIRECTLY, which is precisely what the model forbids
     * and what no supported route can produce.  The audit must see it. */
    int saved_owner = me->cred.owner;
    int saved_uid   = me->cred.uid;
    me->cred.owner  = TASK_OWNER_SYSTEM;
    me->cred.uid    = CRED_UID_ROOT;
    console_write("credtest: laundered this task's identity directly — "
                  "`audit task-identity` must now FAIL\n");

    int v = audit_run_one("task-identity", 0);
    kprintf("credtest: audit reported %d violation(s) — %s\n",
            v, v > 0 ? "DETECTED" : "NOT DETECTED (the check is broken)");

    me->cred.owner = saved_owner;
    me->cred.uid   = saved_uid;
    v = audit_run_one("task-identity", 0);
    kprintf("credtest: restored; audit reports %d violation(s) — %s\n",
            v, v == 0 ? "clean" : "STILL DIRTY");
}

SHELL_CMD(credtest) = { "credtest", "",
                        NULL,              /* hidden, like hardlock */
                        SHELL_G_TEST, cmd_credtest, SHELL_P_ADMIN };
