/* =============================================================================
 * pipetest.c — pipe() + dup2() + fork() (M34 slice D).
 *
 * The parent makes a pipe and forks; the child writes a message into the write
 * end; the parent dup2()s the read end to a high fd and reads the message back
 * through it, proving the pipe channel, fd inheritance across fork, and dup2.
 * ============================================================================= */

#include "libc.h"

int main(void) {
    int fds[2];
    if (pipe(fds) != 0) { printf("pipe: FAIL to create\n"); return 1; }
    printf("pipe: fds[0]=%d (read) fds[1]=%d (write)\n", fds[0], fds[1]);

    int pid = fork();
    if (pid == 0) {
        const char* msg = "hello-through-pipe";
        write(fds[1], msg, 18);
        exit(0);
    }

    int rd = dup2(fds[0], 9);            /* redirect the read end to fd 9 */
    printf("pipe: dup2(fds[0], 9)=%d\n", rd);

    char buf[64];
    int n = (int)read(9, buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
    printf("pipe: parent read %d bytes via fd 9: '%s'\n", n, buf);

    int status = 0;
    waitpid(pid, &status);
    return 0;
}
