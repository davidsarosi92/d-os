/* shell.h — interactive REPL entry point. */

#ifndef SHELL_H
#define SHELL_H

struct vc;

/* Run the shell forever inside `v` — reads keystrokes from `v`'s input
 * ring, dispatches commands, and prints into `v`'s viewport.
 *
 * Requires the keyboard driver and the VC subsystem to be initialized.
 * Each shell task (one per pane) calls this with its own VC.  Never
 * returns. */
void shell_run(struct vc* v);

/* Task entry-point for `task_spawn(..., shell_task_entry)`.  Reads its
 * bound VC out of task_current()->out_console (must be set BEFORE the
 * task is first scheduled) and tail-calls into shell_run. */
void shell_task_entry(void);

#endif
