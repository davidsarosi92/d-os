/* =============================================================================
 * wm.c — the window manager: lifecycle, z-order, focus, popups and modality
 * (§M70; M22.2).
 *
 * Extracted from gui.c.  This is the half of the compositor that answers
 * "which window", while gui.c answers "which pixels".  It owns `windows[]`,
 * `zorder[]` and `focused_win` — all under `state_lock`, and the `_locked`
 * suffix in a name means the caller already holds it (the mouse path does,
 * because the IRQ took it before routing the click).
 *
 * THREE THINGS HERE ARE STRUCTURAL RATHER THAN INCIDENTAL:
 *
 * 1. THE TITLE BUTTONS ARE A COUNT, NOT THREE BOOLEANS.  They are numbered
 *    from the right edge, so "show fewer" is "stop at a lower index" and the
 *    painter, the hit test and the click handler cannot disagree about which
 *    boxes exist.  §4.79 is what that avoids: the painter and the hit test had
 *    computed the same box differently (row 2 px off, gaps 3 vs 4), so the top
 *    edge of every button was dead and the strip above it live — a control
 *    that draws correctly and hit-tests wrongly is invisible in a screenshot.
 *    `title_btn_rect()` is the one place that arithmetic happens.
 *
 * 2. MODALITY IS FOUR GATES, and they are written down together because a
 *    reader who finds one will conclude the feature is half built:
 *    `topmost_at` answers "the modal, or nothing" (covering hover, the
 *    title-button highlight, the right/middle press and the drag start);
 *    the left-press path swallows anything outside it BEFORE the taskbar's
 *    refusal and the desktop fallthrough — gate 1 alone is not enough, because
 *    `topmost_at` returning NULL is exactly how a click on the WALLPAPER is
 *    recognised, so without this a modal would still let you launch shortcuts
 *    behind it; Alt-Tab is refused; and the 45 % backdrop is what makes the
 *    other three LEGIBLE, since swallowed clicks with an undimmed screen
 *    behind them are indistinguishable from a machine that has stopped
 *    responding.  The claim is RELEASED in `destroy_window`, on every route —
 *    a dialog leaves four ways and a screen-wide claim that survives one of
 *    them locks the desktop.
 *
 * 3. `on_dispose` FIRES FROM `destroy_window` ON EVERY ROUTE.  The dosgui
 *    bridge must not have to infer a window's death from the route it happened
 *    to take: a handle whose lifetime is INFERRED is a handle that leaks, and
 *    §M54's symptom was NetSurf refusing to open after four crashes.
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
#include "icons.h"
#include "locale.h"
#include "task.h"
#include "timer.h"
#include "printf.h"
#include "kmalloc.h"
#include "config.h"
#include "vc.h"
#include "shell_provider.h"
#include <stdint.h>
#include <stddef.h>


struct gui_popup_state popup;

struct gui_window* modal_win = NULL;

/* Where the focus was when the modal went up, so dismissing it puts the user
 * back where they were rather than on whatever happens to be topmost. */
struct gui_window* modal_prev_focus = NULL;

/* Instrument, not decoration.  Modality is claimed on one task and PAINTED on
 * another, so "the backdrop is missing" has two completely different causes —
 * the claim never happened, or the claim happened and the compositor cannot
 * see the window in its z-order — and they look identical on screen. */
volatile unsigned modal_dbg_seen = 0, modal_dbg_missing = 0;




void title_btn_rect(int wx, int wy, int ww, int idx,
                           int* rx, int* ry, int* rw, int* rh) {
    const int b = cp_window_btn();
    *rw = b;
    *rh = b;
    *ry = wy + (TITLE_H - b) / 2;
    *rx = wx + ww - BORDER - 4 - (idx + 1) * b - idx * TITLE_BTN_GAP;
}
/* Which button is at (px, py), or -1.  The ONE hit test — the compositor's
 * hover and the click handler both call it, so they cannot drift apart the way
 * the drawing and the click handler just did. */
int title_btn_at_n(int wx, int wy, int ww, int px, int py, int count) {
    for (int i = 0; i < count; i++) {
        int bx, by, bw, bh;
        title_btn_rect(wx, wy, ww, i, &bx, &by, &bw, &bh);
        if (px >= bx && px < bx + bw && py >= by && py < by + bh) return i;
    }
    return -1;
}
int title_btn_count(const struct gui_window* win) {
    if (win && win->no_close) return 0;        /* the lock: nothing to press */
    return (win && win == modal_win) ? 1 : TB_COUNT;
}

/* §M82 fix — see gui_priv.h's `no_close`. */
int gui_window_set_uncloseable(struct gui_window* win, int on) {
    if (!win || !win->used) return -1;
    win->no_close = on ? 1 : 0;
    return 0;
}
/* §M65 — the toolkit's per-window slot.  Accessors rather than a public field
 * so ui.c does not need gui.c's private window struct. */
/* Which widget has the keyboard right now — controls draw their focus ring
 * from it.  Read-only; focus is CHANGED through gui_window_focus_widget. */
struct widget* gui_window_focused_widget(struct gui_window* win) {
    return win ? win->focusw : NULL;
}
/* §M65 — where this window's CONTENT starts on screen.  A widget knows its
 * position inside the content; the popup is a compositor overlay in screen
 * coordinates, and something has to bridge the two.  Exposed rather than
 * exporting BORDER/TITLE_H, so the chrome's geometry stays gui.c's business. */
void gui_window_content_origin(struct gui_window* win, int* sx, int* sy) {
    if (sx) *sx = win ? win->x + BORDER : 0;
    if (sy) *sy = win ? win->y + TITLE_H : 0;
}
/* §M65 — ask for this window's widgets to be laid out and repainted.  The
 * app-host does it for a hosted window, the compositor for a hostless one. */
/* Move the keyboard focus to the next (or previous) FOCUSABLE widget, wrapping.
 * The window's widget list is in creation order, which is the order the layout
 * placed them and therefore the order a person reads them in. */
void gui_window_focus_cycle(struct gui_window* win, int backwards) {
    if (!win || !win->used) return;
    struct widget* first = NULL;
    struct widget* prev  = NULL;
    struct widget* pick  = NULL;
    int take_next = (win->focusw == NULL);

    for (struct widget* w = win->widgets; w; w = w->next) {
        if (!w->focusable) continue;
        if (!first) first = w;
        if (backwards) {
            if (w == win->focusw) { pick = prev; break; }
            prev = w;
        } else {
            if (take_next) { pick = w; break; }
            if (w == win->focusw) take_next = 1;
        }
    }
    if (!pick) {
        /* Wrapped: forwards lands on the first, backwards on the last. */
        if (backwards) { for (struct widget* w = win->widgets; w; w = w->next)
                             if (w->focusable) pick = w; }
        else pick = first;
    }
    if (!pick) return;                      /* nothing focusable in this window */
    gui_window_focus_widget(win, pick);
    gui_window_request_redraw(win);
}
void gui_window_request_layout(struct gui_window* win) {
    if (!win || !win->used) return;
    win->layout_pending = 1;
    need_frame = 1;
}
void* gui_window_ui(struct gui_window* win)            { return win ? win->ui_state : NULL; }
void  gui_window_set_ui(struct gui_window* win, void* p) { if (win) win->ui_state = p; }
int gui_wm_windows_locked(struct gui_window** out, int max) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS && n < max; i++) {
        if (!windows[i].used) continue;
        /* §M69 — A MODAL IS NOT IN THE WINDOW LIST.  It was getting a taskbar
         * button that could not be pressed (gate 2 swallows clicks over the
         * chrome), and a dead control pointing at the one window you cannot
         * miss is worse than no control: it invites a press and answers with
         * nothing.  Excluding it here rather than in the taskbar covers
         * Alt-Tab in the same edit — which already refuses to move focus while
         * a modal is up, so listing it there was equally pointless. */
        if (&windows[i] == modal_win) continue;
        out[n++] = &windows[i];
    }
    return n;
}
int gui_wm_windows(struct gui_window** out, int max) {
    uint32_t fl = spin_lock_irqsave(&state_lock);
    int n = gui_wm_windows_locked(out, max);
    spin_unlock_irqrestore(&state_lock, fl);
    return n;
}
struct gui_window* gui_wm_focused(void) { return focused_win; }
const char* gui_window_title(struct gui_window* w) {
    /* §M69 — THROUGH THE CATALOGUE, here rather than at each consumer: the
     * title bar, the taskbar button and Alt-Tab all ask this, and three
     * lookups would be three chances for one to show the untranslated key.
     * A window whose title is an ordinary string is unaffected — lstr falls
     * back to its argument. */
    return w ? lstr(w->title) : "";
}
void gui_window_clear_widgets(struct gui_window* win) {
    if (!win || win->kind != WIN_APP) return;
    /* An open popup belongs to a widget that is about to stop existing, and it
     * owns the next click wherever it lands (§M65). */
    if (popup.active && popup.owner == win) popup.active = 0;
    app_widgets_reset(win);
}
int window_set_size(struct gui_window* win, int outer_w, int outer_h) {
    int cw = outer_w - 2 * BORDER;
    int ch = outer_h - TITLE_H - BORDER;
    struct gfx_surface ns;
    if (gfx_surface_init(&ns, cw, ch) != 0) return -1;
    gfx_fill(&ns, 0, 0, cw, ch, COL_WIN_BG);

    spin_lock(&win->lock);
    struct gfx_surface old = win->surf;
    win->surf = ns;
    if (win->kind == WIN_TERM) {
        int ncols = (cw - 2 * PAD) / cp_cell_w();
        int nrows = (ch - 2 * PAD) / cp_cell_h();
        if (ncols > gmax_cols) ncols = gmax_cols;
        if (nrows > gmax_rows) nrows = gmax_rows;
        if (win->cells) {
            int excess = win->crow - (nrows - 1);
            if (excess > 0) {
                /* §M58 — a shrink evicts rows off the top exactly as a scroll
                 * does, so they belong in the history for the same reason. */
                for (int r = 0; r < excess; r++) {
                    gterm_sb_push(win, win->cells + (size_t)r * gmax_cols);
                    win->scrolled++;
                }
                for (int r = 0; r < gmax_rows - excess; r++) {
                    char* d = win->cells + (size_t)r * gmax_cols;
                    const char* srow = d + (size_t)excess * gmax_cols;
                    for (int c = 0; c < gmax_cols; c++) d[c] = srow[c];
                }
                for (int r = gmax_rows - excess; r < gmax_rows; r++) {
                    char* d = win->cells + (size_t)r * gmax_cols;
                    for (int c = 0; c < gmax_cols; c++) d[c] = 0;
                }
                win->crow = nrows - 1;
            }
            win->cols = ncols;
            win->rows = nrows;
            if (win->ccol >= ncols) win->ccol = ncols - 1;
            gterm_rerender_locked(win);
        } else {
            win->cols = ncols;
            win->rows = nrows;
        }
    }
    spin_unlock(&win->lock);

    gfx_surface_free(&old);

    /* M22.7 — the app-host owns widget layout + drawing; ask it to re-layout
     * (it runs on_layout + app_redraw next loop).  Set even at creation time:
     * the host processes it once the open fn has created the widgets. */
    if (win->kind == WIN_APP)
        win->layout_pending = 1;
    return 0;
}
int gui_window_hosted(struct gui_window* win) {
    return win && win->used && win->host_task != NULL;
}

int gui_window_hosted_by_current(struct gui_window* win) {
    if (!win || !win->used) return 1;           /* nothing to violate */
    if (!win->host_task) return 1;              /* client-managed — no host */
    return win->host_task == task_current();
}

const char* gui_window_host_name(struct gui_window* win) {
    if (!win || !win->host_task) return "(none)";
    return win->host_task->name;
}

/* §M81 — CAUGHT AT THE DOOR, not by walking the room later.
 *
 * `widget_init` stamps `inited` and THEN calls this, so a widget arriving here
 * unstamped was assembled by hand — the §M63 constructor defect.  Checking it
 * here rather than by walking live windows from an audit matters for a reason
 * the audit itself got wrong first: *walking another task's widget list is
 * exactly the §M22.7 violation the neighbouring audit checks for*, and the
 * list is rebuilt wholesale on the host at every layout.  A check that has to
 * break a rule to observe it is not a check. */
void gui_window_add_widget(struct gui_window* win, struct widget* w) {
    if (!win || win->kind != WIN_APP || !w) return;
    if (w->inited != WIDGET_INITED) {
        if (widget_uninited++ == 0)
            kprintf("gui: a widget was added to '%s' WITHOUT widget_init - its "
                    "base fields were assembled by hand, which is how keyboard "
                    "navigation silently never worked in an item view (§M63)\n",
                    win->title);
    }
    struct widget** p = &win->widgets;
    while (*p) p = &(*p)->next;
    *p = w;
}
void gui_window_focus_widget(struct gui_window* win, struct widget* w) {
    if (!win || win->kind != WIN_APP) return;
    win->focusw = w;
}
int gui_widget_focused(struct widget* w) {
    return w && w->win && w->win->focusw == w;
}
void gui_window_outer_for_content(int cw, int ch, int* ow, int* oh) {
    if (ow) *ow = cw + 2 * BORDER;
    if (oh) *oh = ch + TITLE_H + BORDER;
}
int gui_window_content_size(struct gui_window* win, int* w, int* h) {
    if (!win) return -1;
    if (w) *w = win->surf.w;
    if (h) *h = win->surf.h;
    return 0;
}
void* gui_window_ctx(struct gui_window* win) {
    return win ? win->app_ctx : NULL;
}
void gui_window_request_redraw(struct gui_window* win) {
    if (win && win->used && win->kind == WIN_APP) app_redraw(win);
}
/* M22.7 — redraw + damage only a CONTENT sub-rect (widget-local coords), not
 * the whole window.  A frequently-refreshing app (the Task Manager) uses it
 * to repaint just its listview each second instead of the entire window
 * chrome — the widget clip confines the draw, and only that screen rect is
 * damaged. */
void gui_window_request_redraw_rect(struct gui_window* win,
                                    int cx, int cy, int cw, int ch) {
    if (!win || !win->used || win->kind != WIN_APP) return;
    if (cw <= 0 || ch <= 0) return;
    spin_lock(&win->lock);
    gfx_set_clip(&win->surf, cx, cy, cw, ch);
    gfx_fill(&win->surf, cx, cy, cw, ch, COL_WIN_BG);
    widget_draw_all(win->widgets, &win->surf);  /* clip keeps it to the rect */
    gfx_clear_clip(&win->surf);
    spin_unlock(&win->lock);
    gui_damage(win->x + BORDER + cx, win->y + TITLE_H + cy, cw, ch);
}
/* §M26 — paint a raw pixel block into a window's content surface + composite it
 * (the Wayland compositor bridge: a wl_surface's committed buffer becomes a real
 * window's contents).  Coords are content-relative (exclude the chrome). */
void gui_window_blit(struct gui_window* win, int x, int y,
                     const uint32_t* px, int w, int h, int stride) {
    if (!win || !win->used || win->kind != WIN_APP || !px || w <= 0 || h <= 0) return;
    struct gfx_surface src;
    src.w = w; src.h = h; src.stride = stride; src.px = (uint32_t*)px; src.owns_px = 0;
    gfx_clear_clip(&src);
    spin_lock(&win->lock);
    gfx_blit(&win->surf, x, y, &src, 0, 0, w, h);
    spin_unlock(&win->lock);
    gui_damage(win->x + BORDER + x, win->y + TITLE_H + y, w, h);
}
/* Read a content-surface pixel back (for self-tests). */
uint32_t gui_window_pixel(struct gui_window* win, int x, int y) {
    if (!win || !win->used || x < 0 || y < 0 ||
        x >= win->surf.w || y >= win->surf.h) return 0;
    return win->surf.px[y * win->surf.stride + x];
}
/* §M26 — set the input sink (see gui.h). */
void gui_window_set_input_hook(struct gui_window* win,
        void (*fn)(struct gui_window*, const struct gui_input*, void*), void* ctx) {
    if (!win) return;
    win->input_hook = fn;
    win->input_ctx  = ctx;
}
void gui_window_set_on_close(struct gui_window* win,
                             void (*fn)(struct gui_window*)) {
    if (win) win->on_close = fn;
}
void gui_window_close(struct gui_window* win) {
    if (win && win->used) {                 /* M22.3: TERM windows too */
        win->want_close = 1;
        need_frame = 1;
    }
}
/* Orderly close (2026-09-25) — see gui_priv.h's `close_guard`. */
void gui_window_set_close_guard(struct gui_window* win,
                                int (*guard)(struct gui_window*, int reason)) {
    if (win) win->close_guard = guard;
}
/* A window's lifetime identity for code outside the compositor (apps see the
 * struct as opaque): a slot is reused, a serial never is. */
uint32_t gui_window_serial(struct gui_window* win) { return win ? win->serial : 0; }
int gui_window_alive(struct gui_window* win, uint32_t serial) {
    return win && win->used && win->serial == serial;
}
void gui_window_close_now(struct gui_window* win) {
    if (win && win->used) {
        win->close_confirmed = 1;
        win->want_close = 1;
        need_frame = 1;
    }
}
/* §M42 — a CLIENT-MANAGED WIN_APP window (the dosgui bridge for a ring-3 client
 * like NetSurf).  Sever the host_task binding: the client is a DETACHED task
 * reaped by init, not a compositor-owned app-host, so the compositor must NOT
 * read host_task->state or reap it (that races init → task-table corruption →
 * GUI wedge on the next open).  With host_task == NULL, apply_pending's WIN_APP
 * teardown never observes the task's death — disposal is driven only by the
 * client's explicit release below. */
void gui_window_set_client_managed(struct gui_window* win, int client_pid) {
    if (win) { win->host_task = NULL; win->client_pid = client_pid; }
}
/* §M54 — see gui.h.  One slot, set at creation by the bridge that owns the
 * handle; the compositor calls it exactly once when the struct is disposed. */
void gui_window_set_dispose_cb(struct gui_window* win,
                               void (*cb)(struct gui_window*, void*), void* ctx) {
    if (win) { win->on_dispose = cb; win->dispose_ctx = ctx; }
}
/* §M42 — the client (dosgui_destroy, from dos_finalise) says it is finished with
 * the window and will not touch it again.  Mark it disposable: want_close makes
 * apply_pending pick it up; host_released makes it skip the host-coordination /
 * host_task->state read and dispose immediately (reap_gui_host(NULL) is a
 * no-op), so no init-owned task struct is ever touched. */
void gui_window_client_release(struct gui_window* win) {
    if (win && win->used) {
        win->host_released = 1;
        win->want_close    = 1;
        need_frame         = 1;
    }
}
void gui_window_set_tick(struct gui_window* win,
                         void (*fn)(struct gui_window*)) {
    if (win) win->on_tick = fn;
}
int gui_window_minimized(struct gui_window* w) {
    return w ? w->minimized : 0;
}
/* §M42 — has the window been asked to close (its X button was clicked)?  A
 * WIN_APP that drives itself (NetSurf, via the dosgui bridge) isn't running the
 * app-host loop that would normally see want_close, so it polls this and quits
 * on its own; the compositor then disposes the window when the task dies. */
int gui_window_want_close(struct gui_window* w) {
    return (w && w->used) ? w->want_close : 0;
}
void gui_window_raise(struct gui_window* win) {
    if (!win || !win->used) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    raise_window(win);
    focused_win = win;
    spin_unlock_irqrestore(&state_lock, fl);
    if (win->kind == WIN_TERM) vc_focus(win->vc);
    gui_damage_all();
}
void gui_window_set_title(struct gui_window* win, const char* title) {
    if (!win || !win->used || !title) return;
    str_copy(win->title, title, (int)sizeof(win->title));
    gui_damage_win(win);                    /* repaint chrome (and taskbar
                                             * on the next full frame) */
}
/* Returns non-zero when something about the window's LOOK changed — it was
 * raised, or un-minimized.  A focus change is the caller's to notice; this
 * reports the part only the WM can see. */
int gui_wm_focus_raise_locked(struct gui_window* w) {
    if (!w || !w->used) return 0;
    int changed = w->minimized;
    w->minimized = 0;                       /* activating always restores */
    if (raise_window(w)) changed = 1;
    focused_win = w;
    if (w->kind == WIN_TERM) vc_focus(w->vc);
    return changed;
}
/* M22.5 — maximize/restore toggle.  WM lock held (mouse IRQ).  The
 * geometry change goes through the pending-resize handoff so the
 * surface realloc happens on the compositor task, exactly like a
 * grip-resize release. */
void toggle_maximize_locked(struct gui_window* w) {
    if (!w || !w->used) return;
    if (!w->maximized) {
        w->sav_x = w->x;  w->sav_y = w->y;
        w->sav_w = w->w;  w->sav_h = w->h;
        /* §M88 — maximize fills the MONITOR the window is on (its centre),
         * not the desktop: across two screens it would straddle the bezel. */
        int ox, oy, ow, oh;
        gui_output_workarea(w->x + w->w / 2, w->y + w->h / 2, &ox, &oy, &ow, &oh);
        w->x = ox;  w->y = oy;
        w->pending_w = ow;                      /* work-area aware: height */
        w->pending_h = oh;                      /* stops above the taskbar */
        w->maximized = 1;
    } else {
        w->x = w->sav_x;  w->y = w->sav_y;
        w->pending_w = w->sav_w;
        w->pending_h = w->sav_h;
        w->maximized = 0;
    }
    need_frame = 1;
}
/* Taskbar-button semantics (Windows-style): minimized → restore +
 * focus; focused → minimize; else → focus + raise.  WM lock held. */
void gui_wm_taskbar_activate_locked(struct gui_window* w) {
    if (!w || !w->used) return;
    if (w->minimized) {
        gui_wm_focus_raise_locked(w);
    } else if (focused_win == w) {
        w->minimized = 1;
        struct gui_window* nf = top_visible_locked();
        focused_win = nf;
        if (nf && nf->kind == WIN_TERM) vc_focus(nf->vc);
    } else {
        gui_wm_focus_raise_locked(w);
    }
}
/* Open the popup.  `items` is ONE string with '\n' between entries — flat, so
 * the same call survives being marshalled from ring 3 later, and "-" is a
 * separator.  Called on the owner's app-host task. */
void gui_popup_open(struct gui_window* owner, int sx, int sy,
                    const char* items, int tag) {
    if (!items) return;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    popup.count = 0;
    popup.hover = -1;
    int widest = 0;
    const char* p = items;
    while (*p && popup.count < POPUP_MAX_ITEMS) {
        int n = 0;
        while (*p && *p != '\n' && n < POPUP_ITEM_LEN - 1)
            popup.items[popup.count][n++] = *p++;
        popup.items[popup.count][n] = 0;
        while (*p && *p != '\n') p++;               /* drop an over-long tail */
        if (*p == '\n') p++;
        if (n > widest) widest = n;
        popup.count++;
    }
    popup.w = widest * GFX_GLYPH_W + 24;
    popup.h = popup.count * POPUP_ROW_H + 6;
    popup.x = sx;
    popup.y = sy;
    /* Keep it on screen: a menu opened near the right edge belongs to the LEFT
     * of the pointer, which is what every toolkit does and what stops the last
     * entry from being unreachable. */
    if (popup.x + popup.w > fbsurf.w) popup.x = fbsurf.w - popup.w;
    if (popup.y + popup.h > fbsurf.h) popup.y = fbsurf.h - popup.h;
    if (popup.x < 0) popup.x = 0;
    if (popup.y < 0) popup.y = 0;
    popup.owner = owner;
    popup.tag = tag;
    popup.active = 1;
    spin_unlock_irqrestore(&state_lock, fl);
    gui_damage_all();
}
void gui_popup_close(void) {
    if (!popup.active) return;
    popup.active = 0;
    gui_damage_all();
}
int gui_popup_active(void) { return popup.active; }
/* §M69 — claim (or release) the session-wide modal.  Returns 0 on success,
 * -1 if somebody else already holds it.
 *
 * REFUSED RATHER THAN QUEUED, and the caller is expected to act on that: a
 * dialog that turns up some seconds after the action that raised it asks its
 * question of a user who has already moved on.  The refusal is loud (a klog
 * line) because the alternative — a second dialog silently opening non-modal —
 * would look almost right and behave nothing like it.
 *
 * Raise + focus are part of the claim, not something the caller has to
 * remember: modality means "this window has the focus and is on top", and
 * leaving either to a second call is leaving a state where it half holds. */
int gui_window_set_modal(struct gui_window* win, int on) {
    if (!win || !win->used || win->kind != WIN_APP) return -1;
    uint32_t fl = spin_lock_irqsave(&state_lock);
    if (on) {
        if (modal_win && modal_win->used && modal_win != win) {
            spin_unlock_irqrestore(&state_lock, fl);
            kprintf("gui: modal refused for '%s' - '%s' already holds it\n",
                    win->title, modal_win->title);
            return -1;
        }
        if (modal_win != win) modal_prev_focus = focused_win;
        modal_win = win;
        win->minimized = 0;
        raise_window(win);
        focused_win = win;
    } else if (modal_win == win) {
        modal_win = NULL;
        modal_prev_focus = NULL;
    }
    spin_unlock_irqrestore(&state_lock, fl);
    kprintf("gui: modal %s by '%s'\n", on ? "CLAIMED" : "released", win->title);
    /* The backdrop covers the screen, so putting one up or taking one down is
     * the one case where whole-screen damage is the honest amount. */
    gui_damage_all();
    return 0;
}
int gui_modal_active(void) { return modal_win && modal_win->used; }
/* Which row is (sx,sy) over?  -1 = outside, or a separator (which is not a
 * choice and must not behave like one). */
int popup_row_at(int sx, int sy) {
    if (!popup.active) return -1;
    if (sx < popup.x || sx >= popup.x + popup.w) return -1;
    if (sy < popup.y + 3 || sy >= popup.y + 3 + popup.count * POPUP_ROW_H) return -1;
    int i = (sy - popup.y - 3) / POPUP_ROW_H;
    if (i < 0 || i >= popup.count) return -1;
    if (popup.items[i][0] == '-' && !popup.items[i][1]) return -1;
    return i;
}
/* §M81 — the close notification, on whichever route reached it.
 *
 * THE SLOT IS CLEARED BEFORE THE CALLBACK RUNS, and the order matters: an
 * `on_close` may open something, and a handler that found its own singleton
 * still pointing at the window being torn down would raise a corpse.  Same
 * argument as `on_dispose` firing before any teardown (§M54) — the owner has to
 * be told the object is gone while it is still safe to be told. */
void win_run_on_close(struct gui_window* win) {
    if (!win) return;
    if (win->app_slot) {
        if (*win->app_slot == win) *win->app_slot = NULL;
        win->app_slot = NULL;                   /* fire once, never re-enter */
    }
    if (win->on_close) win->on_close(win);
}

void destroy_window(struct gui_window* win) {
    /* M22.7 — a released WIN_APP already ran on_close + freed its widgets on
     * its host task; don't repeat it here.  WIN_TERM keeps the old path. */
    if (!win->host_released) win_run_on_close(win);

    /* §M54 — tell the handle owner the window is going away.  Unconditional and
     * BEFORE any teardown, because the whole point is that it must not depend
     * on which route got us here: on_close above is skipped for a released
     * window, and the crash route sets host_released, which is exactly the
     * combination that used to leave the dosgui bridge holding a handle to a
     * window that no longer exists. */
    if (win->on_dispose) {
        void (*cb)(struct gui_window*, void*) = win->on_dispose;
        void* ctx = win->dispose_ctx;
        win->on_dispose = NULL;                 /* fire once, never re-enter */
        win->dispose_ctx = NULL;
        cb(win, ctx);
    }

    uint32_t fl = spin_lock_irqsave(&state_lock);
    int i;
    for (i = 0; i < zcount && zorder[i] != win; i++) ;
    if (i < zcount) {
        for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
        zcount--;
    }
    if (drag_win == win) { drag = DRAG_NONE; drag_win = NULL; }
    /* §M69 — RELEASE THE MODAL CLAIM, on every disposal route.  This is the
     * same reason `on_dispose` is fired unconditionally a few lines above: a
     * dialog can leave by its own OK button, by the X, by the Esc hatch, or
     * because its host was force-killed, and a claim on the whole screen that
     * survives ONE of those routes locks the desktop with nothing on screen
     * to point at.  So it is cleared where the window struct dies, not where
     * the dialog thinks it is closing. */
    int was_modal = (modal_win == win);
    struct gui_window* restore = modal_prev_focus;
    if (was_modal) { modal_win = NULL; modal_prev_focus = NULL; }
    struct gui_window* newfocus =
        (focused_win == win) ? top_visible_locked() : focused_win;
    /* Put the focus back where the dialog took it from, if that window is
     * still alive — after a confirmation the user is carrying on with what
     * they were doing, not with whatever happens to be topmost. */
    if (was_modal && restore && restore->used && restore != win &&
        !restore->minimized)
        newfocus = restore;
    focused_win = newfocus;
    spin_unlock_irqrestore(&state_lock, fl);

    if (newfocus && newfocus->kind == WIN_TERM) vc_focus(newfocus->vc);

    struct widget* w = win->widgets;
    while (w) {
        struct widget* nx = w->next;
        if (w->ops && w->ops->destroy) w->ops->destroy(w);  /* M22.5 */
        kfree(w);
        w = nx;
    }
    win->widgets = NULL;
    win->focusw  = NULL;
    gfx_surface_free(&win->surf);
    if (win->cells)   { kfree(win->cells); win->cells = NULL; }
    if (win->sb)      { kfree(win->sb);    win->sb = NULL; win->sb_cap = 0; }
    if (win->app_ctx) { kfree(win->app_ctx); win->app_ctx = NULL; }
    if (win->ui_state) { kfree(win->ui_state); win->ui_state = NULL; }
    win->used = 0;
    gui_damage_all();
}
struct gui_window* topmost_at(int px, int py) {
    /* §M69 gate 1 — while a modal is up this question has exactly two
     * answers: the modal, or nothing.  Putting it here rather than at each
     * caller is deliberate — hover, the title-button highlight, the right and
     * middle press and the drag start all ask "what is under the pointer",
     * and a gate applied to three of the four is a gate nobody can rely on
     * (§M33's nine entry points, same argument). */
    if (modal_win && modal_win->used)
        return modal_hit(px, py) && !modal_win->minimized ? modal_win : NULL;

    for (int i = zcount - 1; i >= 0; i--) {
        struct gui_window* w = zorder[i];
        if (w->minimized) continue;             /* M22.3 */
        if (px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h)
            return w;
    }
    return NULL;
}
/* Returns non-zero when the z-order actually MOVED.  A click on the window
 * that is already on top changes nothing, and the caller needs to know that:
 * damaging a window because it was clicked, rather than because its look
 * changed, is what made every press cost a full-window repaint. */
int raise_window(struct gui_window* win) {
    int i;
    for (i = 0; i < zcount && zorder[i] != win; i++) ;
    if (i >= zcount) return 0;
    if (i == zcount - 1) return 0;              /* already topmost */
    for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = win;
    return 1;
}
struct gui_window* window_alloc(const char* title, enum win_kind kind,
                                       int x, int y, int w, int h) {
    if (w < MIN_W) w = MIN_W;
    if (h < MIN_H) h = MIN_H;
    if (h > work_h) h = work_h;

    /* M22.7 — the slot scan + claim runs under state_lock: app-host tasks
     * now create windows concurrently, so an unlocked "find !used then set
     * used=1" would hand the same slot to two apps.  All fields are set
     * before used=1 (the last store), so a compositor pass that observes
     * used==1 sees a fully-initialised window (x86 TSO — no barrier). */
    uint32_t fl = spin_lock_irqsave(&state_lock);
    struct gui_window* win = NULL;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (!windows[i].used) { win = &windows[i]; break; }
    if (!win) {
        spin_unlock_irqrestore(&state_lock, fl);
        kprintf("gui: window pool exhausted\n");
        return NULL;
    }

    win->kind = kind;
    /* §M88 — an app places its window in the PRIMARY's coordinates (it centres
     * on gui_screen_w/h); the desktop may start further left or up. */
    x += prim_x;  y += prim_y;
    win->x = x;  win->y = y;  win->w = w;  win->h = h;
    win->pending_w = win->pending_h = 0;
    win->want_close = 0;
    win->close_deadline_ms = 0;
    win->close_force_now = 0;
    win->widgets = NULL;  win->focusw = NULL;
    win->on_layout = NULL; win->on_close = NULL; win->app_ctx = NULL;
    win->ui_state = NULL;
    win->cells = NULL; win->vc = NULL;
    win->sb = NULL; win->sb_cap = win->sb_count = win->sb_head = 0;
    win->scrolled = win->view_off = 0;
    win->minimized = 0; win->on_tick = NULL;
    win->maximized = 0;
    win->sav_x = win->sav_y = win->sav_w = win->sav_h = 0;
    win->ccol = win->crow = win->cols = win->rows = 0;
    win->surf.px = NULL; win->surf.owns_px = 0;
    /* M22.7 — per-task app fields. */
    win->host_task = NULL;
    win->client_pid = 0;                /* §M46 — clear stale client on reuse */
    win->on_dispose = NULL;             /* §M54 — never inherit a dead owner   */
    win->dispose_ctx = NULL;
    win->input_hook = NULL;             /* dosgui/wayland re-arm per window     */
    win->input_ctx  = NULL;
    win->aq_h = win->aq_t = 0;
    win->tick_pending = win->layout_pending = win->host_released = 0;
    {
        static uint32_t g_win_serial;                  /* under state_lock */
        win->serial = ++g_win_serial;
    }
    win->close_guard = NULL;
    win->close_reason = 0;
    win->close_confirmed = 0;
    win->no_close = 0;                  /* §M82 — a reused slot is closeable */
    spin_lock_init(&win->lock);
    str_copy(win->title, title, (int)sizeof(win->title));
    win->used = 1;
    spin_unlock_irqrestore(&state_lock, fl);
    return win;
}
void window_show(struct gui_window* win) {
    uint32_t fl = spin_lock_irqsave(&state_lock);
    zorder[zcount++] = win;
    focused_win = win;
    spin_unlock_irqrestore(&state_lock, fl);
    if (win->kind == WIN_TERM) vc_focus(win->vc);
    gui_damage_all();
}
struct gui_window* gui_window_create(const char* title, int x, int y, int w, int h) {
    /* S.1: terminal windows spawn the ACTIVE shell provider.
     * M22.7 — SESSION mode: parent the shell to the desktop (once it exists;
     * the initial two shells are created before it and stay under whoever
     * ran `gui`).  A kill_tree(desktop) then takes session shells with it. */
    return term_window_create(title, x, y, w, h, "shell",
                              shell_provider_active()->entry,
                              desktop_pid > 0 ? desktop_pid : -1);
}
/* M22.7 — DETACHED mode: the shell is parented to init, so it OUTLIVES the
 * desktop session (a kill_tree(desktop) does not reach it).  Its window
 * stays composited as long as the compositor runs — a "detached terminal". */
struct vc* gui_window_console(struct gui_window* win) {
    if (!win || !win->used || win->kind != WIN_TERM) return NULL;
    return win->vc;
}
struct gui_window* gui_window_create_detached(const char* title,
                                              int x, int y, int w, int h) {
    return term_window_create(title, x, y, w, h, "shell",
                              shell_provider_active()->entry,
                              task_reaper_pid());
}
struct gui_window* gui_window_create_task(const char* title, int x, int y,
                                          int w, int h,
                                          const char* task_name,
                                          void (*entry)(void)) {
    /* Custom-task terminals (e.g. BASIC) — parent to the desktop session too. */
    return term_window_create(title, x, y, w, h, task_name, entry,
                              desktop_pid > 0 ? desktop_pid : -1);
}
void gui_window_set_key_hook(struct gui_window* win,
                             void (*fn)(struct gui_window*, char)) {
    if (win) win->key_hook = fn;
}
