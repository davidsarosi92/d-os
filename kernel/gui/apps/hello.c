/* =============================================================================
 * hello.c — the GUI hello-world sample app (M22.2).
 *
 * Doubles as the DoD proof for the app registry (it appears in the
 * Start menu with zero compositor changes) and as the template the
 * DOCS.md GUI chapter walks through.  Deliberately not a singleton —
 * every launch opens a fresh window; state lives in the app_ctx,
 * which the window frees automatically on close.
 *
 * REWRITTEN 2026-09-28 (found by scripts/locale-sweep.py): the template placed
 * its widgets at literal pixel rows measured for the 8x8 font and spliced a
 * count into an English sentence by byte offset — both habits §M65/§M69 exist
 * to retire, and a TEMPLATE is the worst place to keep them, because it is the
 * file people copy.  Now: a declared layout (ui_build), one event sink, and
 * text from the catalogue (the count is composed from a translated fragment,
 * the house rule for numbers in sentences — locale.h).
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "icons.h"
#include "widget.h"
#include "ui.h"
#include "locale.h"
#include "console_plate.h"
#include "kmalloc.h"
#include <stddef.h>

struct hello {
    int  count;
    char text[48];
};

#define HID_COUNT 2
#define HID_CLICK 3

static void hello_show(struct gui_window* win, struct hello* h) {
    /* "<clicked> N" — the fragment is translated, the number is not. */
    const char* f = lstr("hello.clicked");
    int n = 0;
    while (f[n] && n < (int)sizeof h->text - 12) { h->text[n] = f[n]; n++; }
    h->text[n++] = ' ';
    char t[12]; int k = 0; unsigned v = (unsigned)h->count;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    while (k) h->text[n++] = t[--k];
    h->text[n] = 0;
    struct w_label* l = (struct w_label*)ui_by_id(win, HID_COUNT);
    if (l) w_label_set(l, h->text);
}

static void hello_event(struct gui_window* win, int id, int type, int value, void* ctx) {
    (void)value;
    struct hello* h = (struct hello*)ctx;
    if (id == HID_CLICK && type == UI_EV_CLICK) {
        h->count++;
        hello_show(win, h);
    }
}

static void hello_layout(struct gui_window* win) {
    if (ui_node_count(win) > 0) { ui_layout(win); return; }
    struct hello* h = (struct hello*)gui_window_ctx(win);
    static const struct ui_spec spec[] = {
        { .id = 1,         .cls = "label",  .text = "Hello from the registry!", .flags = UI_FILL_W },
        { .id = HID_COUNT, .cls = "label",  .text = "",                         .flags = UI_FILL_W },
        { .id = HID_CLICK, .cls = "button", .text = "Click me" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), hello_event, h);
    if (h) hello_show(win, h);
}

static void hello_open(void) {
    struct hello* h = (struct hello*)kcalloc(1, sizeof(*h));
    if (!h) return;
    struct gui_window* w = gui_app_open(&(struct gui_app_spec){
        .title = "Hello",
        .content_w = cp_px(260), .content_h = cp_px(130), .ctx = h,
        .layout = hello_layout,
    });
    if (!w) kfree(h);                    /* not adopted as app_ctx on failure */
}

GUI_APP_ICON("Hello", hello_open, ICON_APP);
