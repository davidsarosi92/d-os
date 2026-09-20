/* =============================================================================
 * users.h — the account database (§M32 stage 3).
 *
 * Accounts, groups, passwords and the lifecycle that creates and destroys
 * them.  cred.h is what a RUNNING TASK carries; this is what the machine
 * REMEMBERS.  The two meet exactly once, at login (§M32 stage 4).
 *
 * -----------------------------------------------------------------------------
 * THREE ORTHOGONAL FACTS, AND NONE IS DERIVED FROM ANOTHER.
 *
 *   1. IS THERE A PERSON BEHIND THIS ACCOUNT — `type`.  Decides whether it may
 *      log in at all.  root is a PERSON: only root may delete an admin, and
 *      that decision requires root to be loginable.
 *   2. MAY IT ADMINISTER — membership in the admin group (CRED_GID_ADMIN).
 *      **Not a second boolean on the account.**  Two ways to say the same
 *      thing eventually disagree and then nothing says which one wins.
 *   3. HOW IT ADMINISTERS — `elevation`, per account.
 *
 * A "limited user" is therefore not a fourth type; it is a PERSON who is not
 * in the admin group.  The later refinement (several groups, a capability set)
 * grows along axis 2 rather than adding enum values.
 *
 * **uid RANGES ARE AN ALLOCATION POLICY, NOT A SOURCE OF PRIVILEGE.**  Persons
 * are allocated from USER_UID_FIRST upward because that is a tidy convention;
 * nothing anywhere asks whether a uid is "low".  A privilege derived from a
 * number range is a convention that does not fail when it is violated, which
 * is the worst kind.
 *
 * -----------------------------------------------------------------------------
 * A uid IS NEVER REUSED.  EVER.
 *
 * Files are owned by NUMBER, so if `userdel bob` freed uid 1001 and
 * `useradd carol` took it, carol would own every file bob left behind with no
 * operation having granted them.  That is a real and classic defect with
 * exactly two honest fixes: never reuse, or make deletion sweep every volume.
 *
 * Sweeping cannot be done here and saying so is the point: `/mnt` is exFAT,
 * which **cannot store ownership at all** (see `mount`, and §M32 stage 5), so
 * a sweep would be walking a filesystem that has no answer to the question.
 * The high-water mark only ever increases, is persisted with the database, and
 * a deleted account's uid is retired for the life of the machine.  Two billion
 * of them is not a resource this system will exhaust.
 *
 * -----------------------------------------------------------------------------
 * THE GATES ARE NOT PARAMETERS.
 *
 * Every mutating call below reads `cred_current()` itself rather than taking
 * an "acting credentials" argument.  A gate that can be HANDED its answer is a
 * gate a caller can get wrong — including by passing a cred it built — and the
 * one place in this tree where that matters most is the one function that can
 * create an administrator.  The cost is that testing "A may not delete B"
 * means actually being A, which is the honest test anyway.
 *
 * The rules, stated once:
 *
 *   - Creating or deleting a PERSON who is not an admin: any admin.
 *   - Creating, deleting, renaming or DEMOTING an admin: **root only, and root
 *     must be logged in as root.**  An admin cannot unmake another admin, so a
 *     compromised admin session cannot remove the accounts that would notice.
 *   - root itself: cannot be deleted, renamed, or removed from the admin
 *     group, by anybody, including root.  Protection is a property of the
 *     account and not of who is asking.
 *   - Changing your own password: yourself.  Changing somebody else's: an
 *     admin, except that an ADMIN's password needs root.
 *   - **At least one account that can actually log in as an admin must remain
 *     at all times.**  Enforced in every path that could remove the last one —
 *     deletion, demotion and password removal are three different ways to
 *     reach the same unadministrable machine, so the check lives in one place
 *     that all three call.
 *
 * -----------------------------------------------------------------------------
 * WHERE IT LIVES, AND THE BOOTSTRAP.
 *
 * `/` is ramfs, so a database written there is gone at power-off — §M63 stage
 * 0's bug, which was invisible because the write SUCCEEDS.  So the store
 * follows config.c exactly: flat files in the persistent volume's root
 * (`d-os-passwd`, `d-os-shadow`), attached after the mount, and creating them
 * IS the writability test.  With no writable volume everything works and
 * nothing survives, and every write says so.
 *
 * **A PASSWORDLESS ACCOUNT CANNOT LOG IN.**  Not "logs in without a password"
 * — cannot log in.  Which raises the bootstrap question, and the answer is
 * deliberate and stated rather than accidental: on a machine where no account
 * has a password yet, `users_needs_setup()` is true and the boot console runs
 * as SYSTEM, which cred_is_admin() answers 1 for.  **That console is the
 * installer.**  Once any account can log in, the machine has an administrator
 * and the console stops being a way around them.
 * ============================================================================= */

#ifndef USERS_H
#define USERS_H

#include <stdint.h>
#include "cred.h"

#define USER_NAME_MAX      31
#define USER_PATH_MAX      63
#define USER_MAX_ACCOUNTS  32
#define USER_MAX_GROUPS    16
#define GROUP_MAX_MEMBERS  16

/* Persons are allocated from here upward.  A convention, nothing more — see
 * the header on why no gate consults it. */
#define USER_UID_FIRST     1000

/* Salt size.  16 bytes from §M39's CSPRNG: enough that two users with the same
 * password do not share a hash, which is the entire job of a salt. */
#define USER_SALT_BYTES    16

/* PBKDF2 iterations, default.  MEASURED rather than copied: 10000 iterations
 * of PBKDF2-HMAC-SHA256 cost 445 ms on emulated i386, 563 on x86_64 and 432 on
 * aarch64 (`kdftest` prints it on any machine).  Emulation is the slow case and
 * a login is a human-paced event, so this is the cost being spent knowingly.
 * `security.kdf_iterations` moves it for a board where that is wrong. */
#define USER_KDF_ITERS_DEFAULT 10000

/* §M32 — THE DEFAULT ACCOUNT.
 *
 * A machine that boots with no usable account is a machine nobody can log
 * into, and the bootstrap console (below) is a workaround rather than an
 * answer.  So root ships WITH a password, and the tension that creates is
 * resolved rather than hidden:
 *
 *   - A DEFAULT CREDENTIAL THAT IS SILENT IS A BACKDOOR.  This one is
 *     announced on the console at EVERY boot until it is changed, and the
 *     account is marked so the system knows the difference.
 *   - `users_needs_setup()` stays true while root still carries it, so the
 *     installer console keeps working out of the box.  *The machine is not
 *     "configured" because an account exists; it is configured when somebody
 *     has chosen a secret.*
 *
 * `users.default_user` names the account the login surfaces offer first. */
#define USER_DEFAULT_NAME     "root"
#define USER_DEFAULT_PASSWORD "dos"

enum user_type {
    USER_TYPE_PERSON = 0,       /* someone may log in as this               */
    USER_TYPE_SYSTEM = 1,       /* an identity for files/services; no login */
};

enum user_elevation {
    /* THE DEFAULT, and the one that is implemented: a privileged operation
     * re-authenticates.  One operation, no timed window — a window is state
     * that has to be expired correctly and there is nothing to get wrong in
     * not having one. */
    USER_ELEV_PER_OP = 0,
    /* Declared, stored, and NOT implemented: an admin session that is
     * privileged throughout.  The field exists with both legal values because
     * the account database is where the choice belongs; the code that would
     * act on it is §M32's stated open item rather than a silent default. */
    USER_ELEV_ALWAYS = 1,
};

struct user_account {
    int      used;
    char     name[USER_NAME_MAX + 1];
    int      uid;
    int      gid;                       /* primary group */
    int      type;                      /* enum user_type */
    int      elevation;                 /* enum user_elevation */
    char     home[USER_PATH_MAX + 1];
    /* Password material.  `has_password == 0` means LOGIN REFUSED, which is
     * not the same as an empty password and must never become it. */
    int      has_password;
    /* Set while the password is still the shipped default.  A separate fact
     * from has_password: "cannot log in", "can log in with a secret somebody
     * chose" and "can log in with the password printed in the manual" are three
     * different states, and collapsing the last two is what turns a
     * convenience into a backdoor. */
    int      pw_is_default;
    uint32_t kdf_iters;
    char     salt_hex[USER_SALT_BYTES * 2 + 1];
    char     hash_hex[64 + 1];
};

struct group_entry {
    int  used;
    char name[USER_NAME_MAX + 1];
    int  gid;
    int  members[GROUP_MAX_MEMBERS];    /* uids; CRED_UID_NONE = empty slot */
};

/* ---------------------------------------------------------------------------
 * Bring-up.
 * ------------------------------------------------------------------------- */

/* Built-in defaults: root (uid 0, admin, no password), the `admins` and
 * `users` groups.  Runs with no disk and cannot fail — the machine must have
 * an identity model before anything can ask about one. */
void users_init(void);

/* Overlay the persistent store from `dir` (the mounted volume).  Called from
 * BOTH entry paths, right after the mount, exactly like config: miss one and
 * that architecture silently forgets every account. */
int  users_attach_persistent(const char* dir);

/* Write the database out.  Returns 0 when it reached a disk, -1 when it lives
 * in RAM only — the caller is expected to SAY which, never to assume. */
int  users_save(void);

/* Create every account's home directory (0700, owned by the account).
 * Called after the store is attached AND after ramfs is rebuilt at each boot:
 * the database survives a reboot and `/` does not. */
void users_ensure_homes(void);

/* Create uid's preference file on the persistent volume, owned by that account
 * at 0600.  The volume root is root-owned, so a user cannot make it
 * themselves — see the comment on the implementation. */
void users_ensure_pref_store(int uid);

/* True while no account can log in.  See the bootstrap note in the header. */
int  users_needs_setup(void);

/* Is any account still carrying the shipped default password? */
int  users_default_password_in_use(void);
/* The account the login surfaces offer first (`users.default_user`). */
const char* users_default_name(void);

/* §M81 — the account to open a session as WITHOUT asking, or NULL.  All three
 * conditions in the implementation must hold; see users.c for why the third
 * ("it is the only account") is what makes this safe rather than convenient. */
const char* users_autologin_account(void);

/* Can anybody actually sign in?  What makes a picker meaningful. */
int users_anyone_can_sign_in(void);
/* Print the standing warning.  Called from both boot paths, every boot. */
void users_warn_default_password(void);

/* ---------------------------------------------------------------------------
 * Lookup.  These are read-only and ungated: who exists is not a secret, and
 * every surface that displays an owner needs them.
 * ------------------------------------------------------------------------- */
const struct user_account* user_by_name(const char* name);
const struct user_account* user_by_uid(int uid);
/* The display name for a uid, or NULL when nothing claims it.  cred.c's
 * formatter calls this, which is why an unclaimed uid renders as its number
 * rather than as a blank. */
const char* user_name_of(int uid);
int  user_count(void);
const struct user_account* user_at(int i);

const struct group_entry* group_by_name(const char* name);
const struct group_entry* group_by_gid(int gid);
const char* group_name_of(int gid);
int  group_count(void);
const struct group_entry* group_at(int i);
/* Fill `out` with the gids `uid` belongs to (primary first).  Returns how many
 * were written, capped at `max`. */
int  user_groups_of(int uid, int* out, int max);

/* Is this uid in the admin group?  The account-database half of
 * cred_is_admin(); a LOGGED-IN task answers from its own cred instead, because
 * a session must not change privilege because a file changed underneath it. */
int  user_is_admin_uid(int uid);

/* ---------------------------------------------------------------------------
 * Authentication.
 * ------------------------------------------------------------------------- */

/* 0 when the password is right and the account may log in; negative otherwise.
 * Deliberately does NOT distinguish "no such user" from "wrong password" in
 * its return value — the caller has one message for both, or the login prompt
 * becomes a way to enumerate accounts. */
int  user_check_password(const char* name, const char* password);

/* §M81 — AN 8-BIT FINGERPRINT OF A TYPED SECRET, for comparing the two ENDS of
 * the sign-in chain without ever showing what was typed.
 *
 * The problem it solves: the accounts panel reports how many characters it SET
 * and the lock screen how many it RECEIVED, and when BOTH say ONE and the
 * sign-in is still refused, the counts have said everything they can.  The
 * remaining question — *is it the same character?* — cannot be answered by a
 * length and must not be answered by printing the password.
 *
 * So: the first byte of SHA-256(text), as two hex digits.  Irreversible, and
 * eight bits is worth nothing to somebody who can already see the screen —
 * while two ends that DISAGREE prove the keystrokes differ (`keyboard.layout`
 * is a per-user setting, so it applies inside a session and not at the lock
 * screen), and two that AGREE prove they do not, which moves the search to the
 * stored record.
 *
 * `out` gets three bytes: two hex digits and a NUL. */
void user_secret_fingerprint(const char* text, char out[3]);

/* §M81 — WHY THE LAST GATE SAID NO, in the words it used.
 *
 * Every refusal here is printed AND recorded, because with the GUI up the
 * console is suppressed (§4.79) and a panel that can only see `-1` has to guess
 * at the reason — which it did, and the guess was one of four possibilities.
 * A caller shows this instead of what it expected to be true. */
const char* users_last_refusal(void);

/* ---------------------------------------------------------------------------
 * Lifecycle.  Every one of these gates on cred_current() (see the header) and
 * prints the reason when it refuses.  0 on success.
 * ------------------------------------------------------------------------- */
int  user_add(const char* name, int make_admin);
int  user_del(const char* name);
int  user_set_password(const char* name, const char* password);
int  user_set_admin(const char* name, int admin);
int  user_set_elevation(const char* name, int mode);
int  group_create(const char* name);
int  group_destroy(const char* name);
int  group_add_member(const char* gname, const char* uname);
int  group_del_member(const char* gname, const char* uname);

/* §M32 stage 8 — turn ELIGIBILITY into permission for ONE operation.
 *
 * Returns 0 when the caller may proceed and -1 when it may not, having said
 * why.  A SYSTEM or KERNEL context always proceeds; a logged-in administrator
 * re-authenticates; anybody else is refused.  `what` names the operation in
 * the prompt, because "password:" with no subject is a prompt people answer
 * without knowing what they are authorising. */
int auth_elevate(const char* what);

#endif /* USERS_H */
