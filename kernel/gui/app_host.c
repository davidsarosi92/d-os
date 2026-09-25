/* =============================================================================
 * app_host.c — the per-window application host task (§M70; M22.7).
 *
 * Extracted from gui.c.  EVERY WIN_APP WINDOW RUNS ONE OF THESE, on its own
 * task: it builds the widgets, drains that window's event queue `aq`, runs
 * on_tick / on_layout, and renders into the window's own surface.  The
 * compositor only composites that surface and routes input into the queue —
 * which is what stops a slow application handler from freezing the desktop.
 *
 * THE CONTRACT THAT MAKES THE SPLIT SAFE, and it is worth stating where the
 * loop lives rather than only in gui.h: **A TICK DAMAGES WHAT IT CHANGED.**
 * The loop used to fold every drained event into one `worked` flag and then
 * call `app_redraw` — an unconditional full-window repaint two or three times
 * a second, on top of every carefully damaged row.  An optimisation one layer
 * below an unconditional repaint cannot be measured, only assumed.  Worse,
 * AE_HOVER arrives at motion-packet rate, so moving the pointer across a
 * window repainted the whole window PER PACKET: measured at 33 frames/s over
 * 214-235 kpx, 20.5-22.2 ms mean.  Splitting one flag into two — `worked` (do
 * not halt) and `repaint` (pixels changed that nothing damaged) — took the
 * same gesture to 2.7-3.6 ms over 11-18 kpx.
 *
 * A LAYOUT still repaints everything: it MOVES widgets, so the vacated pixels
 * are stale where nothing will paint.  So does a click or a key — their
 * callbacks may write into a label nothing damages, and both are user-paced.
 *
 * §M22.7's ownership rule applies across this file boundary exactly as it did
 * inside gui.c: a window's widgets belong to the task that hosts it.  A
 * cross-task write into them appears to do nothing, because damaging a window
 * is the host's job — the most expensive kind of bug, because it looks like a
 * missing feature and the fix gets aimed at the wrong layer.
 * =========================================================================== */

#include "gui_priv.h"
#include "gui.h"
#include "gui_internal.h"
#include "widget.h"
#include "ui.h"
#include "gfx.h"
#include "console_plate.h"
#include "task.h"
#include "timer.h"
#include "keymap.h"
#include "printf.h"
#include "kmalloc.h"
#include "config.h"
#include <stdint.h>
#include <stddef.h>

/* §M69 — COALESCE POSITIONS, AND NEVER DROP A BUTTON.
 *
 * This ring is 32 deep and used to drop silently when full, with a comment
 * calling that "input flood" as if the flood were the caller's fault.  It is
 * not: a trackpad delivers motion in bursts of dozens of packets, §M69 turned
 * every one of them into an AE_HOVER, and a press arriving into a full ring
 * was simply discarded.
 *
 * That is the whole of a report that looked like four unrelated faults —
 * *"the arrow works sometimes"*, *"I cannot drag the thumb"*, *"the slider
 * cannot be dragged"*, *"sometimes a button click does nothing, as if it were
 * a refresh problem."*  All four are one press or one release that never
 * arrived.  *A queue that drops silently does not degrade under load; it
 * becomes unpredictable, which is much harder to recognise.*
 *
 * Two rules.  A position REPLACES a queued position rather than queueing
 * behind it — a burst of motion collapses to one event and the ring stops
 * filling.  And if the ring is full anyway, the OLDEST coalescible event is
 * sacrificed rather than the newcomer, because a click is the one thing here
 * that carries intent.  Only when neither is possible is anything dropped, and
 * then it is counted and can be printed. */
volatile unsigned aq_dropped, aq_coalesced;

void app_redraw(struct gui_window* win) {
    /* `gui.input_debug` names the FULL-WINDOW repaints, which is what makes
     * the rule above falsifiable: a click that damages precisely prints
     * nothing here, and one that has fallen back to repainting everything says
     * so with its cost beside it.  Without the line, "the panel is slow" and
     * "the panel repaints itself four times per click" look identical. */
    uint64_t t0 = gui_input_debug() ? timer_now_ns() : 0;
    spin_lock(&win->lock);
    gfx_fill(&win->surf, 0, 0, win->surf.w, win->surf.h, COL_WIN_BG);
    widget_draw_all(win->widgets, &win->surf);
    spin_unlock(&win->lock);
    gui_damage_win(win);
    if (t0) kprintf("gui: full repaint of '%s' %dx%d - %u us\n", win->title,
                    win->surf.w, win->surf.h,
                    (unsigned)((timer_now_ns() - t0) / 1000));
}
/* Compositor/IRQ → host handoff (SPSC; the host is the sole consumer). */
/* Events whose value is only their LATEST value: a pointer position.  Two
 * queued hovers describe one pointer, and the older one describes where it is
 * not any more. */
static int aq_coalescible(const struct app_event* e) {
    if (e->type == AE_HOVER) return 1;
    if (e->type == AE_POINTER && e->phase == WPTR_DRAG) return 1;
    return 0;
}
void aq_push(struct gui_window* w, struct app_event e) {
    if (!w) return;

    if (aq_coalescible(&e) && w->aq_h != w->aq_t) {
        uint32_t last = (w->aq_h + AQ_SZ - 1) % AQ_SZ;
        if (w->aq[last].type == e.type &&
            (e.type != AE_POINTER || w->aq[last].phase == e.phase)) {
            w->aq[last] = e;                    /* the newer position wins */
            aq_coalesced++;
            return;
        }
    }

    uint32_t n = (w->aq_h + 1) % AQ_SZ;
    if (n == w->aq_t) {
        /* Full.  NOT evicted: this ring is lock-free because the producer owns
         * the head and the consumer owns the tail, and an eviction from here
         * would write the tail.  See the note above evq_push — the first
         * version of that one did exactly this and hard-locked the machine in
         * interrupt context.
         *
         * Announced ONCE: a dropped event is a click the user made and the
         * machine did not see, and it used to happen in silence — which is why
         * it presented as four unrelated controls being unreliable. */
        if (aq_dropped++ == 0)
            kprintf("gui: INPUT EVENT DROPPED - a window's queue was full "
                    "(see `gui stats`)\n");
        return;
    }
    w->aq[w->aq_h] = e;
    w->aq_h = n;
}
/* Free a WIN_APP window's widget list + app_ctx.  Runs on the owning host
 * (teardown) — the compositor never touches widgets once a host exists. */
void app_widgets_free(struct gui_window* win) {
    struct widget* w = win->widgets;
    while (w) {
        struct widget* nx = w->next;
        if (w->ops && w->ops->destroy) w->ops->destroy(w);
        kfree(w);
        w = nx;
    }
    win->widgets = NULL;
    win->focusw  = NULL;
    if (win->app_ctx) { kfree(win->app_ctx); win->app_ctx = NULL; }
    if (win->ui_state) { kfree(win->ui_state); win->ui_state = NULL; }
}
/* Drop a window's widgets WITHOUT touching `app_ctx` or `ui_state`.
 *
 * THE BUG THIS EXISTS FOR, reported from use as *"the redraw is not perfect at
 * the window edge, and there seems to be a clickable band along the scrollbar
 * with a fragment of an icon in it"*:
 *
 * A resize sets `layout_pending`, and the host answers by calling `on_layout`
 * again.  Every app's `on_layout` CREATES widgets, and `gui_window_add_widget`
 * only ever APPENDS — so each resize left a whole second set of widgets behind
 * the new one.  The old set was still drawn (at its old geometry, which is
 * where the leftover pixels and the icon fragment came from) and still
 * hit-tested by `widget_at`, which is the band that answered clicks.  It leaked
 * the old widgets as well.
 *
 * `on_layout` therefore means *build this window's widgets*, and it is now
 * always called with an EMPTY list — which is what it already assumed at
 * creation time and what makes the two calls the same call. */
void app_widgets_reset(struct gui_window* win) {
    struct widget* w = win->widgets;
    while (w) {
        struct widget* nx = w->next;
        if (w->ops && w->ops->destroy) w->ops->destroy(w);
        kfree(w);
        w = nx;
    }
    win->widgets = NULL;
    win->focusw  = NULL;
}
/* Resolve a recorded pointer position to a widget and move the hover.
 *
 * Redraws ONLY the two widgets whose state changed, never the window: a hover
 * that repainted everything would make moving the mouse across a dialog cost a
 * full window blit per packet, which is the cost the old "motion only on
 * click" rule was avoiding.  Nothing is redrawn at all when the hover has not
 * moved, which is the common case while the pointer sits still. */
static void app_hover_to(struct gui_window* win, int lx, int ly) {
    struct widget* hit = widget_at(win->widgets, lx, ly);
    struct widget* prev = NULL;
    for (struct widget* w = win->widgets; w; w = w->next)
        if (w->hovered) { prev = w; break; }
    if (hit == prev) return;

    /* §M75.2 — ASK THE WIDGET FIRST.  A widget that renders no hover state
     * needs no repaint to stop being hovered, and repainting it anyway is what
     * made a maximized Task Manager lag: its item view is ~1.9 Mpx and draws
     * nothing for hover, so crossing it repainted the screen to set a flag.
     * See widget.h; a NULL op keeps the old whole-widget behaviour, which is
     * what a button wants. */
    if (prev) {
        prev->hovered = 0;
        if (!(prev->ops && prev->ops->hover && prev->ops->hover(prev, 0)))
            gui_window_request_redraw_rect(win, prev->x, prev->y, prev->w, prev->h);
    }
    if (hit && !hit->disabled) {
        hit->hovered = 1;
        if (!(hit->ops && hit->ops->hover && hit->ops->hover(hit, 1)))
            gui_window_request_redraw_rect(win, hit->x, hit->y, hit->w, hit->h);
    }
}
/* Returns non-zero when the event may have changed pixels that NOTHING
 * damaged — i.e. when the host still owes the window a full repaint.
 *
 * §M69 — THE RULE THE TICK ALREADY LIVES UNDER, EXTENDED TO THE POINTER, and
 * it is the whole of a report: *"the wheel scrolls the panel fine, but
 * clicking the scrollbar or its arrows freezes it for seconds and then works
 * or does not."*
 *
 * The host used to repaint the WHOLE window for every event that was not a
 * hover.  A press on a scrollbar arrives as three of them (motion, button,
 * pointer phase) and the release as more, so ONE click on an arrow cost FOUR
 * full-window repaints — measured on the Appearance panel at 431 kpx and
 * 50-70 ms of compositing each, i.e. about a quarter of a second of frames in
 * which the compositor is also the thing that draws the cursor.  A wheel notch
 * cost one such repaint, which is exactly why the wheel felt fine and the bar
 * did not: *the difference the user reported is a factor of four in frames,
 * not two different code paths being broken.*
 *
 * So the answer is per EVENT rather than per event TYPE: an event whose
 * handler damaged precisely what it changed (the toolkit's scrollbar, a
 * container scroll, a click that landed on no widget at all) owes nothing.  A
 * click that reached a widget's own handler still repaints, because that
 * handler may write into a label nothing damages — the same asymmetry the tick
 * comment below draws, for the same reason. */
int app_dispatch_event(struct gui_window* win, const struct app_event* e) {
    if (e->type == AE_HOVER) { app_hover_to(win, e->x, e->y); return 0; }
    /* §M26 — a Wayland-backed window forwards input to its client instead of
     * to widgets.
     *
     * §M65 — UNLESS IT HAS TOOLKIT WIDGETS.  A client that called ui_build
     * asked the kernel to run its interface; forwarding the raw pointer stream
     * as well would mean the click reaches the client and the checkbox under
     * it never moves — which is exactly what happened the first time a ring-3
     * program built widgets: they drew, and nothing was clickable.  A window
     * with widgets is driven by the toolkit; one without keeps the raw stream,
     * which is every existing client. */
    if (win->input_hook && !win->widgets) {
        struct gui_input gi = {0};
        if (e->type == AE_MOUSE)        { gi.type = GUI_INPUT_MOTION; gi.x = e->x; gi.y = e->y; }
        else if (e->type == AE_BUTTON)  { gi.type = GUI_INPUT_BUTTON; gi.x = e->x; gi.y = e->y;
                                          gi.keycode = e->btn; gi.pressed = e->down; }
        else if (e->type == AE_KEYCODE) { gi.type = GUI_INPUT_KEY; gi.keycode = e->kc; gi.pressed = 1; }
        /* AE_KEY carries the keymap's OUTPUT.  It used to be dropped here, on
         * the grounds that a client gets the scancode — but a client with no
         * keymap of its own cannot turn a scancode into a letter, so every
         * typed character arrived as noise. */
        else if (e->type == AE_KEY)     { gi.type = GUI_INPUT_KEY; gi.ch = (unsigned char)e->c;
                                          gi.pressed = 1; }
        else return 0;
        win->input_hook(win, &gi, win->input_ctx);
        /* A client draws its own pixels and presents them itself, so the host
         * has nothing to repaint on its behalf. */
        return 0;
    }
    if (e->type == AE_POPUP) {
        /* §M65 — the popup's answer, on the app host: choosing a menu item
         * runs app code (open a dialog, delete a file), which is why the IRQ
         * only queued it. */
        ui_dispatch_popup(win, e->x, e->y);
        return 1;
    }
    if (e->type == AE_SCROLL) {
        /* §M61 follow-up — the wheel goes to the widget UNDER THE POINTER, not
         * to the focused one: that is what every toolkit does and what the
         * hand expects, and it means a list can be scrolled without clicking
         * into it first. */
        struct widget* w = widget_at(win->widgets, e->x, e->y);
        int dz = (int)(int8_t)e->phase;
        /* §M69 — WHICH WAY IS DOWN?  Asked for from use, and it is a real
         * disagreement rather than a preference nobody holds: a Windows wheel
         * moves the VIEW (push away → the page goes down), a Mac trackpad
         * moves the CONTENT (push away → the page goes up).  Somebody who uses
         * both has one of them wrong all day, and no default can be right for
         * both — which is precisely what a setting is for.
         *
         * Applied HERE, at the one point every wheel notch passes through, so
         * a widget or container that scrolls can never disagree with another
         * about the direction: four scroll handlers each honouring a flag
         * would be four chances for one to forget. */
        if (config_get_long("gui.scroll_invert", 0)) dz = -dz;
        /* `gui.input_debug` — the DISPATCH half of the wheel probe; the other
         * half is in ps2_mouse.c, and the pair is what separates "the device
         * never produced a notch" from "it did and this path dropped it". */
        int dbg = (int)gui_input_debug();
        /* §M69 — THE WIDGET GETS IT FIRST, BUT ONLY KEEPS IT IF IT MOVED.
         * This used to `return` whenever the widget merely HAD a scroll op, so
         * a list scrolled to its end swallowed every further notch and the
         * page it sits in stayed put — which is exactly the report: the wheel
         * worked over the scrollbar (no widget there, so the container got it)
         * and did nothing over the content.  *"Has a handler" and "did
         * something" are different facts, and routing on the first one makes
         * every nested scroll area a dead end.* */
        if (w && w->ops && w->ops->scroll) {
            int took_w = w->ops->scroll(w, dz);
            if (dbg) kprintf("gui: wheel dz=%d -> widget at %d,%d %s\n",
                             dz, e->x, e->y, took_w ? "took it" : "passed it on");
            if (took_w) return 1;
        }
        /* §M65 — nothing under the pointer wanted it?  Ask the toolkit: a
         * SCROLLING CONTAINER is a node, not a widget, so it cannot have a
         * widget's scroll op of its own. */
        /* The container damages its own viewport (ui_scroll_by), so there is
         * nothing left for the host to repaint — this used to ask for the whole
         * window on top of it, which on a settings panel is 431 kpx for a
         * 273 kpx viewport that had just been painted. */
        int took = ui_scroll_at(win, e->x, e->y, dz);
        if (dbg) kprintf("gui: wheel dz=%d at %d,%d -> ui_scroll_at %s\n",
                         dz, e->x, e->y, took ? "TOOK it" : "declined");
        return 0;
    }
    if (e->type == AE_POINTER) {
        /* §M58 — the phase stream.  The GRABBED widget is resolved HERE, on
         * the host task that owns the widget list, and never carried through
         * the queue: a widget pointer travelling through an IRQ-filled ring
         * would be a lifetime bug waiting for the first window teardown
         * mid-drag (§M54's defect class).  The press picks the widget, the
         * drag and release go to whatever the press picked. */
        /* PRESSED IS TRACKED FOR EVERY WIDGET, not only for those that want
         * the pointer stream.  A button handles clicks through `mouse` and has
         * no `pointer` op, so the grab below drops it — but it still has to
         * look held while the button is down, which is the whole point of the
         * state.  Tracked here rather than in each widget for the same reason
         * the hover is: nine widgets would be nine chances to forget. */
        /* §M81 — THE TOOLKIT'S PRE-ROUTE STOOD HERE AND IS GONE.  §M69 had to
         * ask `ui_pointer_at()` BEFORE the widget lookup, because a scrolling
         * container's scrollbar had no widget under it and the bar a panel drew
         * was therefore decoration.  The container is a widget now, its rect IS
         * that strip, and the lookup below finds it — with the grab, the drag
         * and the release handled by the same code that serves a slider. */
        if (e->phase == WPTR_PRESS) {
            /* §M69 — CLEAR ANY STALE `pressed` FIRST.  It used to be cleared
             * only on RELEASE, so a release that went missing left a widget
             * drawn held forever — *"it froze in a lighter colour, as if it
             * were active"* — and the state is what a user reads as "this
             * control is busy".  A press starts a new gesture, so anything
             * still held belongs to one that ended without saying so. */
            for (struct widget* p = win->widgets; p; p = p->next)
                if (p->pressed) {
                    p->pressed = 0;
                    gui_window_request_redraw_rect(win, p->x, p->y, p->w, p->h);
                }
            /* §M32 — A DISABLED WIDGET RECEIVES NO INPUT AT ALL.
             *
             * `widget.disabled` was honoured in exactly one place here — the
             * pressed HIGHLIGHT — while the grab below, the ordinary click
             * (AE_MOUSE) and both keyboard paths dispatched to it regardless.
             * So a greyed control looked dead and ACTED, which is worse than
             * either: reported from use as *"the button is shown disabled and I
             * can still click it"*.
             *
             * Four dispatch points, one rule, checked at each — the same shape
             * as §4.79's title buttons, where the painter and the hit test had
             * computed the same box differently. */
            struct widget* hit = widget_at(win->widgets, e->x, e->y);
            if (hit && hit->disabled) hit = NULL;
            if (hit) {
                hit->pressed = 1;
                gui_window_request_redraw_rect(win, hit->x, hit->y,
                                               hit->w, hit->h);
            }
            win->grabw = hit;
            if (win->grabw && win->grabw->ops && !win->grabw->ops->pointer)
                win->grabw = NULL;      /* widget does not want the stream */
        } else if (e->phase == WPTR_RELEASE) {
            for (struct widget* w = win->widgets; w; w = w->next)
                if (w->pressed) {
                    w->pressed = 0;
                    gui_window_request_redraw_rect(win, w->x, w->y, w->w, w->h);
                }
        }
        struct widget* w = win->grabw;
        int ran = 0;
        if (w && w->ops && w->ops->pointer) {
            /* §M81 — ASK WHAT THE HANDLER LEFT BEHIND (widget.h's WH_*).  This
             * used to set `ran = 1` merely because the op existed, which was
             * right for the four widgets that had one and would be wrong for a
             * container: a press on the empty background inside a scrolling box
             * would cost a whole-window repaint for an event nothing acted on,
             * and a bar drag would cost one per motion packet — the exact 431
             * kpx §M69 spent a milestone removing. */
            ran = w->ops->pointer(w, e->x - w->x, e->y - w->y, e->phase)
                      == WH_REPAINT;
        }
        if (e->phase == WPTR_RELEASE) win->grabw = NULL;
        /* The pressed/hover highlights above damage their own rects, so a
         * press or release that reached no pointer handler owes nothing. */
        return ran;
    }
    if (e->type == AE_MOUSE) {
        struct widget* w = widget_at(win->widgets, e->x, e->y);
        if (w && w->disabled) return 0;                  /* see AE_POINTER */
        if (!w || !w->ops || !w->ops->mouse) return 0;   /* nothing ran */
        w->ops->mouse(w, e->x - w->x, e->y - w->y, e->dbl);
    } else if (e->type == AE_KEY) {
        /* §M61 — a window-level key hook, consulted BEFORE the focused widget.
         * The confirm-or-revert dialog needs Enter/Esc to work whether or not
         * anything is focused: at a mode the display cannot show, the keyboard
         * is the only input the user can aim. */
        if (win->key_hook) { win->key_hook(win, e->c); return 1; }
        struct widget* w = win->focusw;
        if (w && w->disabled) return 1;                  /* see AE_POINTER */
        if (w && w->ops && w->ops->key) w->ops->key(w, e->c);
    } else if (e->type == AE_KEYCODE) {
        /* §M65 — TAB CYCLES FOCUS, at the WINDOW level, before the focused
         * widget sees it.  It has to be here rather than in a widget: no
         * control can know what comes after it, and a toolkit where the only
         * way to reach the third field is the mouse is a toolkit half the
         * people cannot use.  Shift+Tab goes backwards, and the cycle wraps —
         * a focus ring with an end is a trap at both ends. */
        if (e->kc == KC_TAB) {
            int back = (e->mods & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
            gui_window_focus_cycle(win, back);
            return 1;
        }
        struct widget* w = win->focusw;
        if (w && w->disabled) return 1;                  /* see AE_POINTER */
        if (w && w->ops && w->ops->keycode) w->ops->keycode(w, e->kc, e->mods);
    } else {
        /* AE_BUTTON REACHES NO HANDLER ON A WIDGET WINDOW — and demanding a
         * repaint for it was the last of the four full-window frames a click
         * on the scrollbar cost.  The compositor pushes a button event beside
         * every press because a CLIENT window needs the up and down edges;
         * this branch of the dispatcher consumes clicks through AE_MOUSE and
         * the phase stream, so the button event is by construction a no-op.
         * *An event nothing handled cannot have changed a pixel.* */
        return 0;
    }
    /* A key is user-paced and its handler may write anywhere — the repaint is
     * cheap at that rate and the alternative is a stale label nobody damaged. */
    return 1;
}
/* The app-host task entry.  start_arg is the app's launch (open) function;
 * it runs HERE (creating the window(s) + widgets on this task), then this
 * loop services every window the app owns until they all close. */
/* §M81 — WHICH TASKS RUN AN APP-HOST LOOP.
 *
 * A window binds to `task_current()` at creation, and one created on a task
 * with no host loop never lays out and never ticks — it simply sits there, and
 * §M61 recorded that as a convention to remember (`gui_queue_open`, never a
 * direct create from a shell or the compositor).  *A convention is not a type
 * distinction, so a window built the wrong way fails silently*, which is
 * exactly how it cost a round of diagnosis during §M32.
 *
 * A bounded table rather than a flag on `struct task`: this is the GUI's
 * business, one entry per host, and the scheduler has no reason to carry it.
 * A dead task's slot is simply overwritten — the table answers "is this task a
 * host", which a stale entry can only get wrong for a task that no longer
 * exists and therefore cannot be asking. */
#define APP_HOST_MAX (GUI_MAX_WINDOWS * 2)
static struct task* g_hosts[APP_HOST_MAX];
static int g_hosts_next;

void app_host_note_task(struct task* t) {
    if (!t) return;
    for (int i = 0; i < APP_HOST_MAX; i++) if (g_hosts[i] == t) return;
    g_hosts[g_hosts_next] = t;
    g_hosts_next = (g_hosts_next + 1) % APP_HOST_MAX;
}

int app_host_is_host_task(struct task* t) {
    if (!t) return 0;
    for (int i = 0; i < APP_HOST_MAX; i++) if (g_hosts[i] == t) return 1;
    return 0;
}

void app_host_main(void) {
    void (*open_fn)(void) = (void (*)(void))task_start_arg();
    struct task* self = task_current();
    app_host_note_task(self);                   /* before open_fn creates any */
    kprintf("gui: app-host '%s' up (pid %d)\n",
            self ? self->name : "?", self ? self->pid : -1);
    if (open_fn) open_fn();                     /* creates windows on this task */
    /* A host started by gui_queue_open() has no app name to go by; take its
     * first window's title so the Task Manager says what is running. */
    if (self && self->name[0] == 'a' && self->name[4] == 'o' &&
        self->name[5] == 'p' && self->name[6] == 'e' && self->name[7] == 'n' &&
        self->name[8] == 0) {
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            struct gui_window* w = &windows[i];
            if (!w->used || w->host_task != self) continue;
            int p = 4;
            for (int j = 0; w->title[j] && p < TASK_NAME_MAX; j++) self->name[p++] = w->title[j];
            self->name[p] = 0;
            break;
        }
    }

    for (;;) {
        int live = 0, busy = 0;
        for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
            struct gui_window* win = &windows[i];
            if (!win->used || win->kind != WIN_APP || win->host_task != self)
                continue;
            if (win->host_released) continue;   /* handed to the compositor */

            /* ORDERLY CLOSE — the app's guard may keep the window, once per
             * request.  It runs HERE, on the host, because it may touch the
             * app's widgets and open a dialog; refusing clears the request
             * and leaves the window live, and the app finishes the close
             * itself with gui_window_close_now(). */
            if (win->want_close && win->close_guard && !win->close_confirmed &&
                !win->close_guard(win, win->close_reason)) {
                kprintf("gui: '%s' asked to stay open (%s close)\n", win->title,
                        win->close_reason == GUI_CLOSE_SESSION ? "session" : "user");
                win->want_close = 0;
                win->close_reason = GUI_CLOSE_USER;
            }
            if (win->want_close) {              /* graceful, on the host */
                win_run_on_close(win);   /* §M81: clears the owner's slot too */
                app_widgets_free(win);
                win->host_released = 1;         /* compositor disposes the struct */
                need_frame = 1;
                busy = 1;
                continue;                       /* not live anymore */
            }
            live++;

            /* TWO DIFFERENT QUESTIONS, AND THEY HAD ONE ANSWER BETWEEN THEM.
             * `worked` = something ran, so do not halt this pass.  `repaint` =
             * something ran that may have changed pixels NOTHING damaged, so
             * the whole window has to be painted again.  Every full-window
             * repaint in this loop used to hang off the first of those, which
             * is how a mouse moving over a window came to cost a full-window
             * blit per motion packet. */
            int worked = 0, repaint = 0;
            while (win->aq_t != win->aq_h) {
                struct app_event e = win->aq[win->aq_t];
                win->aq_t = (win->aq_t + 1) % AQ_SZ;
                int needs_repaint = app_dispatch_event(win, &e);
                worked = 1;
                /* A HOVER IS THE ONE EVENT THAT DAMAGES ITSELF EXACTLY.
                 * app_hover_to redraws the two widgets whose state changed and
                 * returns having done NOTHING when the pointer has not left the
                 * widget it was already on — which is most packets.  Measured
                 * on a 656x609 Task Manager with the pointer moving over it:
                 * 33 frames a second, EVERY ONE of them the whole 447 kpx
                 * window, 21 ms mean and 48 ms worst, against 24-120 kpx with
                 * the pointer still.  *That* is what "the mouse lags" is — the
                 * compositor draws the cursor, so every packet arriving inside
                 * one of those frames waits for it.
                 *
                 * THE HOVER USED TO BE THE ONLY EXEMPTION, and that was the
                 * whole of "clicking the scrollbar freezes the panel": the
                 * exemption is a property of the HANDLER, not of the event
                 * type, and three of the events a single click produces reach
                 * handlers that damage precisely (or no handler at all).  Each
                 * dispatch answers for itself now — see app_dispatch_event. */
                if (needs_repaint) repaint = 1;
            }
            if (win->layout_pending) {
                win->layout_pending = 0;
                /* §M69 — THE APP GETS FIRST REFUSAL, and the toolkit is the
                 * FALLBACK.  This was the other way round, and the order was
                 * wrong for every window that is PARTLY a toolkit interface:
                 * the file manager builds its menu bar with ui_build and hand-
                 * places everything else, so `ui_state` was set and its
                 * `on_layout` was never called again after the window opened.
                 * Its path bar, button row, list and status line have not been
                 * repositioned on a resize since §M65 gave it that menu.
                 *
                 * FOUND, NOT LOOKED FOR: §M69's dialog needed a way to hand a
                 * deferred answer back to the app-host that owns a window, and
                 * `gui_window_request_layout` is the one route there — the
                 * answer arrived, the tree really was deleted, and the file
                 * manager showed the deleted directory anyway.  *A window that
                 * never re-lays out looks exactly like an app that ignored the
                 * event*, which is why this survived a milestone.
                 *
                 * The inversion is safe because a ui_build app's layout hook
                 * already calls ui_layout itself — ui.h's "build once, lay out
                 * many" makes that the required shape, and all four such apps
                 * here follow it.  A window with a toolkit interface and NO
                 * hook of its own (the dosgui bridge) still gets ui_layout. */
                if (win->on_layout) {
                    /* NOT cleared automatically here, and that restraint is the
                     * whole safety of this fix: `on_layout` means two different
                     * things across this tree.  Some apps CREATE their widgets
                     * in it; others (the editor) only REPOSITION widgets built
                     * at open time.  Freeing the list before calling would turn
                     * the second kind into a use-after-free — it would go on to
                     * assign coordinates through pointers we had just released.
                     * An app that rebuilds says so, by calling
                     * gui_window_clear_widgets() itself. */
                    win->on_layout(win);
                } else if (win->ui_state) {
                    ui_layout(win);
                }
                worked = 1;
                repaint = 1;
            }
            /* A TICK DAMAGES WHAT IT CHANGED; THE HOST DOES NOT REDRAW FOR IT.
             *
             * This used to fold the tick into `worked` and then repaint the
             * WHOLE window — and it is what made every per-rect refresh in this
             * tree pointless.  Measured on a maximized Task Manager: the row
             * diff correctly reported 0-4 dirty rows out of 19 and damaged
             * ~640x32 for them, after which this line blitted all 447 kpx of
             * the window on top, 2-3 times a second, forever.  §M69 spent two
             * attempts making the diff cheaper against a frame cost that was
             * never the diff's — *an optimisation one layer below an
             * unconditional repaint cannot be measured, only assumed.*
             *
             * The contract is now explicit in gui.h: on_tick damages what it
             * changed.  All four tick users already did (the panels call
             * gui_window_request_redraw themselves, the Task Manager asks for
             * its rows), so nothing here is a behaviour change for them — what
             * changes is that a tick which finds NOTHING to do now costs
             * nothing, which is most ticks.
             *
             * A LAYOUT still repaints everything, and that asymmetry is the
             * point: a re-layout moves widgets, so the pixels they vacated are
             * stale in places no widget will ever paint over. */
            if (win->tick_pending) {
                win->tick_pending = 0;
                if (win->on_tick) win->on_tick(win);
                worked = 1;
            }
            if (repaint) app_redraw(win);
            if (worked) busy = 1;
        }
        if (live == 0) break;             /* all my windows closed → exit */
        /* §M75.2 — the ACCOUNTED halt: a poll loop is SCHEDULED while it
         * waits, so `hal_cpu_idle()` here made an idle app-host report ~25 %
         * of a 4-CPU box.  See task.h. */
        if (!busy) task_halt_idle();      /* M22.7 — halt only when idle */
        task_yield();
    }
    /* Host exits; init reaps it (not reap_owned).  Any windows it released
     * are disposed by the compositor's apply_pending. */
}
