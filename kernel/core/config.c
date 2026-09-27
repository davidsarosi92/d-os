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
#include "lock.h"
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

/* §M32 (2026-09-27) — LAYERS, AND WHO IS ASKING DECIDES WHICH ONE ANSWERS.
 *
 * `head` is the MACHINE layer: compiled defaults overlaid by the machine
 * store.  Every signed-in account with a live session has a LAYER of its own
 * (`layers[]`, reference-counted by sessions) holding only the USER-scoped
 * keys it has overridden.  A lookup answers from the CALLER's layer first:
 *   - a task owned by a user (TASK_OWNER_USER) reads its own account's layer;
 *   - a SYSTEM or KERNEL task reads the CONSOLE user's — whoever holds the
 *     machine's seat (the GUI session, or a text login when no GUI user is
 *     signed in), because the compositor, the keymap and every other
 *     subsystem with global state act for the person at the screen.
 * Then the machine layer.
 *
 * THIS REPLACED §M82'S "TWO LAYERS IN ONE ENTRY" (the machine value moved
 * aside while a user's sat in the cache), which was correct for one session
 * and wrong for two: the second login's preferences overwrote the first's in
 * the one cache, so bob logging in on a console put bob's wallpaper on alice's
 * screen.  With a layer per account nothing is moved aside and nothing has to
 * be moved back: the machine layer only ever holds machine values, a user
 * store only ever the user's overrides, and sign-out drops a layer.
 *
 * WATCHERS follow the console.  A change reaches the subsystems (config_notify)
 * only when it changes what the CONSOLE sees: a user key written into the
 * console user's layer, a machine value the console layer does not override,
 * or the console passing from one account to another (every key either layer
 * overrides is re-announced).  A background session's preference is stored
 * and read back by that session, and never repaints somebody else's screen. */
struct entry {
    char* key;
    char* value;
    struct entry* next;
};
static struct entry* head = NULL;

#define CFG_MAX_LAYERS 8
struct ulayer {
    int  uid;
    int  refs;                   /* live sessions of this account; 0 = free   */
    struct entry* head;
    char path[96];               /* its store on the volume, "" = none        */
};
static struct ulayer layers[CFG_MAX_LAYERS];
static int      n_layers;        /* slots with refs > 0 — the fast-path gate   */
static int      console_uid = -1;
static unsigned console_token;   /* bumped per claim; a session releases only
                                  * the claim it made (see config_user_detach) */

/* THE STORE IS SHARED BY EVERY TASK, AND WAS UNLOCKED (2026-09-25).  The
 * compositor reads it every frame, the desktop and every app read it, and a
 * settings panel, a shell or a session switch writes it from yet another task.
 * Two defects followed: `config_set` FREED the old value while a reader was
 * still holding the string `config_get` had just returned, and the list was
 * spliced (a new entry at the head, a user's override removed at sign-out)
 * under concurrent traversal — which can lose an entry or resurrect a freed
 * one that a later write then scribbles into.
 *
 * So: the list is walked and changed only under `cfg_lock`; and memory that a
 * reader may still be using — an old value, a removed entry — is RETIRED into
 * a small ring and freed only when the ring comes round, CFG_RETIRE changes
 * later.  A reader would have to hold a returned pointer across that many
 * config changes on other tasks to see it go, which no caller in this tree
 * does (they read, parse, and let go).  Nothing blocking or re-entrant runs
 * under the lock: file writes work from a snapshot, watchers are told after. */
static spinlock_t cfg_lock = SPINLOCK_INIT;
#define CFG_RETIRE 256
static void*    cfg_retired[CFG_RETIRE];
static unsigned cfg_retire_i;
static void* retire_locked(void* p) {          /* returns what is now safe to free */
    if (!p) return NULL;
    void* old = cfg_retired[cfg_retire_i];
    cfg_retired[cfg_retire_i] = p;
    cfg_retire_i = (cfg_retire_i + 1) % CFG_RETIRE;
    return old;
}

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

/* Caller holds cfg_lock. */
static struct entry* find_in(struct entry* h, const char* key) {
    for (struct entry* e = h; e; e = e->next)
        if (streq(e->key, key)) return e;
    return NULL;
}
static struct ulayer* layer_of_locked(int uid) {
    if (uid < 0) return NULL;
    for (int i = 0; i < CFG_MAX_LAYERS; i++)
        if (layers[i].refs > 0 && layers[i].uid == uid) return &layers[i];
    return NULL;
}
/* The layer that answers for the calling task (see the block comment). */
static int viewer_uid(void) {
    const struct cred* c = cred_current();
    return c->owner == TASK_OWNER_USER ? c->uid : console_uid;
}
static struct ulayer* viewer_layer_locked(void) { return layer_of_locked(viewer_uid()); }

const char* config_get(const char* key, const char* default_value) {
    if (!key) return default_value;
    const char* v = default_value;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    struct ulayer* L = n_layers ? viewer_layer_locked() : NULL;
    struct entry* e = L ? find_in(L->head, key) : NULL;
    if (!e) e = find_in(head, key);
    if (e) v = e->value;
    spin_unlock_irqrestore(&cfg_lock, fl);
    return v;                        /* stays valid: values are retired, not freed */
}

/* §M82 — the MACHINE layer's value, whoever asks (a user's `$PATH` expands to
 * this).  Same lifetime rule as config_get. */
const char* config_get_machine(const char* key, const char* default_value) {
    if (!key) return default_value;
    const char* v = default_value;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    struct entry* e = find_in(head, key);
    if (e) v = e->value;
    spin_unlock_irqrestore(&cfg_lock, fl);
    return v;
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

/* Caller holds cfg_lock; `*freeable` receives memory that may now be freed
 * (after the lock is dropped).  `hp` is the list: the machine layer or an
 * account's. */
static int set_in_locked(struct entry** hp, const char* key, const char* value,
                         void** freeable) {
    *freeable = NULL;
    struct entry* e = find_in(*hp, key);
    if (e) {
        char* nv = strdup_(value);
        if (!nv) return -2;
        *freeable = retire_locked(e->value);
        e->value = nv;
        return 0;
    }
    /* Append new — push to head so most-recently-set are found fastest. */
    e = (struct entry*)kmalloc(sizeof *e);
    if (!e) return -3;
    e->key   = strdup_(key);
    e->value = strdup_(value);
    if (!e->key || !e->value) {
        if (e->key) kfree(e->key);
        if (e->value) kfree(e->value);
        kfree(e);
        return -3;
    }
    e->next  = *hp;
    *hp      = e;
    return 0;
}
static int set_locked(const char* key, const char* value, void** freeable) {
    return set_in_locked(&head, key, value, freeable);
}

/* "key = value\n" for the entries `mode` selects, built under cfg_lock so a
 * writer that sleeps (the VFS) never runs inside it.  mode 0 = the MACHINE
 * layer (the machine store), 1 = layer `L` alone (that user's store),
 * 2 = everything as the CALLER currently sees it (config_dump).  Caller
 * kfree()s. */
static char* snapshot_text(int mode, struct ulayer* L, size_t* out_len) {
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    if (mode == 2) L = n_layers ? viewer_layer_locked() : NULL;
    size_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        char* buf = NULL;
        if (pass == 1) {
            buf = (char*)kmalloc(n + 1);
            if (!buf) { spin_unlock_irqrestore(&cfg_lock, fl); *out_len = 0; return NULL; }
        }
        size_t k = 0;
        /* Up to two lists: the layer (modes 1 and 2), then the machine's
         * entries the layer does not override (modes 0 and 2). */
        for (int list = 0; list < 2; list++) {
            if (list == 0 && (mode == 0 || !L)) continue;
            if (list == 1 && mode == 1) continue;
            for (struct entry* e = list == 0 ? L->head : head; e; e = e->next) {
                if (list == 1 && L && mode == 2 && find_in(L->head, e->key)) continue;
                const char* parts[4] = { mode == 2 ? "  " : "", e->key, " = ", e->value };
                for (int q = 0; q < 4; q++)
                    for (const char* c = parts[q]; *c; c++) { if (buf) buf[k] = *c; k++; }
                if (buf) buf[k] = '\n';
                k++;
            }
        }
        if (pass == 0) { n = k; continue; }
        buf[k] = 0;
        spin_unlock_irqrestore(&cfg_lock, fl);
        *out_len = k;
        return buf;
    }
    spin_unlock_irqrestore(&cfg_lock, fl);
    *out_len = 0;
    return NULL;
}

int config_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    void* fr;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    int rc = set_locked(key, value, &fr);
    spin_unlock_irqrestore(&cfg_lock, fl);
    if (fr) kfree(fr);
    return rc;
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

static int layer_save(int uid, const char* path);

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
     * is the whole defaults table.
     *
     * §M32 — WHERE IT LANDS: a USER-scoped key goes to the CALLER's layer when
     * the caller has one (its own account's, or the console user's for a
     * SYSTEM task); anything else goes to the machine layer. */
    int scope = config_key_scope(key);
    uint32_t cfl = spin_lock_irqsave(&cfg_lock);
    struct ulayer* L = (scope == CFG_SCOPE_USER && n_layers) ? viewer_layer_locked() : NULL;
    struct entry* pe0 = L ? find_in(L->head, key) : NULL;
    if (!pe0) pe0 = find_in(head, key);
    const char* old = pe0 ? pe0->value : (const char*)0;   /* retired, not freed */
    int same = old && streq(old, value);
    void* fr = NULL;
    int rc = L ? set_in_locked(&L->head, key, value, &fr) : set_locked(key, value, &fr);
    /* Does the CONSOLE see this change?  (See the block comment on layers.) */
    int visible;
    if (L) {
        visible = L->uid == console_uid;
    } else {
        struct ulayer* CL = layer_of_locked(console_uid);
        visible = !(CL && find_in(CL->head, key));
    }
    int save_uid = L ? L->uid : -1;
    char save_path[96];
    save_path[0] = 0;
    if (L) for (int i = 0; i < (int)sizeof save_path; i++) {
        save_path[i] = L->path[i];
        if (!L->path[i]) break;
    }
    spin_unlock_irqrestore(&cfg_lock, cfl);
    if (fr) kfree(fr);
    if (rc == 0 && !same) {
        /* LOG the decision.  A settings change is a change to how the machine
         * behaves, and until now the only trace of one was whatever the
         * subsystem chose to print — so a panel that applied a value and a
         * panel that silently did nothing produced the same (empty) log.  It
         * also makes the Control Panel testable without a screen. */
        if (save_uid >= 0)
            klog(KLOG_INFO, "config", "%s = %s (was %s) for uid %d%s\n", key, value,
                 old ? old : "unset", save_uid, visible ? "" : " (background session)");
        else
            klog(KLOG_INFO, "config", "%s = %s (was %s)\n", key, value,
                 old ? old : "unset");
        if (visible) config_notify(key, value);
    }
    /* A preference changed BY a signed-in user belongs in that user's store,
     * not in the machine's.  Writing it to the machine store is exactly the
     * bug stage 9 exists to remove — it would make one person's wallpaper
     * everybody's.  Saved even when unchanged on screen: "the user chose
     * this" is a fact about the store. */
    if (rc == 0 && save_uid >= 0 && save_path[0]) layer_save(save_uid, save_path);
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
        /* §M87 — the settings (and the user store, the shortcuts and the
         * package store beside them) live on this volume from now on, so the
         * disk manager must not be able to unmount or format it underneath
         * them.  The hold NAMES the dependency, which is what a refused
         * "Unmount" says to the person who pressed it. */
        vfs_mount_hold(dir, "the settings store");
        return 0;
    }

    /* Not there (or unreadable).  Create it, which is also the only honest
     * test of whether this volume can be written at all — a persistent path we
     * merely HOPE is writable would turn every later save into a silent
     * failure, which is the exact bug this milestone exists to remove. */
    if (save_to(persist_path) == 0) {
        kprintf("config: persistent store %s created\n", persist_path);
        vfs_mount_hold(dir, "the settings store");          /* §M87 */
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

    /* The MACHINE's values, never the signed-in user's overrides (§M82) —
     * taken as a snapshot so the file write runs outside cfg_lock. */
    size_t len = 0;
    char* text = snapshot_text(0, NULL, &len);
    if (text) { vfs_write(f, text, len); kfree(text); }
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
    /* Pointers snapshotted under the lock, the callback run outside it (it may
     * print, or read config itself).  Keys and values are retired rather than
     * freed, so the pointers outlive the snapshot. */
    /* §M32 — as the CALLER sees it: its layer's overrides, then the machine's
     * entries they do not shadow. */
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    struct ulayer* L = n_layers ? viewer_layer_locked() : NULL;
    int n = 0;
    for (struct entry* e = head; e; e = e->next) n++;
    if (L) for (struct entry* e = L->head; e; e = e->next) n++;
    const char** kv = (const char**)kmalloc(sizeof(char*) * 2 * (size_t)(n ? n : 1));
    int k = 0;
    if (kv) {
        if (L) for (struct entry* e = L->head; e && k < n; e = e->next) {
            kv[2*k] = e->key; kv[2*k+1] = e->value; k++;
        }
        for (struct entry* e = head; e && k < n; e = e->next) {
            if (L && find_in(L->head, e->key)) continue;
            kv[2*k] = e->key; kv[2*k+1] = e->value; k++;
        }
    }
    spin_unlock_irqrestore(&cfg_lock, fl);
    if (!kv) return;
    for (int i = 0; i < k; i++) fn(kv[2*i], kv[2*i+1], ctx);
    kfree(kv);
}

void config_dump(void) {
    size_t len = 0;
    char* text = snapshot_text(2, NULL, &len);
    int n = 0;
    for (size_t i = 0; text && i < len; i++) if (text[i] == '\n') n++;
    kprintf("config (%d entries):\n", n);
    if (text) { kprintf("%s", text); kfree(text); }
}

/* --- §M70 shell registrations ---------------------------------------------
 * §4.63 moved these out of shell.c so the ARM serial REPL could reach the
 * persistent store it was able to create and had no command able to write to.
 * The registration is what removes the second dispatch arm entirely. */

static void cf_config  (const char* a) { (void)a; config_dump(); config_layers_dump(); }
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

/* Read an account's store into its (fresh, empty) layer.  Keys that are not
 * USER-scoped are IGNORED WITH A LINE: silently dropping one hides a file
 * somebody edited expecting it to work, and silently honouring one is the
 * privilege escalation the scope exists to prevent. */
static int layer_load(struct ulayer* L) {
    struct file* f = vfs_open(L->path, VFS_RDONLY);
    if (!f) return 0;                     /* no preferences yet: not an error */
    static char buf[2048];
    ssize_t got = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (got <= 0) return 0;
    buf[got] = 0;

    char line[192];
    int li = 0, applied = 0;
    for (ssize_t i = 0; i <= got; i++) {
        char ch = buf[i];
        if (ch != '\n' && ch != 0) { if (li < (int)sizeof line - 1) line[li++] = ch; continue; }
        line[li] = 0;
        li = 0;
        if (line[0] == '#' || line[0] == 0) { if (ch == 0) break; continue; }
        char k[96], v[96];
        int ki = 0, vi = 0, q = 0;
        while (line[q] && line[q] != ' ' && line[q] != '=' && ki < 95) k[ki++] = line[q++];
        k[ki] = 0;
        while (line[q] == ' ' || line[q] == '=') q++;
        while (line[q] && vi < 95) v[vi++] = line[q++];
        v[vi] = 0;
        if (config_key_scope(k) != CFG_SCOPE_USER) {
            klog(KLOG_WARN, "config",
                 "%s: '%s' is a machine setting and was ignored in a user store\n",
                 L->path, k);
        } else {
            void* fr = NULL;
            uint32_t fl = spin_lock_irqsave(&cfg_lock);
            set_in_locked(&L->head, k, v, &fr);
            spin_unlock_irqrestore(&cfg_lock, fl);
            if (fr) kfree(fr);
            applied++;
        }
        if (ch == 0) break;
    }
    return applied;
}

/* The console passed from `old_uid` to `new_uid` (either may be -1): tell
 * the subsystems about every key either account overrides, with the value
 * the console now sees.  Collected under the lock, announced after it —
 * a watcher may read config or repaint, and neither may run inside a
 * spinlock.  Keys and values are retired, never freed early, so the
 * pointers stay readable. */
#define ANNOUNCE_MAX 128
static void announce_console_change(int old_uid, int new_uid) {
    const char* nkey[ANNOUNCE_MAX];
    const char* nval[ANNOUNCE_MAX];
    int nn = 0;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    struct ulayer* A = layer_of_locked(old_uid);
    struct ulayer* B = layer_of_locked(new_uid);
    for (int pass = 0; pass < 2; pass++) {
        struct ulayer* X = pass == 0 ? A : B;
        if (!X || (pass == 1 && A == B)) continue;
        for (struct entry* e = X->head; e && nn < ANNOUNCE_MAX; e = e->next) {
            int dup = 0;
            for (int i = 0; i < nn; i++) if (streq(nkey[i], e->key)) { dup = 1; break; }
            if (dup) continue;
            struct entry* v = B ? find_in(B->head, e->key) : NULL;
            if (!v) v = find_in(head, e->key);
            nkey[nn] = e->key;
            if (v) nval[nn] = v->value;
            else {
                const struct config_key_def* d = config_key_find(e->key);
                nval[nn] = (d && d->def) ? d->def : "";
            }
            nn++;
        }
    }
    spin_unlock_irqrestore(&cfg_lock, fl);
    for (int i = 0; i < nn; i++) config_notify(nkey[i], nval[i]);
}

int config_user_attach(int uid, int seat) {
    if (uid < 0) return -1;
    int fresh = 0;
    struct ulayer* L = NULL;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    L = layer_of_locked(uid);
    if (L) {
        L->refs++;
    } else {
        for (int i = 0; i < CFG_MAX_LAYERS; i++)
            if (layers[i].refs == 0) { L = &layers[i]; break; }
        if (L) {
            L->uid = uid; L->refs = 1; L->head = NULL; L->path[0] = 0;
            n_layers++;
            fresh = 1;
        }
    }
    spin_unlock_irqrestore(&cfg_lock, fl);
    if (!L) {
        kprintf("config: too many signed-in accounts (%d) - uid %d gets the "
                "machine's settings this session\n", CFG_MAX_LAYERS, uid);
        return -1;
    }
    int loaded = 0;
    if (fresh) {
        user_store_path(uid, L->path, sizeof L->path);
        if (!L->path[0])
            klog(KLOG_INFO, "config",
                 "uid %d has no writable volume — preferences are this session only\n", uid);
        else
            loaded = layer_load(L);
    }

    /* Does this session take the seat? */
    int token = 0, old = -1;
    fl = spin_lock_irqsave(&cfg_lock);
    if (seat == CFG_SEAT_TAKE || (seat == CFG_SEAT_IF_FREE && console_uid < 0)) {
        old = console_uid;
        console_uid = uid;
        token = (int)(++console_token & 0x7fffffff);
        if (!token) token = (int)(++console_token & 0x7fffffff);
    }
    int refs = L->refs;
    spin_unlock_irqrestore(&cfg_lock, fl);
    if (token && old != uid) announce_console_change(old, uid);
    kprintf("config: uid %d attached (%s%d session(s)%s) - %s\n", uid,
            fresh ? "" : "already ", refs,
            fresh ? "" : " with this one",
            token ? "this session holds the console" :
                    "a BACKGROUND session: its preferences are its own and repaint nobody's screen");
    if (fresh) kprintf("config: %d preference(s) loaded for uid %d\n", loaded, uid);
    return token;
}

int config_user_detach(int uid, int token) {
    if (uid < 0) return 0;
    int released = 0, dropped = 0;
    struct entry* doomed = NULL;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    struct ulayer* L = layer_of_locked(uid);
    if (!L) { spin_unlock_irqrestore(&cfg_lock, fl); return 0; }
    /* Release the seat only if THIS session's claim is the current one: a
     * text session that took the console and was then overtaken by a GUI
     * sign-in must not, by logging out, take the seat from the GUI user. */
    if (token && (unsigned)token == (console_token & 0x7fffffff) && console_uid == uid) {
        console_uid = -1;
        released = 1;
    }
    spin_unlock_irqrestore(&cfg_lock, fl);
    /* Announce while the layer still exists: the keys to re-announce are its. */
    if (released) announce_console_change(uid, -1);

    fl = spin_lock_irqsave(&cfg_lock);
    if (--L->refs == 0) {
        doomed = L->head;
        L->head = NULL;
        L->uid = -1;
        L->path[0] = 0;
        n_layers--;
        dropped = 1;
    }
    /* Retire (not free) what readers may still hold. */
    void* tofree[3 * ANNOUNCE_MAX];
    int nf = 0;
    for (struct entry* e = doomed; e; ) {
        struct entry* nx = e->next;
        void* x;
        if ((x = retire_locked(e->key))   && nf < 3 * ANNOUNCE_MAX) tofree[nf++] = x;
        if ((x = retire_locked(e->value)) && nf < 3 * ANNOUNCE_MAX) tofree[nf++] = x;
        if ((x = retire_locked(e))        && nf < 3 * ANNOUNCE_MAX) tofree[nf++] = x;
        e = nx;
    }
    spin_unlock_irqrestore(&cfg_lock, fl);
    for (int i = 0; i < nf; i++) kfree(tofree[i]);
    kprintf("config: uid %d detached%s%s\n", uid,
            released ? " - the console is back on the machine's settings" : "",
            dropped ? "" : " (the account still has another session)");
    return 0;
}

static int layer_save(int uid, const char* path) {
    if (!path || !path[0]) return -1;
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return -1;
    const char* hdr = "# d-os per-user preferences — managed by config.c\n";
    vfs_write(f, hdr, strlen_(hdr));
    size_t len = 0;
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    struct ulayer* L = layer_of_locked(uid);
    spin_unlock_irqrestore(&cfg_lock, fl);
    char* text = L ? snapshot_text(1, L, &len) : NULL;
    if (text) { vfs_write(f, text, len); kfree(text); }
    vfs_close(f);
    return 0;
}

int config_user_active(void) { return console_uid; }

/* One line per signed-in account: its sessions, whether it holds the
 * console, and what it overrides.  Printed by `config` so "whose settings is
 * this machine showing" has an answer on the machine. */
void config_layers_dump(void) {
    uint32_t fl = spin_lock_irqsave(&cfg_lock);
    int uids[CFG_MAX_LAYERS], refs[CFG_MAX_LAYERS], cnt[CFG_MAX_LAYERS], n = 0;
    for (int i = 0; i < CFG_MAX_LAYERS; i++) {
        if (layers[i].refs <= 0) continue;
        int c = 0;
        for (struct entry* e = layers[i].head; e; e = e->next) c++;
        uids[n] = layers[i].uid; refs[n] = layers[i].refs; cnt[n] = c; n++;
    }
    int cu = console_uid;
    spin_unlock_irqrestore(&cfg_lock, fl);
    kprintf("config layers: machine%s\n", cu < 0 ? " (console)" : "");
    for (int i = 0; i < n; i++)
        kprintf("  uid %d: %d session(s), %d override(s)%s\n", uids[i], refs[i], cnt[i],
                uids[i] == cu ? " (console)" : "");
}
