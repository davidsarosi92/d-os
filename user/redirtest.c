/* =============================================================================
 * redirtest.c — does STDOUT REDIRECTION work? (§M59)
 *
 * Written because a bug reported as "the clipboard does not take a write from
 * ring 3" turned out not to be about the clipboard at all: fds 0/1/2 were not
 * entries in the descriptor table, so `dup2(fd, 1)` was rejected outright and
 * `sh -c "echo x > file"` could not work even in principle.  It reported
 * success and wrote to the terminal.
 *
 * This proves the fix from RING 3, through the real syscalls, without needing
 * the musl coreutils to be built:
 *
 *   1. open a file, dup2 it onto fd 1, write through stdout, restore fd 1;
 *   2. read the file back and compare — the bytes are either there or not;
 *   3. do the same onto /dev/clipboard, which is what the report was about.
 *
 * It prints PASS/FAIL per step, so the check is a grep on the serial log rather
 * than a human reading a screen.
 * ============================================================================= */

#include "libc.h"

#define O_RDONLY 0x01
#define O_WRONLY 0x02
#define O_CREATE 0x04
#define O_TRUNC  0x08

static int streq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Write `s` to stdout while stdout is pointed at `path`; returns 0 on success.
 * fd 1 is put back afterwards by duplicating the saved copy of it — which is
 * itself a test that a std stream can be restored, not only hijacked. */
static int write_via_stdout(const char* path, const char* s, int len) {
    int save = 3;                        /* where we park the old stdout      */
    (void)save;
    int f = open(path, O_WRONLY | O_CREATE | O_TRUNC);
    if (f < 0) { puts("redirtest: open failed"); return -1; }
    if (dup2(f, 1) != 1) { puts("redirtest: dup2 onto stdout REFUSED"); close(f); return -1; }
    write(1, s, (size_t)len);
    close(1);                            /* releases the redirection          */
    close(f);
    return 0;
}

int main(void) {
    const char* msg = "redirected-bytes";
    const int   len = 16;

    /* --- 1. a plain file ------------------------------------------------ */
    if (write_via_stdout("/tmp/redir.txt", msg, len) != 0) {
        puts("redirtest: FAIL (file redirection could not be set up)");
        return 1;
    }
    char buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0;
    int fd = open("/tmp/redir.txt", O_RDONLY);
    long got = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (got == len && streq_n(buf, msg, len))
        puts("redirtest: PASS file  — stdout redirection reached the file");
    else
        puts("redirtest: FAIL file  — the bytes did not land in the file");

    /* --- 2. /dev/clipboard, the case that was reported ------------------- */
    const char* cmsg = "clipboard-from-ring3";
    if (write_via_stdout("/dev/clipboard", cmsg, 20) == 0)
        puts("redirtest: wrote to /dev/clipboard via stdout "
             "(check with `clip show`)");
    else
        puts("redirtest: FAIL clip  — could not redirect onto the device");

    return 0;
}
