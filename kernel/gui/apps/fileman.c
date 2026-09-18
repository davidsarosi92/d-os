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
 *   - Del deletes files immediately; a NON-EMPTY directory raises the
 *     §M69 MODAL DIALOG and deletes the tree only on confirmation.  It
 *     used to arm a two-step Del-within-8-seconds gesture — which was
 *     not a design but the absence of one: there was nothing in this
 *     system able to ask a question,
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
#include "shortcut.h"  /* §M64 tail — Send to desktop writes a .lnk */
#include "dialog.h"    /* §M69 — the recursive delete asks, in words */
#include "console_plate.h"
#include "locale.h"
#include "config.h"
#include "vfs.h"
#include "timer.h"
#include "kmalloc.h"
#include "printf.h"
#include "klog.h"
#include <stddef.h>
#include <stdint.h>

#define FM_PATH_MAX 224
#define FM_NBTN     7                   /* Up MkDir Touch Ren Copy Del View */
#define FM_NAME_COL 30                  /* listview name column width */

struct fileman {
    struct gui_window*  win;
    char                path[FM_PATH_MAX];
    struct w_textinput* path_in;
    struct w_label*     status;
    struct w_itemview*  iv;
    struct item_model   model;          /* per-window: ctx points at this fm  */
    struct w_textinput* name_in;
    /* §M69 — held so fm_layout can size them from the live density; they used
     * to be created at literal widths and never touched again. */
    struct w_button*    btn[FM_NBTN];
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
};

static struct gui_window* fm_win = NULL;         /* singleton */
/* §M69 — the same singleton's CONTEXT, kept beside the window because a
 * deferred dialog answer needs to know whether the app it belonged to is
 * still there.  Both are cleared together in fm_on_close. */
static struct fileman*    fm_ctx = NULL;

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
    return c == 0 ? lstr("col.name") : lstr("col.size");
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
    .empty_text = "empty.folder", .empty_action = "empty.folder.act",
    .columns = fm_m_columns, .col_title = fm_m_title,
    .col_weight = fm_m_weight, .cell = fm_m_cell,
};

static void fm_refresh(struct fileman* fm) {
    w_textinput_set(fm->path_in, fm->path);
    fm->count = 0;
    if (fm->iv) { fm->iv->sel = -1; fm->iv->scroll = 0; }

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
    v->lv->base.x = cp_px(6);  v->lv->base.y = cp_px(6);
    v->lv->base.w = cw - 2 * cp_px(6);
    v->lv->base.h = ch - 12;
}

static void viewer_open(const char* path, const char* name) {
    struct viewer* v = (struct viewer*)kcalloc(1, sizeof(*v));
    if (!v) return;

    char title[24] = "View: ";
    int p = 6;
    for (int i = 0; name[i] && p < (int)sizeof(title) - 1; i++) title[p++] = name[i];
    title[p] = 0;

    struct gui_window* win = gui_app_open(&(struct gui_app_spec){
        .title = title,
        .content_w = cp_px(520), .content_h = cp_px(380),
        .layout = viewer_layout, .ctx = v,
    });
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
                     "fm.mkdirok", "fm.mkdirfail");
}

static void fm_touch(struct w_button* b, void* ctx) {
    (void)b;
    fm_create_common((struct fileman*)ctx, vfs_create,
                     "fm.touchok", "fm.touchfail");
}

/* §M69 — RECURSIVE DELETE NOW ASKS, IN WORDS.
 *
 * It used to arm a two-step confirm: press Del, read a status line, press Del
 * again within eight seconds.  That was not a design choice — it was what a
 * system with no way to ask a question does instead of asking one.  It is
 * undiscoverable (nothing on screen suggests a second press), it is a hidden
 * deadline, and the thing it guards is the single most destructive action this
 * application has.
 *
 * THE ANSWER ARRIVES LATER, ON ANOTHER TASK, so nothing captured here may be
 * dereferenced when it comes back.  The PATH is copied by value; the file
 * manager is identified by its singleton window pointer, which is only ever
 * COMPARED against the live `fm_win` and dereferenced solely when they match
 * (§M56.2's rule for a cache that must survive its object dying — the same
 * shape, one layer up).  If the user closed the file manager while the dialog
 * was up, the delete still happens and the refresh simply does not. */
static char fm_del_path[FM_PATH_MAX];
static struct gui_window* fm_del_owner;
/* Handed from the dialog's task to the file manager's; consumed in fm_layout,
 * which is the only code here that runs on the owning host. */
static volatile int fm_reload_pending;
static const char*  fm_reload_msg;

static void fm_del_answer(int answer, void* ctx) {
    (void)ctx;
    if (answer != GUI_DIALOG_OK) return;

    /* THE DELETE HAPPENS HERE regardless of whether the file manager is still
     * open: the user confirmed it, and an action that silently depends on a
     * window still existing is one nobody can predict.  The VFS does not care
     * which task calls it. */
    int ok = (vfs_unlink_recursive(fm_del_path) == 0);
    int live = (fm_win && fm_win == fm_del_owner && fm_ctx);
    kprintf("fileman: recursive delete of '%s' %s (owner %s)\n", fm_del_path,
            ok ? "ok" : "FAILED", live ? "live" : "gone");
    if (!live) return;

    /* THE WIDGETS ARE NOT OURS TO TOUCH.  This runs on the DIALOG's app-host
     * task, and §M22.7's rule is that a window's widgets belong to the task
     * that hosts it — writing a label from here is a cross-task mutation with
     * no lock, and the visible half of it was worse than the invisible half:
     * the first version really did call fm_refresh() from this task, the model
     * really was reloaded, and NOTHING ON SCREEN CHANGED, because damaging a
     * window is the host's job and nobody had asked for a repaint.  *A
     * cross-task write that appears to do nothing is the most expensive kind:
     * it looks like a missing feature, so the fix gets aimed at the wrong
     * layer.*
     *
     * So the answer is handed over as DATA and the file manager's own task
     * acts on it, woken by a layout request — the one existing route from any
     * task into a window's host loop. */
    fm_reload_msg = ok ? "fm.treedeleted" : "recursive delete failed";
    fm_reload_pending = 1;
    gui_window_request_layout(fm_win);
}

/* Del: files (and empty dirs) go immediately — that is reversible enough to
 * be worth a click rather than a conversation; a non-empty directory raises
 * the modal dialog above. */
static void fm_del(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0) { w_label_set(fm->status, "fm.selectfirst"); return; }

    char np[FM_PATH_MAX];
    path_join(np, (int)sizeof(np), fm->path, fm->names[sel]);

    int r = vfs_unlink(np);
    if (r == 0) {
        w_label_set(fm->status, "fm.deleted");
        fm_refresh(fm);
    } else if (r == -2) {
        int i = 0;
        while (np[i] && i < (int)sizeof fm_del_path - 1) { fm_del_path[i] = np[i]; i++; }
        fm_del_path[i] = 0;
        fm_del_owner = fm->win;

        /* COMPOSED IN CODE, and marked as such (locale.h: there is no message
         * formatter, on purpose).  Two catalogue lines joined with the newline
         * the dialog splits on — so a translator still owns both sentences and
         * neither has a placeholder in it. */
        static char body[192];
        {
            const char* a = lstr("dlg.notempty");
            const char* b = lstr("dlg.alsodeleted");
            int n = 0;
            while (*a && n < (int)sizeof body - 2) body[n++] = *a++;
            if (n < (int)sizeof body - 2) body[n++] = '\n';
            while (*b && n < (int)sizeof body - 1) body[n++] = *b++;
            body[n] = 0;
        }
        struct gui_dialog_req req = {
            .title = lstr("dlg.deletedir"),
            .body  = body,
            .info  = fm_del_path,
            .ok_text = lstr("dlg.deletetree"),
            .cancel_text = lstr("btn.cancel"),
            .on_answer = fm_del_answer,
        };
        if (gui_dialog_open(&req) != 0)
            w_label_set(fm->status, "fm.dlgbusy");
        else
            w_label_set(fm->status, "fm.confirm");
    } else {
        w_label_set(fm->status, "fm.delfail");
    }
}

/* Ren: selected entry → name from the input box (same directory). */
/* §M64 tail — SEND TO DESKTOP.  The last piece of "a shortcut is a file": the
 * file manager already creates, renames and deletes files, so the one thing it
 * could not do was make the KIND of file the desktop reads.
 *
 * The target is `file:<path>`, not a copy of anything — a shortcut points at
 * something that already exists, and §M64's one resolver already knows how to
 * open a path through the GUI_APP_ASSOC association.  The icon is left to
 * shortcut_add's default rather than guessed from the extension here: the
 * mapping from content to icon belongs next to the icons, not in a menu
 * handler, and guessing it in two places is how they drift. */
static void fm_sendto(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0) { w_label_set(fm->status, "fm.selectfirst"); return; }

    char p[FM_PATH_MAX];
    path_join(p, (int)sizeof(p), fm->path, fm->names[sel]);

    char target[FM_PATH_MAX + 8];
    int n = 0;
    const char* pre = "file:";
    while (pre[n]) { target[n] = pre[n]; n++; }
    for (int i = 0; p[i] && n < (int)sizeof target - 1; i++) target[n++] = p[i];
    target[n] = '\0';

    int rc = shortcut_add(fm->names[sel], target, NULL);
    if (rc == 0)       { w_label_set(fm->status, "sent to desktop");
                         /* Say it on the log too.  The status line is visible
                          * only in a screenshot, and a screenshot cannot show
                          * that a FILE was written — which is what a shortcut
                          * is (§M64).  The drop in §4.79 logs itself for the
                          * same reason. */
                         klog(KLOG_INFO, "gui", "fileman: sent '%s' to the desktop\n",
                              fm->names[sel]); }
    else if (rc == -2) w_label_set(fm->status, "desktop is full");
    else               w_label_set(fm->status, "could not write the shortcut");
}

static void fm_ren(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    int sel = fm->iv ? fm->iv->sel : -1;
    if (sel < 0)              { w_label_set(fm->status, "fm.selectfirst"); return; }
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
    FM_CMD_SENDTO,
};

static const struct ui_menu_def fm_menu[] = {
    { "menu.file",   "New folder",  FM_CMD_MKDIR   },
    { "menu.file",   "New file",    FM_CMD_TOUCH   },
    { "menu.file",   "-",           0              },
    { "menu.file",   "Rename",      FM_CMD_REN     },
    { "menu.file",   "Copy",        FM_CMD_COPY    },
    { "menu.file",   "Delete",      FM_CMD_DEL     },
    { "menu.file",   "Send to desktop", FM_CMD_SENDTO },
    { "menu.file",   "-",           0              },
    { "menu.file",   "Close",       FM_CMD_CLOSE   },
    { "menu.view",   "Open",        FM_CMD_VIEW    },
    { "menu.view",   "Refresh",     FM_CMD_REFRESH },
    { "menu.go",     "Up",          FM_CMD_UP      },
    { "menu.go",     "Root",        FM_CMD_ROOT    },
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
    case FM_CMD_SENDTO:  fm_sendto(NULL, fm); break;
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

    /* §M69 — a deferred dialog answer, delivered on THIS task.  A layout runs
     * on a resize too, so consuming a flag here costs nothing when there is
     * none, and the host repaints the window afterwards either way. */
    if (fm_reload_pending) {
        fm_reload_pending = 0;
        /* Reload FIRST: fm_refresh writes its own status on failure, and a
         * message set before it would be replaced by a less specific one. */
        fm_refresh(fm);
        w_label_set(fm->status, fm_reload_msg ? fm_reload_msg : "");
    }

    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    /* §M65 — the toolkit owns the menu bar's rectangle; everything below is
     * still hand-placed (this app predates the layout engine, and porting it
     * wholesale is a separate change from giving it a menu). */
    ui_layout(win);

    const int pad  = cp_px(8);
    const int gap  = cp_px(6);
    const int rowh = cp_btn_h();   /* rule 0: this row is buttons + a text box */
    const int menu = FM_MENU_H;

    fm->path_in->base.x = pad;  fm->path_in->base.y = menu + gap;
    fm->path_in->base.w = cw - 2 * pad;
    fm->path_in->base.h = rowh;

    /* §M69 — each button takes the width its own label needs (rule 2), laid
     * left to right.  A row that runs out of width DROPS from the right rather
     * than overlapping: half a button is a control that looks pressable and
     * hits its neighbour. */
    int by = menu + gap + rowh + gap;
    int bx = pad;
    for (int i = 0; i < FM_NBTN; i++) {
        if (!fm->btn[i]) continue;
        int bw = w_button_autosize(fm->btn[i], bx, by);
        fm->btn[i]->base.disabled = 0;
        if (bx + bw > cw - pad) { fm->btn[i]->base.w = 0; continue; }
        bx += bw + gap;
    }

    int top = by + rowh + gap;
    int bot = ch - pad - rowh - gap - cp_row_h();   /* name input + status */
    fm->iv->base.x = pad;   fm->iv->base.y = top;
    fm->iv->base.w = cw - 2 * pad;
    fm->iv->base.h = bot - top;
    if (fm->iv->base.h < rowh) fm->iv->base.h = rowh;

    fm->name_in->base.x = pad;
    fm->name_in->base.y = bot + gap;
    fm->name_in->base.w = cw - 2 * pad;
    fm->name_in->base.h = rowh;

    fm->status->base.x = pad;
    fm->status->base.y = bot + gap + rowh + gap;
    fm->status->base.w = cw - 2 * pad;
}

static void fm_on_close(struct gui_window* win) {
    (void)win;              /* fm_win is the spec's slot; the compositor
                             * clears it, and `ctx` is kfree'd by the window */
    /* §M69 — the context goes with it.  A deferred dialog answer arriving
     * after this point must find nothing to write into. */
    fm_ctx = NULL;
}

void fileman_open(void) {
    if (fm_win) { gui_window_raise(fm_win); return; }

    struct fileman* fm = (struct fileman*)kcalloc(1, sizeof(*fm));
    if (!fm) return;
    fm->path[0] = '/';
    fm->path[1] = 0;

    struct gui_window* win = gui_app_open(&(struct gui_app_spec){
        .title = "app.filemanager",
        .content_w = cp_px(560), .content_h = cp_px(460),
        .layout = fm_layout, .ctx = fm,
        .on_close = fm_on_close, .slot = &fm_win,
    });
    if (!win) { kfree(fm); return; }
    fm_ctx  = fm;
    fm->win = win;

    /* The menu bar goes in FIRST, through the toolkit: it is the only widget
     * here the layout engine owns, and it must sit above everything else. */
    {
        struct ui_spec sp = { .id = FM_MENU_ID, .cls = "menubar",
                              .flags = UI_FILL_W };
        ui_build(win, &sp, 1, fm_ui_event, fm);
        ui_menubar_set(win, FM_MENU_ID, fm_menu, FM_MENU_N);
    }

    fm->path_in = w_textinput_create(win, 8, 4 + FM_MENU_H, 480, fm);
    /* §M69 — the row is BUILT here and SIZED in fm_layout, which is the only
     * place that knows the density is settled.  The literal 44/56/50/54 widths
     * and the 18 px height they all shared were measured for the 8x8 font, and
     * at a runtime face they cut through their own labels. */
    fm->btn[0] = w_button_create(win, 0, 0, 0, 0, "btn.up",    fm_up,    fm);
    fm->btn[1] = w_button_create(win, 0, 0, 0, 0, "btn.mkdir", fm_mkdir, fm);
    fm->btn[2] = w_button_create(win, 0, 0, 0, 0, "btn.touch", fm_touch, fm);
    fm->btn[3] = w_button_create(win, 0, 0, 0, 0, "btn.rename", fm_ren,   fm);
    fm->btn[4] = w_button_create(win, 0, 0, 0, 0, "btn.copy",  fm_copy,  fm);
    fm->btn[5] = w_button_create(win, 0, 0, 0, 0, "btn.delete", fm_del,   fm);
    fm->btn[6] = w_button_create(win, 0, 0, 0, 0, "btn.view",  fm_view,  fm);
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
    /* §M69 — the theme's secondary text.  A literal grey-blue is a dark-theme
     * value, and under the light theme a status line in it is nearly invisible
     * against a light panel. */
    fm->status->role = WLBL_MUTED;   /* theme-following, not a captured colour */

    fm_layout(win);
    fm_refresh(fm);
    gui_window_focus_widget(win, &fm->iv->base);
    gui_window_request_redraw(win);
}

/* Self-registration (M22.2): the Start menu and the `launch` command
 * find us here — nothing references fileman_open by symbol anymore. */
GUI_APP_ICON("File Manager", fileman_open, ICON_FOLDER);
