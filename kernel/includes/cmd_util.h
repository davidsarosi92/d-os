/* =============================================================================
 * cmd_util.h — the handful of helpers the shell command files share (§M70).
 *
 * When every command lived in shell.c these were file-statics.  Splitting the
 * commands into topic files (cmd_fs.c, cmd_task.c, …) turned four of them into
 * things two files need, and the alternative to a shared header is a copy per
 * file — which is exactly the shape that produced FOUR scrollbars and TWO
 * shells.  Four small functions in one place instead.
 *
 * These are for the command layer only.  Nothing here belongs in a driver or a
 * subsystem: a subsystem that needs to parse a number should not be reading
 * command-line text in the first place.
 * =========================================================================== */

#ifndef CMD_UTIL_H
#define CMD_UTIL_H

#include <stdint.h>

/* Exact string equality.  The terminator check at the end is what makes a
 * strict prefix NOT equal — the bug this replaces was prefix matching that
 * looked like equality. */
int cmd_streq(const char* a, const char* b);

/* True iff `s` begins with `p` (including p == ""). */
int cmd_starts_with(const char* s, const char* p);

/* Strict decimal / hex parse.  Return 0 on success, -1 when the string is
 * empty or holds a single character the base does not accept — STRICT on
 * purpose: a lenient parser turns `kill 12x` into `kill 12`, and killing the
 * wrong task is worse than refusing the line. */
int cmd_parse_uint(const char* s, uint32_t* out);
int cmd_parse_hex (const char* s, uint32_t* out);

/* Load an ELF off the VFS and run it under the Linux personality, optionally
 * capturing its stdout into `cap` (§M43).  Shared by the package runner and
 * the test battery, which is why it is here rather than static in one of them.
 * Returns the program's exit code, or -1 when the file could not be loaded. */
int dos_run_elf(const char* path);
int dos_run_elf_cap(const char* path, char* cap, int caplen);

#endif
