/* dynhold.c — a DYNAMICALLY linked musl program that starts and then holds
 * (§M74 rung 2).  The page-cache measurement needs two programs that map the
 * same libc.so ALIVE AT THE SAME TIME, so the question "do they share it?" can
 * be asked of the machine rather than inferred.  It touches nothing on
 * purpose: what it holds is exactly what ld.so mapped to start it. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    printf("dynhold: pid %d up, holding %d s\n", (int)getpid(), secs);
    fflush(stdout);
    sleep((unsigned)secs);
    return 0;
}
