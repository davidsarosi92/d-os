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

#include "users.h"
#include "cred.h"
#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "console_plate.h"
#include "keymap.h"
#include "vfs.h"
#include "kmalloc.h"
#include "devtools.h"          /* §M43 — Compile & Run */
#include "dialog.h"            /* "save your changes?" on close */
#include "config.h"            /* config_persist_path: where a recovery copy survives */
#include "printf.h"
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
/* The answer to "save your changes?", shared between this window and the
 * dialog that asks.  A TICKET WITH TWO REFERENCES, not a pointer to the app:
 * the answer arrives on the DIALOG's host task, possibly after this window was
 * closed by a session end that stopped waiting — and a pointer to a freed app
 * is where that would write.  Each side drops its reference when done; the
 * last one frees it; the answer is only ever READ on the editor's own host. */
struct ed_close_ticket {
    volatile int answer;                 /* -1 = not yet; else GUI_DIALOG_*   */
    volatile int refs;
    struct gui_window* win;
    uint32_t           serial;
};
static void ed_ticket_put(struct ed_close_ticket* t) {
    if (t && __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) == 0) kfree(t);
}

struct edapp_full {
    struct edapp a;
    struct w_button* run_btn;
    struct w_button* open_btn;
    struct w_button* save_btn;
    struct ed_close_ticket* ticket;      /* a close question in flight       */
};

/* ---- closing with unsaved changes (2026-09-25) ------------------------------
 *
 * A close — the X button, or the session ending — first asks this guard.  With
 * nothing unsaved it simply agrees.  Otherwise it asks, and the SAFE answer is
 * the default: the dialog's Cancel (also Escape and its own X) keeps editing,
 * because a dismissal that discarded work would lose it on a stray keypress.
 * If the session gives up waiting, ed_on_close still keeps the text (below). */
static void ed_close_answer(int answer, void* ctx) {
    struct ed_close_ticket* t = (struct ed_close_ticket*)ctx;
    t->answer = answer;
    /* The only route into another task's host loop; a no-op if the window has
     * gone (and a harmless relayout if its slot was reused). */
    if (gui_window_alive(t->win, t->serial)) gui_window_request_layout(t->win);
    ed_ticket_put(t);
}

static int ed_close_guard(struct gui_window* win, int reason) {
    struct edapp_full* af = (struct edapp_full*)gui_window_ctx(win);
    if (!af || !af->a.ed || !af->a.ed->modified) return 1;
    if (af->ticket) return 0;                    /* already asking */
    struct ed_close_ticket* t = (struct ed_close_ticket*)kcalloc(1, sizeof *t);
    if (!t) return 0;                            /* keep it; on_close still saves */
    t->answer = -1; t->refs = 2; t->win = win; t->serial = gui_window_serial(win);
    struct gui_dialog_req req = {
        .title = "Unsaved changes",
        .body  = reason == GUI_CLOSE_SESSION
                   ? "The session is ending. Save the changes to this file first?"
                   : "Save the changes to this file before closing?",
        .info  = af->a.path_in->len ? af->a.path_in->buf : "(no file name yet)",
        .ok_text = "Save", .cancel_text = "Keep editing",
        .on_answer = ed_close_answer, .ctx = t,
    };
    if (gui_dialog_open(&req) != 0) { kfree(t); return 0; }
    af->ticket = t;
    kprintf("editor: unsaved changes - asking before closing\n");
    return 0;
}

/* Runs on the editor's host, from its layout hook: consume a delivered answer. */
static void ed_take_close_answer(struct edapp_full* af) {
    struct ed_close_ticket* t = af->ticket;
    if (!t || t->answer < 0) return;
    int answer = t->answer;
    af->ticket = NULL;
    ed_ticket_put(t);
    if (answer != GUI_DIALOG_OK) return;         /* keep editing */
    ed_save(&af->a);
    if (!af->a.ed->modified) gui_window_close_now(af->a.win);
}

/* EVERY close route passes here — the guard's, the session end that stopped
 * waiting, a host that was killed — so this is where unsaved work is kept when
 * nobody got to answer: a copy next to the file, never over it. */
static void ed_on_close(struct gui_window* win) {
    struct edapp_full* af = (struct edapp_full*)gui_window_ctx(win);
    if (!af) return;
    if (af->ticket) { ed_ticket_put(af->ticket); af->ticket = NULL; }
    if (!af->a.ed || !af->a.ed->modified) return;
    char path[160];
    int n = 0;
    if (af->a.path_in && af->a.path_in->len) {
        const char* base = af->a.path_in->buf;
        for (; base[n] && n < (int)sizeof path - 10; n++) path[n] = base[n];
    } else {
        /* A buffer with no file: keep it where it SURVIVES a power-off.  This
         * runs on shutdown as well as sign-out, and "/" is ramfs — a recovery
         * copy there is gone by the time anybody could look for it. */
        const char* pp = config_persist_path();       /* "<vol>/d-os.conf" */
        int last = -1;
        for (int i = 0; pp && pp[i]; i++) if (pp[i] == '/') last = i;
        for (int i = 0; i < last && n < 120; i++) path[n++] = pp[i];
        const char* leaf = "/untitled";
        for (int i = 0; leaf[i]; i++) path[n++] = leaf[i];
    }
    const char* ext = ".unsaved";
    for (int i = 0; ext[i]; i++) path[n++] = ext[i];
    path[n] = 0;
    int len = 0;
    const char* data = w_editor_text(af->a.ed, &len);
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) {
        /* §M82 (2026-09-27) — NEXT TO THE FILE IS NOT ALWAYS WRITABLE.  An
         * ordinary account editing a file in a directory it does not own
         * (a root-owned `/`, somebody else's folder) cannot create a sibling,
         * and its work was simply lost — invisible while every test ran as an
         * administrator.  The account's own home can always be written. */
        const struct cred* c = cred_current();
        const struct user_account* u = c->owner == TASK_OWNER_USER ? user_by_uid(c->uid) : NULL;
        if (u) {
            char alt[160];
            int m = 0;
            const char* h = user_home(u);
            for (int i = 0; h[i] && m < 100; i++) alt[m++] = h[i];
            alt[m++] = '/';
            int last = -1;
            for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
            for (int i = last + 1; path[i] && m < (int)sizeof alt - 1; i++) alt[m++] = path[i];
            alt[m] = 0;
            f = vfs_open(alt, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
            if (f) {
                kprintf("editor: %s is not writable - keeping the unsaved text in your home\n", path);
                for (int i = 0; i <= m; i++) path[i] = alt[i];
            }
        }
    }
    if (!f) { kprintf("editor: closed with unsaved changes and could not keep them (%s)\n", path); return; }
    int off = 0;
    while (off < len) {
        ssize_t r = vfs_write(f, data + off, (size_t)(len - off));
        if (r <= 0) break;
        off += (int)r;
    }
    vfs_close(f);
    kprintf("editor: closed with unsaved changes - kept them in %s (%d bytes)\n", path, off);
}

static void ed_layout_full(struct gui_window* win) {
    struct edapp_full* af = (struct edapp_full*)gui_window_ctx(win);
    if (!af || !af->a.ed) return;
    ed_take_close_answer(af);
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

static int ed_test_unsaved;              /* see editor_open_test_unsaved */

static void editor_open_with(const char* path) {
    struct edapp_full* af = (struct edapp_full*)kcalloc(1, sizeof(*af));
    if (!af) return;

    struct gui_window* win = gui_app_open(&(struct gui_app_spec){
        .title = "Editor",
        .content_w = cp_px(620), .content_h = cp_px(460),
        .layout = ed_layout_full, .ctx = af,
        .on_close = ed_on_close,
    });
    if (!win) { kfree(af); return; }
    af->a.win = win;
    gui_window_set_close_guard(win, ed_close_guard);

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

    if (ed_test_unsaved) {
        static const char txt[] = "typed before the session ended\n";
        w_textinput_set(af->a.path_in, "/edtest.txt");
        w_editor_set_text(af->a.ed, txt, (int)sizeof txt - 1);
        af->a.ed->modified = 1;
    } else if (path && *path) {
        w_textinput_set(af->a.path_in, path);
        ed_load(&af->a);
    } else {
        gui_window_focus_widget(win, &af->a.ed->base);
    }
    gui_window_request_redraw(win);
}

/* For `logouttest editor` (gui.c): an Editor holding UNSAVED text, built on the
 * editor's own host — the harness cannot type into a GUI window (§4.74), and
 * setting the text from the test task would break the §M22.7 ownership rule
 * the close path is careful to keep. */
void editor_open_test_unsaved(void) {
    vfs_unlink("/edtest.txt");
    vfs_unlink("/edtest.txt.unsaved");
    ed_test_unsaved = 1;
    editor_open_with(NULL);
    ed_test_unsaved = 0;
}
static void editor_launch(void)                { editor_open_with(NULL); }
static void editor_open_path(const char* path) { editor_open_with(path); }

GUI_APP_ASSOC_ICON("Editor", editor_launch, editor_open_path,
                   "txt conf md cfg log", ICON_DOC);
