/* =============================================================================
 * env.c — a minimal coreutils `env`, linked against REAL musl (§M36).
 *
 * Prints the process environment (one VAR=value per line).  Exercises the
 * envp[] that d-os's SysV initial stack now carries (build_initial_stack) —
 * musl exposes it as `environ`.  Run from the store by `pkgrun`.
 * ============================================================================= */
#include <stdio.h>

extern char** environ;

int main(void) {
    for (char** e = environ; e && *e; e++)
        puts(*e);
    return 0;
}
