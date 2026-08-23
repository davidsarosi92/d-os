/* =============================================================================
 * fileman.c — file manager 2.0 (M22.1, reworked in M22.5 stage 6).
 *
 *   ┌ File Manager ────────────────────────────── _ □ x ┐
 *   │ [/mnt/dir_______________________] (editable path) │
 *   │ [Up][MkDir][Touch][Ren][Copy][Del][View]           │
 *   │ NAME                          SIZE   (header)      │
 *   │ ┌────────────────────────────────────────────────┐ │
 *   │ │ subdir/                      <DIR>  (listview)  │ │
 *   │ │ file.txt                      1234              │ │
 *   │ └────────────────────────────────────────────────┘ │
 *   │ [name input___________________]                    │
 *   │ status line                                        │
 *   └────────────────────────────────────────────────────┘
 *
 * M22.5 additions over the M22.1 original:
 *   - editable path bar (Enter navigates; bad paths are refused),
 *   - size column + directories-first, name-sorted listing,
 *   - keyboard navigation for free (listview grew arrows/PgUp/PgDn/
 *     Home/End + Enter-activates in the M22.5 widget work),
 *   - Ren (vfs_rename, same-directory) and Copy (vfs_copy) buttons
 *     driven by the name input,
 *   - Del deletes files immediately; a NON-EMPTY directory arms a
 *     two-step confirm — the second Del within 8 s runs
 *     vfs_unlink_recursive (mistake-proof by default, still one
 *     hand-motion to nuke a tree),
 *   - double-click / Enter on a file consults the GUI_APP_ASSOC
 *     registry (gui_app_for_path): .txt/.md/... open in the Editor,
 *     .bas lands in the BASIC window; anything unclaimed falls back
 *     to the read-only viewer.
 *
 * Everything here runs on the compositor task (widget callbacks), so
 * calling the VFS directly is fine.  See widget.h for the model.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "ui.h"        /* §M65 — the menu bar */
#include "itemview.h"  /* §M65 — the list is a MODEL + a table view now */
#include "config.h"
#include "vfs.h"
#include "timer.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

#define FM_PATH_MAX 224
#define FM_NAME_COL 30                  /* listview name column width */

struct fileman {
    struct gui_window*  win;
    char                path[FM_PATH_MAX];
    struct w_textinput* path_in;
    struct w_label*     status;
    struct w_itemview*  iv;
    struct item_model   model;          /* per-window: ctx points at this fm  */
    struct w_textinput* name_in;
    /* §M65 — THE ENTRIES ARE THE MODEL NOW.  They used to be pre-formatted
     * strings in the listview, with the name padded to a column and the size
     * appended — and a SEPARATE array of raw names, because path arithmetic
     * must not see the display's padding.  Two representations of one
     * directory, kept in step by hand.  One array of records, and the table
     * view asks it for a cell when it needs one. */
    char     names[WLIST_MAX_ITEMS][VFS_NAME_MAX + 1];
    uint64_t sizes[WLIST_MAX_ITEMS];
    uint8_t  types[WLIST_MAX_ITEMS];
    int      count;
    /* Two-step recursive-delete confirm. */
    int      del_armed_idx;             /* -1 = not armed */
    uint64_t del_armed_ms;
};

static struct gui_window* fm_win = NULL;         /* singleton */

/* -------------------------------------------------------------------------- */
/* Small helpers.                                                              */
/* -------------------------------------------------------------------------- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void path_join(char* dst, int cap, const char* base, const char* name) {
    int p = 0;
    for (; base[p] && p < cap - 1; p++) dst[p] = base[p];
    if (p > 1 || (p == 1 && dst[0] != '/')) {    /* base != "/" → add sep */
        if (p < cap - 1) dst[p++] = '/';
    }
    for (int i = 0; name[i] && p < cap - 1; i++) dst[p++] = name[i];
    dst[p] = 0;
}

static void path_parent(char* path) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last <= 0) { path[0] = '/'; path[1] = 0; return; }
    path[last] = 0;
}

/* -------------------------------------------------------------------------- */
/* Directory listing: read, sort (dirs first, then name), column-format.       */
/* -------------------------------------------------------------------------- */

static int put_str_at(char* d, int p, int cap, const char* s) {
    for (; s && *s && p < cap - 1; s++) d[p++] = *s;
    return p;
}

/* Human-ish size: bytes up to 6 digits, then K / M. */
static int put_size(char* d, int p, int cap, uint64_t size, int is_dir) {
    if (is_dir) return put_str_at(d, p, cap, "<DIR>");
    uint32_t v = (uint32_t)size;
    char suffix = 0;
    if (size >= 10u * 1024 * 1024) { v = (uint32_t)(size >> 20); suffix = 'M'; }
    else if (size >= 1000000u)     { v = (uint32_t)(size >> 10); suffix = 'K'; }
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n && p < cap - 1) d[p++] = tmp[--n];
    if (suffix && p < cap - 1) d[p++] = suffix;
    return p;
}


/* dirs first, then case-insensitive name order. */
static int entry_before(struct fileman* fm, int i, int j) {
    int di = fm->types[i] == (uint8_t)INODE_DIR;
    int dj = fm->types[j] == (uint8_t)INODE_DIR;
    if (di != dj) return di;                     /* directory wins */
    const char *a = fm->names[i], *b = fm->names[j];
    while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) <= lower(*b);
}

static void swap_entries(struct fileman* fm, int i, int j) {
    for (int k = 0; k <= VFS_NAME_MAX; k++) {
        char t = fm->names[i][k];
        fm->names[i][k] = fm->names[j][k];
        fm->names[j][k] = t;
    }
    uint64_t sz = fm->sizes[i]; fm->sizes[i] = fm->sizes[j]; fm->sizes[j] = sz;
    uint8_t  ty = fm->types[i]; fm->types[i] = fm->types[j]; fm->types[j] = ty;
}

/* ---- the model the table view reads ------------------------------------- */

static int fm_m_count(void* ctx) { return ((struct fileman*)ctx)->count; }

static int fm_m_get(void* ctx, int i, struct item_entry* out) {
    struct fileman* fm = (struct fileman*)ctx;
    if (i < 0 || i >= fm->count) return -1;
    out->label = fm->names[i];
    out->sub   = NULL;
    out->icon  = fm->types[i] == (uint8_t)INODE_DIR ? ICON_FOLDER : ICON_DOC;
    out->dim   = 0;
    return 0;
}

static void fm_activate_idx(struct fileman* fm, int idx);
static void fm_m_activate(void* ctx, int i) { fm_activate_idx((struct fileman*)ctx, i); }

static int fm_m_columns(void* ctx)  { (void)ctx; return 2; }
static const char* fm_m_title(void* ctx, int c) {
    (void)ctx;
    return c == 0 ? "NAME" : "SIZE";
}
static int fm_m_weight(void* ctx, int c) { (void)ctx; return c == 0 ? 3 : 1; }

static int fm_m_cell(void* ctx, int i, int c, char* out, int cap) {
    struct fileman* fm = (struct fileman*)ctx;
    if (i < 0 || i >= fm->count || cap <= 0) { if (cap) out[0] = 0; return -1; }
    int is_dir = fm->types[i] == (uint8_t)INODE_DIR;
    if (c == 0) {
        int p = 0;
        for (const char* n = fm->names[i]; *n && p < cap - 2; n++) out[p++] = *n;
        if (is_dir && p < cap - 1) out[p++] = '/';
        out[p] = 0;
        return 0;
    }
    /* The size column is the SAME formatter as before — it just no longer has
     * to pad the name to a fixed column, because the table owns the geometry. */
    int p = put_size(out, 0, cap, fm->sizes[i], is_dir);
    out[p] = 0;
    return 0;
}

static struct item_model fm_model = {
    .count = fm_m_count, .get = fm_m_get, .activate = fm_m_activate,
    .columns = fm_m_columns, .col_title = fm_m_title,
    .col_weight = fm_m_weight, .cell = fm_m_cell,
};

static void fm_refresh(struct fileman* fm) {
    w_textinput_set(fm->path_in, fm->path);
    fm->count = 0;
    if (fm->iv) { fm->iv->sel = -1; fm->iv->scroll = 0; }
    fm->del_armed_idx = -1;

    struct file* f = vfs_open(fm->path, VFS_RDONLY);
    if (!f) {
        w_label_set(fm->status, "cannot open directory");
        return;
    }
    struct dirent de;
    int total = 0;
    while (vfs_readdir(f, &de) > 0) {
        if (fm->count >= WLIST_MAX_ITEMS) break;
        int idx = fm->count;
        int i = 0;
        for (; de.name[i] && i < VFS_NAME_MAX; i++) fm->names[idx][i] = de.name[i];
        fm->names[idx][i] = 0;
        fm->sizes[idx] = de.size;
        fm->types[idx] = (uint8_t)de.type;
        fm->count++;
        total++;
    }
    vfs_close(f);

    /* Selection sort — n ≤ 96, and it swaps whole records in place. */
    for (int i = 0; i < fm->count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < fm->count; j++)
            if (!entry_before(fm, best, j)) best = j;
        if (best != i) swap_entries(fm, i, best);
    }

    char st[32] = "   entries";
    st[0] = (char)('0' + (total / 10) % 10);
    st[1] = (char)('0' + total % 10);
    if (total < 10) st[0] = ' ';
    w_label_set(fm->status, st);
}

/* -------------------------------------------------------------------------- */
/* Viewer window (read-only fallback for unassociated types).                  */
/* -------------------------------------------------------------------------- */

struct viewer { struct w_listview* lv; };

static void viewer_layout(struct gui_window* win) {
    struct viewer* v = (struct viewer*)gui_window_ctx(win);
    if (!v || !v->lv) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    v->lv->base.x = 6;  v->lv->base.y = 6;
    v->lv->base.w = cw - 12;
    v->lv->base.h = ch - 12;
}

static void viewer_open(const char* path, const char* name) {
    struct viewer* v = (struct viewer*)kcalloc(1, sizeof(*v));
    if (!v) return;

    char title[24] = "View: ";
    int p = 6;
    for (int i = 0; name[i] && p < (int)sizeof(title) - 1; i++) title[p++] = name[i];
    title[p] = 0;

    struct gui_window* win =
        gui_app_window_create(title, 260, 140, 520, 380, viewer_layout, v);
    if (!win) { kfree(v); return; }
    v->lv = w_listview_create(win, 6, 6, 508, 340, NULL);
    if (!v->lv) { gui_window_close(win); return; }

    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) {
        w_listview_add(v->lv, "(cannot open file)", 0);
    } else {
        char* buf = (char*)kmalloc(8192);
        if (buf) {
            ssize_t n = vfs_read(f, buf, 8191);
            if (n < 0) n = 0;
            buf[n] = 0;
            char line[WLIST_ITEM_LEN];
            int li = 0;
            for (ssize_t i = 0; i <= n; i++) {
                char c = buf[i];
                if (c == '\n' || c == 0 || li == WLIST_ITEM_LEN - 1) {
                    line[li] = 0;
                    if (li > 0 || c == '\n')
                        if (w_listview_add(v->lv, line, 0) < 0) break;
                    li = 0;
                    if (c == 0) break;
                    if (c != '\n') line[li++] = c;   /* overlong line: keep char */
                } else if (c != '\r') {
                    line[li++] = c;
                }
            }
            if (n == 8191) w_listview_add(v->lv, "... (truncated)", 0);
            kfree(buf);
        }
        vfs_close(f);
    }

    viewer_layout(win);
    gui_window_request_redraw(win);
}

/* -------------------------------------------------------------------------- */
/* Widget callbacks.                                                           */
/* -------------------------------------------------------------------------- */

static void fm_activate_idx(struct fileman* fm, int idx) {
    if (!fm || idx < 0 || idx >= fm->count) return;
    const char* name = fm->names[idx];

    if (fm->types[idx] == (uint8_t)INODE_DIR) {
        char np[FM_PATH_MAX];
        path_join(np, (int)sizeof(np), fm->path, name);
        int p = 0;
        for (; np[p]; p++) fm->path[p] = np[p];
        fm->path[p] = 0;
        fm_refresh(fm);
    } else {
        char fp[FM_PATH_MAX];
        path_join(fp, (int)sizeof(fp), fm->path, name);
        /* M22.5 — file-type association: extension → registered app. */
        const struct gui_app_def* app = gui_app_for_path(fp);
        if (app) app->open_path(fp);
        else     viewer_open(fp, name);
    }
}

static void fm_up(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    path_parent(fm->path);
    fm_refresh(fm);
}

/* Shared shape of MkDir / Touch: take the name from the input box,
 * join to the current dir, run `op`, report, refresh. */
static void fm_create_common(struct fileman* fm, int (*op)(const char*),
                             const char* okmsg, const char* failmsg) {
    if (fm->name_in->len == 0) {
        w_label_set(fm->status, "type a name below first");
        return;
    }
    char np[FM_PATH_MAX];
    path_join(np, (int)sizeof(np), fm->path, fm->name_in->buf);
    if (op(np) == 0) {
        w_label_set(fm->status, okmsg);
        w_textinput_set(fm->name_in, "");
        fm_refresh(fm);
    } else {
        w_label_set(fm->status, failmsg);
    }
}

static void fm_mkdir(struct w_button* b, void* ctx) {
    (void)b;
    fm_create_common((struct fileman*)ctx, vfs_mkdir,
                     "directory created", "mkdir failed (exists? read-only fs?)");
}

static void fm_touch(struct w_button* b, void* ctx) {
    (void)b;
    fm_create_common((struct fileman*)ctx, vfs_create,
                     "file created", "create failed (exists? read-only fs?)");
}

/* Del: files (and empty dirs) go immediately; a non-empty directory
 * arms a two-step confirm and the second press within 8 s deletes the
 * whole tree (vfs_unlink_recursive). */
static void fm_del(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0) { w_label_set(fm->status, "select an entry first"); return; }

    char np[FM_PATH_MAX];
    path_join(np, (int)sizeof(np), fm->path, fm->names[sel]);

    if (fm->del_armed_idx == sel &&
        timer_ticks_ms() - fm->del_armed_ms < 8000) {
        if (vfs_unlink_recursive(np) == 0) {
            w_label_set(fm->status, "tree deleted");
            fm_refresh(fm);
        } else {
            w_label_set(fm->status, "recursive delete failed");
            fm->del_armed_idx = -1;
        }
        return;
    }

    int r = vfs_unlink(np);
    if (r == 0) {
        w_label_set(fm->status, "deleted");
        fm_refresh(fm);
    } else if (r == -2) {
        fm->del_armed_idx = sel;
        fm->del_armed_ms  = timer_ticks_ms();
        w_label_set(fm->status, "dir not empty - Del again deletes the TREE");
    } else {
        w_label_set(fm->status, "delete failed (fs read-only?)");
    }
}

/* Ren: selected entry → name from the input box (same directory). */
static void fm_ren(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0)              { w_label_set(fm->status, "select an entry first"); return; }
    if (fm->name_in->len == 0){ w_label_set(fm->status, "type the new name below"); return; }

    char op[FM_PATH_MAX], np[FM_PATH_MAX];
    path_join(op, (int)sizeof(op), fm->path, fm->names[sel]);
    path_join(np, (int)sizeof(np), fm->path, fm->name_in->buf);

    int r = vfs_rename(op, np);
    if (r == 0)       { w_label_set(fm->status, "renamed");
                        w_textinput_set(fm->name_in, ""); fm_refresh(fm); }
    else if (r == -2) w_label_set(fm->status, "target name exists");
    else              w_label_set(fm->status, "rename failed (fs support?)");
}

/* Copy: selected FILE → name from the input box (same directory). */
static void fm_copy(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0)              { w_label_set(fm->status, "select a file first"); return; }
    if (fm->types[sel] == (uint8_t)INODE_DIR) {
        w_label_set(fm->status, "copy works on files (not dirs)");
        return;
    }
    if (fm->name_in->len == 0){ w_label_set(fm->status, "type the copy's name below"); return; }

    char sp[FM_PATH_MAX], dp[FM_PATH_MAX];
    path_join(sp, (int)sizeof(sp), fm->path, fm->names[sel]);
    path_join(dp, (int)sizeof(dp), fm->path, fm->name_in->buf);

    if (vfs_copy(sp, dp) == 0) {
        w_label_set(fm->status, "copied");
        w_textinput_set(fm->name_in, "");
        fm_refresh(fm);
    } else {
        w_label_set(fm->status, "copy failed (exists? read-only fs?)");
    }
}

static void fm_view(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0) { w_label_set(fm->status, "select a file first"); return; }
    if (fm->types[sel] == (uint8_t)INODE_DIR) { fm_activate_idx(fm, sel); return; }
    char fp[FM_PATH_MAX];
    path_join(fp, (int)sizeof(fp), fm->path, fm->names[sel]);
    viewer_open(fp, fm->names[sel]);             /* View = always raw view */
}

static void fm_name_submit(struct w_textinput* t, void* ctx) {
    (void)t;
    /* Enter in the name box = Touch (create file) — the common case. */
    fm_touch(NULL, ctx);
}

/* M22.5 — editable path bar: Enter navigates if the path is a
 * readable directory, otherwise the input snaps back. */
static void fm_path_submit(struct w_textinput* t, void* ctx) {
    struct fileman* fm = (struct fileman*)ctx;
    char np[FM_PATH_MAX];
    int p = 0;

    const char* in = t->buf;
    if (in[0] != '/') { w_label_set(fm->status, "absolute paths only");
                        w_textinput_set(fm->path_in, fm->path); return; }
    for (; in[p] && p < FM_PATH_MAX - 1; p++) np[p] = in[p];
    np[p] = 0;
    while (p > 1 && np[p - 1] == '/') np[--p] = 0;   /* strip trailing '/' */

    struct file* f = vfs_open(np, VFS_RDONLY);
    if (!f || !f->inode || f->inode->type != INODE_DIR) {
        if (f) vfs_close(f);
        w_label_set(fm->status, "not a directory");
        w_textinput_set(fm->path_in, fm->path);
        return;
    }
    vfs_close(f);
    for (p = 0; np[p]; p++) fm->path[p] = np[p];
    fm->path[p] = 0;
    fm_refresh(fm);
    gui_window_focus_widget(fm->win, &fm->iv->base);
}

/* -------------------------------------------------------------------------- */
/* Layout + lifetime.                                                          */
/* -------------------------------------------------------------------------- */

/* ===========================================================================
 * §M65 — THE MENU BAR.
 *
 * The file manager is where the extra commands were always going to run out of
 * button row: seven buttons already sit above the list, and every new operation
 * (Refresh, Select all, Properties…) makes that row worse.  A menu is where
 * commands that are not one-click-frequent belong.
 *
 * The menu is DECLARED, not built: an array of (menu, item, id) triples goes to
 * the toolkit, and the id comes back through the same event sink a button
 * click uses.  The handlers below are the ones the buttons already call — the
 * menu adds a second way in, not a second implementation.
 * ========================================================================= */
enum {
    FM_CMD_UP = 1, FM_CMD_MKDIR, FM_CMD_TOUCH, FM_CMD_REN, FM_CMD_COPY,
    FM_CMD_DEL, FM_CMD_VIEW, FM_CMD_REFRESH, FM_CMD_ROOT, FM_CMD_CLOSE,
};

static const struct ui_menu_def fm_menu[] = {
    { "File",   "New folder",  FM_CMD_MKDIR   },
    { "File",   "New file",    FM_CMD_TOUCH   },
    { "File",   "-",           0              },
    { "File",   "Rename",      FM_CMD_REN     },
    { "File",   "Copy",        FM_CMD_COPY    },
    { "File",   "Delete",      FM_CMD_DEL     },
    { "File",   "-",           0              },
    { "File",   "Close",       FM_CMD_CLOSE   },
    { "View",   "Open",        FM_CMD_VIEW    },
    { "View",   "Refresh",     FM_CMD_REFRESH },
    { "Go",     "Up",          FM_CMD_UP      },
    { "Go",     "Root",        FM_CMD_ROOT    },
};
#define FM_MENU_N ((int)(sizeof fm_menu / sizeof fm_menu[0]))
#define FM_MENU_ID   1
#define FM_MENU_H    24         /* the row the menu bar occupies */

static void fm_ui_event(struct gui_window* win, int id, int type, int value,
                        void* ctx) {
    struct fileman* fm = (struct fileman*)ctx;
    (void)win;
    if (id != FM_MENU_ID || type != UI_EV_CLICK) return;
    switch (value) {
    case FM_CMD_UP:      fm_up(NULL, fm);    break;
    case FM_CMD_MKDIR:   fm_mkdir(NULL, fm); break;
    case FM_CMD_TOUCH:   fm_touch(NULL, fm); break;
    case FM_CMD_REN:     fm_ren(NULL, fm);   break;
    case FM_CMD_COPY:    fm_copy(NULL, fm);  break;
    case FM_CMD_DEL:     fm_del(NULL, fm);   break;
    case FM_CMD_VIEW:    fm_view(NULL, fm);  break;
    case FM_CMD_REFRESH: fm_refresh(fm); gui_window_request_redraw(fm->win); break;
    case FM_CMD_ROOT:
        fm->path[0] = '/'; fm->path[1] = 0;
        fm_refresh(fm);
        gui_window_request_redraw(fm->win);
        break;
    case FM_CMD_CLOSE:   gui_window_close(fm->win); break;
    default: break;
    }
}

static void fm_layout(struct gui_window* win) {
    struct fileman* fm = (struct fileman*)gui_window_ctx(win);
    if (!fm || !fm->iv) return;                  /* widgets not built yet */
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    /* §M65 — the toolkit owns the menu bar's rectangle; everything below is
     * still hand-placed (this app predates the layout engine, and porting it
     * wholesale is a separate change from giving it a menu). */
    ui_layout(win);

    fm->path_in->base.x = 8;   fm->path_in->base.y = 4 + FM_MENU_H;
    fm->path_in->base.w = cw - 16;

    /* Button row keeps fixed positions (set at build time). */

    fm->iv->base.x = 8;   fm->iv->base.y = 44 + FM_MENU_H;
    fm->iv->base.w = cw - 16;
    fm->iv->base.h = ch - 44 - FM_MENU_H - 48;

    fm->name_in->base.x = 8;
    fm->name_in->base.y = ch - 40;
    fm->name_in->base.w = cw - 16;

    fm->status->base.x = 8;   fm->status->base.y = ch - 18;
    fm->status->base.w = cw - 16;
}

static void fm_on_close(struct gui_window* win) {
    (void)win;
    fm_win = NULL;                               /* ctx (struct fileman) is
                                                  * kfree'd by the window */
}

void fileman_open(void) {
    if (fm_win) { gui_window_raise(fm_win); return; }

    struct fileman* fm = (struct fileman*)kcalloc(1, sizeof(*fm));
    if (!fm) return;
    fm->path[0] = '/';
    fm->path[1] = 0;
    fm->del_armed_idx = -1;

    struct gui_window* win =
        gui_app_window_create("File Manager", 220, 100, 520, 460, fm_layout, fm);
    if (!win) { kfree(fm); return; }
    fm_win  = win;
    fm->win = win;
    gui_window_set_on_close(win, fm_on_close);

    /* The menu bar goes in FIRST, through the toolkit: it is the only widget
     * here the layout engine owns, and it must sit above everything else. */
    {
        struct ui_spec sp = { .id = FM_MENU_ID, .cls = "menubar",
                              .flags = UI_FILL_W };
        ui_build(win, &sp, 1, fm_ui_event, fm);
        ui_menubar_set(win, FM_MENU_ID, fm_menu, FM_MENU_N);
    }

    fm->path_in = w_textinput_create(win, 8, 4 + FM_MENU_H, 480, fm);
    w_button_create(win,   8, 24 + FM_MENU_H, 44, 18, "Up",    fm_up,    fm);
    w_button_create(win,  56, 24 + FM_MENU_H, 56, 18, "MkDir", fm_mkdir, fm);
    w_button_create(win, 116, 24 + FM_MENU_H, 56, 18, "Touch", fm_touch, fm);
    w_button_create(win, 176, 24 + FM_MENU_H, 50, 18, "Ren",   fm_ren,   fm);
    w_button_create(win, 230, 24 + FM_MENU_H, 54, 18, "Copy",  fm_copy,  fm);
    w_button_create(win, 288, 24 + FM_MENU_H, 44, 18, "Del",   fm_del,   fm);
    w_button_create(win, 336, 24 + FM_MENU_H, 50, 18, "View",  fm_view,  fm);
    /* §M65 — no hand-padded header label any more: the TABLE view draws its
     * own from the model, so the columns and their titles cannot drift apart
     * (the old one was a string with spaces in it, and it stopped lining up
     * the moment a name was long). */
    /* The MODEL carries its own ctx — the widget's ctx is the widget's.  Set
     * here rather than in the static initialiser because `fm` is this window's
     * instance, and a model shared between two file-manager windows would
     * otherwise answer for whichever opened last. */
    fm->model = fm_model;
    fm->model.ctx = fm;
    fm->iv = w_itemview_create(win, 8, 44 + FM_MENU_H, 480, 300,
                               &fm->model, config_get("fileman.view", "table"), fm);
    fm->name_in = w_textinput_create(win, 8, 380, 480, fm);
    fm->status  = w_label_create(win, 8, 420, 480, "");

    if (!fm->path_in || !fm->iv || !fm->name_in || !fm->status) {
        gui_window_close(win);                   /* frees fm as app_ctx */
        return;
    }

    fm->name_in->on_submit = fm_name_submit;
    fm->path_in->on_submit = fm_path_submit;
    fm->status->color = 0xFF8C9AAAu;

    fm_layout(win);
    fm_refresh(fm);
    gui_window_focus_widget(win, &fm->iv->base);
    gui_window_request_redraw(win);
}

/* Self-registration (M22.2): the Start menu and the `launch` command
 * find us here — nothing references fileman_open by symbol anymore. */
GUI_APP_ICON("File Manager", fileman_open, ICON_FOLDER);
