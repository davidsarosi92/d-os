/* =============================================================================
 * widget.c — label / button / listview / textinput (M22 stage 6).
 *
 * See widget.h for the model.  Drawing is plain gfx primitives; every
 * widget draws its full rect (the window redraw fills the content
 * background first, so widgets need not erase their own old pixels).
 * ============================================================================= */

#include "widget.h"
#include "console_plate.h"
#include "locale.h"
#include "scrollbar.h"
#include "config.h"
#include "printf.h"
#include "gui.h"
#include "gfx.h"
#include "keymap.h"          /* M22.5: KC_* keycodes for navigation */
#include "clipboard.h"       /* M22.5: Ctrl+C/V in textinput */
#include "kmalloc.h"
#include "task.h"
#include "ui.h"             /* §M65 — ui_text_clipped + the class registry */
#include <stddef.h>

/* Palette — deliberately close to the window chrome in gui.c. */
#define WCOL_TEXT (cp_current_theme()->text)
#define WCOL_DIM (cp_current_theme()->muted)
#define WCOL_BTN_TOP (cp_current_theme()->raised)
#define WCOL_BTN_BOT (cp_current_theme()->raised)
#define WCOL_BTN_EDGE (cp_current_theme()->line)
#define WCOL_BOX_BG (cp_current_theme()->sunken)
#define WCOL_BOX_EDGE (cp_current_theme()->line)
#define WCOL_BOX_FOCUS (cp_current_theme()->accent)
#define WCOL_SEL_BG (cp_current_theme()->sel_bg)
#define WCOL_ARROW (cp_current_theme()->muted)

/* -------------------------------------------------------------------------- */
/* Generic helpers.                                                            */
/* -------------------------------------------------------------------------- */

static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

void widget_draw_all(struct widget* head, struct gfx_surface* s) {
    for (struct widget* w = head; w; w = w->next) {
        if (!w->ops || !w->ops->draw) continue;
        /* §M65 — A WIDGET DRAWS INSIDE ITS OWN BOX.  Enforced here, in the one
         * loop every widget's draw goes through, rather than trusted to each
         * of them: the toolkit measures a widget and then tells it a size, and
         * a draw that ignores it lands on the widget next door (the settings
         * page's help text did exactly that).  With a scrolling container the
         * clip is narrower still — its viewport — which is what makes a child
         * scrolled half out of view stop at the edge instead of spilling. */
        /* §M76.1 — INTERSECT WITH THE CALLER'S CLIP; DO NOT REPLACE IT.
         *
         * `gfx_set_clip` REPLACES (§M65 paid for that once already, when
         * `ui_text_clipped` threw a scrolling container's viewport away).  This
         * loop replaced it too — so `gui_window_request_redraw_rect`, which
         * narrows the surface to ONE damaged rect before calling here, had its
         * rect discarded and every widget redrew its WHOLE box.
         *
         * Invisible on screen, because the compositor copies only the damaged
         * rect: the picture was always right and the WORK was not.  It cost
         * little while damage was a whole row, and §M75.2's per-cell damage
         * multiplied it by the number of changed cells — which is what
         * *"the refresh runs top to bottom in a wave, with a little lag"*
         * looks like from a chair.
         *
         * A widget that does not intersect the damaged rect is SKIPPED
         * outright: with a clip of zero width its draw would paint nothing
         * anyway, and the point is not to run it. */
        int cx0 = s->clip_x0, cy0 = s->clip_y0, cx1 = s->clip_x1, cy1 = s->clip_y1;
        int wx0 = (w->clip_w > 0) ? w->clip_x : w->x;
        int wy0 = (w->clip_w > 0) ? w->clip_y : w->y;
        int wx1 = wx0 + ((w->clip_w > 0) ? w->clip_w : w->w);
        int wy1 = wy0 + ((w->clip_h > 0) ? w->clip_h : w->h);
        if (wx0 < cx0) wx0 = cx0;
        if (wy0 < cy0) wy0 = cy0;
        if (wx1 > cx1) wx1 = cx1;
        if (wy1 > cy1) wy1 = cy1;
        if (wx1 <= wx0 || wy1 <= wy0) continue;      /* outside the damage */
        gfx_set_clip(s, wx0, wy0, wx1 - wx0, wy1 - wy0);
        w->ops->draw(w, s);
        /* DISABLED IS A COMPOSITE OVER THE FINISHED WIDGET (the design's rule),
         * which is why it happens here and not inside each draw: tinting every
         * element separately would need a disabled variant of every token, and
         * the nine widgets would drift apart. */
        if (w->disabled) cp_dim(s, w->x, w->y, w->w, w->h);
        /* RESTORE the caller's clip, do not CLEAR it: clearing resets to the
         * whole surface, so the next widget in the loop would intersect against
         * nothing and the narrowing above would work for exactly one widget. */
        gfx_set_clip(s, cx0, cy0, cx1 - cx0, cy1 - cy0);
    }
}

struct widget* widget_at(struct widget* head, int lx, int ly) {
    struct widget* hit = NULL;                  /* last match wins (top-most) */
    /* A disabled widget is skipped HERE rather than in each handler: a control
     * that looks off and still reacts is worse than one that was never
     * disabled, because the user has been told it is inert. */
    for (struct widget* w = head; w; w = w->next)
        if (lx >= w->x && lx < w->x + w->w && ly >= w->y && ly < w->y + w->h)
            hit = w;
    return hit;
}

/* §M81 — CROSS-TASK WIDGET CREATION, COUNTED.
 *
 * §M22.7's rule is that a window's widgets belong to the task that hosts it,
 * and until now that was a CONVENTION — written in three headers and checked
 * nowhere.  `widget_init` is the one route every widget is born through (the
 * contract in widget.h, enforced since §M81), which makes it the one place the
 * rule can be observed without instrumenting every caller.
 *
 * Announced at the moment it happens AND counted, for the same reason
 * `gui_app_open`'s hosting warning is both: the violation's SYMPTOM is that
 * nothing appears, arbitrarily later and in another file, so the line has to be
 * printed where the cause is.  Counted so `audit widget-threading` can report
 * it on a machine nobody was watching — §M71's argument for running from cron.
 *
 * NOT refused, only reported: refusing would turn a rendering bug into a
 * missing control, and §M71 rule 4 says an audit does not fix what it finds. */
unsigned widget_cross_task;
/* §M81 — the other two observations, recorded where they HAPPEN rather than by
 * an audit walking live windows.  See gui_window_add_widget for why that walk
 * was removed: it was itself a cross-task read of a list the host rebuilds. */
unsigned widget_uninited;
unsigned widget_focus_traps;
static const char* wx_last_from = "(none)";
static const char* wx_last_host = "(none)";

void widget_threading_last(const char** from, const char** host) {
    if (from) *from = wx_last_from;
    if (host) *host = wx_last_host;
}

/* §M65 — EXPORTED as widget_init.  It was static, so every widget outside this
 * file hand-rolled its own initialisation — and w_itemview.c's copy forgot to
 * set `win`, which is why keyboard navigation had never worked in an item view
 * (§4.70).  A constructor written by hand skips exactly the line nothing else
 * needed; one shared initialiser is how that stops happening. */
void widget_init(struct widget* w, struct gui_window* win,
                 int x, int y, int ww, int hh,
                 const struct widget_ops* ops, void* ctx, int focusable) {
    if (win && !gui_window_hosted_by_current(win)) {
        struct task* me = task_current();
        wx_last_from = me ? me->name : "(?)";
        wx_last_host = gui_window_host_name(win);
        if (widget_cross_task++ == 0)
            kprintf("gui: a widget is being created on '%s' for a window hosted "
                    "by '%s' - §M22.7 says the host owns them, and a cross-task "
                    "write here appears to do NOTHING (see gui.h)\n",
                    wx_last_from, wx_last_host);
    }
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    w->ops = ops;
    w->win = win;
    w->ctx = ctx;
    w->focusable = focusable;
    w->clip_x = w->clip_y = w->clip_w = w->clip_h = 0;   /* §M65: unrestricted */
    w->next = NULL;
    w->inited = WIDGET_INITED;          /* §M81 — see widget.h */
    /* FOCUSABLE AND DEAF: Tab cycles focus through this widget and then the
     * keyboard does nothing, with nothing on screen to say why.  The silent
     * half of widget.h's contract, observed at construction because that is
     * when both facts are in hand. */
    if (focusable && ops && !ops->key && !ops->keycode) {
        if (widget_focus_traps++ == 0)
            kprintf("gui: a FOCUSABLE widget was built with no key handler - "
                    "Tab will land on it and the keyboard will stop there "
                    "(see widget.h's contract)\n");
    }
    gui_window_add_widget(win, w);
}

/* The Console Plate border: 1 px, with the corner pixels left out.  Every
 * widget in this file already went through here, so routing it to cp_border is
 * what gives the whole toolkit cut corners in one edit instead of nine. */
static void outline(struct gfx_surface* s, int x, int y, int w, int h, uint32_t c) {
    cp_border(s, x, y, w, h, c);
}

/* -------------------------------------------------------------------------- */
/* Label.                                                                      */
/* -------------------------------------------------------------------------- */

static void label_draw(struct widget* w, struct gfx_surface* s) {
    struct w_label* l = (struct w_label*)w;
    /* CAPTION STYLE — the design's label style, and cp_label's first caller.
     * A column header or a section title is not body text: widget_specs.md
     * gives it UPPERCASE, tracking and `muted`, sitting on a `tray` band with a
     * 1 px `line` under it.  That is what separates a table's header from its
     * first row, and without it a list reads as one undifferentiated block —
     * which is exactly how the Task Manager looked next to the catalogue. */
    if (l->caption) {
        const cp_theme* t = cp_current_theme();
        gfx_fill(s, w->x, w->y, w->w, w->h, t->tray);
        /* The rule goes on the side facing the content: under a header, over a
         * footer.  Both bands are `tray`, and without the rule on the right
         * side a footer reads as a header for whatever is below the window. */
        if (l->caption == 2) gfx_fill(s, w->x, w->y, w->w, 1, t->line);
        else                 gfx_fill(s, w->x, w->y + w->h - 1, w->w, 1, t->line);
        cp_label(s, w->x + 4, w->y + (w->h - cp_fh()) / 2, lstr(l->text));
        /* The right-hand half, if there is one.  Measured with cp_label_width
         * because a caption is TRACKED — spacing the letters out and then
         * positioning with the untracked width puts the total off the right
         * edge, which is the sort of thing that only shows on the longest
         * string somebody ever produces. */
        if (l->text2[0]) {
            int tw = cp_label_width(l->text2);
            cp_label(s, w->x + w->w - 4 - tw,
                     w->y + (w->h - cp_fh()) / 2, l->text2);
        }
        return;
    }
    /* §M65 — clipped to the label's own rect.  A label is the widget most
     * likely to be handed text longer than its box (a help line, a path), and
     * text that spills is text drawn over the widget next to it. */
    const cp_theme* lt = cp_current_theme();
    uint32_t lc = l->color ? l->color
                : (l->role == WLBL_MUTED  ? lt->muted
                :  l->role == WLBL_ACCENT ? lt->accent : lt->text);
    ui_text_clipped(s, w, w->x, w->y + (w->h - cp_fh()) / 2, lstr(l->text), lc);
}

static const struct widget_ops label_ops = {
    .draw = label_draw,
};

struct w_label* w_label_create(struct gui_window* win, int x, int y, int w,
                               const char* text) {
    struct w_label* l = (struct w_label*)kcalloc(1, sizeof(*l));
    if (!l) return NULL;
    widget_init(&l->base, win, x, y, w, cp_fh() + 4, &label_ops, NULL, 0);
    l->color = 0;          /* 0 = the theme's `text`, resolved at draw */
    str_copy(l->text, text, (int)sizeof(l->text));
    return l;
}

void w_label_set_caption(struct w_label* l, int on) {
    /* THE VALUE IS KEPT, NOT NORMALISED TO A BOOLEAN.  This was `on ? 1 : 0`,
     * so the footer style (2) was stored as 1 and every footer in the toolkit
     * drew its rule on the BOTTOM — under the last thing in the window, where
     * it reads as a header for whatever comes next.  A parameter documented as
     * taking three values and squashed into two is a setting that silently
     * cannot be selected. */
    if (l) l->caption = on;
}

void w_label_set(struct w_label* l, const char* text) {
    if (!l) return;
    str_copy(l->text, text, (int)sizeof(l->text));
}

void w_label_set_trailing(struct w_label* l, const char* text) {
    if (!l) return;
    str_copy(l->text2, text ? text : "", (int)sizeof(l->text2));
}

/* -------------------------------------------------------------------------- */
/* Button.                                                                     */
/* -------------------------------------------------------------------------- */

static void button_draw(struct widget* w, struct gfx_surface* s) {
    struct w_button* b = (struct w_button*)w;
    const cp_theme* t = cp_current_theme();
    const int focused = gui_widget_focused(w);
    const int hot = w->hovered;
    const int held = w->pressed;
    uint32_t fg = t->text;

    /* Flat throughout, per the design's own statement that the language is
     * built from surface layers and 1 px borders rather than gradients. */
    switch (b->emphasis) {
    case CP_BTN_PRIMARY:
        /* No border: the accent fill IS the edge, and outlining it as well
         * makes the primary button look like a secondary one somebody
         * coloured in. */
        /* An accent-filled control cannot show hover by changing to `hover`
         * (a surface colour) without ceasing to look primary.  The design's
         * answer is a 1 px inner edge in `on_accent`, which reads as a
         * highlight and keeps the fill. */
        /* Pressed goes to `press` outright — the design darkens an accent
         * fill rather than edging it, because a held button should read as
         * pushed in, not as highlighted. */
        cp_fill_plate(s, w->x, w->y, w->w, w->h, held ? t->press : t->accent);
        if (hot && !held) cp_border(s, w->x + 1, w->y + 1, w->w - 2, w->h - 2,
                                    t->on_accent);
        fg = t->on_accent;
        break;
    case CP_BTN_GHOST:
        /* Nothing at rest.  A focus ring still appears, because the design is
         * explicit that no control may be unreachable-looking from the
         * keyboard (widget_specs.md §0). */
        if (held)             cp_fill_plate(s, w->x, w->y, w->w, w->h, t->press);
        else if (hot || focused)
            cp_fill_plate(s, w->x, w->y, w->w, w->h, t->hover);
        break;
    case CP_BTN_ICON:
    case CP_BTN_SECONDARY:
    default:
        cp_plate(s, w->x, w->y, w->w, w->h,
                 held ? t->press : (hot ? t->hover : t->raised), t->line);
        break;
    }

    if (focused) cp_focus_ring(s, w->x, w->y, w->w, w->h);

    /* Rule 3 (console_plate.h): centred on both axes, and measured in the face
     * it is drawn in.  This was `tw * cp_fw()`, so with a proportional label
     * the button's text sat off-centre by the difference between a digit's
     * advance and the real one — worst on the widest labels, which is where it
     * shows most. */
    /* §M69 — RESOLVED AT DRAW TIME, not at creation.  A widget that stored
     * the TRANSLATED string held a copy the catalogue could no longer reach,
     * so changing the language only took effect at the next boot — reported
     * from use, and the fix has to be here rather than a "re-create every
     * widget" pass, because that would throw away every widget's state.
     *
     * Safe to apply to EVERY widget in the tree: `lstr` falls back to the key
     * itself, so a caption that is a plain literal renders exactly as before.
     * The cost is a small linear scan per drawn string; locale.c names the
     * fix (a sorted table) if it ever measures. */
    const char* btxt = lstr(b->text);
    cp_text(s, w->x + (w->w - cp_text_w(btxt)) / 2,
            w->y + (w->h - cp_fh()) / 2, btxt, fg);
}

void w_button_set_emphasis(struct w_button* b, int emphasis) {
    if (b) b->emphasis = emphasis;
}

static void button_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    struct w_button* b = (struct w_button*)w;
    if (b->on_click) b->on_click(b, w->ctx);
}

static const struct widget_ops button_ops = {
    .draw = button_draw, .mouse = button_mouse,
};

struct w_button* w_button_create(struct gui_window* win, int x, int y,
                                 int w, int h, const char* text,
                                 void (*on_click)(struct w_button*, void*),
                                 void* ctx) {
    struct w_button* b = (struct w_button*)kcalloc(1, sizeof(*b));
    if (!b) return NULL;
    widget_init(&b->base, win, x, y, w, h, &button_ops, ctx, 0);
    b->on_click = on_click;
    str_copy(b->text, text, (int)sizeof(b->text));
    return b;
}

/* §M69 — PLACE A BUTTON AT THE SIZE IT ASKS FOR, and report the width so a
 * caller can advance a cursor.
 *
 * The apps that predate the layout engine hand-place their button rows, and
 * every one of them picked the size by eye in the 8x8 era: the file manager's
 * row was seven buttons at a literal height of 18 and widths of 44/56/50/54
 * chosen to fit a fixed 8 px advance.  With a proportional face at 137 %
 * density the text is ~20 px tall, so THE FRAME CUT THROUGH ITS OWN LABEL —
 * "MkDir" touched both edges and the descenders of "Copy" were sliced off.
 *
 * This is not a second layout engine and must not become one.  It is the one
 * question those call sites never asked: `cls_button_measure` has known the
 * right answer since §M69 wrote the convention down, and nothing outside the
 * toolkit could reach it.  Porting these apps to `ui_build` is the real end
 * state and a much larger change; this makes the interim honest rather than
 * leaving nine windows contradicting a rule the header states in capitals. */
static void cls_button_measure(struct widget* w, int avail_w, int* min_w,
                               int* pref_w, int* pref_h);

int w_button_autosize(struct w_button* b, int x, int y) {
    if (!b) return 0;
    int min_w = 0, pref_w = 0, pref_h = 0;
    /* A generous `avail_w`: these are hand-placed rows, so the caller — not
     * the button — decides what to do when the row runs out of room. */
    cls_button_measure(&b->base, 1 << 20, &min_w, &pref_w, &pref_h);
    b->base.x = x;
    b->base.y = y;
    b->base.w = pref_w;
    b->base.h = pref_h;
    return pref_w;
}

/* -------------------------------------------------------------------------- */
/* List view.  Rows of text; right-edge 12px strip = scroll arrows.            */
/* -------------------------------------------------------------------------- */

/* ONE width for the scrollbar, used by BOTH the draw and the hit test.  They
 * were two different numbers — the bar was drawn CP_SCROLLBAR_W wide and the
 * strip tested cp_fw()+4 — so an 8 px band looked like list and scrolled when
 * clicked.  A view that draws one geometry and hit-tests another is the defect
 * §M64 called invisible in a screenshot; the only fix is that there is nothing
 * to keep in step. */

static int lv_visible_rows(const struct w_listview* lv) {
    int r = (lv->base.h - 4) / WLIST_ROW_H;
    return r > 0 ? r : 1;
}

/* The end arrows are GONE, and that is the design's instruction rather than a
 * simplification: widget_specs.md §10 says "nincs végnyíl" — a Console Plate
 * scrollbar is a trough and a proportional thumb.  The compiler naming this
 * function unused is what confirmed nothing else still drew them. */

/* THE TABLE, AS THE DESIGN DRAWS IT (design/reference §06, and the rendered
 * catalogue in OS_HTML_DES.html).
 *
 * The old version was a box with text in it: no row separation, a selection
 * that stopped short of the scrollbar, and a scroll strip in a hardcoded
 * colour that belonged to no theme.  Four things carry a Console Plate list
 * and each is one fill:
 *
 *   - ROW SEPARATORS in `line_soft`, not alternating fills.  The design uses a
 *     hairline between rows; banding would fight the selection for the same
 *     visual job and win, which is why every row here has the same fill.
 *   - THE SELECTION SPANS THE FULL ROW, right up to the scroll strip.  Stopping
 *     short reads as a highlighted cell rather than a chosen row — and it is
 *     the row that is chosen.
 *   - THE SCROLL TROUGH IS `tray` AND THE THUMB IS `muted`, both from the
 *     theme.  The literal 0xFF16202E it used before was invisible in the dark
 *     theme and a dark bar down the side of the light one.
 *   - THE THUMB IS PROPORTIONAL: viewport² / content, floored at the design's
 *     CP_SCROLLBAR_THUMB_MIN.  A fixed-size thumb says nothing about how much
 *     list there is, which is most of what a scrollbar is for. */
static void lv_sb_metrics(const struct w_listview* lv, struct sb_metrics* m);

static void listview_draw(struct widget* w, struct gfx_surface* s) {
    struct w_listview* lv = (struct w_listview*)w;
    const cp_theme* t = cp_current_theme();

    cp_fill_plate(s, w->x, w->y, w->w, w->h, WCOL_BOX_BG);
    outline(s, w->x, w->y, w->w, w->h, WCOL_BOX_EDGE);

    const int sb = cp_scrollbar_w();
    const int inner_w = w->w - sb - 2;
    int rows = lv_visible_rows(lv);

    for (int r = 0; r < rows; r++) {
        int idx = lv->scroll + r;
        if (idx >= lv->count) break;
        int ry = w->y + 2 + r * WLIST_ROW_H;

        if (idx == lv->sel)
            gfx_fill(s, w->x + 1, ry, inner_w, WLIST_ROW_H, t->sel_bg);
        else if (r)                       /* no rule above the first row */
            gfx_fill(s, w->x + 1, ry, inner_w, 1, t->line_soft);

        (lv->mono ? cp_mono_text : cp_text)
                  (s, w->x + 8, ry + (WLIST_ROW_H - cp_fh()) / 2,
                lv->items[idx], idx == lv->sel ? t->sel_fg : t->text);
    }

    /* §M69 — ONE scrollbar (scrollbar.c): arrows, trough and a proportional
     * thumb, computed by the same call the hit test uses so the drawn box and
     * the pressable box cannot disagree. */
    struct sb_metrics sbm;
    lv_sb_metrics(lv, &sbm);
    sb_draw(s, &sbm, lv->sb_part);
}

/* The bar's geometry, in WINDOW coordinates — one definition, called by the
 * painter and by every input path. */
static void lv_sb_metrics(const struct w_listview* lv, struct sb_metrics* m) {
    const struct widget* w = &lv->base;
    int sb = cp_scrollbar_w();
    sb_metrics(m, w->x + w->w - sb - 1, w->y + 1, sb, w->h - 2,
               lv->count, lv_visible_rows((struct w_listview*)lv), lv->scroll);
}

/* One band of rows as a damage rect, in window coordinates — THE SAME
 * ARITHMETIC THE PAINTER USES (`w->y + 2 + r * WLIST_ROW_H` in listview_draw).
 * Shared by the content diff and by the widget's own state changes so a row
 * cannot be painted at one y and damaged at another; §M69 paid for that shape
 * once already in the title buttons, where the painter and the hit test each
 * computed the same box and disagreed by two pixels.  Returns 0 when the band
 * is entirely outside the viewport. */
static int lv_row_band(const struct w_listview* lv, int from, int n,
                       int* ox, int* oy, int* ow, int* oh) {
    int lo = from - lv->scroll;
    if (lo < 0) { n += lo; lo = 0; }
    if (n <= 0) return 0;
    int y = lv->base.y + 2 + lo * WLIST_ROW_H;
    int h = n * WLIST_ROW_H;
    int bot = lv->base.y + lv->base.h;
    if (y >= bot) return 0;
    if (y + h > bot) h = bot - y;
    if (h <= 0) return 0;
    *ox = lv->base.x; *oy = y; *ow = lv->base.w; *oh = h;
    return 1;
}

/* THE OWNER'S DIFF CANNOT SEE A SELECTION, AND MUST NOT BE ASKED TO.
 *
 * A self-refreshing owner (the Task Manager) repaints only the rows whose TEXT
 * changed — the right question for content and the wrong one for state:
 * choosing another row changes no text at all, only which row is drawn in the
 * selection colours.  Under the old repaint-everything that was invisible;
 * with a diff in place the highlight simply never moved, and a table whose
 * selection does not follow the click is broken in the one way a user notices
 * immediately.  *A speedup that costs correctness is not a speedup.*
 *
 * So the widget damages its OWN appearance changes, at the moment it makes
 * them — this is the only place that knows both the row it left and the row it
 * took, and it costs two rows rather than a table. */
static void lv_damage_row(struct w_listview* lv, int idx) {
    int x, y, w, h;
    if (idx < 0 || idx >= lv->count) return;
    if (lv_row_band(lv, idx, 1, &x, &y, &w, &h))
        gui_window_request_redraw_rect(lv->base.win, x, y, w, h);
}

/* A scroll moves every row, so the honest damage is the whole widget — but the
 * WIDGET, not the window: repainting the chrome, the buttons and the footer
 * because a list scrolled is the cost this whole diff exists to avoid. */
static void lv_damage_all(struct w_listview* lv) {
    gui_window_request_redraw_rect(lv->base.win, lv->base.x, lv->base.y,
                                   lv->base.w, lv->base.h);
}

static void listview_mouse(struct widget* w, int lx, int ly, int kind) {
    struct w_listview* lv = (struct w_listview*)w;
    int rows = lv_visible_rows(lv);

    gui_window_focus_widget(w->win, w);         /* M22.5: keyboard nav */

    /* §M69 — the scrollbar is handled by the POINTER path (press/drag/release)
     * so the thumb can be dragged; a click event alone cannot express that.
     * This arm only has to keep the click from selecting a row underneath. */
    if (lx >= w->w - cp_scrollbar_w() - 1) return;

    int r = (ly - 2) / WLIST_ROW_H;
    int idx = lv->scroll + r;
    if (r < 0 || r >= rows || idx >= lv->count) return;

    int was = lv->sel;
    lv->sel = idx;
    if (was != idx) { lv_damage_row(lv, was); lv_damage_row(lv, idx); }
    if (kind == 1) {
        if (lv->on_activate) lv->on_activate(lv, idx, w->ctx);
    } else {
        if (lv->on_select) lv->on_select(lv, idx, w->ctx);
    }
}

/* M22.5 — keep the selected row inside the viewport. */
static void lv_scroll_to_sel(struct w_listview* lv) {
    int rows = lv_visible_rows(lv);
    if (lv->sel < lv->scroll) lv->scroll = lv->sel;
    if (lv->sel >= lv->scroll + rows) lv->scroll = lv->sel - rows + 1;
    if (lv->scroll < 0) lv->scroll = 0;
}

/* M22.5 — keyboard navigation: arrows / PgUp / PgDn / Home / End move
 * the selection (firing on_select like a click does). */
static void listview_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    (void)mods;
    struct w_listview* lv = (struct w_listview*)w;
    if (lv->count == 0) return;
    int rows = lv_visible_rows(lv);
    int sel = lv->sel < 0 ? 0 : lv->sel;

    switch (kc) {
    case KC_UP:    sel--;        break;
    case KC_DOWN:  sel++;        break;
    case KC_PGUP:  sel -= rows;  break;
    case KC_PGDN:  sel += rows;  break;
    case KC_HOME:  sel = 0;      break;
    case KC_END:   sel = lv->count - 1; break;
    default: return;
    }
    if (sel < 0) sel = 0;
    if (sel >= lv->count) sel = lv->count - 1;
    if (sel == lv->sel) return;
    int was = lv->sel, was_scroll = lv->scroll;
    lv->sel = sel;
    lv_scroll_to_sel(lv);
    /* Same rule as the click: the widget damages what it changed.  A keyboard
     * move that also scrolled the viewport moved every row, so that case takes
     * the whole widget; the ordinary case is two rows. */
    if (lv->scroll != was_scroll) lv_damage_all(lv);
    else { lv_damage_row(lv, was); lv_damage_row(lv, sel); }
    if (lv->on_select) lv->on_select(lv, sel, w->ctx);
}

/* M22.5 — Enter activates the selection (same as double-click). */
static void listview_key(struct widget* w, char c) {
    struct w_listview* lv = (struct w_listview*)w;
    if (c != '\n' || lv->sel < 0 || lv->sel >= lv->count) return;
    if (lv->on_activate) lv->on_activate(lv, lv->sel, w->ctx);
}

/* §M61 follow-up — the wheel, for the same reason the item view got one: with
 * no wheel a list ends at its last visible row.  Three rows per notch. */
/* §M69 — PRESS / DRAG / RELEASE on the scrollbar.
 *
 * All three parts of the report land here.  A press on an ARROW steps; on the
 * TROUGH it pages toward the pointer (the design's own behaviour); on the
 * THUMB it starts a drag, and gui.c's pointer GRAB is what keeps the motion
 * arriving after the pointer has wandered off the narrow bar — without the
 * grab a drag would stop the instant the hand strayed twelve pixels sideways,
 * which is most of the time. */
/* §M81 — the op returns WH_* now (widget.h).  WH_REPAINT everywhere is
 * EXACTLY what this did before it had a return value: the host set
 * `ran = 1` whenever a pointer op existed.  Kept identical on purpose, so
 * the only behaviour step 1 changes is the container's — several of these
 * paths already damage precisely and are candidates for WH_DAMAGED, which
 * is a saving with its own measurement. */
static int listview_pointer(struct widget* w, int lx, int ly, int phase) {
    struct w_listview* lv = (struct w_listview*)w;
    struct sb_metrics m;
    lv_sb_metrics(lv, &m);
    int px = w->x + lx, py = w->y + ly;          /* window coordinates */

    if (phase == WPTR_PRESS) {
        /* §M69 — a press starts a new gesture, so a latch still held from the
         * last one is stale; see ui.c for why this must be self-healing rather
         * than merely correct on the happy path. */
        lv->sb_part = 0;
        int part = sb_hit(&m, px, py);
        if (gui_input_debug())
            kprintf("lv: press at %d,%d bar %d,%d %dx%d part=%d\n",
                    px, py, m.x, m.y, m.w, m.h, part);
        if (part == SB_NONE) return WH_REPAINT;
        lv->sb_part = part;
        lv->sb_grab_dy = py - m.thumb_y;
        int was = lv->scroll, rows = lv_visible_rows(lv);
        int max = lv->count - rows;
        if (max < 0) max = 0;
        switch (part) {
        case SB_UP:          lv->scroll--; break;
        case SB_DOWN:        lv->scroll++; break;
        case SB_TROUGH_UP:   lv->scroll -= sb_page(rows); break;
        case SB_TROUGH_DOWN: lv->scroll += sb_page(rows); break;
        default: break;                          /* SB_THUMB: grab only */
        }
        if (lv->scroll < 0)   lv->scroll = 0;
        if (lv->scroll > max) lv->scroll = max;
        if (lv->scroll != was) lv_damage_all(lv);
        else                   gui_window_request_redraw(w->win);  /* the emphasis */
        return WH_REPAINT;
    }

    if (phase == WPTR_DRAG) {
        if (lv->sb_part != SB_THUMB) return WH_REPAINT;
        int rows = lv_visible_rows(lv);
        int ns = sb_scroll_from_thumb(&m, lv->count, rows, py - lv->sb_grab_dy);
        if (ns != lv->scroll) { lv->scroll = ns; lv_damage_all(lv); }
        return WH_REPAINT;
    }

    /* RELEASE — drop the grab and repaint, because the pressed arrow is drawn
     * emphasised and would otherwise stay lit after the button came up. */
    if (lv->sb_part) { lv->sb_part = 0; gui_window_request_redraw(w->win); }
    return WH_REPAINT;
}

static int listview_scroll(struct widget* w, int dz) {
    struct w_listview* lv = (struct w_listview*)w;
    /* §M69 — clamped to the LAST FULL SCREEN, not to `count - 1`.  The old
     * bound let the wheel scroll a list until one row was left at the top with
     * empty space under it, and the thumb then had nowhere sensible to sit. */
    int rows = lv_visible_rows(lv);
    int max = lv->count - rows;
    if (max < 0) max = 0;
    int was = lv->scroll;
    /* §M69 — the same `gui.scroll_lines` the container uses, so one notch
     * means the same amount of content wherever the pointer is. */
    lv->scroll -= dz * ui_wheel_lines();
    if (lv->scroll < 0) lv->scroll = 0;
    if (lv->scroll > max) lv->scroll = max;
    if (lv->scroll == was) return 0;      /* at the end — let the page have it */
    gui_window_request_redraw(w->win);
    return 1;
}

static const struct widget_ops listview_ops = {
    .draw = listview_draw, .mouse = listview_mouse, .key = listview_key, .keycode = listview_keycode, .pointer = listview_pointer, .scroll = listview_scroll,
};

struct w_listview* w_listview_create(struct gui_window* win, int x, int y,
                                     int w, int h, void* ctx) {
    struct w_listview* lv = (struct w_listview*)kcalloc(1, sizeof(*lv));
    if (!lv) return NULL;
    widget_init(&lv->base, win, x, y, w, h, &listview_ops, ctx, 1);
    lv->sel = -1;
    return lv;
}

void w_listview_clear(struct w_listview* lv) {
    if (!lv) return;
    lv->count = 0;
    lv->sel = -1;
    lv->scroll = 0;
}

int w_listview_add(struct w_listview* lv, const char* text, uint8_t tag) {
    if (!lv || lv->count >= WLIST_MAX_ITEMS) return -1;
    int i = lv->count;
    str_copy(lv->items[i], text, WLIST_ITEM_LEN);
    lv->tags[i] = tag;
    return lv->count++;
}

/* -------------------------------------------------------------------------- */
/* Text input.                                                                 */
/* -------------------------------------------------------------------------- */

static void textinput_draw(struct widget* w, struct gfx_surface* s) {
    struct w_textinput* t = (struct w_textinput*)w;
    int focused = gui_widget_focused(w);
    cp_fill_plate(s, w->x, w->y, w->w, w->h, WCOL_BOX_BG);
    outline(s, w->x, w->y, w->w, w->h, focused ? WCOL_BOX_FOCUS : WCOL_BOX_EDGE);
    if (focused) cp_focus_ring(s, w->x, w->y, w->w, w->h);

    int maxch = (w->w - 10) / cp_fw();
    int cw;
    if (t->secret) {
        /* §M81 — ONE MARK PER CHARACTER, WHICH REVERSES §M32's DECISION HERE.
         *
         * §M32 drew a FIXED `***` whatever the length, on the argument that a
         * row of bullets publishes how many characters the password has — "the
         * one thing a shoulder-surfer cannot otherwise get".
         *
         * THAT PREMISE IS WEAK AND THE COST WAS NOT.  Anyone close enough to
         * count bullets is close enough to count KEYSTROKES, so the secret was
         * never really being kept; meanwhile the field gave NO SIGN that a key
         * had landed, because one character and five looked identical.
         *
         * Reported from use twice, the second time as the thing that finally
         * named it: *"interesting that it writes three asterisks into the input
         * straight away."*  It had not — one keystroke drew three marks — but
         * from a chair those are the same observation, and it is why a working
         * password change was reported as broken through several rounds: the
         * only feedback the field offered was a constant.
         *
         * *A control that cannot show that it received your input is not
         * protecting a secret; it is hiding its own state.*  Bounded by the
         * width like any other text, so a long password does not draw past the
         * box.
         *
         * THE MASK IS STILL PLAIN ASCII, and that is the §4.66 trap: the first
         * version used 0xB7 for a middle dot, which is what it is in LATIN-1 —
         * and this font is byte-indexed ISO-8859-2, where 0xB7 is a CARON.  The
         * field drew three hooks, reported as "some squiggle gets in", and it
         * raised a far worse suspicion than the bug deserved: that the MASK was
         * being submitted instead of the value.  It is not; drawing never
         * touches `buf`.  `scripts/check-drawn-strings.py` is the check that
         * came out of meeting that trap a fourth time. */
        char mask[64];
        int nm = t->len > maxch ? maxch : t->len;
        if (nm > (int)sizeof mask - 1) nm = (int)sizeof mask - 1;
        for (int i = 0; i < nm; i++) mask[i] = '*';
        mask[nm] = 0;
        cp_text(s, w->x + 5, w->y + (w->h - cp_fh()) / 2, mask, WCOL_TEXT);
        cw = nm;
    } else {
        /* Right-align overflow: show the tail that fits. */
        const char* p = t->buf;
        if (t->len > maxch) p += t->len - maxch;
        cp_text(s, w->x + 5, w->y + (w->h - cp_fh()) / 2, p, WCOL_TEXT);
        cw = t->len > maxch ? maxch : t->len;
    }

    if (focused)                                /* caret after the text */
        gfx_fill(s, w->x + 5 + cw * cp_fw() + 1, w->y + 3, 1, w->h - 6,
                 WCOL_TEXT);
}

static void textinput_mouse(struct widget* w, int lx, int ly, int kind) {
    (void)lx; (void)ly; (void)kind;
    gui_window_focus_widget(w->win, w);         /* click = take keyboard focus */
}

static void textinput_key(struct widget* w, char c) {
    struct w_textinput* t = (struct w_textinput*)w;
    if (c == '\n') {
        if (t->on_submit) t->on_submit(t, w->ctx);
        return;
    }
    if (c == '\b') {
        if (t->len > 0) t->buf[--t->len] = 0;
        return;
    }
    if (c < 0x20 || c > 0x7E) return;           /* printable ASCII only */
    if (t->len < (int)sizeof(t->buf) - 1) {
        t->buf[t->len++] = c;
        t->buf[t->len] = 0;
    }
}

/* M22.5 — clipboard shortcuts.  No in-line cursor (the caret sits at
 * the end by design), so copy/cut act on the whole content. */
static void textinput_keycode(struct widget* w, uint8_t kc, uint8_t mods) {
    struct w_textinput* t = (struct w_textinput*)w;
    if (!(mods & KBD_MOD_CTRL_MASK)) return;
    if (kc == KC_C || kc == KC_X) {
        /* §M32 — A SECRET FIELD IS NOT COPYABLE.  Ctrl+C here would put the
         * password on the SYSTEM clipboard, where §M59 makes it readable by
         * `clip show`, by every other window and by any ring-3 program that
         * opens /dev/clipboard.  *A field that hides its contents on screen and
         * hands them to the clipboard hides nothing.*  Cut is refused with it:
         * clearing the field is harmless, but the copy half is the point.
         *
         * Paste INTO one is still allowed — a password manager is exactly the
         * kind of thing that would want it, and it moves a secret toward the
         * field rather than away from it. */
        if (t->secret) return;
        clipboard_set(t->buf, t->len);
        if (kc == KC_X) { t->len = 0; t->buf[0] = 0; }
    } else if (kc == KC_V) {
        char tmp[sizeof t->buf];
        int n = clipboard_get(tmp, (int)sizeof tmp);
        for (int i = 0; i < n && t->len < (int)sizeof(t->buf) - 1; i++) {
            char c = tmp[i];
            if (c < 0x20 || c > 0x7E) continue;     /* single-line box */
            t->buf[t->len++] = c;
        }
        t->buf[t->len] = 0;
    }
}

static const struct widget_ops textinput_ops = {
    .draw = textinput_draw, .mouse = textinput_mouse, .key = textinput_key, .keycode = textinput_keycode,
};

struct w_textinput* w_textinput_create(struct gui_window* win, int x, int y,
                                       int w, void* ctx) {
    struct w_textinput* t = (struct w_textinput*)kcalloc(1, sizeof(*t));
    if (!t) return NULL;
    widget_init(&t->base, win, x, y, w, 16, &textinput_ops, ctx, 1);
    return t;
}

void w_textinput_set_secret(struct w_textinput* t, int on) {
    if (t) t->secret = on ? 1 : 0;
}

void w_textinput_set(struct w_textinput* t, const char* text) {
    if (!t) return;
    str_copy(t->buf, text, (int)sizeof(t->buf));
    t->len = 0;
    while (t->buf[t->len]) t->len++;
}

/* ===========================================================================
 * §M65 — CLASS REGISTRATIONS for the M22 controls.
 *
 * The controls themselves are untouched: each class is a thin adapter that
 * builds one from a `struct ui_spec` and reports the size it wants.  That is
 * what lets the layout engine place a twenty-line-old listview next to a
 * brand-new checkbox without either knowing about the other — and what lets a
 * panel (or, later, a ring-3 client) name "listview" in DATA instead of
 * calling a function pointer it cannot marshal.
 * ========================================================================= */


static struct widget* cls_label_create(struct gui_window* win,
                                       const struct ui_spec* sp) {
    struct w_label* l = w_label_create(win, 0, 0, 120, sp->text);
    return l ? &l->base : NULL;
}
static void cls_label_measure(struct widget* w, int avail_w, int* min_w,
                              int* pref_w, int* pref_h) {
    struct w_label* l = (struct w_label*)w;
    /* `strlen * cp_fw()` is the habit the 8x8 font taught and console_plate.h
     * warns about in capitals: cp_fw is a DIGIT's advance, correct for a column
     * of numbers and approximate for anything else.  In a UI_GRID the label
     * column is sized to the widest label's pref_w, so under-measuring by a few
     * pixels clips the longest caption and only the longest — "SEGMENTED" came
     * out as "SEGMENTEI" in the widget gallery while every shorter label fit,
     * which reads as one bad string rather than as a measuring rule. */
    *min_w  = cp_fw() * 4;
    *pref_w = cp_text_w(lstr(l->text));
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = cp_fh() + 4;
}
static void cls_label_settext(struct widget* w, const char* t) {
    w_label_set((struct w_label*)w, t);
}
static int cls_label_gettext(struct widget* w, char* out, int cap) {
    struct w_label* l = (struct w_label*)w;
    int i = 0; for (; l->text[i] && i < cap - 1; i++) out[i] = l->text[i];
    if (cap) out[i] = 0;
    return i;
}
WIDGET_CLASS(wc_label) = {
    .ops = &label_ops,
    .name = "label", .create = cls_label_create, .measure = cls_label_measure,
    .set_text = cls_label_settext, .get_text = cls_label_gettext,
};

/* The button's click reaches the window's ONE event sink rather than a
 * per-widget callback: (id, type) is what a boundary can carry. */
static void cls_button_click(struct w_button* b, void* ctx) {
    (void)ctx;
    ui_emit(&b->base, UI_EV_CLICK, 0);
}
static struct widget* cls_button_create(struct gui_window* win,
                                        const struct ui_spec* sp) {
    struct w_button* b = w_button_create(win, 0, 0, 90, 22, sp->text,
                                         cls_button_click, NULL);
    return b ? &b->base : NULL;
}
static void cls_button_measure(struct widget* w, int avail_w, int* min_w,
                               int* pref_w, int* pref_h) {
    struct w_button* b = (struct w_button*)w;
    /* Rules 1-3: content width plus the design's horizontal padding, the
     * convention's control height.  The old `strlen * cp_fw() + 20` / 22 pair
     * was two 8x8-era constants that stopped tracking the type in §M69. */
    *min_w  = cp_ctrl_pad_x() * 2 + cp_fw() * 2;
    /* Measured on the TRANSLATION, or the box is sized for the key and drawn
     * with the text — which is how a button ends up clipping its own label in
     * one language and not another. */
    *pref_w = cp_text_w(lstr(b->text)) + 2 * cp_ctrl_pad_x();
    if (*pref_w > avail_w) *pref_w = avail_w;
    *pref_h = cp_btn_h();                /* rule 0: a button is a TARGET */
}
static void cls_button_settext(struct widget* w, const char* t) {
    str_copy(((struct w_button*)w)->text, t, (int)sizeof ((struct w_button*)w)->text);
}
WIDGET_CLASS(wc_button) = {
    .ops = &button_ops,
    .name = "button", .create = cls_button_create, .measure = cls_button_measure,
    .set_text = cls_button_settext,
};

static void cls_list_select(struct w_listview* lv, int idx, void* ctx) {
    (void)ctx;
    ui_emit(&lv->base, UI_EV_CHANGE, idx);
}
static void cls_list_activate(struct w_listview* lv, int idx, void* ctx) {
    (void)ctx;
    ui_emit(&lv->base, UI_EV_ACTIVATE, idx);
}
static struct widget* cls_list_create(struct gui_window* win,
                                      const struct ui_spec* sp) {
    (void)sp;
    struct w_listview* lv = w_listview_create(win, 0, 0, 200, 120, NULL);
    if (!lv) return NULL;
    lv->on_select   = cls_list_select;
    lv->on_activate = cls_list_activate;
    return &lv->base;
}
static void cls_list_measure(struct widget* w, int avail_w, int* min_w,
                             int* pref_w, int* pref_h) {
    (void)w;
    *min_w  = 80;
    *pref_w = avail_w;                  /* a list takes what it is given */
    *pref_h = 120;                      /* …and grows by weight, not by wish */
}
static int  cls_list_get(struct widget* w) { return ((struct w_listview*)w)->sel; }
static void cls_list_set(struct widget* w, int v) {
    struct w_listview* lv = (struct w_listview*)w;
    if (v >= 0 && v < lv->count) lv->sel = v;
}
WIDGET_CLASS(wc_listview) = {
    .ops = &listview_ops,
    .name = "listview", .create = cls_list_create, .measure = cls_list_measure,
    .get_value = cls_list_get, .set_value = cls_list_set,
};

static void cls_text_submit(struct w_textinput* t, void* ctx) {
    (void)ctx;
    ui_emit(&t->base, UI_EV_SUBMIT, 0);
}
static struct widget* cls_text_create(struct gui_window* win,
                                      const struct ui_spec* sp) {
    struct w_textinput* t = w_textinput_create(win, 0, 0, 160, NULL);
    if (!t) return NULL;
    if (sp->text) w_textinput_set(t, sp->text);
    t->on_submit = cls_text_submit;
    return &t->base;
}
static void cls_text_measure(struct widget* w, int avail_w, int* min_w,
                             int* pref_w, int* pref_h) {
    /* §M69 rule 0 — a text box is AIMED AT (you click into it to type), so it
     * takes the design's control height rather than the beside-text one.  The
     * literal 22 was an 8x8-era count that did not move when the type became a
     * runtime fact, and `w->h > 0 ? w->h : 22` quietly froze whatever the
     * caller happened to construct it with. */
    (void)w;
    *min_w  = cp_fw() * 8;
    *pref_w = avail_w;
    *pref_h = cp_btn_h();
}
static void cls_text_settext(struct widget* w, const char* t) {
    w_textinput_set((struct w_textinput*)w, t);
}
static int cls_text_gettext(struct widget* w, char* out, int cap) {
    struct w_textinput* t = (struct w_textinput*)w;
    int i = 0; for (; t->buf[i] && i < cap - 1; i++) out[i] = t->buf[i];
    if (cap) out[i] = 0;
    return i;
}
WIDGET_CLASS(wc_textinput) = {
    .ops = &textinput_ops,
    .name = "textinput", .create = cls_text_create, .measure = cls_text_measure,
    .set_text = cls_text_settext, .get_text = cls_text_gettext,
};
