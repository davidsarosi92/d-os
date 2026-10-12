/* ptymusl.c — §M90 rung 6 falsifier for terminals, run by `ptytest`.
 *
 * Everything through musl's own terminal calls (posix_openpt, unlockpt,
 * ptsname, tcgetattr/tcsetattr, cfmakeraw, tcgetpgrp, ioctl TIOC*), so what is
 * tested is the interface a real program reaches, not the kernel's view of it.
 * Each check can fail, and says which; "pty: PASS" only if all held.
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
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>

static int fail(const char* what) { printf("pty: FAIL - %s (errno %d)\n", what, errno); return 1; }

/* read whatever the master has within `ms`, into buf (NUL-terminated) */
static int drain(int m, char* buf, int cap, int ms) {
    int n = 0;
    for (;;) {
        struct pollfd pf = { m, POLLIN, 0 };
        if (poll(&pf, 1, ms) <= 0) break;
        int r = (int)read(m, buf + n, (size_t)(cap - 1 - n));
        if (r <= 0) break;
        n += r;
        ms = 50;
        if (n >= cap - 1) break;
    }
    buf[n] = 0;
    return n;
}
static long ms_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static volatile int g_int, g_hup, g_winch;
static void on_int(int s)   { (void)s; g_int++; }
static void on_hup(int s)   { (void)s; g_hup++; }
static void on_winch(int s) { (void)s; g_winch++; }

static int open_pair(int* pm, int* ps, char* name, int cap, int noctty) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    char* n = ptsname(m);
    if (!n) return -2;
    snprintf(name, (size_t)cap, "%s", n);
    if (open(name, O_RDWR | O_NOCTTY) >= 0 || errno != EIO) return -3;   /* still locked */
    if (unlockpt(m) != 0) return -4;
    int s = open(name, O_RDWR | (noctty ? O_NOCTTY : 0));
    if (s < 0) return -5;
    *pm = m; *ps = s;
    return 0;
}

int main(void) {
    char name[64], buf[256];
    int m, s;
    int e = open_pair(&m, &s, name, sizeof name, 1);
    if (e == -3) return fail("the slave opened while still locked (unlockpt not needed?)");
    if (e) { printf("pty: open_pair step %d\n", e); return fail("posix_openpt/ptsname/unlockpt/open"); }
    if (strncmp(name, "/dev/pts/", 9) != 0) return fail("ptsname is not /dev/pts/N");
    if (!isatty(s) || !isatty(m)) return fail("isatty on a pty end");
    struct termios t;
    if (tcgetattr(s, &t) != 0) return fail("tcgetattr");
    if (!(t.c_lflag & ICANON) || !(t.c_lflag & ECHO) || !(t.c_oflag & ONLCR) || t.c_cc[VINTR] != 3)
        return fail("the default termios is not Linux's");
    printf("pty: %s opened (locked until unlockpt), isatty, default termios cooked\n", name);

    /* canonical editing + echo: "ab<DEL>c\n" reads as "ac\n" */
    write(m, "ab\x7f" "c\n", 5);
    int r = (int)read(s, buf, sizeof buf);
    if (r != 3 || memcmp(buf, "ac\n", 3)) return fail("canonical line editing (erase)");
    drain(m, buf, sizeof buf, 200);
    if (strcmp(buf, "ab\b \bc\r\n")) { printf("pty: echo was '%s'\n", buf); return fail("echo with ECHOE and ONLCR"); }
    /* output post-processing */
    write(s, "x\n", 2);
    drain(m, buf, sizeof buf, 200);
    if (strcmp(buf, "x\r\n")) return fail("ONLCR on the slave's output");
    /* a line is delivered only when complete */
    write(m, "par", 3);
    struct pollfd pf = { s, POLLIN, 0 };
    if (poll(&pf, 1, 100) != 0) return fail("a partial line made the slave readable");
    write(m, "t\r", 2);                                         /* ICRNL ends it */
    r = (int)read(s, buf, sizeof buf);
    if (r != 5 || memcmp(buf, "part\n", 5)) return fail("ICRNL / the completed line");
    drain(m, buf, sizeof buf, 100);
    /* VEOF on an empty line reads as end of file */
    write(m, "\x04", 1);
    if (read(s, buf, sizeof buf) != 0) return fail("^D on an empty line is not EOF");
    printf("pty: canonical - erase, echo, ONLCR, ICRNL, whole lines, ^D = EOF\n");

    /* raw: no ICRNL, no echo; VMIN 0 / VTIME 2 times out */
    struct termios raw = t;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 2;
    if (tcsetattr(s, TCSANOW, &raw) != 0) return fail("tcsetattr raw");
    write(m, "\r", 1);
    r = (int)read(s, buf, sizeof buf);
    if (r != 1 || buf[0] != '\r') return fail("raw mode changed the byte");
    if (drain(m, buf, sizeof buf, 100) != 0) return fail("raw mode echoed");
    long t0 = ms_now();
    r = (int)read(s, buf, sizeof buf);
    long dt = ms_now() - t0;
    if (r != 0 || dt < 150 || dt > 1500) { printf("pty: VTIME read %d after %ld ms\n", r, dt); return fail("VMIN 0 / VTIME 2"); }
    tcsetattr(s, TCSANOW, &t);
    printf("pty: raw - bytes untouched, no echo, VTIME 2 timed out after %ld ms\n", dt);

    /* window size, and the slave's close seen by the master */
    struct winsize ws = { 33, 101, 0, 0 }, got;
    if (ioctl(m, TIOCSWINSZ, &ws) != 0 || ioctl(s, TIOCGWINSZ, &got) != 0 ||
        got.ws_row != 33 || got.ws_col != 101) return fail("TIOCSWINSZ / TIOCGWINSZ");
    close(s);
    r = (int)read(m, buf, sizeof buf);
    if (r != -1 || errno != EIO) return fail("master read after the slave closed is not EIO");
    close(m);
    printf("pty: window size shared; the slave's close reads EIO on the master\n");

    /* the controlling terminal: a new session acquires it by opening the slave */
    if (open_pair(&m, &s, name, sizeof name, 1)) return fail("second pair");
    pid_t c = fork();
    if (c == 0) {
        close(m); close(s);
        setsid();
        int fd = open(name, O_RDWR);                            /* no O_NOCTTY: acquire */
        if (fd < 0) _exit(10);
        if (tcgetpgrp(fd) != getpid()) _exit(11);
        if (tcgetsid(fd) != getpid()) _exit(12);
        int t2 = open("/dev/tty", O_RDWR);
        if (t2 < 0) _exit(13);
        close(t2);
        struct sigaction sa = { 0 };
        sa.sa_handler = on_int; sigaction(SIGINT, &sa, NULL);
        sa.sa_handler = on_hup; sigaction(SIGHUP, &sa, NULL);
        sa.sa_handler = on_winch; sigaction(SIGWINCH, &sa, NULL);
        signal(SIGTSTP, SIG_IGN);                               /* as a shell does */
        write(fd, "R", 1);                                      /* ready */
        char b[16];
        int rr = (int)read(fd, b, sizeof b);                    /* ^C interrupts this */
        if (!(rr < 0 && errno == EINTR && g_int == 1)) _exit(14);
        write(fd, "I", 1);
        rr = (int)read(fd, b, sizeof b);                        /* ^Z is ignored: this reads a line */
        if (rr != 3 || memcmp(b, "ok\n", 3)) _exit(15);
        write(fd, "Z", 1);
        while (!g_winch) usleep(10000);
        write(fd, "W", 1);
        rr = (int)read(fd, b, sizeof b);                        /* the master closes: EOF */
        if (rr != 0) _exit(16);
        for (int i = 0; i < 100 && !g_hup; i++) usleep(10000);
        _exit(g_hup ? 0 : 17);
    }
    char want[] = "R";
    if (drain(m, buf, sizeof buf, 3000) < 1 || !strchr(buf, want[0])) return fail("child never got ready");
    close(s);                         /* (kept until now: a slave closed by all reads EIO on the master) */
    write(m, "\x03", 1);
    drain(m, buf, sizeof buf, 3000);
    if (!strchr(buf, 'I')) { printf("pty: after ^C the master read '%s'\n", buf); return fail("^C did not interrupt the foreground read"); }
    write(m, "\x1a" "ok\n", 4);
    drain(m, buf, sizeof buf, 3000);
    if (!strchr(buf, 'Z')) return fail("^Z stopped a process that ignores SIGTSTP");
    struct winsize w2 = { 40, 120, 0, 0 };
    ioctl(m, TIOCSWINSZ, &w2);
    drain(m, buf, sizeof buf, 3000);
    if (!strchr(buf, 'W')) return fail("SIGWINCH did not reach the foreground group");
    close(m);                                                   /* hang up */
    int st = 0;
    waitpid(c, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("pty: session child status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st));
        return fail("controlling terminal: acquire / ^C / ^Z / SIGWINCH / hangup");
    }
    printf("pty: ctty acquired, /dev/tty opens, ^C = SIGINT + EINTR, ignored ^Z, SIGWINCH, hangup = EOF + SIGHUP\n");

    /* kill(-pgid): a process group as a target */
    c = fork();
    if (c == 0) { setpgid(0, 0); for (;;) pause(); }
    usleep(100000);
    if (kill(-c, SIGTERM) != 0) return fail("kill(-pgid)");
    waitpid(c, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) return fail("the group was not signalled");
    if (kill(-99999, SIGTERM) != -1 || errno != ESRCH) return fail("kill(-nosuchgroup) is not ESRCH");
    printf("pty: kill(-pgid) reaches the group, ESRCH for none\n");
    /* a devpts mount is a new instance with Linux's fixed numbers: runc
     * verifies a container's ptmx by them before trusting it */
    {
        mkdir("/tmp", 0755);
        mkdir("/tmp/ptsi", 0755);
        if (mount("devpts", "/tmp/ptsi", "devpts", 0, "newinstance,ptmxmode=0666") != 0) return fail("mount devpts");
        int pm = open("/tmp/ptsi/ptmx", O_RDWR | O_NOCTTY);
        if (pm < 0) return fail("open the instance's ptmx");
        struct stat sb; struct statfs sf;
        if (fstat(pm, &sb) != 0 || fstatfs(pm, &sf) != 0) return fail("fstat/fstatfs ptmx");
        if (sb.st_ino != 2 || !S_ISCHR(sb.st_mode) || major(sb.st_rdev) != 5 || minor(sb.st_rdev) != 2 ||
            sf.f_type != 0x1cd1) {
            printf("pty: ptmx ino %lu mode %o rdev %u:%u magic %lx\n", (unsigned long)sb.st_ino,
                   (unsigned)sb.st_mode, major(sb.st_rdev), minor(sb.st_rdev), (unsigned long)sf.f_type);
            return fail("the instance's ptmx is not devpts inode 2, 5:2");
        }
        int idx = -1;
        if (ioctl(pm, TIOCGPTN, &idx) != 0 || idx != 0) return fail("a new instance numbers from 0");
        char sp[64];
        snprintf(sp, sizeof sp, "/tmp/ptsi/%d", idx);
        if (stat(sp, &sb) != 0 || sb.st_ino != (ino_t)(idx + 3) || major(sb.st_rdev) != 136)
            return fail("the slave is not inode N+3, 136:N");
        close(pm);
        if (stat(sp, &sb) == 0) return fail("the slave's name outlived its master");
        umount("/tmp/ptsi");
        printf("pty: devpts instance - ptmx inode 2 (5:2, magic 1cd1), pts/0 inode 3 (136:0), name gone with the master\n");
    }
    /* the console is a terminal too: isatty, a size, raw mode times out */
    {
        if (!isatty(1)) return fail("the console (stdout) is not a terminal");
        struct winsize cw;
        if (ioctl(1, TIOCGWINSZ, &cw) != 0 || cw.ws_row == 0 || cw.ws_col == 0) return fail("the console's size");
        struct termios ct, cr;
        if (tcgetattr(0, &ct) != 0) return fail("tcgetattr on the console");
        cr = ct; cfmakeraw(&cr); cr.c_cc[VMIN] = 0; cr.c_cc[VTIME] = 1;
        if (tcsetattr(0, TCSANOW, &cr) != 0) return fail("raw mode on the console");
        long c0 = ms_now();
        int cn = (int)read(0, buf, sizeof buf);
        long cd = ms_now() - c0;
        tcsetattr(0, TCSANOW, &ct);
        struct termios back;
        tcgetattr(0, &back);
        if (cn != 0 || cd < 50 || !(back.c_lflag & ICANON)) { printf("pty: console raw read %d after %ld ms\n", cn, cd); return fail("the console's raw read / restore"); }
        printf("pty: console - a terminal of %dx%d, raw read timed out after %ld ms, cooked again\n", cw.ws_col, cw.ws_row, cd);
    }
    printf("pty: PASS\n");
    return 0;
}
