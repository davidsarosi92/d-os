/* =============================================================================
 * accountspanel.c — user accounts in the Control Panel (§M32).
 *
 * A SETTINGS_PANEL(), not a GUI_APP, for §M63's reason: the Start menu has
 * twelve slots with eleven taken, so a new tool has to be a ROW.
 *
 * -----------------------------------------------------------------------------
 * THE PANEL DOES NOT ENFORCE ANYTHING, AND THAT IS THE DESIGN.
 *
 * Every action here calls the same `users.h` function the shell commands call,
 * and those read `cred_current()` themselves — deliberately, so that a gate
 * cannot be HANDED its answer (users.h says so in capitals).  A panel that
 * re-implemented "may this person do that" would be a second copy of the rule,
 * and the two would eventually disagree about something that is a security
 * statement.
 *
 * So what this file adds is not enforcement but HONESTY ABOUT ENFORCEMENT: the
 * buttons that cannot work for the current identity and the selected row are
 * drawn DISABLED, and the detail line says why.
 *
 * **THAT IS A DEPARTURE FROM devicepanel.c's RULE AND IT IS DELIBERATE.**  That
 * file argues — correctly — that *a control whose only state is disabled is a
 * control nobody learns the meaning of*, and offers its buttons live so the
 * refusal can explain itself.  The difference here is that disabled-ness
 * DEPENDS on the selection and on who is looking: the same button is live on
 * your own row and dead on somebody else's, so the greying is not a permanent
 * mystery but the answer itself — *this row is not yours.*  A user who selects
 * their own account sees the same panel come alive.
 *
 * -----------------------------------------------------------------------------
 * WHAT A NON-ADMIN CAN DO, which is the whole point of the request:
 *
 *   - see every account (who exists is not a secret, and an owner column
 *     elsewhere in the system already shows the names)
 *   - change THEIR OWN password
 *   - nothing else: no creating, no deleting, no promoting, not even their own
 *     elevation mode, because that one is a privilege setting and §M32 keeps
 *     those in the account database where only an administrator writes them.
 *
 * An administrator can do the rest, except that anything touching an ADMIN
 * account needs root — including deleting one, and including root itself,
 * which cannot be deleted by anybody.
 * ============================================================================= */

#include "gui.h"
#include "locale.h"   /* §M87 — translated fragments */
#include "widget.h"
#include "ui.h"
#include "itemview.h"
#include "settings.h"
#include "users.h"
#include "cred.h"
#include "icons.h"
#include "console_plate.h"
#include "config.h"
#include "dialog.h"
#include "printf.h"
#include "shellcmd.h"
#include "console.h"
#include "task.h"
#include <stddef.h>

/* ---------------------------------------------------------------- */
/* Model.                                                            */
/* ---------------------------------------------------------------- */

enum { AC_NAME = 0, AC_UID, AC_ROLE, AC_LOGIN, AC_HOME, AC__COUNT };

static int ac_put(char* out, int cap, int n, const char* s) {
    for (; s && *s && n < cap - 1; s++) out[n++] = *s;
    out[n] = 0;
    return n;
}
static int ac_put_int(char* out, int cap, int n, int v) {
    char t[12]; int m = 0;
    if (v < 0) { if (n < cap - 1) out[n++] = '-'; v = -v; }
    if (v == 0) t[m++] = '0';
    while (v > 0 && m < 12) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m > 0 && n < cap - 1) out[n++] = t[--m];
    out[n] = 0;
    return n;
}

static int ac_count(void* ctx) { (void)ctx; return user_count(); }

static int ac_get(void* ctx, int i, struct item_entry* out) {
    (void)ctx;
    const struct user_account* u = user_at(i);
    if (!u) return -1;
    out->label = u->name;
    out->sub   = NULL;
    out->icon  = ICON_APP;
    /* An account nobody can log into is drawn dim — the same distinction the
     * `users` command makes with a NO-PASSWORD column.  *An administrator with
     * no password is a name, not an administrator*, and the row should not
     * look like a working account. */
    out->dim   = u->has_password ? 0 : 1;
    return 0;
}

static int ac_columns(void* ctx) { (void)ctx; return AC__COUNT; }

static const char* ac_col_title(void* ctx, int c) {
    (void)ctx;
    switch (c) {
        case AC_NAME:  return "Account";
        case AC_UID:   return "UID";
        case AC_ROLE:  return "Role";
        case AC_LOGIN: return "Sign-in";
        case AC_HOME:  return "Home";
    }
    return "";
}

static int ac_col_weight(void* ctx, int c) {
    (void)ctx;
    switch (c) {
        case AC_NAME:  return 3;
        case AC_UID:   return 1;
        case AC_ROLE:  return 2;
        case AC_LOGIN: return 2;
        case AC_HOME:  return 4;
    }
    return 1;
}

static int ac_col_style(void* ctx, int c) {
    (void)ctx;
    if (c == AC_UID) return ICOL_RIGHT | ICOL_MONO;
    return 0;
}

static int ac_cell(void* ctx, int i, int c, char* out, int cap) {
    (void)ctx;
    const struct user_account* u = user_at(i);
    int n = 0;
    if (!u) { out[0] = 0; return 0; }
    switch (c) {
        case AC_NAME:  n = ac_put(out, cap, 0, u->name); break;
        case AC_UID:   n = ac_put_int(out, cap, 0, u->uid); break;
        case AC_ROLE:
            /* "Administrator" and "the protected account" are different facts
             * and root is both; saying only the first would hide the one rule
             * a reader most needs to know before pressing Delete. */
            if (u->uid == CRED_UID_ROOT)        n = ac_put(out, cap, 0, lstr("Administrator (protected)"));
            else if (user_is_admin_uid(u->uid)) n = ac_put(out, cap, 0, lstr("Administrator"));
            else                                n = ac_put(out, cap, 0, lstr("Standard"));
            break;
        case AC_LOGIN:
            n = ac_put(out, cap, 0, lstr(u->has_password ? "yes" : "no password"));
            break;
        case AC_HOME:  n = ac_put(out, cap, 0, user_home(u)); break;
    }
    (void)n;
    return 0;
}

static void ac_activate(void* ctx, int i);
static void ac_refresh(void);
static void ac_update_controls(void);
static void accounts_panel_open(void);

static const struct item_model ac_model = {
    .count = ac_count, .get = ac_get, .activate = ac_activate, .ctx = NULL,
    .columns = ac_columns, .col_title = ac_col_title,
    .col_weight = ac_col_weight, .cell = ac_cell, .col_style = ac_col_style,
};

/* ---------------------------------------------------------------- */
/* Window state.                                                     */
/* ---------------------------------------------------------------- */

static struct gui_window* ac_win;
static struct w_itemview* ac_view;
static struct w_label*    ac_detail;

/* §M81 — AN ANSWER SURVIVES THE REFRESH THAT FOLLOWS IT.
 *
 * `ac_update_controls` rewrites the detail line from the SELECTION ("david -
 * uid 1000 ..."), and every action here ends with `ac_refresh()`.  So a handler
 * that reported its outcome into that label had it overwritten before a single
 * frame was drawn: the password really was set, and the panel said NOTHING.
 * Reported from use in exactly those words — *"I type it, press OK, everything
 * disappears and it writes nothing"* — and it is why a working change read as a
 * broken one.
 *
 * THIS FILE'S NEIGHBOUR ALREADY HAD THIS BUG.  §M69: *"the Task Manager's
 * button messages were written and then overwritten by `tm_refresh` IN THE SAME
 * CALL, so the answer to a button press was never on screen for a single
 * frame."*  Same shape, same file family, found again by a user rather than by
 * the fix that was written for it.
 *
 * So the answer is not "remember to order the two calls correctly" — that is
 * the convention that just failed.  A pending ANSWER is state: `ac_update_controls`
 * shows it instead of the selection line, once, and clears it.  A handler that
 * sets one cannot have it eaten, whatever it calls afterwards. */
static char ac_answer[160];

static void ac_say(const char* msg) {
    int i = 0;
    for (; msg && msg[i] && i < (int)sizeof ac_answer - 1; i++) ac_answer[i] = msg[i];
    ac_answer[i] = 0;
}

static struct w_button*   ac_btn_new;
static struct w_button*   ac_btn_pw;
static struct w_button*   ac_btn_admin;
static struct w_button*   ac_btn_del;
static struct w_button*   ac_btn_auto;
/* The selection is remembered BY NAME, not by index — §M66's slot-table lesson
 * applies here too: creating or deleting an account renumbers the list, and an
 * index captured before an action can name a different row after it. */
static char ac_sel_name[USER_NAME_MAX + 1];

static int s_same(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const struct user_account* ac_selected(void) {
    return ac_sel_name[0] ? user_by_name(ac_sel_name) : NULL;
}

/* ---------------------------------------------------------------- */
/* What the current identity may do to the selected row.             */
/*                                                                   */
/* This MIRRORS users.h's rules for the sake of the buttons; it does */
/* not replace them.  The call is still gated where the decision is  */
/* made, so a mistake here makes a button look wrong, not a rule go  */
/* missing.                                                          */
/* ---------------------------------------------------------------- */

static int me_is_admin(void) { return cred_is_admin(cred_current()); }
static int me_is_root(void)  {
    const struct cred* c = cred_current();
    /* A SYSTEM context counts while the machine has no administrator yet —
     * the installer console (users.h).  Mirrored from actor_is_root so the
     * buttons agree with the gate on a fresh machine. */
    if (c->owner != TASK_OWNER_USER) return users_needs_setup();
    return cred_is_root(c);
}
static int is_me(const struct user_account* u) {
    const struct cred* c = cred_current();
    return u && c->owner == TASK_OWNER_USER && cred_uid(c) == u->uid;
}

/* WHAT IS GREYED, AND IT IS DELIBERATELY LESS THAN WHAT IS REFUSED.
 *
 * Corrected from use: *"the Set password button is shown disabled for root, yet
 * I can still click it — it is right that it works, but it should not look
 * disabled.  Only a non-admin should be blocked, and only on accounts that are
 * not their own."*
 *
 * The first version mirrored ALL of users.h's rules, including "only root may
 * change an administrator's password".  That was wrong twice: it greyed a
 * control for an administrator who is entitled to TRY (and whose attempt the
 * gate would explain), and it made the panel a second copy of a rule that is
 * allowed to change.
 *
 * So the greying now answers ONE question — *is this row yours to touch at
 * all* — and everything finer is left to users.h, which refuses with a reason.
 * That is also devicepanel.c's rule back in force: a live control whose refusal
 * explains itself teaches more than a dead one. */
static int may_set_password(const struct user_account* u) {
    if (!u) return 0;
    if (me_is_admin()) return 1;        /* entitled to try; the gate decides */
    return is_me(u);                    /* a standard user: their own row    */
}
static int may_delete(const struct user_account* u) {
    if (!u) return 0;
    /* root is the one exception kept here, because it is not a permission
     * question: NOBODY may delete it, including root, so the control can never
     * come alive and a live one would be a promise the gate always breaks. */
    if (u->uid == CRED_UID_ROOT) return 0;
    return me_is_admin();
}
static int may_toggle_admin(const struct user_account* u) {
    if (!u) return 0;
    if (u->uid == CRED_UID_ROOT) return 0;   /* protected, for the same reason */
    return me_is_admin();
}

/* ---------------------------------------------------------------- */
/* A one-field prompt (a name, or a password).                       */
/*                                                                   */
/* Small enough to live here rather than becoming a toolkit control:  */
/* dialog.c answers a QUESTION with buttons and has no input field,   */
/* and growing it one would be a general feature built for one use.   */
/* ---------------------------------------------------------------- */

static struct gui_window*  pr_win;
static struct w_textinput* pr_field;
static void (*pr_done)(const char* value);
static char pr_title[48];
static int  pr_secret;

static void pr_close(void) {
    if (pr_win) {
        struct gui_window* w = pr_win;
        pr_win = NULL; pr_field = NULL;
        gui_window_close(w);
    }
}

static void pr_submit(struct w_textinput* t, void* ctx) {
    (void)ctx;
    if (!t) return;
    char v[64];
    int i = 0;
    for (; t->buf[i] && i < (int)sizeof v - 1; i++) v[i] = t->buf[i];
    v[i] = 0;
    /* Scrub before the widget is freed: a freed heap block holding a password
     * is a password on the heap (lockscreen.c makes the same point). */
    for (int k = 0; k < (int)sizeof t->buf; k++) t->buf[k] = 0;
    t->len = 0;
    void (*fn)(const char*) = pr_done;
    pr_close();
    if (fn) fn(v);
    for (int k = 0; k < (int)sizeof v; k++) v[k] = 0;
}

static void pr_ok(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    if (pr_field) pr_submit(pr_field, NULL);
}
static void pr_cancel(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    pr_done = NULL;
    pr_close();
}

static void pr_layout(struct gui_window* win) {
    gui_window_clear_widgets(win);
    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    const int pad = cp_px(14);
    w_label_create(win, pad, pad, cw - 2 * pad, pr_title);
    pr_field = w_textinput_create(win, pad, pad + cp_row_h() + cp_px(4),
                                  cw - 2 * pad, NULL);
    if (pr_field) {
        w_textinput_set_secret(pr_field, pr_secret);
        pr_field->on_submit = pr_submit;
    }
    int y = ch - cp_btn_h() - pad;
    struct w_button* ok = w_button_create(win, 0, 0, 0, 0, "OK", pr_ok, NULL);
    struct w_button* ca = w_button_create(win, 0, 0, 0, 0, "Cancel", pr_cancel, NULL);
    if (ca) { w_button_autosize(ca, 0, y); ca->base.x = cw - pad - ca->base.w; }
    if (ok) {
        w_button_autosize(ok, 0, y);
        ok->base.x = ca ? ca->base.x - cp_px(8) - ok->base.w : cw - pad - ok->base.w;
    }
    gui_window_focus_widget(win, (struct widget*)pr_field);
}

static void pr_closed(struct gui_window* w) {
    (void)w;                            /* pr_win is the spec's slot (§M81) */
    pr_field = NULL;
}

static void pr_build(void) {
    /* The second file that wrote `-1,-1` meaning "centre" on the same day.  Now
     * it says what it means — see gui.h's GUI_PLACE_*. */
    gui_app_open(&(struct gui_app_spec){
        .title = "Account",
        .content_w = cp_px(340), .content_h = cp_px(130),
        .place = GUI_PLACE_DIALOG, .modal = 1,
        .layout = pr_layout, .on_close = pr_closed, .slot = &pr_win,
    });
}

static void ac_prompt(const char* title, int secret, void (*done)(const char*)) {
    if (pr_win) return;                          /* one at a time */
    int i = 0;
    for (; title[i] && i < (int)sizeof pr_title - 1; i++) pr_title[i] = title[i];
    pr_title[i] = 0;
    pr_secret = secret;
    pr_done   = done;
    /* Through the queue, for §M61's reason: a window built on a task with no
     * app-host loop never lays out, and the button callback that got us here
     * runs on one that does — but routing both the same way means this cannot
     * become wrong if the callback ever moves. */
    gui_queue_open(pr_build);
}

/* ---------------------------------------------------------------- */
/* Actions.  Each one calls users.h and lets the GATE decide.        */
/* ---------------------------------------------------------------- */

static void done_new_name(const char* name) {
    if (!name || !name[0]) return;
    /* Not an admin by default, and there is no checkbox offering it: promoting
     * is root's and has its own button, so a "make this an admin" tick here
     * would be a second route to the same decision with a different gate in
     * front of it. */
    user_add(name, 0);
    ac_refresh();
}

static void done_password(const char* pw) {
    const struct user_account* u = ac_selected();
    if (!u) return;
    /* AN EMPTY FIELD IS NOT A REQUEST TO REMOVE THE PASSWORD.
     *
     * `user_set_password(name, NULL)` DISABLES sign-in, and the first version
     * reached it by pressing OK on an empty box — so a keystroke that failed to
     * land (or a mis-click) silently turned an account into one that refuses
     * every password, and the explanation went to the console, which the GUI
     * suppresses (§4.79).  Reported from use as *"I set the password in the
     * Control Panel and it always says wrong password"*, which is exactly what
     * that looks like from a chair.
     *
     * The shell keeps the destructive form because it is EXPLICIT there
     * (`passwd <name> -`).  A blank field is an accident, not a sentence. */
    if (!pw || !pw[0]) {
        if (ac_detail)
            ac_say("No password typed - nothing was changed. "
               "(To disable sign-in, use `passwd <name> -` at a shell.)");
        kprintf("accounts: done_password received 0 characters\n");
        if (ac_win) gui_window_request_redraw(ac_win);
        return;
    }
    /* CHECK THE RETURN VALUE.  The first version called this and then reported
     * success unconditionally — so a refusal (which users.c prints to the
     * console, and the GUI suppresses) came out of this panel as *"Password set
     * for david"*.  **A panel that reports the outcome it hoped for is worse
     * than one that reports nothing**, because the user then goes looking for
     * the fault everywhere except where it is. */
    /* §M81 — HOW MANY CHARACTERS ACTUALLY ARRIVED, on screen and on the wire.
     *
     * The chain from here down is PROVEN (`accttest`: field -> submit ->
     * user_set_password -> user_check_password OK), and a report of *"the GUI
     * change password still does not work"* therefore has to be about what
     * reaches this function.  The mask cannot answer it — it is fixed width by
     * design, so one character and three look identical — and the detail line
     * is the only surface the user can read while the GUI suppresses the
     * console.  The COUNT, never the content. */
    int plen = 0;
    while (pw[plen]) plen++;
    kprintf("accounts: done_password received %d character(s) for '%s'\n",
            plen, u->name);
    int rc = user_set_password(u->name, pw);
    {
        static char m[140];
        int n;
        if (rc == 0) {
            n = ac_put(m, sizeof m, 0, lstr("Password set for "));
            n = ac_put(m, sizeof m, n, u->name);
            n = ac_put(m, sizeof m, n, lstr(" ("));
            n = ac_put_int(m, sizeof m, n, plen);
            n = ac_put(m, sizeof m, n, lstr(" chars, fp "));
            /* §M81 — THE FINGERPRINT, and the panel VERIFIES ITS OWN WORK.
             *
             * When both ends of the chain report ONE character and the sign-in
             * is still refused, a length has said all it can.  Two things are
             * left, and this line separates them: `fp` is 8 irreversible bits
             * of what was typed, so a lock screen showing a DIFFERENT fp proves
             * the keystrokes differ rather than the hashing — and
             * `keyboard.layout` is a per-user setting, which applies inside a
             * session and not at the lock screen.
             *
             * And `ok`/`MISMATCH` is the panel checking the record it just
             * wrote, immediately, through the same function the lock screen
             * calls.  *A panel that reports the outcome it hoped for is worse
             * than one that reports nothing* — this reports the outcome it
             * MEASURED, one line after causing it. */
            {
                char fp[3];
                user_secret_fingerprint(pw, fp);
                n = ac_put(m, sizeof m, n, fp);
                n = ac_put(m, sizeof m, n, lstr(user_check_password(u->name, pw) == 0
                                               ? ", verified ok).  They can sign in now."
                                               : ", VERIFY FAILED - the record did not take)."));
            }
        } else {
            /* §M81 — THE REASON THAT ACTUALLY FIRED, and WHO WE ARE.
             *
             * This said "(An administrator's password needs root.)" — a GUESS
             * at one of four possible refusals, and wrong for the other three.
             * `users_last_refusal()` is the gate's own words.
             *
             * And the identity, because the report that found this came with
             * exactly the right question attached: *"the GUI comes up - which
             * user is this?  It should be root but it never asked for a
             * password."*  It is a SYSTEM session — `gui.login` is off by
             * default — which counts as an administrator and NOT as root once
             * any account has a chosen secret.  A panel that refuses an action
             * without saying who it was acting as leaves the user to deduce the
             * one thing the machine already knows. */
            char who[40];
            n = ac_put(m, sizeof m, 0, lstr("NOT changed - "));
            n = ac_put(m, sizeof m, n, users_last_refusal());
            n = ac_put(m, sizeof m, n, lstr(".  Acting as "));
            n = ac_put(m, sizeof m, n,
                       cred_owner_name(cred_current(), who, sizeof who));
            n = ac_put(m, sizeof m, n, lstr("."));
        }
        (void)n;
        ac_say(m);
    }
    ac_refresh();
}

static void act_new(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    ac_prompt("New account name", 0, done_new_name);
}

static void act_password(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    const struct user_account* u = ac_selected();
    if (!u) return;
    char t[48];
    int n = ac_put(t, sizeof t, 0, lstr("New password for "));
    ac_put(t, sizeof t, n, u->name);
    ac_prompt(t, 1, done_password);
}

static void act_admin(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    const struct user_account* u = ac_selected();
    if (!u) return;
    /* One button, both directions — devicepanel.c's rule, and it applies here
     * because the pair would always have one member greyed. */
    user_set_admin(u->name, user_is_admin_uid(u->uid) ? 0 : 1);
    ac_refresh();
}

static void del_answered(int answer, void* ctx) {
    (void)ctx;
    if (answer != 1) return;                      /* cancelled, or dismissed */
    const struct user_account* u = ac_selected();
    if (!u) return;
    user_del(u->name);
    ac_sel_name[0] = 0;
    ac_refresh();
}

/* §M81 — SIGN THIS ACCOUNT IN WITHOUT ASKING.
 *
 * First asked for with the condition *only a passwordless account may be
 * chosen*; reversed from use on 2026-09-29, because the account a person wants
 * opened for them is their own, which has a password.  The gate that remains
 * — an administrator decides — is config.c's machine-scope check, so it holds
 * for `setconf` as much as for this button.
 *
 * ONE BUTTON, BOTH DIRECTIONS, like the admin toggle beside it: a pair would
 * always have one member greyed, and a control whose only state is disabled is
 * one nobody learns. */
static void act_autologin(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    const struct user_account* u = ac_selected();
    if (!u) return;
    const char* cur = config_get("users.autologin", "");
    int already = cur && cur[0] && s_same(cur, u->name);

    config_apply("users.autologin", already ? "" : u->name);

    static char m[160];
    int n;
    if (already) {
        n = ac_put(m, sizeof m, 0, lstr("Auto sign-in is off.  The picker will ask."));
    } else {
        n = ac_put(m, sizeof m, 0, lstr("Signing in to "));
        n = ac_put(m, sizeof m, n, u->name);
        n = ac_put(m, sizeof m, n, lstr(" automatically at power-on.  Its "
                                   "password still guards the lock screen and "
                                   "sign-in after Sign out."));
    }
    (void)n;
    ac_say(m);
    ac_refresh();
}

static void act_delete(struct w_button* b, void* ctx) {
    (void)b; (void)ctx;
    const struct user_account* u = ac_selected();
    if (!u) return;
    /* ASK.  Deleting an account is the one irreversible operation in this
     * panel — the uid is retired for the life of the machine and the files it
     * owned belong to nobody afterwards — so it goes through §M69's dialog
     * rather than acting on a single click.  The file manager's recursive
     * delete made the same move, for the same reason. */
    static char body[176];
    int n = ac_put(body, sizeof body, 0, lstr("Delete '"));
    n = ac_put(body, sizeof body, n, u->name);
    n = ac_put(body, sizeof body, n, lstr("'?\nIts uid is retired permanently, and "
                                     "the files it owns\nwill then belong to no "
                                     "account."));
    (void)n;
    struct gui_dialog_req req = { 0 };
    req.title       = "Delete account";
    req.body        = body;
    req.ok_text     = "Delete";
    req.cancel_text = "Cancel";
    req.on_answer   = del_answered;
    gui_dialog_open(&req);
}

/* ---------------------------------------------------------------- */
/* The buttons say what this identity may do to THIS row.            */
/* ---------------------------------------------------------------- */

static void ac_update_controls(void) {
    const struct user_account* u = ac_selected();

    if (ac_btn_new)   ac_btn_new->base.disabled   = !me_is_admin();
    if (ac_btn_pw)    ac_btn_pw->base.disabled    = !may_set_password(u);
    if (ac_btn_admin) ac_btn_admin->base.disabled = !may_toggle_admin(u);
    if (ac_btn_del)   ac_btn_del->base.disabled   = !may_delete(u);
    /* A person account, and only an admin may decide it — the same facts
     * `users_autologin_account()` and config.c's machine-scope gate re-check.
     * (A password no longer rules it out: 2026-09-29, users.c.) */
    if (ac_btn_auto)  ac_btn_auto->base.disabled =
        !(u && u->type == USER_TYPE_PERSON && me_is_admin());

    if (!ac_detail) return;
    /* A pending answer wins over the selection description, exactly once. */
    if (ac_answer[0]) {
        w_label_set(ac_detail, ac_answer);
        ac_answer[0] = 0;
        return;                 /* the buttons were updated above */
    }
    static char msg[224];
    int n = 0;
    if (!u) {
        n = ac_put(msg, sizeof msg, 0, lstr("Select an account."));
        if (!me_is_admin())
            n = ac_put(msg, sizeof msg, n, lstr("  You are signed in as a standard user, so you may "
                       "change your own password and nothing else."));
    } else {
        n = ac_put(msg, sizeof msg, 0, u->name);
        n = ac_put(msg, sizeof msg, n, lstr(" - uid "));
        n = ac_put_int(msg, sizeof msg, n, u->uid);
        if (!u->has_password)
            n = ac_put(msg, sizeof msg, n, lstr(", cannot sign in (no password)"));
        /* WHY a control is dead, in the words a person needs.  A greyed button
         * with no explanation is the failure devicepanel.c warns about; the
         * difference here is that the reason changes with the selection, so it
         * belongs on the line that also changes. */
        if (u->uid == CRED_UID_ROOT)
            n = ac_put(msg, sizeof msg, n, lstr(".  root is protected: it cannot be deleted or demoted "
                       "by anyone, including root."));
        else if (user_is_admin_uid(u->uid) && !me_is_root())
            n = ac_put(msg, sizeof msg, n, lstr(".  This is an administrator - only root may change or "
                       "remove one."));
        else if (!me_is_admin() && !is_me(u))
            n = ac_put(msg, sizeof msg, n, lstr(".  Not your account: a standard user may change only "
                       "their own password."));
    }
    (void)n;
    w_label_set(ac_detail, msg);
}

static void ac_refresh(void) {
    if (!ac_win) return;
    if (ac_view) w_itemview_refresh(ac_view);
    ac_update_controls();
    gui_window_request_redraw(ac_win);
}

static void ac_activate(void* ctx, int i) {
    (void)ctx;
    const struct user_account* u = user_at(i);
    if (!u) return;
    int k = 0;
    for (; u->name[k] && k < (int)sizeof ac_sel_name - 1; k++) ac_sel_name[k] = u->name[k];
    ac_sel_name[k] = 0;
    ac_refresh();
}

static void ac_on_select(struct w_itemview* iv, int idx, void* c) {
    (void)iv; (void)c;
    ac_activate(NULL, idx);
}

static void ac_on_close(struct gui_window* w) {
    (void)w;                            /* ac_win is the spec's slot (§M81) */
    ac_view = NULL; ac_detail = NULL;
    ac_btn_new = ac_btn_pw = ac_btn_admin = ac_btn_del = NULL;
    ac_sel_name[0] = 0;
}

/* §M81 — THIS PANEL IS COMPOSED, NOT HAND-PLACED, and it is the first window
 * with a real table to be.
 *
 * What it looked like before is in the history: a `w_itemview_create` at
 * computed pixels, a label under it at `ch - bar - cp_row_h()`, and four
 * buttons walked across with a cursor and `w_button_autosize` — the last of
 * them then pushed to the right edge by writing `base.x` directly.  Every one
 * of those numbers is the app doing the layout engine's job, and widget.h has
 * said since §M69 that `w_button_autosize` is an INTERIM with `ui_build` and a
 * `UI_ROW` as the end state.
 *
 * TWO THINGS MADE IT POSSIBLE, both from §M81.  A container is a WIDGET, so the
 * tree is one hierarchy; and `w_itemview` is a registered CLASS ("view"), so a
 * table can appear in a spec at all — it had ops and no class, which is why
 * every window holding one had to place it by hand.
 *
 * THE MODEL IS ATTACHED AFTERWARDS, on purpose: a spec is DATA so that a ring-3
 * client can send the same array (§M65), and a model is a pointer.  The spec
 * carries the LAYOUT's name, which is the same string `accounts.view` holds, so
 * table-versus-list stays a config change.
 *
 * `UI_ALIGN_END` on the Delete button's own row is what replaces writing
 * `base.x = cw - pad - w`: Delete sits apart from the others because *the gap is
 * the only thing standing between a curious click and something that cannot be
 * undone* — and that intent is now expressed rather than computed. */
enum {
    AC_ID_VIEW = 1, AC_ID_DETAIL, AC_ID_ROW, AC_ID_SPACER,
    AC_ID_NEW, AC_ID_PW, AC_ID_ADMIN, AC_ID_AUTO, AC_ID_DEL,
};

static void ac_event(struct gui_window* win, int id, int type, int value,
                     void* ctx) {
    (void)win; (void)value; (void)ctx;
    if (type == UI_EV_ACTIVATE && id == AC_ID_VIEW) { ac_on_select(ac_view, value, NULL); return; }
    if (type != UI_EV_CLICK) return;
    switch (id) {
    case AC_ID_NEW:   act_new(NULL, NULL);      break;
    case AC_ID_PW:    act_password(NULL, NULL); break;
    case AC_ID_ADMIN: act_admin(NULL, NULL);    break;
    case AC_ID_AUTO:  act_autologin(NULL, NULL); break;
    case AC_ID_DEL:   act_delete(NULL, NULL);   break;
    default: break;
    }
}

static void ac_layout(struct gui_window* win) {
    gui_window_clear_widgets(win);
    ac_view = NULL; ac_detail = NULL;
    ac_btn_new = ac_btn_pw = ac_btn_admin = ac_btn_del = NULL;

    static const struct ui_spec spec[] = {
        { .id = AC_ID_VIEW,   .cls = "view",  .weight = 1,
          .text = "table",    .flags = UI_FILL_W | UI_FOCUSABLE },
        { .id = AC_ID_DETAIL, .cls = "label", .text = "Select an account.",
          .flags = UI_FILL_W },
        { .id = AC_ID_ROW,    .cls = "box",   .flags = UI_ROW | UI_FILL_W },
        { .id = AC_ID_NEW,    .parent = AC_ID_ROW, .cls = "button", .text = "New" },
        { .id = AC_ID_PW,     .parent = AC_ID_ROW, .cls = "button", .text = "Set password" },
        { .id = AC_ID_ADMIN,  .parent = AC_ID_ROW, .cls = "button", .text = "Toggle admin" },
        { .id = AC_ID_AUTO,   .parent = AC_ID_ROW, .cls = "button", .text = "Auto sign-in" },
        /* A weighted empty box is the gap: it takes the slack, so Delete ends
         * up hard against the right edge whatever the other three measure. */
        { .id = AC_ID_SPACER, .parent = AC_ID_ROW, .cls = "box", .weight = 1 },
        { .id = AC_ID_DEL,    .parent = AC_ID_ROW, .cls = "button", .text = "Delete" },
    };
    ui_build(win, spec, (int)(sizeof spec / sizeof spec[0]), ac_event, NULL);

    ac_view   = (struct w_itemview*)ui_by_id(win, AC_ID_VIEW);
    ac_detail = (struct w_label*)   ui_by_id(win, AC_ID_DETAIL);
    ac_btn_new   = (struct w_button*)ui_by_id(win, AC_ID_NEW);
    ac_btn_pw    = (struct w_button*)ui_by_id(win, AC_ID_PW);
    ac_btn_admin = (struct w_button*)ui_by_id(win, AC_ID_ADMIN);
    ac_btn_auto  = (struct w_button*)ui_by_id(win, AC_ID_AUTO);
    ac_btn_del   = (struct w_button*)ui_by_id(win, AC_ID_DEL);

    if (ac_view) {
        w_itemview_set_model(&ac_view->base, &ac_model, NULL);
        ac_view->on_select = ac_on_select;
    }
    ac_update_controls();
}

static void accounts_panel_open(void) {
    struct gui_app_spec sp = {
        .title = "User accounts",
        .content_w = cp_px(720), .content_h = cp_px(360),
        .layout = ac_layout, .on_close = ac_on_close, .slot = &ac_win,
    };
    gui_app_open(&sp);
}

/* Designated fields: a positional initializer silently re-binds when the
 * struct grows (it did — `live` was appended; §M58's scar). */
SETTINGS_PANEL(accounts) = {
    .name    = "User accounts",
    .summary = "who may use this machine, and what each of them may do",
    .icon    = ICON_USERS,          /* was ICON_APP, the generic window tile */
    .open    = accounts_panel_open,
};

/* ===========================================================================
 * `accttest <user>:<password>` — drive the panel's REAL password chain.
 *
 * The lock screen got `gui.locktest` and this path never got its equivalent,
 * which is why three rounds of reading looked in the wrong file: the shell's
 * `passwd` works, the lock screen's input works, and the ONE untested link was
 * the prompt's submit -> done_password.  A path with no instrument is a path
 * whose failure is indistinguishable from every other path's.
 *
 * It fills the prompt's field and calls `pr_submit` — the same function the
 * Enter key calls — rather than calling `done_password` directly: a test that
 * skipped the submit would pass over exactly the code under suspicion.
 * ========================================================================= */

/* ON ITS OWN TASK.  With the GUI up, a kprintf from the SHELL's task goes to
 * the suppressed console and reaches nobody (§4.79) — the first run of this
 * test produced complete silence for exactly that reason, which is the same
 * trap `gui.locktest` had to be moved out of. */
static char at_user[64], at_pass[64];

static void accttest_main(void) {
    const char* u = at_user;
    const char* p = at_pass;
    /* Select the row the way a click does. */
    int k = 0;
    for (; u[k] && k < (int)sizeof ac_sel_name - 1; k++) ac_sel_name[k] = u[k];
    ac_sel_name[k] = 0;
    const struct user_account* sel = ac_selected();
    if (!sel) { kprintf("accttest: no account '%s'\n", u); return; }
    kprintf("accttest: selected '%s' (uid %d)\n", sel->name, sel->uid);

    /* Raise the prompt exactly as the button does, wait for its field, fill it
     * and submit through the REAL path. */
    ac_prompt("test", 1, done_password);
    int waited = 0;
    for (; !pr_field && waited < 60000; waited++) task_yield();
    if (!pr_field) { console_write("accttest: the prompt's field never appeared\n"); return; }

    w_textinput_set(pr_field, p);
    /* The LENGTH, because it is the one thing the mask deliberately hides: it
     * is fixed-width so it cannot leak how long a password is, which also means
     * it cannot distinguish one character from three.  Printed only by this
     * test, never by the field. */
    kprintf("accttest: field now holds %d character(s)\n", pr_field->len);
    pr_submit(pr_field, NULL);

    /* And CHECK it, here, against the same function login uses — so the answer
     * is about the stored hash rather than about what we think we wrote. */
    kprintf("accttest: user_check_password('%s', typed) -> %s\n", u,
            user_check_password(u, p) == 0 ? "OK" : "MISMATCH");
}

static void cmd_accttest(const char* args) {
    int i = 0;
    while (*args == ' ') args++;
    while (*args && *args != ':' && i < (int)sizeof at_user - 1) at_user[i++] = *args++;
    at_user[i] = 0;
    if (*args == ':') args++;
    i = 0;
    while (*args && *args != ' ' && i < (int)sizeof at_pass - 1) at_pass[i++] = *args++;
    at_pass[i] = 0;
    if (!at_user[0]) {
        console_write("accttest: usage: accttest <user>:<password>\n");
        return;
    }
    task_spawn_detached("accttest", accttest_main);
}

SHELL_CMD(accttest) = { "accttest", "<user>:<password>", NULL,
                        SHELL_G_TEST, cmd_accttest, SHELL_P_ADMIN };
