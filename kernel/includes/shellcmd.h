/* =============================================================================
 * shellcmd.h — the shell command registry (§M70).
 *
 * Every other extension point in this tree is a linker-section registry:
 * DRIVER(), GUI_APP(), SERVICE(), CONFIG_KEY(), WIDGET_CLASS(), ITEM_VIEW(),
 * SETTINGS_PANEL(), CRASH_SINK(), LOCALE_CATALOG().  Adding one of those is a
 * FILE.  The shell was the exception — a 172-arm `if`-chain inside
 * `dispatch()` plus a hand-written `cmd_help()` — and it cost exactly the
 * three things a registry prevents:
 *
 *   1. THE HELP DRIFTED.  A command needed a dispatch arm AND a mention in
 *      `cmd_help`, kept in step by hand.  Measured before this registry
 *      existed: 172 dispatch arms, ~60 mentioned in help.  Over a hundred
 *      commands existed and were documented nowhere.  Help is GENERATED here,
 *      so it cannot drift: a command that is registered is a command that is
 *      listed, and there is no second place to forget.
 *
 *   2. THE ORDER SILENTLY SWALLOWED COMMANDS.  The chain matched prefixes, so
 *      an arm testing `"gui "` placed above the exact `"gui stats"` answered
 *      the wrong one and `gui stats` started reporting "already running"
 *      (§4.67.1).  **THE RULE THAT MAKES THAT UNREPRESENTABLE: A VERB OWNS ITS
 *      WHOLE ARGUMENT TAIL.**  The dispatcher splits the line at the first
 *      space and looks the VERB up exactly; everything after it is handed to
 *      that one handler as `args`.  There are no prefix arms to order, so
 *      there is no order to get wrong — `gui` decides what `stats` means,
 *      because `gui` is the only thing that can.
 *
 *   3. COMMANDS WERE x86-ONLY BY ACCIDENT.  aarch64 runs its own REPL
 *      (`hal/aarch64/serial_shell.c`), so every command written into shell.c
 *      had to be duplicated there by hand or not exist on ARM.  §M24's rule
 *      ("a command lives in its own .c, both shells call one copy") was
 *      followed for a handful of files and violated by the other ~130
 *      commands — not as a decision, but because writing into shell.c was the
 *      cheapest way to add one.  Both shells walk THIS registry now, so a
 *      registration is automatically on every architecture that links the
 *      file it lives in.
 *
 * WHERE A COMMAND SHOULD LIVE: next to the code it drives, exactly like
 * CONFIG_KEY().  `audio_cmd_play` belongs in audio.c; the settings commands
 * belong in settings.c.  `kernel/core/cmd_*.c` exists for the commands whose
 * subject is the kernel itself (tasks, memory, the test battery) and which
 * therefore have no other home.
 *
 * THREADING: a handler runs on the shell's own task, in ring 0, with nothing
 * held.  It may block, allocate, touch the VFS and spawn.  It may NOT assume a
 * display exists — on aarch64 with no framebuffer the same handler runs on a
 * PL011 serial REPL.
 * ============================================================================= */

#ifndef SHELLCMD_H
#define SHELLCMD_H

struct vc;

/* ---------------------------------------------------------------------------
 * The descriptor.
 *
 * `name` is the VERB: one word, no spaces.  A multi-word command is a verb
 * with arguments (`gui stats` is the `gui` verb given "stats"), never two
 * registrations — see rule 2 above.
 *
 * `run` always receives a NUL-terminated argument tail with leading spaces
 * stripped, "" when the verb was typed alone.  It is never NULL, so a handler
 * may dereference it without checking.
 *
 * `usage` is the argument sketch shown by `help` ("<path>", "[ms]"), "" for a
 * command that takes none.  `help` is one line; a NULL help HIDES the command
 * from the listing — for aliases and for the deliberately destructive tests
 * (`hardlock` takes the machine down on purpose), which should be reachable
 * without being advertised.
 *
 * `group` sorts the listing.  Use one of the SHELL_G_* strings below rather
 * than a new one, or the listing grows a section per author.
 * ------------------------------------------------------------------------- */
struct shell_cmd {
    const char* name;
    const char* usage;
    const char* help;
    const char* group;
    void      (*run)(const char* args);
};

/* The groups `help` prints, in this order.  Anything else is listed last
 * under "other" — visible, so a stray group name is a thing you can see
 * rather than a command that quietly vanishes. */
#define SHELL_G_SYS    "system"
#define SHELL_G_FS     "files"
#define SHELL_G_TASK   "tasks"
#define SHELL_G_MEM    "memory"
#define SHELL_G_DEV    "devices"
#define SHELL_G_NET    "network"
#define SHELL_G_AUDIO  "audio"
#define SHELL_G_GUI    "desktop"
#define SHELL_G_PKG    "packages"
#define SHELL_G_TEST   "tests"

extern struct shell_cmd __start_shell_cmds[];
extern struct shell_cmd __stop_shell_cmds[];

/* Register a command.  Same shape as every other registry here:
 *
 *   static void cmd_tone(const char* args) { ... }
 *   SHELL_CMD(tone) = { "tone", "<hz> [ms]", "square-wave test tone",
 *                       SHELL_G_AUDIO, cmd_tone };
 *
 * The `used` attribute is load-bearing — nothing REFERENCES the object, so
 * without it the compiler is free to discard the registration and the command
 * disappears from a build that compiled cleanly. */
#define SHELL_CMD(_var)                                                  \
    static const struct shell_cmd                                        \
    __attribute__((used, section("shell_cmds"), aligned(4)))             \
    _var##_registration

/* ---- the registry ------------------------------------------------------- */

int  shell_cmd_count(void);
const struct shell_cmd* shell_cmd_at(int i);

/* Exact lookup by verb.  NULL when nothing claims it. */
const struct shell_cmd* shell_cmd_find(const char* verb);

/* Split `line` into verb + argument tail and run the matching command.
 * Returns 1 when a command claimed the line, 0 when the verb is unknown —
 * the CALLER prints the "unknown command" message, because the two shells
 * report it differently (one has panes and a VC, the other is a UART).
 *
 * An empty line returns 1 having done nothing: "nothing to do" is handled,
 * not unknown. */
int  shell_cmd_dispatch(const char* line);

/* `help`, generated from the registry.  Takes an argument tail so
 * `help <verb>` can print one command's usage line. */
void shell_cmd_help(const char* args);

/* ---------------------------------------------------------------------------
 * The current VC, for the handful of commands that genuinely address the
 * TERMINAL rather than the machine (`clear`, `pane`, `run`).
 *
 * It is set around dispatch by whichever REPL is running and is NULL on a
 * shell that has no VC at all — which is the aarch64 serial REPL, and is
 * exactly why this is an accessor and not a handler argument: making every
 * handler take a `struct vc*` would put a parameter in 170 signatures that
 * three of them read and that is NULL on one architecture.  A command that
 * needs a VC asks for one and says so when there is none.
 * ------------------------------------------------------------------------- */
struct vc* shell_current_vc(void);
void       shell_set_current_vc(struct vc* v);

#endif
