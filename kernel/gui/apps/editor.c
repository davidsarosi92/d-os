/* =============================================================================
 * editor.c — text editor app (M22.5, PLAN §M22.5 stage 4).
 *
 * A thin VFS shell around the w_editor widget:
 *
 *   ┌ Editor: name ─────────────────── _ □ x ┐
 *   │ [path input______________][Open][Save] │
 *   │ ┌─────────────────────────────────────┐│
 *   │ │ editor widget                       ││
 *   │ └─────────────────────────────────────┘│
 *   │ status line                            │
 *   └─────────────────────────────────────────┘
 *
 * Open loads the path in the input box; Save writes the buffer back
 * (VFS_CREATE|VFS_TRUNC — so Save-as = edit the path, then Save).
 * Ctrl+S saves too (w_editor forwards unclaimed Ctrl+letters through
 * on_shortcut).  NOT a singleton: every launch is a fresh window, so
 * two files can be edited side by side.
 *
 * Registered with a file-type association (GUI_APP_ASSOC): the file
 * manager double-click opens .txt/.conf/.md/.cfg/.log here.
 *
 * All callbacks run on the compositor task — VFS use is fine.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include "keymap.h"
#include "vfs.h"
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

#define ED_MAX_FILE (256 * 1024)        /* refuse to load bigger files */

struct edapp {
    struct gui_window*  win;
    struct w_textinput* path_in;
    struct w_editor*    ed;
    struct w_label*     status;
};

/* ---- tiny string helpers ---------------------------------------------------- */

static void set_title(struct edapp* a, const char* path) {
    /* "Edit: <basename>" — the window title array is 24 bytes. */
    char t[24] = "Edit: ";
    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '/') base = p + 1;
    int i = 6;
    for (int j = 0; base[j] && i < (int)sizeof(t) - 1; j++) t[i++] = base[j];
    t[i] = 0;
    gui_window_set_title(a->win, t);
}

/* ---- load / save ------------------------------------------------------------- */

static void ed_load(struct edapp* a) {
    const char* path = a->path_in->buf;
    if (a->path_in->len == 0) { w_label_set(a->status, "type a path first"); return; }

    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { w_label_set(a->status, "cannot open file"); return; }

    /* Read-loop into a growing buffer (dirent size isn't in reach from
     * a bare path, and trusting it would race with writers anyway). */
    int cap = 4096, len = 0;
    char* buf = (char*)kmalloc((size_t)cap);
    if (!buf) { vfs_close(f); w_label_set(a->status, "OOM"); return; }
    for (;;) {
        if (len == cap) {
            if (cap >= ED_MAX_FILE) { w_label_set(a->status, "file too large"); break; }
            int ncap = cap * 2;
            char* nb = (char*)kmalloc((size_t)ncap);
            if (!nb) { w_label_set(a->status, "OOM"); break; }
            for (int i = 0; i < len; i++) nb[i] = buf[i];
            kfree(buf);
            buf = nb; cap = ncap;
        }
        ssize_t r = vfs_read(f, buf + len, (size_t)(cap - len));
        if (r <= 0) break;
        len += (int)r;
    }
    vfs_close(f);

    if (w_editor_set_text(a->ed, buf, len) != 0) {
        w_label_set(a->status, "OOM loading buffer");
    } else {
        char st[48] = "loaded ";
        int p = 7, v = len, digs = 0;
        char tmp[12];
        do { tmp[digs++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (digs) st[p++] = tmp[--digs];
        st[p++] = ' '; st[p++] = 'b'; st[p++] = 'y'; st[p++] = 't';
        st[p++] = 'e'; st[p++] = 's'; st[p] = 0;
        w_label_set(a->status, st);
        set_title(a, path);
    }
    kfree(buf);
    gui_window_focus_widget(a->win, &a->ed->base);
}

static void ed_save(struct edapp* a) {
    const char* path = a->path_in->buf;
    if (a->path_in->len == 0) { w_label_set(a->status, "type a path first"); return; }

    int len = 0;
    const char* data = w_editor_text(a->ed, &len);

    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) { w_label_set(a->status, "cannot open/create (read-only fs?)"); return; }

    int off = 0, err = 0;
    while (off < len) {
        ssize_t r = vfs_write(f, data + off, (size_t)(len - off));
        if (r <= 0) { err = 1; break; }
        off += (int)r;
    }
    vfs_close(f);

    if (err) {
        w_label_set(a->status, "write failed (fs full / read-only?)");
    } else {
        a->ed->modified = 0;
        char st[48] = "saved ";
        int p = 6, v = len, digs = 0;
        char tmp[12];
        do { tmp[digs++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (digs) st[p++] = tmp[--digs];
        st[p++] = ' '; st[p++] = 'b'; st[p++] = 'y'; st[p++] = 't';
        st[p++] = 'e'; st[p++] = 's'; st[p] = 0;
        w_label_set(a->status, st);
        set_title(a, path);
    }
}

/* ---- callbacks ---------------------------------------------------------------- */

static void ed_open_click(struct w_button* b, void* ctx) {
    (void)b; ed_load((struct edapp*)ctx);
}

static void ed_save_click(struct w_button* b, void* ctx) {
    (void)b; ed_save((struct edapp*)ctx);
}

static void ed_path_submit(struct w_textinput* t, void* ctx) {
    (void)t; ed_load((struct edapp*)ctx);       /* Enter in path = Open */
}

static void ed_shortcut(struct w_editor* e, uint8_t kc, void* ctx) {
    (void)e;
    if (kc == KC_S) ed_save((struct edapp*)ctx);  /* Ctrl+S */
    if (kc == KC_O) ed_load((struct edapp*)ctx);  /* Ctrl+O */
}

/* ---- layout + lifetime --------------------------------------------------------- */

static void ed_layout(struct gui_window* win) {
    struct edapp* a = (struct edapp*)gui_window_ctx(win);
    if (!a || !a->ed) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    a->path_in->base.x = 8;   a->path_in->base.y = 6;
    a->path_in->base.w = cw - 16 - 2 * 58;

    a->ed->base.x = 8;   a->ed->base.y = 30;
    a->ed->base.w = cw - 16;
    a->ed->base.h = ch - 30 - 22;

    a->status->base.x = 8;   a->status->base.y = ch - 16;
    a->status->base.w = cw - 16;
}

/* The two buttons need layout too — stash them in the ctx. */
struct edapp_full {
    struct edapp a;
    struct w_button* open_btn;
    struct w_button* save_btn;
};

static void ed_layout_full(struct gui_window* win) {
    struct edapp_full* af = (struct edapp_full*)gui_window_ctx(win);
    if (!af || !af->a.ed) return;
    ed_layout(win);
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    (void)ch;
    af->open_btn->base.x = cw - 8 - 2 * 54 - 4;  af->open_btn->base.y = 5;
    af->save_btn->base.x = cw - 8 - 54;          af->save_btn->base.y = 5;
}

static void editor_open_with(const char* path) {
    struct edapp_full* af = (struct edapp_full*)kcalloc(1, sizeof(*af));
    if (!af) return;

    struct gui_window* win =
        gui_app_window_create("Editor", 180, 90, 620, 460, ed_layout_full, af);
    if (!win) { kfree(af); return; }
    af->a.win = win;

    af->a.path_in = w_textinput_create(win, 8, 6, 460, af);
    af->open_btn  = w_button_create(win, 480, 5, 54, 18, "Open",
                                    ed_open_click, &af->a);
    af->save_btn  = w_button_create(win, 540, 5, 54, 18, "Save",
                                    ed_save_click, &af->a);
    af->a.ed      = w_editor_create(win, 8, 30, 588, 380, &af->a);
    af->a.status  = w_label_create(win, 8, 424, 588, "new buffer");

    if (!af->a.path_in || !af->open_btn || !af->save_btn ||
        !af->a.ed || !af->a.status) {
        gui_window_close(win);                  /* frees af as app_ctx */
        return;
    }

    af->a.path_in->on_submit = ed_path_submit;
    af->a.ed->on_shortcut    = ed_shortcut;
    af->a.status->color      = 0xFF8C9AAAu;

    ed_layout_full(win);

    if (path && *path) {
        w_textinput_set(af->a.path_in, path);
        ed_load(&af->a);
    } else {
        gui_window_focus_widget(win, &af->a.ed->base);
    }
    gui_window_request_redraw(win);
}

static void editor_launch(void)                { editor_open_with(NULL); }
static void editor_open_path(const char* path) { editor_open_with(path); }

GUI_APP_ASSOC("Editor", editor_launch, editor_open_path,
              "txt conf md cfg log");
