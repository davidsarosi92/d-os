/* =============================================================================
 * login.c — authentication and sessions (§M32 stage 4).
 *
 * Where the account database (users.h) and a running task's identity (cred.h)
 * meet.  They meet exactly once, here, and the shape of this file is dictated
 * by one rule from cred.h:
 *
 * -----------------------------------------------------------------------------
 * A SESSION IS A NEW TASK, BECAUSE OWNERSHIP IS IMMUTABLE.
 *
 * The obvious implementation is to authenticate and then re-tag the shell that
 * asked.  It is wrong twice over:
 *
 *   - Ownership is captured at spawn and never changes (cred.h), precisely so
 *     that re-parenting cannot launder it.  A shell that could re-tag itself
 *     on login could re-tag itself on anything.
 *   - And `logout` would have nowhere to go back TO.  An identity that can be
 *     put on can be taken off, and then "this task belongs to alice" is a
 *     statement with an expiry date — which is exactly what every gate in the
 *     tree would be reading.
 *
 * So `login` SPAWNS a session leader and waits for it.  The leader adopts the
 * identity **at its own entry point, before it has run anything**, which is
 * the one moment cred_become_user permits: no children exist yet and nothing
 * has been done under the old identity.  Logging out is the leader exiting,
 * after which control returns to the (still unprivileged, still SYSTEM)
 * console that called login.  There is no transition to undo.
 *
 * -----------------------------------------------------------------------------
 * WHAT AUTHENTICATION SAYS WHEN IT FAILS.
 *
 * One message for "no such user" and for "wrong password" — user_check_password
 * deliberately cannot distinguish them in its return value either.  A login
 * prompt that answers differently is an account enumerator, and the honest
 * message costs nothing.
 *
 * There is a DELAY on failure, and it is not decoration: without one, a
 * failed login costs the attacker exactly one PBKDF2 (about half a second on
 * this machine, measured), and this console is scriptable.
 * ============================================================================= */

#include "users.h"
#include "cred.h"
#include "task.h"
#include "shellcmd.h"
#include "shell_provider.h"
#include "console.h"
#include "printf.h"
#include "kmalloc.h"
#include "vfs.h"
#include "config.h"
#include <stddef.h>


struct session_req {
    int  uid;
    int  gid;
    int  session;
    int  ngroups;
    int  groups[CRED_MAX_GROUPS];
    char name[USER_NAME_MAX + 1];
    char home[USER_PATH_MAX + 8];
    int* cfg_token;             /* where the session reports its seat token */
};

/* The session leader's entry point.
 *
 * Everything it does before `shell_provider_active()` happens under the
 * SYSTEM identity it inherited, which is why the FIRST thing is to stop being
 * that.  If the transition fails the task exits rather than continuing — a
 * session shell running with the console's identity is precisely the
 * privilege the login was supposed to drop. */
static void session_entry(void) {
    struct session_req* r = (struct session_req*)task_start_arg();
    if (!r) return;

    struct task* me = task_current();
    if (!me) { kfree(r); return; }

    users_ensure_home_of(r->uid);       /* while still SYSTEM, see gui.c */
    if (cred_become_user(me->pid, r->uid, r->gid, r->groups, r->ngroups,
                         r->session) != 0) {
        console_write("login: could not adopt the account's identity — "
                      "refusing to run the session\n");
        kfree(r);
        return;
    }

    kprintf("login: session %d opened for '%s' (uid %d) on pid %d\n",
            r->session, r->name, r->uid, me->pid);
    /* §M32 stage 9 — the account's preferences.  AFTER the identity is
     * adopted, because config_apply's watchers run as this task and a wallpaper
     * applied while still SYSTEM would be the console's, not the user's. */
    /* §M32 (2026-09-27) — a text login takes the console's seat only if
     * nobody holds it; beside a signed-in desktop it is a BACKGROUND session
     * whose preferences are its own (config.c, "layers").  The token goes back
     * to do_login, which is where every exit of this session is observed. */
    int tok = config_user_attach(r->uid, CFG_SEAT_IF_FREE);
    if (r->cfg_token) *r->cfg_token = tok;
    /* The home directory may not exist on a machine where the account was
     * created without a writable volume — say so rather than silently landing
     * somewhere else. */
    if (r->home[0]) {
        struct file* f = vfs_open(r->home, VFS_RDONLY);
        if (f) vfs_close(f);
        else   kprintf("login: home %s is not there\n", r->home);
    }
    kfree(r);

    const struct shell_provider* sp = shell_provider_active();
    if (sp && sp->entry) sp->entry();
    /* NOTE: this point is NOT reached on the ordinary exit.  `logout` ends the
     * session with task_exit_code(), which is `noreturn` — so the withdrawal
     * lives in do_login, after task_wait.  See the comment there. */
}

/* Authenticate and open a session.  `name` and `password` may be NULL, in
 * which case they are prompted for. */
static int do_login(const char* name_in, const char* pw_in) {
    char name[USER_NAME_MAX + 1];
    char pw[128];

    if (name_in && *name_in) {
        int i = 0;
        for (; name_in[i] && i < (int)sizeof name - 1; i++) name[i] = name_in[i];
        name[i] = 0;
    } else if (shell_read_line("login: ", name, sizeof name) <= 0) {
        console_write("login: no input source on this shell\n");
        return -1;
    }

    if (pw_in) {
        int i = 0;
        for (; pw_in[i] && i < (int)sizeof pw - 1; i++) pw[i] = pw_in[i];
        pw[i] = 0;
    } else if (shell_read_secret("password: ", pw, sizeof pw) < 0) {
        console_write("login: no input source on this shell\n");
        return -1;
    }

    if (user_check_password(name, pw) != 0) {
        /* Scrub the attempt out of our stack before doing anything that could
         * block.  Cheap, and the alternative is a password sitting in a kernel
         * stack for as long as the task lives. */
        for (int i = 0; i < (int)sizeof pw; i++) pw[i] = 0;
        task_msleep(1000);
        console_write("login: incorrect username or password\n");
        return -1;
    }
    for (int i = 0; i < (int)sizeof pw; i++) pw[i] = 0;

    const struct user_account* u = user_by_name(name);
    if (!u) return -1;
    /* Copied: the account may be deleted while its session runs (`userdel`
     * of a signed-in user is a case §M32 runs), and `u` points into the
     * account table. */
    int s_uid = u->uid;

    struct session_req* r = (struct session_req*)kcalloc(1, sizeof *r);
    if (!r) { console_write("login: out of memory\n"); return -1; }
    r->uid     = u->uid;
    r->gid     = u->gid;
    r->session = cred_session_alloc();
    r->ngroups = user_groups_of(u->uid, r->groups, CRED_MAX_GROUPS);
    for (int i = 0; u->name[i] && i < (int)sizeof r->name - 1; i++) r->name[i] = u->name[i];
    {   /* §M82 — the REAL home (on the volume when there is one). */
        const char* h = user_home(u);
        for (int i = 0; h[i] && i < (int)sizeof r->home - 1; i++) r->home[i] = h[i];
    }

    /* Bind the SAME console the caller is using, at spawn.  §M49 moved console
     * binding into the spawn because doing it afterwards is an SMP race: the
     * task may already be running on another core.  A session shell that came
     * up with no VC would print its prompt to nobody. */
    int cfg_token = 0;                 /* written by the session, read after it */
    r->cfg_token = &cfg_token;
    struct task* me = task_current();
    struct task* t = task_spawn_arg_console("session", session_entry, r,
                                            me ? me->pid : -1,
                                            me ? me->out_console : NULL);
    if (!t) { kfree(r); console_write("login: could not start a session\n"); return -1; }

    int code = 0;
    task_wait(t->pid, &code);

    /* §M32 stage 9 — WITHDRAW THE PREFERENCES HERE, and this placement is a
     * corrected bug rather than a preference.
     *
     * The first version put it at the end of the session task, after the
     * shell's entry returned, with a comment claiming it "covers every exit".
     * It covered none of the common one: `logout` calls task_exit_code(),
     * which is `noreturn`, so that line was unreachable — and the measurement
     * said so plainly, with the second user's wallpaper still on screen for
     * the third login.  *A cleanup placed after a call that never returns is
     * not a cleanup.*
     *
     * `task_wait` is the point every exit passes through, because the login
     * task is parked on it whatever kills the session. */
    config_user_detach(s_uid, cfg_token);
    console_write("login: session ended\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Commands.
 * ------------------------------------------------------------------------- */

static void cmd_login(const char* args) {
    /* A password may be given as an argument so the harness can drive this
     * headlessly — and it is a BAD way to log in, because the line is visible
     * in the shell's history and in the serial log.  Said out loud rather than
     * left as a convenience somebody adopts. */
    char name[USER_NAME_MAX + 1] = { 0 };
    char pw[128] = { 0 };
    int i = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < (int)sizeof name - 1) name[i++] = *args++;
    name[i] = 0;
    while (*args == ' ') args++;
    i = 0;
    while (*args && i < (int)sizeof pw - 1) pw[i++] = *args++;
    pw[i] = 0;

    if (pw[0])
        console_write("login: NOTE — a password given on the command line is "
                      "visible in the log; the prompt does not echo\n");
    do_login(name[0] ? name : NULL, pw[0] ? pw : NULL);
}

static void cmd_whoami(const char* args) {
    (void)args;
    const struct cred* c = cred_current();
    char buf[24];
    kprintf("%s\n", cred_owner_name(c, buf, sizeof buf));
}

static void cmd_id(const char* args) {
    (void)args;
    const struct cred* c = cred_current();
    char buf[24];
    kprintf("owner=%s name=%s uid=%d gid=%d session=%d admin=%s\n",
            cred_owner_kind_name(c->owner),
            cred_owner_name(c, buf, sizeof buf),
            cred_uid(c), cred_gid(c), c->session,
            cred_is_admin(c) ? "yes" : "no");
    if (c->owner != TASK_OWNER_USER) {
        /* The distinction that the whole owner tag exists for, restated where
         * somebody is most likely to be confused by `admin=yes`. */
        console_write("id: this is a SYSTEM context, not a logged-in user — it "
                      "is privileged because it IS the machine\n");
        return;
    }
    kprintf("groups:");
    int g[CRED_MAX_GROUPS];
    int n = user_groups_of(cred_uid(c), g, CRED_MAX_GROUPS);
    for (int i = 0; i < n; i++) {
        const char* nm = group_name_of(g[i]);
        if (nm) kprintf(" %s(%d)", nm, g[i]);
        else    kprintf(" %d", g[i]);
    }
    kprintf("\n");
}

static void cmd_logout(const char* args) {
    (void)args;
    const struct cred* c = cred_current();
    if (c->owner != TASK_OWNER_USER) {
        console_write("logout: not in a session\n");
        return;
    }
    /* Ending the session is ending this TASK — there is no identity to take
     * off (see the header).  The login that spawned it is waiting. */
    struct task* me = task_current();
    if (me) task_exit_code(0);
}

SHELL_CMD(login)  = { "login",  "[name] [password]",
                      "authenticate and open a session", SHELL_G_SYS, cmd_login, SHELL_P_ANY };
SHELL_CMD(logout) = { "logout", "", "end this session", SHELL_G_SYS, cmd_logout, SHELL_P_ANY };
SHELL_CMD(whoami) = { "whoami", "", "print the current identity", SHELL_G_SYS, cmd_whoami, SHELL_P_ANY };
SHELL_CMD(id)     = { "id",     "", "print identity, groups and privilege",
                      SHELL_G_SYS, cmd_id, SHELL_P_ANY };

/* =============================================================================
 * §M32 stage 8 — ELEVATION.
 *
 * An administrator is not automatically privileged: they are ELIGIBLE, and the
 * account says how eligibility becomes permission.  Two modes are declared in
 * users.h and one is implemented — per-operation, which is the default.
 *
 * WHY PER-OPERATION IS THE DEFAULT, in one sentence: it is what protects the
 * administrator from their own browser.  An "always elevated" session runs
 * everything it launches with the power to change the machine, and the thing
 * most likely to be launched is the thing most likely to be attacked.
 *
 * NO TIMED WINDOW.  sudo's few-minute grace is state that has to be expired
 * correctly — per session, across a logout, when the account is demoted — and
 * every one of those is a way to keep a privilege that should have ended.
 * There is nothing to get wrong in not having one, and the cost is a password
 * per operation, which is a human-paced event on a machine whose PBKDF2 was
 * measured at under half a second.
 *
 * A SYSTEM CONTEXT NEVER ELEVATES.  It is the machine; there is nobody to ask
 * and nothing to ask for.  That is also what keeps every service, the boot
 * console and the whole test harness working unchanged.
 * ============================================================================= */

int auth_elevate(const char* what) {
    const struct cred* c = cred_current();

    /* Not a logged-in user: the kernel, a service, the installer console. */
    if (c->owner != TASK_OWNER_USER) return 0;

    /* Not eligible at all.  It SAYS SO: an earlier version returned silently
     * here, and the dispatcher's generic "refused" replaced the specific
     * "this needs an administrator" that the gate used to print — a message
     * regression introduced while making the rule single-sourced.  *Moving a
     * decision into one place must not move its explanation out of the way.* */
    if (!cred_is_admin(c)) {
        kprintf("elevate: '%s' is not an administrator\n",
                cred_owner_name(c, (char[24]){0}, 24));
        return -1;
    }

    const struct user_account* u = user_by_uid(cred_uid(c));
    if (!u) return -1;

    if (u->elevation == USER_ELEV_ALWAYS) {
        /* DECLARED, STORED, NOT IMPLEMENTED — and it says so every time rather
         * than silently behaving like the other mode.  A setting that is
         * accepted and ignored is the failure §M33's honesty gate is named
         * after; a setting that announces the divergence is merely unfinished. */
        kprintf("elevate: '%s' is set to always-elevated, which is not "
                "implemented — re-authenticating instead\n", u->name);
    }

    char pw[128];
    kprintf("elevate: %s needs administrator rights\n", what ? what : "this");
    if (shell_read_secret("password: ", pw, sizeof pw) < 0) {
        /* No input source — a GUI window with no console behind it, or a
         * non-interactive caller.  REFUSE.  The alternative is to let an
         * operation through because nobody could be asked, which is the one
         * failure mode a confirmation must not have. */
        console_write("elevate: no way to ask for a password here — refused\n");
        return -1;
    }

    int ok = (user_check_password(u->name, pw) == 0);
    for (int i = 0; i < (int)sizeof pw; i++) pw[i] = 0;
    if (!ok) {
        task_msleep(1000);
        console_write("elevate: incorrect password\n");
        return -1;
    }
    return 0;
}
