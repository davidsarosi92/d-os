/* =============================================================================
 * shellcmd.c — the shell command registry (§M70).
 *
 * See shellcmd.h for why this exists.  The whole of the dispatcher is the
 * verb split plus an exact lookup, and that is the point: the previous
 * implementation was 172 ordered prefix tests, and its failure mode was a
 * command that existed and could not be reached.
 * ============================================================================= */

#include "shellcmd.h"
#include "console.h"
#include "printf.h"
#include "vc.h"
#include "cred.h"   /* §M32 — the privilege gate */
#include "users.h"  /* §M32 — auth_elevate */

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int shell_cmd_count(void) {
    return (int)(__stop_shell_cmds - __start_shell_cmds);
}

const struct shell_cmd* shell_cmd_at(int i) {
    if (i < 0 || i >= shell_cmd_count()) return 0;
    return &__start_shell_cmds[i];
}

const struct shell_cmd* shell_cmd_find(const char* verb) {
    int n = shell_cmd_count();
    for (int i = 0; i < n; i++)
        if (__start_shell_cmds[i].name && streq_(__start_shell_cmds[i].name, verb))
            return &__start_shell_cmds[i];
    return 0;
}

/* ---- the current VC ------------------------------------------------------
 *
 * PER TASK, NOT A GLOBAL (2026-09-25).  This used to be one global that each
 * REPL set around its own dispatch, with a note that it would have to become
 * per-task "if shells ever run commands concurrently on two panes".  They
 * always could: every pane is its own shell task, and a command that SLEEPS
 * (anything touching the disk, the network or a timer) lets another pane
 * dispatch and overwrite the global — after which the first command's
 * `clear` clears somebody else's terminal.  The serial command channel made
 * the concurrency explicit (a command arriving on COM1 runs beside whatever
 * the focused pane is doing), so the note's condition is now simply true.
 *
 * The answer was already recorded where it cannot race: every x86 shell task
 * is spawned with its VC bound as `out_console` (§M49 moved that INTO the
 * spawn), which is also what `vc_source` below reads for input.  A task with
 * no VC — the ARM serial REPL, the COM1 command channel, a service — gets
 * NULL, which is what those handlers already treat as "no terminal here". */
#include "task.h"

struct vc* shell_current_vc(void) {
    struct task* t = task_current();
    return t ? (struct vc*)t->out_console : NULL;
}

/* ---- dispatch -------------------------------------------------------------
 *
 * THE VERB OWNS ITS ARGUMENT TAIL.  Split at the first space, look the verb up
 * exactly, hand everything after it to that handler.  No prefix matching
 * anywhere, so no arm can be shadowed by one above it (§4.67.1). */
int shell_cmd_dispatch(const char* line) {
    while (*line == ' ') line++;
    if (*line == '\0') return 1;                /* empty line: handled, no-op */

    /* Copy the verb out.  A verb longer than this is not a verb; it falls
     * through to "unknown", which is the honest answer. */
    char verb[32];
    int  n = 0;
    while (line[n] && line[n] != ' ' && n < (int)sizeof(verb) - 1) {
        verb[n] = line[n];
        n++;
    }
    verb[n] = '\0';

    const struct shell_cmd* c = shell_cmd_find(verb);
    /* §M89 — not a built-in: a PROGRAM on the PATH (an installed application
     * in /bin, a Linux binary), run in the foreground with the whole line as
     * its arguments.  Built-ins win, so no program can take over a command. */
    if (!c || !c->run) return shell_run_from_path(line);

    /* §M32 stage 7 — THE PRIVILEGE GATE, in one place for every command.
     *
     * `cred_is_admin()` answers 1 for a KERNEL or SYSTEM context, so the boot
     * console, every service and every script are unaffected — the gate bites
     * exactly one case, a LOGGED-IN user who is not an administrator, which is
     * the case it was built for.
     *
     * UNDECLARED is refused rather than waved through.  A command whose author
     * did not think about this is not a command whose author meant "anybody";
     * the audit names it, and until somebody decides, nobody runs it. */
    if (c->priv == SHELL_P_UNDECLARED) {
        kprintf("%s: this command has not declared who may run it — refused "
                "(see `audit command-privilege`)\n", c->name);
        return 1;
    }
    if (c->priv == SHELL_P_ADMIN) {
        /* §M32 stage 8 — eligibility is not permission.  auth_elevate answers
         * both questions in one call: a SYSTEM context proceeds, an eligible
         * administrator re-authenticates, and anybody else is refused with the
         * reason.  Splitting it into "are you an admin" here and "prove it"
         * somewhere else would be two rules that can disagree. */
        if (auth_elevate(c->name) != 0) {
            kprintf("%s: refused\n", c->name);
            return 1;
        }
    }

    const char* args = line + n;
    while (*args == ' ') args++;                /* handlers never see leading  */
    c->run(args);                               /* spaces, and never get NULL  */
    return 1;
}

/* ---- generated help -------------------------------------------------------
 *
 * NB: this kernel's printf has NO WIDTH SPECIFIERS — `%-12s` prints
 * literally.  It has bitten this project repeatedly (§M65, §M66, §M33
 * stage 5, the localisation sweep), so the columns are padded by hand. */

static void pad_to(int have, int want) {
    while (have++ < want) console_putchar(' ');
}

static int print_one(const struct shell_cmd* c) {
    /* "  verb usage" padded to a column, then the help text. */
    int w = 2;
    console_write("  ");
    console_write(c->name);
    for (const char* p = c->name; *p; p++) w++;
    if (c->usage && c->usage[0]) {
        console_putchar(' ');
        w++;
        console_write(c->usage);
        for (const char* p = c->usage; *p; p++) w++;
    }
    if (c->help && c->help[0]) {
        pad_to(w, 30);
        console_write("  ");
        console_write(c->help);
    }
    console_putchar('\n');
    return 1;
}

static const char* const g_groups[] = {
    SHELL_G_SYS, SHELL_G_FS, SHELL_G_TASK, SHELL_G_MEM, SHELL_G_DEV,
    SHELL_G_NET, SHELL_G_AUDIO, SHELL_G_GUI, SHELL_G_PKG, SHELL_G_TEST
};
#define N_GROUPS ((int)(sizeof(g_groups) / sizeof(g_groups[0])))

/* Is `g` one of the known groups?  Anything else is printed under "other" —
 * VISIBLE rather than dropped, so a mistyped group name costs a misplaced
 * line instead of a command nobody can find in the listing. */
static int known_group(const char* g) {
    if (!g) return 0;
    for (int i = 0; i < N_GROUPS; i++)
        if (streq_(g_groups[i], g)) return 1;
    return 0;
}

void shell_cmd_help(const char* args) {
    int total = shell_cmd_count();

    /* `help <verb>` — one command, in full. */
    if (args && args[0]) {
        const struct shell_cmd* c = shell_cmd_find(args);
        if (!c) {
            console_write("help: no such command: ");
            console_write(args);
            console_putchar('\n');
            return;
        }
        print_one(c);
        return;
    }

    for (int gi = 0; gi <= N_GROUPS; gi++) {
        const char* g = (gi < N_GROUPS) ? g_groups[gi] : 0;   /* last pass: other */

        /* Count first, so a group with nothing in it prints no heading —
         * an empty section reads as a subsystem that failed to register. */
        int hits = 0;
        for (int i = 0; i < total; i++) {
            const struct shell_cmd* c = &__start_shell_cmds[i];
            if (!c->name || !c->help) continue;               /* hidden */
            int mine = g ? (c->group && streq_(c->group, g)) : !known_group(c->group);
            if (mine) hits++;
        }
        if (!hits) continue;

        console_putchar('\n');
        console_write(g ? g : "other");
        console_write(":\n");
        for (int i = 0; i < total; i++) {
            const struct shell_cmd* c = &__start_shell_cmds[i];
            if (!c->name || !c->help) continue;
            int mine = g ? (c->group && streq_(c->group, g)) : !known_group(c->group);
            if (mine) print_one(c);
        }
    }

    kprintf("\n%d commands registered (`help <command>` for one).\n", total);
}

/* `help` is itself a registration — otherwise it would be the one command
 * that still needed a hand-written dispatch arm. */
static void cmd_help_entry(const char* args) { shell_cmd_help(args); }

SHELL_CMD(help) = {
    "help", "[command]", "list commands, or explain one",
    SHELL_G_SYS, cmd_help_entry, SHELL_P_ANY };

/* ---------------------------------------------------------------------------
 * §M32 stage 4 — interactive input.  See shellcmd.h for why this is here and
 * not in any one of the three shells.
 * ------------------------------------------------------------------------- */

#include "task.h"
#include "vc.h"

static int (*g_getch)(void);

/* The default: whatever VC this task is bound to.  Both x86 shells bind one at
 * spawn (§M49 moved that INTO the spawn precisely so it could not race), so a
 * command running on either of them reads the pane the user is typing into
 * rather than whichever pane happens to hold focus. */
static int vc_source(void) {
    struct task* t = task_current();
    struct vc* v = t ? (struct vc*)t->out_console : NULL;
    if (!v) return -1;
    return (int)(unsigned char)vc_getchar(v);
}

void shell_set_input_source(int (*getch)(void)) { g_getch = getch; }

static int read_into(const char* prompt, char* buf, int cap, int echo) {
    int (*get)(void) = g_getch ? g_getch : vc_source;
    int len = 0;
    if (prompt) kprintf("%s", prompt);
    for (;;) {
        int ci = get();
        if (ci < 0) {           /* no input source — refuse rather than spin */
            buf[0] = 0;
            return -1;
        }
        char c = (char)ci;
        if (c == '\n' || c == '\r') {
            console_write("\n");
            buf[len] = 0;
            return len;
        }
        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                /* Erase on screen only when we were echoing.  A secret prints
                 * nothing at all, so there is nothing to erase — and emitting
                 * a backspace would betray that a character had been typed. */
                if (echo) console_write("\b \b");
            }
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = c;
            if (echo) { char s[2] = { c, 0 }; console_write(s); }
        }
    }
}

int shell_read_line(const char* prompt, char* buf, int cap) {
    return read_into(prompt, buf, cap, 1);
}

int shell_read_secret(const char* prompt, char* buf, int cap) {
    return read_into(prompt, buf, cap, 0);
}

/* ---------------------------------------------------------------------------
 * §M71 — every command has declared who may run it.
 *
 * The sweep that introduced `priv` covered 152 registrations; the point of this
 * check is the 153rd.  A command added without a declaration is refused at
 * runtime (see shell_cmd_dispatch) AND named here, so it fails on the day it is
 * added rather than the day somebody needed it.
 *
 * HOW TO MAKE IT FAIL: `privtest`, which clears one registration's declaration
 * in place — §M71 rule 1, detection rather than reporting.
 * ------------------------------------------------------------------------- */

#include "audit.h"

/* The registration is `const`, so the falsifier cannot write through the
 * ordinary pointer; it takes the section's own address instead.  Deliberate:
 * a test that needed the table to be mutable would have made it mutable for
 * everybody. */
static int au_command_privilege(int verbose) {
    int n = shell_cmd_count();
    if (n == 0) return AUDIT_SKIP;
    int bad = 0, admin = 0, anyone = 0;
    for (int i = 0; i < n; i++) {
        const struct shell_cmd* c = shell_cmd_at(i);
        if (!c) continue;
        if (c->priv == SHELL_P_UNDECLARED) {
            kprintf("  !! '%s' has not declared who may run it\n", c->name);
            bad++;
        } else if (c->priv == SHELL_P_ADMIN) admin++;
        else anyone++;
    }
    if (verbose)
        kprintf("  %d command(s): %d admin, %d unrestricted, %d undeclared\n",
                n, admin, anyone, bad);
    return bad;
}

AUDIT(command_privilege) = {
    "command-privilege",
    "every registered shell command declares who may run it",
    au_command_privilege
};

static void cmd_privtest(const char* args) {
    (void)args;
    struct shell_cmd* t = (struct shell_cmd*)shell_cmd_at(0);
    if (!t) { console_write("privtest: no commands registered\n"); return; }
    int saved = t->priv;
    t->priv = SHELL_P_UNDECLARED;
    kprintf("privtest: cleared '%s' declaration — `audit command-privilege` "
            "must FAIL\n", t->name);
    int v = audit_run_one("command-privilege", 0);
    kprintf("privtest: audit reported %d violation(s) — %s\n",
            v, v > 0 ? "DETECTED" : "NOT DETECTED (the check is broken)");
    t->priv = saved;
    v = audit_run_one("command-privilege", 0);
    kprintf("privtest: restored; audit reports %d violation(s) — %s\n",
            v, v == 0 ? "clean" : "STILL DIRTY");
}

SHELL_CMD(privtest) = { "privtest", "", NULL, SHELL_G_TEST, cmd_privtest, SHELL_P_ADMIN };
