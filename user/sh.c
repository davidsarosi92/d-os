/* =============================================================================
 * sh.c — a minimal POSIX-ish shell, linked against REAL musl (§M36).
 *
 * Non-interactive for now: `sh -c "cmd1 args; cmd2 args"`.  It splits the
 * command line on ';', tokenises each command on whitespace, and runs it with
 * the classic fork() + execvp() + waitpid() dance — the real shell core.  This
 * exercises the Linux-ABI fork/execve/waitpid path (a musl process spawning
 * other musl processes from /bin via PATH), the natural proof that d-os hosts a
 * genuine Unix process model, not just single-shot programs.
 *
 * execvp resolves bare names against PATH (=/bin, where `pkg install` exposes
 * the store binaries).  Interactive REPL mode is a follow-up (needs blocking
 * stdin wired for a real user process).
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* Tokenise `cmd` in place on whitespace and run it (fork/execvp/waitpid). */
static void run_command(char* cmd) {
    char* argv[32];
    int argc = 0;
    char* p = cmd;
    while (*p && argc < 31) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    if (argc == 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "sh: %s: command not found\n", argv[0]);
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        fprintf(stderr, "sh: fork failed\n");
    }
}

/* Split a command line on ';' and run each piece in order. */
static void run_line(char* line) {
    char* start = line;
    for (char* p = line; ; p++) {
        if (*p == ';' || *p == '\0') {
            char end = *p;
            *p = '\0';
            run_command(start);
            if (end == '\0') break;
            start = p + 1;
        }
    }
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        run_line(argv[2]);
        return 0;
    }

    /* Interactive REPL — reads a line from the cooked stdin (the kernel's
     * vc line-discipline), runs it, repeats.  `exit`/`quit` (or EOF) leaves. */
    char line[256];
    for (;;) {
        fputs("d-os$ ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;          /* EOF */
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (line[0]) run_line(line);
    }
    return 0;
}
