/* =============================================================================
 * crashapp.c — the "Crash Reports" window + the GUI REPORT sink (§M47 stage 2).
 *
 * NOT A "POPUP" — A REPORT.  The distinction is the design, not wording: a crash
 * record has exactly ONE representation, and the surface it is shown on is a
 * detail.  With no GUI it is text (`crash`, /proc/crash, the system log); with a
 * GUI it is this window, listing the same records.  Nothing is a modal alert
 * demanding an answer, and nothing exists only in the GUI — the report window is
 * a view, never the storage.
 *
 * WHY THIS FILE IS A GUI APP AND NOT PART OF crash.c
 * --------------------------------------------------
 * §M47 stage 1 built the seam on purpose: capture happens in fault context,
 * delivery happens later on an ordinary task, and a new destination is added by
 * REGISTERING A SINK — never by touching a fault path.  This file is the proof
 * that the seam works.  It is a completely ordinary GUI app (widgets, kmalloc,
 * a 1 Hz tick) plus eight lines of sink, and crash.c does not know it exists.
 *
 * Adding "notify over the network" or "append to a file" later is the same
 * shape: one more file, one more CRASH_SINK(), zero changes to the exception
 * handlers.  That was the user's requirement — the ability to arm a reporting
 * mechanism at any time, in any form.
 *
 * THE SURFACING RULE
 * ------------------
 * The sink runs from the watchdog task (crash_drain), NOT from the compositor,
 * so it must not build a window itself.  It does the one thing that is safe from
 * any task — pushes an app launch onto the compositor's queue (a lock-free ring
 * store) — and the compositor opens the window on its own schedule.  That also
 * means a wedged compositor cannot be made worse by a crash report.
 *
 * Gated by `crash.report` (default on).  Off leaves the record fully captured
 * and still visible in `crash`, /proc/crash and the system log: turning the
 * window off silences the interruption, never the recording.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "crash.h"
#include "config.h"
#include "kmalloc.h"
#include <stddef.h>
#include <stdint.h>

struct crashapp {
    struct w_listview* lv;
    struct w_label*    status;
    int                last_seen;      /* crash_count() at the last refresh */
};

static struct gui_window* ca_win = NULL;

/* ---- tiny formatting helpers (no snprintf in a freestanding build) --------- */

static int put_str(char* d, int p, int cap, const char* s) {
    for (; s && *s && p < cap - 1; s++) d[p++] = *s;
    return p;
}

static int put_u32(char* d, int p, int cap, uint32_t v) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
    while (n && p < cap - 1) d[p++] = tmp[--n];
    return p;
}

static int put_hex(char* d, int p, int cap, uintptr_t v) {
    static const char* hx = "0123456789abcdef";
    int started = 0;
    p = put_str(d, p, cap, "0x");
    for (int sh = (int)(sizeof(uintptr_t) * 8) - 4; sh >= 0; sh -= 4) {
        unsigned nib = (unsigned)((v >> sh) & 0xF);
        if (!nib && !started && sh) continue;          /* trim leading zeros */
        started = 1;
        if (p < cap - 1) d[p++] = hx[nib];
    }
    if (!started && p < cap - 1) d[p++] = '0';
    return p;
}

/* ---- refresh ---------------------------------------------------------------
 * Newest first, one line per record.  crash_at() already indexes that way, so
 * the list reads top-down as "what went wrong most recently".
 * --------------------------------------------------------------------------- */
static void ca_refresh(struct gui_window* win) {
    struct crashapp* ca = (struct crashapp*)gui_window_ctx(win);
    if (!ca || !ca->lv) return;

    int sel = ca->lv->sel;
    w_listview_clear(ca->lv);

    int n = crash_count();
    for (int i = 0; i < WLIST_MAX_ITEMS; i++) {
        const struct crash_record* r = crash_at(i);
        if (!r) break;
        char line[WLIST_ITEM_LEN];
        int p = 0;
        p = put_u32(line, p, sizeof line, (uint32_t)(r->ms / 1000));
        p = put_str(line, p, sizeof line, "s ");
        p = put_str(line, p, sizeof line, crash_kind_name(r->kind));
        p = put_str(line, p, sizeof line, " pid ");
        p = put_u32(line, p, sizeof line, (uint32_t)r->pid);
        p = put_str(line, p, sizeof line, " ");
        p = put_str(line, p, sizeof line, r->comm[0] ? r->comm : "?");
        p = put_str(line, p, sizeof line, " @");
        p = put_hex(line, p, sizeof line, r->pc);
        line[p] = 0;
        w_listview_add(ca->lv, line, (uint8_t)r->kind);
    }
    if (sel >= 0 && sel < ca->lv->count) ca->lv->sel = sel;

    char st[96];
    int p = 0;
    if (n == 0) {
        p = put_str(st, p, sizeof st, "No crashes recorded on this boot.");
    } else {
        p = put_u32(st, p, sizeof st, (uint32_t)n);
        /* ASCII only: the 8x8 bitmap font has no em dash — it renders as a
         * filled box, which reads as a corrupted string. */
        p = put_str(st, p, sizeof st,
                    n == 1 ? " crash recorded - also in `crash` and /proc/crash"
                           : " crashes recorded - also in `crash` and /proc/crash");
    }
    st[p] = 0;
    w_label_set(ca->status, st);

    ca->last_seen = n;
    gui_window_request_redraw(win);
}

/* Selecting a row shows that record's full detail — the listview line has to
 * fit 72 characters, so `what` and the faulting address live down here. */
static void ca_select(struct w_listview* lv, int idx, void* ctx) {
    (void)lv;
    struct crashapp* ca = (struct crashapp*)ctx;
    const struct crash_record* r = crash_at(idx);
    if (!ca || !r) return;
    char st[96];
    int p = 0;
    p = put_str(st, p, sizeof st, r->what[0] ? r->what : "(no detail)");
    p = put_str(st, p, sizeof st, "  addr=");
    p = put_hex(st, p, sizeof st, r->addr);
    p = put_str(st, p, sizeof st, " code=");
    p = put_u32(st, p, sizeof st, (uint32_t)r->code);
    p = put_str(st, p, sizeof st, " cpu");
    p = put_u32(st, p, sizeof st, (uint32_t)r->cpu);
    st[p] = 0;
    w_label_set(ca->status, st);
    if (ca_win) gui_window_request_redraw(ca_win);
}

/* ~1 Hz.  Only rebuilds when the count actually moved, so an open window costs
 * nothing while the system is healthy. */
static void ca_tick(struct gui_window* win) {
    struct crashapp* ca = (struct crashapp*)gui_window_ctx(win);
    if (ca && crash_count() != ca->last_seen) ca_refresh(win);
}

static void ca_btn_refresh(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    if (ca_win) ca_refresh(ca_win);
}

static void ca_layout(struct gui_window* win) {
    struct crashapp* ca = (struct crashapp*)gui_window_ctx(win);
    int w = 0, h = 0;
    if (!ca || !gui_window_content_size(win, &w, &h)) return;
    if (ca->lv) {
        ca->lv->base.w = w - 16;
        ca->lv->base.h = h - 74;
    }
    if (ca->status) { ca->status->base.y = h - 20; ca->status->base.w = w - 16; }
}

static void ca_on_close(struct gui_window* win) { (void)win; ca_win = NULL; }

static void crashapp_open(void) {
    if (ca_win) { gui_window_raise(ca_win); ca_refresh(ca_win); return; }

    struct crashapp* ca = (struct crashapp*)kcalloc(1, sizeof(*ca));
    if (!ca) return;

    struct gui_window* win =
        gui_app_window_create("Crash Reports", 260, 160, 500, 300, ca_layout, ca);
    if (!win) { kfree(ca); return; }
    ca_win = win;
    gui_window_set_on_close(win, ca_on_close);

    w_label_create(win, 8, 3, 400, "UPTIME KIND         PID  NAME  PC");
    ca->lv = w_listview_create(win, 8, 26, 484, 200, ca);
    w_button_create(win, 8, 232, 90, 18, "Refresh", ca_btn_refresh, ca);
    ca->status = w_label_create(win, 8, 258, 484, "");
    if (!ca->lv || !ca->status) { gui_window_close(win); return; }
    ca->lv->on_select = ca_select;
    ca->status->color = 0xFF8C9AAAu;
    ca->last_seen = -1;                         /* force the first refresh */

    gui_window_set_tick(win, ca_tick);
    ca_layout(win);
    ca_refresh(win);
}

GUI_APP_ICON("Crash Reports", crashapp_open, ICON_WARN);

/* ---------------------------------------------------------------------------
 * The sink.  See the file header for why this is only a queue push.
 * --------------------------------------------------------------------------- */
static void gui_report_sink(const struct crash_record* r) {
    (void)r;
    /* No GUI: the record is already in the log and in `crash` — this sink has
     * no surface to draw on and simply has nothing to add. */
    if (!gui_is_active()) return;

    /* Read the gate on every record, not once at init: the user must be able to
     * arm or silence the report window at ANY time (`config crash.report off`)
     * without a reboot — that was the explicit requirement behind §M47. */
    const char* v = config_get("crash.report", "on");
    if (v) {
        char a = v[0], b = v[1];
        if (a == '0' || a == 'n' || a == 'N' ||                  /* 0 / no      */
            ((a == 'o' || a == 'O') && (b == 'f' || b == 'F')))  /* off (not on)*/
            return;
    }

    const struct gui_app_def* app = gui_app_find("Crash Reports");
    if (app) gui_queue_launch(app);
}
CRASH_SINK("gui-report", gui_report_sink);
