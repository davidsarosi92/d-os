/* =============================================================================
 * config.c — kernel key/value store, persisted via the VFS.
 *
 * Cache structure: a singly-linked list of `struct entry` allocated on
 * the kernel heap.  All API calls are O(N) over the cache; with a
 * working set of a few dozen entries this is well within budget.
 *
 * The conf file at `/etc/d-os.conf` is parsed line-by-line at
 * `config_init`.  If the file is missing, that is not an error — we
 * just keep the in-memory defaults and a later `config_save` will
 * create the file.
 *
 * Parser is intentionally tolerant: blank lines, '#' comments, trailing
 * whitespace, and the key/value separator may all have surrounding
 * spaces.  No quoting, no escapes, no multi-line values.
 * ============================================================================= */

#include "config.h"
#include "settings.h"   /* §M32 — CONFIG_KEY descriptors carry the scope */
#include "cred.h"       /* §M32 — who may change a machine setting */
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "vfs.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include <stddef.h>
#include <stdint.h>

#define CONF_PATH       "/etc/d-os.conf"
#define MAX_LINE_LEN    256

/* ------------------------------------------------------------------- */
/* String helpers — no libc.                                            */
/* ------------------------------------------------------------------- */

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static size_t strlen_(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static char* strdup_(const char* s) {
    size_t n = strlen_(s) + 1;
    char* p = (char*)kmalloc(n);
    if (!p) return NULL;
    for (size_t i = 0; i < n; i++) p[i] = s[i];
    return p;
}
static int is_space(char c) { return c == ' ' || c == '\t'; }

/* ------------------------------------------------------------------- */
/* Cache state.                                                         */
/* ------------------------------------------------------------------- */

/* §M82 — TWO LAYERS IN ONE ENTRY.
 *
 * `value` is what everybody reads: the user's choice while one is attached,
 * the machine's otherwise.  When a signed-in user first overrides a
 * USER-scoped key, the machine's value is moved aside into `machine`
 * (`has_machine` = 0 means the machine had set nothing at all) and `user_set`
 * marks the override.  Three things depend on that record and were wrong
 * without it, each measured by `sessiontest`:
 *   - sign-out restores what the MACHINE says, not the compiled default (and
 *     only the keys the user actually changed);
 *   - the user's store holds only what the user set, rather than a snapshot of
 *     every user-scoped key in the cache — which froze machine values into the
 *     file and carried the previous user's choices into the next user's;
 *   - the machine store written during a session carries the machine's value,
 *     never the signed-in user's — otherwise one person's wallpaper becomes
 *     everybody's the first time an administrator presses Save. */
struct entry {
    char* key;
    char* value;
    char* machine;       /* the machine layer while `user_set`; else NULL  */
    uint8_t has_machine; /* 0 = the machine had no value for this key      */
    uint8_t user_set;    /* the active user overrides this key             */
    struct entry* next;
};
static struct entry* head = NULL;

/* ------------------------------------------------------------------- */
/* Built-in defaults.  Add new keys here so consumers always have a     */
/* sensible value even on a fresh system.                               */
/* ------------------------------------------------------------------- */
static const struct config_default builtin_defaults[] = {
    { "console.fg_color", "0xE0E0E0" },
    { "console.bg_color", "0x101828" },
    { "shell.prompt",     "d-os> "   },
    { "shell.motd",       "welcome." },
    { "keyboard.layout",  "us"       },
    { NULL, NULL }
};

/* ------------------------------------------------------------------- */
/* API.                                                                 */
/* ------------------------------------------------------------------- */

const char* config_get(const char* key, const char* default_value) {
    if (!key) return default_value;
    for (struct entry* e = head; e; e = e->next) {
        if (streq(e->key, key)) return e->value;
    }
    return default_value;
}

/* Parse a config value as a base-10 (long) integer, returning `def` when the
 * key is missing or the value is not a valid number.  Leading spaces and an
 * optional sign are accepted; parsing stops at the first non-digit. */
long config_get_long(const char* key, long def) {
    const char* s = config_get(key, (const char*)0);
    if (!s) return def;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    if (*s < '0' || *s > '9') return def;            /* no digits → default */
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static struct entry* find_entry(const char* key) {
    for (struct entry* e = head; e; e = e->next)
        if (streq(e->key, key)) return e;
    return NULL;
}

int config_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    /* Replace existing. */
    for (struct entry* e = head; e; e = e->next) {
        if (streq(e->key, key)) {
            char* nv = strdup_(value);
            if (!nv) return -2;
            kfree(e->value);
            e->value = nv;
            return 0;
        }
    }
    /* Append new — push to head so most-recently-set are found fastest. */
    struct entry* e = (struct entry*)kmalloc(sizeof *e);
    if (!e) return -3;
    e->key   = strdup_(key);
    e->value = strdup_(value);
    e->machine = NULL;
    e->has_machine = 0;
    e->user_set = 0;
    e->next  = head;
    head     = e;
    return 0;
}

/* ------------------------------------------------------------------- */
/* §M63 stage 0 — change notification + persistence.                    */
/* ------------------------------------------------------------------- */

/* Where a save actually lands.  Empty until a writable volume is attached. */
static char persist_path[160] = "";

static int starts_with_(const char* s, const char* pfx) {
    while (*pfx) { if (*s++ != *pfx++) return 0; }
    return 1;
}

void config_notify(const char* key, const char* value) {
    if (!key) return;
    int n = (int)(__stop_config_watches - __start_config_watches);
    for (int i = 0; i < n; i++) {
        const struct config_watch* w = &__start_config_watches[i];
        if (!w->changed) continue;
        if (w->prefix && *w->prefix && !starts_with_(key, w->prefix)) continue;
        w->changed(key, value);
    }
}

int config_apply(const char* key, const char* value) {
    if (!key || !value) return -1;

    /* §M32 stage 9 — WHO MAY CHANGE THIS.
     *
     * One decision point, because config_apply is what every route ends in:
     * the `conf` command, the settings panel, a watcher's re-apply and the
     * overlay loaders.  A rule placed in the panel instead would be a rule the
     * shell walks past.
     *
     * A MACHINE key needs an administrator.  A USER key is anybody's — it only
     * ever reaches their own store.  A SYSTEM context passes both, which is
     * what keeps boot, the overlays and every service working. */
    {
        const struct cred* c = cred_current();
        if (c->owner == TASK_OWNER_USER &&
            config_key_scope(key) == CFG_SCOPE_MACHINE &&
            !cred_is_admin(c)) {
            kprintf("config: '%s' is a machine setting and needs an "
                    "administrator\n", key);
            return -2;
        }
    }
    /* Notify only on a REAL change.  Re-applying the same value happens all
     * the time (a config file overlaid onto identical defaults, a panel
     * re-writing what is already there), and a subsystem told to re-read on
     * every no-op change would rebuild its state for nothing — at boot, that
     * is the whole defaults table. */
    const char* old = config_get(key, (const char*)0);
    int same = old && streq(old, value);

    /* §M82 — a user override moves the machine's value aside FIRST, while it
     * is still the value in the cache.  Recorded even when the new value is
     * the same, because "the user chose this" is a fact about the store even
     * when it is not a change on screen. */
    int user_layer = config_user_active() >= 0 &&
                     config_key_scope(key) == CFG_SCOPE_USER;
    char* moved = NULL;
    int   had   = 0;
    struct entry* pe = find_entry(key);
    if (user_layer && !(pe && pe->user_set)) {
        had = pe != NULL;
        if (had) { moved = strdup_(pe->value); if (!moved) return -3; }
    }
    int rc = config_set(key, value);
    if (rc == 0 && user_layer) {
        struct entry* ne = find_entry(key);
        if (ne && !ne->user_set) {
            ne->user_set    = 1;
            ne->has_machine = (uint8_t)had;
            ne->machine     = moved;
            moved = NULL;
        }
    }
    if (moved) kfree(moved);
    if (rc == 0 && !same) {
        /* LOG the decision.  A settings change is a change to how the machine
         * behaves, and until now the only trace of one was whatever the
         * subsystem chose to print — so a panel that applied a value and a
         * panel that silently did nothing produced the same (empty) log.  It
         * also makes the Control Panel testable without a screen. */
        klog(KLOG_INFO, "config", "%s = %s (was %s)\n", key, value,
             old ? old : "unset");
        config_notify(key, value);
        /* A preference changed BY a logged-in user belongs in that user's
         * store, not in the machine's.  Writing it to the machine store is
         * exactly the bug this stage exists to remove — it would make one
         * person's wallpaper everybody's. */
        if (config_user_active() >= 0 &&
            config_key_scope(key) == CFG_SCOPE_USER)
            config_user_save();
    }
    return rc;
}

const char* config_persist_path(void) {
    return persist_path[0] ? persist_path : (const char*)0;
}

/* Write the cache to `path`.  Split out of config_save so the persistent
 * target and the ramfs one share one writer. */
static int save_to(const char* path);

int config_attach_persistent(const char* dir) {
    if (!dir || !*dir) return -1;

    /* Build "<dir>/d-os.conf".  A flat file in the volume root rather than
     * "<dir>/etc/d-os.conf": creating a directory on exFAT is a code path this
     * has no reason to depend on, and a config file you can see at the top of
     * the disk is easier to rescue with another OS. */
    int n = 0;
    while (dir[n] && n < (int)sizeof persist_path - 12) { persist_path[n] = dir[n]; n++; }
    if (n > 0 && persist_path[n - 1] == '/') n--;          /* no double slash */
    const char* leaf = "/d-os.conf";
    for (int i = 0; leaf[i]; i++) persist_path[n++] = leaf[i];
    persist_path[n] = '\0';

    /* Load it if it is there.  Overlay via config_apply so any subsystem that
     * already consumed a key at boot is told the saved value differs — the
     * keyboard layout is the live example: keymap picks its layout long before
     * this runs. */
    int loaded = config_load_path(persist_path);
    if (loaded == 0) {
        kprintf("config: persistent store %s loaded\n", persist_path);
        return 0;
    }

    /* Not there (or unreadable).  Create it, which is also the only honest
     * test of whether this volume can be written at all — a persistent path we
     * merely HOPE is writable would turn every later save into a silent
     * failure, which is the exact bug this milestone exists to remove. */
    if (save_to(persist_path) == 0) {
        kprintf("config: persistent store %s created\n", persist_path);
        return 0;
    }

    persist_path[0] = '\0';
    klog(KLOG_WARN, "config",
         "%s not writable — settings will NOT survive a reboot\n", dir);
    return -1;
}

static int save_to(const char* path) {
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;

    /* Header comment so a hex-dump tells you what file this is. */
    const char* hdr = "# d-os configuration — managed by config.c\n";
    vfs_write(f, hdr, strlen_(hdr));

    for (struct entry* e = head; e; e = e->next) {
        /* The MACHINE's value, never the signed-in user's override (§M82). */
        const char* v = e->value;
        if (e->user_set) {
            if (!e->has_machine) continue;      /* the machine never set it */
            v = e->machine;
        }
        vfs_write(f, e->key,  strlen_(e->key));
        vfs_write(f, " = ",   3);
        vfs_write(f, v, strlen_(v));
        vfs_write(f, "\n",    1);
    }
    vfs_close(f);
    return 0;
}

int config_save(void) {
    /* The persistent target when one has been attached; the ramfs path
     * otherwise.  Note this NEVER fails over from one to the other: if the
     * disk write fails, saying so is the point — falling back to a copy in RAM
     * would report success for a save that vanishes at the next boot. */
    const char* p = config_persist_path();
    return save_to(p ? p : CONF_PATH);
}

/* ------------------------------------------------------------------- */
/* Shell commands — implemented HERE so both shells run one copy.       */
/*                                                                      */
/* §M24's rule, and this file was breaking it: `setconf`/`getconf`/     */
/* `saveconf` lived in shell.c only, so on aarch64 (which runs its own  */
/* serial_shell.c) there was NO way to change or save a setting at all. */
/* The §M63 stage-0 work found it the obvious way — the persistent      */
/* store was created on ARM, and then `saveconf` answered "unknown      */
/* command".  Config is the last subsystem that should be reachable on  */
/* one arch only.                                                       */
/* ------------------------------------------------------------------- */

void config_cmd_getconf(const char* key) {
    while (key && *key == ' ') key++;
    if (!key || !*key) { kprintf("getconf: missing key\n"); return; }
    const char* v = config_get(key, (const char*)0);
    if (v) kprintf("%s = %s\n", key, v);
    else   kprintf("%s: not set\n", key);
}

void config_cmd_setconf(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { kprintf("setconf: missing args\n"); return; }
    const char* p = args;
    while (*p && *p != ' ') p++;
    if (!*p) { kprintf("setconf: missing value\n"); return; }

    char key[64];
    int i = 0;
    while (args + i < p && i < (int)sizeof key - 1) { key[i] = args[i]; i++; }
    key[i] = 0;
    const char* val = p + 1;
    while (*val == ' ') val++;

    /* config_apply, not config_set: a key typed by a person is a DECISION, and
     * the subsystem that read it at boot has to hear about it. */
    if (config_apply(key, val) == 0) kprintf("%s = %s\n", key, val);
    else                             kprintf("setconf: failed\n");
}

void config_cmd_saveconf(void) {
    /* Report the PATH, not just success.  "config saved." was true and
     * useless: without a writable volume the save lands on ramfs and
     * evaporates at the next boot, which is exactly what the person doing
     * this needs to be told. */
    const char* p = config_persist_path();
    if (config_save() == 0) {
        if (p) kprintf("config saved to %s (survives reboot)\n", p);
        else   kprintf("config saved to %s on ramfs — will NOT survive a "
                       "reboot (no writable volume)\n", CONF_PATH);
    } else {
        kprintf("saveconf: failed writing %s\n", p ? p : CONF_PATH);
    }
}

/* Trim leading + trailing whitespace in place.  Returns a pointer into
 * the original buffer (no allocation). */
static char* trim(char* s) {
    while (*s && is_space(*s)) s++;
    char* end = s;
    while (*end) end++;
    while (end > s && (is_space(end[-1]) || end[-1] == '\r')) end--;
    *end = 0;
    return s;
}

/* Parse one line; on success register key/value via config_set. */
static void parse_line(char* line) {
    char* trimmed = trim(line);
    if (*trimmed == 0)   return;                /* blank */
    if (*trimmed == '#') return;                /* comment */

    /* Find '='. */
    char* eq = trimmed;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') return;                     /* malformed, skip */

    /* Split: trimmed..eq-1 is key, eq+1..end is value. */
    *eq = 0;
    char* key = trim(trimmed);
    char* val = trim(eq + 1);
    if (*key == 0) return;
    /* config_apply, not config_set: a file loaded AFTER boot (the persistent
     * store, attached once the disk is mounted) carries decisions subsystems
     * have already acted on, and the watchers are what let them catch up. */
    config_apply(key, val);
}

int config_load_path(const char* path) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;                          /* not an error — file may not exist yet */

    /* Read entire file.  Cap at 16 KiB which is a lot for a conf file. */
    enum { CAP = 16 * 1024 };
    char* buf = (char*)kmalloc(CAP);
    if (!buf) { vfs_close(f); return -2; }
    ssize_t got = 0, n;
    while ((n = vfs_read(f, buf + got, CAP - 1 - got)) > 0) got += n;
    buf[got] = 0;
    vfs_close(f);

    /* Tokenize on '\n' in place. */
    char* line = buf;
    for (ssize_t i = 0; i <= got; i++) {
        if (buf[i] == '\n' || buf[i] == 0) {
            buf[i] = 0;
            parse_line(line);
            line = buf + i + 1;
        }
    }
    kfree(buf);
    return 0;
}

int config_load(void) { return config_load_path(CONF_PATH); }

void config_init(void) {
    /* Plant defaults first.  config_set replaces existing entries, so
     * the subsequent file load can override any of these. */
    for (const struct config_default* d = builtin_defaults; d->key; d++) {
        config_set(d->key, d->value);
    }
    if (config_load() == 0) {
        kprintf("config: loaded %s\n", CONF_PATH);
    } else {
        klog(KLOG_NOTICE, "config", "%s missing — using defaults\n", CONF_PATH);
    }
}

void config_for_each(config_iter_fn fn, void* ctx) {
    if (!fn) return;
    for (struct entry* e = head; e; e = e->next) fn(e->key, e->value, ctx);
}

void config_dump(void) {
    int n = 0;
    for (struct entry* e = head; e; e = e->next) n++;
    kprintf("config (%d entries):\n", n);
    for (struct entry* e = head; e; e = e->next) {
        kprintf("  %s = %s\n", e->key, e->value);
    }
}

/* --- §M70 shell registrations ---------------------------------------------
 * §4.63 moved these out of shell.c so the ARM serial REPL could reach the
 * persistent store it was able to create and had no command able to write to.
 * The registration is what removes the second dispatch arm entirely. */

static void cf_config  (const char* a) { (void)a; config_dump(); }
static void cf_saveconf(const char* a) { (void)a; config_cmd_saveconf(); }

SHELL_CMD(config)   = { "config", "", "every config key currently in effect",
                        SHELL_G_SYS, cf_config, SHELL_P_ADMIN };
SHELL_CMD(getconf)  = { "getconf", "<key>", "read one config key",
                        SHELL_G_SYS, config_cmd_getconf, SHELL_P_ANY };
SHELL_CMD(setconf)  = { "setconf", "<key> <value>", "set a key (undeclared keys allowed)",
                        SHELL_G_SYS, config_cmd_setconf, SHELL_P_ANY };
SHELL_CMD(saveconf) = { "saveconf", "", "persist the config to disk",
                        SHELL_G_SYS, cf_saveconf, SHELL_P_ADMIN };

/* =============================================================================
 * §M32 stage 9 — PER-USER SETTINGS.
 *
 * The mechanism is §M63 stage 0's, used a second time: defaults, then the
 * machine store, then the USER store, last writer wins — and `config_apply`'s
 * watchers fire exactly as they do now, so a live theme or language switch
 * keeps working with no per-key code.
 *
 * WHAT IS DELIBERATELY NOT SOLVED HERE, and it is a real limit rather than an
 * oversight: the config cache is ONE cache for the machine, so a user's
 * preferences are applied when their session opens and withdrawn when it
 * closes.  With one session at a time that is correct.  **With two sessions at
 * once it is not** — the second login's wallpaper would be on the first user's
 * screen — and making it correct means a per-session view of the config, which
 * is the same change as a per-session compositor (§M32 stage 10).  Said here
 * because a reader who finds this working for one user must not conclude it
 * works for two.
 * ============================================================================= */

static char user_path[96];
static int  user_uid_active = -1;

int config_key_scope(const char* key) {
    const struct config_key_def* d = config_key_find(key);
    /* An UNDECLARED key is MACHINE.  `setconf` can still reach keys with no
     * descriptor, and treating those as personal would make "undescribed" a
     * way past the rule rather than merely a gap in the documentation. */
    return d ? d->scope : CFG_SCOPE_MACHINE;
}

/* Build "<vol>/d-os-user-<uid>.conf". */
static void user_store_path(int uid, char* out, int cap) {
    const char* base = config_persist_path();
    int n = 0;
    if (!base) { out[0] = 0; return; }
    /* Reuse the machine store's directory by trimming its leaf. */
    int last = -1;
    for (int i = 0; base[i]; i++) if (base[i] == '/') last = i;
    for (int i = 0; i < last && n < cap - 24; i++) out[n++] = base[i];
    const char* leaf = "/d-os-user-";
    for (int i = 0; leaf[i] && n < cap - 12; i++) out[n++] = leaf[i];
    char num[12]; int m = 0, v = uid < 0 ? 0 : uid;
    if (v == 0) num[m++] = '0';
    while (v > 0 && m < 12) { num[m++] = (char)('0' + v % 10); v /= 10; }
    while (m > 0 && n < cap - 6) out[n++] = num[--m];
    const char* ext = ".conf";
    for (int i = 0; ext[i] && n < cap - 1; i++) out[n++] = ext[i];
    out[n] = 0;
}

int config_user_attach(int uid) {
    user_uid_active = uid;
    user_store_path(uid, user_path, sizeof user_path);
    if (!user_path[0]) {
        klog(KLOG_INFO, "config",
             "uid %d has no writable volume — preferences are this session only\n", uid);
        return -1;
    }

    struct file* f = vfs_open(user_path, VFS_RDONLY);
    if (!f) return 0;                     /* no preferences yet: not an error */
    static char buf[2048];
    ssize_t got = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (got <= 0) return 0;
    buf[got] = 0;

    char line[192];
    int li = 0, applied = 0, refused = 0;
    for (ssize_t i = 0; i <= got; i++) {
        char ch = buf[i];
        if (ch != '\n' && ch != 0) { if (li < (int)sizeof line - 1) line[li++] = ch; continue; }
        line[li] = 0;
        li = 0;
        if (line[0] == '#' || line[0] == 0) { if (ch == 0) break; continue; }

        /* "key = value" */
        char k[96], v[96];
        int ki = 0, vi = 0, p = 0;
        while (line[p] && line[p] != ' ' && line[p] != '=' && ki < 95) k[ki++] = line[p++];
        k[ki] = 0;
        while (line[p] == ' ' || line[p] == '=') p++;
        while (line[p] && vi < 95) v[vi++] = line[p++];
        v[vi] = 0;

        if (config_key_scope(k) != CFG_SCOPE_USER) {
            /* IGNORED **WITH A LINE**.  Silently dropping it and silently
             * honouring it are both worse than saying so: one hides a file
             * somebody edited expecting it to work, the other is the privilege
             * escalation this scope exists to prevent. */
            klog(KLOG_WARN, "config",
                 "%s: '%s' is a machine setting and was ignored in a user store\n",
                 user_path, k);
            refused++;
            if (ch == 0) break;
            continue;
        }
        config_apply(k, v);
        applied++;
        if (ch == 0) break;
    }
    kprintf("config: %d preference(s) applied for uid %d%s\n", applied, uid,
            refused ? ", some machine settings ignored (see dmesg)" : "");
    return 0;
}

int config_user_detach(void) {
    if (user_uid_active < 0) return 0;
    int uid = user_uid_active;
    user_uid_active = -1;
    user_path[0] = 0;

    /* Put every key THIS USER overrode back to what the MACHINE says — and
     * where the machine had set nothing, back to unset, so every reader falls
     * to its own default exactly as before the session.  Without this the
     * last session's wallpaper stays on the screen after the logout — which is
     * not merely untidy: it leaks one user's preferences to the next person at
     * the console, and on a shared machine a preference can be a fact about
     * somebody (their language, their layout).
     *
     * §M82: this used to reset EVERY user-scoped key to its compiled default,
     * which threw away the administrator's machine-wide choice on every
     * sign-out and touched keys the user had never changed. */
    int restored = 0;
    struct entry** pp = &head;
    while (*pp) {
        struct entry* e = *pp;
        if (!e->user_set) { pp = &e->next; continue; }
        restored++;
        if (e->has_machine) {
            kfree(e->value);
            e->value = e->machine;
            e->machine = NULL;
            e->user_set = 0;
            e->has_machine = 0;
            klog(KLOG_INFO, "config", "%s = %s (machine value, user withdrawn)\n",
                 e->key, e->value);
            config_notify(e->key, e->value);
            pp = &e->next;
        } else {
            *pp = e->next;
            const struct config_key_def* d = config_key_find(e->key);
            const char* def = (d && d->def) ? d->def : "";
            klog(KLOG_INFO, "config", "%s unset (user withdrawn)\n", e->key);
            config_notify(e->key, def);
            kfree(e->key); kfree(e->value); kfree(e);
        }
    }
    kprintf("config: preferences for uid %d withdrawn (%d key(s))\n", uid, restored);
    return 0;
}

int config_user_save(void) {
    if (!user_path[0]) return -1;
    struct file* f = vfs_open(user_path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;
    const char* hdr = "# d-os per-user preferences — managed by config.c\n";
    vfs_write(f, hdr, strlen_(hdr));
    for (struct entry* e = head; e; e = e->next) {
        /* Only what THIS user chose (§M82) — not a snapshot of every
         * user-scoped key the cache happens to hold. */
        if (!e->user_set) continue;
        vfs_write(f, e->key, strlen_(e->key));
        vfs_write(f, " = ", 3);
        vfs_write(f, e->value, strlen_(e->value));
        vfs_write(f, "\n", 1);
    }
    vfs_close(f);
    return 0;
}

int config_user_active(void) { return user_uid_active; }
