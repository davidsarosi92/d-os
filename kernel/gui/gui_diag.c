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
