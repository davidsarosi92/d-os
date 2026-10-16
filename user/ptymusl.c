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
#include <sys/epoll.h>
#include <pthread.h>
#include <sched.h>

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

/* a close by another thread while this one is blocked on the descriptor */
static int g_epfd, g_pr, g_pw;
static volatile int g_ep_ret = 99, g_rd_ret = 99;
static char g_rdbuf[16];
static void* ep_waiter(void* a) { (void)a; struct epoll_event ev; g_ep_ret = epoll_wait(g_epfd, &ev, 1, 1500); return NULL; }
static void* rd_waiter(void* a) { (void)a; g_rd_ret = (int)read(g_pr, g_rdbuf, sizeof g_rdbuf); return NULL; }

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
        /* the mount is in the reader's own table, with its type */
        char mt[4096]; int mf = open("/proc/mounts", O_RDONLY), mn = mf >= 0 ? (int)read(mf, mt, sizeof mt - 1) : -1;
        if (mf >= 0) close(mf);
        if (mn <= 0) return fail("read /proc/mounts");
        mt[mn] = 0;
        if (!strstr(mt, " /tmp/ptsi devpts ")) { printf("pty: /proc/mounts:\n%s", mt); return fail("/proc/mounts does not list the devpts mount"); }
        umount("/tmp/ptsi");
        mf = open("/proc/mounts", O_RDONLY); mn = mf >= 0 ? (int)read(mf, mt, sizeof mt - 1) : -1;
        if (mf >= 0) close(mf);
        mt[mn > 0 ? mn : 0] = 0;
        if (strstr(mt, "/tmp/ptsi")) return fail("/proc/mounts still lists it after umount");
        printf("pty: devpts instance - ptmx inode 2 (5:2, magic 1cd1), pts/0 inode 3 (136:0), name gone with the master, listed in /proc/mounts until umount\n");
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
    /* a descriptor closed under a call blocked on it: the object lives until
     * the call ends (Linux's fget) — it used to be freed under the waiter */
    {
        int fds[2];
        g_epfd = epoll_create1(0);
        if (g_epfd < 0 || pipe(fds) != 0) return fail("epoll_create1/pipe");
        g_pr = fds[0]; g_pw = fds[1];
        struct epoll_event ev = { EPOLLIN, { 0 } };
        epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_pr, &ev);
        pthread_t a, b;
        pthread_create(&a, NULL, ep_waiter, NULL);
        pthread_create(&b, NULL, rd_waiter, NULL);
        usleep(200000);
        close(g_epfd);                                   /* under the epoll_wait */
        close(g_pr);                                     /* under the read: its ONLY fd */
        for (int i = 0; i < 32; i++) {                   /* churn the heap */
            int e = epoll_create1(0), q[2];
            if (pipe(q) == 0) { close(q[0]); close(q[1]); }
            if (e >= 0) close(e);
        }
        write(g_pw, "late", 4);
        pthread_join(b, NULL);
        pthread_join(a, NULL);
        close(g_pw);
        if (g_rd_ret != 4 || memcmp(g_rdbuf, "late", 4)) { printf("pty: read after close gave %d\n", g_rd_ret); return fail("a read blocked on a pipe whose fd another thread closed"); }
        if (g_ep_ret != 0 && g_ep_ret != 1) { printf("pty: epoll_wait after close gave %d\n", g_ep_ret); return fail("an epoll_wait whose set another thread closed"); }
        printf("pty: a close under a blocked read / epoll_wait - the read got its data, the wait ended (%d)\n", g_ep_ret);
    }
    /* /proc/self/stat read the way runc reads it (skip past the LAST ')',
     * then field 22 = starttime): the fields after 12 sat one slot to the
     * right, so starttime read 0 and runc took an unrecorded init for a
     * recorded one ("invalid state transition from stopped to created"). */
    {
        char sb[1024]; int fd = open("/proc/self/stat", O_RDONLY);
        int n = fd >= 0 ? (int)read(fd, sb, sizeof sb - 1) : -1;
        if (fd >= 0) close(fd);
        if (n <= 0) return fail("read /proc/self/stat");
        sb[n] = 0;
        char* p = strrchr(sb, ')');
        if (!p) return fail("/proc/self/stat has no comm");
        p += 2;                                          /* field 3, the state */
        char st = *p;
        for (int f = 3; f < 22 && p; f++) { p = strchr(p, ' '); if (p) p++; }
        unsigned long long start = p ? strtoull(p, NULL, 10) : 0;
        p = strrchr(sb, ')') + 2;
        for (int f = 3; f < 20 && p; f++) { p = strchr(p, ' '); if (p) p++; }
        long thr = p ? strtol(p, NULL, 10) : 0;
        if (st != 'R' && st != 'S') return fail("/proc/self/stat field 3 is not the state");
        if (start == 0) return fail("/proc/self/stat field 22 (starttime) is 0");
        if (thr < 1) return fail("/proc/self/stat field 20 (num_threads) is not >= 1");
        printf("pty: /proc/self/stat field 3 = %c, 20 = %ld threads, 22 = starttime %llu\n", st, thr, start);
    }
    /* A bind made in ANOTHER mount namespace does not pin the name in this
     * one (Linux's is_local_mountpoint): BuildKit removes the resolv.conf
     * stub runc bound over inside the build container, and got EBUSY.  A
     * bind made in our own namespace still does (the control). */
    {
        mkdir("/tmp", 0755);
        int fs = open("/tmp/bsrc", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int ft = open("/tmp/btgt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fs < 0 || ft < 0) return fail("create /tmp/bsrc /tmp/btgt");
        write(fs, "src\n", 4); write(ft, "tgt\n", 4); close(fs); close(ft);
        int up[2], down[2];
        if (pipe(up) || pipe(down)) return fail("pipe");
        pid_t ch = fork();
        if (ch == 0) {
            char c = 'x';
            if (unshare(CLONE_NEWNS) == 0 && mount("/tmp/bsrc", "/tmp/btgt", NULL, MS_BIND, NULL) == 0) {
                char b[8] = {0}; int fd = open("/tmp/btgt", O_RDONLY);
                if (fd >= 0 && read(fd, b, 4) == 4 && b[0] == 's') c = 'b';
                if (fd >= 0) close(fd);
            }
            write(up[1], &c, 1);
            read(down[0], &c, 1);                        /* hold the namespace */
            _exit(0);
        }
        char c = 0;
        read(up[0], &c, 1);
        if (c != 'b') { write(down[1], "x", 1); return fail("bind inside an unshared mount namespace"); }
        int r = unlink("/tmp/btgt");
        int e = errno;
        write(down[1], "x", 1);
        waitpid(ch, NULL, 0);
        if (r != 0) { errno = e; return fail("unlink of a name bound only in ANOTHER namespace"); }
        ft = open("/tmp/btgt", O_CREAT | O_WRONLY, 0644); if (ft >= 0) close(ft);
        if (mount("/tmp/bsrc", "/tmp/btgt", NULL, MS_BIND, NULL) != 0) return fail("bind in our own namespace");
        r = unlink("/tmp/btgt"); e = errno;
        umount("/tmp/btgt");
        unlink("/tmp/btgt"); unlink("/tmp/bsrc");
        if (r == 0 || e != EBUSY) { printf("pty: own-namespace unlink gave %d errno %d\n", r, e); return fail("unlink of a name bound in OUR namespace must be EBUSY"); }
        printf("pty: a bind in another mount namespace does not pin the name; one in ours is EBUSY\n");
    }
    /* runc 1.5 (pathrs) reads /proc/<pid>/stat by opening it O_PATH and
     * re-opening it through /proc/thread-self/fd/N.  The re-open must stay on
     * THAT process: it answered for the caller, so runc compared its own
     * start time and called every created container "stopped". */
    {
        int sp[2];
        if (pipe(sp)) return fail("pipe");
        pid_t ch = fork();
        if (ch == 0) { char c; read(sp[0], &c, 1); _exit(0); }
        char path[64], via[64], sb[512];
        snprintf(path, sizeof path, "/proc/%d/stat", (int)ch);
        int h = open(path, O_PATH | O_CLOEXEC);
        int ok = 0, got = -1;
        if (h >= 0) {
            snprintf(via, sizeof via, "/proc/thread-self/fd/%d", h);
            int fd = open(via, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                int n = (int)read(fd, sb, sizeof sb - 1);
                if (n > 0) { sb[n] = 0; got = atoi(sb); ok = got == (int)ch; }
                close(fd);
            }
            close(h);
        }
        write(sp[1], "x", 1);
        waitpid(ch, NULL, 0);
        if (!ok) { printf("pty: re-opened /proc/%d/stat answered for pid %d\n", (int)ch, got); return fail("/proc/<pid>/stat re-opened through /proc/thread-self/fd"); }
        printf("pty: /proc/<pid>/stat re-opened through its O_PATH handle stays on pid %d\n", got);
    }
    /* /proc/meminfo the way Prometheus procfs parses it (BuildKit samples it
     * per build step): every line "Name: <number>[ kB]" — one stray line
     * PANICKED dockerd mid-build.  And /proc/stat exists with a "cpu" line. */
    {
        FILE* f = fopen("/proc/meminfo", "r");
        if (!f) return fail("open /proc/meminfo");
        char line[256]; int lines = 0;
        while (fgets(line, sizeof line, f)) {
            char a[96], b[96], c[96], d[96];
            int n = sscanf(line, "%95s %95s %95s %95s", a, b, c, d);
            int num = n >= 2 && b[0];
            for (char* q = b; num && *q; q++) if (*q < '0' || *q > '9') num = 0;
            if (n < 2 || n > 3 || !num || (n == 3 && strcmp(c, "kB")) || a[strlen(a) - 1] != ':') {
                fclose(f);
                printf("pty: /proc/meminfo line not Linux-shaped: %s", line);
                return fail("/proc/meminfo format");
            }
            lines++;
        }
        fclose(f);
        f = fopen("/proc/stat", "r");
        unsigned long long u = 0, ni = 0, sy = 0, id = 0;
        int ok = f && fscanf(f, "cpu %llu %llu %llu %llu", &u, &ni, &sy, &id) == 4;
        if (f) fclose(f);
        if (!ok) return fail("/proc/stat has no \"cpu\" line");
        printf("pty: /proc/meminfo %d Linux-shaped lines; /proc/stat cpu %llu user %llu idle\n", lines, u, id);
    }
    /* TIOCPKT — packet mode on the master (2026-10-16).  Every master read
     * starts with a status byte: 0 + data, or one byte of events.  Each event
     * is provoked from the SLAVE side and must arrive on the master. */
    {
#ifndef TIOCGPKT
#define TIOCGPKT 0x80045438
#endif
        int m, s; char nm[64];
        if (open_pair(&m, &s, nm, sizeof nm, 1) != 0) return fail("pkt: open a pair");
        int one = 1, got = -1;
        if (ioctl(s, TIOCPKT, &one) == 0 || errno != ENOTTY) return fail("pkt: TIOCPKT on a SLAVE must be ENOTTY");
        if (ioctl(m, TIOCPKT, &one) != 0) return fail("pkt: TIOCPKT on");
        if (ioctl(m, TIOCGPKT, &got) != 0 || got != 1) return fail("pkt: TIOCGPKT reads the mode back");
        struct termios t; tcgetattr(s, &t); t.c_oflag &= ~(tcflag_t)OPOST; tcsetattr(s, TCSANOW, &t);
        unsigned char b[32]; int r;
        (void)!write(s, "hi", 2);
        r = (int)read(m, b, sizeof b);
        if (r != 3 || b[0] != 0 || b[1] != 'h' || b[2] != 'i') return fail("pkt: data comes as 0 + bytes");
        tcflow(s, TCOOFF);
        struct pollfd pf = { m, POLLPRI, 0 };
        if (poll(&pf, 1, 500) != 1 || !(pf.revents & POLLPRI)) return fail("pkt: an event makes the master POLLPRI");
        r = (int)read(m, b, sizeof b);
        if (r != 1 || b[0] != 0x04) return fail("pkt: tcflow(TCOOFF) reads as TIOCPKT_STOP");
        tcflow(s, TCOON);
        r = (int)read(m, b, sizeof b);
        if (r != 1 || b[0] != 0x08) return fail("pkt: tcflow(TCOON) reads as TIOCPKT_START");
        tcflush(s, TCIFLUSH);
        r = (int)read(m, b, sizeof b);
        if (r != 1 || b[0] != 0x01) return fail("pkt: tcflush(TCIFLUSH) on the slave reads as FLUSHREAD");
        tcflush(s, TCOFLUSH);
        r = (int)read(m, b, sizeof b);
        if (r != 1 || b[0] != 0x02) return fail("pkt: tcflush(TCOFLUSH) on the slave reads as FLUSHWRITE");
        t.c_iflag &= ~(tcflag_t)IXON; tcsetattr(s, TCSANOW, &t);
        r = (int)read(m, b, sizeof b);
        if (r != 1 || b[0] != 0x10) return fail("pkt: IXON off reads as TIOCPKT_NOSTOP");
        t.c_iflag |= IXON; tcsetattr(s, TCSANOW, &t);
        r = (int)read(m, b, sizeof b);
        if (r != 1 || b[0] != 0x20) return fail("pkt: IXON on (^S/^Q) reads as TIOCPKT_DOSTOP");
        int zero = 0;
        if (ioctl(m, TIOCPKT, &zero) != 0) return fail("pkt: TIOCPKT off");
        (void)!write(s, "ok", 2);
        r = (int)read(m, b, sizeof b);
        if (r != 2 || b[0] != 'o') return fail("pkt: off again, data comes plain");
        close(s); close(m);
        printf("pty: TIOCPKT data/STOP/START/FLUSHREAD/FLUSHWRITE/NOSTOP/DOSTOP and POLLPRI all as Linux\n");
    }
    printf("pty: PASS\n");
    return 0;
}
