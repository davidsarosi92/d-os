/* =============================================================================
 * w_chart.c — a line chart over §M75's kernel history ring.
 *
 * IT IS A WIDGET CLASS AND NOT DRAWING CODE INSIDE THE TASK MANAGER, and that
 * is this milestone's structural test rather than a style preference.  There
 * are FOUR of them in the very first client, and this tree's most expensive
 * recurring defect is the second copy of something that looked too small to
 * share: four scrollbars that were four pictures of a scrollbar (§4.85.9), two
 * shells with 24 duplicated commands (§M70), three item layouts before
 * `ITEM_VIEW()` existed.  A graph drawn by its app would be the next one, and
 * the next caller is already visible — §M74's memory-pressure work wants
 * exactly this picture.
 *
 * THE WIDGET OWNS NO DATA.  It holds a series NUMBER and asks sysmon at draw
 * time.  That is what makes closing the Task Manager harmless: the history is
 * the kernel's (see sysmon.h), the chart is a view of it, and reopening the
 * window shows the minute you were not watching.
 *
 * DAMAGE: a scrolling chart changes every pixel it owns once a second, so its
 * damage is ITS OWN RECT — small, and four of them.  It must never mark the
 * window.  §M69 spent a milestone removing exactly that cost (an unconditional
 * full-window repaint two or three times a second, on top of every carefully
 * damaged row), and four 1 Hz charts are the most natural way to put it
 * straight back.  The widget does not damage anything itself — a widget cannot
 * know when its data moved — so the OWNER calls `w_chart_refresh()` on its
 * tick, which damages precisely this box.
 * ============================================================================= */

#include "widget.h"
#include "ui.h"        /* struct ui_spec + WIDGET_CLASS */
#include "gui.h"
#include "gfx.h"
#include "console_plate.h"
#include "sysmon.h"
#include "kmalloc.h"
#include "locale.h"
#include <stdint.h>

struct w_chart {
    struct widget base;
    int series;             /* enum sysmon_series */
    uint32_t seen;          /* sysmon sample count at the last damage */
};

/* ---------------------------------------------------------------------------
 * A tiny unsigned-to-text helper.
 *
 * This kernel's printf has NO WIDTH SPECIFIERS (a fact this project has been
 * bitten by in §M65, §M66, §M33 stage 5 and the localisation sweep), and a
 * chart's corner label wants an exact string rather than a formatted line, so
 * the digits are laid down by hand.
 * ------------------------------------------------------------------------- */
static int u32_to_str(char* out, int cap, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof tmp) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    int len = 0;
    while (n > 0 && len < cap - 1) out[len++] = tmp[--n];
    out[len] = '\0';
    return len;
}

/* "100.0" for a percent series (stored in tenths), "1234" for a rate. */
static void value_str(char* out, int cap, int series, uint32_t v) {
    int n;
    if (sysmon_is_percent(series)) {
        n = u32_to_str(out, cap, v / 10u);
        if (n < cap - 2) {
            out[n++] = '.';
            out[n++] = (char)('0' + (v % 10u));
            out[n]   = '\0';
        }
    } else {
        n = u32_to_str(out, cap, v);
        (void)n;
    }
}

static void str_cat(char* dst, int cap, const char* src) {
    int n = 0;
    while (dst[n] && n < cap - 1) n++;
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = '\0';
}

static void ch_draw(struct widget* w, struct gfx_surface* s) {
    struct w_chart* c = (struct w_chart*)w;
    const cp_theme* t = cp_current_theme();

    cp_plate(s, w->x, w->y, w->w, w->h, t->sunken, t->line);

    const int pad   = cp_px(4);
    const int fh    = cp_fh();
    const int top   = w->y + pad + fh + cp_px(2);      /* below the header row */
    const int bot   = w->y + w->h - pad;
    const int left  = w->x + pad;
    const int right = w->x + w->w - pad;
    const int plotw = right - left;
    const int ploth = bot - top;
    if (plotw <= 2 || ploth <= 2) return;              /* too small to say anything */

    /* --- header: the name, and the CURRENT value with its unit -------------
     * The unit is on screen because the four charts do not share one: two are
     * percentages and two are rates, and a number without its unit invites the
     * reader to compare them. */
    cp_text(s, left, w->y + pad, lstr(sysmon_series_name(c->series)), t->text);

    uint32_t hist[SYSMON_SAMPLES], mx = 0;
    int have = sysmon_history(c->series, hist, SYSMON_SAMPLES, &mx);

    /* NOTHING MEASURED YET IS NOT THE SAME AS A MACHINE AT REST, and a chart
     * is the surface where the two look most alike: both draw a flat line on
     * the floor.  §M71's rule 3, at the size of one string. */
    if (have == 0 || sysmon_sample_count() == 0) {
        const char* msg = lstr("chart.nodata");
        cp_text(s, left + (plotw - cp_text_w(msg)) / 2,
                   top + (ploth - fh) / 2, msg, t->muted);
        return;
    }

    /* THE NAME AND THE VALUE MUST NOT COLLIDE, and at four charts across one
     * window they very nearly do: "NETWORK" plus "0 pkt/s" is wider than a
     * quarter of the content box, and the first version drew them straight
     * over each other as `NETWORK0 pkt/s`.  A number that has run into a word
     * is not a smaller label — it is an unreadable one.
     *
     * THE NAME WINS when only one fits: it is what says WHICH chart this is,
     * and the line already shows roughly what the value is.  A nameless chart
     * with a precise number on it is the worse half to keep. */
    char buf[24];
    value_str(buf, (int)sizeof buf, c->series, sysmon_latest(c->series));
    str_cat(buf, (int)sizeof buf, " ");
    str_cat(buf, (int)sizeof buf, sysmon_series_unit(c->series));
    int name_w = cp_text_w(lstr(sysmon_series_name(c->series)));
    int val_w  = cp_text_w(buf);
    if (name_w + cp_px(6) + val_w <= plotw)
        cp_text(s, right - val_w, w->y + pad, buf, t->accent);

    /* --- the axis ceiling -------------------------------------------------
     * A PERCENTAGE HAS A FIXED CEILING so two moments in time are comparable
     * at a glance; a RATE has no natural maximum, so it autoscales — AND AN
     * AUTOSCALED CHART MUST SHOW ITS SCALE, or a flat line at 10 ops/s and one
     * at 10000 are the same picture.  *A graph whose scale is invisible is a
     * shape, not a measurement.* */
    uint32_t ceil_v;
    if (sysmon_is_percent(c->series)) {
        ceil_v = SYSMON_PERCENT_FULL;
    } else {
        ceil_v = mx;
        if (ceil_v == 0) ceil_v = 1;       /* divide-by-zero guard, NOT a scale */
        /* The ceiling is DRAWN for a rate, because it is the only thing that
         * gives the line a size.  A percentage's ceiling is always 100 and
         * printing it would be noise.
         *
         * **IT PRINTS `mx`, THE MEASURED MAXIMUM, AND NOT `ceil_v`.**  The
         * first version printed the divide-by-zero guard, so an idle disk drew
         * a chart labelled "1" — an internal constant leaking onto the screen
         * as though it were a measurement, which is the most confusing kind of
         * wrong number because it is plausible.  An all-zero series honestly
         * reads "0". */
        char cb[24];
        value_str(cb, (int)sizeof cb, c->series, mx);
        cp_text(s, left, top, cb, t->muted);
    }

    /* --- the line ---------------------------------------------------------
     * One segment per adjacent pair.  Integer arithmetic throughout (§A2: no
     * FP in kernel context), and the x span is divided by `have - 1` so the
     * newest sample always lands on the right edge — a chart whose live end
     * drifts inward as the ring fills reads as the data slowing down. */
    int denom = have > 1 ? have - 1 : 1;
    int prev_x = 0, prev_y = 0;
    for (int i = 0; i < have; i++) {
        uint32_t v = hist[i];
        if (v > ceil_v) v = ceil_v;        /* a percentage can exceed by rounding */
        int x = left + (plotw * i) / denom;
        int y = bot - (int)(((uint64_t)ploth * v) / ceil_v);
        if (i > 0) gfx_line(s, prev_x, prev_y, x, y, t->accent);
        else if (have == 1) gfx_line(s, x, y, x, y, t->accent);
        prev_x = x; prev_y = y;
    }
}

static const struct widget_ops ch_ops = { .draw = ch_draw };

static struct widget* ch_create(struct gui_window* win, const struct ui_spec* sp) {
    struct w_chart* c = (struct w_chart*)kcalloc(1, sizeof *c);
    if (!c) return NULL;
    c->series = (sp->value >= 0 && sp->value < SYSMON_NSERIES) ? sp->value : 0;
    /* NOT focusable: it takes no input, and a Tab stop on a control nobody can
     * operate is a dead step in every keyboard traversal (§M65's rule, and the
     * same reasoning the progress bar is built on). */
    widget_init(&c->base, win, 0, 0, cp_px(160), cp_px(70), &ch_ops, NULL, 0);
    return &c->base;
}

static void ch_measure(struct widget* w, int avail_w, int* min_w, int* pref_w,
                       int* pref_h) {
    (void)w;
    /* Wide by preference and short by design: four of these sit in a row under
     * a table, and the information is in the SHAPE of the line, which needs
     * width far more than it needs height. */
    *min_w  = cp_px(90);
    *pref_w = avail_w;
    *pref_h = cp_px(70);
}

static int  ch_get(struct widget* w) { return ((struct w_chart*)w)->series; }
static void ch_set(struct widget* w, int v) {
    if (v >= 0 && v < SYSMON_NSERIES) ((struct w_chart*)w)->series = v;
}

WIDGET_CLASS(wc_chart) = {
    .name = "chart", .create = ch_create, .measure = ch_measure,
    .get_value = ch_get, .set_value = ch_set,
};

/* ---------------------------------------------------------------------------
 * The owner's tick hook.
 *
 * gui.h's contract since §M69 is that **a tick damages what it changed**, and
 * a chart's data changes without the widget being told — so the owner calls
 * this once a second and gets exactly this box repainted, not the window.
 * ------------------------------------------------------------------------- */
void w_chart_refresh(struct widget* w) {
    if (!w || !w->win) return;

    /* §M75.2 — ONLY WHEN THE DATA ACTUALLY MOVED.
     *
     * The owner's tick is not a 1 Hz beat: the compositor sets it every 500 ms
     * AND immediately on every task spawn/kill/reap, and this machine's cron
     * jobs produce several of those a second.  Measured on the Task Manager:
     * **3.6 refreshes a second** against a sampler that produces ONE new
     * sample a second — so three out of four repaints redrew an identical
     * chart.
     *
     * That is affordable at the default window size and is not when the window
     * is maximized, which is where it was reported from: four charts across
     * 1920 px are ~130 kpx, and paying for them four times a second is
     * ~500 kpx/s of compositing that says nothing new.  gui.h's contract since
     * §M69 is that a tick damages what it CHANGED; a chart whose series has
     * not advanced has changed nothing.
     *
     * The sample count is the right test rather than the value: a series that
     * legitimately reads the same number twice has still SCROLLED, so its
     * picture differs. */
    struct w_chart* c = (struct w_chart*)w;
    uint32_t n = sysmon_sample_count();
    if (n == c->seen) return;
    c->seen = n;
    gui_window_request_redraw_rect(w->win, w->x, w->y, w->w, w->h);
}
