/* =============================================================================
 * forktest.c — exercise fork() + waitpid() (M34 slice B).
 *
 * The parent forks; the child prints its own pid and exits with a known code;
 * the parent waitpid()s and reports the reaped pid + status.  Proves the
 * cloned address space (each side sees its own `secret`), the child resuming
 * at the fork point with fork()==0, and exit-code propagation.
 * ============================================================================= */

#include "libc.h"

int main(void) {
    int secret = 111;                       /* private to each address space */
    printf("fork: parent pid=%d, secret=%d\n", getpid(), secret);

    int pid = fork();
    if (pid == 0) {
        secret = 222;                       /* only the child's copy changes */
        printf("fork: child pid=%d sees fork()=0, sets secret=%d, exit(7)\n",
               getpid(), secret);
        exit(7);
    }

    int status = -1;
    int w = waitpid(pid, &status);
    printf("fork: parent fork()=%d, waitpid=%d status=%d, secret still=%d\n",
           pid, w, status, secret);
    return 0;
}
