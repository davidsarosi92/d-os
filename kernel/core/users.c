/* =============================================================================
 * users.c — the account database (§M32 stage 3).
 *
 * See users.h for the model and, more importantly, for the RULES: which
 * operations need root rather than an admin, why a uid is never reused, why
 * the gates read cred_current() instead of taking it as an argument, and what
 * the bootstrap console is.
 * ============================================================================= */

#include "users.h"
#include "cred.h"
#include "sha256.h"
#include "random.h"
#include "vfs.h"
#include "printf.h"
#include "klog.h"
#include "config.h"
#include "settings.h"
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * State.  Static tables rather than a heap: the login path must not be able to
 * fail for lack of memory, and an account database that cannot be read is a
 * machine nobody can get into.
 * ------------------------------------------------------------------------- */

static struct user_account g_users[USER_MAX_ACCOUNTS];
static struct group_entry  g_groups[USER_MAX_GROUPS];
/* THE HIGH-WATER MARK.  Only ever increases; persisted.  A deleted account's
 * uid is retired for the life of the machine (users.h explains why). */
static int  g_next_uid = USER_UID_FIRST;
static int  g_next_gid = 100;
static char g_passwd_path[96];
static char g_shadow_path[96];
static int  g_inited;

/* ---- tiny string helpers (this tree has no string library) --------------- */

static int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int s_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void s_copy(char* d, const char* s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void s_cat(char* d, const char* s, int cap) {
    int n = s_len(d);
    for (int i = 0; s && s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
}
static int s_atoi(const char* s) {
    int v = 0, neg = 0;
    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static void s_itoa(int v, char* d, int cap) {
    char t[12]; int n = 0, neg = 0, o = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < 12) { t[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg && o < cap - 1) d[o++] = '-';
    while (n > 0 && o < cap - 1) d[o++] = t[--n];
    d[o] = 0;
}
static const char* HEXD = "0123456789abcdef";
static void to_hex(const uint8_t* p, int n, char* out) {
    for (int i = 0; i < n; i++) {
        out[i * 2]     = HEXD[(p[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEXD[p[i] & 0xF];
    }
    out[n * 2] = 0;
}
static int from_hex(const char* s, uint8_t* out, int max) {
    int n = 0;
    while (s[0] && s[1] && n < max) {
        int hi = -1, lo = -1;
        for (int i = 0; i < 16; i++) { if (HEXD[i] == s[0]) hi = i; if (HEXD[i] == s[1]) lo = i; }
        if (hi < 0 || lo < 0) return n;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

/* Constant-time comparison of two hex digests.
 *
 * NOT premature caution: the alternative leaks how many leading bytes of a
 * guess were right, one comparison at a time, which is enough to reconstruct a
 * hash byte by byte given enough attempts.  It costs nothing to do correctly
 * and cannot be retrofitted convincingly once something depends on it. */
static int hash_equal(const char* a, const char* b) {
    int la = s_len(a), lb = s_len(b);
    if (la != lb) return 0;
    int diff = 0;
    for (int i = 0; i < la; i++) diff |= (a[i] ^ b[i]);
    return diff == 0;
}

/* ---------------------------------------------------------------------------
 * Lookup.
 * ------------------------------------------------------------------------- */

const struct user_account* user_by_name(const char* name) {
    if (!g_inited) users_init();
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (g_users[i].used && s_eq(g_users[i].name, name)) return &g_users[i];
    return NULL;
}

const struct user_account* user_by_uid(int uid) {
    if (!g_inited) users_init();
    if (uid == CRED_UID_NONE) return NULL;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (g_users[i].used && g_users[i].uid == uid) return &g_users[i];
    return NULL;
}

const char* user_name_of(int uid) {
    const struct user_account* u = user_by_uid(uid);
    return u ? u->name : NULL;
}

int user_count(void) {
    int n = 0;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) if (g_users[i].used) n++;
    return n;
}

const struct user_account* user_at(int i) {
    int n = 0;
    for (int k = 0; k < USER_MAX_ACCOUNTS; k++)
        if (g_users[k].used && n++ == i) return &g_users[k];
    return NULL;
}

const struct group_entry* group_by_name(const char* name) {
    for (int i = 0; i < USER_MAX_GROUPS; i++)
        if (g_groups[i].used && s_eq(g_groups[i].name, name)) return &g_groups[i];
    return NULL;
}

const struct group_entry* group_by_gid(int gid) {
    for (int i = 0; i < USER_MAX_GROUPS; i++)
        if (g_groups[i].used && g_groups[i].gid == gid) return &g_groups[i];
    return NULL;
}

const char* group_name_of(int gid) {
    const struct group_entry* g = group_by_gid(gid);
    return g ? g->name : NULL;
}

int group_count(void) {
    int n = 0;
    for (int i = 0; i < USER_MAX_GROUPS; i++) if (g_groups[i].used) n++;
    return n;
}

const struct group_entry* group_at(int i) {
    int n = 0;
    for (int k = 0; k < USER_MAX_GROUPS; k++)
        if (g_groups[k].used && n++ == i) return &g_groups[k];
    return NULL;
}

static int group_has(const struct group_entry* g, int uid) {
    if (!g) return 0;
    for (int i = 0; i < GROUP_MAX_MEMBERS; i++) if (g->members[i] == uid) return 1;
    return 0;
}

int user_groups_of(int uid, int* out, int max) {
    int n = 0;
    const struct user_account* u = user_by_uid(uid);
    if (u && n < max) out[n++] = u->gid;
    for (int i = 0; i < USER_MAX_GROUPS && n < max; i++) {
        if (!g_groups[i].used) continue;
        if (u && g_groups[i].gid == u->gid) continue;      /* already primary */
        if (group_has(&g_groups[i], uid)) out[n++] = g_groups[i].gid;
    }
    return n;
}

int user_is_admin_uid(int uid) {
    if (uid == CRED_UID_ROOT) return 1;        /* root is one by definition */
    const struct user_account* u = user_by_uid(uid);
    if (!u) return 0;
    if (u->gid == CRED_GID_ADMIN) return 1;
    return group_has(group_by_gid(CRED_GID_ADMIN), uid);
}

/* ---------------------------------------------------------------------------
 * The invariant that keeps the machine administrable.
 *
 * Deletion, demotion and password removal are three different ways to reach
 * the same unadministrable machine, so the check lives HERE and all three call
 * it.  Three copies would be three chances for one of them to be the path
 * somebody used.
 * ------------------------------------------------------------------------- */

/* How many accounts could log in as an admin if `excluding_uid` were gone (or
 * pass CRED_UID_NONE to count them all)?  "Could log in" is the operative
 * phrase: an admin with no password is not an administrator, it is a name. */
static int usable_admin_count(int excluding_uid) {
    int n = 0;
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        const struct user_account* u = &g_users[i];
        if (!u->used) continue;
        if (u->uid == excluding_uid) continue;
        if (u->type != USER_TYPE_PERSON) continue;
        if (!u->has_password) continue;
        if (!user_is_admin_uid(u->uid)) continue;
        n++;
    }
    return n;
}

/* §M81 — A REFUSAL THAT ONLY REACHES THE CONSOLE REACHES NOBODY.
 *
 * Every gate below printed its reason with `kprintf` and returned -1.  That is
 * the right place to SAY it and the wrong place to LEAVE it: with the GUI up
 * the console is suppressed (§4.79), so the accounts panel could only report
 * that something was refused and then GUESS at why — it guessed "an
 * administrator's password needs root", which happens to be one of four
 * reasons and is simply wrong for the other three.
 *
 * Reported from use as *"REFUSED - the password for david was not changed"*
 * followed by exactly the right question: **"the GUI comes up — which user is
 * this?  It should be root but it never asked for a password."**  It is a
 * SYSTEM session (`gui.login` is off by default), which `cred_is_admin` answers
 * 1 for and `actor_is_root` answers 0 for once any account has a chosen secret
 * — so a non-admin's password can be changed from it and an admin's cannot.
 * Every word of that was on the console and none of it on the screen.
 *
 * The reason is RECORDED as well as printed, so a caller can show the one that
 * actually fired instead of the one it expected. */
static char g_refusal[192];

/* TWO TEXTS, and the split is the point.  `brief` is what a one-line label can
 * hold and what the user can ACT on; `full` is the explanation, and it goes to
 * the console where there is room for it.  The first version recorded the long
 * one and the accounts panel clipped it mid-sentence — so the actionable half
 * ("sign in as root") fell off the right-hand edge, which is the same defect as
 * printing nothing, dressed up as a message. */
static void refuse(const char* brief, const char* full) {
    int i = 0;
    for (; brief[i] && i < (int)sizeof g_refusal - 1; i++) g_refusal[i] = brief[i];
    g_refusal[i] = 0;
    kprintf("users: refused - %s\n", full);
}

const char* users_last_refusal(void) {
    return g_refusal[0] ? g_refusal : "no reason was recorded";
}

static int would_strand_machine(int uid_losing_admin) {
    if (usable_admin_count(uid_losing_admin) > 0) return 0;
    refuse("it would leave nobody able to administer this machine",
           "that would leave the machine with no administrator who can log in");
    return 1;
}

/* ---------------------------------------------------------------------------
 * The gates.  Each answers ONE question and prints its own reason.
 * ------------------------------------------------------------------------- */

static int actor_is_admin(void) {
    if (cred_is_admin(cred_current())) return 1;
    refuse("this needs an administrator",
           "this needs an administrator, and this session is not one");
    return 0;
}

static int actor_is_root(void) {
    const struct cred* c = cred_current();
    /* A SYSTEM task counts as root ONLY while the machine has no administrator
     * yet — that is the bootstrap console (users.h), and it closes the moment
     * an account can log in.  Without this the installer could not set the
     * first password; with it unconditional, the console would be a permanent
     * way around every account on the machine. */
    if (c->owner != TASK_OWNER_USER) {
        if (users_needs_setup()) return 1;
        refuse("sign in as root first",
               "the machine already has an administrator, so this SYSTEM "
               "session no longer counts as root - sign in as root to do this");
        return 0;
    }
    if (cred_is_root(c)) return 1;
    refuse("only root may do this",
           "only root may do this, and only while signed in as root");
    return 0;
}

/* Is this account one that only root may touch? */
static int is_protected(const struct user_account* u) {
    return u && u->uid == CRED_UID_ROOT;
}

/* ---------------------------------------------------------------------------
 * Password material.
 * ------------------------------------------------------------------------- */

static uint32_t kdf_iterations(void) {
    long v = config_get_long("security.kdf_iterations", USER_KDF_ITERS_DEFAULT);
    if (v < 1000) v = 1000;              /* a floor, not a suggestion */
    if (v > 5000000) v = 5000000;
    return (uint32_t)v;
}

static void derive(const char* password, const char* salt_hex,
                   uint32_t iters, char* out_hex) {
    uint8_t salt[USER_SALT_BYTES];
    uint8_t dk[SHA256_DIGEST_LEN];
    int sl = from_hex(salt_hex, salt, USER_SALT_BYTES);
    pbkdf2_sha256(password, (size_t)s_len(password), salt, (size_t)sl,
                  iters, dk, SHA256_DIGEST_LEN);
    to_hex(dk, SHA256_DIGEST_LEN, out_hex);
}

void user_secret_fingerprint(const char* text, char out[3]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t d[SHA256_DIGEST_LEN];
    int n = 0;
    while (text && text[n]) n++;
    sha256(text ? text : "", (size_t)n, d);
    out[0] = hex[(d[0] >> 4) & 0xF];
    out[1] = hex[d[0] & 0xF];
    out[2] = 0;
}

int user_check_password(const char* name, const char* password) {
    const struct user_account* u = user_by_name(name);
    /* One answer for "no such user" and "wrong password".  Distinguishing them
     * turns the login prompt into a way to enumerate accounts, and the caller
     * has one message for both anyway. */
    if (!u || !u->has_password || u->type != USER_TYPE_PERSON) return -1;
    char got[65];
    derive(password ? password : "", u->salt_hex, u->kdf_iters, got);
    return hash_equal(got, u->hash_hex) ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Persistence.
 * ------------------------------------------------------------------------- */

static int write_line(struct file* f, const char* s) {
    int n = s_len(s);
    return vfs_write(f, s, (size_t)n) == n ? 0 : -1;
}

static int save_passwd(const char* path) {
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;
    char line[256], num[16];
    write_line(f, "# d-os accounts — name:uid:gid:type:elevation:home\n");
    /* The high-water mark is part of the DATABASE, not a derived value.  If it
     * were recomputed as max(uid)+1 at load, deleting the highest account would
     * hand its uid to the next one created — which is the whole defect this
     * file refuses to have. */
    s_copy(line, "@nextuid:", sizeof line);
    s_itoa(g_next_uid, num, sizeof num); s_cat(line, num, sizeof line);
    s_cat(line, ":", sizeof line);
    s_itoa(g_next_gid, num, sizeof num); s_cat(line, num, sizeof line);
    s_cat(line, "\n", sizeof line);
    write_line(f, line);

    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        const struct user_account* u = &g_users[i];
        if (!u->used) continue;
        s_copy(line, u->name, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa(u->uid, num, sizeof num);        s_cat(line, num, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa(u->gid, num, sizeof num);        s_cat(line, num, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa(u->type, num, sizeof num);       s_cat(line, num, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa(u->elevation, num, sizeof num);  s_cat(line, num, sizeof line);
        s_cat(line, ":", sizeof line);
        s_cat(line, u->home, sizeof line);
        s_cat(line, "\n", sizeof line);
        write_line(f, line);
    }

    write_line(f, "# groups — @group:name:gid:uid,uid,...\n");
    for (int i = 0; i < USER_MAX_GROUPS; i++) {
        const struct group_entry* g = &g_groups[i];
        if (!g->used) continue;
        s_copy(line, "@group:", sizeof line);
        s_cat(line, g->name, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa(g->gid, num, sizeof num); s_cat(line, num, sizeof line);
        s_cat(line, ":", sizeof line);
        int first = 1;
        for (int m = 0; m < GROUP_MAX_MEMBERS; m++) {
            if (g->members[m] == CRED_UID_NONE) continue;
            if (!first) s_cat(line, ",", sizeof line);
            s_itoa(g->members[m], num, sizeof num);
            s_cat(line, num, sizeof line);
            first = 0;
        }
        s_cat(line, "\n", sizeof line);
        write_line(f, line);
    }
    vfs_close(f);
    return 0;
}

/* The hashes live in their OWN file, exactly as /etc/shadow does.
 *
 * Not decoration: the account list is something every surface displays (an
 * owner column needs names), while the hashes are the one thing on the machine
 * whose disclosure IS the compromise.  Two files is what lets stage 5 give
 * them different modes; one file would force the readable half to be secret
 * or the secret half to be readable. */
static int save_shadow(const char* path) {
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;
    char line[256], num[16];
    write_line(f, "# d-os password hashes — name:iterations:salt:pbkdf2-sha256\n");
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        const struct user_account* u = &g_users[i];
        if (!u->used || !u->has_password) continue;
        s_copy(line, u->name, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa((int)u->kdf_iters, num, sizeof num); s_cat(line, num, sizeof line);
        s_cat(line, ":", sizeof line);
        s_cat(line, u->salt_hex, sizeof line);
        s_cat(line, ":", sizeof line);
        s_cat(line, u->hash_hex, sizeof line);
        s_cat(line, ":", sizeof line);
        s_itoa(u->pw_is_default, num, sizeof num); s_cat(line, num, sizeof line);
        s_cat(line, "\n", sizeof line);
        write_line(f, line);
    }
    vfs_close(f);
    return 0;
}

int users_save(void) {
    if (!g_passwd_path[0]) return -1;         /* RAM only; the caller says so */
    if (save_passwd(g_passwd_path) != 0) return -1;
    if (save_shadow(g_shadow_path) != 0) return -1;
    return 0;
}

/* Split `line` at ':' into up to `max` fields, in place. */
static int split(char* line, char** fields, int max, char sep) {
    int n = 0;
    fields[n++] = line;
    for (char* p = line; *p && n < max; p++) {
        if (*p == sep) { *p = 0; fields[n++] = p + 1; }
    }
    return n;
}

static struct user_account* alloc_slot(void) {
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (!g_users[i].used) return &g_users[i];
    return NULL;
}

static struct group_entry* alloc_group(void) {
    for (int i = 0; i < USER_MAX_GROUPS; i++)
        if (!g_groups[i].used) return &g_groups[i];
    return NULL;
}

static void parse_passwd_line(char* line) {
    if (line[0] == '#' || line[0] == 0) return;

    if (line[0] == '@') {
        char* f[8];
        int n = split(line, f, 8, ':');
        if (s_eq(f[0], "@nextuid") && n >= 3) {
            int nu = s_atoi(f[1]), ng = s_atoi(f[2]);
            /* NEVER LOWER IT.  A store written by an older build, or one that
             * has been edited, must not be able to hand out a uid that has
             * already been used. */
            if (nu > g_next_uid) g_next_uid = nu;
            if (ng > g_next_gid) g_next_gid = ng;
            return;
        }
        if (s_eq(f[0], "@group") && n >= 4) {
            struct group_entry* g = alloc_group();
            if (!g) return;
            /* A group named in the file may already exist as a built-in. */
            const struct group_entry* ex = group_by_name(f[1]);
            if (ex) g = (struct group_entry*)ex;
            g->used = 1;
            s_copy(g->name, f[1], sizeof g->name);
            g->gid = s_atoi(f[2]);
            for (int i = 0; i < GROUP_MAX_MEMBERS; i++) g->members[i] = CRED_UID_NONE;
            char* mem[GROUP_MAX_MEMBERS];
            int mn = split(f[3], mem, GROUP_MAX_MEMBERS, ',');
            int w = 0;
            for (int i = 0; i < mn && w < GROUP_MAX_MEMBERS; i++)
                if (mem[i][0]) g->members[w++] = s_atoi(mem[i]);
            return;
        }
        return;
    }

    char* f[8];
    if (split(line, f, 8, ':') < 6) return;
    const struct user_account* ex = user_by_name(f[0]);
    struct user_account* u = ex ? (struct user_account*)ex : alloc_slot();
    if (!u) return;
    u->used = 1;
    s_copy(u->name, f[0], sizeof u->name);
    u->uid       = s_atoi(f[1]);
    u->gid       = s_atoi(f[2]);
    u->type      = s_atoi(f[3]);
    u->elevation = s_atoi(f[4]);
    s_copy(u->home, f[5], sizeof u->home);
}

static void parse_shadow_line(char* line) {
    if (line[0] == '#' || line[0] == 0) return;
    char* f[6];
    int n = split(line, f, 6, ':');
    if (n < 4) return;
    const struct user_account* ex = user_by_name(f[0]);
    if (!ex) return;
    struct user_account* u = (struct user_account*)ex;
    u->kdf_iters = (uint32_t)s_atoi(f[1]);
    s_copy(u->salt_hex, f[2], sizeof u->salt_hex);
    s_copy(u->hash_hex, f[3], sizeof u->hash_hex);
    u->has_password = 1;
    /* Field 4 is optional: a store written before this flag existed simply has
     * no fifth column, and the absent value means "not the default" — which is
     * the safe reading, because it under-claims (it will not warn about a
     * password somebody actually chose). */
    u->pw_is_default = (n >= 5) ? s_atoi(f[4]) : 0;
}

static int load_file(const char* path, void (*fn)(char*)) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    static char buf[4096];
    ssize_t got = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (got <= 0) return -1;
    buf[got] = 0;

    char line[256];
    int li = 0;
    for (ssize_t i = 0; i <= got; i++) {
        char ch = buf[i];
        if (ch == '\n' || ch == 0) {
            line[li] = 0;
            if (li > 0) fn(line);
            li = 0;
            if (ch == 0) break;
        } else if (li < (int)sizeof line - 1) {
            line[li++] = ch;
        }
    }
    return 0;
}

/* Make sure every account's home exists, with the right owner and 0700.
 * Idempotent: an existing directory is left alone except for its ownership,
 * which is re-asserted because ramfs is rebuilt at every boot while the
 * account database survives. */
void users_ensure_homes(void) {
    vfs_mkdir("/home");
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++) {
        struct user_account* u = &g_users[i];
        if (!u->used || !u->home[0]) continue;
        vfs_mkdir(u->home);                  /* -2 when it already exists */
        vfs_chmod(u->home, 0700);
        vfs_chown(u->home, u->uid, u->gid);
        users_ensure_pref_store(u->uid);
    }
}

/* §M32 stage 9 — CREATE THE USER'S PREFERENCE FILE, OWNED BY THEM.
 *
 * The store lives in the volume's root beside the machine's, and that root is
 * root-owned 0755 — so a logged-in user cannot CREATE a file there, and their
 * first `setconf` saved nothing.  Measured, not reasoned about: two users set
 * two wallpapers and the second one's was still on screen for the third login.
 *
 * The file is therefore made HERE, by the system, while it still can, and
 * handed to its owner at 0600 — the same trick as the home directory one
 * function up.  *The alternative was to let the save bypass the permission
 * check, which is a boundary with a door in it for the code that put the
 * boundary there.* */
void users_ensure_pref_store(int uid) {
    const char* base = config_persist_path();
    if (!base) return;                       /* no writable volume */
    char path[96];
    int n = 0, last = -1;
    for (int i = 0; base[i]; i++) if (base[i] == '/') last = i;
    for (int i = 0; i < last && n < (int)sizeof path - 24; i++) path[n++] = base[i];
    const char* leaf = "/d-os-user-";
    for (int i = 0; leaf[i]; i++) path[n++] = leaf[i];
    char num[12]; int m = 0, v = uid < 0 ? 0 : uid;
    if (v == 0) num[m++] = '0';
    while (v > 0 && m < 12) { num[m++] = (char)('0' + v % 10); v /= 10; }
    while (m > 0) path[n++] = num[--m];
    const char* ext = ".conf";
    for (int i = 0; ext[i]; i++) path[n++] = ext[i];
    path[n] = 0;

    struct file* f = vfs_open(path, VFS_RDONLY);
    if (f) { vfs_close(f); }
    else {
        f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
        if (!f) return;
        const char* hdr = "# d-os per-user preferences\n";
        int hl = 0; while (hdr[hl]) hl++;
        vfs_write(f, hdr, (size_t)hl);
        vfs_close(f);
    }
    vfs_chmod(path, 0600);
    vfs_chown(path, uid, uid);
}

int users_attach_persistent(const char* dir) {
    if (!dir || !*dir) return -1;

    s_copy(g_passwd_path, dir, sizeof g_passwd_path);
    int n = s_len(g_passwd_path);
    if (n > 0 && g_passwd_path[n - 1] == '/') g_passwd_path[n - 1] = 0;
    s_copy(g_shadow_path, g_passwd_path, sizeof g_shadow_path);
    s_cat(g_passwd_path, "/d-os-passwd", sizeof g_passwd_path);
    s_cat(g_shadow_path, "/d-os-shadow", sizeof g_shadow_path);

    /* EVERY ACCOUNT'S HOME, created here rather than only in user_add.
     *
     * root is built by users_init, which runs before the VFS exists — so
     * `/root` was a field in the database that nothing ever made, and the
     * first thing that tried to write there failed with "open failed", which
     * reads as a permission problem and is not one.  *A home directory that is
     * a string and not a directory is the §M64 shortcut bug in a new costume.*
     * Accounts loaded from the store have the same problem: they were created
     * on a previous boot, and `/` is ramfs. */
    int loaded = load_file(g_passwd_path, parse_passwd_line);
    if (loaded == 0) {
        load_file(g_shadow_path, parse_shadow_line);
        kprintf("users: account database %s loaded (%d account(s), %d group(s))\n",
                g_passwd_path, user_count(), group_count());
        users_ensure_homes();
        return 0;
    }

    /* Not there.  Creating it is the only honest test that this volume can be
     * written at all — §M63 stage 0's lesson, where a path we merely HOPED was
     * writable turned every later save into a silent failure. */
    if (users_save() == 0) {
        kprintf("users: account database %s created\n", g_passwd_path);
        users_ensure_homes();
        return 0;
    }

    g_passwd_path[0] = 0;
    g_shadow_path[0] = 0;
    klog(KLOG_WARN, "users",
         "%s not writable — accounts will NOT survive a reboot\n", dir);
    /* Homes are made even here.  They live on ramfs, so they were never going
     * to survive anyway — but a machine with no disk must still be usable, and
     * an account whose home is missing is one whose session opens with a
     * warning for a reason that has nothing to do with the disk. */
    users_ensure_homes();
    return -1;
}

/* ---------------------------------------------------------------------------
 * Bring-up.
 * ------------------------------------------------------------------------- */

static void make_group(const char* name, int gid) {
    struct group_entry* g = alloc_group();
    if (!g) return;
    g->used = 1;
    s_copy(g->name, name, sizeof g->name);
    g->gid = gid;
    for (int i = 0; i < GROUP_MAX_MEMBERS; i++) g->members[i] = CRED_UID_NONE;
}

void users_init(void) {
    if (g_inited) return;
    g_inited = 1;

    make_group("admins", CRED_GID_ADMIN);
    make_group("users",  100);

    /* root.  Exists on every machine, is a PERSON (only root may delete an
     * admin, which requires root to be loginable), and starts with NO
     * PASSWORD — so it cannot be logged into until somebody sets one.  See
     * users.h on why that is the bootstrap and not a hole. */
    struct user_account* r = alloc_slot();
    r->used      = 1;
    s_copy(r->name, "root", sizeof r->name);
    r->uid       = CRED_UID_ROOT;
    r->gid       = CRED_GID_ADMIN;
    r->type      = USER_TYPE_PERSON;
    r->elevation = USER_ELEV_PER_OP;
    s_copy(r->home, "/root", sizeof r->home);
    r->kdf_iters    = USER_KDF_ITERS_DEFAULT;

    /* THE DEFAULT CREDENTIAL (users.h).  Set here so a freshly built machine
     * has an account somebody can actually sign in as — the bootstrap console
     * is a way to CONFIGURE a machine, not a way to use one. */
    {
        uint8_t salt[USER_SALT_BYTES];
        random_bytes(salt, sizeof salt);
        to_hex(salt, USER_SALT_BYTES, r->salt_hex);
        derive(USER_DEFAULT_PASSWORD, r->salt_hex, r->kdf_iters, r->hash_hex);
        r->has_password  = 1;
        r->pw_is_default = 1;
    }
}

int users_needs_setup(void) {
    if (!g_inited) users_init();
    /* "Somebody has CHOSEN a secret", not "an account exists".  root ships with
     * a default password (users.h), so counting that as configured would close
     * the installer console on a machine nobody has set up yet — and the
     * console is how it gets set up. */
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (g_users[i].used && g_users[i].type == USER_TYPE_PERSON &&
            g_users[i].has_password && !g_users[i].pw_is_default) return 0;
    return 1;
}

int users_default_password_in_use(void) {
    if (!g_inited) users_init();
    for (int i = 0; i < USER_MAX_ACCOUNTS; i++)
        if (g_users[i].used && g_users[i].pw_is_default) return 1;
    return 0;
}

const char* users_default_name(void) {
    return config_get("users.default_user", USER_DEFAULT_NAME);
}

void users_warn_default_password(void) {
    if (!users_default_password_in_use()) return;
    /* EVERY BOOT, until it is changed.  A default credential nobody is told
     * about is a backdoor; one that announces itself is a task on a list. */
    kprintf("\n!! '%s' still has the SHIPPED DEFAULT PASSWORD (\"%s\").\n"
            "!! Anybody who knows this system can sign in.  Change it with "
            "`passwd %s <new>`.\n\n",
            USER_DEFAULT_NAME, USER_DEFAULT_PASSWORD, USER_DEFAULT_NAME);
}

/* ---------------------------------------------------------------------------
 * Lifecycle.
 * ------------------------------------------------------------------------- */

static int name_ok(const char* name) {
    if (!name || !*name) { kprintf("users: a name is required\n"); return 0; }
    if (s_len(name) > USER_NAME_MAX) {
        kprintf("users: '%s' is longer than %d characters\n", name, USER_NAME_MAX);
        return 0;
    }
    /* The store is colon-separated text, so a name containing a separator
     * would write a line that parses back as something else — a name with a
     * ':' in it could inject a second account on the next load.  Refused at
     * the door rather than escaped, because there is no reason to allow it. */
    for (const char* p = name; *p; p++) {
        if (*p == ':' || *p == ',' || *p == '\n' || *p == '@' || *p == ' ') {
            kprintf("users: '%c' is not allowed in a name\n", *p);
            return 0;
        }
    }
    return 1;
}

int user_add(const char* name, int make_admin) {
    if (!name_ok(name)) return -1;
    /* Creating an ADMIN is a root operation; creating an ordinary person is an
     * admin one.  An admin who could create admins could promote themselves
     * past the account that is allowed to unmake them. */
    if (make_admin) { if (!actor_is_root())  return -1; }
    else            { if (!actor_is_admin()) return -1; }

    if (user_by_name(name)) {
        kprintf("users: '%s' already exists\n", name);
        return -1;
    }
    struct user_account* u = alloc_slot();
    if (!u) { kprintf("users: no free account slots (max %d)\n", USER_MAX_ACCOUNTS); return -1; }

    u->used      = 1;
    s_copy(u->name, name, sizeof u->name);
    u->uid       = g_next_uid++;
    u->gid       = make_admin ? CRED_GID_ADMIN : 100;
    u->type      = USER_TYPE_PERSON;
    u->elevation = USER_ELEV_PER_OP;
    u->has_password = 0;
    u->kdf_iters    = kdf_iterations();
    s_copy(u->home, "/home/", sizeof u->home);
    s_cat(u->home, name, sizeof u->home);

    /* THE HOME DIRECTORY IS CREATED HERE, not left as a field nothing acts on.
     * Mode 0700 — private by default.  The difference between 0700 and 0755 is
     * the difference between a home and a public directory, and a default that
     * is readable by everybody is one nobody will notice is wrong. */
    vfs_mkdir("/home");
    if (vfs_mkdir(u->home) == 0) {
        vfs_chmod(u->home, 0700);
        vfs_chown(u->home, u->uid, u->gid);
    }

    users_ensure_pref_store(u->uid);
    kprintf("users: created '%s' uid %d, home %s%s\n",
            u->name, u->uid, u->home, make_admin ? ", ADMIN" : "");
    if (users_save() != 0)
        kprintf("users: (RAM only — no writable volume, this will not survive a reboot)\n");
    return 0;
}

int user_del(const char* name) {
    const struct user_account* cu = user_by_name(name);
    if (!cu) { kprintf("users: no such account '%s'\n", name); return -1; }
    struct user_account* u = (struct user_account*)cu;

    /* root is protected against everybody, INCLUDING root.  Protection is a
     * property of the account, not of who is asking — otherwise the one
     * account that cannot be removed would be removable by exactly the
     * identity most likely to be borrowed. */
    if (is_protected(u)) {
        kprintf("users: 'root' is protected and cannot be deleted\n");
        return -1;
    }
    /* Deleting an ADMIN needs root; deleting an ordinary person needs an
     * admin.  This is the decision that gives the protected account its job. */
    if (user_is_admin_uid(u->uid)) { if (!actor_is_root())  return -1; }
    else                           { if (!actor_is_admin()) return -1; }

    if (user_is_admin_uid(u->uid) && would_strand_machine(u->uid)) return -1;

    int uid = u->uid;
    /* The uid is NOT returned to the pool — see users.h.  Nothing here touches
     * g_next_uid, which is the entire mechanism. */
    for (int i = 0; i < USER_MAX_GROUPS; i++) {
        if (!g_groups[i].used) continue;
        for (int m = 0; m < GROUP_MAX_MEMBERS; m++)
            if (g_groups[i].members[m] == uid) g_groups[i].members[m] = CRED_UID_NONE;
    }
    kprintf("users: deleted '%s' (uid %d retired, never reused)\n", u->name, uid);
    /* The FILES are left where they are, owned by a uid nothing claims.  That
     * is the deliberate consequence of never reusing a uid: they become
     * unreadable to everyone but an admin, which is a recoverable state, where
     * handing them to the next account created is not. */
    kprintf("users: files owned by uid %d are left in place and now belong to "
            "no account\n", uid);
    for (int i = 0; i < (int)(sizeof *u); i++) ((char*)u)[i] = 0;

    if (users_save() != 0)
        kprintf("users: (RAM only — this deletion will not survive a reboot)\n");
    return 0;
}

int user_set_password(const char* name, const char* password) {
    const struct user_account* cu = user_by_name(name);
    if (!cu) { kprintf("users: no such account '%s'\n", name); return -1; }
    struct user_account* u = (struct user_account*)cu;

    const struct cred* actor = cred_current();
    int self = (actor->owner == TASK_OWNER_USER && cred_uid(actor) == u->uid);
    if (!self) {
        /* Somebody else's password.  An ADMIN's needs root — otherwise one
         * admin could take over another admin's account and the "only root may
         * delete an admin" rule would be reachable by a detour. */
        if (user_is_admin_uid(u->uid)) { if (!actor_is_root())  return -1; }
        else                           { if (!actor_is_admin()) return -1; }
    }

    if (!password || !*password) {
        /* Removing a password DISABLES login; it does not create an empty one.
         * Gated by the same invariant as deletion, because it reaches the same
         * unadministrable machine by a different road. */
        if (user_is_admin_uid(u->uid) && would_strand_machine(u->uid)) return -1;
        u->has_password = 0;
        kprintf("users: '%s' can no longer log in (no password)\n", u->name);
    } else {
        uint8_t salt[USER_SALT_BYTES];
        random_bytes(salt, sizeof salt);
        to_hex(salt, USER_SALT_BYTES, u->salt_hex);
        u->kdf_iters = kdf_iterations();
        derive(password, u->salt_hex, u->kdf_iters, u->hash_hex);
        u->has_password  = 1;
        u->pw_is_default = 0;          /* somebody has now chosen a secret */
        kprintf("users: password set for '%s' (pbkdf2-sha256, %u iterations)\n",
                u->name, u->kdf_iters);
    }

    if (users_save() != 0)
        kprintf("users: (RAM only — this will not survive a reboot)\n");
    return 0;
}

int user_set_admin(const char* name, int admin) {
    const struct user_account* cu = user_by_name(name);
    if (!cu) { kprintf("users: no such account '%s'\n", name); return -1; }
    struct user_account* u = (struct user_account*)cu;

    /* Promotion AND demotion are root's, and root itself can be neither. */
    if (!actor_is_root()) return -1;
    if (is_protected(u)) {
        kprintf("users: 'root' is protected and cannot be removed from the "
                "admin group\n");
        return -1;
    }
    if (!admin && would_strand_machine(u->uid)) return -1;

    struct group_entry* g = (struct group_entry*)group_by_gid(CRED_GID_ADMIN);
    if (!g) { kprintf("users: the admin group is missing\n"); return -1; }

    if (admin) {
        if (!group_has(g, u->uid)) {
            int placed = 0;
            for (int i = 0; i < GROUP_MAX_MEMBERS && !placed; i++)
                if (g->members[i] == CRED_UID_NONE) { g->members[i] = u->uid; placed = 1; }
            if (!placed) { kprintf("users: the admin group is full\n"); return -1; }
        }
    } else {
        for (int i = 0; i < GROUP_MAX_MEMBERS; i++)
            if (g->members[i] == u->uid) g->members[i] = CRED_UID_NONE;
        if (u->gid == CRED_GID_ADMIN) u->gid = 100;
    }
    kprintf("users: '%s' is %s an administrator\n", u->name, admin ? "now" : "no longer");
    if (users_save() != 0)
        kprintf("users: (RAM only — this will not survive a reboot)\n");
    return 0;
}

int user_set_elevation(const char* name, int mode) {
    const struct user_account* cu = user_by_name(name);
    if (!cu) { kprintf("users: no such account '%s'\n", name); return -1; }
    struct user_account* u = (struct user_account*)cu;
    if (!actor_is_root()) return -1;
    if (mode != USER_ELEV_PER_OP && mode != USER_ELEV_ALWAYS) {
        kprintf("users: elevation must be per-operation or always\n");
        return -1;
    }
    u->elevation = mode;
    kprintf("users: '%s' elevation is now %s\n", u->name,
            mode == USER_ELEV_ALWAYS ? "always" : "per-operation");
    if (mode == USER_ELEV_ALWAYS)
        kprintf("users: NOTE — 'always' is recorded but NOT implemented; this "
                "account still re-authenticates per operation\n");
    if (users_save() != 0)
        kprintf("users: (RAM only — this will not survive a reboot)\n");
    return 0;
}

int group_create(const char* name) {
    if (!name_ok(name)) return -1;
    if (!actor_is_admin()) return -1;
    if (group_by_name(name)) { kprintf("users: group '%s' already exists\n", name); return -1; }
    struct group_entry* g = alloc_group();
    if (!g) { kprintf("users: no free group slots (max %d)\n", USER_MAX_GROUPS); return -1; }
    g->used = 1;
    s_copy(g->name, name, sizeof g->name);
    g->gid = g_next_gid++;
    for (int i = 0; i < GROUP_MAX_MEMBERS; i++) g->members[i] = CRED_UID_NONE;
    kprintf("users: created group '%s' gid %d\n", g->name, g->gid);
    if (users_save() != 0) kprintf("users: (RAM only)\n");
    return 0;
}

int group_destroy(const char* name) {
    const struct group_entry* cg = group_by_name(name);
    if (!cg) { kprintf("users: no such group '%s'\n", name); return -1; }
    /* The admin group is not a group like the others: destroying it would
     * demote every administrator at once, which is the stranding case reached
     * by a third road. */
    if (cg->gid == CRED_GID_ADMIN) {
        kprintf("users: the admin group is protected and cannot be destroyed\n");
        return -1;
    }
    if (!actor_is_admin()) return -1;
    struct group_entry* g = (struct group_entry*)cg;
    for (int i = 0; i < (int)(sizeof *g); i++) ((char*)g)[i] = 0;
    kprintf("users: destroyed group '%s'\n", name);
    if (users_save() != 0) kprintf("users: (RAM only)\n");
    return 0;
}

int group_add_member(const char* gname, const char* uname) {
    const struct group_entry* cg = group_by_name(gname);
    const struct user_account* u = user_by_name(uname);
    if (!cg) { kprintf("users: no such group '%s'\n", gname); return -1; }
    if (!u)  { kprintf("users: no such account '%s'\n", uname); return -1; }
    /* Adding somebody to the ADMIN group IS promoting them, so it goes through
     * the same gate — reached here by a different verb, and a gate that only
     * covers one of the two verbs is not a gate. */
    if (cg->gid == CRED_GID_ADMIN) return user_set_admin(uname, 1);
    if (!actor_is_admin()) return -1;

    struct group_entry* g = (struct group_entry*)cg;
    if (group_has(g, u->uid)) { kprintf("users: already a member\n"); return 0; }
    for (int i = 0; i < GROUP_MAX_MEMBERS; i++)
        if (g->members[i] == CRED_UID_NONE) {
            g->members[i] = u->uid;
            kprintf("users: '%s' added to group '%s'\n", u->name, g->name);
            if (users_save() != 0) kprintf("users: (RAM only)\n");
            return 0;
        }
    kprintf("users: group '%s' is full (max %d)\n", g->name, GROUP_MAX_MEMBERS);
    return -1;
}

int group_del_member(const char* gname, const char* uname) {
    const struct group_entry* cg = group_by_name(gname);
    const struct user_account* u = user_by_name(uname);
    if (!cg) { kprintf("users: no such group '%s'\n", gname); return -1; }
    if (!u)  { kprintf("users: no such account '%s'\n", uname); return -1; }
    if (cg->gid == CRED_GID_ADMIN) return user_set_admin(uname, 0);
    if (!actor_is_admin()) return -1;
    struct group_entry* g = (struct group_entry*)cg;
    for (int i = 0; i < GROUP_MAX_MEMBERS; i++)
        if (g->members[i] == u->uid) g->members[i] = CRED_UID_NONE;
    kprintf("users: '%s' removed from group '%s'\n", u->name, g->name);
    if (users_save() != 0) kprintf("users: (RAM only)\n");
    return 0;
}

/* =============================================================================
 * Commands.  They live here, next to the code they drive (§M70's rule), which
 * is also what puts them on all three architectures and in both shells from
 * one registration.
 * ========================================================================== */

#include "shellcmd.h"
#include "console.h"

/* Split "a b" into two NUL-terminated words.  Returns how many were found. */
static int two_words(const char* args, char* a, int acap, char* b, int bcap) {
    int n = 0, i = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < acap - 1) a[i++] = *args++;
    a[i] = 0;
    if (i) n++;
    while (*args == ' ') args++;
    i = 0;
    while (*args && *args != ' ' && i < bcap - 1) b[i++] = *args++;
    b[i] = 0;
    if (i) n++;
    return n;
}

static void cmd_users(const char* args) {
    (void)args;
    kprintf("USER  UID  GID  TYPE  ADMIN  LOGIN  ELEVATION  HOME\n");
    for (int i = 0; i < user_count(); i++) {
        const struct user_account* u = user_at(i);
        if (!u) continue;
        kprintf("%s   %d   %d   %s   %s   %s   %s   %s\n",
                u->name, u->uid, u->gid,
                u->type == USER_TYPE_PERSON ? "person" : "system",
                user_is_admin_uid(u->uid) ? "yes" : "no",
                /* "can log in" is a DIFFERENT column from "is an admin", and
                 * keeping them apart is what makes the last-administrator
                 * invariant readable: an admin with no password is a name. */
                u->has_password ? "yes" : "NO-PASSWORD",
                u->elevation == USER_ELEV_ALWAYS ? "always" : "per-op",
                u->home);
    }
    kprintf("groups:\n");
    for (int i = 0; i < group_count(); i++) {
        const struct group_entry* g = group_at(i);
        if (!g) continue;
        kprintf("  %s (gid %d):", g->name, g->gid);
        int any = 0;
        for (int m = 0; m < GROUP_MAX_MEMBERS; m++) {
            if (g->members[m] == CRED_UID_NONE) continue;
            const char* n = user_name_of(g->members[m]);
            kprintf(" %s", n ? n : "?");
            any = 1;
        }
        /* A group's members by PRIMARY gid are not in the member list, so a
         * group that looks empty may not be.  Said, rather than left to be
         * discovered by somebody wondering why root is not in `admins`. */
        for (int k = 0; k < user_count(); k++) {
            const struct user_account* u = user_at(k);
            if (u && u->gid == g->gid) { kprintf(" %s(primary)", u->name); any = 1; }
        }
        if (!any) kprintf(" (empty)");
        kprintf("\n");
    }
    if (users_needs_setup()) {
        /* This sentence used to read "NO ACCOUNT CAN LOG IN YET", which was
         * true until root started shipping with a default password and became
         * false in the same change — the exact §M52 shape this milestone keeps
         * closing, so it is corrected rather than left. */
        if (users_default_password_in_use())
            kprintf("users: THIS MACHINE IS NOT SET UP — '%s' still has the "
                    "shipped default password, so this console is the "
                    "installer.  Change it with `passwd %s <new>`.\n",
                    USER_DEFAULT_NAME, USER_DEFAULT_NAME);
        else
            console_write("users: no account can sign in — this console is the "
                          "installer; set a password with `passwd root <new>`\n");
    }
}

static void cmd_useradd(const char* args) {
    char name[64], flag[16];
    two_words(args, name, sizeof name, flag, sizeof flag);
    if (!name[0]) { console_write("usage: useradd <name> [admin]\n"); return; }
    int admin = s_eq(flag, "admin");
    user_add(name, admin);
}

static void cmd_userdel(const char* args) {
    char name[64], unused[8];
    two_words(args, name, sizeof name, unused, sizeof unused);
    if (!name[0]) { console_write("usage: userdel <name>\n"); return; }
    user_del(name);
}

static void cmd_passwd(const char* args) {
    char name[64], pw[128];
    two_words(args, name, sizeof name, pw, sizeof pw);
    if (!name[0]) { console_write("usage: passwd <name> <password>   (or `passwd <name> -` to disable login)\n"); return; }
    if (!pw[0])   { console_write("passwd: a password is required (use '-' to disable login)\n"); return; }
    /* A password typed as a command ARGUMENT is visible in the shell history
     * and in the serial log.  That is why §M32 stage 4 adds an interactive,
     * echo-suppressed prompt — this form stays because it is the only one a
     * headless test can drive, and it says so rather than pretending. */
    if (s_eq(pw, "-")) user_set_password(name, NULL);
    else               user_set_password(name, pw);
}

static void cmd_usermod(const char* args) {
    char name[64], what[32];
    two_words(args, name, sizeof name, what, sizeof what);
    if (!name[0] || !what[0]) {
        console_write("usage: usermod <name> admin|noadmin|elev-perop|elev-always\n");
        return;
    }
    if      (s_eq(what, "admin"))       user_set_admin(name, 1);
    else if (s_eq(what, "noadmin"))     user_set_admin(name, 0);
    else if (s_eq(what, "elev-perop"))  user_set_elevation(name, USER_ELEV_PER_OP);
    else if (s_eq(what, "elev-always")) user_set_elevation(name, USER_ELEV_ALWAYS);
    else console_write("usermod: unknown change\n");
}

static void cmd_groupadd(const char* args) {
    char name[64], u[8];
    two_words(args, name, sizeof name, u, sizeof u);
    if (!name[0]) { console_write("usage: groupadd <name>\n"); return; }
    group_create(name);
}

static void cmd_groupdel(const char* args) {
    char name[64], u[8];
    two_words(args, name, sizeof name, u, sizeof u);
    if (!name[0]) { console_write("usage: groupdel <name>\n"); return; }
    group_destroy(name);
}

static void cmd_groupmod(const char* args) {
    char gname[64], rest[96];
    two_words(args, gname, sizeof gname, rest, sizeof rest);
    char verb[16], uname[64];
    two_words(rest, verb, sizeof verb, uname, sizeof uname);
    if (!gname[0] || !verb[0] || !uname[0]) {
        console_write("usage: groupmod <group> add|del <user>\n");
        return;
    }
    if      (s_eq(verb, "add")) group_add_member(gname, uname);
    else if (s_eq(verb, "del")) group_del_member(gname, uname);
    else console_write("groupmod: expected add or del\n");
}

SHELL_CMD(users)    = { "users",    "",  "list accounts and groups",
                        SHELL_G_SYS, cmd_users, SHELL_P_ANY };
SHELL_CMD(useradd)  = { "useradd",  "<name> [admin]", "create an account",
                        SHELL_G_SYS, cmd_useradd, SHELL_P_ADMIN };
SHELL_CMD(userdel)  = { "userdel",  "<name>", "delete an account",
                        SHELL_G_SYS, cmd_userdel, SHELL_P_ADMIN };
SHELL_CMD(passwd)   = { "passwd",   "<name> <password>", "set or clear a password",
                        SHELL_G_SYS, cmd_passwd, SHELL_P_ADMIN };
SHELL_CMD(usermod)  = { "usermod",  "<name> admin|noadmin|elev-perop|elev-always",
                        "change an account", SHELL_G_SYS, cmd_usermod, SHELL_P_ADMIN };
SHELL_CMD(groupadd) = { "groupadd", "<name>", "create a group",
                        SHELL_G_SYS, cmd_groupadd, SHELL_P_ADMIN };
SHELL_CMD(groupdel) = { "groupdel", "<name>", "destroy a group",
                        SHELL_G_SYS, cmd_groupdel, SHELL_P_ADMIN };
SHELL_CMD(groupmod) = { "groupmod", "<group> add|del <user>", "change group membership",
                        SHELL_G_SYS, cmd_groupmod, SHELL_P_ADMIN };

CONFIG_KEY(ck_default_user) = {
    .key = "users.default_user", .group = "System", .type = CFG_STRING,
    .def = USER_DEFAULT_NAME,
    .help = "the account the sign-in surfaces offer first",
    .scope = CFG_SCOPE_MACHINE,
};
