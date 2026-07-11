/* =============================================================================
 * posixtest.c — exercise the broader POSIX syscall surface (M36 stage 1).
 *
 * Uses uname / stat / getdents / clock_gettime / nanosleep from ring 3 — the
 * kind of surface a real libc (musl, §M36 stage 2) sits on.
 * ============================================================================= */

#include "libc.h"

int main(void) {
    struct utsname u;
    if (uname(&u) == 0)
        printf("posix: uname: %s %s (%s) [%s]\n", u.sysname, u.release, u.version, u.machine);

    struct stat st;
    if (stat("/bin/args", &st) == 0)
        printf("posix: stat /bin/args -> size=%u type=%d mode=%o\n", st.size, st.type, st.mode);
    else
        printf("posix: stat /bin/args FAILED\n");

    int fd = open("/bin", 0);
    if (fd >= 0) {
        char buf[512];
        long n = getdents(fd, buf, sizeof(buf));
        printf("posix: /bin listing (%d bytes):\n", (int)n);
        long o = 0;
        while (o + 3 <= n) {
            unsigned reclen = (unsigned char)buf[o] | ((unsigned char)buf[o + 1] << 8);
            int         type = (unsigned char)buf[o + 2];
            const char* name = &buf[o + 3];
            if (reclen == 0) break;
            printf("  [t%d] %s\n", type, name);
            o += reclen;
        }
        close(fd);
    } else {
        printf("posix: open /bin FAILED\n");
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    printf("posix: realtime epoch = %u s\n", ts.sec);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("posix: monotonic uptime = %u.%u s\n", ts.sec, ts.nsec / 1000000);

    printf("posix: nanosleep 50 ms...\n");
    nanosleep_ms(50);
    printf("posix: done\n");
    return 0;
}
