/* =============================================================================
 * shortcut.c — desktop shortcuts (§M64).  See shortcut.h for the file format
 * and for why a shortcut is a file rather than a config key.
 *
 * Structure:
 *   - the in-memory table (bounded, reloaded from /desktop on demand)
 *   - the .lnk reader/writer
 *   - the item_model the desktop's item_view draws
 *   - one resolver, `shortcut_launch`
 *   - the `shortcut` shell command (here, not in a shell — §M24's rule)
 *
 * The table is STATIC and bounded for the same reason every other table in
 * this kernel is: it is walked from the compositor while the mouse IRQ may be
 * changing the selection, and a growable array would need a lifetime rule for
 * a feature whose whole point is that it is simple.  Sixty-four icons is more
 * than any desktop this system will host, and the limit is reported rather
 * than silently applied.
 * ============================================================================= */

#include "shortcut.h"
#include "itemview.h"
#include "gfx.h"
#include "icons.h"
#include "gui.h"
#include "gui_app.h"
#include "vfs.h"
#include "printf.h"
#include "klog.h"
#include "task.h"
#include "shell_provider.h"
#include <stddef.h>

struct shortcut {
    char name[48];
    char target[128];
    int  icon;
    int  x, y;                  /* grid slot, not pixels — see below */
    char file[80];              /* the .lnk path, for rewrites */
};

static struct shortcut list[SHORTCUT_MAX];
static int list_n = 0;

/* ------------------------------------------------------------------- */
/* Small string helpers (no libc).                                      */
/* ------------------------------------------------------------------- */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int starts_(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}
static void copy_(char* dst, int cap, const char* src) {
    int i = 0;
    if (!src) { dst[0] = '\0'; return; }
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}
static int len_(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static const char* trim_(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static void rtrim_(char* s) {
    int n = len_(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}
static int num_(const char* s) {
    int v = 0, neg = 0;
    s = trim_(s);
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static void num_str(char* out, int cap, int v) {
    char tmp[16]; int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg) tmp[n++] = '-';
    int i = 0;
    while (n-- > 0 && i < cap - 1) out[i++] = tmp[n];
    out[i] = '\0';
}

/* ------------------------------------------------------------------- */
/* .lnk read / write.                                                   */
/* ------------------------------------------------------------------- */

static int read_lnk(const char* path, struct shortcut* out) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;

    char buf[512];
    ssize_t got = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (got <= 0) return -1;
    buf[got] = '\0';

    out->name[0] = out->target[0] = '\0';
    out->icon = ICON_APP;
    out->x = out->y = -1;
    copy_(out->file, sizeof out->file, path);

    char* line = buf;
    for (ssize_t i = 0; i <= got; i++) {
        if (buf[i] != '\n' && buf[i] != '\0') continue;
        buf[i] = '\0';
        char* eq = line;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            char key[24];
            copy_(key, sizeof key, trim_(line));
            rtrim_(key);
            char val[160];
            copy_(val, sizeof val, trim_(eq + 1));
            rtrim_(val);
            if      (streq_(key, "name"))   copy_(out->name, sizeof out->name, val);
            else if (streq_(key, "target")) copy_(out->target, sizeof out->target, val);
            else if (streq_(key, "icon"))   out->icon = icon_by_name(val);
            else if (streq_(key, "x"))      out->x = num_(val);
            else if (streq_(key, "y"))      out->y = num_(val);
        }
        line = buf + i + 1;
    }

    /* A shortcut with no target is not a shortcut.  A shortcut with no name
     * takes the file's — a hand-written .lnk should still work. */
    if (!out->target[0]) return -1;
    if (!out->name[0]) {
        const char* base = path;
        for (const char* p = path; *p; p++) if (*p == '/') base = p + 1;
        copy_(out->name, sizeof out->name, base);
        int n = len_(out->name);                    /* strip ".lnk" */
        if (n > 4 && streq_(out->name + n - 4, ".lnk")) out->name[n - 4] = '\0';
    }
    return 0;
}

static int write_lnk(const struct shortcut* sc) {
    vfs_unlink(sc->file);
    struct file* f = vfs_open(sc->file, VFS_WRONLY | VFS_CREATE);
    if (!f) return -1;
    char line[256];
    int n = 0;
    #define PUT(str) do { const char* _s = (str); \
                          for (int _i = 0; _s[_i] && n < (int)sizeof line - 1; _i++) \
                              line[n++] = _s[_i]; } while (0)
    PUT("name = ");   PUT(sc->name);   PUT("\n");
    PUT("target = "); PUT(sc->target); PUT("\n");
    PUT("icon = ");   PUT(icon_name(sc->icon)); PUT("\n");
    char nb[16];
    num_str(nb, sizeof nb, sc->x); PUT("x = "); PUT(nb); PUT("\n");
    num_str(nb, sizeof nb, sc->y); PUT("y = "); PUT(nb); PUT("\n");
    #undef PUT
    vfs_write(f, line, (size_t)n);
    vfs_close(f);
    return 0;
}

/* ------------------------------------------------------------------- */
/* Loading.                                                             */
/* ------------------------------------------------------------------- */

int shortcut_reload(void) {
    list_n = 0;

    struct file* d = vfs_open(SHORTCUT_DIR, VFS_RDONLY);
    if (!d) {
        /* No /desktop yet — create it so `shortcut add` and the file manager
         * have somewhere to write.  Missing is not an error: a fresh system
         * has no shortcuts and that is the intended state (§M64 ships empty,
         * deliberately — see the header on auto-population). */
        vfs_mkdir(SHORTCUT_DIR);
        return 0;
    }

    struct dirent de;
    /* vfs_readdir returns >0 per entry, 0 at end, <0 on error — NOT 0 per
     * entry.  The first version of this loop tested `== 0` and therefore read
     * nothing, silently: `shortcut add` reported success (the file was
     * written) and `shortcut list` showed an empty desktop.  A convention
     * worth copying from an existing caller (cmd_ls) rather than assuming. */
    while (vfs_readdir(d, &de) > 0) {
        if (list_n >= SHORTCUT_MAX) {
            klog(KLOG_WARN, "gui", "shortcut: more than %d in %s — ignoring the rest\n",
                 SHORTCUT_MAX, SHORTCUT_DIR);
            break;
        }
        int n = len_(de.name);
        if (n < 5 || !streq_(de.name + n - 4, ".lnk")) continue;

        char path[80];
        copy_(path, sizeof path, SHORTCUT_DIR);
        int p = len_(path);
        if (p < (int)sizeof path - 1) path[p++] = '/';
        for (int i = 0; de.name[i] && p < (int)sizeof path - 1; i++) path[p++] = de.name[i];
        path[p] = '\0';

        if (read_lnk(path, &list[list_n]) == 0) list_n++;
        else klog(KLOG_WARN, "gui", "shortcut: %s is malformed — skipped\n", path);
    }
    vfs_close(d);
    return list_n;
}

/* ------------------------------------------------------------------- */
/* The item model.                                                      */
/* ------------------------------------------------------------------- */

static int m_count(void* ctx) { (void)ctx; return list_n; }

static int m_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    if (i < 0 || i >= list_n) return -1;
    out->label = list[i].name;
    out->sub   = list[i].target;
    out->icon  = list[i].icon;
    out->dim   = 0;
    return 0;
}

static void m_activate(void* ctx, int i) { (void)ctx; shortcut_launch(i); }

static const struct item_model model = {
    .count = m_count, .get = m_get, .activate = m_activate, .ctx = NULL,
};

const struct item_model* shortcut_model(void) { return &model; }

/* ------------------------------------------------------------------- */
/* The one resolver.                                                    */
/* ------------------------------------------------------------------- */

/* Match an app name TOLERANTLY: case-insensitive, and '-'/'_' count as a
 * space.  A shortcut target doubles as something a person types on a command
 * line and as text in a file, where "Task Manager" is awkward — so
 * `app:Task-Manager` has to find the app registered as "Task Manager".  The
 * rule lives here rather than in gui_app_find(), whose contract (exact name,
 * case-insensitive) other callers rely on. */
static char fold_(char c) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c == '-' || c == '_') c = ' ';
    return c;
}
static const struct gui_app_def* app_find_loose(const char* want) {
    for (int i = 0; i < gui_app_count(); i++) {
        const struct gui_app_def* a = gui_app_at(i);
        if (!a || !a->name) continue;
        const char* p = a->name;
        const char* q = want;
        while (*p && *q && fold_(*p) == fold_(*q)) { p++; q++; }
        if (!*p && !*q) return a;
    }
    return NULL;
}

void shortcut_launch(int index) {
    if (index < 0 || index >= list_n) return;
    const char* t = list[index].target;

    if (starts_(t, "app:")) {
        const struct gui_app_def* app = app_find_loose(t + 4);
        if (!app) {
            klog(KLOG_WARN, "gui", "shortcut '%s': no app named '%s'\n",
                 list[index].name, t + 4);
            return;
        }
        /* M22.7 — the compositor spawns the app-host task.  Calling
         * app->launch() here would run the app on the caller's task with no
         * event loop, which is the bug §M22.7 exists to prevent. */
        gui_queue_launch(app);
        return;
    }

    if (starts_(t, "file:")) {
        const struct gui_app_def* app = gui_app_for_path(t + 5);
        if (app && app->open_path) {
            /* Association opens run on the caller (an ordinary task): the app
             * creates its own window, exactly as the file manager does. */
            app->open_path(t + 5);
            return;
        }
        klog(KLOG_WARN, "gui", "shortcut '%s': nothing claims '%s'\n",
             list[index].name, t + 5);
        return;
    }

    if (starts_(t, "run:") || starts_(t, "store:")) {
        /* Reserved spelling, honest behaviour: there is no "run a command line
         * in a fresh window" primitive yet — that wants the terminal window to
         * accept an initial command, which is a shell change, not a shortcut
         * change.  Say so instead of silently doing nothing. */
        kprintf("shortcut '%s': target kind '%s' is not implemented yet\n",
                list[index].name, t);
        klog(KLOG_WARN, "gui", "shortcut: unimplemented target '%s'\n", t);
        return;
    }

    klog(KLOG_WARN, "gui", "shortcut '%s': unknown target '%s'\n",
         list[index].name, t);
}

/* ------------------------------------------------------------------- */
/* Mutation.                                                            */
/* ------------------------------------------------------------------- */

int shortcut_add(const char* name, const char* target, const char* icon) {
    if (!name || !*name || !target || !*target) return -1;
    if (list_n >= SHORTCUT_MAX) return -2;

    vfs_mkdir(SHORTCUT_DIR);                 /* no-op when it exists */

    struct shortcut sc;
    copy_(sc.name, sizeof sc.name, name);
    copy_(sc.target, sizeof sc.target, target);
    sc.icon = icon_by_name(icon);
    sc.x = sc.y = -1;                        /* -1 = let the layout place it */

    copy_(sc.file, sizeof sc.file, SHORTCUT_DIR);
    int p = len_(sc.file);
    sc.file[p++] = '/';
    for (int i = 0; name[i] && p < (int)sizeof sc.file - 5; i++) {
        char c = name[i];
        /* A shortcut name is a filename here, so the characters a path cannot
         * carry are folded rather than rejected: refusing "Task Manager"
         * because of the space would be a surprising rule for a label. */
        sc.file[p++] = (c == '/' || c == ' ') ? '_' : c;
    }
    sc.file[p] = '\0';
    const char* ext = ".lnk";
    for (int i = 0; ext[i] && p < (int)sizeof sc.file - 1; i++) sc.file[p++] = ext[i];
    sc.file[p] = '\0';

    if (write_lnk(&sc) != 0) return -3;
    shortcut_reload();
    gui_desktop_icons_changed();
    return 0;
}

int shortcut_remove(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < list_n; i++) {
        if (streq_(list[i].name, name)) {
            vfs_unlink(list[i].file);
            shortcut_reload();
            gui_desktop_icons_changed();
            return 0;
        }
    }
    return -1;
}

int shortcut_set_pos(int index, int x, int y) {
    if (index < 0 || index >= list_n) return -1;
    list[index].x = x;
    list[index].y = y;
    return write_lnk(&list[index]);
}

/* ------------------------------------------------------------------- */
/* Shell command.                                                       */
/* ------------------------------------------------------------------- */

static const char* word(const char* s, char* out, int cap) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && n < cap - 1) out[n++] = *s++;
    out[n] = '\0';
    while (*s == ' ') s++;
    return s;
}

void shortcut_cmd(const char* args) {
    char cmd[16];
    const char* rest = word(args ? args : "", cmd, sizeof cmd);

    if (!cmd[0] || streq_(cmd, "list")) {
        shortcut_reload();
        kprintf("desktop shortcuts (%d) in %s:\n", list_n, SHORTCUT_DIR);
        for (int i = 0; i < list_n; i++)
            kprintf("  %s  ->  %s  [%s]\n", list[i].name, list[i].target,
                    icon_name(list[i].icon));
        if (!list_n)
            kprintf("  (none — `shortcut add <name> <target> [icon]`)\n");
        kprintf("  targets: app:<name> | file:<path> | run:<cmd> | store:<pkg>\n");
        kprintf("  also: shortcut open <name> | shortcut check [view]\n");
        return;
    }

    if (streq_(cmd, "add")) {
        char name[48], target[128], icon[24];
        rest = word(rest, name, sizeof name);
        rest = word(rest, target, sizeof target);
        word(rest, icon, sizeof icon);
        if (!name[0] || !target[0]) {
            kprintf("shortcut: add <name> <target> [icon]\n");
            return;
        }
        int rc = shortcut_add(name, target, icon[0] ? icon : NULL);
        if (rc == 0)       kprintf("shortcut: added %s -> %s\n", name, target);
        else if (rc == -2) kprintf("shortcut: desktop is full (%d)\n", SHORTCUT_MAX);
        else               kprintf("shortcut: could not write %s\n", name);
        return;
    }

    if (streq_(cmd, "rm") || streq_(cmd, "remove")) {
        char name[48];
        word(rest, name, sizeof name);
        if (!name[0]) { kprintf("shortcut: rm <name>\n"); return; }
        if (shortcut_remove(name) == 0) kprintf("shortcut: removed %s\n", name);
        else                            kprintf("shortcut: no such shortcut '%s'\n", name);
        return;
    }

    if (streq_(cmd, "check")) {
        /* Render the icon field into an OFF-SCREEN surface and print a
         * checksum — the same trick §M60 needed, for the same reason: the
         * aarch64 test harness has no display device, so a screendump there is
         * impossible rather than merely awkward.  It also makes the LAYOUT
         * falsifiable: grid and list must produce different numbers, and the
         * same view must produce the same number on every arch. */
        char vname[16];
        word(rest, vname, sizeof vname);
        const struct item_view* v = item_view_by_name(vname[0] ? vname : NULL);
        if (!v) { kprintf("shortcut check: no item views linked\n"); return; }
        shortcut_reload();

        struct gfx_surface tmp;
        int w = 384, h = 288;
        if (gfx_surface_init(&tmp, w, h) != 0) {
            kprintf("shortcut check: cannot allocate surface\n");
            return;
        }
        gfx_fill(&tmp, 0, 0, w, h, 0xFF101820u);
        v->draw(&tmp, 0, 0, w, h, shortcut_model(), 0, 0);
        uint32_t sum = 0;
        for (int y = 0; y < h; y++) {
            const uint32_t* row = tmp.px + (size_t)y * (size_t)tmp.stride;
            for (int x = 0; x < w; x++) sum = sum * 31u + row[x];
        }
        kprintf("shortcut check: view=%s items=%d %dx%d sum=%x\n",
                v->name, list_n, w, h, sum);
        /* Hit-test the first cell's centre: a layout that draws correctly and
         * hit-tests wrongly is the bug this catches, and it is invisible in a
         * screenshot. */
        kprintf("  hit(centre of cell 0) = %d, page = %d\n",
                v->hit(24, 24, w, h, shortcut_model(), 0), v->page(w, h));
        gfx_surface_free(&tmp);
        return;
    }

    if (streq_(cmd, "open")) {
        char name[48];
        word(rest, name, sizeof name);
        shortcut_reload();
        for (int i = 0; i < list_n; i++) {
            if (streq_(list[i].name, name)) { shortcut_launch(i); return; }
        }
        kprintf("shortcut: no such shortcut '%s'\n", name);
        return;
    }

    kprintf("shortcut: list | add <name> <target> [icon] | rm <name> | open <name>\n");
}
