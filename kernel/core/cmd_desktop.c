/* =============================================================================
 * cmd_desktop.c — desktop, terminal-pane and keyboard-layout commands (§M70).
 *
 * Split out of shell.c.  The compositor-side implementations live in
 * kernel/gui/ and register themselves there (`wallpaper`, `theme`, `shortcut`,
 * `mode`, `clip`, `ui`, `dialog`); what is left here is the set that drives
 * the GUI from OUTSIDE it — starting and stopping the session, launching a
 * registered app, splitting panes — plus the keyboard layout, which belongs to
 * the console rather than to the desktop.
 *
 * `gui` OWNS ITS ARGUMENT TAIL and that is the fix for a real regression:
 * §4.67.1 had a `gui ` prefix arm dispatched above the exact `gui stats`, so
 * `gui stats` answered "already running".  With one verb and one handler there
 * is no second arm to be ordered wrongly.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "gui.h"
#include "gui_app.h"
#include "ui.h"
#include "vc.h"
#include "keymap.h"
#include "basic.h"
#include "task.h"
#include "config.h"
#include "proc.h"
#include "vfs.h"
#include "kmalloc.h"
#include "hal_api.h"
#include "shell_provider.h"
#include <stdint.h>
#include <stddef.h>

static void pane_list_one(struct vc* v, void* ctx) {
    (void)ctx;
    int x, y, w, h;
    if (vc_get_rect(v, &x, &y, &w, &h) != 0) return;
    const char* tag = (vc_focused() == v) ? " <focus>" : "";
    int pid = v->task ? v->task->pid : -1;
    kprintf("  [%d] rect=(%d,%d %dx%d) pid=%d%s\n",
            v->id, x, y, w, h, pid, tag);
}

static void cmd_pane_list(void) {
    kprintf("panes (%d):\n", vc_count());
    vc_for_each(pane_list_one, NULL);
    kprintf("(Alt-1..Alt-9 to switch focus)\n");
}

static void cmd_pane_split(struct vc* my_vc, enum vc_split_dir dir) {
    struct vc* nv = vc_split(my_vc, dir);
    if (!nv) {
        kprintf("pane: split failed (max %d panes?)\n", VC_MAX);
        return;
    }
    /* Spawn the shell task for the new pane with the VC already bound, so
     * the task's first kprintf routes through vc_putchar(nv, ...).
     * §M49 — this was a set-after-spawn under preempt_disable, which is
     * not a barrier against another CPU picking the task up (that counter
     * is per-CPU).  The binding belongs in the spawn. */
    struct task* t = task_spawn_console("shell", shell_provider_active()->entry,
                                        -1, nv);
    if (t) nv->task = t;

    if (!t) {
        kprintf("pane: spawn failed for new pane id=%d\n", nv->id);
        return;
    }
    /* The new pane's shell will draw its own prompt as soon as it runs. */
}

static void lslayout_one(const struct kbd_layout* l, void* ctx) {
    (void)ctx;
    const char* tag = cmd_streq(l->name, keymap_current()) ? " <active>" : "";
    kprintf("  %s%s\n", l->name, tag);
}

static void cmd_lslayout(void) {
    kprintf("keyboard layouts:\n");
    keymap_for_each(lslayout_one, NULL);
}

static void cmd_setlayout(const char* name) {
    if (!name || !*name) { console_write("setlayout: missing name\n"); return; }
    if (keymap_select(name) == 0) {
        kprintf("layout: now '%s'\n", keymap_current());
    } else {
        kprintf("setlayout: unknown layout '%s'\n", name);
    }
}

/* `launch [app]` — walk the GUI_APP registry (M22.2).  Without an
 * argument it lists the registered apps; with one it launches the
 * (case-insensitive, prefix-matched) app.  This is how apps start
 * under chromeless desktop shells (gui.shell=bare), and it runs the
 * app on THIS shell task — the gui/widget APIs are task-agnostic. */
static void cmd_launch(const char* args) {
    if (!gui_is_active()) {
        console_write("launch: GUI not running (start it with 'gui')\n");
        return;
    }
    if (!args || !*args) {
        kprintf("registered GUI apps (%d):\n", gui_app_count());
        for (int i = 0; i < gui_app_count(); i++)
            kprintf("  %s\n", gui_app_at(i)->name);
        return;
    }
    const struct gui_app_def* app = gui_app_find(args);
    if (!app) { kprintf("launch: no app matching '%s'\n", args); return; }
    /* M22.7 — hand it to the compositor, which spawns the app-host task.
     * Calling app->launch() here would run the app on this shell's task
     * with no event loop. */
    gui_queue_launch(app);
}

/* `run <path>` — batch-run a Tiny-BASIC program on this shell's VC
 * (M22.5).  The interpreter state is ~22 KiB, so it lives on the heap
 * — never on the 4 KiB task stack.  If the shell is killed mid-run
 * the block leaks; acceptable for a hand-driven command (the GUI
 * BASIC window uses a static instance instead). */
static void cmd_run(struct vc* my_vc, const char* path) {
    while (*path == ' ') path++;
    if (!*path) { kprintf("run: usage: run <path.bas>\n"); return; }
    struct basic* b = (struct basic*)kmalloc(sizeof *b);
    if (!b) { kprintf("run: OOM\n"); return; }
    basic_init(b, my_vc);
    if (basic_load(b, path) != 0)
        kprintf("run: cannot load %s (missing? unnumbered lines?)\n", path);
    else
        basic_run(b);
    kfree(b);
}

/* `gui stats` — damage-rect effectiveness counters (M22.3). */
static void cmd_gui_stats(void) {
    if (!gui_is_active()) { console_write("gui stats: GUI not running\n"); return; }
    unsigned full = 0, partial = 0, avg_kb = 0;
    gui_get_stats(&full, &partial, &avg_kb);
    kprintf("frames: %u full, %u partial (dirty-rect), avg %u KB blitted/frame\n",
            full, partial, avg_kb);
    struct gui_desktop_stats d;
    gui_get_desktop_stats(&d);
    kprintf("desktop: %u loop iterations, %u panel repaints, %u chrome events\n",
            d.iters, d.draws, d.events);
    {
        /* §M69 — INPUT QUEUE HEALTH.  `dropped` must stay 0: it counts events
         * thrown away because the per-window ring was full of things that all
         * mattered, and a dropped press is a click the user made and the
         * machine did not see.  It used to happen silently, which is why four
         * separate reports ("the arrow works sometimes", "I cannot drag the
         * thumb", "the slider cannot be dragged", "a button click does
         * nothing") all pointed at different controls and had one cause. */
        unsigned dr = 0, co = 0;
        gui_get_input_stats(&dr, &co);
        kprintf("input:   %u event(s) dropped, %u position(s) coalesced\n", dr, co);
    }
    kprintf("         %u half-second ticks, %u changed the chrome, clock %u ms\n",
            d.ticks, d.tick_dirty, d.clock_ms);
}

/* `gui` — start the M22 compositor.  The calling shell keeps running in
 * its (now invisible) pane; two fresh shells come up in windows.  A
 * second invocation is a no-op — the compositor is a singleton. */
static void cmd_gui(const char* args) {
    while (args && *args == ' ') args++;
    if (args && cmd_starts_with(args, "stop")) {
        /* The other direction, from the shell side.  It exists because the
         * Start menu's "Exit GUI" is unreachable when the desktop is what went
         * wrong — and a way out that only works while everything works is not
         * a way out. */
        if (gui_stop() != 0) console_write("gui: not running\n");
        return;
    }
    if (gui_is_active()) {
        console_write("gui: already running\n");
        return;
    }
    if (gui_start() != 0)
        console_write("gui: start failed (no framebuffer?)\n");
}

/* `pane [split horizontal|vertical]` argument parser. */
static void cmd_pane(struct vc* my_vc, const char* args) {
    if (!args || !*args) { cmd_pane_list(); return; }

    /* skip leading spaces */
    while (*args == ' ') args++;
    if (!*args) { cmd_pane_list(); return; }

    /* expect "split <dir>" */
    if (cmd_starts_with(args, "split ")) {
        const char* dir = args + 6;
        while (*dir == ' ') dir++;
        if (cmd_starts_with(dir, "horiz") || cmd_streq(dir, "h")) {
            cmd_pane_split(my_vc, VC_SPLIT_HORIZ);
        } else if (cmd_starts_with(dir, "vert") || cmd_streq(dir, "v")) {
            cmd_pane_split(my_vc, VC_SPLIT_VERT);
        } else {
            console_write("pane: split direction must be horizontal or vertical\n");
        }
        return;
    }

    console_write("pane: unknown subcommand (try: pane, pane split horizontal)\n");
}

extern const unsigned char _binary_user_uidemo_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_uidemo_elf_end[]   __attribute__((weak));

static void cmd_uidemo(void) {
    if (!_binary_user_uidemo_elf_start) {
        console_write("uidemo: not embedded for this arch\n");
        return;
    }
    if (!gui_is_active()) { console_write("uidemo: the GUI is not running\n"); return; }
    size_t len = (size_t)(_binary_user_uidemo_elf_end - _binary_user_uidemo_elf_start);
    const char* argv[] = { "uidemo" };
    int pid = proc_spawn_argv_under("uidemo", _binary_user_uidemo_elf_start, len,
                                    1, argv, 0, gui_desktop_pid());
    kprintf("uidemo: spawned pid %d\n", pid);
}

/* --- registrations --------------------------------------------------------- */

/* THE VERB OWNS THE TAIL.  Everything `gui` can be asked is decided here, in
 * one function, because `gui` is the only thing that knows what `stats` means. */
static void ds_gui(const char* a) {
    if (cmd_streq(a, "stats"))     { cmd_gui_stats();      return; }
    if (cmd_streq(a, "widgets"))   { gui_widget_report();  return; }
    if (cmd_streq(a, "relayout"))  { gui_relayout_all();   return; }
    if (cmd_streq(a, "relaytest")) { gui_relayout_test(3); return; }
    if (cmd_streq(a, "bench"))     { gui_compose_bench(6); return; }
    /* §M81's falsifier — deliberately makes the mistake `gui_app_open` warns
     * about.  Hidden from `help` like `hardlock` and `leaktest`: it is a way to
     * see a check fail, not a thing to do. */
    if (cmd_streq(a, "hosttest"))  { gui_host_test();      return; }
    if (cmd_streq(a, "slottest"))  { gui_slot_test();      return; }
    cmd_gui(a);                                  /* "" = start, "stop" = stop */
}

static void ds_termcheck(const char* a) { (void)a; gui_term_check(); }
static void ds_uidemo   (const char* a) { (void)a; cmd_uidemo();     }
static void ds_lslayout (const char* a) { (void)a; cmd_lslayout();   }

/* `run <path.bas>` needs the VC to give Tiny-BASIC a terminal to talk to. */
static void ds_run(const char* a) {
    struct vc* v = shell_current_vc();
    if (!v) { console_write("run: needs a terminal (this shell has none)\n"); return; }
    cmd_run(v, a);
}

static void ds_pane(const char* a) {
    struct vc* v = shell_current_vc();
    if (!v) { console_write("pane: this shell has no panes\n"); return; }
    cmd_pane(v, a);
}

/* wheeltest <x> <y> <dz> — a notch through the REAL dispatcher at a CONTENT
 * coordinate.  dz == 0 is PROBE mode: it reports what is under the point and
 * who would take a notch there WITHOUT moving anything, so a sweep is a MAP.
 * Injecting real notches to sample several points changes the layout between
 * samples, and the second sample then describes something the first did not. */
static void ds_wheeltest(const char* a) {
    int v[3] = { 0, 0, -1 };
    for (int i = 0; i < 3 && *a; i++) {
        int neg = 0;
        while (*a == ' ') a++;
        if (*a == '-') { neg = 1; a++; }
        int n = 0;
        while (*a >= '0' && *a <= '9') n = n * 10 + (*a++ - '0');
        v[i] = neg ? -n : n;
    }
    gui_wheel_test(v[0], v[1], v[2]);
}

SHELL_CMD(gui)       = { "gui", "[stop|stats|widgets|relayout|bench]",
                         "the compositor and the desktop session",
                         SHELL_G_GUI, ds_gui, SHELL_P_ANY };
SHELL_CMD(launch)    = { "launch", "[app]", "start a registered GUI app",
                         SHELL_G_GUI, cmd_launch, SHELL_P_ADMIN };
SHELL_CMD(run)       = { "run", "<path.bas>", "run a Tiny-BASIC program",
                         SHELL_G_GUI, ds_run, SHELL_P_ANY };
SHELL_CMD(pane)      = { "pane", "[split horizontal|vertical]", "terminal panes",
                         SHELL_G_GUI, ds_pane, SHELL_P_ANY };
SHELL_CMD(lslayout)  = { "lslayout", "", "keyboard layouts",
                         SHELL_G_SYS, ds_lslayout, SHELL_P_ANY };
SHELL_CMD(setlayout) = { "setlayout", "<us|hu|...>", "switch keyboard layout",
                         SHELL_G_SYS, cmd_setlayout, SHELL_P_ANY };

SHELL_CMD(termcheck) = { "termcheck", "", "terminal scrollback self-test, in a GUI window",
                         SHELL_G_TEST, ds_termcheck, SHELL_P_ANY };
SHELL_CMD(uidemo)    = { "uidemo", "", "the toolkit driven from a ring-3 client",
                         SHELL_G_TEST, ds_uidemo, SHELL_P_ANY };
SHELL_CMD(wheeltest) = { "wheeltest", "<x> <y> <dz>",
                         "inject (dz=0: probe) a wheel notch at a content point",
                         SHELL_G_TEST, ds_wheeltest, SHELL_P_ANY };
