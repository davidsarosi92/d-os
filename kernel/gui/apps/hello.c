/* =============================================================================
 * hello.c — the GUI hello-world sample app (M22.2).
 *
 * Doubles as the DoD proof for the app registry (it appears in the
 * Start menu with zero compositor changes) and as the template the
 * DOCS.md GUI chapter walks through.  Deliberately not a singleton —
 * every launch opens a fresh window; state lives in the app_ctx,
 * which the window frees automatically on close.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "widget.h"
#include "kmalloc.h"
#include <stddef.h>

struct hello {
    struct w_label* lbl;
    int count;
};

static void hello_click(struct w_button* b, void* ctx) {
    (void)b;
    struct hello* h = (struct hello*)ctx;
    h->count++;
    char t[32] = "clicked     times";      /* digits at [8..10], space [11] */
    int n = h->count % 1000;
    t[8]  = (char)('0' + (n / 100) % 10);
    t[9]  = (char)('0' + (n / 10) % 10);
    t[10] = (char)('0' + n % 10);
    w_label_set(h->lbl, t);
    /* No explicit redraw needed: event dispatch repaints the window. */
}

static void hello_open(void) {
    struct hello* h = (struct hello*)kcalloc(1, sizeof(*h));
    if (!h) return;

    struct gui_window* w =
        gui_app_window_create("Hello", 520, 200, 240, 130, NULL, h);
    if (!w) { kfree(h); return; }        /* not adopted as app_ctx on failure */

    w_label_create(w, 16, 10, 200, "Hello from the registry!");
    h->lbl = w_label_create(w, 16, 32, 200, "clicked 000 times");
    w_button_create(w, 16, 60, 100, 22, "Click me", hello_click, h);
    gui_window_request_redraw(w);
}

GUI_APP("Hello", hello_open);
