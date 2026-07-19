/* =============================================================================
 * forktest64.c — POSIX process model from REAL musl (used to validate the
 * x86_64 fork/execve/waitpid/pipe port).  Distinct from user/forktest.c, which
 * is the in-tree NATIVE-libc test; this one is a normal hosted musl program.
 *
 * Part 1: fork() a child that execve()'s /bin/hello64 (the kernel provisions it
 *         from the muslhello blob).  If exec works the child becomes hello and
 *         exits 0; the parent waitpid()s it.
 * Part 2: fork() a child, hand a string through a pipe, waitpid() the exit code
 *         — proving the child got its own (eager-copied) address space and that
 *         inherited pipe fds work.
 * ============================================================================= */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

extern char** environ;

int main(void) {
    /* Part 1 — fork + execve. */
    pid_t p = fork();
    if (p == 0) {
        char* av[] = { "hello64", NULL };
        execve("/bin/hello64", av, environ);
        _exit(99);                          /* only reached if execve failed */
    }
    int st = 0;
    waitpid(p, &st, 0);
    printf("forktest64: execve child exit=%d\n", WEXITSTATUS(st));

    /* Part 2 — fork + pipe + waitpid. */
    int fd[2];
    if (pipe(fd) != 0) { printf("forktest64: pipe failed\n"); return 1; }
    pid_t q = fork();
    if (q == 0) {
        close(fd[0]);
        write(fd[1], "ok", 2);
        _exit(3);
    }
    close(fd[1]);
    char b[4] = { 0 };
    int n = (int)read(fd[0], b, sizeof b);
    int s2 = 0;
    waitpid(q, &s2, 0);
    printf("forktest64: pipe='%.*s' child exit=%d\n", n, b, WEXITSTATUS(s2));
    return 0;
}
