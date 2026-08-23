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

struct entry {
    char* key;
    char* value;
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
    /* Notify only on a REAL change.  Re-applying the same value happens all
     * the time (a config file overlaid onto identical defaults, a panel
     * re-writing what is already there), and a subsystem told to re-read on
     * every no-op change would rebuild its state for nothing — at boot, that
     * is the whole defaults table. */
    const char* old = config_get(key, (const char*)0);
    int same = old && streq(old, value);
    int rc = config_set(key, value);
    if (rc == 0 && !same) {
        /* LOG the decision.  A settings change is a change to how the machine
         * behaves, and until now the only trace of one was whatever the
         * subsystem chose to print — so a panel that applied a value and a
         * panel that silently did nothing produced the same (empty) log.  It
         * also makes the Control Panel testable without a screen. */
        klog(KLOG_INFO, "config", "%s = %s (was %s)\n", key, value,
             old ? old : "unset");
        config_notify(key, value);
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
        vfs_write(f, e->key,  strlen_(e->key));
        vfs_write(f, " = ",   3);
        vfs_write(f, e->value, strlen_(e->value));
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
