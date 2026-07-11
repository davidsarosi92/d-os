/* =============================================================================
 * forkexec.c — the classic fork() + execv() + waitpid() pattern (M34 slice C).
 *
 * The parent forks; the child replaces itself with /bin/args (installed into
 * the ramfs at boot) via execv; the parent waits and reports the exit status.
 * /bin/args returns its argc as the exit code, so with two argv the parent
 * should see status = 2.
 * ============================================================================= */

#include "libc.h"

int main(void) {
    printf("forkexec: parent pid=%d\n", getpid());

    int pid = fork();
    if (pid == 0) {
        char* av[] = { "args", "via-execve", 0 };
        printf("forkexec: child pid=%d exec'ing /bin/args\n", getpid());
        execv("/bin/args", av);
        printf("forkexec: execv FAILED\n");    /* only reached on failure */
        exit(99);
    }

    int status = -1;
    int w = waitpid(pid, &status);
    printf("forkexec: child %d exited status=%d (expect argc=2)\n", w, status);
    return 0;
}
