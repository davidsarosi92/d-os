/* =============================================================================
 * spin.c — a demo user program that proves CONCURRENT preemptible processes
 * (Tier B).  It prints its own pid + a tick counter a few times, burning CPU
 * between lines so the timer preempts it; run two copies with `procspawn` and
 * their lines interleave — two independent ring-3 processes time-sliced by the
 * kernel scheduler, each exiting on its own.
 *
 * Each line is emitted with ONE write(2) so lines stay intact (interleaving is
 * at line granularity, not mid-line).  Links against the in-tree libc.
 * ============================================================================= */

#include "libc.h"

static void put_int(char* b, int* k, int v) {
    char t[12]; int i = 0;
    if (v == 0) { b[(*k)++] = '0'; return; }
    if (v < 0)  { b[(*k)++] = '-'; v = -v; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) b[(*k)++] = t[i];
}

int main(void) {
    int pid = getpid();
    for (int n = 0; n < 6; n++) {
        char line[32]; int k = 0;
        line[k++] = '['; put_int(line, &k, pid); line[k++] = ']'; line[k++] = ' ';
        line[k++] = 't'; line[k++] = 'i'; line[k++] = 'c'; line[k++] = 'k';
        line[k++] = ' '; put_int(line, &k, n); line[k++] = '\n';
        write(1, line, (unsigned)k);                 /* one syscall → atomic line */
        for (volatile long j = 0; j < 6000000; j++) { }  /* burn → timer preempts */
    }
    return 0;
}
