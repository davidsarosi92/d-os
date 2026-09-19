/* =============================================================================
 * dialog.c — the modal dialog (§M69, design/widget_specs.md §14).
 *
 * The seventh and last of the design's undrawn controls, and the only one that
 * was never really a control: a button is a rectangle you can draw, while a
 * dialog is a CLAIM ON THE SESSION — the backdrop, the trapped focus, the
 * swallowed clicks.  That claim is gui.c's `gui_window_set_modal`, built for
 * this; what lives here is the panel that uses it.
 *
 * WHAT IT REPLACES IS THE ARGUMENT FOR BUILDING IT.  With nothing to ask a
 * question with, every place in this tree that needed one invented a
 * substitute: §M61's confirm-or-revert is an ordinary window anything can be
 * clicked in front of, and the file manager asks about a recursive delete by
 * requiring the Del key TWICE within eight seconds — a keyboard gesture
 * standing in for a sentence, undiscoverable and unexplained.  *A system that
 * cannot ask does not stop asking; it asks badly.*
 *
 * -----------------------------------------------------------------------------
 * TWO DELIBERATE DIVERGENCES FROM THE SPEC, both written down here rather than
 * left to be rediscovered by somebody holding our screen against the design:
 *
 *   1. THE TITLE IS THE WINDOW'S TITLE BAR, not a 22 px line inside the panel.
 *      Our windows are decorated; a title inside a title bar is the same word
 *      twice.  It also makes the X button mean "cancel" for free, which is
 *      what a dialog's close box has to mean.
 *
 *   2. THE BODY WRAPS — and this REVERSES the original decision, because its
 *      premise did not survive localisation.  It used to split only on '\n'
 *      and leave the breaks to the caller, on the argument that a wrapping
 *      engine is real work.  True of a general one; false here, where a dialog
 *      has ONE known width and wrapping is a greedy walk over spaces.  And the
 *      moment the strings came from a catalogue the caller STOPPED BEING ABLE
 *      to pick the breaks, because a translation is a different length.  *A
 *      rule that depends on the author knowing the final text cannot survive
 *      translation.*  An explicit '\n' still forces a break.
 *
 * -----------------------------------------------------------------------------
 * THE ANSWER ARRIVES EXACTLY ONCE.  A dialog can be finished four ways — OK,
 * Cancel, the X, or the compositor's Esc hatch — and three of those do not run
 * a button handler.  So the callback fires from the window's `on_close`, which
 * every route passes through, with whatever `answer` was recorded before it.
 * The default is CANCEL: if a dialog is torn down by something nobody
 * predicted, the outcome that happens is the one that does nothing.
 * ========================================================================= */

#include "gui.h"
#include "gui_app.h"
#include "dialog.h"
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "widget.h"
#include "ui.h"
#include "console_plate.h"
#include "gfx.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>

/* Local, like w_controls.c's — this tree has no libc in ring 0 and a truncating
 * copy is the only shape any of this needs. */
static void dcopy(char* dst, const char* src, int cap) {
    int i = 0;
    if (src) while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ---------------------------------------------------------------------------
 * The info row — a small class of its own (design §14's optional line).
 *
 * `sunken` fill, 1 px `line`, a 3 px `accent` bar down the left, mono 12 px
 * `muted`.  It exists as a class rather than as a label with a different
 * colour because it is the part that carries the SUBJECT of the question —
 * the path about to be deleted, the device about to be stopped — and the
 * design sets that in mono for the reason it sets every path in mono: a
 * proportional face makes `/mnt/store` and `/mnt/st0re` look alike.
 * ------------------------------------------------------------------------- */

struct w_dlginfo {
    struct widget base;
    char text[96];
};

#define DI_PAD_X   cp_px(10)
#define DI_BAR     cp_px(3)

static void dlginfo_draw(struct widget* w, struct gfx_surface* s) {
    struct w_dlginfo* d = (struct w_dlginfo*)w;
    const cp_theme* th = cp_current_theme();
    cp_plate(s, w->x, w->y, w->w, w->h, th->sunken, th->line);
    /* The accent bar is INSIDE the border, so the plate's 1 px line still
     * reads as the row's edge rather than as a seam next to the bar. */
    gfx_fill(s, w->x + 1, w->y + 1, DI_BAR, w->h - 2, th->accent);
    cp_mono_text(s, w->x + DI_BAR + DI_PAD_X,
                 w->y + (w->h - cp_fh()) / 2, d->text, th->muted);
}

static void dlginfo_measure(struct widget* w, int avail_w, int* min_w,
                            int* pref_w, int* pref_h) {
    struct w_dlginfo* d = (struct w_dlginfo*)w;
    (void)avail_w;
    int n = 0;
    while (d->text[n]) n++;
    /* Mono, so the advance is exact and a character count is the right unit
     * here — the one place in this file where it is (§M69's measuring rule is
     * about PROPORTIONAL text). */
    *pref_w = DI_BAR + 2 * DI_PAD_X + n * cp_mono_cell_w();
    *min_w  = DI_BAR + 2 * DI_PAD_X + 4 * cp_mono_cell_w();
    *pref_h = cp_ctrl_h();
}

static const struct widget_ops dlginfo_ops = {
    .draw = dlginfo_draw,
};

static struct widget* dlginfo_create_spec(struct gui_window* win,
                                          const struct ui_spec* sp) {
    struct w_dlginfo* d = (struct w_dlginfo*)kcalloc(1, sizeof *d);
    if (!d) return NULL;
    dcopy(d->text, sp->text, sizeof d->text);
    /* Not focusable: it is the SUBJECT of the question, not a control — and a
     * focus stop nobody can act on is a Tab press that appears to do nothing. */
    widget_init(&d->base, win, 0, 0, 120, cp_ctrl_h(), &dlginfo_ops, NULL, 0);
    return &d->base;
}

WIDGET_CLASS(wc_dlginfo) = {
    .ops = &dlginfo_ops,
    .name = "dlginfo", .create = dlginfo_create_spec, .measure = dlginfo_measure,
};

/* ---------------------------------------------------------------------------
 * The dialog itself.
 * ------------------------------------------------------------------------- */

/* ONE at a time, matching gui.c's single modal slot — and the state lives in
 * a file-scope struct for the same reason the slot does: two dialogs would
 * have to agree about who owns the keyboard, and there is no answer. */
#define DLG_MAX_LINES 4
#define DLG_ID_OK     1
#define DLG_ID_CANCEL 2

static struct {
    int  open;
    struct gui_window* win;
    int  answer;                        /* GUI_DIALOG_* — cancel by default  */
    void (*cb)(int answer, void* ctx);
    void* ctx;
    char title[24];
    char body[DLG_MAX_LINES][80];
    int  nlines;
    char info[96];
    char ok_text[24];
    char cancel_text[24];
} dlg;

static void dlg_finish(int answer) {
    if (!dlg.open) return;
    dlg.answer = answer;
    /* Ask the window to close rather than closing it here: this runs on the
     * dialog's own app-host task (a button handler), and `gui_window_close`
     * from inside a widget callback tears down the widget list the caller is
     * still standing in.  `want_close` is the route every other exit takes. */
    if (dlg.win) gui_window_close(dlg.win);
}

static void dlg_event(struct gui_window* win, int id, int type, int value,
                      void* ctx) {
    (void)win; (void)value; (void)ctx;
    if (type != UI_EV_CLICK) return;
    if (id == DLG_ID_OK)     dlg_finish(GUI_DIALOG_OK);
    else if (id == DLG_ID_CANCEL) dlg_finish(GUI_DIALOG_CANCEL);
}

/* Enter confirms.  Escape is NOT handled here on purpose: the compositor traps
 * it (gui.c's escape hatch) so that a dialog whose host has wedged can still
 * be dismissed — and a second copy of the binding here would be a second place
 * to keep the two in step. */
static void dlg_key(struct gui_window* win, char c) {
    (void)win;
    if (c == '\n' || c == '\r') dlg_finish(GUI_DIALOG_OK);
}

/* Fires on EVERY exit route — that is why the answer is delivered here and
 * not from the button handlers.  See the header comment. */
static void dlg_closed(struct gui_window* win) {
    (void)win;
    void (*cb)(int, void*) = dlg.cb;
    void* ctx = dlg.ctx;
    int answer = dlg.answer;
    dlg.open = 0;
    dlg.win = NULL;
    dlg.cb = NULL;
    dlg.ctx = NULL;
    /* Logged HERE, once, for every client — not in each caller's callback.
     * A dialog that has gone leaves no picture behind, so "which of the four
     * exit routes produced this answer" is unanswerable from a screenshot;
     * without the line, a client that never acts on OK and a dialog that
     * reported CANCEL look exactly alike. */
    kprintf("dialog: '%s' answered %s%s\n", dlg.title,
            answer == GUI_DIALOG_OK ? "OK" : "CANCEL",
            cb ? "" : " (no client listening)");
    if (cb) cb(answer, ctx);
}

static void dlg_layout(struct gui_window* win) {
    /* BUILD ONCE, LAYOUT MANY (ui.h): a second ui_build would add a second
     * copy of every control and the panel would come back empty. */
    if (ui_node_count(win) > 0) { ui_layout(win); return; }

    struct ui_spec sp[DLG_MAX_LINES + 6];
    int n = 0;
    /* The root column carries the design's 20 px padding and 12 px gap through
     * ui.c's own UI_PAD/UI_GAP, so nothing here restates them. */
    for (int i = 0; i < dlg.nlines; i++)
        sp[n++] = (struct ui_spec){ .cls = "label", .text = dlg.body[i],
                                    .flags = UI_FILL_W };
    if (dlg.info[0])
        sp[n++] = (struct ui_spec){ .cls = "dlginfo", .text = dlg.info,
                                    .flags = UI_FILL_W };
    /* A spacer row with a weight: it eats the leftover height so the action
     * row is pushed to the bottom of the panel whatever the body's length.
     * (UI_ALIGN_END is about the MAIN axis of the row below, not this one.) */
    sp[n++] = (struct ui_spec){ .id = 90, .cls = "box", .weight = 1,
                                .flags = UI_COL | UI_FILL_W };
    sp[n++] = (struct ui_spec){ .id = 91, .cls = "box",
                                .flags = UI_ROW | UI_FILL_W | UI_ALIGN_END };
    /* Cancel first, confirm LAST — §14 puts the confirming button on the outer
     * edge, and with UI_ALIGN_END "last" is "rightmost".  The order in this
     * array is therefore the order on screen, which is worth knowing before
     * somebody "tidies" it. */
    if (dlg.cancel_text[0])
        sp[n++] = (struct ui_spec){ .id = DLG_ID_CANCEL, .parent = 91,
                                    .cls = "button", .text = dlg.cancel_text,
                                    .flags = UI_FOCUSABLE };
    sp[n++] = (struct ui_spec){ .id = DLG_ID_OK, .parent = 91,
                                .cls = "button", .text = dlg.ok_text,
                                .flags = UI_FOCUSABLE };

    ui_build(win, sp, n, dlg_event, NULL);

    /* §14: the focus starts on the confirming button.  A dialog that opens
     * with nothing focused answers Tab and Enter with nothing, which reads as
     * a dialog the keyboard cannot drive at all. */
    {
        struct widget* ok = ui_by_id(win, DLG_ID_OK);
        if (ok) gui_window_focus_widget(win, ok);
    }
}

/* Built on a fresh app-host task (gui_queue_open), like every other window
 * here: `gui_app_window_create` binds the window to `task_current()`, so a
 * window built on the caller's task would have no host loop to drain its
 * events (§M61 paid for this one). */
static void dlg_build(void) {
    /* §14: panel width 380–440 design px.  400 through cp_px(), so it tracks
     * the density like everything else — a constant here would be right at one
     * resolution and wrong at the next (§M62's argument for the vector logo). */
    int cw = cp_px(400);
    int rows = dlg.nlines + (dlg.info[0] ? 1 : 0);
    int ch = 2 * cp_px(20)                       /* the column's padding      */
           + rows * cp_row_h()
           + cp_px(12) + cp_btn_h();             /* gap + the action row      */

    /* GUI_PLACE_DIALOG is "a third of the way down rather than centred": a
     * dialog sitting exactly in the middle covers whatever it is asking about.
     * That sentence used to be a comment above hand-written arithmetic in each
     * of three files; it is the intent's definition now (gui.h). */
    struct gui_window* w = gui_app_open(&(struct gui_app_spec){
        .title = dlg.title,
        .content_w = cw, .content_h = ch,
        .place = GUI_PLACE_DIALOG,
        .layout = dlg_layout, .on_close = dlg_closed,
    });
    if (!w) {
        /* No window means no way to ask, so the answer is the safe one and it
         * is delivered immediately — a caller left waiting for a callback that
         * will never arrive is a hung feature, not a failed dialog. */
        kprintf("dialog: no window available — answering CANCEL\n");
        dlg_closed(NULL);
        return;
    }
    dlg.win = w;
    gui_window_set_key_hook(w, dlg_key);
    if (gui_window_set_modal(w, 1) != 0) {
        /* Refused: somebody else holds the claim.  Close what we just made and
         * answer CANCEL rather than leaving a non-modal look-alike on screen
         * that behaves nothing like a dialog. */
        gui_window_close(w);
        return;
    }
}

static void copy_into(char* dst, int cap, const char* src, const char* fallback) {
    dcopy(dst, (src && src[0]) ? src : fallback, cap);
}

int gui_dialog_open(const struct gui_dialog_req* req) {
    if (!req) return -1;
    if (dlg.open) {
        kprintf("dialog: refused — one is already open ('%s')\n", dlg.title);
        return -1;
    }
    if (!gui_is_active()) {
        kprintf("dialog: refused — no GUI session\n");
        return -1;
    }

    /* Cleared field by field rather than with a memset: this is ring 0 and the
     * tree has no libc here, which the linker says the first time you forget. */
    dlg.win = NULL; dlg.cb = NULL; dlg.ctx = NULL;
    dlg.nlines = 0;
    dlg.title[0] = dlg.info[0] = dlg.ok_text[0] = dlg.cancel_text[0] = 0;
    for (int i = 0; i < DLG_MAX_LINES; i++) dlg.body[i][0] = 0;
    dlg.open   = 1;
    dlg.answer = GUI_DIALOG_CANCEL;         /* the safe default; see header  */
    dlg.cb     = req->on_answer;
    dlg.ctx    = req->ctx;
    copy_into(dlg.title, sizeof dlg.title, req->title, "Confirm");
    copy_into(dlg.ok_text, sizeof dlg.ok_text, req->ok_text, "OK");
    /* An empty cancel label is how a caller asks for a one-button dialog — a
     * notice rather than a question.  Escape and the X still cancel it, so
     * "acknowledge" and "dismiss" remain distinguishable. */
    if (req->cancel_text)
        copy_into(dlg.cancel_text, sizeof dlg.cancel_text, req->cancel_text, "");
    else
        copy_into(dlg.cancel_text, sizeof dlg.cancel_text, "Cancel", "Cancel");
    if (req->info)
        copy_into(dlg.info, sizeof dlg.info, req->info, "");

    /* Split on '\n', THEN wrap each line to the panel.
     *
     * §M69 — the header used to say the body does not wrap and the caller
     * picks the breaks.  That was right about a general text engine and wrong
     * about this: a dialog has ONE known width, so wrapping it is a greedy
     * walk over spaces, not a layout system — and the moment the strings came
     * from a catalogue the caller stopped being able to pick the breaks,
     * because a translation is a different length.  *A rule that depends on
     * the author knowing the final text cannot survive translation.*
     *
     * Explicit '\n' still forces a break. */
    {
        const int avail = cp_px(400) - 2 * cp_px(20);
        const char* p = req->body ? req->body : "";
        while (*p && dlg.nlines < DLG_MAX_LINES) {
            char* out = dlg.body[dlg.nlines];
            int i = 0, last_space = -1;
            while (*p && *p != '\n' && i < (int)sizeof dlg.body[0] - 1) {
                if (*p == ' ') last_space = i;
                out[i] = *p;
                out[i + 1] = 0;
                if (cp_text_w(out) > avail && last_space > 0) {
                    /* Back up to the last space: the word that overflowed
                     * belongs to the NEXT line, and breaking mid-word is the
                     * one thing a reader notices immediately. */
                    p -= (i - last_space);
                    i = last_space;
                    out[i] = 0;
                    break;
                }
                i++;
                p++;
            }
            out[i] = 0;
            dlg.nlines++;
            while (*p == ' ') p++;              /* no leading space next line */
            if (*p == '\n') p++;
        }
        if (dlg.nlines == 0) dlg.nlines = 1;    /* never a bodyless panel    */
    }

    gui_queue_open(dlg_build);
    return 0;
}

int gui_dialog_active(void) { return dlg.open; }

/* ---------------------------------------------------------------------------
 * `dialog` — the shell's way in, and the only way this can be TESTED here.
 *
 * The harness cannot type once a GUI window holds the focus (§4.74), and a
 * modal dialog holds it by definition — so a dialog raised from a menu could
 * be opened and then never answered by anything automated.  Raised from the
 * shell BEFORE any window has focus, the whole path is drivable: open it,
 * screenshot the backdrop, drive the mouse onto a button, and read the answer
 * off the serial log.
 *
 * It also answers the question a screenshot cannot: the log line says which of
 * the four exit routes produced the answer, and *a picture of a dialog that
 * has gone cannot tell OK from Escape.*
 * ------------------------------------------------------------------------- */
static void dlg_demo_answer(int answer, void* ctx) {
    (void)ctx;
    /* Distinct from dlg_closed's own line on purpose: that one proves the
     * DIALOG produced an answer, this one proves a CLIENT was handed it.  The
     * two failed separately once already (§M69's file-manager path answered
     * OK and the client's effect never reached the screen). */
    kprintf("dialog: demo client received %s\n",
            answer == GUI_DIALOG_OK ? "OK" : "CANCEL");
}

void dialog_command(const char* arg) {
    if (!gui_is_active()) {
        kprintf("dialog: the GUI is not running (start it with 'gui')\n");
        return;
    }
    struct gui_dialog_req r = {
        .title = "Delete file",
        /* Deliberately ONE long line with no newline in it: the demo exists
         * to exercise the wrap, and a body the author has already broken
         * proves nothing about what happens to a translated one. */
        .body  = "This file will be removed from the volume and it cannot be "
                 "recovered afterwards by any means.",
        .info  = (arg && arg[0]) ? arg : "/mnt/store/example.txt",
        .ok_text = "Delete",
        .cancel_text = "Cancel",
        .on_answer = dlg_demo_answer,
    };
    if (gui_dialog_open(&r) == 0)
        kprintf("dialog: open — Enter confirms, Esc cancels, clicks elsewhere "
                "are swallowed\n");
}

/* --- §M70 shell registration ----------------------------------------------- */
SHELL_CMD(dialog) = { "dialog", "[demo]", "a modal dialog, and the modality behind it",
                      SHELL_G_GUI, dialog_command, SHELL_P_ANY };
