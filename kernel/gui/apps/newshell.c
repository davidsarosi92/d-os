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

static void newshell_launch(void) {
    char name[16] = "shell ";
    int n = next_shell_no++;
    int p = 6;
    if (n >= 10) name[p++] = (char)('0' + n / 10);
    name[p++] = (char)('0' + n % 10);
    name[p] = 0;

    int k = n % 5;                      /* stagger spawn positions */
    gui_window_create(name, 120 + k * 48, 80 + k * 40, 560, 360);
}

GUI_APP("New Shell", newshell_launch);
