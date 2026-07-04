/* =============================================================================
 * fileman.c — file manager app window (M22.1).
 *
 * A single-instance APP window built from the widget toolkit:
 *
 *   ┌ File Manager ────────────────────────── x ┐
 *   │ /mnt/dir                    (path label)  │
 *   │ [Up] [MkDir] [Touch] [Del] [View]         │
 *   │ ┌───────────────────────────────────────┐ │
 *   │ │ subdir/                    (listview) │ │
 *   │ │ file.txt                              │ │
 *   │ └───────────────────────────────────────┘ │
 *   │ [name input___________________]           │
 *   │ status line                               │
 *   └───────────────────────────────────────────┘
 *
 * Interaction model: single click selects, double click opens (descend
 * into directory / view file).  MkDir + Touch create `path/<input>`;
 * Del removes the selected entry (vfs_unlink: files + empty dirs —
 * note ramfs implements it, exFAT does not yet and reports failure).
 * View opens a read-only viewer window (first ~8 KiB, line-split).
 *
 * Everything here runs on the compositor task (widget callbacks), so
 * calling the VFS directly is fine.  See widget.h for the model.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include "vfs.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

#define FM_PATH_MAX 224

struct fileman {
    struct gui_window*  win;
    char                path[FM_PATH_MAX];
    struct w_label*     path_lbl;
    struct w_label*     status;
    struct w_listview*  lv;
    struct w_textinput* name_in;
};

static struct gui_window* fm_win = NULL;         /* singleton */

/* -------------------------------------------------------------------------- */
/* Path helpers (absolute paths only — mirrors the VFS conventions).           */
/* -------------------------------------------------------------------------- */

static int str_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

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

/* Strip the display suffix ("/" on dirs) the listview carries. */
static void entry_name(const char* item, char* out, int cap) {
    int n = str_len(item);
    if (n > 0 && item[n - 1] == '/') n--;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = item[i];
    out[n] = 0;
}

/* -------------------------------------------------------------------------- */
/* Directory listing.                                                          */
/* -------------------------------------------------------------------------- */

static void fm_refresh(struct fileman* fm) {
    w_label_set(fm->path_lbl, fm->path);
    w_listview_clear(fm->lv);

    struct file* f = vfs_open(fm->path, VFS_RDONLY);
    if (!f) {
        w_label_set(fm->status, "cannot open directory");
        return;
    }
    struct dirent de;
    int n, total = 0;
    while ((n = vfs_readdir(f, &de)) > 0) {
        char item[WLIST_ITEM_LEN];
        int p = 0;
        for (; de.name[p] && p < WLIST_ITEM_LEN - 2; p++) item[p] = de.name[p];
        if (de.type == INODE_DIR) item[p++] = '/';
        item[p] = 0;
        w_listview_add(fm->lv, item, (uint8_t)de.type);
        total++;
    }
    vfs_close(f);

    char st[32] = "   entries";                  /* [0][1]=digits [2]=space */
    st[0] = (char)('0' + (total / 10) % 10);
    st[1] = (char)('0' + total % 10);
    if (total < 10) st[0] = ' ';
    w_label_set(fm->status, st);
}

/* -------------------------------------------------------------------------- */
/* Viewer window (read-only, first 8 KiB, line-split into a listview).         */
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

static void fm_activate(struct w_listview* lv, int idx, void* ctx) {
    struct fileman* fm = (struct fileman*)ctx;
    if (idx < 0 || idx >= lv->count) return;
    char name[VFS_NAME_MAX + 1];
    entry_name(lv->items[idx], name, (int)sizeof(name));

    if (lv->tags[idx] == (uint8_t)INODE_DIR) {
        char np[FM_PATH_MAX];
        path_join(np, (int)sizeof(np), fm->path, name);
        int p = 0;
        for (; np[p]; p++) fm->path[p] = np[p];
        fm->path[p] = 0;
        fm_refresh(fm);
    } else {
        char fp[FM_PATH_MAX];
        path_join(fp, (int)sizeof(fp), fm->path, name);
        viewer_open(fp, name);
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

static void fm_del(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    if (fm->lv->sel < 0) { w_label_set(fm->status, "select an entry first"); return; }
    char name[VFS_NAME_MAX + 1];
    entry_name(fm->lv->items[fm->lv->sel], name, (int)sizeof(name));
    char np[FM_PATH_MAX];
    path_join(np, (int)sizeof(np), fm->path, name);
    int r = vfs_unlink(np);
    if (r == 0)       { w_label_set(fm->status, "deleted"); fm_refresh(fm); }
    else if (r == -2) w_label_set(fm->status, "directory not empty");
    else              w_label_set(fm->status, "delete failed (fs read-only?)");
}

static void fm_view(struct w_button* b, void* ctx) {
    (void)b;
    struct fileman* fm = (struct fileman*)ctx;
    if (fm->lv->sel < 0) { w_label_set(fm->status, "select a file first"); return; }
    fm_activate(fm->lv, fm->lv->sel, fm);        /* same as double-click */
}

static void fm_name_submit(struct w_textinput* t, void* ctx) {
    (void)t;
    /* Enter in the name box = Touch (create file) — the common case. */
    fm_touch(NULL, ctx);
}

/* -------------------------------------------------------------------------- */
/* Layout + lifetime.                                                          */
/* -------------------------------------------------------------------------- */

static void fm_layout(struct gui_window* win) {
    struct fileman* fm = (struct fileman*)gui_window_ctx(win);
    if (!fm || !fm->lv) return;                  /* widgets not built yet */
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    fm->path_lbl->base.x = 8;   fm->path_lbl->base.y = 3;
    fm->path_lbl->base.w = cw - 16;

    /* Button row keeps fixed positions (set at build time). */

    fm->lv->base.x = 8;   fm->lv->base.y = 44;
    fm->lv->base.w = cw - 16;
    fm->lv->base.h = ch - 44 - 44;

    fm->name_in->base.x = 8;
    fm->name_in->base.y = ch - 38;
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

    struct gui_window* win =
        gui_app_window_create("File Manager", 220, 100, 500, 430, fm_layout, fm);
    if (!win) { kfree(fm); return; }
    fm_win  = win;
    fm->win = win;
    gui_window_set_on_close(win, fm_on_close);

    fm->path_lbl = w_label_create(win, 8, 3, 460, "/");
    w_button_create(win,   8, 20, 44, 18, "Up",    fm_up,    fm);
    w_button_create(win,  58, 20, 56, 18, "MkDir", fm_mkdir, fm);
    w_button_create(win, 120, 20, 56, 18, "Touch", fm_touch, fm);
    w_button_create(win, 182, 20, 44, 18, "Del",   fm_del,   fm);
    w_button_create(win, 232, 20, 50, 18, "View",  fm_view,  fm);
    fm->lv      = w_listview_create(win, 8, 44, 460, 300, fm);
    fm->name_in = w_textinput_create(win, 8, 350, 460, fm);
    fm->status  = w_label_create(win, 8, 390, 460, "");

    if (!fm->path_lbl || !fm->lv || !fm->name_in || !fm->status) {
        gui_window_close(win);                   /* frees fm as app_ctx */
        return;
    }

    fm->lv->on_activate  = fm_activate;
    fm->name_in->on_submit = fm_name_submit;
    fm->status->color = 0xFF8C9AAAu;

    gui_window_focus_widget(win, &fm->name_in->base);
    fm_layout(win);
    fm_refresh(fm);
    gui_window_request_redraw(win);
}

/* Self-registration (M22.2): the Start menu and the `launch` command
 * find us here — nothing references fileman_open by symbol anymore. */
GUI_APP("File Manager", fileman_open);
