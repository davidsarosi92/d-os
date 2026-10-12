/* ptyrun.c — run a command on a fresh pseudo-terminal and type at it (§M90
 * rung 6).  `ptyrun '<keys>' <cmd> [args...]`
 *
 * The command gets a pty slave as stdin/stdout/stderr and as its
 * CONTROLLING terminal (a new session), 24 rows by 80 columns, exactly what a
 * terminal emulator gives a program; ptyrun copies everything it prints to
 * its own stdout and types <keys> into it.  That is how `docker run -it` is
 * driven from a harness that has no terminal of its own: the docker CLI
 * insists on a terminal for -t ("the input device is not a TTY").
 *
 * <keys>: literal characters plus \n \r \t \e \\ and \xHH, ^X for a control
 * character (^C, ^D, ^Z), `~` for a one-second pause (`\~` is a tilde), and
 * `{text}` to wait until the command has printed `text` since the previous
 * wait (a prompt, say) — at most 300 s.
 * After the last key ptyrun keeps copying until the command closes the
 * terminal, then reports its exit status; it gives up after 600 s.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static long ms_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Decode <keys> into bytes, a 0x100 entry meaning "pause one second". */
static char g_waits[32][64];
static int g_nwaits;
static int decode(const char* s, int* out, int cap) {
    int n = 0;
    while (*s && n < cap) {
        if (*s == '~') { out[n++] = 0x100; s++; continue; }
        if (*s == '{' && g_nwaits < 32) {                 /* {text}: wait for output */
            s++;
            int k = 0;
            while (*s && *s != '}' && k < 63) g_waits[g_nwaits][k++] = *s++;
            g_waits[g_nwaits][k] = 0;
            if (*s == '}') s++;
            out[n++] = 0x200 + g_nwaits++;
            continue;
        }
        if (*s == '^' && s[1]) { out[n++] = s[1] == '?' ? 0x7f : (s[1] & 0x1f); s += 2; continue; }
        if (*s == '\\' && s[1]) {
            char c = s[1];
            s += 2;
            if (c == 'n') out[n++] = '\n';
            else if (c == 'r') out[n++] = '\r';
            else if (c == 't') out[n++] = '\t';
            else if (c == 'e') out[n++] = 0x1b;
            else if (c == 'x' && hexv(s[0]) >= 0 && hexv(s[1]) >= 0) { out[n++] = hexv(s[0]) * 16 + hexv(s[1]); s += 2; }
            else out[n++] = (unsigned char)c;
            continue;
        }
        out[n++] = (unsigned char)*s++;
    }
    return n;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: ptyrun '<keys>' <cmd> [args...]\n"); return 2; }
    static int keys[4096];
    int nk = decode(argv[1], keys, 4096);
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || unlockpt(m) != 0) { perror("ptyrun: ptmx"); return 1; }
    char* sn = ptsname(m);
    if (!sn) { perror("ptyrun: ptsname"); return 1; }
    char name[64];
    snprintf(name, sizeof name, "%s", sn);
    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);
    pid_t c = fork();
    if (c < 0) { perror("ptyrun: fork"); return 1; }
    if (c == 0) {
        setsid();
        int s = open(name, O_RDWR);                       /* the controlling terminal */
        if (s < 0) _exit(126);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(m);
        execvp(argv[2], argv + 2);
        fprintf(stderr, "ptyrun: %s: %s\n", argv[2], strerror(errno));
        _exit(127);
    }
    printf("ptyrun: %s on %s (pid %d)\n", argv[2], name, (int)c);
    fflush(stdout);
    int ki = 0;
    long next = ms_now() + 1000;                          /* let it start before typing */
    long deadline = ms_now() + 600000;
    char buf[1024];
    static char seen[8192];                               /* output since the last wait */
    int nseen = 0;
    long wait_until = 0;
    for (;;) {
        long now = ms_now();
        if (now > deadline) { printf("\nptyrun: timed out\n"); kill(c, SIGKILL); break; }
        while (ki < nk && now >= next) {                  /* type what is due */
            if (keys[ki] == 0x100) { next = now + 1000; ki++; break; }
            if (keys[ki] >= 0x200) {                       /* {text} */
                const char* w = g_waits[keys[ki] - 0x200];
                seen[nseen] = 0;
                if (strstr(seen, w)) { ki++; nseen = 0; wait_until = 0; next = now + 200; break; }
                if (!wait_until) wait_until = now + 300000;
                if (now > wait_until) { printf("\nptyrun: never saw '%s'\n", w); ki = nk; }
                break;
            }
            char b = (char)keys[ki++];
            if (write(m, &b, 1) != 1) { ki = nk; break; }
        }
        int wait_ms = ki < nk ? (int)(next > now ? next - now : (keys[ki] >= 0x200 ? 100 : 1)) : 1000;
        struct pollfd pf = { m, POLLIN, 0 };
        int r = poll(&pf, 1, wait_ms);
        if (r > 0) {
            ssize_t n = read(m, buf, sizeof buf);
            if (n > 0) {
                fwrite(buf, 1, (size_t)n, stdout); fflush(stdout);
                for (ssize_t i = 0; i < n; i++) {        /* keep the tail for {text} */
                    if (nseen >= (int)sizeof seen - 1) {
                        memmove(seen, seen + 4096, (size_t)(nseen - 4096)); nseen -= 4096;
                    }
                    seen[nseen++] = buf[i] ? buf[i] : ' ';
                }
                continue;
            }
            if (n == 0 || errno == EIO) break;            /* the command closed its terminal */
            if (errno != EINTR && errno != EAGAIN) break;
        }
        int st;
        if (ki >= nk && waitpid(c, &st, WNOHANG) == c) {  /* gone, and nothing left to read */
            while ((r = (int)read(m, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)r, stdout);
            printf("\nptyrun: exit %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
            return 0;
        }
    }
    int st = 0;
    waitpid(c, &st, 0);
    printf("\nptyrun: exit %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
    return 0;
}
