/* killmusl.c — §M90: who may signal whom, as on Linux.  Prints
 * "kill: PASS" or the first failing check.
 *   - a SIBLING with the same uid may be killed (the old rule allowed only
 *     the caller's own descendants — `docker rm -f` could not kill a
 *     container whose init had been reparented away from its shim)
 *   - a process that has dropped to another uid (and so lost CAP_KILL) may
 *     NOT kill a root process: EPERM
 *   - kill(pid, 0) probes existence under the same rule */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static int fail(const char* why) { printf("kill: FAIL %s\n", why); return 1; }
static volatile int g_usr1;
static void on_usr1(int s) { (void)s; g_usr1++; }

/* The checks run in a FORKED child: the program itself may be hosted by a
 * kernel task (a shell excursion), which no user program may signal — and
 * the sigsuspend and uid checks signal "the process running the test". */
static int run(void);
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pid_t r = fork();
    if (r == 0) _exit(run());
    int st = 0;
    waitpid(r, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static int run(void) {
    pid_t a = fork();
    if (a == 0) { for (;;) pause(); }
    pid_t b = fork();
    if (b == 0) _exit(kill(a, SIGKILL) == 0 ? 0 : 1);       /* sibling kills sibling */
    int st = 0;
    waitpid(b, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { kill(a, SIGKILL); waitpid(a, NULL, 0); return fail("a same-uid sibling could not be killed"); }
    waitpid(a, &st, 0);
    printf("kill: sibling status %x\n", st);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) return fail("the sibling did not die of SIGKILL");
    printf("kill: a same-uid sibling was killed\n");
    /* and the control: a child that EXITS with 137 is not a signal death */
    pid_t e = fork();
    if (e == 0) _exit(137);
    printf("kill: forked %d, waiting\n", e);
    waitpid(e, &st, 0);
    printf("kill: exit child status %x\n", st);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 137) return fail("exit(137) read as something else");
    printf("kill: exit(137) is WIFEXITED 137, SIGKILL is WIFSIGNALED 9\n");

    /* sigsuspend: SIGUSR1 blocked; a child sends it; sigsuspend with it
     * unblocked runs the handler, and afterwards the mask is the old one */
    {
        struct sigaction sa = { 0 };
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        sigset_t blk, old, empty, now;
        sigemptyset(&blk); sigaddset(&blk, SIGUSR1);
        sigprocmask(SIG_BLOCK, &blk, &old);
        pid_t parent = getpid();
        pid_t k = fork();
        if (k == 0) { kill(parent, SIGUSR1); _exit(0); }
        sigemptyset(&empty);
        int r = sigsuspend(&empty);
        int e = errno;
        sigprocmask(SIG_BLOCK, NULL, &now);
        waitpid(k, NULL, 0);
        if (r != -1 || e != EINTR || g_usr1 != 1) return fail("sigsuspend did not run the handler with EINTR");
        if (!sigismember(&now, SIGUSR1)) return fail("sigsuspend left the suspend mask in place");
        sigprocmask(SIG_SETMASK, &old, NULL);
        printf("kill: sigsuspend ran the handler and restored the mask\n");
    }

    /* waitid: WNOWAIT reports a dead child and LEAVES it (Go's
     * os.Process.Wait peeks this way, then reaps with wait4); the siginfo says
     * CLD_EXITED / CLD_KILLED with the status, si_uid the child's uid. */
    {
        pid_t w = fork();
        if (w == 0) { setuid(77); _exit(5); }
        siginfo_t si;
        memset(&si, 0, sizeof si);
        if (waitid(P_PID, w, &si, WEXITED | WNOWAIT) != 0) return fail("waitid WNOWAIT");
        if (si.si_signo != SIGCHLD || si.si_code != CLD_EXITED || si.si_pid != w ||
            si.si_status != 5 || si.si_uid != 77)
            return fail("waitid siginfo (exit)");
        int st2 = 0;
        if (waitpid(w, &st2, 0) != w || !WIFEXITED(st2) || WEXITSTATUS(st2) != 5)
            return fail("WNOWAIT reaped the child (wait4 after it lost the status)");
        pid_t z = fork();
        if (z == 0) { for (;;) pause(); }
        kill(z, SIGKILL);
        memset(&si, 0, sizeof si);
        if (waitid(P_ALL, 0, &si, WEXITED) != 0) return fail("waitid P_ALL");
        if (si.si_code != CLD_KILLED || si.si_status != SIGKILL || si.si_pid != z)
            return fail("waitid siginfo (killed)");
        memset(&si, 0, sizeof si);
        if (waitid(P_ALL, 0, &si, WEXITED | WNOHANG) != -1 || errno != ECHILD)
            return fail("waitid with no children is not ECHILD");
        printf("kill: waitid WNOWAIT left the child, CLD_EXITED 5 uid 77, CLD_KILLED 9\n");
    }

    pid_t me = getpid();
    pid_t c = fork();
    if (c == 0) {
        if (setuid(4242) != 0) _exit(3);
        if (kill(me, 0) == 0) _exit(1);                      /* must be refused ... */
        _exit(errno == EPERM ? 0 : 2);                       /* ... with EPERM */
    }
    waitpid(c, &st, 0);
    if (!WIFEXITED(st)) return fail("uid child crashed");
    if (WEXITSTATUS(st) == 3) return fail("setuid(4242)");
    if (WEXITSTATUS(st) != 0) return fail("another uid without CAP_KILL could signal root");
    printf("kill: another uid was refused (EPERM)\n");
    printf("kill: PASS\n");
    return 0;
}
