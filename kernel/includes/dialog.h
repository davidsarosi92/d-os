/* =============================================================================
 * dialog.h — the modal dialog (§M69, design/widget_specs.md §14).
 *
 * ASK A QUESTION AND GET ONE ANSWER.  The request is DATA (a struct of strings)
 * rather than a call with eight parameters, for §M65's reason: a description
 * that is data can cross a process boundary later, and a caller that leaves a
 * field NULL is asking for the default rather than reciting it.
 *
 * THE ANSWER IS ALWAYS DELIVERED, EXACTLY ONCE, and it may be CANCEL for a
 * reason the caller never sees: Escape, the X button, a refused modal claim,
 * or no window being available at all.  A caller must therefore treat CANCEL
 * as "do nothing" and never as "this cannot have happened" — which is also why
 * there is no return path that means "no answer": a feature waiting for a
 * callback that will never arrive is indistinguishable from a hang.
 *
 * ASYNCHRONOUS ON PURPOSE.  There is no `gui_dialog_ask()` that blocks and
 * returns the answer, because the caller is usually an app-host task and the
 * dialog needs one of its own; a blocking form would have the asking task
 * parked while the answering task runs, which is a lifetime relationship this
 * tree has paid for twice already (§M54, §M57).  The callback runs on the
 * DIALOG's host task, after its window is gone.
 * ========================================================================= */

#ifndef DIALOG_H
#define DIALOG_H

#define GUI_DIALOG_CANCEL 0
#define GUI_DIALOG_OK     1

struct gui_dialog_req {
    const char* title;        /* the window's title bar; "Confirm" if NULL   */
    const char* body;         /* '\n' splits lines — the dialog does NOT wrap */
    const char* info;         /* optional mono line (a path, a device); NULL  */
    const char* ok_text;      /* "OK" if NULL                                 */
    /* "Cancel" if NULL.  An EMPTY string means a one-button dialog — a notice
     * rather than a question.  Escape and the X still cancel it, so
     * "acknowledged" and "dismissed" stay distinguishable to the caller. */
    const char* cancel_text;
    void (*on_answer)(int answer, void* ctx);
    void* ctx;
};

/* 0 = the dialog is on its way up, -1 = refused (one is already open, or there
 * is no GUI session).  A refusal is REPORTED, never queued: a question that
 * arrives seconds after the action that raised it is asked of a user who has
 * already moved on. */
int gui_dialog_open(const struct gui_dialog_req* req);

int gui_dialog_active(void);

/* The `dialog [path]` shell command — a demonstration dialog, and the only
 * automated route to one: the harness cannot type once a GUI window has focus
 * (§4.74) and a modal holds it by definition. */
void dialog_command(const char* arg);

#endif /* DIALOG_H */
