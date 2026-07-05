/* =============================================================================
 * newshell.c — "New Shell" launcher entry (M22.2).
 *
 * Was a hardcoded Start-menu action inside gui.c; now it is just an
 * app registration that opens one more terminal window.  The window
 * counter lives here — the compositor core doesn't number shells.
 * ============================================================================= */

#include "gui.h"
#include "gui_app.h"

static int next_shell_no = 3;           /* "shell 1" + "shell 2" exist at start */

/* Build "shell N" (session) or "shell N*" (detached) into `name`. */
static int shell_name(char* name, int detached) {
    int n = next_shell_no++;
    int p = 0;
    name[p++] = 's'; name[p++] = 'h'; name[p++] = 'e'; name[p++] = 'l';
    name[p++] = 'l'; name[p++] = ' ';
    if (n >= 10) name[p++] = (char)('0' + n / 10);
    name[p++] = (char)('0' + n % 10);
    if (detached) name[p++] = '*';     /* mark detached in the title */
    name[p] = 0;
    return n;
}

/* M22.7 — SESSION shell: belongs to the desktop, dies with it. */
static void newshell_launch(void) {
    char name[16];
    int n = shell_name(name, 0);
    int k = n % 5;                      /* stagger spawn positions */
    gui_window_create(name, 120 + k * 48, 80 + k * 40, 560, 360);
}

/* M22.7 — DETACHED shell: parented to init, so it survives closing the
 * desktop session (its window persists while the compositor runs). */
static void newshell_launch_detached(void) {
    char name[16];
    int n = shell_name(name, 1);
    int k = n % 5;
    gui_window_create_detached(name, 140 + k * 48, 100 + k * 40, 560, 360);
}

GUI_APP("New Shell", newshell_launch);
GUI_APP("Detached Shell", newshell_launch_detached);
