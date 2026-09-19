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
#include "icons.h"
#include "widget.h"
#include "console_plate.h"
#include "keymap.h"
#include "vfs.h"
#include "kmalloc.h"
#include "devtools.h"          /* §M43 — Compile & Run */
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

/* A read-only "Output" window showing a run's captured stdout (§M43). */
struct outwin { struct w_editor* ed; };
static void outwin_layout(struct gui_window* win) {
    struct outwin* o = (struct outwin*)gui_window_ctx(win);
    if (!o || !o->ed) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    o->ed->base.x = 6; o->ed->base.y = 6;
    o->ed->base.w = cw - 12; o->ed->base.h = ch - 12;
}
static void show_output(const char* text) {
    int len = 0; while (text[len]) len++;
    struct outwin* o = (struct outwin*)kcalloc(1, sizeof *o);
    if (!o) return;
    struct gui_window* win = gui_app_open(&(struct gui_app_spec){
        .title = "Output",
        .content_w = cp_px(460), .content_h = cp_px(280),
        .layout = outwin_layout, .ctx = o,
    });
    if (!win) { kfree(o); return; }
    o->ed = w_editor_create(win, 6, 6, 448, 268, o);
    if (!o->ed) { gui_window_close(win); return; }
    w_editor_set_text(o->ed, text, len);
    outwin_layout(win);
}

/* §M43 — Compile & Run: save the buffer (must be a .c path), compile it with
 * the on-device tcc, run the result, and show the program's captured output in
 * an "Output" window.  The status line reports compile/run success + exit code. */
static void ed_run(struct edapp* a) {
    if (!dos_tcc_available()) { w_label_set(a->status, "tcc not built (make tcc)"); return; }
    if (a->path_in->len == 0) { w_label_set(a->status, "type a .c path first"); return; }
    ed_save(a);                                  /* persist the buffer first */
    w_label_set(a->status, "compiling...");
    if (dos_tcc_compile(a->path_in->buf, "/tmp.run.elf") != 0) {
        w_label_set(a->status, "compile FAILED (see console)");
        return;
    }
    char* cap = (char*)kcalloc(1, 8192);
    int rc = dos_run_elf_cap("/tmp.run.elf", cap, cap ? 8192 : 0);
    if (cap) { show_output(cap); kfree(cap); }   /* w_editor_set_text copies it */

    char st[48] = "compiled OK - ran, rc=";
    int p = 22, v = rc < 0 ? -rc : rc;           /* small-int formatter */
    char tmp[12]; int d = 0;
    if (rc < 0) st[p++] = '-';
    do { tmp[d++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (d) st[p++] = tmp[--d];
    st[p] = 0;
    w_label_set(a->status, st);
}

/* ---- callbacks ---------------------------------------------------------------- */

static void ed_run_click(struct w_button* b, void* ctx) {
    (void)b; ed_run((struct edapp*)ctx);
}

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
    /* (Compile & Run is the "Run" button; no Ctrl shortcut — KC_R unmapped.) */
}

/* ---- layout + lifetime --------------------------------------------------------- */

static void ed_layout(struct gui_window* win) {
    struct edapp* a = (struct edapp*)gui_window_ctx(win);
    if (!a || !a->ed) return;
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);

    const int pad = cp_px(8), gap = cp_px(6), rowh = cp_btn_h();

    a->path_in->base.x = pad;   a->path_in->base.y = gap;
    a->path_in->base.w = cw - 2 * pad;          /* narrowed below when the
                                                 * window has a button row  */
    a->path_in->base.h = rowh;

    int top = gap + rowh + gap;
    a->ed->base.x = pad;   a->ed->base.y = top;
    a->ed->base.w = cw - 2 * pad;
    a->ed->base.h = ch - top - gap - cp_row_h();
    if (a->ed->base.h < rowh) a->ed->base.h = rowh;

    a->status->base.x = pad;
    a->status->base.y = ch - cp_row_h();
    a->status->base.w = cw - 2 * pad;
}

/* The two buttons need layout too — stash them in the ctx. */
struct edapp_full {
    struct edapp a;
    struct w_button* run_btn;
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
    const int pad = cp_px(8), gap = cp_px(6);

    /* §M69 — RIGHT-ALIGNED, and measured RIGHT TO LEFT so the outer edge is
     * the fixed one.  The old code placed all three from a literal 54 px
     * width, so with a proportional face "Open" and "Save" sat inside boxes
     * meant for a different font and the row no longer met the margin.
     *
     * Sized first, then positioned: the width is not known until the button
     * has been asked, and the position of the one to its left depends on it. */
    struct w_button* row[3] = { af->save_btn, af->open_btn, af->run_btn };
    int x = cw - pad;
    for (int i = 0; i < 3; i++) {
        if (!row[i]) continue;
        int bw = w_button_autosize(row[i], 0, gap);   /* measure */
        x -= bw;
        row[i]->base.x = x;
        x -= gap;
    }
    /* Whatever the buttons left is the path bar's — so the two cannot overlap
     * however long the labels become (a localised "Mentés" is wider). */
    int avail = x + gap - pad - gap;
    if (avail < cp_fw() * 8) avail = cp_fw() * 8;
    af->a.path_in->base.w = avail;
}

static void editor_open_with(const char* path) {
    struct edapp_full* af = (struct edapp_full*)kcalloc(1, sizeof(*af));
    if (!af) return;

    struct gui_window* win = gui_app_open(&(struct gui_app_spec){
        .title = "Editor",
        .content_w = cp_px(620), .content_h = cp_px(460),
        .layout = ed_layout_full, .ctx = af,
    });
    if (!win) { kfree(af); return; }
    af->a.win = win;

    af->a.path_in = w_textinput_create(win, 8, 6, 400, af);
    /* Geometry is ed_layout_full's, which runs before the first paint. */
    af->run_btn   = w_button_create(win, 0, 0, 0, 0, "Run",
                                    ed_run_click, &af->a);
    af->open_btn  = w_button_create(win, 0, 0, 0, 0, "Open",
                                    ed_open_click, &af->a);
    af->save_btn  = w_button_create(win, 0, 0, 0, 0, "Save",
                                    ed_save_click, &af->a);
    af->a.ed      = w_editor_create(win, 8, 30, 588, 380, &af->a);
    af->a.status  = w_label_create(win, 8, 424, 588, "new buffer");

    if (!af->a.path_in || !af->run_btn || !af->open_btn || !af->save_btn ||
        !af->a.ed || !af->a.status) {
        gui_window_close(win);                  /* frees af as app_ctx */
        return;
    }

    af->a.path_in->on_submit = ed_path_submit;
    af->a.ed->on_shortcut    = ed_shortcut;
    af->a.status->color      = cp_current_theme()->muted;

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

GUI_APP_ASSOC_ICON("Editor", editor_launch, editor_open_path,
                   "txt conf md cfg log", ICON_DOC);
