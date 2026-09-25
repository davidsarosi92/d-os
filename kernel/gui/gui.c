/* =============================================================================
 * gui.c — compositor + window manager CORE (M22, M22.1, M22.2).
 *
 * After the M22.2 modularity cut this file owns ONLY:
 *   - surfaces, backbuffer, wallpaper, cursor, composition
 *   - windows (terminal + app kinds), z-order, focus, drag/resize
 *   - input routing (mouse IRQ, keyboard hook) + IRQ→task queues
 *   - the app/desktop-shell REGISTRIES (walk helpers + launch queue)
 *
 * Everything that looks like a desktop — taskbar, launcher menu,
 * clock — lives behind `struct desktop_shell` (desktop.h) and is
 * picked at gui_start by the `gui.shell` config key.  Apps register
 * with GUI_APP() (gui_app.h); this file references no app by symbol.
 *
 * Threading model (three actors, three lock scopes):
 *
 *   shell task(s)   — gterm_emit renders shell output into the window's
 *                     content surface.       Holds:  win->lock.
 *   mouse/kbd IRQ   — updates cursor, focus, z-order, drag state,
 *                     pending resize/close; forwards chrome events to
 *                     the desktop shell; ENQUEUES app events / keys /
 *                     launches.              Holds:  state_lock.
 *   compositor task — drains the queues (widget callbacks + app
 *                     launches run HERE), applies pending resizes +
 *                     closes, composes, calls shell->draw, blits.
 *
 * Damage model: `need_frame` global flag; the shell's second_tick
 * (clock) may also raise it.  Terminal windows keep a char backing
 * store so resize re-renders content (M22.1).
 * ============================================================================= */

#include "gui.h"
#include "gui_priv.h"
#include "users.h"
#include "cred.h"
#include "console_plate.h"
#include "gui_app.h"
#include "desktop.h"
#include "gui_internal.h"
#include "gfx.h"
#include "fb_present.h"                 /* fb_present_flush — virtio-gpu scanout push (M21) */
#include "widget.h"
#include "ui.h"
#include "vc.h"
#include "task.h"
#include "lock.h"
#include "mouse.h"
#include "timer.h"
#include "config.h"
#include "vfs.h"            /* §M82 sessiontest reads the stores back */
#include "shellcmd.h"   /* §M76 — gui.autorun dispatches one command */
#include "locale.h"
#include "settings.h"   /* CONFIG_KEY — gui.occlude is a declared setting */
#include "wallpaper.h"          /* §M60: the desktop background source */
#include "clipboard.h"          /* §M58/§M59: selection → primary */
#include "klog.h"               /* §M61: a refused gui.mode is a warning */
#include "keymap.h"          /* M22.3: Alt-Tab raw keycodes */
#include "version.h"         /* DOS_LABEL — the desktop milestone label */
#include "kmalloc.h"
#include "printf.h"
#include "hal.h"
#include "hal_api.h"
#include "driver.h"
#include <stddef.h>
#include <stdint.h>

/* S.1: terminal windows spawn the ACTIVE shell provider — no direct
 * symbol reference to any particular shell implementation. */
#include "shell_provider.h"

/* -------------------------------------------------------------------------- */
/* Metrics + palette (window chrome only — desktop chrome is the shell's).    */
/* -------------------------------------------------------------------------- */

#define GUI_MAX_WINDOWS 8

/* The Console Plate frame is ONE pixel.  M22 used two, which reads as a bevel
 * and is the single most old-fashioned thing about the old chrome. */
/* DEVICE pixels, via the scale derived from the framebuffer — not the design's
 * absolute numbers.  These are functions now, so a §M61 mode change moves the
 * chrome with the text instead of leaving one behind. */
#define TITLE_H     cp_titlebar_h()
#define PAD         3
#define CLOSE_W     cp_window_btn()
#define CLOSE_H     cp_window_btn()

/* THE THREE TITLE BUTTONS' GEOMETRY, IN ONE PLACE.
 *
 * It was in two, and they disagreed: the painter put the row at
 * `(TITLE_H - btn) / 2` with 4 px gaps, the hit test used `y + 4` with 3 px
 * gaps.  At today's 38 px title bar that is a box drawn two pixels below where
 * it can be pressed and one pixel left of it — so the top edge of every button
 * was dead and the strip above it live.  Exactly §4.79's "a view that draws
 * correctly and hit-tests wrongly is invisible in a screenshot": each half
 * looks right alone, and only pressing the edge shows it.
 *
 * Index 0 is the CLOSE button and they run right to left, because the right
 * edge is the fixed one — numbering from the left would move every button
 * whenever one is added. */


/* §M69 — how many of the three a window shows.  A MODAL WINDOW SHOWS ONLY THE
 * CLOSE BOX, and that is a safety property rather than a stylistic one:
 * minimising a modal would hide the one thing that cannot be got past, leaving
 * a desktop that swallows every click with nothing on screen to explain why.
 * (Maximising one is merely meaningless, and goes with it.)
 *
 * A COUNT rather than three booleans because the buttons are numbered from the
 * RIGHT edge (see above) — so "show fewer" is exactly "stop at a lower index",
 * and the painter, the hit test and the click handler all take the same number
 * and cannot disagree about which boxes exist (§4.79: the top edge of every
 * button was dead for a year because two of those three computed it apart). */


/* (There is deliberately no `title_btn_at(...)` taking TB_COUNT implicitly.
 * Both callers must decide how many buttons the window HAS, and a convenience
 * wrapper that assumes three would be the easy thing to reach for and wrong
 * for exactly the window where it matters — §M52's "a default nobody chose".) */

/* The hovered button, recorded in the mouse IRQ and read by the compositor —
 * §M22.7's split, the same shape as `popup.hover`.  A window pointer rather
 * than an index because two windows must not both look hovered; it is only ever
 * COMPARED against the compose snapshot, never dereferenced, so a window freed
 * between the two simply stops matching. */
struct gui_window* volatile tb_hover_win;
volatile int tb_hover_idx = -1;

/* §M65 popup palette — deliberately the same values the Start menu uses in
 * shell_vista.c: two menus that look different are two menus, and a shared
 * header for four colours would be a header for four colours. */

/* -------------------------------------------------------------------------- */
/* Window object.                                                              */
/* -------------------------------------------------------------------------- */

/* struct gui_window, struct app_event and the AE_* vocabulary moved to
 * gui_priv.h in §M70, so the compositor can be built out of several files.
 * They are still PRIVATE — that header is only for the files that together
 * implement the compositor.  See its comment for the three-header split. */


struct move_hint mv_hint;               /* guarded by state_lock      */

/* How many composites took the copy path and how many fell back.  Both
 * numbers matter: all-fast would mean the fallback is never exercised (and so
 * never tested), and all-slow would mean the optimisation is not running at
 * all while the timing appears to improve for some other reason. */
uint32_t drag_fast, drag_slow;


/* Scene. */
struct gfx_surface fbsurf, backsurf, wallsurf;
int work_h = 0;                  /* screen minus shell chrome     */
int gmax_cols = 0, gmax_rows = 0;
/* §M46 — see gui_start: X on a package window force-kills a wedged client.
 * §M47.1 — but that is the FALLBACK, not the close path.  Killing on the first
 * compositor pass meant the X never gave the client a chance to notice the close
 * event and quit by itself, so an ordinary "close the browser" was reported as a
 * forced kill — a crash record, and (since §M47 stage 2) a Crash Reports window
 * popping up as though something had gone wrong.
 *
 * The escalation is now the USER's, which is the familiar desktop contract:
 *   1st X click → ask the client to close;
 *   2nd X click → it clearly is not going to, so force it, immediately.
 * `close_grace_ms` remains only as an UNATTENDED backstop for the case where
 * nobody is there to click again, so it is deliberately generous — long enough
 * that it never pre-empts a client that is merely slow to shut down. */
static int close_forces_kill = 1;
static unsigned close_grace_ms = 10000;

/* M22.6 — tear-free presentation via a Bochs-VBE double buffer (see
 * fb_terminal.c).  When `flip_ok`, compose() copies the dirty region from
 * backsurf into the currently HIDDEN scanout buffer and pans to it, instead
 * of blitting straight into the live scanout.  QEMU then never reads a
 * half-updated frame — no mid-scanout shear.  flip_ok==0 keeps the legacy
 * single-buffer direct blit (real hardware / non-Bochs display). */

struct gui_window windows[GUI_MAX_WINDOWS];

/* Z-order, bottom → top (state_lock). */
struct gui_window* zorder[GUI_MAX_WINDOWS];
int                zcount = 0;
struct gui_window* focused_win = NULL;

/* WM / pointer state (state_lock; IRQ writer). */
spinlock_t state_lock;
int mx, my;
enum drag_mode      drag = DRAG_NONE;
struct gui_window*  drag_win = NULL;
int grab_dx, grab_dy;
int rubber_w, rubber_h;


int flip_ok = 0;
struct gfx_surface flipbuf[2];   /* alias the two scanout buffers  */
int flip_front = 0;              /* buffer index currently visible */

volatile int need_frame = 0;
int gui_active = 0;

/* M22.7-B — the desktop shell (taskbar/launcher/clock) runs on its OWN
 * "desktop" task and renders into a full-screen `panelsurf` at screen
 * coordinates (so the shell's draw code is unchanged).  The compositor
 * composites only the OPAQUE parts of it — the taskbar strip (always) and
 * the launcher popup rect (when open) — on top of the windows, so the rest
 * of panelsurf never occludes anything.  Input in those regions is routed
 * to the panel task's queue; the shell's click/motion run there (under
 * state_lock, which they assume held) instead of in the mouse IRQ. */
/* panelsurf is addressed in SCREEN coordinates (so the shell's draw code is
 * unchanged) but only the bottom `strip` is actually backed by memory: the
 * taskbar reserve + PANEL_POPUP_MAX for the launcher.  `px` points
 * `panel_strip_top` rows "before" the real allocation so screen-row Y lands
 * on backed row Y-strip_top; the clip keeps draws inside the strip.  Saves
 * ~5 MiB versus a full-screen panel at 1920×1200. */
struct gfx_surface panelsurf;
uint32_t*    panel_buf = NULL;           /* real allocation base      */
int          panel_strip_top = 0;        /* first backed screen row   */
int          panel_ready = 0;
spinlock_t   panel_lock;
/* pid of the desktop task (0 until spawned).  Launched session terminals
 * are parented here so they belong to the desktop session (M22.7). */
int          desktop_pid = 0;
volatile int panel_dirty = 1;            /* shell needs a redraw     */
volatile int panel_gen = 0;              /* bumped on WM changes     */
/* ===========================================================================
 * §M65 — THE WINDOW POPUP: one overlay above every window.
 *
 * The system had exactly one popup before this — the Start menu — and it
 * belongs to the PANEL (gui_panel_set_popup, composited out of panelsurf).  A
 * window menu and a combo box need the same thing and could not have it.
 *
 * ONE slot, not a stack, because one is the truth: a popup is modal by nature
 * (the next click either picks from it or dismisses it), and a second one open
 * at the same time would have no way to say which owns the pointer.
 *
 * It has its OWN small surface rather than drawing into the window beneath it:
 * a menu that is clipped to its window is not a menu, and painting onto the
 * back buffer directly would be erased by the next compose of anything under
 * it.
 * ========================================================================= */


/* ===========================================================================
 * §M69 — MODALITY: one window owns the whole screen until it is dismissed.
 *
 * The popup above is modal WITHIN a window; this is modal over the SESSION,
 * which is what widget_specs.md §14's dialog asks for ("modal backdrop: black
 * 45 % over the FULL screen") and what nothing in this tree could express.
 * Every earlier confirmation here worked around the gap rather than filling
 * it: §M61's confirm-or-revert is an ordinary window that anything can be
 * clicked in front of, and the file manager asks for a destructive delete by
 * making the user press Del TWICE within eight seconds — a keyboard gesture
 * standing in for a question, because there was nothing to ask it with.
 *
 * ONE SLOT, NOT A STACK, for the popup's reason exactly: modality is a claim
 * about who owns the next event, and two claimants have no way to settle it.
 * A second request is REFUSED and says so, rather than being queued — a dialog
 * that appears some seconds after the action that caused it is worse than one
 * that never appears, because by then the user is somewhere else.
 *
 * WHAT MODALITY IS, MECHANICALLY, IS FOUR GATES.  They are listed here because
 * they live in four different functions and a reader who finds only one of
 * them will conclude the feature is half built:
 *
 *   1. `topmost_at` answers "the modal, or nothing" — which covers hover, the
 *      title-button highlight, the middle/right press and the drag start in
 *      one place, because all four ask that same question.
 *   2. the left-press path swallows anything landing outside it, BEFORE the
 *      taskbar's first refusal and before the desktop fallthrough.  Gate 1
 *      alone would send those clicks to the DESKTOP (topmost_at returning
 *      NULL is exactly how a click on the wallpaper is recognised), i.e. a
 *      modal dialog would still let you launch shortcuts behind it.
 *   3. focus cannot leave: Alt-Tab is refused while a modal is up.
 *   4. the backdrop, painted in draw_scene_rect, which is what makes the
 *      other three VISIBLE — input that is silently swallowed with nothing
 *      on screen to explain it reads as a frozen machine.
 *
 * AND ONE ESCAPE HATCH, which is not decoration.  A modal window whose host
 * task wedges would lock the desktop with no way out: the dialog's own Esc
 * handler runs on that host and would never run.  So Esc is trapped HERE, in
 * the compositor, and asks the window to close through the same `want_close`
 * the X button uses — including its second-press force.  (Ctrl+Alt+Del is
 * unaffected either way: §M46 traps it in the keyboard IRQ, above the GUI.)
 * ========================================================================= */



int modal_hit(int px, int py) {
    struct gui_window* m = modal_win;
    if (!m || !m->used) return 1;               /* no modal: everything hits */
    return px >= m->x && px < m->x + m->w &&
           py >= m->y && py < m->y + m->h;
}

/* Published launcher-popup extent (set by the shell via gui_panel_set_popup)
 * — read by the compositor (what to composite) and input routing. */
volatile int pnl_pop_on = 0;
volatile int pnl_pop_x = 0, pnl_pop_y = 0, pnl_pop_w = 0, pnl_pop_h = 0;



/* §M64 tail — does the desktop hold the keyboard?  Published by the shell
 * whenever its icon selection appears or clears (gui_desktop_focus), and read
 * in the keyboard IRQ to decide whether Enter and Escape belong to a shortcut
 * or to the shell behind the desktop.  A flag rather than a query because the
 * reader is an interrupt handler and the writer is the desktop task. */
volatile int desk_focus = 0;

void gui_desktop_focus(int on) { desk_focus = on ? 1 : 0; }

/* M22.4 — set by the task-lifecycle hook (any context) and consumed by
 * the compositor loop: run every window's on_tick NOW instead of at
 * the next 1 Hz beat, so a closed/killed program vanishes from the
 * Task Manager within one frame. */
static volatile int tasks_changed = 0;

static void gui_task_change_hook(void) {
    tasks_changed = 1;
    need_frame = 1;
}








/* Active desktop shell (chosen once at gui_start). */
const struct desktop_shell* shell = NULL;


/* ---- IRQ → compositor queues (SPSC: IRQ produces, compositor consumes) ---- */




/* App-launch queue + power request (shell chrome → compositor task). */
#define LQ_SZ 8
static const struct gui_app_def* volatile launchq[LQ_SZ];
static volatile uint32_t lq_h = 0, lq_t = 0;
/* §M61 — the same queue for an ANONYMOUS opener: a window that is not an app
 * in the launcher (the confirm-or-revert dialog) still has to be built on its
 * own APP-HOST task, because `gui_app_window_create` binds the window to
 * `task_current()` and only an app-host loop runs `on_layout` / `on_tick`.
 * Creating one from the compositor or a shell produced a window that never
 * laid out and never ticked — an empty box with a live countdown behind it. */
static void (* volatile openq[LQ_SZ])(void);
static volatile uint32_t oq_h = 0, oq_t = 0;
static volatile int power_req = 0;      /* 0 none / 1 reboot / 2 shutdown */
static volatile int exit_req  = 0;      /* Start → Exit GUI: end the session   */

/* §M81 — RESTART THE SESSION, POSSIBLY AS SOMEBODY.
 *
 * The GUI sign-in used to set a flag and copy a NAME into a string, which is
 * authentication without authorisation: `cred_become_user` is the only call
 * that gives a task an identity, and nothing in the GUI reached it — so a
 * desktop "signed in as root" ran every one of its tasks as SYSTEM.
 *
 * It cannot be fixed in place.  `cred_become_user` refuses a task that already
 * has children, deliberately, because §M32 captures ownership at SPAWN and
 * makes it immutable so that re-parenting cannot launder it — and by the time
 * the lock screen runs, the session has a compositor and app-hosts under it.
 *
 * So the greeter is not the session, which is why every display manager is
 * shaped this way: tear the session down and build a new one on a task that
 * adopted the account BEFORE it had children.  `login.c`'s `session_entry` is
 * the worked example and this is the same three steps. */
static void gui_session_main(void);
static volatile int restart_req = 0;
static int  pend_uid = -1, pend_gid, pend_session;
static int  pend_ngroups, pend_groups[CRED_MAX_GROUPS];
static char pend_name[USER_NAME_MAX + 1];
/* The new session is already authenticated; raising the lock again would ask
 * for the password that just worked, forever. */
static int  skip_lock_once = 0;
static int  gui_next_session = 1;
/* Set only while the session leader is calling gui_start, so the autologin
 * hand-off in gui_start does not route its own session back to itself. */
static int  in_session_leader = 0;
static void gui_stop_main(void);        /* teardown task; defined by gui_stop  */


/* §M69 — WRITE THE WHOLE SLOT, ALWAYS, AND NEVER FIELD BY FIELD.
 *
 * THE BUG THIS EXISTS FOR IS THE SECOND HALF OF *"I click the scrollbar and it
 * either works or it does not"*, and it is not a race, a queue overflow or a
 * lost interrupt — it is a ring slot being REUSED with one field left over.
 *
 * `evq_push_ptr` and `evq_push_wheel` each set seven of this struct's eight
 * fields and left `hover` alone.  A slot that had last carried a hover still
 * read `hover = 1`, and `dispatch_events` tests that flag FIRST — so a PRESS
 * landing in such a slot was delivered as a pointer MOVE.  It moved the
 * highlight, changed nothing, and reported nothing: the press was not dropped,
 * it arrived wearing the wrong hat.  Intermittent by construction, because
 * whether it happens depends only on what occupied that one slot of thirty-two
 * before it — which is why the same click on the same pixel worked four times
 * and then did not.  Measured: six driven presses on a scrollbar arrow, four
 * dispatched, two silently turned into hovers.
 *
 * Every producer now assigns a complete `struct gev`, so the compiler zeroes
 * what the caller does not name and a field cannot be forgotten.  *A partial
 * write into a reused buffer is not a missing line; it is a value invented by
 * whatever ran before.* */








/* Mouse-wheel listener (IRQ).  Finds the window under the cursor and queues;
 * the widget hit-test happens on the app-host like every other input. */
/* Defined further down with the rest of the terminal-grid code; needed here by
 * the wheel listener, which is the IRQ half of the same feature. */


/* -------------------------------------------------------------------------- */
/* App registry walk helpers (gui_app.h).                                      */
/* -------------------------------------------------------------------------- */






int gui_app_count(void) {
    return (int)(__stop_gui_apps - __start_gui_apps);
}

const struct gui_app_def* gui_app_at(int idx) {
    if (idx < 0 || idx >= gui_app_count()) return NULL;
    return &__start_gui_apps[idx];
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

const struct gui_app_def* gui_app_find(const char* name) {
    if (!name || !*name) return NULL;
    /* Exact (case-insensitive) first, then unique-enough prefix. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < gui_app_count(); i++) {
            const char* a = __start_gui_apps[i].name;
            const char* b = name;
            while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
            if (*b == 0 && (pass == 1 || *a == 0))
                return &__start_gui_apps[i];
        }
    }
    return NULL;
}

/* M22.5 — extension → app association (see gui_app.h). */
const struct gui_app_def* gui_app_for_path(const char* path) {
    if (!path) return NULL;
    const char* ext = NULL;                     /* after the last '.' */
    for (const char* p = path; *p; p++) {
        if (*p == '.')      ext = p + 1;
        else if (*p == '/') ext = NULL;         /* dot belonged to a dir */
    }
    if (!ext || !*ext) return NULL;

    for (int i = 0; i < gui_app_count(); i++) {
        const char* list = __start_gui_apps[i].extensions;
        if (!list || !__start_gui_apps[i].open_path) continue;
        const char* p = list;
        while (*p) {
            while (*p == ' ') p++;
            const char* a = p;                  /* one list entry */
            const char* b = ext;
            while (*a && *a != ' ' && *b && lower(*a) == lower(*b)) { a++; b++; }
            if ((*a == 0 || *a == ' ') && *b == 0)
                return &__start_gui_apps[i];
            while (*p && *p != ' ') p++;
        }
    }
    return NULL;
}

/* ---- gui_internal.h services ---------------------------------------------- */





void gui_queue_launch(const struct gui_app_def* app) {
    if (!app) return;
    uint32_t n = (lq_h + 1) % LQ_SZ;
    if (n == lq_t) return;
    launchq[lq_h] = app;
    lq_h = n;
    need_frame = 1;
}

/* §M46 — Ctrl+Alt+X: request the compositor close the top-most app window.  Set
 * from the keyboard IRQ (secure-attention key), acted on by the compositor task
 * (never the possibly-frozen app), so the combo works even when an app is wedged.
 * IRQ-safe: a single volatile store, no lock/alloc. */
void gui_request_close_last(void) {
    sak_close_req = 1;
    need_frame = 1;
}

void gui_queue_power(int reboot) {
    power_req = reboot ? 1 : 2;
    need_frame = 1;
}

/* End the GUI session and go back to the text console.  Only a flag is set
 * here: this is called from the chrome (a click, dispatched on the compositor)
 * and possibly from an IRQ, and the teardown KILLS the compositor.  See
 * gui_stop_main for the task that actually does it. */
void gui_queue_exit(void) {
    exit_req = 1;
    need_frame = 1;
}

void gui_request_frame(void) { gui_damage_all(); }

int gui_screen_w(void) { return fbsurf.w; }
int gui_screen_h(void) { return fbsurf.h; }

/* -------------------------------------------------------------------------- */
/* Small utils.                                                                */
/* -------------------------------------------------------------------------- */

void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}


/* -------------------------------------------------------------------------- */
/* Terminal-in-a-window ("gterm").                                             */
/* -------------------------------------------------------------------------- */











/* §M58 — the compositor half of terminal selection.  The mouse IRQ only
 * RECORDS what changed (a cell range, a request to copy); everything that
 * costs time or allocates happens here:
 *
 *   - re-rendering the grid is thousands of glyph blits — not IRQ work;
 *   - `clipboard_set` allocates — not IRQ work either.
 *
 * Same split as every other input path in this file (§M22.7), and the reason
 * the selection is a MODEL range rather than painted pixels: the IRQ can move
 * it for free and the repaint happens once per frame no matter how many mouse
 * packets arrived.  */





/* -------------------------------------------------------------------------- */
/* App-window redraw + resize plumbing.                                        */
/* -------------------------------------------------------------------------- */


/* -------------------------------------------------------------------------- */
/* M22.7 — per-task app host.  Each WIN_APP window runs on its own task; the   */
/* compositor routes input into win->aq and this loop consumes it, so a slow   */
/* app handler never stalls compositing.                                       */
/* -------------------------------------------------------------------------- */












/* Force a re-layout of every app window, as a resize would.  Exists so the
 * count above can be taken BEFORE and AFTER without a mouse: the harness cannot
 * drive a resize grip once a GUI window has focus, so the one path that
 * reproduces this bug would otherwise be untestable here. */
/* A THEME CHANGE INVALIDATES EVERYTHING, AND BOTH HALVES ARE NECESSARY.
 *
 * Reported from use: switching dark → light left the Control Panel unchanged,
 * and switching back made it draw a streak along the mouse path.  One cause.
 * The palette is read live, so after a switch every widget WOULD draw in the
 * new colours — but nothing was marked dirty, so nothing redrew.  The window
 * kept its old pixels, and then any partial repaint that did happen (the
 * cursor's own rectangle, which the compositor always refreshes) painted
 * new-theme content into an old-theme window.  That trail IS the streak.
 *
 * §M65 hit the same shape with a hover highlight: `need_frame` with an empty
 * damage list repaints only the cursor's footprint.  So this does not set
 * need_frame and hope — it damages the whole screen AND makes every window
 * redraw its own surface, because re-compositing stale window pixels would
 * show the old theme just as faithfully.
 *
 * Every window kind, not just WIN_APP: a terminal renders its own grid and a
 * client-managed window is told through the ordinary redraw path. */
void gui_theme_changed(void) {
    int n = 0;
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].used) continue;
        /* app_redraw is the ordinary "this window's content is stale" path —
         * the same one a resize and a widget change use, so nothing here needs
         * a mechanism of its own.  A terminal re-renders from its cell model on
         * the next frame; a client-managed window is told through its bridge. */
        if (windows[i].kind == WIN_APP) {
            windows[i].layout_pending = 1;
            app_redraw(&windows[i]);
        }
        n++;
    }
    panel_gen++;              /* the taskbar and the desktop repaint too */
    gui_damage_all();
    /* Quiet when there is no GUI: this watcher also fires at boot, when the
     * persistent store is overlaid and no window exists yet. */
    if (n) kprintf("gui: theme change - %d window(s) invalidated\n", n);
}




CONFIG_KEY(ck_gui_occlude) = {
    .key = "gui.occlude", .group = "Display", .type = CFG_BOOL, .def = "1",
    .help = "skip painting what an opaque window completely covers",
};








/* -------------------------------------------------------------------------- */
/* Widget plumbing (gui.h API used by widget.c + apps).                        */
/* -------------------------------------------------------------------------- */
























/* Topmost non-minimized window — focus fallback. */
struct gui_window* top_visible_locked(void) {
    for (int i = zcount - 1; i >= 0; i--)
        if (!zorder[i]->minimized) return zorder[i];
    return NULL;
}



/* -------------------------------------------------------------------------- */
/* Cursor sprite.                                                              */
/* -------------------------------------------------------------------------- */











/* -------------------------------------------------------------------------- */
/* Composition.                                                                */
/* -------------------------------------------------------------------------- */



/* -------------------------------------------------------------------------- */
/* M22.7-B — desktop shell / panel task.                                       */
/* -------------------------------------------------------------------------- */


/* The active shell publishes its launcher-popup extent here (0 = closed).
 * The compositor composites this rect on top of the windows while open, and
 * the mouse IRQ routes clicks inside it to the panel. */
void gui_panel_set_popup(int on, int x, int y, int w, int h) {
    /* §M32 — A POPUP THAT DOES NOT FIT THE STRIP SAYS SO.
     *
     * `panelsurf` is addressed in SCREEN coordinates but only the bottom strip
     * is backed, and the clip quietly discards anything above it.  A shell that
     * grew its menu past the reserve therefore lost the TOP of it — rows that
     * were drawn, hit-tested and simply never appeared — and the symptom points
     * at whatever was added last rather than at the strip.
     *
     * Once, on the console, because this is a geometry fact and not an event:
     * it is the same every frame the menu is open, and a line per frame would
     * bury the log the moment somebody left the menu up. */
    if (on && y < panel_strip_top) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            kprintf("gui: POPUP TOO TALL - its top row %d is above the panel "
                    "strip at %d, so %d pixel(s) are being CLIPPED AWAY.  "
                    "Raise PANEL_POPUP_MAX (gui_priv.h).\n",
                    y, panel_strip_top, panel_strip_top - y);
        }
    }
    pnl_pop_x = x; pnl_pop_y = y; pnl_pop_w = w; pnl_pop_h = h;
    pnl_pop_on = on ? 1 : 0;
    panel_dirty = 1;
    need_frame = 1;
    /* The actual repaint (including the OLD extent when the menu closes) is
     * issued by desktop_main's panel-repaint block, OUTSIDE state_lock — this
     * setter can be called from vista_click with state_lock held, so it must
     * not touch the damage list itself. */
}

/* M22.7 — the shell asks for a chrome-only repaint (taskbar + open popup),
 * NOT a full-screen recompose.  vista_motion uses this for menu-hover
 * changes so gliding over the open menu doesn't repaint the whole 1920×1200
 * screen per motion event (the old gui_request_frame path — the menu lag). */
void gui_panel_dirty(void) {
    panel_dirty = 1;
    need_frame = 1;
}


/* The desktop-shell task: renders the chrome into panelsurf and services
 * its input off the compositor.  shell->click/motion assume the WM lock is
 * held (their old IRQ contract), so we hold state_lock across them. */
static void dispatch_launches(void);            /* defined below; run by desktop */

/* Desktop-loop counters.  `gui stats` prints them, because "the taskbar is not
 * updating" has exactly three causes — the loop is not running, it is running
 * but never marks itself dirty, or it draws and the damage never reaches the
 * compositor — and from outside the guest they look identical. */
volatile uint32_t desk_iters = 0, desk_draws = 0, desk_events = 0;
volatile uint32_t desk_ticks = 0, desk_tick_dirty = 0;
volatile uint32_t desk_now_ms = 0;


static void desktop_main(void) {
    kprintf("gui: desktop shell up on pid %d (shell '%s')\n",
            task_current() ? task_current()->pid : -1, shell ? shell->name : "none");
    uint64_t last_tick = 0;
    int gen_seen = -1;
    /* Previous launcher-popup extent — so when the menu closes we can repaint
     * the pixels it used to cover (compose only repaints the damage list). */
    int last_pop_on = 0, last_pop_x = 0, last_pop_y = 0, last_pop_w = 0, last_pop_h = 0;
    for (;;) {
        int busy = 0;
        desk_iters++;

        /* M22.7 — launches run HERE now: an app spawned from the taskbar (or
         * the `launch` command's queue) becomes a child of the desktop/
         * session, not of the display server (compositor). */
        dispatch_launches();

        struct pev e;
        while (pevq_pop(&e)) {
            if (e.type == PEV_DESK_CLICK || e.type == PEV_DESK_DBL) {
                /* §M64 — dispatched WITHOUT the WM lock, unlike the chrome
                 * events below.  A desktop click touches only the shell's own
                 * icon state, and activating a shortcut opens files and spawns
                 * an app-host task — work that must not run with state_lock
                 * held (§M49's "a metric is only as honest as the state it
                 * observes" had the same root: doing real work under a lock
                 * taken for something else). */
                if (shell && shell->desktop_click)
                    shell->desktop_click(e.x, e.y, e.type == PEV_DESK_DBL);
            } else if (e.type == PEV_DESK_KEY) {
                if (shell && shell->desktop_key) shell->desktop_key(e.x, e.y);
            } else if (e.type == PEV_DESK_PRESS || e.type == PEV_DESK_DRAG ||
                       e.type == PEV_DESK_RELEASE) {
                /* §M64 tail — same contract as desktop_click above: the
                 * desktop task, no lock.  The release writes a `.lnk`. */
                if (shell && shell->desktop_pointer)
                    shell->desktop_pointer(e.x, e.y,
                        e.type == PEV_DESK_PRESS   ? WPTR_PRESS :
                        e.type == PEV_DESK_DRAG    ? WPTR_DRAG  : WPTR_RELEASE);
            } else {
                uint32_t fl = spin_lock_irqsave(&state_lock);
                if (e.type == PEV_CLICK) { if (shell && shell->click)  shell->click(e.x, e.y); }
                else                     { if (shell && shell->motion) shell->motion(e.x, e.y); }
                spin_unlock_irqrestore(&state_lock, fl);
            }
            /* Clicks change chrome state (menu, focus) → always repaint.
             * Motion is frequent; let the shell request a repaint itself
             * (vista only does so when the hover row changes) so a mouse
             * drag across the open menu doesn't repaint the chrome on every
             * event. */
            /* A desktop click changes only the icon layer, and that layer is
             * damaged precisely by the shell — repainting the whole panel for
             * it would undo §4.61's damage discipline on every click. */
            if (e.type == PEV_CLICK) panel_dirty = 1;
            desk_events++;
            busy = 1;
        }

        uint64_t now = timer_ticks_ms();
        desk_now_ms = (uint32_t)now;
        if (now - last_tick >= 500) {
            last_tick = now;
            desk_ticks++;
            if (shell && shell->second_tick && shell->second_tick()) {
                desk_tick_dirty++;
                panel_dirty = 1;
            }
        }
        if (panel_gen != gen_seen) { gen_seen = panel_gen; panel_dirty = 1; }

        if (panel_dirty) {
            panel_dirty = 0;
            desk_draws++;
            busy = 1;
            spin_lock(&panel_lock);
            if (shell && shell->draw) shell->draw(&panelsurf);
            spin_unlock(&panel_lock);
            gui_damage(0, work_h, fbsurf.w, fbsurf.h - work_h);   /* taskbar */
            /* Repaint the popup's CURRENT extent (if open) AND the extent it
             * had LAST frame (if it just closed or moved).  Without the "last"
             * rect a launcher menu that closes via the app-launch path — which
             * doesn't otherwise damage the screen — leaves its stale pixels on
             * screen ("the menu won't disappear").  This runs OUTSIDE state_lock
             * (gui_damage takes damage_lock), so no lock nesting. */
            if (pnl_pop_on)
                gui_damage(pnl_pop_x, pnl_pop_y, pnl_pop_w, pnl_pop_h);
            if (last_pop_on &&
                (!pnl_pop_on || last_pop_x != pnl_pop_x || last_pop_y != pnl_pop_y ||
                 last_pop_w != pnl_pop_w || last_pop_h != pnl_pop_h))
                gui_damage(last_pop_x, last_pop_y, last_pop_w, last_pop_h);
            last_pop_on = pnl_pop_on;
            last_pop_x = pnl_pop_x; last_pop_y = pnl_pop_y;
            last_pop_w = pnl_pop_w; last_pop_h = pnl_pop_h;
        }
        if (!busy) task_halt_idle();     /* §M75.2 accounted halt; see task.h */
        task_yield();
    }
}




/* -------------------------------------------------------------------------- */
/* Window teardown (compositor task only).                                     */
/* -------------------------------------------------------------------------- */


/* -------------------------------------------------------------------------- */
/* Queue dispatch — runs on the compositor task.                               */
/* -------------------------------------------------------------------------- */


void gui_queue_open(void (*open_fn)(void)) {
    if (!open_fn) return;
    uint32_t n = (oq_h + 1) % LQ_SZ;
    if (n == oq_t) return;
    openq[oq_h] = open_fn;
    oq_h = n;
}

static void dispatch_launches(void) {
    if (sak_close_req) { sak_close_req = 0; sak_close_top_app(); }
    while (oq_t != oq_h) {
        void (*fn)(void) = openq[oq_t];
        oq_t = (oq_t + 1) % LQ_SZ;
        if (!fn) continue;
        struct task* host = task_spawn_arg("app:dialog", app_host_main,
                                           (void*)(uintptr_t)fn);
        if (host) task_set_reap_owned(host, 1);
    }
    while (lq_t != lq_h) {
        const struct gui_app_def* app = launchq[lq_t];
        lq_t = (lq_t + 1) % LQ_SZ;
        if (!app || !app->launch) continue;
        /* M22.7 — each app runs on its OWN task.  Spawn an app-host and hand
         * it the launch fn via start_arg; the host runs it (creating the
         * window(s) + widgets on that task) then services them.  A singleton
         * app whose open fn just raises an existing window creates nothing,
         * so its host exits immediately (init reaps it). */
        char nm[TASK_NAME_MAX + 1] = "app:";
        int p = 4;
        for (const char* s = app->name; s && *s && p < TASK_NAME_MAX; s++)
            nm[p++] = *s;
        nm[p] = 0;
        struct task* host =
            task_spawn_arg(nm, app_host_main, (void*)(uintptr_t)app->launch);
        if (!host) { kprintf("gui: app-host spawn failed for '%s'\n", app->name); continue; }
        /* The compositor owns the host's reap (window-teardown ordering) —
         * keep init off it, same contract as WIN_TERM shells. */
        task_set_reap_owned(host, 1);
    }
    if (power_req == 1) system_reboot();
    if (power_req == 2) system_power_off();
    if (exit_req) {
        /* Not here: this is the compositor, and the teardown kills it.  Hand
         * the job to a task outside the session (see gui_teardown). */
        exit_req = 0;
        if (!task_spawn_detached("gui-stop", gui_stop_main))
            kprintf("gui: cannot spawn the teardown task - session stays up\n");
    }
    if (restart_req) {
        /* Same reasoning, same escape: the restart TEARS DOWN this compositor
         * before it builds the next session, so it cannot run here. */
        restart_req = 0;
        if (!task_spawn_detached("gui-session", gui_session_main))
            kprintf("gui: cannot spawn the session task - session stays up\n");
    }
}




/* M22.7 — is `t` still referenced by any live window (app-host or the
 * terminal-shell task)?  Used to decide when an app-host can be reaped. */
static int gui_task_referenced(struct task* t) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* w = &windows[i];
        if (!w->used) continue;
        if (w->host_task == t) return 1;
        if (w->kind == WIN_TERM && w->vc && w->vc->task == t) return 1;
    }
    return 0;
}

/* Reap an app-host once it is DEAD and owns no more windows.  The host is
 * reap_owned (init won't touch it), so the compositor is its sole reaper. */
static void reap_gui_host(struct task* host) {
    if (!host || host->state != TASK_DEAD) return;
    if (gui_task_referenced(host)) return;
    task_reap(host->pid);
}

/* Sweep for DEAD reap_owned tasks no window references — catches a
 * singleton app whose open fn only raised an existing window (its host
 * created nothing and exited immediately).  Run on task-set changes.
 * A terminal shell mid-teardown is still referenced (win->vc->task), so
 * this never races the WIN_TERM reap path. */
struct gui_host_scan { int pids[GUI_MAX_WINDOWS * 2]; int n; };
static void gui_host_scan_cb(const struct task* t, int is_current, void* ctx) {
    struct gui_host_scan* s = (struct gui_host_scan*)ctx;
    if (is_current || t->state != TASK_DEAD || !t->reap_owned) return;
    if (s->n < (int)(sizeof s->pids / sizeof s->pids[0])) s->pids[s->n++] = t->pid;
}
static void reap_dead_gui_hosts(void) {
    struct gui_host_scan s = { .n = 0 };
    task_for_each(gui_host_scan_cb, &s);
    for (int i = 0; i < s.n; i++) {
        struct task* t = task_find(s.pids[i]);
        if (t && !gui_task_referenced(t)) task_reap(t->pid);
    }
}

void apply_pending(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used) continue;

        /* M22.6 — auto-close a terminal window whose hosted task has died
         * by ANY route: the window's own X button (want_close, below),
         * the Task Manager's "End task", a CLI `kill`, or the task simply
         * returning from its entry.  Without this, an externally-killed
         * shell left its (now inert, un-typeable) window on screen.
         *
         * The trigger is TASK_DEAD — the task has ACTUALLY stopped.  A
         * task merely FLAGGED to stop (task_kill sets kill_pending; a
         * kthread only dies at its next yield / task_should_stop poll) is
         * still RUNNABLE, so its window stays until it truly terminates —
         * that is the "instruction to stop" vs "has stopped" distinction.
         *
         * Safe pointer: a VC-bound DEAD task is reaped ONLY by the
         * want_close path below (which nulls win->vc->task); the Task
         * Manager's reap pass skips vc_task_bound tasks.  So win->vc->task
         * stays valid here until we tear it down. */
        if (!win->want_close && win->kind == WIN_TERM &&
            win->vc && win->vc->task &&
            win->vc->task->state == TASK_DEAD) {
            kprintf("gui: window '%s' auto-closing (hosted pid %d died)\n",
                    win->title, win->vc->task->pid);
            win->want_close = 1;
        }

        /* M22.7 — same for a WIN_APP whose host task died (e.g. End task /
         * CLI kill of the app-host).  host_task is reap_owned, so it stays
         * valid until WE reap it below — the ->state read is safe. */
        if (!win->want_close && win->kind == WIN_APP &&
            win->host_task && win->host_task->state == TASK_DEAD &&
            !win->host_released) {
            win->want_close = 1;
        }

        /* §M46 — a CLIENT-MANAGED window (dosgui bridge, host_task == NULL) whose
         * ring-3 client died WITHOUT a clean DOSGUI_DESTROY (force-kill / crash):
         * the client can no longer release the window, so the compositor does it.
         * task_find(NULL/DEAD) = the client is gone → mark disposable.  (init is
         * the client's reaper; we only dispose the window.) */
        if (!win->want_close && win->kind == WIN_APP && !win->host_released &&
            win->host_task == NULL && win->client_pid > 0) {
            struct task* ct = task_find(win->client_pid);
            if (!ct || ct->state == TASK_DEAD) {
                kprintf("gui: client-managed window '%s' orphaned (pid %d gone) - disposing\n",
                        win->title, win->client_pid);
                win->host_released = 1;   /* client gone; nothing to coordinate */
                win->want_close    = 1;   /* teardown below disposes it */
            }
        }

        /* §M46 — a client-managed (package) window whose X button was clicked
         * (want_close).  A dosgui client is expected to poll the close event and
         * quit itself, but a WEDGED client (frozen browser) never will, so its
         * window could otherwise never close — "the chrome doesn't work when the
         * app is frozen".  Handle it on the compositor task (outside state_lock —
         * task_find/task_force_kill take the scheduler lock, which must not nest
         * under state_lock):
         *   - client already gone → mark host_released so the teardown disposes;
         *   - client still alive + close_forces_kill → force-kill it (M46), then
         *     wait for it to die (the next passes fall into the "gone" branch).
         * The X button thus ALWAYS closes the window.  With close_forces_kill off
         * the window waits for a cooperative quit instead (classic behaviour). */
        if (win->want_close && win->kind == WIN_APP && !win->host_released &&
            win->host_task == NULL && win->client_pid > 0) {
            struct task* ct = task_find(win->client_pid);
            if (!ct || ct->state == TASK_DEAD) {
                win->host_released = 1;              /* nothing left to coordinate */
            } else if (close_forces_kill && !ct->kill_forced) {
                uint64_t now = timer_ticks_ms();
                if (win->close_force_now) {
                    kprintf("gui: second close click on '%s' -> force-killing "
                            "client pid %d\n", win->title, win->client_pid);
                    task_force_kill(win->client_pid);
                } else if (!win->close_deadline_ms) {
                    /* First pass: start the backstop and let the client quit. */
                    win->close_deadline_ms = now + close_grace_ms;
                } else if (now >= win->close_deadline_ms) {
                    kprintf("gui: '%s' did not close within %ums -> force-killing "
                            "client pid %d\n", win->title, close_grace_ms,
                            win->client_pid);
                    task_force_kill(win->client_pid);
                }
            }
        }

        /* M22.7 — WIN_APP teardown is a two-actor dance.  Normally the host
         * sees want_close, runs on_close + frees its widgets, and sets
         * host_released; we then dispose the struct.  If the host died
         * WITHOUT releasing (it was killed), we do that cleanup here since
         * the host can no longer touch the widgets. */
        if (win->want_close && win->kind == WIN_APP) {
            if (!win->host_released) {
                int host_dead = win->host_task &&
                                win->host_task->state == TASK_DEAD;
                if (!host_dead) continue;       /* host still cleaning up */
                if (win->on_close) win->on_close(win);
                app_widgets_free(win);
                win->host_released = 1;
            }
            struct task* host = win->host_task;
            kprintf("gui: app window '%s' closed (host pid %d)\n",
                    win->title, host ? host->pid : -1);
            win->want_close = 0;
    win->close_deadline_ms = 0;
    win->close_force_now = 0;
            destroy_window(win);
            reap_gui_host(host);                /* reap once its last window is gone */
            continue;
        }

        if (win->want_close) {
            if (win->kind == WIN_TERM && win->vc && win->vc->task) {
                /* Kill the hosted shell first (cooperative — it dies at
                 * its next vc_getchar yield), then reap; retry on the
                 * next compositor pass until the reap succeeds.  Only
                 * then is it safe to free the VC and the surface.
                 *
                 * M27 — kill the whole SUBTREE: anything the shell spawned
                 * (e.g. `spawn`) dies with the window instead of orphaning.
                 * The shell itself is reap_owned, so WE reap it here; its
                 * (non-owned) children are reaped by init once they die. */
                struct task* t = win->vc->task;
                task_kill_tree(t->pid);
                if (task_reap(t->pid) != 0) continue;   /* not DEAD yet */
                win->vc->task = NULL;
            }
            if (win->kind == WIN_TERM && win->vc) {
                vc_destroy(win->vc);
                win->vc = NULL;
            }
            win->want_close = 0;
    win->close_deadline_ms = 0;
    win->close_force_now = 0;
            destroy_window(win);
            continue;
        }

        int nw = 0, nh = 0;
        uint32_t fl = spin_lock_irqsave(&state_lock);
        if (win->pending_w) {
            nw = win->pending_w;  nh = win->pending_h;
            win->pending_w = win->pending_h = 0;
            win->w = nw;  win->h = nh;
        }
        spin_unlock_irqrestore(&state_lock, fl);

        if (nw && window_set_size(win, nw, nh) != 0)
            kprintf("gui: resize OOM (%dx%d), window keeps stale surface\n", nw, nh);
    }
}


/* §M65 — DRAW the widgets of a window that has no host task.
 *
 * §M40 taught the compositor to PUMP INPUT for such a window (a Wayland or
 * dosgui client's window has `host_task` cleared by design, §M54) — and the
 * same hole existed on the drawing side, invisibly, until a ring-3 program
 * built toolkit widgets in its window and got an empty rectangle: the widgets
 * existed, the layout ran, and nothing ever painted them.
 *
 * A window in this mode uses the toolkit INSTEAD of blitting its own pixels;
 * if a client does both, the last writer wins, which is the honest outcome of
 * asking for both. */
static void pump_hostless_redraw(void) {
    for (int i = 0; i < GUI_MAX_WINDOWS; i++) {
        struct gui_window* win = &windows[i];
        if (!win->used || win->kind != WIN_APP) continue;
        if (win->host_task || !win->widgets) continue;
        if (!win->layout_pending) continue;
        win->layout_pending = 0;
        ui_layout(win);
        app_redraw(win);
    }
}

/* §M75.2 — compositor loop accounting; see the note in the loop body. */
static uint64_t gl_work_ns, gl_halt_ns, gl_loop_ns, gl_since;
static uint32_t gl_iters;

static void gui_compositor_main(void) {
    kprintf("gui: compositor up on pid %d (shell '%s')\n",
            task_current() ? task_current()->pid : -1,
            shell ? shell->name : "none");
    uint64_t last_tick = 0;
    for (;;) {
        /* M22.7 — app launches moved to the desktop task (so launched apps
         * are children of the desktop/session, not the display server). */
        dispatch_events();
        dispatch_keys();
        dispatch_keycodes();
        pump_hostless_input();
        pump_hostless_redraw();          /* §M65 — …and paint them */
        apply_pending();
        /* §M70 — the instruments report from HERE, not from the task that
         * asked: with the GUI up, a kprintf on the requesting task goes to the
         * suppressed console and the answer reaches nobody. */
        gui_diag_service();
        term_selection_service();       /* §M58 — repaint + copy */
        apply_mode_change();            /* §M61 — resolution, between frames */
        apply_mode_revert();            /* §M61 — …and the undo, same rule */

        /* ~1 Hz per-window housekeeping.  (The shell clock moved to the
         * desktop task in M22.7-B; the compositor no longer runs it.) */
        uint64_t now = timer_ticks_ms();
        if (now - last_tick >= 500) {
            last_tick = now;
            /* M22.3/M22.7: per-window ~1 Hz ticks (e.g. task manager
             * refresh) — signal the owning app-host, which runs on_tick on
             * its own task rather than blocking the compositor here. */
            for (int i = 0; i < GUI_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick)
                    windows[i].tick_pending = 1;
        }

        /* M22.4: task set changed (spawn/kill/exit/reap) → nudge the tick
         * refreshes immediately, don't wait for the 1 Hz beat.  Ticks are
         * idempotent refreshes, so an early one is safe. */
        if (tasks_changed) {
            tasks_changed = 0;
            for (int i = 0; i < GUI_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].on_tick)
                    windows[i].tick_pending = 1;
            reap_dead_gui_hosts();          /* M22.7 — sweep exited app-hosts */
        }

        /* M22.7 — latency fix: halt the CPU only when there is NOTHING to
         * compose.  The old unconditional hal_cpu_idle() slept a whole timer
         * tick every iteration, so with several always-runnable tasks
         * (compositor + desktop + app-hosts) the compositor's turn came
         * around only every N ticks — visible cursor lag with the menu or
         * Task Manager open.  Under load need_frame stays set, so we spin
         * through the scheduler (fast); when truly idle we hlt (power save). */
        /* §M75.2 — WHERE DOES THE COMPOSITOR'S TIME ACTUALLY GO?
         *
         * §M75's chart put a number on §M49's open item for the first time: an
         * idle desktop reads ~50 % of a 4-CPU box, and `ps` attributes it to
         * this task and the desktop task.  **But `busy_ms` is credited to
         * whichever task is SCHEDULED, and this loop spends most of its turns
         * halted inside `hal_cpu_idle()`** — so "50 % busy" may be 50 % of real
         * work or 50 % of a task sitting in `hlt`, and those call for opposite
         * fixes.  A waitq is the answer to the second and a waste of a risky
         * change to the most timing-sensitive code in the tree if it is the
         * first.
         *
         * So the loop times its own halt.  §M69's rule, paid for twice there:
         * an optimisation aimed at a guess cannot be measured, only assumed. */
        uint64_t l0 = timer_now_ns();
        if (need_frame) {
            need_frame = 0;
            compose();
            gl_work_ns += timer_now_ns() - l0;
        } else {
            /* §M75.2 — TIME THE HALT ITSELF, not the branch around it.
             *
             * The first version stamped `l0` before the `if` and charged the
             * whole else-arm to "halted", which reported 97 % in `hlt` — and
             * that reading contradicted the task's own `cpu_ms`, which had not
             * moved at all.  One of the two had to be lying, and it was the
             * instrument: §4.61's lesson (a measurement placed on the wrong
             * side of the work reports the work as free) with the sides
             * swapped.  The halt is now bracketed exactly, and whatever is
             * left over is loop overhead — which is the number that decides
             * whether a waitq is worth the risk. */
            uint64_t h0 = timer_now_ns();
            task_halt_idle();
            uint64_t h1 = timer_now_ns();
            gl_halt_ns += h1 - h0;
            gl_loop_ns += h1 - l0 - (h1 - h0);
        }
        gl_iters++;
        if (!gl_since) gl_since = l0;
        else if (config_get_long("gui.stats_ms", 0) > 0 &&
                 timer_now_ns() - gl_since > 5000000000ull) {
            uint64_t span = (timer_now_ns() - gl_since) / 1000000ull;
            kprintf("compositor: %u loops in %u ms (%u/s) - %u %% composing, "
                    "%u %% in hlt, %u %% loop overhead\n",
                    (unsigned)gl_iters, (unsigned)span,
                    (unsigned)((uint64_t)gl_iters * 1000ull / (span ? span : 1)),
                    (unsigned)(gl_work_ns / 10000ull / (span ? span : 1)),
                    (unsigned)(gl_halt_ns / 10000ull / (span ? span : 1)),
                    (unsigned)(gl_loop_ns / 10000ull / (span ? span : 1)));
            gl_since = timer_now_ns(); gl_work_ns = gl_halt_ns = gl_loop_ns = 0; gl_iters = 0;
        }
        task_yield();
    }
}

/* -------------------------------------------------------------------------- */
/* Pointer handling — IRQ context.                                             */
/* -------------------------------------------------------------------------- */



/* §M63 — declared next to the code that READS it, which is this file's wheel
 * router.  A descriptor means the Appearance panel renders it with no per-key
 * UI code, and `conf set` validates it. */
CONFIG_KEY(ck_scroll_invert) = {
    .key = "gui.scroll_invert", .group = "Appearance", .type = CFG_BOOL,
    .def = "0",
    .help = "reverse the wheel direction (Mac-style natural scrolling)",
    .scope = CFG_SCOPE_USER,
};






/* -------------------------------------------------------------------------- */
/* Window creation + bring-up.                                                 */
/* -------------------------------------------------------------------------- */



/* Shared body of gui_window_create / gui_window_create_task: a
 * terminal window whose hosted task is the caller's choice.  The task
 * gets the window's offscreen VC as its output console and is owned by
 * the window (vc->task — the close path kills + reaps it). */
struct gui_window* term_window_create(const char* title,
                                             int x, int y, int w, int h,
                                             const char* task_name,
                                             void (*entry)(void),
                                             int shell_ppid) {
    struct gui_window* win = window_alloc(title, WIN_TERM, x, y, w, h);
    if (!win) return NULL;

    win->cells = (char*)kcalloc(1, (size_t)gmax_cols * gmax_rows);
    if (!win->cells) { win->used = 0; return NULL; }

    /* §M58 — scrollback.  Sized from config, in LINES, because that is the unit
     * the user thinks in; the bytes follow from the screen width.  A failed
     * allocation is not fatal: sb_cap stays 0 and the terminal behaves exactly
     * as it did before scrollback existed — a window that refuses to open
     * because it could not get its history would be a worse trade. */
    {
        long want = config_get_long("gui.scrollback", 500);
        if (want < 0)    want = 0;
        if (want > 5000) want = 5000;
        if (want > 0) {
            win->sb = (char*)kcalloc(1, (size_t)gmax_cols * (size_t)want);
            win->sb_cap = win->sb ? (int)want : 0;
        }
        win->sb_count = win->sb_head = 0;
        win->scrolled = win->view_off = 0;
    }

    if (window_set_size(win, win->w, win->h) != 0) {
        kfree(win->cells);
        win->used = 0;
        return NULL;
    }

    win->vc = vc_create_offscreen(gterm_emit, win);
    if (!win->vc) {
        gfx_surface_free(&win->surf);
        kfree(win->cells);
        win->used = 0;
        return NULL;
    }

    /* M22.7 — parent the shell as requested: the desktop/session (session
     * shells, so a kill_tree(desktop) takes them with it), init (detached
     * shells, which outlive the session), or the caller (< 0).  Without this
     * a shell launched from the taskbar orphaned to init when its transient
     * launcher app-host exited.
     * §M49 — the window's VC is bound by the spawn; setting it afterwards
     * raced the task's own start on another core (preempt_disable is
     * per-CPU and does not hold that off). */
    struct task* t = task_spawn_console(task_name, entry, shell_ppid, win->vc);
    if (t) {
        win->vc->task = t;
        /* M27 — this window owns its shell's reap (the close teardown
         * kills + reaps it and nulls the pointer).  Tell init's universal
         * reaper to keep its hands off, so the two never race for the
         * same struct. */
        task_set_reap_owned(t, 1);
    }
    if (!t) kprintf("gui: task spawn failed for '%s'\n", win->title);

    window_show(win);
    return win;
}





struct gui_window* gui_app_window_create(const char* title, int x, int y,
                                         int w, int h,
                                         void (*on_layout)(struct gui_window*),
                                         void* app_ctx) {
    struct gui_window* win = window_alloc(title, WIN_APP, x, y, w, h);
    if (!win) return NULL;
    win->on_layout = on_layout;
    win->app_ctx   = app_ctx;
    /* M22.7 — bind the window to the app-host that is creating it (this runs
     * inside the app's open fn, which the host task invokes).  The host's
     * loop then owns this window's events + rendering + teardown. */
    win->host_task = task_current();

    if (window_set_size(win, win->w, win->h) != 0) {
        win->used = 0;
        return NULL;
    }
    window_show(win);
    return win;
}

/* ---------------------------------------------------------------------------
 * §M81 step 2 — the window lifecycle, once.  See gui.h for the measurement
 * this replaces: twenty hand-rolled copies of the same six lines, and every
 * §M32 GUI defect living in them rather than in the compositor.
 * ------------------------------------------------------------------------- */

/* Turn a placement INTENT into a position, clamped so the result is on screen.
 *
 * The clamp is not a tidiness: `window_alloc` stores x/y VERBATIM, so nothing
 * below this line would have refused a window placed off the edge — which is
 * how `-1,-1` produced a sign-in screen nobody could see, and how a window
 * sized past the bottom of a 1200 px framebuffer read as a broken layout
 * (§M69's own gallery, at the measured 137 % density). */
static void place_for(int intent, int ow, int oh, int ax, int ay,
                      int* out_x, int* out_y) {
    int sw = gui_screen_w(), sh = gui_screen_h();
    int x, y;
    switch (intent) {
    case GUI_PLACE_CENTER:
        x = (sw - ow) / 2; y = (sh - oh) / 2;
        break;
    case GUI_PLACE_DIALOG:
        x = (sw - ow) / 2; y = (sh - oh) / 3;
        break;
    case GUI_PLACE_AT:
        x = ax; y = ay;
        break;
    default: {
        /* CASCADE — the next slot in a stagger.  Every app used to carry its
         * own literal origin (180,100 / 140,110 / 120,100 / 300,120 …), each
         * approximating this by hand and none of them aware of the others, so
         * two panels opened in a row could land exactly on top of each other.
         * The counter never resets, which is right: what matters is that two
         * CONSECUTIVE opens differ, not where the series began. */
        static int nth;
        int step = cp_px(28);
        x = cp_px(120) + (nth % 8) * step;
        y = cp_px(80)  + (nth % 8) * step;
        nth++;
        break;
    }
    }
    /* Clamp last, so an intent that computes an off-screen position for a
     * window larger than the screen still yields a visible title bar. */
    if (x + ow > sw) x = sw - ow;
    if (y + oh > sh) y = sh - oh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    *out_x = x; *out_y = y;
}

struct gui_window* gui_app_open(const struct gui_app_spec* sp) {
    if (!sp) return NULL;

    /* THE SINGLETON, once.  Twelve copies of this test existed, each paired
     * with a one-line `on_close` handler whose only job was to null the same
     * pointer — and the pairing was a convention, so either half could be
     * forgotten independently. */
    if (sp->slot && *sp->slot) {
        gui_window_raise(*sp->slot);
        return *sp->slot;
    }

    /* THE HOSTING TASK, NAMED RATHER THAN ASSUMED.  A window binds to
     * `task_current()`, and one created on a task with no app-host loop never
     * lays out and never ticks — it just sits there looking like an app that
     * ignored its own events (§M61).  This cannot be made impossible without
     * taking the creation call away from the apps, but it can be made LOUD:
     * the failure is silent, and a silent failure is what cost the diagnosis
     * last time.  Reported, not refused — `gui bench` and the Wayland bridge
     * legitimately create windows outside a host. */
    if (!app_host_is_host_task(task_current())) {
        struct task* t = task_current();
        kprintf("gui: '%s' is being created on '%s' (pid %d), which runs no "
                "app-host loop - it will never lay out or tick.  Open it "
                "through gui_queue_open().\n",
                sp->title ? sp->title : "?", t ? t->name : "?", t ? t->pid : -1);
    }

    int ow = 0, oh = 0;
    gui_window_outer_for_content(sp->content_w, sp->content_h, &ow, &oh);
    int x = 0, y = 0;
    place_for(sp->place, ow, oh, sp->x, sp->y, &x, &y);

    struct gui_window* win = gui_app_window_create(sp->title, x, y, ow, oh,
                                                  sp->layout, sp->ctx);
    if (!win) return NULL;

    win->app_slot = sp->slot;
    win->on_close = sp->on_close;
    if (sp->tick)  gui_window_set_tick(win, sp->tick);
    if (sp->modal) gui_window_set_modal(win, 1);
    if (sp->slot)  *sp->slot = win;
    return win;
}

int gui_is_active(void) { return gui_active; }

/* §M42 — the desktop/session task pid (0 until gui_start spawns it).  A GUI app
 * launched from the taskbar should parent under this so it shows up under the
 * desktop session in the process tree (and dies with the session), rather than
 * being an init-owned detached task. */
int gui_desktop_pid(void) { return desktop_pid; }

/* Pick the desktop shell: `gui.shell` config value, matched against the
 * registry; falls back to "vista", then to the first registration. */
static const struct desktop_shell* pick_shell(void) {
    int n = (int)(__stop_desktop_shells - __start_desktop_shells);
    if (n == 0) return NULL;

    const char* want = config_get("gui.shell", "vista");
    for (int pass = 0; pass < 2; pass++) {
        const char* name = pass == 0 ? want : "vista";
        for (int i = 0; i < n; i++) {
            const char* a = __start_desktop_shells[i].name;
            const char* b = name;
            while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
            if (*a == 0 && *b == 0) return &__start_desktop_shells[i];
        }
    }
    return &__start_desktop_shells[0];
}



void gui_desktop_icons_changed(void) {
    if (!gui_active) return;
    /* The icon field is the whole work area (screen minus the taskbar).  A
     * finer rect would need the shell's layout, and this runs on an add or a
     * delete — rare, human-paced events, not the per-click path that §4.61's
     * damage discipline is about. */
    gui_damage(0, 0, fbsurf.w, work_h);
}

int gui_wallpaper_reload(void) {
    if (!gui_active) {
        /* Not running: the config key is set and boot will read it.  Report
         * what a render WOULD do without pretending we painted anything. */
        return 0;
    }
    int rc = paint_wallpaper();
    gui_damage_all();
    return rc;
}


/* ==========================================================================
 * gui_stop — end the session and hand the screen back to the text console.
 *
 * This is the exact inverse of gui_start, and the ORDER is the whole design:
 * every step below undoes something that a still-running compositor would
 * otherwise be using.
 *
 * It runs on a task of ITS OWN (`gui-stop`, detached → parented to init) for
 * one structural reason: the teardown kills the desktop session, and the
 * compositor — where a Start-menu click is dispatched — lives inside that
 * session.  A task cannot free the surfaces it is still composing from, and it
 * certainly cannot outlive its own kill_tree to do the tidying afterwards.
 * ========================================================================== */

static int gui_teardown(void) {
    if (!gui_active) return -1;

    /* 1. INPUT FIRST.  An event delivered into a compositor that is being torn
     *    down is the classic teardown crash: the queues it drains, the windows
     *    it routes to and the surfaces it draws into are all about to go away,
     *    and the mouse IRQ does not know that. */
    mouse_set_listener(NULL);
    mouse_set_wheel_listener(NULL);
    vc_set_kbd_hook(NULL);
    vc_set_raw_kbd_hook(NULL);
    task_set_change_hook(NULL);

    /* 2. Hand every app-host's REAP back to init before its owner dies.  The
     *    compositor claims the reap of the hosts it spawns (window-teardown
     *    ordering, apply_pending); with the compositor gone, a host still
     *    marked reap_owned would be a corpse nobody is allowed to collect —
     *    §M27's universal reaper deliberately skips owned tasks. */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].host_task)
            task_set_reap_owned(windows[i].host_task, 0);

    /* 3. Kill the session.  The desktop is the session root (gui_start), so one
     *    kill_tree takes the compositor, the app-hosts and every terminal with
     *    it — the same "parent dies → children die" rule the GUI is built on. */
    int dp = desktop_pid;
    int sess[TASK_KILLTREE_MAX];
    int nsess = dp > 0 ? task_kill_tree_pids(dp, sess, TASK_KILLTREE_MAX) : 0;
    if (nsess < 0) nsess = 0;

    /* 4. WAIT for the WHOLE session to actually be gone.  Freeing a surface
     *    while the compositor is mid-compose is a use-after-free of several
     *    megabytes, and "we asked it to die" is not the same statement as "it
     *    is dead".  Poll for DISAPPEARANCE rather than task_wait()ing: init is
     *    a universal reaper and may collect a task first, and waiting on a
     *    child somebody else reaped never completes (§M57).
     *
     *    §M82 — EVERY MEMBER, NOT THE ROOT.  This used to wait for the desktop
     *    alone, and the desktop dying says nothing about the compositor under
     *    it — which is exactly the task the paragraph above is about.  The
     *    compositor regularly outlived it (logs show `reaped 'compositor'`
     *    AFTER `session ended`); a full-screen composite in flight then blitted
     *    ~9 MB of wallpaper into a freed back buffer, and whatever the heap
     *    handed out next — the NEW session's kernel stacks among it — was
     *    overwritten.  Measured as a GPF at rip = 0xff0000ffff0000ff (two
     *    wallpaper pixels) and as NMIs with the CPUs executing inside the font
     *    DATA tables, 3 runs in 3 on x86_64 with a theme change mid-session
     *    (which makes every frame full-screen and so widens the window).
     *    The membership is captured AT KILL TIME because once the root is dead
     *    its children are re-parented to init and cannot be found from it. */
    {
        int alive = 0;
        for (int round = 0; round < 400; round++) {
            alive = 0;
            for (int k = 0; k < nsess; k++) if (task_find(sess[k])) alive++;
            if (!alive) break;
            task_msleep(5);
        }
        if (alive) {
            for (int k = 0; k < nsess; k++) {
                struct task* t = task_find(sess[k]);
                if (t) klog(KLOG_WARN, "gui", "session task '%s' (pid %d) outlived the "
                                             "teardown deadline - freeing anyway\n",
                            t->name, t->pid);
            }
        }
    }

    /* 5. Windows.  Their hosts are dead, so nothing will run on_close on its
     *    own task any more; destroy_window also fires the dispose callback
     *    that tells a dosgui client's bridge its window is gone (§M54). */
    for (int i = 0; i < GUI_MAX_WINDOWS; i++)
        if (windows[i].used) destroy_window(&windows[i]);
    zcount = 0;
    focused_win = NULL;
    drag = DRAG_NONE;
    drag_win = NULL;

    /* 6. The screen buffers. */
    gfx_surface_free(&backsurf);
    gfx_surface_free(&wallsurf);
    if (panel_buf) { kfree(panel_buf); panel_buf = NULL; }
    panel_ready = 0;
    panelsurf.px = NULL;

    /* 7. Put the SCANOUT back on buffer 0.  The console writes into the base
     *    framebuffer; if the page flip left the display panned to the second
     *    buffer, every restored line would be written to memory nobody is
     *    looking at — a black screen produced by a working console. */
    if (flip_ok) fb_flip_to(0);
    flip_ok = 0;
    flip_front = 0;

    gui_active   = 0;
    desktop_pid  = 0;
    exit_req     = 0;
    need_frame   = 0;

    /* 8. Give the screen back, and put something on it.  A leaf VC has no cell
     *    backing store — output produced while the GUI owned the screen was
     *    DROPPED, not buffered — so there is nothing to restore, only a clean
     *    slate to draw. */
    vc_screen_suppress(0);
    struct vc* root = vc_root();
    if (root) {
        vc_clear(root);
        vc_focus(root);
    }
    kprintf("gui: session ended - back at the text console\n");

    /* The shell is blocked reading a LINE; it prints its prompt after it gets
     * one.  Feed it an empty line so a prompt appears immediately instead of
     * the user having to press Enter at an apparently dead screen. */
    vc_kbd_push('\n');
    /* 9. §M82 — END THE ACCOUNT'S SESSION HERE, where every route ends one.
     *    The first version of the fix put the withdrawal in gui_stop_main,
     *    which is only the Start menu's "Exit GUI" route; the `gui stop`
     *    COMMAND calls gui_teardown directly and so kept the last user's
     *    preferences on the console.  One call in the one function every
     *    teardown passes through cannot be missed by the next route. */
    config_user_detach();

    return 0;
}

/* §M32 stage 10 — the lock instrument runs on its own task; see gui_start. */
static const char* gui_locktest_creds;
static void gui_locktest_main(void) {
    if (gui_locktest_creds) gui_lock_test(gui_locktest_creds);
}

/* §M76 — see the autorun note at the end of gui_start. */
static const char* gui_autorun_cmd;
static void gui_autorun_main(void) {
    /* The desktop is up but its first frame may not have been composed yet;
     * a moment's grace keeps the launched window's own layout out of the
     * bring-up's damage list, which is tidier to read in a log and costs
     * nothing a human would notice. */
    task_msleep(300);
    if (gui_autorun_cmd && !shell_cmd_dispatch(gui_autorun_cmd))
        kprintf("gui: autorun - unknown command '%s'\n", gui_autorun_cmd);
}

CONFIG_KEY(ck_pageflip) = {
    .key = "gui.page_flip", .group = "Display", .type = CFG_BOOL, .def = "1",
    .help = "double-buffered present (off = single buffer; diagnostic)",
};

CONFIG_KEY(ck_autorun) = {
    .key = "gui.autorun", .group = "System", .type = CFG_STRING, .def = "",
    .help = "one shell command to run once the desktop is up (test hook)",
};

/* §M81 — THE GUI'S SESSION LEADER.  Three steps, in this order, and the order
 * is the whole design:
 *
 *   1. TEAR THE OLD SESSION DOWN.  §M64 built and verified this for Start ->
 *      Exit GUI; it kills the compositor, which is why this runs on a detached
 *      task outside the session rather than on the one that asked.
 *   2. ADOPT THE ACCOUNT, before this task has spawned anything.  That is the
 *      one moment `cred_become_user` permits, and the reason a display
 *      manager's greeter is not its session.
 *   3. BUILD THE NEW SESSION.  `spawn_common` takes a child's identity from its
 *      CALLER (§M32: deliberately not from `ppid`, or detaching would launder
 *      it), so the desktop, the compositor and every app-host spawned from here
 *      inherit the account without any of them knowing about login.
 *
 * A FAILED ADOPTION DOES NOT FALL BACK TO SYSTEM.  `login.c` refuses to run the
 * session in that case, and the argument is the same one: *a session running
 * with the console's identity is precisely the privilege the login was supposed
 * to drop.*  Here that would be worse, because the desktop would come up
 * looking signed in. */
static void gui_session_main(void) {
    gui_teardown();

    /* §M82 — THE PREVIOUS USER LEAVES BEFORE THE NEXT ONE ARRIVES.  Every route
     * that replaces a session passes through here (a sign-in over another
     * session, Sign out, the lock screen's switch), and only the TEXT logout
     * used to withdraw preferences — so the next person, or the greeter,
     * inherited every choice the last one made, and the next user's first
     * change copied them into their own store.  Measured by `sessiontest`.
     * A no-op when nobody was attached. */
    config_user_detach();

    if (pend_uid >= 0) {
        struct task* me = task_current();
        if (!me || cred_become_user(me->pid, pend_uid, pend_gid, pend_groups,
                                    pend_ngroups, pend_session) != 0) {
            kprintf("gui: could not adopt '%s' - REFUSING to open the desktop "
                    "(a session that cannot become the user must not look like "
                    "one that did)\n", pend_name);
            pend_uid = -1;
            return;
        }
        /* §M32 stage 9 — the account's preferences, AFTER the identity, because
         * `config_apply`'s watchers run as this task and a wallpaper applied
         * while still SYSTEM would be the console's rather than the user's. */
        config_user_attach(pend_uid);
        kprintf("gui: session %d opened for '%s' (uid %d) on pid %d\n",
                pend_session, pend_name, pend_uid, me->pid);
        skip_lock_once = 1;             /* it just authenticated */
        pend_uid = -1;
    }
    in_session_leader = 1;
    gui_start();
    in_session_leader = 0;
}

void gui_session_restart_as(const char* name) {
    pend_uid = -1;
    pend_name[0] = 0;
    if (name) {
        const struct user_account* u = user_by_name(name);
        if (!u) {
            kprintf("gui: no account '%s' - not restarting the session\n", name);
            return;
        }
        pend_uid     = u->uid;
        pend_gid     = u->gid;
        pend_session = gui_next_session++;
        pend_ngroups = user_groups_of(u->uid, pend_groups, CRED_MAX_GROUPS);
        str_copy(pend_name, u->name, sizeof pend_name);
    }
    /* WHO ACTS ON THIS depends on whether a session is up.  With one running,
     * the restart must NOT happen on the caller — it tears down the compositor
     * that dispatched the click — so it is queued and the compositor hands it
     * to a detached task.  With none (boot, autologin), there is nothing to
     * queue it to and nothing to tear down, so the leader starts directly. */
    if (!gui_active) {
        if (!task_spawn_detached("gui-session", gui_session_main))
            kprintf("gui: cannot spawn the session task\n");
        return;
    }
    restart_req = 1;
    need_frame  = 1;
}

/* §M82 — `sessiontest <a> <b>`: THE PREFERENCE BOUNDARY BETWEEN TWO SESSIONS,
 * DRIVEN WITHOUT TYPING.
 *
 * Why it exists: the question "does user B see user A's wallpaper?" needs two
 * sessions in a row, and this project's harness loses keystrokes across a GUI
 * start/stop (§4.74's wall, one layer over — a typed `gui stop` arrived merged
 * with the next command).  So the scenario runs on its OWN task through the
 * REAL route (`gui_session_restart_as`, the call the lock screen and Sign out
 * make) and prints one verdict per property.
 *
 * Password checking is deliberately NOT part of it: that is `gui.locktest`'s
 * job, and folding it in would make a failure here ambiguous between "the
 * login refused" and "the preferences leaked".
 *
 * What it checks, each a separate line so one failure cannot hide another:
 *   1. B's session does not show a preference A set and B did not;
 *   2. B's store on disk holds only what B set (not A's, not the machine's);
 *   3. saving the MACHINE store during B's session does not write B's choice
 *      into it (one person's wallpaper would become everybody's);
 *   4. after sign-out the machine's own value is back — not the compiled
 *      default, which would silently discard what an administrator chose.
 *
 * It CHANGES the machine's state (two user stores and the machine store), so
 * it is hidden from `help` like the other falsifiers and wants a scratch disk. */
static char st_a[32], st_b[32];

static int st_wait_uid(int uid, int want_gui) {
    for (int i = 0; i < 400; i++) {                 /* 400 x 50 ms = 20 s */
        if (config_user_active() == uid && (!want_gui || gui_active)) return 0;
        task_msleep(50);
    }
    return -1;
}

/* Does `path` contain the line prefix `key` + " ="?  A file that cannot be
 * opened answers "no", which is what a missing store means. */
static int st_file_has_value(const char* path, const char* key, const char* val) {
    static char buf[2048];
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return 0;
    ssize_t n = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (n <= 0) return 0;
    buf[n] = 0;
    int kl = 0; while (key[kl]) kl++;
    for (ssize_t i = 0; i < n; i++) {
        if (i && buf[i - 1] != '\n') continue;
        int k = 0;
        while (k < kl && buf[i + k] == key[k]) k++;
        if (k != kl || (buf[i + k] != ' ' && buf[i + k] != '=')) continue;
        if (!val) return 1;
        int p = i + k;
        while (buf[p] == ' ' || buf[p] == '=') p++;
        int j = 0;
        while (val[j] && buf[p + j] == val[j]) j++;
        if (!val[j] && (buf[p + j] == '\n' || buf[p + j] == 0)) return 1;
    }
    return 0;
}
static int st_file_has(const char* path, const char* key) {
    return st_file_has_value(path, key, NULL);
}

static int st_same(const char* a, const char* b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void st_user_path(int uid, char* out, int cap) {
    /* Mirrors config.c's layout: "<dir of the machine store>/d-os-user-<uid>.conf".
     * Rebuilt here rather than exported because a test that asks the code
     * under test where to look can only ever agree with it. */
    const char* base = config_persist_path();
    int n = 0, last = -1;
    out[0] = 0;
    if (!base) return;
    for (int i = 0; base[i]; i++) if (base[i] == '/') last = i;
    for (int i = 0; i < last && n < cap - 32; i++) out[n++] = base[i];
    const char* leaf = "/d-os-user-";
    for (int i = 0; leaf[i] && n < cap - 16; i++) out[n++] = leaf[i];
    char num[12]; int m = 0, v = uid < 0 ? 0 : uid;
    if (v == 0) num[m++] = '0';
    while (v > 0 && m < 12) { num[m++] = (char)('0' + v % 10); v /= 10; }
    while (m > 0 && n < cap - 6) out[n++] = num[--m];
    const char* ext = ".conf";
    for (int i = 0; ext[i] && n < cap - 1; i++) out[n++] = ext[i];
    out[n] = 0;
}

static void gui_sessiontest_main(void) {
    const struct user_account* ua = user_by_name(st_a);
    const struct user_account* ub = user_by_name(st_b);
    if (!ua || !ub) { kprintf("sessiontest: no such account\n"); return; }
    int uida = ua->uid, uidb = ub->uid;
    const char* mp = config_persist_path();
    if (!mp) { kprintf("sessiontest: no writable volume - nothing to test\n"); return; }

    /* The machine's own values, captured BEFORE any session touches them. */
    static char m_wall[160], m_theme[32];
    str_copy(m_wall,  config_get("gui.wallpaper", ""), sizeof m_wall);
    str_copy(m_theme, config_get("gui.theme", ""),     sizeof m_theme);
    kprintf("sessiontest: machine gui.wallpaper='%s' gui.theme='%s'\n", m_wall, m_theme);

    int fails = 0;

    gui_session_restart_as(st_a);
    if (st_wait_uid(uida, 1)) { kprintf("sessiontest: '%s' never became active\n", st_a); return; }
    config_apply("gui.wallpaper", "solid:FF0000");        /* A's choice */

    gui_session_restart_as(st_b);
    if (st_wait_uid(uidb, 1)) { kprintf("sessiontest: '%s' never became active\n", st_b); return; }

    const char* w = config_get("gui.wallpaper", "");
    int ok1 = st_same(w, m_wall);
    kprintf("sessiontest: 1 %s - '%s' sees gui.wallpaper='%s' (machine '%s')\n",
            ok1 ? "ok  " : "FAIL", st_b, w, m_wall);
    fails += !ok1;

    config_apply("gui.theme", "light");                   /* B's choice */
    char pb[96]; st_user_path(uidb, pb, sizeof pb);
    int leak_a = st_file_has(pb, "gui.wallpaper");
    int leak_m = st_file_has(pb, "keyboard.layout");
    int has_b  = st_file_has(pb, "gui.theme");
    int ok2 = has_b && !leak_a && !leak_m;
    kprintf("sessiontest: 2 %s - %s holds gui.theme:%s gui.wallpaper:%s keyboard.layout:%s\n",
            ok2 ? "ok  " : "FAIL", pb, has_b ? "yes" : "NO",
            leak_a ? "YES (A's)" : "no", leak_m ? "YES (never set by B)" : "no");
    fails += !ok2;

    config_save();                                        /* a machine save mid-session */
    int ok3 = !st_same(m_theme, "light") ? !st_file_has_value(mp, "gui.theme", "light") : 1;
    kprintf("sessiontest: 3 %s - machine store %s gui.theme=light after a save in '%s''s session\n",
            ok3 ? "ok  " : "FAIL", ok3 ? "does not carry" : "CARRIES", st_b);
    fails += !ok3;

    gui_session_restart_as(NULL);                         /* Sign out */
    if (st_wait_uid(-1, 1)) { kprintf("sessiontest: sign-out never withdrew '%s'\n", st_b); fails++; }
    const char* w2 = config_get("gui.wallpaper", "");
    const char* t2 = config_get("gui.theme", "");
    int ok4 = st_same(w2, m_wall) && st_same(t2, m_theme);
    kprintf("sessiontest: 4 %s - after sign-out gui.wallpaper='%s' gui.theme='%s'\n",
            ok4 ? "ok  " : "FAIL", w2, t2);
    fails += !ok4;

    kprintf("sessiontest: %s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
}

static void cmd_sessiontest(const char* args) {
    int i = 0;
    while (args && *args == ' ') args++;
    while (args && *args && *args != ' ' && i < (int)sizeof st_a - 1) st_a[i++] = *args++;
    st_a[i] = 0; i = 0;
    while (args && *args == ' ') args++;
    while (args && *args && *args != ' ' && i < (int)sizeof st_b - 1) st_b[i++] = *args++;
    st_b[i] = 0;
    if (!st_a[0] || !st_b[0]) { kprintf("usage: sessiontest <user-a> <user-b>\n"); return; }
    if (!task_spawn_detached("sessiontest", gui_sessiontest_main))
        kprintf("sessiontest: cannot spawn\n");
}
SHELL_CMD(sessiontest) = { "sessiontest", "<user-a> <user-b>", 0, SHELL_G_TEST,
                           cmd_sessiontest, SHELL_P_ADMIN };

/* §M82 — `sessionstorm <a> <b> <n>`: REPLACE THE SESSION n TIMES, FAST.
 *
 * Found while measuring the preference boundary: on x86_64, both with and
 * without that fix, a machine left idle after a session switch took a GPF in
 * the compositor at `rip = 0xff0000ffff0000ff` (two wallpaper pixels) or an NMI
 * with the CPUs executing inside the font DATA tables — i.e. a code pointer
 * read out of memory that has since been reused.  One switch reproduces it
 * perhaps half the time, which is too slow to bisect with; this does many,
 * alternating A, B and a sign-out, with uneven pauses so a race is not always
 * sampled at the same phase.  Same shape as `killstorm` (§M54): a bug that
 * needs a person and a reboot is a bug nobody can work on. */
static int storm_n, storm_mode;   /* mode: 1 = paint a wallpaper, 2 = flip the theme */
static void gui_sessionstorm_main(void) {
    const struct user_account* ua = user_by_name(st_a);
    const struct user_account* ub = user_by_name(st_b);
    if (!ua || !ub) { kprintf("sessionstorm: no such account\n"); return; }
    for (int i = 0; i < storm_n; i++) {
        const char* who = (i % 3 == 0) ? st_a : (i % 3 == 1) ? st_b : NULL;
        int uid = (i % 3 == 0) ? ua->uid : (i % 3 == 1) ? ub->uid : -1;
        gui_session_restart_as(who);
        if (st_wait_uid(uid, 1)) {
            kprintf("sessionstorm: round %d - session for %s never came up\n",
                    i, who ? who : "(signed out)");
            return;
        }
        kprintf("sessionstorm: round %d up (%s)\n", i, who ? who : "signed out");
        /* What `sessiontest` does mid-session, from a task that is not the
         * compositor — each separately switchable, to bisect the corruption. */
        if (storm_mode & 1) config_apply("gui.wallpaper", (i & 1) ? "solid:FF0000" : "solid:00FF00");
        if (storm_mode & 2) config_apply("gui.theme", (i & 1) ? "light" : "dark");
        task_msleep(150 + (uint32_t)((i * 137) % 900));
    }
    kprintf("sessionstorm: %d rounds done\n", storm_n);
}

static void cmd_sessionstorm(const char* args) {
    int i = 0;
    while (args && *args == ' ') args++;
    while (args && *args && *args != ' ' && i < (int)sizeof st_a - 1) st_a[i++] = *args++;
    st_a[i] = 0; i = 0;
    while (args && *args == ' ') args++;
    while (args && *args && *args != ' ' && i < (int)sizeof st_b - 1) st_b[i++] = *args++;
    st_b[i] = 0;
    while (args && *args == ' ') args++;
    storm_n = 0;
    while (args && *args >= '0' && *args <= '9') storm_n = storm_n * 10 + (*args++ - '0');
    while (args && *args == ' ') args++;
    storm_mode = 0;
    while (args && *args >= '0' && *args <= '9') storm_mode = storm_mode * 10 + (*args++ - '0');
    if (!st_a[0] || !st_b[0] || storm_n <= 0) {
        kprintf("usage: sessionstorm <user-a> <user-b> <rounds>\n"); return;
    }
    if (!task_spawn_detached("sessionstorm", gui_sessionstorm_main))
        kprintf("sessionstorm: cannot spawn\n");
}
SHELL_CMD(sessionstorm) = { "sessionstorm", "<user-a> <user-b> <rounds> [mode]", 0,
                            SHELL_G_TEST, cmd_sessionstorm, SHELL_P_ADMIN };

static void gui_stop_main(void) {
    gui_teardown();
    task_exit();
}

int gui_autostart(void) {
    /* The text shell is spawned FIRST by both callers and stays behind the
     * desktop: the GUI only suppresses the console, so Start → Exit GUI (or
     * `gui stop`) lands on a shell that has been running all along. */
    const char* ga = config_get("gui.autostart", "1");
    if (!ga || !(ga[0] == '1' || ga[0] == 'y' || ga[0] == 't' ||
                 ga[0] == 'Y' || ga[0] == 'T')) return 0;
    if (gui_start() != 0) {
        kprintf("gui: autostart failed - staying on the text console\n");
        return -1;
    }
    return 1;
}

int gui_stop(void) {
    if (!gui_active) return -1;
    /* Called directly (the `gui stop` command, from a shell task): that task is
     * not in the session, so it may do the teardown itself. */
    return gui_teardown();
}

int gui_start(void) {
    if (gui_active) return 0;

    /* §M81 — AUTOLOGIN IS DECIDED BEFORE ANYTHING IS SPAWNED, and HERE rather
     * than in `gui_autostart`, because that is the BOOT path only — the `gui`
     * command calls this directly, and the first version put the check in the
     * other one, so typing `gui` produced a picker on a machine configured to
     * open itself.  *A decision that exists on one of two ways in is a
     * behaviour nobody can predict from the setting.*
     *
     * A session's identity can only be adopted by a task with no children
     * (§M32), so it cannot be decided later: by the time this function returns
     * there is a desktop and a compositor and both already ARE somebody. */
    if (!in_session_leader) {
        const char* al = users_autologin_account();
        if (al) {
            kprintf("gui: autologin to '%s' (it has no password)\n", al);
            gui_session_restart_as(al);
            return 0;
        }
    }

    /* §M46 — whether the X button on a client-managed (package) window
     * force-kills a wedged client instead of only requesting a cooperative
     * close (default on: the chrome must keep working when the app is frozen).
     * A future refinement makes this a per-package policy; for now it is a
     * single global gate so it is configurable rather than hard-coded. */
    {
        const char* v = config_get("gui.close_forces_kill", "1");
        close_forces_kill = (v && (v[0]=='1'||v[0]=='y'||v[0]=='t'||v[0]=='Y'));
        /* The UNATTENDED backstop only — the primary escalation is the user's
         * second click on the X (see close_force_now).  Generous on purpose: it
         * must never pre-empt a client that is merely slow to shut down. */
        const char* g = config_get("gui.close_grace_ms", "10000");
        unsigned ms = 0;
        for (const char* c = g; c && *c >= '0' && *c <= '9'; c++)
            ms = ms * 10u + (unsigned)(*c - '0');
        if (ms) close_grace_ms = ms;
    }

    /* §M61 — a CONFIRMED resolution survives a reboot.  `gui.mode` is written
     * only by the OK button (or `mode confirm`), so a mode that was never
     * confirmed cannot come back and lock the user out at the next boot; and
     * it is applied HERE, before any surface is sized, because everything
     * below this line is derived from the screen's dimensions.
     *
     * A refusal is silent-but-logged rather than fatal: the display may be a
     * different one than the machine had when the mode was saved, and a GUI
     * that will not start because of a remembered preference is worse than one
     * that starts at the boot resolution. */
    {
        const char* m = config_get("gui.mode", "");
        int w = 0, h = 0;
        const char* p = m;
        while (*p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
        if (*p == 'x' || *p == 'X') {
            p++;
            while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
        }
        if (w >= 320 && h >= 200) {
            if (fb_mode_set((uint32_t)w, (uint32_t)h, 32) == 0)
                kprintf("gui: mode %dx%d (from gui.mode)\n", w, h);
            else
                klog(KLOG_WARN, "gui", "gui.mode=%s refused by the display - "
                                       "using the boot mode\n", m);
        }
    }

    if (gfx_fb_surface(&fbsurf) != 0) {
        kprintf("gui: no 32-bpp framebuffer - GUI unavailable\n");
        return -1;
    }
    if (gfx_surface_init(&backsurf, fbsurf.w, fbsurf.h) != 0 ||
        gfx_surface_init(&wallsurf, fbsurf.w, fbsurf.h) != 0) {
        kprintf("gui: backbuffer OOM\n");
        return -1;
    }

    /* M22.6 — try to stand up the Bochs-VBE double buffer.  On success both
     * scanout buffers alias the same geometry as fbsurf; the first compose
     * is a full-frame damage (gui_damage_all below), so both buffers get a
     * complete paint within the first two frames — the buffer-age-2 present
     * is consistent from then on.  Any failure leaves flip_ok==0 and the
     * compositor keeps its single-buffer path. */
    {
        volatile uint32_t *b0, *b1;
        if (fb_flip_init(&b0, &b1) == 0) {
            for (int i = 0; i < 2; i++) {
                flipbuf[i] = fbsurf;                /* copy w/h/stride */
                flipbuf[i].owns_px = 0;
            }
            flipbuf[0].px = (uint32_t*)(uintptr_t)b0;
            flipbuf[1].px = (uint32_t*)(uintptr_t)b1;
            flip_front = 0;
            /* §M76.4 — `gui.page_flip = 0` forces the single-buffer path.
             *
             * An INSTRUMENT, not a setting anybody should need.  It was built
             * to test one hypothesis about a reported flicker — that §M22.6's
             * buffer-age-2 double buffer was letting two buffers disagree —
             * and **THAT HYPOTHESIS WAS FALSIFIED BY IT**: single-buffered, the
             * flicker was unchanged.  The cause was §M76.5's cleared clip in
             * `table_draw`, one layer up.
             *
             * The key stays because the question recurs and because a negative
             * answer took one boot: *a flicker that needs no input is either
             * about presentation or about painting, and with the flip off the
             * first cannot be true.*  Ruling a layer out is worth as much as
             * finding one, and this is the switch that does it. */
            if (config_get_long("gui.page_flip", 1)) {
                flip_ok = 1;
                kprintf("gui: page-flip present enabled (Bochs-VBE double buffer)\n");
            } else {
                kprintf("gui: page flip DISABLED by gui.page_flip - "
                        "single-buffer present (may shear)\n");
            }
        } else {
            kprintf("gui: no page flip - single-buffer present (may shear)\n");
        }
    }

    shell = pick_shell();
    if (shell && shell->init) shell->init(fbsurf.w, fbsurf.h);
    work_h = fbsurf.h -
             ((shell && shell->bottom_reserve) ? shell->bottom_reserve() : 0);

    gmax_cols = fbsurf.w / cp_cell_w();
    gmax_rows = fbsurf.h / cp_cell_h();

    /* §M60 — put the shipped default image on the filesystem before the first
     * render, so a fresh boot shows a picture rather than a gradient.  Once. */
    wallpaper_provision();
    paint_wallpaper();

    /* M22.7-B — panel surface: screen-addressed, but only the bottom strip
     * (taskbar reserve + popup headroom) is backed (see PANEL_POPUP_MAX).
     * The desktop task renders chrome into it; only the taskbar strip + open
     * popup are ever composited from it.  If it OOMs we run without a panel. */
    spin_lock_init(&panel_lock);
    {
        int reserve = fbsurf.h - work_h;                    /* bottom_reserve */
        int strip_h = reserve + PANEL_POPUP_MAX;
        if (strip_h > fbsurf.h) strip_h = fbsurf.h;
        panel_strip_top = fbsurf.h - strip_h;
        panel_buf = (uint32_t*)kmalloc((size_t)fbsurf.w * strip_h * 4);
        if (panel_buf) {
            panelsurf.w      = fbsurf.w;                    /* pretend full-screen */
            panelsurf.h      = fbsurf.h;
            panelsurf.stride = fbsurf.w;
            panelsurf.px     = panel_buf -
                               (size_t)panel_strip_top * fbsurf.w;
            panelsurf.owns_px = 0;                          /* panel_buf is the base */
            gfx_set_clip(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h);
            gfx_fill(&panelsurf, 0, panel_strip_top, fbsurf.w, strip_h,
                     COL_WALL_BOT);
            panel_ready = 1;
        } else {
            kprintf("gui: panel surface OOM - taskbar disabled\n");
        }
    }

    spin_lock_init(&state_lock);
    spin_lock_init(&damage_lock);
    mx = fbsurf.w / 2;
    my = fbsurf.h / 2;

    gui_active = 1;
    vc_screen_suppress(1);

    /* Input hooks first, so the compositor + desktop see events immediately. */
    mouse_set_listener(gui_mouse);
    vc_set_kbd_hook(gui_kbd_hook);
    vc_set_raw_kbd_hook(gui_raw_key);       /* Alt-Tab */
    mouse_set_wheel_listener(gui_wheel);    /* §M61 follow-up — scrolling */
    task_set_change_hook(gui_task_change_hook);  /* M22.4: taskman refresh */

    /* M22.7 — the GUI is its own SESSION.  The `desktop` task is the session
     * root; the compositor and the two starter shells hang UNDER it, not
     * under whatever shell happened to run `gui`.  So spawn the desktop
     * first, record its pid, and parent the rest to it — a kill_tree of the
     * desktop then cleanly closes the whole GUI session.  (The boot shell
     * remains only the launcher that started the session.) */
    if (panel_ready) {
        /* The desktop is the GUI SESSION ROOT — detached (parented to init), so
         * it survives whatever transient task ran `gui` / gui_start (the boot
         * worker, a shell): a launcher's exit must not take the whole session
         * down.  The session still tears down top-down — a kill_tree / crash of
         * the desktop takes the compositor + every app with it (they hang UNDER
         * the desktop), which is the "parent dies → children die" rule. */
        struct task* dt = task_spawn_detached("desktop", desktop_main);
        if (dt) desktop_pid = dt->pid;
        else    kprintf("gui: desktop task spawn failed - taskbar static\n");
    }

    int sess = desktop_pid > 0 ? desktop_pid : -1;   /* session parent, or caller */
    if (!task_spawn_under("compositor", gui_compositor_main, sess)) {
        kprintf("gui: FATAL - compositor spawn failed\n");
        vc_set_kbd_hook(NULL);
        vc_set_raw_kbd_hook(NULL);
        task_set_change_hook(NULL);
        mouse_set_listener(NULL);
        vc_screen_suppress(0);
        gui_active = 0;
        return -1;
    }

    /* M22.7 — no auto-started shells: the GUI comes up as a clean desktop
     * (wallpaper + taskbar).  The user opens a terminal when they want one,
     * from Start → "New Shell" (session) or "Detached Shell".  Zero windows
     * is a supported state — focus is simply NULL until one is opened. */
    gui_damage_all();

    kprintf("gui: up - %dx%d, %d windows, shell '%s', %d apps registered\n",
            fbsurf.w, fbsurf.h, zcount,
            shell ? shell->name : "none", gui_app_count());

    /* §M32 stage 10 — GATE THE DESKTOP, if the machine has anyone to ask.
     *
     * AFTER the compositor exists, because the lock is a WINDOW and there is
     * nothing to draw it before that; and after `gui: up`, so a machine that
     * fails to raise it still reports a working desktop rather than an
     * ambiguous silence.
     *
     * It is off by default (`gui.login`).  A gate that defaults ON would be a
     * gate this project's own harness cannot get past (§4.74), and the first
     * casualty of turning it on by default would be every existing GUI test —
     * §M46's argument for `hardlock` being reachable but not standard, applied
     * to the thing that stands between a person and their desktop. */
    /* §M81 — …UNLESS THIS SESSION WAS JUST AUTHENTICATED.  The sign-in rebuilds
     * the session (see gui_session_main), so without this the new desktop would
     * immediately ask for the password that had just opened it, forever.
     *
     * ONE-SHOT, and cleared whether or not the lock was going to be raised: a
     * flag that survives its own session is a machine that skips the NEXT
     * sign-in too, which is the only failure mode here worth worrying about. */
    if (skip_lock_once) {
        skip_lock_once = 0;
        kprintf("gui: session already authenticated - not locking\n");
    /* §M81 — ASK WHEN THERE IS SOMEBODY TO ASK.
     *
     * `gui.login` used to be the only way in, defaulting OFF because a gate
     * that defaults ON is one this project's own harness cannot get past
     * (§4.74).  That argument still holds for a machine where nobody has chosen
     * a secret — every GUI test in the tree boots with root carrying the
     * SHIPPED DEFAULT — and it stops holding the moment a real password exists:
     * a desktop that opens itself on a machine with accounts is the same
     * authentication theatre this milestone just removed, one layer up.
     *
     * So the gate is `users_anyone_can_sign_in()` OR the explicit key, and the
     * harness is unaffected because a default password is not a chosen one. */
    } else if (users_anyone_can_sign_in() || config_get_long("gui.login", 0)) {
        if (gui_lock_raise() != 0)
            kprintf("gui: login was requested and could not be raised - the "
                    "desktop is UNLOCKED\n");
        /* And the instrument — on its OWN TASK, the same shape as autorun
         * below.  It waits for the lock's fields to be laid out, and waiting
         * here would block the bring-up that produces them: *an instrument
         * that blocks the thing it is measuring measures a machine that does
         * not exist.*  Read from a key because by the next line the lock owns
         * the keyboard and nothing can be typed at it (§4.74). */
        const char* lt = config_get("gui.locktest", "");
        if (lt && lt[0]) {
            gui_locktest_creds = lt;
            task_spawn_detached("gui-locktest", gui_locktest_main);
        }
    }

    /* §M76 — RUN ONE COMMAND NOW THAT THE DESKTOP EXISTS.
     *
     * §4.74: this project's harness cannot type once a GUI window holds focus,
     * and on aarch64 it is worse — with `gui.autostart` on, the desktop is up
     * before the first command lands, so NOTHING could be typed at all.
     * Measured while trying to run §M76's own test client there: neither `ps`
     * nor `uidemo` reached the shell.
     *
     * *A test that cannot be started is not a test*, which is why this tree
     * already grew `conf open` (to reach a settings panel), `gui.wheeltest` (to
     * reach the wheel router) and `gui.ui_dump` (to reach a layout).  Each
     * solved one instance of the same wall.  This is the general one: a key set
     * BEFORE the GUI takes over, naming a command to dispatch once it is up.
     *
     * ONCE, and on a DETACHED TASK.  Once, because a key that re-fires would
     * relaunch on every mode change; detached, because the command may block
     * (it usually spawns a window) and this runs on the task that brought the
     * desktop up. */
    const char* autorun = config_get("gui.autorun", "");
    if (autorun && autorun[0]) {
        static char ar[96];
        int i = 0;
        while (autorun[i] && i < (int)sizeof ar - 1) { ar[i] = autorun[i]; i++; }
        ar[i] = 0;
        kprintf("gui: autorun '%s'\n", ar);
        gui_autorun_cmd = ar;
        task_spawn_detached("gui-autorun", gui_autorun_main);
    }
    return 0;
}
