/* =============================================================================
 * w_itemview.c — the window-side half of the item view (§M63).
 *
 * itemview.h keeps the LAYOUTS stateless on purpose: selection and scroll
 * belong to the thing being looked at, not to the way it is arranged.  This
 * widget is one such thing — a window's view of a model.  The desktop is
 * another, and it holds its own selection without going through here, which is
 * exactly why the view API takes a surface and an origin instead of a window.
 *
 * Keyboard navigation lives here for the same reason: arrows and Enter move a
 * SELECTION, and the selection is the widget's, so a new layout never has to
 * reimplement them.
 * ============================================================================= */

#include "widget.h"
#include <stdarg.h>
#include "console_plate.h"
#include "scrollbar.h"
#include "ui.h"
#include "config.h"
#include "settings.h"   /* CONFIG_KEY */
#include "printf.h"
#include "itemview.h"
#include "gfx.h"
#include "gui.h"
#include "keymap.h"
#include "kmalloc.h"
#include <stddef.h>

/* ---- precise damage + the content diff ------------------------------------
 *
 * Everything below exists because this widget used to answer every event with
 * `gui_window_request_redraw(w->win)` — the WHOLE window, chrome included, for
 * a selection moving one row.  That was affordable only while the app host
 * repainted the window anyway; once it stopped (gui.h: a tick damages what it
 * changed), a view that does not damage itself is a view that stops updating.
 */

/* One slot's damage rect, in window coordinates.  Asked of the VIEW — `rect()`
 * already knows where an item lands, so this works for a grid without knowing
 * anything about rows or columns, which is the point of swappable layouts.
 * A view without `rect` gets the whole widget: honest, and there is none. */
/* §M75.2 — damage ONE CELL, falling back to the whole row when the view has no
 * per-cell geometry (the list and the grid, whose row IS one cell).  Returns 1
 * when it damaged a cell, 0 when the caller should damage the row instead.
 *
 * The clipping is `iv_damage_item`'s, deliberately: a cell in a partly-visible
 * row needs exactly the same top/bottom trimming, and two copies of that
 * arithmetic would drift the first time somebody changed the header height. */
/* §M79.2 — the probe that located the defect, kept and gated.  It prints the
 * DAMAGE rect and the PAINT extent for the same cell, which is the whole point:
 * four attempts at this artifact were made by reasoning about one end at a
 * time, and the two numbers side by side settled it in one run. */
CONFIG_KEY(ck_iv_probe) = {
    .key = "gui.iv_probe", .group = "System", .type = CFG_BOOL, .def = "0",
    .help = "log item-view damage rects against paint extents (diagnostic)",
};

int iv_probe_on(void) { return (int)config_get_long("gui.iv_probe", 0); }
void iv_probe(const char* fmt, ...) {
    /* kprintf takes the varargs directly; this exists so itemview.c can reach
     * the same gate without including config.h. */
    va_list ap; va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

static int iv_damage_cell(struct w_itemview* iv, int i, int col) {
    struct widget* w = &iv->base;
    int x, y, ww, hh;
    if (i < 0 || !iv->view || !iv->view->cell_rect) return 0;
    if (iv->view->cell_rect(i, col, w->w, w->h, iv->model, iv->scroll,
                            &x, &y, &ww, &hh) != 0) return 0;
    if (y >= w->h || y + hh <= 0) return 1;            /* off-screen: nothing to do */
    if (y < 0) { hh += y; y = 0; }
    if (y + hh > w->h) hh = w->h - y;
    if (hh <= 0) return 1;
    if (iv_probe_on())
        iv_probe("dmg   col%d x=%d w=%d (win %d..%d)\n",
                 col, x, ww, w->x + x, w->x + x + ww);
    gui_window_request_redraw_rect(w->win, w->x + x, w->y + y, ww, hh);
    return 1;
}

static void iv_damage_item(struct w_itemview* iv, int i) {
    struct widget* w = &iv->base;
    int x, y, ww, hh;
    if (i < 0) return;
    if (!iv->view || !iv->view->rect ||
        iv->view->rect(i, w->w, w->h, iv->model, iv->scroll, &x, &y, &ww, &hh) != 0)
        return;
    if (y >= w->h || y + hh <= 0) return;              /* scrolled out of sight */
    if (y < 0) { hh += y; y = 0; }
    if (y + hh > w->h) hh = w->h - y;
    if (hh <= 0) return;
    gui_window_request_redraw_rect(w->win, w->x + x, w->y + y, ww, hh);
}

static void iv_damage_all(struct w_itemview* iv) {
    struct widget* w = &iv->base;
    gui_window_request_redraw_rect(w->win, w->x, w->y, w->w, w->h);
}

/* §M76.2 — THE COLUMN GEOMETRY OF THE LAST REFRESH.
 *
 * Per-cell damage is only valid WHILE THE COLUMNS HAVE NOT MOVED, and in this
 * table they move on their own: `t_layout` derives every column's width from
 * its CONTENT (§M65 — *the longest cell is a fact*), so a number growing from
 * 4481 to 44751 widens its column and shifts every column after it.
 *
 * The damaged rect would be computed from the NEW layout while the OLD text
 * sits at the OLD position, so the old glyphs would never be painted over.
 *
 * **THIS WAS WRITTEN AS THE EXPLANATION OF A REPORTED ARTIFACT AND IT WAS NOT
 * THAT** — the doubled glyphs were §M76.5's cleared clip.  The hazard here is
 * nonetheless real on its own terms, which is why the check stays: any future
 * per-cell damage needs stable geometry, and this is what notices when it is
 * not.  *Keeping a correct guard is right; leaving it labelled as the fix for
 * something it never fixed is §M52's shape.*
 *
 * So the geometry is remembered and compared.  When it moves, the whole pane is
 * damaged, which is what a re-layout has always meant everywhere else in this
 * tree (§M69: a layout repaints everything, because widgets vacate pixels
 * nothing will paint over).  Cheap, because it only happens when a column
 * actually resizes. */
#define IV_MAXCOL 8
/* §M76.2 — which branch does a refresh take?  Reported as *"unchanged, maybe it
 * spread to the whole table"* after the column-geometry fix, and the two
 * possible readings — the fix never fires, or it fires constantly — call for
 * opposite responses.  One counter tells them apart. */
unsigned iv_stat_pane, iv_stat_cells, iv_stat_rows, iv_stat_colmove;
static int iv_cols_moved(struct w_itemview* iv) {
    struct widget* w = &iv->base;
    if (!iv->view || !iv->view->cell_rect) return 0;
    int moved = 0;
    for (int c = 0; c < IV_MAXCOL; c++) {
        int x = 0, y = 0, cw = 0, ch = 0;
        if (iv->view->cell_rect(iv->scroll, c, w->w, w->h, iv->model, iv->scroll,
                                &x, &y, &cw, &ch) != 0) { x = -1; cw = -1; }
        if (iv->colx[c] != x || iv->colw[c] != cw) moved = 1;
        iv->colx[c] = x; iv->colw[c] = cw;
    }
    return moved;
}


/* What is drawn in the slot showing item `i`, as one string.
 *
 * The leading byte is the SELECTION, which is what makes one comparison cover
 * both "the numbers moved" and "another row is chosen" — see widget.h. */
static void iv_signature(struct w_itemview* iv, int i, char* out, int cap) {
    int p = 0;
    out[p++] = (i == iv->sel) ? '>' : ' ';
    int cols = 1;
    if (iv->model && iv->model->columns && iv->model->cell) {
        cols = iv->model->columns(iv->model->ctx);
        if (cols < 1) cols = 1;
        if (cols > 8) cols = 8;
    }
    for (int c = 0; c < cols && p < cap - 2; c++) {
        char buf[64];
        item_cell_text(iv->model, i, c, buf, (int)sizeof buf);
        for (int k = 0; buf[k] && p < cap - 2; k++) out[p++] = buf[k];
        out[p++] = '\t';                     /* so "ab|c" and "a|bc" differ */
    }
    out[p] = 0;
}

static int iv_sig_differs(const char* a, const char* b) {
    for (int i = 0; i < IV_SIG_LEN; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

void w_itemview_refresh(struct w_itemview* iv) {
    /* §M76.1 — this function damages one rect per changed CELL, and without
     * the bracket the compositor composes between them: the refresh arrives on
     * screen as a wave down the table instead of as one update. */
    gui_damage_begin();
    if (!iv || !iv->model || !iv->model->count) return;
    struct widget* w = &iv->base;
    int total = iv->model->count(iv->model->ctx);
    int slots = (iv->view && iv->view->page) ? iv->view->page(w->w, w->h) : 1;
    if (slots > IV_SLOTS) slots = IV_SLOTS;
    if (slots < 0) slots = 0;

    /* A SCROLL MOVED EVERY SLOT, and so did a first draw or a resize.  Repaint
     * the pane once and re-take the whole signature: reporting thirty separate
     * changed slots would be true and would cost more than the one rect they
     * add up to. */
    /* §M76.2 — a column that resized invalidates every per-cell rect below, so
     * it takes the same branch a scroll does. */
    int cols_moved = iv_cols_moved(iv);
    if (!iv->sig_valid || iv->scroll != iv->sig_scroll || slots != iv->sig_slots
        || cols_moved) {
        iv_stat_pane++;
        if (cols_moved) iv_stat_colmove++;
        for (int r = 0; r < slots; r++) {
            int i = iv->scroll + r;
            if (i < total) iv_signature(iv, i, iv->rowsig[r], IV_SIG_LEN);
            else           iv->rowsig[r][0] = 0;
        }
        iv->sig_slots = slots;
        iv->sig_scroll = iv->scroll;
        iv->sig_valid = 1;
        iv_damage_all(iv);
        gui_damage_end();
        return;
    }

    for (int r = 0; r < slots; r++) {
        int i = iv->scroll + r;
        char sig[IV_SIG_LEN];
        if (i < total) iv_signature(iv, i, sig, IV_SIG_LEN);
        else           sig[0] = 0;
        if (!iv_sig_differs(sig, iv->rowsig[r])) continue;
        /* §M75.2 — the PREVIOUS signature, taken before it is overwritten
         * below: the field walk needs both sides. */
        char prev_sig[IV_SIG_LEN];
        int  old_sig_valid = 1;
        for (int k = 0; k < IV_SIG_LEN; k++) {
            prev_sig[k] = iv->rowsig[r][k];
            if (!prev_sig[k]) break;
        }
        prev_sig[IV_SIG_LEN - 1] = 0;

        for (int k = 0; k < IV_SIG_LEN; k++) {
            iv->rowsig[r][k] = sig[k];
            if (!sig[k]) break;
        }
        /* A SLOT THAT LOST ITS ITEM still has to be painted over: the old row
         * is sitting there and nothing else will ever cover it.  `rect` is
         * asked for the index that WOULD live there, which every view computes
         * from the slot rather than from the item. */

        /* §M75.2 — WHICH CELL MOVED?  The signature is already TAB-separated
         * by column (iv_signature), so the fields can be walked in step and
         * only the differing ones damaged — no extra per-cell storage at all.
         *
         * At 1920 px a row is ~76 kpx and a `TIME` cell is ~6 kpx, and five
         * moving rows a second is what a maximized Task Manager felt like
         * through a compositor that also draws the cursor.
         *
         * THE FIRST CHARACTER IS THE SELECTION MARKER, NOT A CELL: the wash
         * spans the whole row, so a marker change damages the row.  Getting
         * that wrong would leave half a highlight behind, which is the visible
         * kind of failure — but it is still worth stating, because the offset
         * is the sort of thing a later edit to iv_signature would silently
         * invalidate. */
        int per_cell = 0;
        if (old_sig_valid && iv->view->cell_rect && sig[0] == prev_sig[0]) {
            const char* a = sig + 1;
            const char* b = prev_sig + 1;
            int col = 0, damaged = 0, ok = 1;
            while (*a || *b) {
                const char* ae = a; while (*ae && *ae != '\t') ae++;
                const char* be = b; while (*be && *be != '\t') be++;
                int alen = (int)(ae - a), blen = (int)(be - b);
                int same = (alen == blen);
                for (int k = 0; same && k < alen; k++) if (a[k] != b[k]) same = 0;
                if (!same) {
                    if (!iv_damage_cell(iv, i, col)) { ok = 0; break; }
                    damaged = 1;
                }
                if (!*ae || !*be) { if (*ae != *be) ok = 0; break; }
                a = ae + 1; b = be + 1; col++;
                if (col >= 8) break;
            }
            /* `damaged == 0` with `ok` means the signature differed in a way
             * the field walk could not attribute (a column count change).  Fall
             * back rather than damage nothing — a diff that reports no change
             * for a row that changed is a stale row nobody can explain. */
            per_cell = ok && damaged;
        }
        /* §M77.1 — PER-CELL DAMAGE IS BACK ON, AND BOTH HALVES OF ITS HISTORY
         * ARE HERE BECAUSE THE SECOND ONE IS THE USEFUL ONE.
         *
         * §M75.2 damaged the changed CELLS instead of the row, cutting the mean
         * frame area about tenfold on a maximized window.  A report of rows
         * going BOLD at random was then attributed to it — four attempts were
         * made here and upstream, two introducing new defects — and it was
         * switched off as not carrying its weight.
         *
         * **IT WAS INNOCENT.**  The cause was `table_draw` clearing the clip
         * (§M76.5): from the second cell onward it painted unclipped over the
         * whole window, on top of rows nobody had cleared.  Every one of those
         * four attempts changed WHICH rects were damaged while that went on
         * painting all of them regardless.
         *
         * What genuinely blocked it was the OTHER finding from that hunt: this
         * path needs stable column geometry, and a content-sized column moves
         * on its own.  §M77 fixed that at the source, so the precondition now
         * holds rather than being hoped for — and `iv_cols_moved` still guards
         * it, so a resize that does happen takes the whole-pane branch.
         *
         * Judged with this file's own counters (`gui.stats_ms` reports pane /
         * colmove / cells / rows per refresh) and with a picture, because a
         * damage optimisation's failure mode is stale pixels and no counter
         * can see those. */
        /* §M77.1 — AND IT WENT BACK OFF, WITH A SYMPTOM THIS TIME INSTEAD OF A
         * THEORY.  That is the difference between this entry and the last one.
         *
         * With §M77's stable columns in place the numbers were good — refresh
         * 9-11 ms -> 5-6 ms, one CPU 3 % -> 1-2 %, `pane 0 colmove 0 cells 36
         * rows 0` — and **the picture still showed stale pixels**: a
         * right-aligned CPU% cell going from `24.5 %` to `0.0 %` left the
         * leading digit's fragment behind, as `?0.0 %`.
         *
         * THE COUNTERS CANNOT SEE THAT, and it is worth saying why they never
         * will: they count which rects were damaged, and a stale pixel is a
         * rect that was NOT.  *For a damage optimisation the picture is the
         * authority and the counters are a convenience* — the reverse of every
         * other optimisation in this tree.
         *
         * The concrete symptom is the useful part: it is specific to a
         * RIGHT-ALIGNED cell whose text got SHORTER, so the next attempt starts
         * from a reproducible case rather than from a theory.  Everything else
         * about the path is now sound — the clip discipline (§M76.5), stable
         * geometry (§M77) and the whole-pane fallback (`iv_cols_moved`) — which
         * is why the remaining defect is finally small enough to name.
         *
         * Row damage costs 9-11 ms a refresh on a window nobody keeps
         * maximized.  *That is not a price worth a wrong pixel.* */
        /* §M79 — OFF, AND NOW WITH A ONE-COMMAND REPRODUCTION.
         *
         * `launch Task Manager` -> `loop 4` -> `loopstop` -> screenshot.  The
         * hogs drive a right-aligned CPU% cell to `25.0 %` and `loopstop` drops
         * it to `0.0 %`; the shorter text leaves a PARTIAL leading glyph behind
         * (`?0.0 %`, `?5.0 %`).  It reproduces every time.
         *
         * WHAT THE PARTIAL GLYPH SAYS, and it is the useful part: the damage
         * rect's LEFT EDGE cuts through a character.  Not a missing rect — a
         * MISPLACED one.  So the cell's damaged box and the cell's painted box
         * disagree about where the column starts, which points at the geometry
         * between `table_cell_rect` (damage) and `table_draw` (paint) rather
         * than at the diff that decides WHICH cells to damage.  `iv_cols_moved`
         * is supposed to catch exactly that and reported `colmove 0` on an idle
         * desktop; it has not been measured across this driven shrink.
         *
         * §4.79's shape — a painter and a hit test computing one rectangle
         * differently — which this tree has paid for twice before.
         *
         * The path's other preconditions are sound now (§M76.5's clip
         * discipline, §M77's stable columns, the whole-pane fallback), so what
         * is left is small and located.  It stays off because row damage costs
         * 9-11 ms a refresh on a window nobody keeps maximized, and *that is
         * not a price worth a wrong pixel* — but the next attempt starts from a
         * reproduction and a narrowed suspect rather than from a theory. */
        /* §M79 — OFF, WITH A REPRODUCTION AND ONE SUSPECT ELIMINATED.
         *
         * REPRODUCTION (deterministic): `launch Task Manager` -> `loop 4` ->
         * `loopstop` -> screendump.  The hogs drive a right-aligned CPU% cell
         * to `25.0 %`, `loopstop` drops it to `0.0 %`, and the shorter text
         * leaves a PARTIAL leading glyph — `?0.0 %`, `?5.0 %`.  Every time.
         *
         * A character cut in half means the damage rect's LEFT EDGE fell inside
         * it: a MISPLACED rect, not a missing one.  The obvious suspect was the
         * column geometry — §4.79's shape, a painter and a hit test computing
         * one rectangle differently.
         *
         * **MEASURED, AND THE SUSPECT IS ELIMINATED.**  Across that exact
         * driven case the counters read `colmove 1` when `loop 4` WIDENS the
         * column and **`colmove 0` on the shrink** — the column does not move
         * back, because §M77 rounds mono widths up to a 4-character boundary
         * and `25.0 %` and `0.0 %` land in the same one.  So the geometry is
         * stable exactly when the artifact appears, and `table_cell_rect`
         * versus `table_draw` is NOT where this lives.
         *
         * What remains: the damage rect spans `xs[c]-1 .. xs[c]+ws[c]+1` while
         * the paint is confined to `xs[c] .. xs[c]+cw`, so the damage is the
         * WIDER of the two and ought to cover any old text.  That it does not
         * is the next thing to measure — logging the rect actually passed to
         * `gui_window_request_redraw_rect` beside the text's own extent, rather
         * than reasoning about either.
         *
         * *Two theories have now been killed by measurement rather than by
         * argument, and the fix has still not been guessed at.*  The path's
         * other preconditions are sound (§M76.5's clip discipline, §M77's
         * stable columns, the whole-pane fallback); row damage costs 9-11 ms a
         * refresh on a window nobody keeps maximized, and that is not a price
         * worth a wrong pixel. */
        /* §M79.2 — OFF, AND NOW LOCALISED TO A NUMBER.
         *
         * REPRODUCTION: `launch Task Manager` -> `loop 4` -> `loopstop`.  A
         * right-aligned CPU% cell shrinks and leaves a PARTIAL leading glyph.
         *
         * TWO SUSPECTS KILLED BY MEASUREMENT, not by argument.  First the
         * column geometry moving (§4.79's shape): `colmove 0` across the
         * shrink, because §M77 keeps `25.0 %` and `0.0 %` inside one 4-char
         * step.  Then the diff missing the cell: it does not.
         *
         * WHAT `gui.iv_probe` FINALLY SHOWED, printing both ends at once:
         *
         *     dmg   col3 x=348 w=112
         *     paint col3 xs=344 ws=88  tx=344 tw=66
         *     paint col3 xs=349 ws=77  tx=349 tw=55
         *
         * The damage begins at 348 while the old text began at 344 — **the
         * leftmost four pixels are never cleared**, which is the fragment.  And
         * its width, 112, matches NEITHER paint state (they would be 79 and
         * 90).  So `table_cell_rect` and `table_draw` are computing different
         * column geometry for the same column, although both call `t_layout`
         * with what look like the same arguments.
         *
         * *That is a located defect, not a theory* — and it is the third thing
         * this artifact turned out to be, after two that measurement ruled out.
         * The next step is to print `t_layout`'s inputs at both ends, since its
         * outputs demonstrably differ.
         *
         * Off until then: row damage costs 9-11 ms a refresh on a window nobody
         * keeps maximized, and that is not a price worth a wrong pixel. */
        (void)per_cell;
        iv_stat_rows++;
        iv_damage_item(iv, i);
    }
    gui_damage_end();
}

static int iv_sb(struct w_itemview* iv, struct sb_metrics* m);
static int iv_scroll_max(struct w_itemview* iv, int n);

static void iv_draw(struct widget* w, struct gfx_surface* s) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view || !iv->view->draw) return;

    /* A quiet backdrop so the items read as a pane rather than as marks on the
     * window background — and so the selection wash has something to sit on. */
    /* §M69 — the theme's list surface, not a hardcoded dark blue.  This one
     * literal is what made the file manager's pane stay dark under the light
     * theme — *"the colours are not in harmony, the blue is very telling."* */
    gfx_fill(s, w->x, w->y, w->w, w->h, cp_current_theme()->sunken);
    iv->view->draw(s, w->x, w->y, w->w, w->h, iv->model, iv->sel, iv->scroll);

    /* §M69 — and the scrollbar, painted by the WIDGET.  The view reserved the
     * strip and told us where it is; the widget owns `scroll` and the grab, so
     * it owns the only state a usable bar has (which part is held). */
    struct sb_metrics m;
    if (iv_sb(iv, &m)) sb_draw(s, &m, iv->sb_part);
}

/* THE SCROLL CEILING, in the view's own unit.
 *
 * Three answers, and conflating the last two is the bug this exists to stop:
 *   - the view has no `.scrollbar` op   → it never told us; fall back to items
 *   - the op says "everything fits"     → the ceiling is ZERO.  Nothing may
 *                                         scroll, and the wheel must do
 *                                         literally nothing.
 *   - the op reports a range            → content - viewport
 *
 * The Control Panel showed what the middle case costs: everything fitted, the
 * view drew no bar, and the wheel still scrolled — using an ITEM count as if
 * it were a ROW count — so most of the icons left the screen with nothing on
 * screen able to bring them back. */
static int iv_scroll_max(struct w_itemview* iv, int n) {
    if (!iv->view || !iv->view->scrollbar) return n > 0 ? n - 1 : 0;
    struct sb_metrics m;
    if (!iv_sb(iv, &m)) return 0;              /* it fits — do not move */
    int max = iv->sb_content - iv->sb_viewport;
    return max > 0 ? max : 0;
}

/* The bar's metrics in WINDOW coordinates, or 0 if this view has none.  One
 * definition for the painter and every input path — §4.79's lesson, where the
 * drawn box and the pressable box were computed apart and disagreed. */
static int iv_sb(struct w_itemview* iv, struct sb_metrics* m) {
    struct widget* w = &iv->base;
    if (!iv->view || !iv->view->scrollbar) return 0;
    int bx, by, bw, bh, content, viewport;
    if (!iv->view->scrollbar(w->w, w->h, iv->model, iv->scroll,
                             &bx, &by, &bw, &bh, &content, &viewport))
        return 0;
    sb_metrics(m, w->x + bx, w->y + by, bw, bh, content, viewport, iv->scroll);
    iv->sb_content = content;
    iv->sb_viewport = viewport;
    return 1;
}

/* §M61 fix — pointer DRAG past the top or bottom edge scrolls, which is the
 * other half of "scroll works": with no wheel, dragging is how a mouse user
 * reaches an item that is not on screen. */
static void iv_pointer(struct widget* w, int lx, int ly, int phase) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view) return;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return;
    int px = w->x + lx, py = w->y + ly;
    struct sb_metrics m;
    int has_sb = iv_sb(iv, &m);

    /* §M69 — THE SCROLLBAR GETS THE STREAM FIRST, and once a press has landed
     * on it the whole gesture belongs to it: gui.c grabs the pointer for this
     * widget, so without the `sb_part` latch a drag that wandered a few pixels
     * off the twelve-pixel-wide bar would start selecting rows instead. */
    if (phase == WPTR_PRESS && gui_input_debug())
        kprintf("iv: press at %d,%d  bar=%s part=%d scroll=%d/%d\n", px, py,
                has_sb ? "yes" : "NO", has_sb ? sb_hit(&m, px, py) : -1,
                iv->scroll, iv->sb_content);
    /* §M69 — stale-latch recovery; see ui.c. */
    if (phase == WPTR_PRESS) iv->sb_part = 0;

    if (phase == WPTR_PRESS && has_sb) {
        int part = sb_hit(&m, px, py);
        if (part != SB_NONE) {
            iv->sb_part = part;
            iv->sb_grab_dy = py - m.thumb_y;
            int was = iv->scroll;
            int max = iv->sb_content - iv->sb_viewport;
            if (max < 0) max = 0;
            switch (part) {
            case SB_UP:          iv->scroll--; break;
            case SB_DOWN:        iv->scroll++; break;
            case SB_TROUGH_UP:   iv->scroll -= sb_page(iv->sb_viewport); break;
            case SB_TROUGH_DOWN: iv->scroll += sb_page(iv->sb_viewport); break;
            default: break;                       /* SB_THUMB: grab only */
            }
            if (iv->scroll < 0)   iv->scroll = 0;
            if (iv->scroll > max) iv->scroll = max;
            if (iv->scroll != was) w_itemview_refresh(iv);
            else gui_window_request_redraw(w->win);   /* the held emphasis */
            return;
        }
    }

    if (phase == WPTR_DRAG && iv->sb_part) {
        if (iv->sb_part != SB_THUMB) return;      /* an arrow steps once */
        if (!has_sb) return;
        int ns = sb_scroll_from_thumb(&m, iv->sb_content, iv->sb_viewport,
                                      py - iv->sb_grab_dy);
        if (ns != iv->scroll) { iv->scroll = ns; w_itemview_refresh(iv); }
        return;
    }

    if (phase == WPTR_RELEASE) {
        if (iv->sb_part) { iv->sb_part = 0; gui_window_request_redraw(w->win); }
        return;
    }

    if (phase != WPTR_DRAG) return;

    /* §M61 — dragging past an edge scrolls, which is the other half of "scroll
     * works" for a mouse with no wheel. */
    int max = iv_scroll_max(iv, n);
    if (ly < 0 && iv->scroll > 0)             iv->scroll--;
    else if (ly > w->h && iv->scroll < max)   iv->scroll++;
    else {
        int idx = iv->view->hit(lx, ly, w->w, w->h, iv->model, iv->scroll);
        if (idx >= 0) iv->sel = idx;
    }
    w_itemview_refresh(iv);       /* the diff covers both: a scroll and a move */
}

static void iv_mouse(struct widget* w, int lx, int ly, int kind) {
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->view || !iv->view->hit) return;
    gui_window_focus_widget(w->win, w);

    /* §M69 — a click on the bar is the pointer path's; it must not also pick
     * the row that happens to lie behind it. */
    {
        struct sb_metrics m;
        if (iv_sb(iv, &m) && sb_hit(&m, w->x + lx, w->y + ly) != SB_NONE) return;
    }

    int idx = iv->view->hit(lx, ly, w->w, w->h, iv->model, iv->scroll);
    if (idx >= 0) iv->sel = idx;
    /* The selection is part of the signature, so the diff repaints the row that
     * lost the highlight and the row that took it — two rows, not a window. */
    w_itemview_refresh(iv);
    if (iv->on_select) iv->on_select(iv, idx, w->ctx);

    /* kind 1 = double click → activate.  Single click only selects: a settings
     * category that opened on one click would fire while the user was still
     * deciding, and the desktop's rule (§M64) is the same one. */
    if (kind == 1 && idx >= 0 && iv->model && iv->model->activate)
        iv->model->activate(iv->model->ctx, idx);
}

/* §M61 fix — keep the SELECTION visible.
 *
 * Reported from use: *"scroll doesn't work in the resolution list."*  It did
 * not: `scroll` existed in the widget and in every view's signature, and
 * NOTHING EVER CHANGED IT — arrow keys moved the selection off the bottom of
 * the pane and the items below simply could not be reached.  There is no wheel
 * to fall back on either (this PS/2 driver decodes the 3-byte packet, no
 * wheel), so the selection has to carry the viewport with it.
 *
 * Asked of the VIEW rather than computed here: `rect()` already knows where an
 * item lands, so "is it inside the box" needs no assumption about rows,
 * columns or item height — which is the whole point of the layouts being
 * swappable. */
static void iv_ensure_visible(struct w_itemview* iv, struct widget* w) {
    if (!iv->view || !iv->view->rect || iv->sel < 0) return;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return;

    for (int guard = 0; guard < n + 2; guard++) {
        int x, y, ww, hh;
        if (iv->view->rect(iv->sel, w->w, w->h, iv->model, iv->scroll,
                           &x, &y, &ww, &hh) == 0 && y >= 0 && y + hh <= w->h)
            return;                              /* fully visible — done */
        if (iv->sel < iv->scroll) {
            if (iv->scroll == 0) return;
            iv->scroll--;                        /* selection above the pane */
        } else {
            if (iv->scroll >= n - 1) return;
            iv->scroll++;                        /* below it (or clipped) */
        }
    }
}

static void iv_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_itemview* iv = (struct w_itemview*)w;
    if (!iv->model || !iv->model->count) return;
    int n = iv->model->count(iv->model->ctx);
    if (n <= 0) return;

    int page = (iv->view && iv->view->page) ? iv->view->page(w->w, w->h) : 1;
    /* One row of the layout: for a grid that is the page divided by the number
     * of visible rows, and asking the view for a rect is more honest than
     * guessing a column count here. */
    int row = 1;
    if (iv->view && iv->view->rect) {
        int x0, y0, ww, hh, x1, y1, w1, h1;
        if (iv->view->rect(0, w->w, w->h, iv->model, iv->scroll, &x0, &y0, &ww, &hh) == 0) {
            for (int i = 1; i < n; i++) {
                if (iv->view->rect(i, w->w, w->h, iv->model, iv->scroll,
                                   &x1, &y1, &w1, &h1) != 0) break;
                if (y1 != y0) { row = i; break; }    /* first item on row 2 */
            }
        }
    }

    int sel = iv->sel;
    switch (kc) {
        case KC_LEFT:  sel--; break;
        case KC_RIGHT: sel++; break;
        case KC_UP:    sel -= row; break;
        case KC_DOWN:  sel += row; break;
        case KC_HOME:  sel = 0; break;
        case KC_END:   sel = n - 1; break;
        case KC_PGUP:  sel -= page; break;
        case KC_PGDN:  sel += page; break;
        case KC_ENTER:
            if (iv->sel >= 0 && iv->model->activate)
                iv->model->activate(iv->model->ctx, iv->sel);
            return;
        default: return;
    }
    if (sel < 0) sel = 0;
    if (sel >= n) sel = n - 1;
    if (sel != iv->sel) {
        iv->sel = sel;
        iv_ensure_visible(iv, w);
        w_itemview_refresh(iv);
        if (iv->on_select) iv->on_select(iv, sel, w->ctx);
    }
}

/* §M61 follow-up — the wheel.  Three items per notch is what feels like one
 * gesture; one is glacial on an eleven-row list and a page is disorienting. */
static int iv_scroll(struct widget* w, int dz) {
    struct w_itemview* iv = (struct w_itemview*)w;
    int n = (iv->model && iv->model->count) ? iv->model->count(iv->model->ctx) : 0;
    if (n <= 0) return 0;
    /* §M69 — the ceiling is the VIEW's, in the VIEW's unit; see
     * iv_scroll_max.  It used to be `n - 1` items, which is what let the wheel
     * scroll a two-row grid by three rows. */
    int max = iv_scroll_max(iv, n);
    int was = iv->scroll;
    iv->scroll -= dz * ui_wheel_lines();     /* wheel-up (positive) scrolls up */
    if (iv->scroll < 0) iv->scroll = 0;
    if (iv->scroll > max) iv->scroll = max;
    if (iv->scroll == was) return 0;      /* at the end — let the page have it */
    w_itemview_refresh(iv);   /* a changed scroll takes the whole-pane branch */
    return 1;
}

/* §M75.2 — THIS VIEW RENDERS NO HOVER STATE, so there is nothing to repaint
 * when the pointer enters or leaves it.
 *
 * That is a statement of fact about `iv_draw`, not a decision to skip work:
 * grep this file, itemview.c and itemview.h for "hover" and there are no
 * matches.  The host was repainting the whole widget to set a flag nothing
 * draws — free while the widget is a list in a small window, and ~1.9 Mpx when
 * it fills a maximized one, which is where *"the mouse lags terribly"* came
 * from.  See widget.h.
 *
 * IF THIS VIEW EVER GAINS A HOVERED ROW, this must stop returning 1 and start
 * damaging that row — the honest failure of forgetting is a highlight that
 * does not appear, which is visible immediately, rather than a slow window
 * nobody can attribute. */
static int iv_hover(struct widget* w, int entering) {
    (void)w; (void)entering;
    return 1;                       /* handled: nothing changed on screen */
}

/* POSITIONAL, and the order is the struct's: draw, mouse, key, keycode,
 * measure, pointer, scroll, hover.  widget.h says new optional ops go at the
 * END for exactly this reason — inserting one in the middle silently re-binds
 * every table in the tree by a slot (§M58's scar). */
static const struct widget_ops itemview_ops = {
    .draw = iv_draw, .mouse = iv_mouse, .keycode = iv_keycode, .pointer = iv_pointer, .scroll = iv_scroll, .hover = iv_hover,
};

struct w_itemview* w_itemview_create(struct gui_window* win, int x, int y,
                                     int w, int h,
                                     const struct item_model* model,
                                     const char* view_name, void* ctx) {
    struct w_itemview* iv = (struct w_itemview*)kcalloc(1, sizeof *iv);
    if (!iv) return NULL;
    iv->base.x = x; iv->base.y = y; iv->base.w = w; iv->base.h = h;
    iv->base.ops = &itemview_ops;
    iv->base.ctx = ctx;
    iv->base.focusable = 1;
    /* §M63 fix — `win` is what `gui_window_focus_widget(w->win, w)` and
     * `gui_window_request_redraw(w->win)` are given, and this hand-written
     * constructor did not set it.  Every other widget goes through widget.c's
     * shared `widget_init`, which does; writing a constructor by hand skipped
     * the one line nothing else needed.  The symptom was specific and
     * confusing: the mouse worked (selection followed clicks) and the KEYBOARD
     * did nothing, because focus was set on a NULL window and the keycode had
     * no focused widget to reach. */
    iv->base.win = win;
    iv->model = model;
    iv->view  = item_view_by_name(view_name);
    iv->sel = -1;
    iv->scroll = 0;
    gui_window_add_widget(win, &iv->base);
    return iv;
}
