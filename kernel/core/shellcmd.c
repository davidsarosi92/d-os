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
 * One global rather than per-task state: a VC-bound command runs on the shell
 * task that set it, and the two REPLs in this tree are the only writers.  A
 * second shell on a second VC sets it around its own dispatch, so the value is
 * correct for the duration of a handler — which is all any handler reads it
 * for.  (If shells ever run commands concurrently on two panes this becomes a
 * per-task field; the accessor is here so that is one edit.) */
static struct vc* g_cur_vc;

struct vc* shell_current_vc(void)          { return g_cur_vc; }
void       shell_set_current_vc(struct vc* v) { g_cur_vc = v; }

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
    if (!c || !c->run) return 0;

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
    SHELL_G_SYS, cmd_help_entry
};
