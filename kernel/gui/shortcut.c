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
#include "vc.h"          /* vc_kbd_push_to — hand the command to its window */
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

/* Where the `.lnk` files live.  Starts as the RAM directory and is redirected
 * onto the persistent volume by shortcut_attach_persistent() — see the header
 * for why this is not a compile-time constant (it was, and shortcuts silently
 * did not survive a reboot). */
static char sc_dir[64] = SHORTCUT_DIR;

const char* shortcut_dir(void) { return sc_dir; }

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

int shortcut_attach_persistent(const char* mount) {
    if (!mount || !*mount) return -1;

    char dir[64];
    copy_(dir, sizeof dir, mount);
    int p = len_(dir);
    while (p > 0 && dir[p - 1] == '/') dir[--p] = '\0';
    const char* leaf = "/desktop";
    for (int i = 0; leaf[i] && p < (int)sizeof dir - 1; i++) dir[p++] = leaf[i];
    dir[p] = '\0';

    /* Creating the directory IS the write test — it is a write to the volume,
     * and it is the same argument §M63 stage 0 made about creating the config
     * file.  When it already exists a previous boot created it, which is
     * evidence of the same thing.  What we must not do is assume: a path we
     * merely HOPE is writable turns every later `shortcut add` into a silent
     * failure, which is precisely the bug being fixed here. */
    vfs_mkdir(dir);
    struct file* d = vfs_open(dir, VFS_RDONLY);
    if (!d) return -1;                  /* no directory there — keep the RAM one */
    vfs_close(d);

    copy_(sc_dir, sizeof sc_dir, dir);
    return shortcut_reload() >= 0 ? 0 : -1;
}

int shortcut_reload(void) {
    list_n = 0;

    struct file* d = vfs_open(shortcut_dir(), VFS_RDONLY);
    if (!d) {
        /* No /desktop yet — create it so `shortcut add` and the file manager
         * have somewhere to write.  Missing is not an error: a fresh system
         * has no shortcuts and that is the intended state (§M64 ships empty,
         * deliberately — see the header on auto-population). */
        vfs_mkdir(shortcut_dir());
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
                 SHORTCUT_MAX, shortcut_dir());
            break;
        }
        int n = len_(de.name);
        if (n < 5 || !streq_(de.name + n - 4, ".lnk")) continue;

        char path[80];
        copy_(path, sizeof path, shortcut_dir());
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

/* §M64 tail — the slot the user dragged this icon to, if any.  -1/-1 is the
 * "never placed" state a fresh `.lnk` carries (shortcut_add writes it), and it
 * is what keeps a new shortcut landing in a tidy flow position instead of on
 * top of whatever occupies cell (0,0). */
static int m_pos(void* ctx, int i, int* col, int* row) {
    (void)ctx;
    if (i < 0 || i >= list_n) return -1;
    if (list[i].x < 0 || list[i].y < 0) return -1;
    *col = list[i].x; *row = list[i].y;
    return 0;
}

static const struct item_model model = {
    .count = m_count, .get = m_get, .activate = m_activate, .ctx = NULL,
    .pos = m_pos,
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
        /* A COMMAND LINE IN ITS OWN TERMINAL.  The window is opened and the
         * command is pushed into ITS console as if typed — deliberately, so
         * there is no second "execute this" path in the shell to drift from
         * the one people use.  The shell parses it, reports its errors and
         * leaves its output on screen exactly as an interactive command does.
         *
         * `store:` maps here too: running a package IS a command line, and
         * having the spelling resolve to the same mechanism is the point of
         * having reserved it. */
        const char* cmd = t + (starts_(t, "run:") ? 4 : 6);
        while (*cmd == ' ') cmd++;
        if (!*cmd) {
            kprintf("shortcut '%s': empty command\n", list[index].name);
            return;
        }

        char title[32];
        copy_(title, sizeof title, list[index].name);
        struct gui_window* w = gui_window_create(title, 160, 120, 620, 380);
        if (!w) {
            klog(KLOG_WARN, "gui", "shortcut: no window for '%s'\n", t);
            return;
        }
        struct vc* v = gui_window_console(w);
        if (!v) {
            klog(KLOG_WARN, "gui", "shortcut: window has no console\n");
            return;
        }
        /* The shell prints its prompt before it reads, so the characters can
         * go in immediately — they wait in the console's ring until it does. */
        const char* pre = starts_(t, "store:") ? "pkgrun " : "";
        for (int i = 0; pre[i]; i++)  vc_kbd_push_to(v, pre[i]);
        for (int i = 0; cmd[i]; i++)  vc_kbd_push_to(v, cmd[i]);
        vc_kbd_push_to(v, '\n');
        klog(KLOG_INFO, "gui", "shortcut '%s': running '%s%s'\n",
             list[index].name, pre, cmd);
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

    vfs_mkdir(shortcut_dir());                 /* no-op when it exists */

    struct shortcut sc;
    copy_(sc.name, sizeof sc.name, name);
    copy_(sc.target, sizeof sc.target, target);
    sc.icon = icon_by_name(icon);
    sc.x = sc.y = -1;                        /* -1 = let the layout place it */

    copy_(sc.file, sizeof sc.file, shortcut_dir());
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

int shortcut_set_pos_live(int index, int x, int y) {
    if (index < 0 || index >= list_n) return -1;
    list[index].x = x;
    list[index].y = y;
    return 0;
}

void shortcut_pos_of(int index, int* x, int* y) {
    if (index < 0 || index >= list_n) { *x = *y = -1; return; }
    *x = list[index].x;
    *y = list[index].y;
}

/* Which PLACED shortcut holds slot (x,y)?  -1 = none. */
static int placed_at(int x, int y, int except) {
    for (int i = 0; i < list_n; i++)
        if (i != except && list[i].x == x && list[i].y == y) return i;
    return -1;
}

int shortcut_set_pos(int index, int x, int y) {
    if (index < 0 || index >= list_n) return -1;

    /* A DROP ONTO AN OCCUPIED SLOT SWAPS THE TWO.  The alternatives are worse:
     * stacking hides one shortcut behind another (it looks deleted, and the
     * hit test can only return one of them), and refusing the drop makes the
     * icon spring back for a reason nothing on screen explains.  A swap is
     * deterministic, needs no search, and cannot fail.
     *
     * If the slot merely LOOKS occupied — an unplaced icon that the flow
     * layout happened to put there — there is nothing to swap with, and the
     * layout moves it along on the next draw, because g_place() skips slots a
     * placed item has claimed. */
    int other = (x >= 0 && y >= 0) ? placed_at(x, y, index) : -1;
    int ox = list[index].x, oy = list[index].y;

    list[index].x = x;
    list[index].y = y;
    int rc = write_lnk(&list[index]);

    if (other >= 0) {
        list[other].x = ox;              /* may be -1/-1: back to flow order */
        list[other].y = oy;
        if (write_lnk(&list[other]) != 0) rc = -1;
    }
    return rc;
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
        kprintf("desktop shortcuts (%d) in %s:\n", list_n, shortcut_dir());
        for (int i = 0; i < list_n; i++)
            kprintf("  %s  ->  %s  [%s]\n", list[i].name, list[i].target,
                    icon_name(list[i].icon));
        if (!list_n)
            kprintf("  (none — `shortcut add <name> <target> [icon]`)\n");
        kprintf("  targets: app:<name> | file:<path> | run:<cmd> | store:<pkg>\n");
        kprintf("  also: shortcut open <name> | move <name> <col> <row> "
                "| check [view]\n");
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

    if (streq_(cmd, "move")) {
        /* §M64 tail — THE DROP, WITHOUT A MOUSE.  The pointer transport can
         * only be exercised by driving the pointer, but everything the drop
         * DECIDES — the slot, the swap with whatever was there, the rewrite of
         * both files — is reachable from here, so it can be regression-tested
         * on a machine with no display at all (the rule §M60 paid for: a
         * feature with no headless path cannot be checked here). */
        char name[48], cs[12], rs[12];
        rest = word(rest, name, sizeof name);
        rest = word(rest, cs, sizeof cs);
        word(rest, rs, sizeof rs);
        if (!name[0] || !cs[0] || !rs[0]) {
            kprintf("shortcut: move <name> <col> <row>   (-1 -1 = unplace)\n");
            return;
        }
        shortcut_reload();
        for (int i = 0; i < list_n; i++) {
            if (!streq_(list[i].name, name)) continue;
            int c = num_(cs), r = num_(rs);
            int oc = list[i].x, orow = list[i].y;
            int other = (c >= 0 && r >= 0) ? placed_at(c, r, i) : -1;
            if (shortcut_set_pos(i, c, r) != 0) {
                kprintf("shortcut: could not write %s\n", name);
                return;
            }
            kprintf("shortcut: %s (%d,%d) -> (%d,%d)\n", name, oc, orow, c, r);
            if (other >= 0)
                kprintf("  swapped with %s, now at (%d,%d)\n",
                        list[other].name, list[other].x, list[other].y);
            gui_desktop_icons_changed();
            return;
        }
        kprintf("shortcut: no such shortcut '%s'\n", name);
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

        /* §M64 tail — WHERE each item actually landed, and whether the view's
         * own hit test agrees.  A placement bug and a hit-test bug produce the
         * same checksum, and neither is visible in a screenshot: this is the
         * line that separates "the icon is drawn there" from "clicking there
         * finds that icon". */
        kprintf("  arrangeable = %s\n", v->slot_at ? "yes" : "no (fixed order)");
        for (int i = 0; i < list_n; i++) {
            int ox, oy, ow, oh;
            if (!v->rect || v->rect(i, w, h, shortcut_model(), 0,
                                    &ox, &oy, &ow, &oh) != 0) {
                kprintf("    %s slot=(%d,%d) off-field\n",
                        list[i].name, list[i].x, list[i].y);
                continue;
            }
            int back = v->hit(ox + ow / 2, oy + oh / 2, w, h, shortcut_model(), 0);
            kprintf("    %s slot=(%d,%d) px=(%d,%d) hit=%d%s\n",
                    list[i].name, list[i].x, list[i].y, ox, oy, back,
                    back == i ? "" : "  <-- MISMATCH");
        }
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

    kprintf("shortcut: list | add <name> <target> [icon] | rm <name> "
            "| open <name> | move <name> <col> <row> | check [view]\n");
}
