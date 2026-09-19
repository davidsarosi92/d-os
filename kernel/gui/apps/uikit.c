/* =============================================================================
 * uikit.c — every registered widget class, on one screen.
 *
 * WHY THIS IS IN THE TREE AND NOT A THROWAWAY.
 *
 * §M69 shipped a real typeface and left seven controls in the design's
 * catalogue undrawn.  Three of them exist now (switch, segmented, progress) and
 * the other four had never been looked at next to the reference at all — and
 * the whole lesson of the Console Plate port is that *every visual defect it
 * had was obvious in a picture and invisible in a pixel probe*: a taskbar at a
 * sixth of its height, a clock drawn twice, a button clipped at both ends.
 *
 * So this is the instrument, not a demo.  `scripts/design-compare.sh` renders
 * the design's own catalogue; this renders OURS, in the same order, so the two
 * can be put side by side and a claim about conformance becomes a check.  A
 * control that draws wrongly at one density, or that a class registered without
 * a `measure` collapses to nothing, shows up here in one screenshot instead of
 * in whichever app happens to use it first.
 *
 * IT IS A SHELL COMMAND, NOT A GUI_APP, and deliberately: eleven of twelve
 * Start-menu slots are taken (§M63 exists because that cap is real), and a test
 * harness has no business consuming the last one.  `uikit` opens it; the window
 * closes like any other.
 *
 * It is BUILT FROM THE CLASS REGISTRY where it can be — the point is to catch a
 * class nobody instantiates, and a hand-written list would only ever show the
 * classes somebody remembered to add to it.
 * ============================================================================= */

#include "gui.h"
#include "ui.h"
#include "shellcmd.h"   /* §M70 — the commands register themselves */
#include "widget.h"
#include "console_plate.h"
#include "kmalloc.h"
#include "printf.h"
#include <stddef.h>

/* ids: the events print themselves, which is what makes this drivable from the
 * serial log on a machine whose harness cannot type once a window has focus
 * (§M64's limit).  A screenshot shows that a control DREW; only an event shows
 * that it is connected to anything. */
enum {
    UK_ROOT = 1, UK_SCROLL, UK_GRID,
    UK_BTN = 10, UK_CHECK, UK_SWITCH, UK_RADIO, UK_SEG, UK_SLIDER,
    UK_COMBO, UK_TEXT, UK_PROGRESS, UK_LIST, UK_CHART,
};

static struct gui_window* uk_win = NULL;

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void uk_event(struct gui_window* win, int id, int type, int value,
                     void* ctx) {
    (void)win; (void)ctx;
    kprintf("uikit: widget id %d, type %d, value %d\n", id, type, value);
    /* The slider drives the progress bar, so the one control that takes a
     * continuous value has somewhere to show it — and the progress bar, which
     * takes no input at all, becomes falsifiable by moving something else. */
    if (id == UK_SLIDER && type == UI_EV_CHANGE) {
        struct widget* pw = ui_by_id(uk_win, UK_PROGRESS);
        const struct widget_class* c = ui_class_find("progress");
        if (pw && c && c->set_value) {
            c->set_value(pw, value);
            gui_window_request_redraw_rect(uk_win, pw->x, pw->y, pw->w, pw->h);
        }
    }
}

/* One row of the grid: a caption and the control it names.
 *
 * `_fl` carries UI_FILL_W or 0, and the DEFAULT IS 0 — rule 2 of the control
 * convention (console_plate.h).  The first version forced UI_FILL_W on every
 * row, which is precisely the habit the rule exists to stop: it made a button
 * as wide as the window, and that is what a full-width Save button in the
 * Control Panel looks like from the other side. */
#define UK_ROW(_id, _cls, _label, _text, _v, _lo, _hi, _fl)                   \
    sp[k++] = (struct ui_spec){ .id = 0, .parent = UK_GRID, .cls = "label",   \
                                .text = (_label) };                           \
    sp[k++] = (struct ui_spec){ .id = (_id), .parent = UK_GRID, .cls = (_cls),\
                                .text = (_text), .value = (_v),               \
                                .min = (_lo), .max = (_hi),                   \
                                .flags = (_fl) | UI_FOCUSABLE }

static void uk_layout(struct gui_window* win) {
    /* ui.h: a built interface is RE-LAID OUT, never rebuilt — describing it a
     * second time produces a second set of controls and an empty-looking
     * window (the §M65 note that cost a round). */
    ui_layout(win);
}

static void uikit_open(void) {
    if (uk_win) { gui_window_raise(uk_win); return; }

    struct gui_window* win =
        /* TEN ROWS AT THE DENSITY'S CONTROL HEIGHT, plus the chrome — and the
         * first version got this wrong in the way this whole file exists to
         * catch: `26 * cp_row_h()` is 1144 px at the measured 137 % density,
         * so on a 1200 px screen the window started at y=100 and ended past
         * the bottom edge.  Nothing draws outside the framebuffer, so the
         * missing half read as a broken LAYOUT rather than as a window sized
         * past the screen. */
        /* §M75 — the chart is not a control-height row: it is `cp_px(70)`
         * tall, so it gets its own term.  ADDING IT CAUGHT THE HAZARD ABOVE
         * FOR REAL — the first run with a chart row drew its header and had
         * its plot area cut off by the window's bottom edge, which is exactly
         * the "reads as a broken layout" failure the comment above describes.
         *
         * AND THE ROW COUNT HERE IS THE SAME DEFECT AS THE "10 shown" LITERAL
         * ONE SCREEN DOWN: a number kept in step with the rows by hand.  It is
         * left as a number because the layout engine cannot be asked how tall
         * the content will be until the window exists to lay it out in — but
         * it is written down as a known hazard rather than as an arrangement
         * that works, and the row count and the term below must move
         * together. */
        gui_app_open(&(struct gui_app_spec){
            .title = "Widget kit",
            .content_w = 72 * cp_fw() + 40,
            .content_h = 10 * cp_current_density()->control_h
                             + 12 * cp_fh()
                             + cp_px(70) + cp_row_h(),   /* the chart row */
            .layout = uk_layout, .slot = &uk_win,
        });
    if (!win) return;

    const int cap = 32;
    struct ui_spec* sp = (struct ui_spec*)kcalloc((size_t)cap, sizeof *sp);
    if (!sp) { gui_window_close(win); uk_win = NULL; return; }
    int k = 0;

    /* NO UI_SCROLL AROUND THE GRID — and the reason recorded here first was
     * WRONG, which is worth keeping rather than quietly replacing.
     *
     * The first version wrapped the grid in one, the gallery showed three rows
     * and then nothing, and this comment concluded "the viewport comes out
     * about 100 px tall whatever the window's height".  `ui_dump` falsified
     * that: in a 360 px window the settings panel's viewport measures 242 px,
     * exactly the leftover it should get.  It was never a sizing bug — the
     * content really is taller than the viewport, and there is NO SCROLLBAR, so
     * nothing on screen says the rest exists and no gesture in the harness's
     * repertoire went looking for it.  *An instrument that produces a confident
     * wrong explanation is worse than one that produces none*, which is why the
     * dump now prints what each node ASKED for next to what it got.
     *
     * The gallery still avoids the container, because a missing scroll
     * indicator would make the one screen that is supposed to show every
     * control show some of them. */
    sp[k++] = (struct ui_spec){ .id = UK_GRID, .cls = "box",
                                .flags = UI_GRID | UI_FILL_W };

    UK_ROW(UK_BTN, "button", "BUTTON", "Telepites", 0, 0, 0, 0);
    UK_ROW(UK_CHECK, "checkbox", "CHECKBOX", "SMP tamogatas", 1, 0, 0, 0);
    UK_ROW(UK_SWITCH, "switch", "SWITCH", "Preemptiv utemezes", 1, 0, 0, 0);
    UK_ROW(UK_RADIO, "radio", "RADIO", "ext2 FAT32 tmpfs", 0, 0, 0, 0);
    UK_ROW(UK_SEG, "segmented", "SEGMENTED", "Lista Racs Fa", 0, 0, 0, 0);
    UK_ROW(UK_SLIDER, "slider", "SLIDER", "", 60, 0, 100, UI_FILL_W);
    UK_ROW(UK_COMBO, "combo", "DROPDOWN", "CFS RR FIFO", 0, 0, 0, 0);
    UK_ROW(UK_TEXT, "textinput", "TEXT", "/mnt/data", 0, 0, 0, UI_FILL_W);
    UK_ROW(UK_PROGRESS, "progress", "PROGRESS", "", 60, 0, 100, UI_FILL_W);
    UK_ROW(UK_LIST, "listview", "LIST", "", 0, 0, 0, UI_FILL_W);
    /* §M75 — the chart joins the gallery in the same change that adds the
     * class, which is what the gallery is FOR: a registered class nobody
     * instantiates shows up here as a count that does not match, rather than
     * as a gap somebody has to notice.  Series 0 is CPU, so this row is live
     * on any machine — a demo chart fed with invented data would be the one
     * control on the screen that proves nothing. */
    UK_ROW(UK_CHART, "chart", "CHART", "", 0, 0, 0, UI_FILL_W);

    /* ui_build returns the number of specs it BUILT, not a status — an unknown
     * class is reported and skipped so a panel missing one control still opens
     * (ui.h).  So the check is "did every row arrive", and it names the
     * shortfall: this is the one instrument whose whole job is to notice a
     * control that did not appear. */
    int built = ui_build(win, sp, k, uk_event, NULL);
    if (built != k)
        kprintf("uikit: %d of %d specs built - %d control(s) MISSING\n",
                built, k, k - built);

    /* The class NAMES are string literals, so keeping the pointers is enough —
     * but they must be taken before `sp` is freed, which is the whole reason
     * this array exists rather than the report walking `sp` directly. */
    const char** sp_cls = (const char**)kcalloc((unsigned)k, sizeof(char*));
    if (sp_cls) for (int i = 0; i < k; i++) sp_cls[i] = sp[i].cls;
    kfree(sp);

    /* THE REGISTRY IS THE CHECKLIST, printed rather than assumed: if a class is
     * registered and no row above instantiates it, that is a control this
     * screenshot does NOT prove anything about, and the count is how anybody
     * notices.
     *
     * **THE "SHOWN" FIGURE USED TO BE THE LITERAL 10.**  Which means that for
     * as long as it stood there, the one instrument written to catch a class
     * nobody instantiates could not catch one: adding `chart` took the tree to
     * 14 registered and 11 shown, and this line went on saying 10.  §M52's
     * shape — a statement that was true when written, in the code whose entire
     * job is to notice statements that stopped being true.  *A count that is
     * typed is not a count.*
     *
     * It is DERIVED now, by asking each registered class whether any spec in
     * the array above named it, and the classes with no row are printed BY
     * NAME rather than left as a subtraction the reader has to do. */
    int shown = 0;
    for (int i = 0; i < ui_class_count(); i++) {
        const char* nm = ui_class_at(i)->name;
        int used = 0;
        for (int j = 0; j < k; j++)
            if (sp_cls[j] && nm && streq_(sp_cls[j], nm)) { used = 1; break; }
        if (used) shown++;
    }
    kprintf("uikit: %d widget class(es) registered, %d shown\n",
            ui_class_count(), shown);
    for (int i = 0; i < ui_class_count(); i++) {
        const char* nm = ui_class_at(i)->name;
        int used = 0;
        for (int j = 0; j < k; j++)
            if (sp_cls[j] && nm && streq_(sp_cls[j], nm)) { used = 1; break; }
        kprintf("uikit:   %s%s\n", nm, used ? "" : "   (NO ROW - unproven)");
    }

    kfree(sp_cls);

    ui_layout(win);
}

void uikit_command(void) {
    if (!gui_is_active()) {
        kprintf("uikit: the GUI is not running (start it with 'gui')\n");
        return;
    }
    /* §M61's rule: a window must be created ON a task that runs an app-host
     * loop, so it is queued to the compositor rather than built on the shell's
     * task — one built here would never lay out and never tick. */
    gui_queue_open(uikit_open);
}

/* --- §M70 shell registration -----------------------------------------------
 * A shell command and NOT a GUI_APP on purpose: the Start menu is nearly full
 * and a test harness has no business taking its last slot. */
static void uk_uikit(const char* a) { (void)a; uikit_command(); }
SHELL_CMD(uikit) = { "uikit", "", "every registered widget class in one window",
                     SHELL_G_GUI, uk_uikit, SHELL_P_ANY };
