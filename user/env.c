/* =============================================================================
 * env.c — coreutils `env`, linked against REAL musl (§M36; POSIX form §M90).
 *
 *   env [-i] [-u NAME]... [NAME=value]... [command [arg]...]
 *
 * With no command it prints the (resulting) environment, one VAR=value per
 * line — what it always did.  With one it RUNS it in that environment, which
 * is what `env` is for: `env DOCKER_BUILDKIT=0 docker build .` printed the
 * environment and built nothing, because this env ignored its arguments.
 * -i starts from an empty environment, -u removes a name.  The command is
 * looked up in PATH (execvp); exit 127 when it is not found, 126 when it
 * cannot be run — POSIX's numbers.
 * ============================================================================= */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

extern char** environ;

int main(int argc, char** argv) {
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-") == 0) { clearenv(); continue; }
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) { unsetenv(argv[++i]); continue; }
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        break;
    }
    for (; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++) {
        if (putenv(argv[i]) != 0) { perror("env"); return 125; }
    }
    if (i >= argc) {
        for (char** e = environ; e && *e; e++) puts(*e);
        return 0;
    }
    execvp(argv[i], argv + i);
    int e = errno;
    fprintf(stderr, "env: %s: %s\n", argv[i], strerror(e));
    return e == ENOENT ? 127 : 126;
}
