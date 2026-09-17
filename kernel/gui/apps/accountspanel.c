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
#include "widget.h"
#include "itemview.h"
#include "settings.h"
#include "users.h"
#include "cred.h"
#include "icons.h"
#include "console_plate.h"
#include "config.h"
#include "dialog.h"
#include "printf.h"
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
            if (u->uid == CRED_UID_ROOT)        n = ac_put(out, cap, 0, "Administrator (protected)");
            else if (user_is_admin_uid(u->uid)) n = ac_put(out, cap, 0, "Administrator");
            else                                n = ac_put(out, cap, 0, "Standard");
            break;
        case AC_LOGIN:
            n = ac_put(out, cap, 0, u->has_password ? "yes" : "no password");
            break;
        case AC_HOME:  n = ac_put(out, cap, 0, u->home); break;
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
static struct w_button*   ac_btn_new;
static struct w_button*   ac_btn_pw;
static struct w_button*   ac_btn_admin;
static struct w_button*   ac_btn_del;
/* The selection is remembered BY NAME, not by index — §M66's slot-table lesson
 * applies here too: creating or deleting an account renumbers the list, and an
 * index captured before an action can name a different row after it. */
static char ac_sel_name[USER_NAME_MAX + 1];

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

static int may_set_password(const struct user_account* u) {
    if (!u) return 0;
    if (is_me(u)) return 1;                       /* your own, always */
    if (user_is_admin_uid(u->uid)) return me_is_root();
    return me_is_admin();
}
static int may_delete(const struct user_account* u) {
    if (!u) return 0;
    if (u->uid == CRED_UID_ROOT) return 0;        /* protected, for everybody */
    return user_is_admin_uid(u->uid) ? me_is_root() : me_is_admin();
}
static int may_toggle_admin(const struct user_account* u) {
    if (!u) return 0;
    if (u->uid == CRED_UID_ROOT) return 0;
    return me_is_root();
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
    (void)w;
    pr_win = NULL; pr_field = NULL;
}

static void pr_build(void) {
    int ow, oh;
    gui_window_outer_for_content(cp_px(340), cp_px(130), &ow, &oh);
    pr_win = gui_app_window_create("Account", -1, -1, ow, oh, pr_layout, NULL);
    if (!pr_win) return;
    gui_window_set_on_close(pr_win, pr_closed);
    gui_window_set_modal(pr_win, 1);
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
    user_set_password(u->name, (pw && pw[0]) ? pw : NULL);
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
    int n = ac_put(t, sizeof t, 0, "New password for ");
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
    int n = ac_put(body, sizeof body, 0, "Delete '");
    n = ac_put(body, sizeof body, n, u->name);
    n = ac_put(body, sizeof body, n, "'?\nIts uid is retired permanently, and "
                                     "the files it owns\nwill then belong to no "
                                     "account.");
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

    if (!ac_detail) return;
    static char msg[224];
    int n = 0;
    if (!u) {
        n = ac_put(msg, sizeof msg, 0, "Select an account.");
        if (!me_is_admin())
            n = ac_put(msg, sizeof msg, n,
                       "  You are signed in as a standard user, so you may "
                       "change your own password and nothing else.");
    } else {
        n = ac_put(msg, sizeof msg, 0, u->name);
        n = ac_put(msg, sizeof msg, n, " — uid ");
        n = ac_put_int(msg, sizeof msg, n, u->uid);
        if (!u->has_password)
            n = ac_put(msg, sizeof msg, n, ", cannot sign in (no password)");
        /* WHY a control is dead, in the words a person needs.  A greyed button
         * with no explanation is the failure devicepanel.c warns about; the
         * difference here is that the reason changes with the selection, so it
         * belongs on the line that also changes. */
        if (u->uid == CRED_UID_ROOT)
            n = ac_put(msg, sizeof msg, n,
                       ".  root is protected: it cannot be deleted or demoted "
                       "by anyone, including root.");
        else if (user_is_admin_uid(u->uid) && !me_is_root())
            n = ac_put(msg, sizeof msg, n,
                       ".  This is an administrator — only root may change or "
                       "remove one.");
        else if (!me_is_admin() && !is_me(u))
            n = ac_put(msg, sizeof msg, n,
                       ".  Not your account: a standard user may change only "
                       "their own password.");
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
    (void)w;
    ac_win = NULL; ac_view = NULL; ac_detail = NULL;
    ac_btn_new = ac_btn_pw = ac_btn_admin = ac_btn_del = NULL;
    ac_sel_name[0] = 0;
}

static void ac_layout(struct gui_window* win) {
    gui_window_clear_widgets(win);
    ac_view = NULL; ac_detail = NULL;
    ac_btn_new = ac_btn_pw = ac_btn_admin = ac_btn_del = NULL;

    int cw, ch;
    gui_window_content_size(win, &cw, &ch);
    const int pad = cp_px(6), gap = cp_px(6);
    const int bar = cp_btn_h() + 2 * gap;

    ac_view = w_itemview_create(win, pad, pad, cw - 2 * pad,
                                ch - bar - cp_row_h() - gap, &ac_model,
                                config_get("accounts.view", "table"), NULL);
    if (ac_view) ac_view->on_select = ac_on_select;

    ac_detail = w_label_create(win, pad, ch - bar - cp_row_h(), cw - 2 * pad,
                               "Select an account.");

    int y = ch - bar + gap, x = pad;
    ac_btn_new = w_button_create(win, 0, 0, 0, 0, "New", act_new, NULL);
    if (ac_btn_new) x += w_button_autosize(ac_btn_new, x, y) + gap;
    ac_btn_pw = w_button_create(win, 0, 0, 0, 0, "Set password", act_password, NULL);
    if (ac_btn_pw) x += w_button_autosize(ac_btn_pw, x, y) + gap;
    ac_btn_admin = w_button_create(win, 0, 0, 0, 0, "Toggle admin", act_admin, NULL);
    if (ac_btn_admin) x += w_button_autosize(ac_btn_admin, x, y) + gap;

    /* Delete sits at the far right, away from the others — devicepanel.c puts
     * `Crash` there on the same reasoning: the gap is the only thing standing
     * between a curious click and something that cannot be undone. */
    ac_btn_del = w_button_create(win, 0, 0, 0, 0, "Delete", act_delete, NULL);
    if (ac_btn_del) {
        w_button_autosize(ac_btn_del, 0, y);
        ac_btn_del->base.x = cw - pad - ac_btn_del->base.w;
    }
    ac_update_controls();
}

static void accounts_panel_open(void) {
    if (ac_win) { gui_window_raise(ac_win); return; }
    int ow, oh;
    gui_window_outer_for_content(cp_px(720), cp_px(360), &ow, &oh);
    ac_win = gui_app_window_create("User accounts", 140, 110, ow, oh,
                                   ac_layout, NULL);
    if (ac_win) gui_window_set_on_close(ac_win, ac_on_close);
}

SETTINGS_PANEL(accounts) = {
    "User accounts",
    "who may use this machine, and what each of them may do",
    ICON_APP,
    accounts_panel_open
};
