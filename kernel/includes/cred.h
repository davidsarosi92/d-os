/* =============================================================================
 * cred.h — who a task belongs to (§M32 stage 1).
 *
 * THE QUESTION THIS ANSWERS IS NOT "WHICH uid".  It is "is there a person
 * behind this at all".
 *
 * This machine runs a great many tasks that belong to nobody: pid 0, the
 * per-CPU idle tasks, init, the §M29 services (cron, watchdog, netd, the
 * kworkers), the compositor and the desktop.  Folding them into "root"
 * produces a task list where forty rows say `root` and the two that matter are
 * lost among them — *a label that is true of everything distinguishes
 * nothing.*  So a task's owner is a TAGGED VALUE and not a number:
 *
 *   TASK_OWNER_KERNEL — the scheduler's own scaffolding: pid 0 and the idle
 *                       tasks.  No session, no credentials in any meaningful
 *                       sense.  Displays as `kernel`.
 *   TASK_OWNER_SYSTEM — a task that is part of the system rather than of a
 *                       session: init, a §M29 service, the compositor.
 *                       Displays as `system`, NEVER as `root` — the whole
 *                       point of the tag is that those are different answers.
 *   TASK_OWNER_USER   — a task inside a user's session, carrying that user's
 *                       uid AND the session id.
 *
 * THE SESSION IS WHAT MAKES A TASK A USER'S, NOT THE uid.  A service
 * configured to run under a user's uid is still a system task; a shell in that
 * user's session is theirs.  That is why TASK_OWNER_USER carries `session` and
 * not only the number — and it is also what lets root LOG IN (a decision taken
 * deliberately: only root may delete an admin account) without uid 0 becoming
 * ambiguous.  A root login is USER + uid 0 + a session; cron stays SYSTEM.
 * The Task Manager's owner column therefore stays honest by construction.
 *
 * -----------------------------------------------------------------------------
 * ZERO MUST MEAN `KERNEL`, AND THIS IS NOT A STYLE CHOICE.
 *
 * `task.c` already records the lesson in its own comment: **`struct task` is
 * constructed in FOUR places and only one of them is `spawn_common`** — the
 * other three are pid 0, the BSP idle task and each AP's idle task, all
 * synthesised by hand.  When §M49 added a scheduling weight in `spawn_common`
 * alone, the other three kept `kcalloc`'s zero and the first boot took a divide
 * error before the scheduler had run once.
 *
 * Those three sites are EXACTLY the tasks that must be kernel-owned.  So the
 * enum is ordered so that the zero every unattended construction site already
 * produces is the correct and safest answer.  A bare uid field would make zero
 * mean **root**, and every task those three sites create would silently become
 * root's: *a field whose default is the most privileged value is a laundering
 * mechanism waiting for a caller who forgets.*
 *
 * -----------------------------------------------------------------------------
 * INHERITANCE HAS ONE ROUTE, AND RE-PARENTING IS NOT IT.
 *
 * A child takes its parent's credentials inside `spawn_common`, under the same
 * lock, and no caller assigns them afterwards.  §M57's `cpu_home` is both the
 * precedent and the warning: it was documented as a fact, assigned by callers
 * at moments when they merely INTENDED a placement, and four sites ended up
 * mutating the wrong queue's ring.  Ownership has the same shape and a worse
 * failure, because a wrong answer here is a security statement.
 *
 * **RE-PARENTING MUST NOT LAUNDER OWNERSHIP.**  §M27's init re-parents orphans,
 * so killing a user's shell hands its children to init — and if ownership
 * followed the parent, those children would become SYSTEM tasks.  That is a
 * privilege escalation with no attacker in it: an ordinary kill produces it.
 * Ownership is therefore captured at spawn and is immutable for the life of the
 * task; re-parenting moves `ppid` and nothing else.  The only sanctioned
 * transition is `cred_become_user()` — a session leader dropping privilege at
 * login — which happens ONCE, before the session's first child exists, and is
 * counted so the audit can see that it happened at most once.
 *
 * **A KERNEL PARENT DOES NOT PASS ITS TAG ON.**  pid 0 spawns init, and init
 * is not scaffolding.  So inheritance from a KERNEL-owned parent yields SYSTEM:
 * the kernel's own tasks are not an inheritable identity.  Stated here rather
 * than left implicit, because the alternative reads as a bug in the enum.
 *
 * -----------------------------------------------------------------------------
 * ONE PREDICATE DECIDES ADMIN-NESS.
 *
 * `cred_is_admin()` is the single place that answers "may this task administer
 * the machine", so the later refinement the §M32 plan promises (a capability
 * set rather than a boolean) is one edit and every gate inherits it.  §M33's
 * `domain_enforceable` is the precedent — and §M78 is the warning that came
 * with it: that function was the appointed single place that knows what is
 * real, and BOTH its premises had expired underneath it while it went on
 * refusing.  *A gate is only as honest as its premises.*
 *
 * Admin-ness is GROUP MEMBERSHIP, not a second boolean on the account.  Two
 * ways to say the same thing eventually disagree, and then nothing says which
 * one wins.
 * ============================================================================= */

#ifndef CRED_H
#define CRED_H

#include <stdint.h>

/* The tag.  KERNEL is 0 on purpose — see the header comment. */
enum task_owner_kind {
    TASK_OWNER_KERNEL = 0,
    TASK_OWNER_SYSTEM = 1,
    TASK_OWNER_USER   = 2,
};

/* Supplementary groups carried per task.  Eight is not a guess about how many
 * groups a person needs; it is how many a TASK can carry without every struct
 * task paying for a pointer chase.  The account database may list more, and
 * `cred_build_for_user` truncates with a line rather than silently — a dropped
 * group is a permission that quietly is not there. */
#define CRED_MAX_GROUPS 8

/* Reserved ids.  uid 0 is the system's own identity and the one account that
 * may delete an admin; gid 0 is its group. */
#define CRED_UID_ROOT   0
#define CRED_GID_ROOT   0
/* The admin group.  Membership IS admin-ness (plus uid 0, which is a member by
 * definition and cannot be removed from it — see users.h, protected root). */
#define CRED_GID_ADMIN  1
/* "No user" — what a KERNEL/SYSTEM task's uid reads as.  Deliberately NOT 0:
 * a system task is not root, and a reader that prints the number must not be
 * able to render it as the most privileged account on the machine. */
#define CRED_UID_NONE   (-1)

/* No session.  A KERNEL or SYSTEM task has none, and so does a USER task only
 * while it is being constructed. */
#define CRED_SESSION_NONE 0

struct cred {
    int owner;                      /* enum task_owner_kind; 0 = KERNEL       */
    /* MEANINGFUL ONLY WHEN owner == TASK_OWNER_USER.  Read it through
     * cred_uid(), never directly — and the reason is the same zeroing that
     * makes the owner tag correct by construction:
     *
     * the three hand-built tasks (pid 0, BSP idle, each AP idle) come out of
     * kcalloc, so this field reads **0**, which is CRED_UID_ROOT.  The owner
     * tag saves us — they are KERNEL and every predicate below asks the tag
     * first — but a caller who reached past the tag and tested `c->uid == 0`
     * would conclude that the idle tasks are root.  cred_uid() makes that
     * unreachable by answering CRED_UID_NONE for anything that is not a USER,
     * whatever the field happens to hold.
     *
     * The alternative (initialising it at the three sites) would trade a
     * property the compiler guarantees for one three call sites must remember,
     * which is the trade §M49's divide-by-zero already lost once. */
    int uid;
    int gid;                        /* primary group; same rule as uid        */
    int ngroups;
    int groups[CRED_MAX_GROUPS];    /* supplementary groups                   */
    int session;                    /* §M32 session id; 0 = none              */
};

/* The uid this identity really has: CRED_UID_NONE for anything that is not a
 * USER, whatever the raw field holds.  Every reader uses this. */
int cred_uid(const struct cred* c);
int cred_gid(const struct cred* c);

/* ---------------------------------------------------------------------------
 * Construction and inheritance.
 * ------------------------------------------------------------------------- */

/* The identity a hand-built kernel task has: KERNEL, no user, no session.
 * Equal to what kcalloc already produces, and provided anyway so a future
 * field cannot silently default to something else at the three sites that do
 * not call spawn_common. */
void cred_init_kernel(struct cred* c);

/* The ONE inheritance route, called from spawn_common only.  A KERNEL-owned
 * parent yields SYSTEM (see the header); anything else is copied verbatim,
 * session included. */
void cred_inherit(struct cred* child, const struct cred* parent);

/* The one sanctioned transition: a session leader adopting a user's identity
 * at login.  Returns 0 on success, -1 if the task already has children or has
 * already transitioned — both of which would be laundering rather than login.
 *
 * `groups`/`ngroups` may be NULL/0; `gid` is the primary group. */
int cred_become_user(int pid, int uid, int gid,
                     const int* groups, int ngroups, int session);

/* ---------------------------------------------------------------------------
 * Predicates.  Every privilege gate in the tree asks through these.
 * ------------------------------------------------------------------------- */

/* May this identity administer the machine?  uid 0, or a member of the admin
 * group.  A KERNEL or SYSTEM task answers 1: the kernel's own code is not
 * something to gate against itself, and saying otherwise would mean every
 * service had to authenticate to do its job. */
int cred_is_admin(const struct cred* c);

/* Is this identity uid 0 — the protected root account?  Separate from
 * cred_is_admin because exactly one operation needs it (deleting or demoting
 * an admin account) and every other gate must NOT, or the distinction between
 * "an admin" and "the one account that can undo admins" is lost. */
int cred_is_root(const struct cred* c);

/* Is `uid` this identity's own, or is the identity privileged over it?  The
 * question every "may I touch that task / that file" gate asks. */
int cred_may_act_on_uid(const struct cred* c, int uid);

/* Group membership, primary or supplementary. */
int cred_in_group(const struct cred* c, int gid);

/* ---------------------------------------------------------------------------
 * Display.  One place, because the shell's `ps`, /proc and §M75's Task Manager
 * all ask and three lookups would be three chances for one to disagree.
 * ------------------------------------------------------------------------- */

/* "kernel" / "system" / a user name (or "uid<N>" when the account database has
 * no entry — a uid with no account is a fact worth seeing, not one to hide).
 *
 * Takes a CALLER-OWNED buffer and returns it (or a string literal, when the
 * answer is a constant).  Not a shared static: `ps` walks the task list on one
 * CPU while the Task Manager renders on another, and a shared formatting
 * buffer would give one of them the other's answer — an identity displayed
 * against the wrong row is the one rendering bug in this file that would be
 * read as a security failure. */
const char* cred_owner_name(const struct cred* c, char* buf, int cap);

/* The tag alone, for a caller that wants to group rather than name. */
const char* cred_owner_kind_name(int owner);

/* ---------------------------------------------------------------------------
 * The current task's identity.  Never NULL — a caller before task_init gets
 * the kernel identity, because "there is no current task" and "the current
 * task is unprivileged" are different answers and only one of them is true.
 * ------------------------------------------------------------------------- */
const struct cred* cred_current(void);

#endif /* CRED_H */
