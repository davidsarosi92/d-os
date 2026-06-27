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
    { "keyboard.layout",  "us"       },     /* M16 — consumed by keymap_init */
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

int config_save(void) {
    struct file* f = vfs_open(CONF_PATH, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
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
    config_set(key, val);
}

int config_load(void) {
    struct file* f = vfs_open(CONF_PATH, VFS_RDONLY);
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

void config_init(void) {
    /* Plant defaults first.  config_set replaces existing entries, so
     * the subsequent file load can override any of these. */
    for (const struct config_default* d = builtin_defaults; d->key; d++) {
        config_set(d->key, d->value);
    }
    if (config_load() == 0) {
        kprintf("config: loaded %s\n", CONF_PATH);
    } else {
        kprintf("config: %s missing — using defaults\n", CONF_PATH);
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
