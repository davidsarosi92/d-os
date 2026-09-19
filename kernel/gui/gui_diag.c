/* =============================================================================
 * gui_diag.c — how this compositor is MEASURED (§M70).
 *
 * Extracted from gui.c, and it is a coherent file rather than a leftovers bin:
 * every §M69 performance claim in DOCS came out of exactly these instruments,
 * and more than one of them was BUILT because reasoning had already produced a
 * confident wrong answer.
 *
 * `gui.stats_ms` EXISTS BECAUSE THE OBVIOUS ROUTE IS BLOCKED.  `gui stats` has
 * to be TYPED, and this project's harness cannot type once a GUI window holds
 * focus (§4.74) — *reaching the state to be measured destroyed the means of
 * measuring it.*  It reports AREA as well as time, because a slow frame is
 * either big or fixed-cost and those want opposite fixes; without the pixel
 * count the two are indistinguishable, which is how two attempts were each
 * aimed at a guess.
 *
 * `wheeltest <x> <y> <dz>` INJECTS A NOTCH AT A CONTENT COORDINATE, because
 * *"the wheel works on the scrollbar and not on the content" is a claim about
 * two POSITIONS, and a test that cannot name a position cannot check it.*
 * `dz == 0` is PROBE mode: it reports what is under the point and who would
 * take a notch there WITHOUT moving anything, so a sweep is a MAP — injecting
 * real notches to sample several points changes the layout between samples,
 * and the second sample then describes something the first did not.  It also
 * caught a patch of mine that compiled and did nothing, which is
 * indistinguishable from a wrong theory until the path is actually run.
 *
 * `gui bench` COUNTS BOTH PATHS ON PURPOSE.  `40 copied, 0 repainted` would
 * mean the fallback is never exercised and therefore never tested; a real drag
 * gives `37 copied, 3 repainted`.
 *
 * THE INSTRUMENT ITSELF IS NOT EXEMPT.  §4.85.8: the cached
 * `gui_input_debug()` accessor called ITSELF — unbounded recursion, 16 bytes
 * of stack a frame, on the first BUTTON transition, so pure motion never
 * reached it and the machine died on the first click with `eip == cr2`.  *A
 * bisect over FEATURES cannot find a bug in the thing doing the measuring*,
 * and every experiment that day still ran through the broken accessor.  Hence
 * the CONFIG_WATCH that drops the cache: a debug switch whose silent failure
 * costs a whole round of diagnosis is the worst one to leave stale.
 * =========================================================================== */

#include "gui_priv.h"
#include "audit.h"   /* §M81 — the widget-contract check */
#include "gui.h"
#include "gui_internal.h"
#include "gui_app.h"
#include "desktop.h"
#include "widget.h"
#include "ui.h"
#include "gfx.h"
#include "console_plate.h"
#include "task.h"
#include "timer.h"
#include "printf.h"
#include "config.h"
#include "kmalloc.h"
#include <stdint.h>
#include <stddef.h>
#include "console.h"

/* `gui.input_debug`, CACHED.  Both probes ask on the path a mouse packet takes
 * — one of them from the mouse driver — and `config_get_long` walks the store
 * comparing strings, which is nothing next to a compose and real work at
 * packet rate.  -1 = not read yet.
 *
 * THE BUG THIS LINE HELD FOR ONE SESSION, written down because the shape is
 * worth more than the typo: the cached read said
 *
 *     if (g_input_dbg < 0) g_input_dbg = (int)gui_input_debug();
 *
 * i.e. the accessor called ITSELF.  Unbounded recursion, 16 bytes of stack a
 * frame, on the FIRST call — and the first call is a BUTTON TRANSITION, because
 * `&&` short-circuits and both probe sites test something cheaper first.  So
 * pure motion never reached it and the machine died on the first CLICK, with a
 * smashed kernel stack: `EXCEPTION 14 ... eip == cr2` at a garbage address, i.e.
 * execution returning through whatever the overflow had written.
 *
 * IT WAS THE INSTRUMENT, WHICH IS WHY THE BISECT KEPT CLEARING EVERYTHING.  I
 * disabled the ring coalescing, the IRQ-side probe, the stale-latch sweep and
 * the measure/arrange split in turn, and the fault survived all four — because
 * none of them is this function, and every one of those experiments still went
 * through it.  *A bisect over features cannot find a bug in the thing doing the
 * measuring*, and the conclusion it produced ("older than this session") was
 * confident and wrong. */
static int g_input_dbg = -1;

/* Filled by gui_relayout_test, printed by the compositor — see the note at the
 * end of that function for why the two are not the same task. */
static volatile int g_relay_report, g_relay_wins, g_relay_before,
                    g_relay_after, g_relay_rounds;

/* Same trick, same reason: the shell's kprintf goes to its VC while the GUI is
 * up, so the compositor says the number. */
static volatile int      g_bench_report, g_bench_w, g_bench_h;

static volatile uint64_t g_bench_ns;

static volatile uint32_t g_bench_fr;

int gui_input_debug(void) {
    if (g_input_dbg < 0) g_input_dbg = (int)config_get_long("gui.input_debug", 0);
    return g_input_dbg;
}
void gui_input_debug_refresh(void) { g_input_dbg = -1; }
static void gui_input_debug_watch(const char* k, const char* v) {
    (void)k; (void)v;
    gui_input_debug_refresh();
}
void gui_get_input_stats(unsigned* dropped, unsigned* coalesced) {
    if (dropped)   *dropped = aq_dropped + evq_dropped;
    if (coalesced) *coalesced = aq_coalesced + evq_coalesced;
}
/* The public half: an app whose `on_layout` BUILDS widgets calls this at the
 * top of it, so the rebuild replaces the old set instead of stacking on it.
 *
 * Explicit rather than automatic, because `on_layout` means two things in this
 * tree — build, or merely reposition — and clearing the list before a
 * repositioning one would hand it freed pointers to write coordinates into. */
/* Per-window widget counts.
 *
 * THIS IS THE TEST FOR THE RE-LAYOUT BUG, and it has to be a COUNT because the
 * defect is invisible in a screenshot: a duplicated widget list draws the new
 * widget over the old one, so the window looks almost right — the only visible
 * trace is a strip of stale pixels where the geometry changed and a region that
 * answers clicks.  §M64 made the same argument for `shortcut check`: a view
 * that draws correctly and hit-tests wrongly cannot be photographed. */
void gui_widget_report(void) {
    int total = 0, wins = 0;
    kprintf("gui widgets:\n");
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used || win->kind != WIN_APP) continue;
        int n = 0;
        for (struct widget* w = win->widgets; w; w = w->next) n++;
        kprintf("  '%s'  %dx%d  %d widget(s)\n", win->title, win->w, win->h, n);
        total += n;
        wins++;
    }
    if (!wins) kprintf("  (no app windows open)\n");
    else kprintf("  %d window(s), %d widget(s) total\n", wins, total);
}
void gui_relayout_all(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].used || windows[i].kind != WIN_APP) continue;
        windows[i].layout_pending = 1;
        n++;
    }
    need_frame = 1;
    kprintf("gui: re-layout requested for %d window(s)\n", n);
}
/* MEASURE THE REPORTED CASE, rather than reason about it.
 *
 * Reported from use: *"when a window is full size, the whole thing lags."*  The
 * compositor is software, and §4.61 already measured its fill rate — so the
 * question is not whether a full-screen window is expensive but WHERE the
 * pixels go, and a number is the only thing that answers that.
 *
 * Opens a window covering the whole screen (which is the case being reported),
 * forces N full-screen composites, and reports the per-frame cost.  Self-
 * contained for the same reason `gui relaytest` is: once a GUI window has focus
 * the harness cannot type a second command. */
static void bench_open(void) {
    int w = backsurf.w, h = backsurf.h;
    struct gui_window* win =
        gui_app_window_create("Bench", 0, 0, w, h, NULL, NULL);
    (void)win;
}
/* §M81 — THE FALSIFIER FOR THE HOSTING-TASK CHECK.
 *
 * `gui_app_open` warns when a window is built on a task with no app-host loop,
 * because such a window never lays out and never ticks and there is nothing on
 * screen to say so.  §M71's first rule applies: *a check must be able to FAIL*,
 * and a warning nobody has ever seen fire is a warning nobody can rely on.
 *
 * So this makes the mistake ON PURPOSE, from whatever task typed the command —
 * a shell, which is exactly the wrong place — and the proof is two facts
 * together: the warning names that task, and the window it produces is EMPTY,
 * because its layout hook is never called.  A window that merely looked odd
 * would not distinguish "the check fired" from "the check is noise". */
static void hosttest_layout(struct gui_window* win) {
    gui_window_clear_widgets(win);
    w_label_create(win, 8, 8, 200, "if you can read this, it laid out");
    kprintf("hosttest: layout RAN — this window has a host after all\n");
}

void gui_host_test(void) {
    if (!gui_is_active()) { kprintf("hosttest: the GUI is not running\n"); return; }
    kprintf("hosttest: opening a window from '%s' — expect a warning\n",
            task_current() ? task_current()->name : "?");
    struct gui_window* w = gui_app_open(&(struct gui_app_spec){
        .title = "hosttest",
        .content_w = cp_px(240), .content_h = cp_px(80),
        .place = GUI_PLACE_DIALOG,
        .layout = hosttest_layout,
    });
    if (!w) { kprintf("hosttest: no window\n"); return; }
    /* Give a host, if there were one, every chance to run the hook before we
     * report.  There is not one, which is the point. */
    task_msleep(600);
    kprintf("hosttest: window is up, widgets = %s\n",
            w->widgets ? "PRESENT (check did not reproduce)"
                       : "NONE — it never laid out, as warned");
    gui_window_close(w);
}

/* §M81 — THE SINGLETON SLOT, DRIVEN.
 *
 * `gui_app_open`'s `slot` replaced twelve copies of a raise-or-create test and
 * twelve one-line `on_close` handlers that existed only to null a static.  The
 * claim it makes is that the pointer is filled on create, honoured on a second
 * open, and CLEARED on close — and the third is the one that fails silently:
 * a slot left pointing at a destroyed window means the panel never opens again
 * (it "raises" a corpse), which from a chair is a Start-menu entry that has
 * stopped working.
 *
 * Run on a REAL app-host task, because that is where the interesting close
 * route is: the host's graceful path calls `win_run_on_close` itself, and the
 * compositor's `destroy_window` calls it only for a window the host never
 * released.  Testing on the caller's task would exercise the easier half. */
static struct gui_window* slot_win;
static int slot_layouts;

static void slottest_layout(struct gui_window* win) {
    gui_window_clear_widgets(win);
    w_label_create(win, 8, 8, 200, "slottest");
    slot_layouts++;
}

/* THE OPEN HALF RUNS ON THE HOST; THE OBSERVING HALF MUST NOT — and the first
 * version of this test got that wrong, which is worth keeping.
 *
 * `app_host_main` calls the open fn and THEN enters its loop, so a test that
 * opened, closed and polled all inside the open fn was waiting, on the host
 * task, for the host loop to process the close.  It never could: the loop is
 * the code the polling had blocked.  It reported `slot STILL SET` and
 * `layouts ran: 0` — a confident, wrong bug report about the product.
 *
 * *An observer running inside the thing it observes cannot see it work* —
 * §M55's poller-inside-the-poll in a new costume. */
static void slottest_open(void) {
    slot_layouts = 0;
    gui_app_open(&(struct gui_app_spec){
        .title = "slottest",
        .content_w = cp_px(220), .content_h = cp_px(70),
        .place = GUI_PLACE_DIALOG,
        .layout = slottest_layout, .slot = &slot_win,
    });
    /* The SECOND open, here, is the raise-or-create half: same task, same spec,
     * and it must hand back the window just built rather than a second one. */
    struct gui_window* first = slot_win;
    struct gui_window* again = gui_app_open(&(struct gui_app_spec){
        .title = "slottest",
        .content_w = cp_px(220), .content_h = cp_px(70),
        .place = GUI_PLACE_DIALOG,
        .layout = slottest_layout, .slot = &slot_win,
    });
    kprintf("slottest: create   -> slot %s\n",
            first ? "FILLED" : "EMPTY (the create did not take)");
    kprintf("slottest: reopen   -> %s\n",
            (again && again == first) ? "same window RAISED (no second built)"
                                      : "A SECOND WINDOW — slot not honoured");
}

void gui_slot_test(void) {
    if (!gui_is_active()) { kprintf("slottest: the GUI is not running\n"); return; }
    slot_win = NULL;
    gui_queue_open(slottest_open);      /* a real app-host, see above */

    /* Wait for the window to exist, then close it FROM HERE — the shell task —
     * so the host loop is free to run its graceful close path. */
    for (int i = 0; i < 100 && !slot_win; i++) task_msleep(20);
    if (!slot_win) { kprintf("slottest: no window appeared\n"); return; }
    kprintf("slottest: laid out -> %s\n",
            slot_layouts ? "yes (it has a host)" : "NO — no host loop ran it");

    gui_window_close(slot_win);
    /* Poll rather than sleep a fixed time: a fixed wait that happened to be
     * long enough here would prove nothing about the next machine. */
    for (int i = 0; i < 100 && slot_win; i++) task_msleep(20);
    kprintf("slottest: close    -> slot %s\n",
            slot_win ? "STILL SET — a later open would raise a dead window"
                     : "CLEARED");
}

/* =============================================================================
 * §M81 — `audit widget-contract`: every class implements what widget.h says it
 * must, and every live widget came through the shared initialiser.
 *
 * The contract itself is in widget.h.  This is the half that can FAIL, because
 * a contract stated only in a comment is the §M52 shape — and this tree has the
 * receipt one layer down: §M65 exported `widget_init` precisely so constructors
 * would stop hand-rolling the base fields, and TWO of them went on doing it for
 * three milestones.  Nothing was checking.
 *
 * TWO HALVES, because neither alone covers the toolkit:
 *
 *   the REGISTRY, statically.  Reaches a class nobody has instantiated, which
 *     an instance-only check never could — `uikit` already reports two
 *     registered classes with no row in its gallery, i.e. exactly those.
 *   the LIVE WINDOWS, by walking them.  Reaches what the registry cannot: a
 *     widget built by a hand-rolled constructor is in no class's table at all,
 *     which is how `w_itemview_create` stayed outside the rule.
 *
 * AUDIT_SKIP when the GUI is not running — there are no windows to walk, and
 * reporting a pass for a check that only half ran is rule 3's failure mode.
 * The registry half still runs and is reported, so "the GUI is down" does not
 * hide a class with no draw op.
 * ========================================================================= */
static int au_widget_contract(int verbose) {
    int bad = 0, n = ui_class_count();

    for (int i = 0; i < n; i++) {
        const struct widget_class* c = ui_class_at(i);
        if (!c) continue;
        if (!c->create) {
            kprintf("  class '%s' has no create fn — ui_build cannot make one\n",
                    c->name ? c->name : "(unnamed)");
            bad++;
            continue;
        }
        if (!c->ops) {
            /* Not pedantry: without the declaration this audit cannot see the
             * class's ops at all unless somebody instantiates it, so an
             * undeclared class is a hole in the check rather than a style
             * lapse. */
            kprintf("  class '%s' declares no ops table — its contract is "
                    "UNCHECKABLE\n", c->name);
            bad++;
            continue;
        }
        if (!c->ops->draw) {
            kprintf("  class '%s' has no draw op — it is laid out, given space "
                    "and hit-tested, and paints nothing\n", c->name);
            bad++;
        }
    }

    /* THE LIVE HALF IS A COUNTER, NOT A WALK — and the first version of this
     * function got that wrong in a way worth writing down.
     *
     * It walked every open window's widget list to look for widgets that had
     * skipped `widget_init` and for focusable widgets with no key handler.  That
     * walk runs on whatever task calls the audit — cron, usually — while the
     * OWNING host rebuilds the whole list at every layout (`app_widgets_reset`
     * frees each node).  *That is precisely the §M22.7 violation the
     * neighbouring audit checks for, committed by the checker.*  A check that
     * has to break a rule in order to observe it is not a check.
     *
     * Both facts are now recorded where they HAPPEN, by the code that is
     * already holding them: `widget_init` sees `focusable` and the ops table,
     * and `gui_window_add_widget` sees an unstamped widget arriving.  No walk,
     * no race, and the observation is exact rather than a sample of whatever
     * happened to be open when the audit ran.
     *
     * (A one-off page fault on `cron` was what sent me looking here.  It did
     * NOT reproduce in six further runs and is NOT root-caused — said plainly.
     * The walk was wrong on its own terms, which is why it went whether or not
     * it was the cause.) */
    if (widget_uninited) {
        kprintf("  %u widget(s) were added to a window WITHOUT widget_init — "
                "hand-rolled constructors (§M63)\n", widget_uninited);
        bad++;
    }
    if (widget_focus_traps) {
        kprintf("  %u focusable widget(s) were built with no key handler — Tab "
                "lands on them and the keyboard stops\n", widget_focus_traps);
        bad++;
    }
    if (verbose)
        kprintf("  %d class(es) checked; %u hand-rolled, %u focus trap(s) "
                "observed since boot\n", n, widget_uninited, widget_focus_traps);
    return bad;
}

AUDIT(widget_contract) = {
    "widget-contract",
    "every widget class implements its mandatory ops, and every live widget "
    "came through widget_init",
    au_widget_contract
};

/* THE FALSIFIER (§M71 rule 1).  Two violations, one of each kind, built into a
 * real window so the audit meets them exactly where it would meet the real
 * thing: a widget assembled WITHOUT `widget_init` (the §M63 constructor bug),
 * and a focusable widget with no key handler (the focus trap).
 *
 * Both are torn down again by closing the window, so the machine is left as it
 * was — an audit falsifier that leaves the box dirty is one nobody will run
 * twice. */
static void ct_draw(struct widget* w, struct gfx_surface* s) {
    cp_text(s, w->x + 4, w->y + 4, "contracttest", cp_current_theme()->text);
}
static const struct widget_ops ct_bad_ops = { .draw = ct_draw };   /* no keys */

static struct gui_window* ct_win;

static void ct_build(struct gui_window* win) {
    gui_window_clear_widgets(win);

    /* (1) FOCUSABLE AND DEAF — through widget_init, so only the second rule is
     * broken and the audit's two findings stay distinguishable. */
    struct widget* deaf = (struct widget*)kcalloc(1, sizeof *deaf);
    if (deaf) widget_init(deaf, win, 4, 4, 120, cp_ctrl_h(), &ct_bad_ops, NULL, 1);

    /* (2) NEVER INITIALISED — the hand-rolled constructor, assembled exactly
     * the way `w_itemview_create` used to be. */
    struct widget* raw = (struct widget*)kcalloc(1, sizeof *raw);
    if (raw) {
        raw->x = 4; raw->y = 4 + 2 * cp_ctrl_h();
        raw->w = 120; raw->h = cp_ctrl_h();
        raw->ops = &ct_bad_ops;
        raw->win = win;
        gui_window_add_widget(win, raw);        /* …and no widget_init */
    }
}

/* THE WINDOW IS OPENED ON A REAL APP-HOST, and the first version of this test
 * was not — it called `gui_app_open` straight from the shell task, so
 * `ct_build` (the LAYOUT hook) never ran, the bad widgets were never built, and
 * the falsifier reported *"NOT DETECTED (the check is broken)"* about a check
 * that was fine.
 *
 * §M81 step 2's hosting warning named the cause in the same log — `'contracttest'
 * is being created on 'shell' (pid 20), which runs no app-host loop` — which is
 * the first time one of these instruments caught another.  Same lesson as
 * `slottest`'s first version, one layer over: *the half that builds runs on the
 * host, the half that observes must not.* */
static void ct_open(void) {
    gui_app_open(&(struct gui_app_spec){
        .title = "contracttest",
        .content_w = cp_px(200), .content_h = cp_px(90),
        .place = GUI_PLACE_DIALOG,
        .layout = ct_build, .slot = &ct_win,
    });
}

void gui_contract_test(void) {
    if (!gui_is_active()) { kprintf("contracttest: the GUI is not running\n"); return; }
    ct_win = NULL;
    kprintf("contracttest: a clean machine first —\n");
    int before = audit_run_one("widget-contract", 0);
    kprintf("contracttest: %d violation(s) before\n", before);

    gui_queue_open(ct_open);
    for (int i = 0; i < 100 && !ct_win; i++) task_msleep(20);
    if (!ct_win) { kprintf("contracttest: no window appeared\n"); return; }
    task_msleep(400);                           /* let its host lay it out */

    int during = audit_run_one("widget-contract", 0);
    kprintf("contracttest: %d violation(s) with the bad widgets up — %s\n",
            during, during > before ? "DETECTED"
                                    : "NOT DETECTED (the check is broken)");

    gui_window_close(ct_win);
    for (int i = 0; i < 100 && ct_win; i++) task_msleep(20);
    /* THE COUNT DOES NOT GO BACK DOWN, and that is the design rather than a
     * leak.  Both facts are RECORDED WHERE THEY HAPPEN now, so the audit
     * reports what this boot has seen — not what happens to be on screen when
     * somebody asks.  A live sample would answer "clean" for a window that had
     * already closed, which is how a defect gets closed as unreproducible. */
    kprintf("contracttest: %d violation(s) after closing the window — the "
            "counters are a RECORD of this boot, not a live state\n",
            audit_run_one("widget-contract", 0));
}

/* §M81 — `audit widget-threading`: §M22.7's ownership rule, checked.
 *
 * *A window's widgets belong to the task that hosts it.*  That sentence is in
 * three headers and was checked nowhere, and §M69 paid for it: the delete
 * dialog's answer arrived on the DIALOG's host and called the file manager's
 * `fm_refresh()` directly — the model really was reloaded and NOTHING ON SCREEN
 * CHANGED, because damaging a window is the host's job.  *A cross-task write
 * that appears to do nothing is the most expensive kind: it looks like a
 * missing feature, so the fix gets aimed at the wrong layer.*
 *
 * WHAT IS OBSERVED, not believed: `widget_init` is the one route a widget is
 * born through, and it compares the creating task against the window's host at
 * that moment.  The audit reports the count and the last pair of names — the
 * count because a violation's symptom appears arbitrarily later and elsewhere,
 * the names because "some task did this to some window" is not a lead.
 *
 * NEVER SKIP.  Unlike the contract check this needs no windows and no GUI: the
 * counter is zero on a machine that has never drawn anything, and that is a
 * real "checked, clean" rather than a "could not look". */
static int au_widget_threading(int verbose) {
    const char *from = "(none)", *host = "(none)";
    widget_threading_last(&from, &host);
    if (widget_cross_task) {
        kprintf("  %u widget(s) created off their window's host task; the last "
                "was made on '%s' for a window hosted by '%s'\n",
                widget_cross_task, from, host);
        /* One violation, however many times it happened: the count is the
         * evidence and a per-occurrence tally would make a busy app look
         * catastrophic next to a rare one. */
        return 1;
    }
    if (verbose)
        kprintf("  every widget so far was created on its window's host task\n");
    return AUDIT_OK;
}

AUDIT(widget_threading) = {
    "widget-threading",
    "every widget is created on the task that hosts its window (§M22.7)",
    au_widget_threading
};

/* THE FALSIFIER.  Two tasks are needed to break this rule, so the test opens a
 * window on a real app-host and then builds a widget into it FROM THE CALLING
 * TASK — which is exactly §M69's dialog, reduced to its two lines.
 *
 * The widget is deliberately left where it lands: tearing it down would need
 * the host's cooperation, which is the whole point of the rule.  Closing the
 * window disposes of it on the ordinary path. */
static struct gui_window* th_win;

static void th_layout(struct gui_window* win) {
    gui_window_clear_widgets(win);      /* on the host — legitimate */
    w_label_create(win, 4, 4, 200, "threadtest");
}

static void th_open(void) {
    gui_app_open(&(struct gui_app_spec){
        .title = "threadtest",
        .content_w = cp_px(200), .content_h = cp_px(70),
        .place = GUI_PLACE_DIALOG,
        .layout = th_layout, .slot = &th_win,
    });
}

void gui_thread_test(void) {
    if (!gui_is_active()) { kprintf("threadtest: the GUI is not running\n"); return; }
    th_win = NULL;
    kprintf("threadtest: %d violation(s) before\n",
            audit_run_one("widget-threading", 0));

    gui_queue_open(th_open);            /* the window gets a REAL host */
    for (int i = 0; i < 100 && !th_win; i++) task_msleep(20);
    if (!th_win) { kprintf("threadtest: no window appeared\n"); return; }
    task_msleep(300);

    /* THE VIOLATION: this is the shell task, and that window is hosted by an
     * app-host.  §M69's dialog did precisely this and the screen did not move. */
    w_label_create(th_win, 4, cp_px(40), 200, "made on the wrong task");

    int during = audit_run_one("widget-threading", 0);
    kprintf("threadtest: %d violation(s) after the cross-task create — %s\n",
            during, during > 0 ? "DETECTED"
                               : "NOT DETECTED (the check is broken)");
    gui_window_close(th_win);
    for (int i = 0; i < 100 && th_win; i++) task_msleep(20);
    kprintf("threadtest: the counter does not reset — it is a record of what "
            "happened, not a live state\n");
}

void gui_compose_bench(int frames) {
    if (frames <= 0) frames = 30;
    g_occlude = (int)config_get_long("gui.occlude", 1);
    occluded_rects = painted_rects = 0;
    int had = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].kind == WIN_APP) had = 1;
    if (!had) { gui_queue_open(bench_open); task_msleep(1200); }

    uint64_t ns0 = total_compose_ns;
    uint32_t f0  = frames_full + frames_partial;
    for (int i = 0; i < frames; i++) {
        gui_damage_all();
        need_frame = 1;
        /* Let the compositor actually run: this task is not it, and a loop that
         * only queued damage would measure nothing but its own speed. */
        task_msleep(40);
    }
    uint64_t ns = total_compose_ns - ns0;
    uint32_t fr = (frames_full + frames_partial) - f0;

    g_bench_ns = ns;
    g_bench_fr = fr;
    g_bench_w  = backsurf.w;
    g_bench_h  = backsurf.h;
    g_bench_report = 1;
    need_frame = 1;
}
/* The whole regression test, in ONE command.
 *
 * It has to be one command because of a harness limit this project has hit
 * before: once a GUI window takes focus the shell can no longer be typed at, so
 * "open a panel" and "then count its widgets" cannot be two commands here.  The
 * same constraint that made §M64's Send-to-desktop row unverifiable.
 *
 * Counts, re-layouts N times, counts again.  A stable total is the pass; a
 * total that grows by the same amount each round is exactly the bug — one extra
 * set of widgets per resize. */
void gui_relayout_test(int rounds) {
    if (rounds <= 0) rounds = 3;
    int before = 0, after = 0, wins = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].used || windows[i].kind != WIN_APP) continue;
        wins++;
        for (struct widget* w = windows[i].widgets; w; w = w->next) before++;
    }
    if (!wins) {
        /* OPEN ONE OURSELVES rather than telling the user to.  `launch` as a
         * separate command cannot work here: it gives the new window focus, and
         * from that moment the harness cannot type the test.  A regression test
         * that only a human can run is one that stops being run. */
        const struct gui_app_def* app = gui_app_find("Control Panel");
        if (!app) { kprintf("relayout: no app to test with\n"); return; }
        gui_queue_launch(app);
        task_msleep(1200);
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            if (!windows[i].used || windows[i].kind != WIN_APP) continue;
            wins++;
            for (struct widget* w = windows[i].widgets; w; w = w->next) before++;
        }
        if (!wins) { kprintf("relayout: the test window did not open\n"); return; }
    }
    for (int r = 0; r < rounds; r++) {
        gui_relayout_all();
        /* The re-layout runs on each window's OWN host task, so this has to
         * wait for it rather than read straight back — a count taken before the
         * host has run would pass no matter what. */
        task_msleep(400);
    }
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].used || windows[i].kind != WIN_APP) continue;
        for (struct widget* w = windows[i].widgets; w; w = w->next) after++;
    }
    /* HAND THE VERDICT TO THE COMPOSITOR TO PRINT, which is not fussiness.
     *
     * While the GUI is up the SHELL's output goes to its VC through the
     * per-task hook and never reaches the serial line — the standing note
     * against `gui stats`, and every automated check in this project is a grep
     * over that line.  The compositor has no VC bound, so its kprintf goes
     * through the console sinks and out of the serial port, which is why its
     * own messages are in every log above.
     *
     * So the test computes the numbers here and lets the task that CAN be heard
     * say them.  *A test whose result cannot be read is not a test.* */
    g_relay_wins   = wins;
    g_relay_before = before;
    g_relay_after  = after;
    g_relay_rounds = rounds;
    g_relay_report = 1;
    need_frame = 1;
}
/* §M69 — INJECT A WHEEL NOTCH AT A CONTENT COORDINATE.
 *
 * This harness cannot deliver a real one (measured: QEMU's monitor produces no
 * wheel for our 4-byte IntelliMouse decode), so every claim about wheel
 * ROUTING has had to be made by reading the code — and reading is exactly what
 * produced two wrong explanations of the same report.  A synthetic notch goes
 * through the identical dispatcher a device notch would, so what it proves is
 * everything above the packet decode.
 *
 * `x`/`y` are CONTENT coordinates, which is what the router works in, and the
 * point is the whole value of the command: "the wheel does not work on the
 * content but works on the scrollbar" is a claim about two POSITIONS, and a
 * test that cannot name a position cannot check it. */
void gui_wheel_test(int x, int y, int dz) {
    struct gui_window* win = focused_win;
    if (!win || !win->used || win->kind != WIN_APP) {
        kprintf("wheeltest: no focused app window\n");
        return;
    }
    /* dz == 0 is PROBE MODE: report what is under the point and who would take
     * a notch there, WITHOUT moving anything.
     *
     * A sweep of probes is a MAP, and a map is what the report needs: "the
     * wheel works on the scrollbar and not on the content" is a claim about
     * two positions, and injecting a real notch at each of them changes the
     * layout between samples — so the second sample no longer describes the
     * thing the first one did. */
    if (dz == 0) {
        struct widget* w = widget_at(win->widgets, x, y);
        kprintf("wheelprobe: %d,%d -> widget %s scroll=%s\n", x, y,
                w ? "yes" : "none",
                (w && w->ops && w->ops->scroll) ? "YES (would take it)" : "no");
        return;
    }
    kprintf("wheeltest: dz=%d at content %d,%d in '%s'\n", dz, x, y, win->title);
    struct app_event e = {0};
    e.type = AE_SCROLL;
    e.x = (int16_t)x;
    e.y = (int16_t)y;
    e.phase = (uint8_t)(int8_t)dz;
    aq_push(win, e);
    need_frame = 1;
}
void gui_get_desktop_stats(struct gui_desktop_stats* out) {
    if (!out) return;
    out->iters      = desk_iters;
    out->draws      = desk_draws;
    out->events     = desk_events;
    out->ticks      = desk_ticks;
    out->tick_dirty = desk_tick_dirty;
    out->clock_ms   = desk_now_ms;
}

/* …and the cache is only honest if something drops it.  Without this a
 * `setconf gui.input_debug 1` typed after the first packet would be recorded,
 * reported as set by `conf list`, and change nothing — the debug switch being
 * the one setting whose silent failure costs a whole round of diagnosis. */
CONFIG_WATCH(cw_input_dbg) = {
    .prefix = "gui.input_debug",
    .changed = gui_input_debug_watch,
};

/* Drained once per frame by the compositor.  The reports are PRINTED HERE
 * rather than by the task that asked for them: with the GUI up, a kprintf on
 * the requesting task goes to the suppressed console, so the answer reaches
 * nobody — the §4.79 observation that `gui stats` printed nothing on serial
 * while `shortcut list` did. */
void gui_diag_service(void) {
        if (g_bench_report) {
            g_bench_report = 0;
            uint32_t fr = g_bench_fr ? g_bench_fr : 1;
            kprintf("compose: %ux%u, %u frame(s), %u us/frame total %u ms "
                    "[occlusion %s: %u rect(s) skipped, %u painted]\n",
                    g_bench_w, g_bench_h, g_bench_fr,
                    (unsigned)((g_bench_ns / fr) / 1000ull),
                    (unsigned)(g_bench_ns / 1000000ull),
                    g_occlude ? "on" : "off",
                    occluded_rects, painted_rects);
            occluded_rects = painted_rects = 0;
        }
        if (g_relay_report) {
            g_relay_report = 0;
            kprintf("relayout: %d window(s), %d widget(s) before, %d after "
                    "%d re-layout(s) — %s\n",
                    g_relay_wins, g_relay_before, g_relay_after, g_relay_rounds,
                    g_relay_before == g_relay_after
                        ? "PASS (the set was replaced)"
                        : "FAIL (widgets accumulated)");
        }
}
