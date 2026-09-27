/* =============================================================================
 * diskpanel.c — the Control Panel's Disks page (§M87).
 *
 * The window over storage.c: one row per block device, and the operations
 * that make it disk MANAGEMENT rather than a disk list — mount, unmount,
 * format, create and remove a RAM disk, write everything back.
 *
 * EVERY REFUSAL IS A SENTENCE, and it comes from storage_error() — the same
 * words `disk` prints — so the panel and the shell cannot give two different
 * reasons for one refusal.  The one that matters most is the held volume:
 * "Unmount /mnt" answers *the system depends on it (the settings store)*,
 * because a button that silently does nothing on the system disk teaches the
 * user that the button is broken.
 *
 * FORMAT ASKS FIRST, in a modal of its own, naming the disk and what happens
 * to its contents — the one irreversible action in the Control Panel.  The
 * confirm button is labelled with the VERB ("Format"), not "OK": a person
 * skimming a dialog reads the button, and "OK" says nothing about what it
 * will do.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "ui.h"
#include "console_plate.h"
#include "itemview.h"
#include "settings.h"
#include "locale.h"
#include "storage.h"
#include "ramdisk.h"
#include "printf.h"
#include <stddef.h>
#include <stdint.h>

static int put(char* o, int cap, int n, const char* s) {
    if (!s) return n;
    for (int i = 0; s[i] && n < cap - 1; i++) o[n++] = s[i];
    if (n < cap) o[n] = 0;
    return n;
}
static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int seq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

#define DK_MAX 8
static struct storage_info dk[DK_MAX];
static int dk_n;
static int dk_sel = -1;
static char dk_sel_name[8];

static struct gui_window* dk_win;
static struct w_itemview* dk_view;
static struct w_label*    dk_detail;
static struct w_label*    dk_result;

enum { DC_NAME = 0, DC_SIZE, DC_FS, DC_MOUNT, DC_FREE, DC_NOTE, DC__COUNT };

static void dk_rescan(void) {
    dk_n = storage_query(dk, DK_MAX);
    dk_sel = -1;
    for (int i = 0; i < dk_n; i++)
        if (dk_sel_name[0] && seq(dk[i].name, dk_sel_name)) dk_sel = i;
    if (dk_view) dk_view->sel = dk_sel;
}
static struct storage_info* dk_selected(void) {
    return (dk_sel >= 0 && dk_sel < dk_n) ? &dk[dk_sel] : NULL;
}

static int dk_count(void* c) { (void)c; return dk_n; }
static char dk_lbl[40];
static int dk_get(void* c, int i, struct item_entry* out) {
    (void)c;
    if (i < 0 || i >= dk_n) return -1;
    int n = put(dk_lbl, sizeof dk_lbl, 0, dk[i].name);
    n = put(dk_lbl, sizeof dk_lbl, n, " - ");
    put(dk_lbl, sizeof dk_lbl, n, dk[i].fs);
    out->label = dk_lbl;
    out->icon  = ICON_STORAGE;
    out->dim   = !dk[i].mounted;
    return 0;
}
static int dk_columns(void* c) { (void)c; return DC__COUNT; }
static const char* dk_col_title(void* c, int col) {
    (void)c;
    switch (col) {
    case DC_NAME:  return "disk.col.disk";
    case DC_SIZE:  return "disk.col.size";
    case DC_FS:    return "disk.col.fs";
    case DC_MOUNT: return "disk.col.mount";
    case DC_FREE:  return "disk.col.free";
    case DC_NOTE:  return "disk.col.note";
    }
    return "";
}
static int dk_col_weight(void* c, int col) {
    (void)c;
    return (col == DC_MOUNT || col == DC_NOTE || col == DC_FREE) ? 2 : 1;
}
static int dk_col_style(void* c, int col) {
    (void)c;
    return (col == DC_SIZE || col == DC_FREE) ? ICOL_RIGHT | ICOL_MONO : 0;
}
static int dk_cell(void* c, int i, int col, char* out, int cap) {
    (void)c;
    if (i < 0 || i >= dk_n || cap <= 0) return -1;
    struct storage_info* s = &dk[i];
    out[0] = 0;
    switch (col) {
    case DC_NAME:  put(out, cap, 0, s->name); break;
    case DC_SIZE:  storage_fmt_size(s->bytes, out, cap); break;
    /* The filesystem's name is a technical identifier ("exfat") except for
     * the two words that are not: "empty" and "unknown" — translated. */
    case DC_FS:    put(out, cap, 0, lstr(s->fs)); break;
    case DC_MOUNT: put(out, cap, 0, s->mounted ? s->mount : "-"); break;
    case DC_FREE:
        if (s->have_space) storage_fmt_size(s->free, out, cap);
        else put(out, cap, 0, "-");
        break;
    case DC_NOTE:
        if (s->hold)           put(out, cap, 0, lstr("disk.note.system"));
        else if (s->removable) put(out, cap, 0, lstr("disk.note.ram"));
        break;
    default: return -1;
    }
    return 0;
}
static const struct item_model dk_model = {
    .count = dk_count, .get = dk_get,
    .columns = dk_columns, .col_title = dk_col_title,
    .col_weight = dk_col_weight, .cell = dk_cell, .col_style = dk_col_style,
    .empty_text = "disk.empty",
};

static char dk_detail_buf[160];
static void dk_refresh_detail(void) {
    if (!dk_detail) return;
    struct storage_info* s = dk_selected();
    if (!s) { w_label_set(dk_detail, "disk.hint.select"); return; }
    int n = put(dk_detail_buf, sizeof dk_detail_buf, 0, s->name);
    n = put(dk_detail_buf, sizeof dk_detail_buf, n, ": ");
    if (s->mounted && s->have_space) {
        char used[16], tot[16];
        storage_fmt_size(s->total - s->free, used, sizeof used);
        storage_fmt_size(s->total, tot, sizeof tot);
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, used);
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, " / ");
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, tot);
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, " ");
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, lstr("disk.d.used"));
    } else if (s->mounted) {
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, lstr("disk.d.mounted"));
    } else {
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, lstr("disk.d.notmounted"));
    }
    if (s->hold) {
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, " - ");
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, lstr("disk.d.heldby"));
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, " ");
        n = put(dk_detail_buf, sizeof dk_detail_buf, n, lstr(s->hold));
    }
    (void)n;
    w_label_set(dk_detail, dk_detail_buf);
}

/* The result line: the verb that was tried and storage_error()'s sentence. */
static char dk_msg[128];
static void dk_say(const char* verb_key, int rc, const char* extra) {
    if (!dk_result) return;
    int n = put(dk_msg, sizeof dk_msg, 0, lstr(verb_key));
    n = put(dk_msg, sizeof dk_msg, n, ": ");
    n = put(dk_msg, sizeof dk_msg, n, lstr(storage_error(rc)));
    if (extra) {
        n = put(dk_msg, sizeof dk_msg, n, " (");
        n = put(dk_msg, sizeof dk_msg, n, lstr(extra));
        n = put(dk_msg, sizeof dk_msg, n, ")");
    }
    (void)n;
    w_label_set(dk_result, dk_msg);
}

static void dk_on_select(struct w_itemview* iv, int idx, void* ctx) {
    (void)iv; (void)ctx;
    dk_sel = idx;
    if (idx >= 0 && idx < dk_n) scopy(dk_sel_name, dk[idx].name, sizeof dk_sel_name);
    dk_refresh_detail();
    if (dk_win) gui_window_request_redraw(dk_win);
}

static void fmt_open(const char* dev);

enum {
    DK_ID_VIEW = 1, DK_ID_DETAIL, DK_ID_RESULT, DK_ID_ROW, DK_ID_MOUNT,
    DK_ID_UMOUNT, DK_ID_FORMAT, DK_ID_SPACER, DK_ID_NEWRAM, DK_ID_REMOVE,
    DK_ID_SYNC,
};

static void dk_event(struct gui_window* win, int id, int type, int value, void* ctx) {
    (void)win; (void)value; (void)ctx;
    if (type != UI_EV_CLICK) return;
    struct storage_info* s = dk_selected();
    int need_sel = (id == DK_ID_MOUNT || id == DK_ID_UMOUNT ||
                    id == DK_ID_FORMAT || id == DK_ID_REMOVE);
    if (need_sel && !s) { if (dk_result) w_label_set(dk_result, "disk.hint.select"); return; }
    switch (id) {
    case DK_ID_MOUNT:  dk_say("disk.v.mount", storage_mount(s->name, NULL), NULL); break;
    case DK_ID_UMOUNT: {
        const char* why = s->hold;
        int rc = storage_umount(s->name);
        dk_say("disk.v.umount", rc, rc == STOR_EHELD ? why : NULL);
        break;
    }
    case DK_ID_FORMAT:
        if (s->mounted) { dk_say("disk.v.format", STOR_EMOUNTED, NULL); break; }
        fmt_open(s->name);
        break;
    case DK_ID_NEWRAM: {
        const char* nm = ramdisk_create(16);
        if (nm) { scopy(dk_sel_name, nm, sizeof dk_sel_name); dk_say("disk.v.newram", STOR_OK, NULL); }
        else    dk_say("disk.v.newram", STOR_ENOMEM, NULL);
        break;
    }
    case DK_ID_REMOVE: dk_say("disk.v.remove", storage_remove(s->name), NULL); break;
    case DK_ID_SYNC:   dk_say("disk.v.sync", storage_sync_all(), NULL); break;
    }
    dk_rescan();
    dk_refresh_detail();
    if (dk_view) w_itemview_refresh(dk_view);
    if (dk_win) gui_window_request_redraw(dk_win);
}

static int dk_ticks;
static volatile int dk_pending;         /* a format result from the dialog */
static int dk_pending_rc;
static void dk_tick(struct gui_window* win) {
    if (__atomic_exchange_n(&dk_pending, 0, __ATOMIC_ACQ_REL))
        dk_say("disk.v.format", dk_pending_rc, NULL);
    else
    /* Free space changes as files are written, and a disk can be mounted from
     * a prompt while this is open — about once a second, like the device
     * manager. */
    if (++dk_ticks & 1) return;
    dk_rescan();
    dk_refresh_detail();
    if (dk_view) w_itemview_refresh(dk_view);
    gui_window_request_redraw(win);
}

static void dk_on_close(struct gui_window* w) {
    (void)w;
    dk_view = NULL; dk_detail = dk_result = NULL;
}

static void dk_layout(struct gui_window* win) {
    dk_view = NULL;
    dk_rescan();
    static const struct ui_spec spec[] = {
        { .id = DK_ID_VIEW,   .cls = "view", .text = "table", .weight = 1,
          .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = DK_ID_DETAIL, .cls = "label", .text = "disk.hint.select", .flags = UI_FILL_W },
        { .id = DK_ID_RESULT, .cls = "label", .text = "", .flags = UI_FILL_W },
        { .id = DK_ID_ROW,    .cls = "box",   .flags = UI_ROW | UI_FILL_W },
        { .id = DK_ID_MOUNT,  .parent = DK_ID_ROW, .cls = "button", .text = "disk.btn.mount" },
        { .id = DK_ID_UMOUNT, .parent = DK_ID_ROW, .cls = "button", .text = "disk.btn.umount" },
        { .id = DK_ID_FORMAT, .parent = DK_ID_ROW, .cls = "button", .text = "disk.btn.format" },
        { .id = DK_ID_SYNC,   .parent = DK_ID_ROW, .cls = "button", .text = "disk.btn.sync" },
        { .id = DK_ID_SPACER, .parent = DK_ID_ROW, .cls = "box", .weight = 1 },
        { .id = DK_ID_NEWRAM, .parent = DK_ID_ROW, .cls = "button", .text = "disk.btn.newram" },
        { .id = DK_ID_REMOVE, .parent = DK_ID_ROW, .cls = "button", .text = "disk.btn.remove" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), dk_event, NULL);
    dk_view   = (struct w_itemview*)ui_by_id(win, DK_ID_VIEW);
    dk_detail = (struct w_label*)ui_by_id(win, DK_ID_DETAIL);
    dk_result = (struct w_label*)ui_by_id(win, DK_ID_RESULT);
    if (dk_view) {
        w_itemview_set_model(&dk_view->base, &dk_model, NULL);
        dk_view->on_select = dk_on_select;
        dk_view->sel = dk_sel;
    }
    dk_refresh_detail();
}

static void disks_panel_open(void) {
    struct gui_app_spec sp = {
        .title = "Disks",
        .content_w = cp_px(740), .content_h = cp_px(320),
        .layout = dk_layout, .tick = dk_tick,
        .on_close = dk_on_close, .slot = &dk_win,
    };
    gui_app_open(&sp);
}

SETTINGS_PANEL(sp_disks) = {
    .name    = "Disks",
    .summary = "mount, unmount, format, RAM disks",
    .icon    = ICON_STORAGE,
    .open    = disks_panel_open,
};

/* ---------------------------------------------------------------- */
/* The format confirmation.                                          */
/* ---------------------------------------------------------------- */

static struct gui_window* fm_win;
static char fm_dev[8];
static char fm_warn[128];

enum { FM_ID_WARN = 1, FM_ID_GRID, FM_ID_L_LABEL, FM_ID_LABEL, FM_ID_MSG,
       FM_ID_ROW, FM_ID_CANCEL, FM_ID_GO };

static void fm_event(struct gui_window* w, int id, int type, int value, void* ctx) {
    (void)value; (void)ctx;
    if (type == UI_EV_SUBMIT && id == FM_ID_LABEL) { id = FM_ID_GO; type = UI_EV_CLICK; }
    if (type != UI_EV_CLICK) return;
    if (id == FM_ID_CANCEL) { gui_window_close(w); return; }
    if (id != FM_ID_GO) return;
    struct w_textinput* t = (struct w_textinput*)ui_by_id(w, FM_ID_LABEL);
    int rc = storage_format(fm_dev, t ? t->buf : "");
    kprintf("disks: format %s - %s\n", fm_dev, storage_error(rc));
    if (rc == STOR_OK) {
        /* The answer belongs on the Disks page, where the user is looking once
         * this closes — but that window's widgets belong to ITS host task
         * (§M22.7), and this runs on the dialog's.  So the result is handed
         * over as DATA and the page's own tick shows it (the file manager's
         * delete dialog learned this the hard way, §M69). */
        dk_pending_rc = rc;
        __atomic_store_n(&dk_pending, 1, __ATOMIC_RELEASE);
        gui_window_close(w);
    } else {
        struct w_label* m = (struct w_label*)ui_by_id(w, FM_ID_MSG);
        if (m) w_label_set(m, storage_error(rc));
    }
}

static void fm_layout(struct gui_window* win) {
    static const struct ui_spec spec[] = {
        { .id = FM_ID_WARN,    .cls = "label", .text = "", .flags = UI_FILL_W },
        { .id = FM_ID_GRID,    .cls = "box", .flags = UI_GRID | UI_FILL_W },
        { .id = FM_ID_L_LABEL, .parent = FM_ID_GRID, .cls = "label", .text = "disk.f.label" },
        { .id = FM_ID_LABEL,   .parent = FM_ID_GRID, .cls = "textinput",
          .text = "DOS", .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = FM_ID_MSG,     .cls = "label", .text = "disk.f.hint", .flags = UI_FILL_W },
        { .id = FM_ID_ROW,     .cls = "box", .flags = UI_ROW | UI_FILL_W | UI_ALIGN_END },
        { .id = FM_ID_CANCEL,  .parent = FM_ID_ROW, .cls = "button", .text = "Cancel" },
        { .id = FM_ID_GO,      .parent = FM_ID_ROW, .cls = "button", .text = "disk.btn.formatnow" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), fm_event, NULL);
    /* "Everything on ram0 will be erased." — composed from a catalogue line
     * that ENDS where the device name goes, so the translator owns the whole
     * sentence and the name is data. */
    int n = put(fm_warn, sizeof fm_warn, 0, lstr("disk.f.warn"));
    n = put(fm_warn, sizeof fm_warn, n, " ");
    put(fm_warn, sizeof fm_warn, n, fm_dev);
    struct w_label* l = (struct w_label*)ui_by_id(win, FM_ID_WARN);
    if (l) w_label_set(l, fm_warn);
}

static void fm_open_now(void) {
    gui_app_open(&(struct gui_app_spec){
        .title = "disk.title.format",
        .content_w = cp_px(420), .content_h = cp_px(190),
        .place = GUI_PLACE_DIALOG, .modal = 1,
        .layout = fm_layout, .slot = &fm_win,
    });
}
static void fmt_open(const char* dev) {
    scopy(fm_dev, dev, sizeof fm_dev);
    gui_queue_open(fm_open_now);
}
