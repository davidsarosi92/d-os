/* =============================================================================
 * sigtest.c — install a signal handler, raise the signal, catch it (M34 slice E).
 *
 * Exercises sigaction (via signal()), kill()/raise(), the kernel's
 * return-to-user delivery, the handler running in ring 3, and SYS_SIGRETURN
 * restoring the interrupted context — all in one process.
 * ============================================================================= */

#include "libc.h"

static volatile int caught = 0;

static void handler(int sig) {
    caught = sig;
    printf("sig: handler ran in ring 3, caught signal %d\n", sig);
}

int main(void) {
    signal(SIGUSR1, handler);
    printf("sig: installed SIGUSR1 handler (pid=%d), raising it...\n", getpid());
    raise(SIGUSR1);
    printf("sig: back after raise, caught=%d (expected %d)\n", caught, SIGUSR1);
    return caught == SIGUSR1 ? 0 : 1;
}
