/* isigmusl.c — §M90: ^C on a CONSOLE signals the console's foreground process
 * group (tty.c's ISIG).  The program makes its own group the foreground one
 * (tcsetpgrp, as a job-control shell does for its job), waits up to 30 s for a
 * SIGINT, and says which happened.  The harness presses Ctrl+C on the
 * keyboard; without the console's ISIG the key is just a character in the
 * input and the wait times out.  Prints "isig: PASS" or "isig: FAIL ...". */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>

static volatile int g_int;
static void on_int(int s) { (void)s; g_int = 1; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, on_int);
    setpgid(0, 0);
    if (tcsetpgrp(0, getpgrp()) != 0) { printf("isig: FAIL tcsetpgrp\n"); return 1; }
    pid_t fg = tcgetpgrp(0);
    printf("isig: foreground group %d (mine %d) - press Ctrl+C\n", (int)fg, (int)getpgrp());
    for (int i = 0; i < 300 && !g_int; i++) usleep(100000);
    tcsetpgrp(0, 0);
    printf(g_int ? "isig: PASS - ^C reached the foreground group as SIGINT\n"
                 : "isig: FAIL - no SIGINT within 30 s\n");
    return g_int ? 0 : 1;
}
