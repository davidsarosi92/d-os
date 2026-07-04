/* =============================================================================
 * rescue_shell.c — minimal alternative shell provider (§S.1).
 *
 * Exists to PROVE the shell_provider registry swaps, the same way
 * shell_bare.c proves the desktop-shell swap: `setconf shell.provider
 * rescue`, then every NEW shell (pane split, GUI terminal window,
 * next boot's root shell if saved) runs this instead of the full
 * d-os shell.  Also the template for purpose-built shells (installer,
 * kiosk, serial-only rescue console...).
 *
 * Deliberately tiny: three commands, no VFS, no config writes.  Talks
 * to its console exactly like shell.c does — via the struct vc bound
 * to task->out_console, which works in panes AND GUI windows.
 * ============================================================================= */

#include "shell_provider.h"
#include "vc.h"
#include "task.h"
#include "printf.h"
#include "timer.h"
#include "hal.h"
#include <stdint.h>
#include <stddef.h>

#define RLINE_MAX 64

static int rstreq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void rescue_read_line(struct vc* v, char* buf, int cap) {
    int len = 0;
    for (;;) {
        char c = vc_getchar(v);
        if (c == '\n') {
            vc_putchar(v, '\n');
            buf[len] = 0;
            return;
        }
        if (c == '\b') {
            if (len > 0) { len--; vc_putchar(v, '\b'); }
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = c;
            vc_putchar(v, c);
        }
    }
}

static void rescue_entry(void) {
    struct task* me = task_current();
    struct vc*   v  = me ? (struct vc*)me->out_console : NULL;
    if (!v) {
        kprintf("rescue: no VC bound - exiting\n");
        return;
    }

    /* ASCII only — the 8x8 font has no glyphs above 0x7E. */
    kprintf("[RESCUE shell - commands: help, uptime, reboot]\n");
    char line[RLINE_MAX];
    for (;;) {
        kprintf("rescue> ");
        rescue_read_line(v, line, RLINE_MAX);

        if (line[0] == 0) continue;
        if (rstreq(line, "help")) {
            kprintf("rescue shell: help, uptime, reboot\n"
                    "(switch back: full shell sets shell.provider=d-os)\n");
        } else if (rstreq(line, "uptime")) {
            uint64_t ms = timer_ticks_ms();
            kprintf("up %u.%us\n", (unsigned)(ms / 1000),
                    (unsigned)((ms % 1000) / 100));
        } else if (rstreq(line, "reboot")) {
            kprintf("rebooting...\n");
            hal_reboot();
        } else {
            kprintf("rescue: unknown '%s' (help, uptime, reboot)\n", line);
        }
    }
}

SHELL_PROVIDER("rescue", rescue_entry);
