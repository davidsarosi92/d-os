/* =============================================================================
 * lockscreen.c — the GUI authentication surface (§M32 stage 10).
 *
 * ONE SURFACE, TWO USES.  Locking a running desktop and gating it at startup
 * are the same question — *is the person at this keyboard allowed to be here* —
 * so they are one window rather than two that would drift apart.  The only
 * difference is what happens on success: a lock simply goes away, while a
 * login also opens a session.
 *
 * -----------------------------------------------------------------------------
 * WHY THIS IS MODALITY AND NOT A WIDGET.
 *
 * §M69 built compositor modality for the dialog, and it is exactly what a lock
 * needs: `topmost_at` answers "the modal, or nothing", the left-press path
 * swallows anything outside it BEFORE the taskbar's refusal and the desktop
 * fallthrough (without which a click on the WALLPAPER would still launch
 * shortcuts behind the lock), Alt-Tab is refused, and the 45 % backdrop is what
 * makes the other three legible.
 *
 * **WITH ONE DELIBERATE DIFFERENCE: ESCAPE MUST NOT DISMISS THIS ONE.**  §M69
 * traps Esc in the COMPOSITOR and routes it to `want_close`, which is right for
 * a dialog — it means cancel — and would be a hole here, because cancelling a
 * lock is precisely what an unauthorised person would do.
 *
 * The answer is NOT a new compositor knob.  `gui_window_set_key_hook` returns
 * void, so a hook cannot consume a key, and suppressing the close box would
 * mean a `title_btn_count` setter — one more control on the compositor for one
 * caller.  Instead every route out (the X button, Escape, a stray close) ends
 * in `on_close`, which RAISES THE LOCK AGAIN while it is still locked.  *There
 * is no way past this window* is the same statement, made with the mechanism
 * that already exists and covering every route rather than only the button.
 *
 * -----------------------------------------------------------------------------
 * WHAT THIS DOES **NOT** DO, and it is the honest limit of this stage.
 *
 * The desktop task, the compositor and the app hosts remain SYSTEM-owned.  This
 * gates ACCESS to a running desktop; it does not make the desktop belong to the
 * person who unlocked it.  Doing that means the desktop task, the app hosts and
 * the window list all become per-session — the same restructuring as running
 * two sessions at once, and the same one §M32's per-user settings named as its
 * own limit.  *A lock screen that claimed to be a full multi-user desktop would
 * be the isolation theatre §M33 refuses by name*, so it says what it is.
 *
 * -----------------------------------------------------------------------------
 * AND IT IS DRIVABLE, WHICH IS WHY IT EXISTS AT ALL.
 *
 * §4.74: this project's harness cannot type once a GUI window holds focus —
 * which is exactly where a password field is.  That is why §M32 stopped at
 * stage 9 the first time.  `gui.locktest` drives the REAL submit path with
 * supplied credentials, the same shape as §M69's `gui.wheeltest` and
 * `gui.ui_scrolltest`: everything above the keystroke is measured, and the
 * keystroke delivery is named as the part this harness cannot reach.
 * ============================================================================= */

#include "gui.h"
#include "widget.h"
#include "itemview.h"
#include "icons.h"
#include "users.h"
#include "cred.h"
#include "console_plate.h"
#include "printf.h"
#include "console.h"
#include "klog.h"
#include "config.h"
#include "settings.h"
#include "shellcmd.h"
#include "task.h"
#include <stddef.h>

struct lock_state {
    struct gui_window*   win;
    struct w_itemview*   picker;     /* the accounts that can sign in       */
    struct w_textinput*  pass;
    struct w_label*      status;
    int                  unlocked;
};

/* ---------------------------------------------------------------------------
 * THE PICKER: the accounts that can actually sign in.
 *
 * A text field asked the person at a locked machine to remember a name; a list
 * asks them to recognise one, which is the easier question and the one every
 * other sign-in screen asks.  It is also the honest set: an account with no
 * password CANNOT sign in (users.h), so offering it would be a row that
 * refuses every password typed into it.
 *
 * Names are not a secret — the Control Panel lists them, and so does every
 * owner column in the system — so showing them costs nothing that is not
 * already public.  The PASSWORD is the secret, and it is the field below. */
static int lk_signable(const struct user_account* u) {
    return u && u->used && u->type == USER_TYPE_PERSON && u->has_password;
}

static int lk_count(void* ctx) {
    (void)ctx;
    int n = 0;
    for (int i = 0; i < user_count(); i++)
        if (lk_signable(user_at(i))) n++;
    return n;
}

static const struct user_account* lk_at(int idx) {
    int n = 0;
    for (int i = 0; i < user_count(); i++) {
        const struct user_account* u = user_at(i);
        if (!lk_signable(u)) continue;
        if (n++ == idx) return u;
    }
    return NULL;
}

static int lk_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    const struct user_account* u = lk_at(i);
    if (!u) return -1;
    out->label = u->name;
    out->sub   = NULL;
    out->icon  = ICON_APP;
    out->dim   = 0;
    return 0;
}

static void lk_activate(void* ctx, int i);

static const struct item_model lk_model = {
    .count = lk_count, .get = lk_get, .activate = lk_activate, .ctx = NULL,
};

static struct lock_state g_lock;

/* WHO SIGNED IN AT THIS SCREEN.
 *
 * The desktop task is SYSTEM-owned (see the header), so `cred_current()` on
 * the panel or the Start menu answers "system" and cannot name a person.  That
 * is the honest answer for the TASK and the wrong one for the chrome, which is
 * reporting who is at the keyboard.  The lock screen is the only place that
 * knows, so it records it — and it is a NAME rather than a cred, precisely so
 * nothing mistakes it for an identity that grants anything. */
static char g_session_user[USER_NAME_MAX + 1];

const char* gui_session_user(void) {
    return g_session_user[0] ? g_session_user : NULL;
}

void gui_session_clear(void) { g_session_user[0] = 0; }

/* Raising re-enters from on_close (a lock that was dismissed comes back), so
 * the two refer to each other and one of them has to be declared first. */
int gui_lock_raise(void);
int gui_lock_active(void);

/* The one place that turns two strings into a verdict.  Both the typed submit
 * and the instrument call THIS, so a test cannot pass through a path the user
 * never takes (§M64's `shortcut check` argument: a command that reassembles the
 * same facts by a second route would agree with itself and not with the
 * screen). */
/* WHO IS SELECTED HAS ONE ANSWER, AND IT IS THE PICKER'S.
 *
 * The first version kept a `chosen` name beside the list and set it from
 * on_select — two sources of truth for one fact, and they disagreed on the
 * first driven run: the instrument chose `alice`, the item view's own initial
 * selection then fired on_select for row 0, and the machine authenticated
 * `root` while the log said it was submitting alice.  *A cached copy of a
 * widget's state is a second answer waiting to be the wrong one*, which is the
 * same shape as §M69's widget storing a resolved string instead of a key.
 *
 * So there is no copy: the submit reads `picker->sel`. */
static void lk_choose(int idx) {
    if (!g_lock.picker) return;
    g_lock.picker->sel = idx;
    /* The password field takes focus the moment a name is chosen: choosing is
     * the first half of one gesture, and making the person click again to type
     * is a step that exists only because the window has two widgets. */
    if (g_lock.win && g_lock.pass)
        gui_window_focus_widget(g_lock.win, (struct widget*)g_lock.pass);
}

static void lk_activate(void* ctx, int i) { (void)ctx; lk_choose(i); }

static void lk_on_select(struct w_itemview* iv, int idx, void* c) {
    (void)iv; (void)c;
    lk_choose(idx);
}

static int lock_try(const char* user, const char* pass) {
    if (user_check_password(user, pass) != 0) {
        /* NAME THE ACCOUNT IT TRIED.
         *
         * The generic "incorrect user name or password" is the right message
         * where a login prompt would otherwise ENUMERATE accounts — and that
         * argument does not apply to a picker that is already listing every one
         * of them on screen.  What it cost instead was a debugging session in
         * which "I chose david and typed his password" and "it submitted a
         * different row" produced identical evidence.
         *
         * It also reports whether anything was typed, because "wrong password"
         * and "no password reached the field" are different problems and the
         * mask (fixed-width by design) cannot tell them apart. */
        if (g_lock.status) {
            static char m[96];
            int n = 0;
            const char* a = "Incorrect password for '";
            for (int i = 0; a[i] && n < 94; i++) m[n++] = a[i];
            for (int i = 0; user[i] && n < 92; i++) m[n++] = user[i];
            m[n++] = '\'';
            if (!pass || !pass[0]) {
                const char* b = " (nothing was typed)";
                for (int i = 0; b[i] && n < 94; i++) m[n++] = b[i];
            }
            m[n++] = '.'; m[n] = 0;
            w_label_set(g_lock.status, m);
        }
        kprintf("lock: authentication FAILED for '%s'\n", user);
        /* NO SLEEP HERE.  The shell's login delays a failed attempt on the
         * SHELL's task, which is the thing waiting anyway.  This runs on the
         * lock window's APP-HOST task, and sleeping it stops that window's
         * event loop — so a few wrong passwords made the desktop appear to
         * seize up.  Reported from use as *"after several attempts the GUI
         * starts to lag"*, and it was not lag: it was the window not running.
         *
         * The cost of losing it is small and named: this surface is reached by
         * a person at the keyboard, and PBKDF2 already puts ~0.5 s between
         * attempts (measured, users.h).  A real rate limit belongs in
         * `user_check_password`, where every caller inherits it — which is
         * where it should have gone in the first place. */
        return -1;
    }
    kprintf("lock: authenticated '%s'\n", user);
    {
        int i = 0;
        for (; user[i] && i < (int)sizeof g_session_user - 1; i++)
            g_session_user[i] = user[i];
        g_session_user[i] = 0;
    }
    g_lock.unlocked = 1;
    return 0;
}

static void lock_submit(struct w_textinput* t, void* ctx) {
    (void)t; (void)ctx;
    if (!g_lock.picker || !g_lock.pass) {
        /* SAYS SO.  This returned silently in the first version, and the
         * instrument then reported "still locked" for a CORRECT password —
         * a passing-looking failure whose cause was three layers away.  The
         * widgets are built in the layout hook, which runs later on the host
         * task (§M61's "a window that never lays out looks exactly like an app
         * that ignored the event"), so a submit can genuinely arrive first. */
        kprintf("lock: submit before the fields exist - ignored\n");
        return;
    }
    const struct user_account* who = lk_at(g_lock.picker->sel);
    if (!who) {
        /* Nothing chosen.  SAYS SO rather than authenticating against an empty
         * name, which would fail for a reason that has nothing to do with the
         * password the person just typed. */
        if (g_lock.status) w_label_set(g_lock.status, "Choose an account first.");
        if (g_lock.win) gui_window_request_redraw(g_lock.win);
        return;
    }
    if (lock_try(who->name, g_lock.pass->buf) == 0) {
        /* Scrub the password out of the widget before the window goes: the
         * struct is freed, not zeroed, and a freed heap block holding a
         * password is a password on the heap. */
        for (int i = 0; i < (int)sizeof g_lock.pass->buf; i++) g_lock.pass->buf[i] = 0;
        g_lock.pass->len = 0;
        gui_window_close(g_lock.win);
        g_lock.win = NULL;
    } else {
        for (int i = 0; i < (int)sizeof g_lock.pass->buf; i++) g_lock.pass->buf[i] = 0;
        g_lock.pass->len = 0;
        gui_window_request_redraw(g_lock.win);
    }
}

int gui_lock_active(void) { return g_lock.win != NULL && !g_lock.unlocked; }

static void lock_layout(struct gui_window* win);

/* Closing is REFUSED by re-raising, which needs no new compositor API.
 *
 * §M69 already reduces a modal's chrome to its close box; suppressing that box
 * too would mean a `title_btn_count` setter, and one more knob on the
 * compositor for one caller.  Coming back is the same statement — *there is no
 * way past this window* — made with the mechanism that already exists, and it
 * covers every route into destroy_window rather than only the button. */
static void lock_closed(struct gui_window* w) {
    (void)w;
    g_lock.win = NULL;
    g_lock.picker = NULL;
    g_lock.pass = NULL;
    g_lock.status = NULL;
    if (!g_lock.unlocked) {
        kprintf("lock: closed while still locked - raising it again\n");
        gui_lock_raise();
    }
}

/* The window is BUILT here, and this function must run on a task that has an
 * app-host loop — see gui_lock_raise below, which is what guarantees it. */
static void lock_build(void) {
    if (g_lock.win) return;

    /* CENTRED EXPLICITLY.  `window_alloc` stores x and y verbatim — there is no
     * "-1 means you pick" convention anywhere in the WM, and passing -1 put
     * this window at literally (-1,-1), i.e. off the top-left corner.  It had
     * therefore NEVER been visible, and the two-point probe did not catch it:
     * `modal visible to the compositor (z-index 0 of 1)` reports the Z-ORDER,
     * not the pixels, and I read it as the latter for several rounds.
     *
     * *A probe that answers a neighbouring question is the one most likely to
     * be believed* — §M79's own lesson, met again. */
    /* §M81 — the intent, not the arithmetic.  The comment above is kept because
     * it is the receipt: `-1,-1` was written here meaning "centre", and there
     * is no such convention in the WM.  GUI_PLACE_DIALOG is that intent, and
     * the wrong answer can no longer be spelled. */
    struct gui_window* win = gui_app_open(&(struct gui_app_spec){
        .title = "Locked",
        .content_w = cp_px(400), .content_h = cp_px(300),
        .place = GUI_PLACE_DIALOG,
        .layout = lock_layout, .on_close = lock_closed,
    });
    if (!win) return;
    g_lock.win = win;
    g_lock.unlocked = 0;

    if (gui_window_set_modal(win, 1) != 0) {
        /* Somebody else holds the single modal claim.  A non-modal look-alike
         * would be a picture of a lock with a live desktop behind it, which is
         * worse than refusing. */
        kprintf("lock: another modal window holds the screen - refused\n");
        gui_window_close(win);
        g_lock.win = NULL;
        return;
    }
}

/* §M61's MECHANISM, and this is the bug it was built for.
 *
 * `gui_app_window_create` binds the window to `task_current()`, so a window
 * made on a task with no app-host loop **never lays out and never ticks** — its
 * widgets are built by the layout hook, which nothing ever calls.  `gui_start`
 * is exactly such a task, and the symptom was precise and misleading: the
 * window existed, modality was claimed AND painted, and the password field did
 * not exist.  The instrument said so in as many words — *the window was created
 * but never laid out* — which is the sentence §M61 already had in the tree.
 *
 * `gui_queue_open` hands the construction to the compositor, which is the one
 * task that has the loop. */
int gui_lock_raise(void) {
    if (g_lock.win) return 0;                     /* already up */
    if (users_needs_setup()) {
        /* Nothing to authenticate against.  Locking here would leave a machine
         * nobody can get into — the same stranding §M32's account rules refuse
         * three other ways. */
        kprintf("lock: no account can log in yet - refusing to lock\n");
        return -1;
    }
    gui_queue_open(lock_build);
    return 0;
}

/* Widgets are built in the LAYOUT hook, which is where gui.h says an app window
 * must build them: the content size is not established until it runs. */
static void lock_layout(struct gui_window* win) {
    if (g_lock.picker) return;                    /* build once (ui.h's rule) */
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    const int pad = cp_px(18);
    int y = cp_px(14);

    w_label_create(win, pad, y, cw - 2 * pad,
                   "This screen is locked.  Choose an account and sign in.");
    y += cp_row_h() + cp_px(6);

    int list_h = ch - y - 2 * cp_row_h() - cp_px(26);
    if (list_h < cp_row_h() * 2) list_h = cp_row_h() * 2;
    g_lock.picker = w_itemview_create(win, pad, y, cw - 2 * pad, list_h,
                                      &lk_model, "list", NULL);
    if (g_lock.picker) g_lock.picker->on_select = lk_on_select;
    y += list_h + cp_px(8);

    w_label_create(win, pad, y, cp_px(90), "Password");
    g_lock.pass = w_textinput_create(win, pad + cp_px(96), y,
                                     cw - pad - cp_px(96) - pad, NULL);
    w_textinput_set_secret(g_lock.pass, 1);
    if (g_lock.pass) g_lock.pass->on_submit = lock_submit;
    y += cp_row_h() + cp_px(8);
    g_lock.status = w_label_create(win, pad, y, cw - 2 * pad, "");

    /* Pre-choose the default account (`users.default_user`) when it can sign
     * in, so the common case is one password away.  A NAME is not a secret;
     * pre-filling it saves the one thing somebody at a locked machine should
     * not have to guess. */
    {
        const char* d = users_default_name();
        for (int i = 0; i < lk_count(NULL); i++) {
            const struct user_account* u = lk_at(i);
            int same = 1;
            for (int k = 0; ; k++) {
                if (u->name[k] != d[k]) { same = 0; break; }
                if (!d[k]) break;
            }
            if (same) { lk_choose(i); break; }
        }
    }
    gui_window_focus_widget(win, (struct widget*)g_lock.pass);
}

/* ---------------------------------------------------------------------------
 * The instrument.  See the header: without it this whole surface would be the
 * one claim in §M32 that was asserted rather than measured.
 * ------------------------------------------------------------------------- */

void gui_lock_test(const char* creds) {
    char u[64], p[64];
    int i = 0;
    while (*creds == ' ') creds++;
    while (*creds && *creds != ':' && i < (int)sizeof u - 1) u[i++] = *creds++;
    u[i] = 0;
    if (*creds == ':') creds++;
    i = 0;
    while (*creds && *creds != ' ' && i < (int)sizeof p - 1) p[i++] = *creds++;
    p[i] = 0;

    /* The raise is QUEUED (see gui_lock_raise), so neither the window nor its
     * fields exist when this task starts.  One wait covers both. */
    /* WAIT FOR THE WINDOW TO HAVE LAID ITSELF OUT.  The lock is raised on the
     * caller's task and its widgets are built by the layout hook on the host
     * task, so "the window exists" and "the fields exist" are different facts
     * arriving at different times — §M76's autorun sleeps for the same reason.
     * Bounded, and it REPORTS a timeout rather than submitting into nothing. */
    /* task_yield, NOT task_msleep, and that is measured rather than stylistic:
     * a msleep on this task did not return at all — the loop never completed,
     * never timed out, and printed nothing, which reads exactly like a task
     * that was never spawned.  Bounded so a lock that genuinely never lays out
     * reports it instead of hanging the instrument. */
    int waited = 0;
    for (; !g_lock.pass && waited < 60000; waited++)
        task_yield();
    if (!g_lock.pass) {
        kprintf("locktest: the fields never appeared - the window was "
                "created but never laid out\n");
        return;
    }
    kprintf("locktest: submitting '%s' through the REAL path\n", u);
    /* Fill the widgets and call the SAME submit the Enter key calls — not
     * lock_try directly.  A test that skipped the widgets would pass while the
     * field was disconnected, which is the defect most likely to exist. */
    /* Choose through the PICKER rather than writing the name into a field —
     * the field is gone, and a test that set state the user interface no longer
     * offers would pass over a broken picker. */
    {
        int found = -1;
        for (int i = 0; i < lk_count(NULL); i++) {
            const struct user_account* a = lk_at(i);
            int same = 1;
            for (int k = 0; ; k++) {
                if (a->name[k] != u[k]) { same = 0; break; }
                if (!u[k]) break;
            }
            if (same) { found = i; break; }
        }
        if (found < 0) {
            kprintf("locktest: '%s' is not an account that can sign in\n", u);
            return;
        }
        lk_choose(found);
    }
    w_textinput_set(g_lock.pass, p);
    lock_submit(g_lock.pass, NULL);
    kprintf("locktest: result - %s\n",
            g_lock.unlocked ? "UNLOCKED" : "still locked");
}

/* THE INSTRUMENT IS A CONFIG KEY, NOT ONLY A COMMAND, and that is the whole
 * reason this stage could be finished at all.
 *
 * The first version offered `lock alice:pw1` and nothing else — and the
 * measurement said, immediately, that it cannot work: raising the lock takes
 * the keyboard (which is the feature), so the shell that would type the test
 * is exactly the shell that can no longer be typed into.  §4.74, demonstrated
 * by the thing it was blocking.
 *
 * `gui.locktest` is read by gui_start AFTER the lock is up, the same shape as
 * §M69's `gui.wheeltest` and `gui.ui_dump`: a key that can be set BEFORE the
 * GUI takes over is the only kind of switch that can reach a state in which
 * typing is impossible.  The command form stays for a human at a second pane. */
static void cmd_lock(const char* args) {
    if (args && args[0]) { gui_lock_test(args); return; }
    if (!gui_is_active()) { console_write("lock: the desktop is not running\n"); return; }
    if (gui_lock_raise() == 0) console_write("lock: screen locked\n");
}

SHELL_CMD(lock) = { "lock", "[user:password]",
                    "lock the desktop (with credentials: drive the real submit)",
                    SHELL_G_GUI, cmd_lock, SHELL_P_ANY };

CONFIG_KEY(ck_gui_locktest) = {
    .key = "gui.locktest", .group = "System", .type = CFG_STRING, .def = "",
    .help = "diagnostic: drive the lock screen's submit with user:password",
    .scope = CFG_SCOPE_MACHINE,
};

CONFIG_KEY(ck_gui_login) = {
    .key = "gui.login", .group = "System", .type = CFG_BOOL, .def = "0",
    .help = "require authentication before the desktop can be used",
    .scope = CFG_SCOPE_MACHINE,
};
