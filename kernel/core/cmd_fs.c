/* =============================================================================
 * cmd_fs.c — filesystem shell commands (§M70).
 *
 * Split out of shell.c, which had grown to 4467 lines carrying ~80 command
 * bodies whose only common property was that somebody had typed them.  These
 * nine share a subject, so they share a file.
 *
 * Argument parsing is deliberately tiny: each takes the raw tail after its
 * verb and splits it itself.  A general argv splitter is a feature nothing
 * here has asked for, and writing one now would be a second string convention
 * for the next reader to learn.
 * =========================================================================== */

#include "shellcmd.h"
#include "cmd_util.h"
#include "console.h"
#include "printf.h"
#include "block.h"     /* §M75 — blkstat */
#include "vfs.h"
#include "users.h"   /* §M32 — chown resolves a NAME to a uid */
#include "kmalloc.h"
#include <stdint.h>
#include <stddef.h>

/* Why did an open fail?  "not found" for a file that exists and was REFUSED
 * sent the reader looking for a missing file (§M32, 2026-09-27: `cat` of
 * another user's 0600 file printed the VFS's refusal and then "not found"). */
static const char* open_failure(const char* path) {
    struct vfs_stat st;
    return vfs_stat(path, &st) == 0 ? "permission denied" : "not found";
}

static void cmd_ls(const char* path) {
    if (!path || !*path) path = "/";
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { kprintf("ls: %s: %s\n", path, open_failure(path)); return; }

    struct dirent de;
    int n;
    while ((n = vfs_readdir(f, &de)) > 0) {
        const char* tag = (de.type == INODE_DIR) ? "/" : "";
        /* size is uint64_t; our kprintf doesn't speak %llu so truncate
         * for display — files >4 GiB will misprint until printf grows. */
        kprintf("  %s%s  (%u bytes)\n", de.name, tag, (unsigned)de.size);
    }
    if (n < 0) kprintf("ls: readdir failed\n");
    vfs_close(f);
}

static void cmd_cat(const char* path) {
    if (!path || !*path) { console_write("cat: missing path\n"); return; }
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { kprintf("cat: %s: %s\n", path, open_failure(path)); return; }

    char buf[128];
    ssize_t got;
    while ((got = vfs_read(f, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < got; i++) console_putchar(buf[i]);
    }
    /* Make sure the prompt lands on a fresh line even if the file
     * doesn't end with one. */
    console_putchar('\n');
    vfs_close(f);
}

static void cmd_mkdir(const char* path) {
    if (!path || !*path) { console_write("mkdir: missing path\n"); return; }
    int r = vfs_mkdir(path);
    if (r != 0) kprintf("mkdir: %s: failed (%d)\n", path, r);
}

static void cmd_touch(const char* path) {
    if (!path || !*path) { console_write("touch: missing path\n"); return; }
    int r = vfs_create(path);
    if (r != 0) kprintf("touch: %s: failed (%d)\n", path, r);
}

/* `mount <fs> <path> [dev]` — calls vfs_mount with the given fs name,
 * mountpoint path, and optional backing block device (e.g. "vda").
 * Useful for `mount exfat /mnt vda` once exFAT lands; for in-memory
 * filesystems the `dev` argument is omitted. */
static void cmd_mount(const char* args) {
    /* §M32 stage 5 — WITH NO ARGUMENTS, LIST WHAT IS MOUNTED AND SAY WHETHER
     * ITS PERMISSIONS ARE REAL.
     *
     * This is not a convenience.  The ownership model is enforced from RAM,
     * and only a filesystem that STORES it keeps it across a reboot: ramfs,
     * devfs and procfs are volatile; exFAT stores owner and mode since
     * 2026-09-27 in a d-os Vendor Extension entry inside each file's own entry
     * set (exfat.c).  Another operating system reading the volume ignores
     * that entry — which is true of every filesystem's permissions.
     *
     * *That is a fact about the machine, so it belongs ON the machine and not
     * only in a design document* — the §M33 honesty gate applied to storage.
     * `mount` used to refuse an empty argument list; now the empty form is the
     * one that answers the question. */
    if (!args || !*args) {
        int n = vfs_mount_count();
        if (n == 0) { console_write("mount: nothing mounted\n"); return; }
        console_write("PATH  FS  OWNERSHIP\n");
        int any_advisory = 0, any_stored = 0;
        for (int i = 0; i < n; i++) {
            const struct vfs_mount* m = vfs_mount_at(i);
            if (!m) continue;
            kprintf("%s   %s   %s\n", m->path, m->fs_name,
                    m->stores_ownership ? "STORED on the volume (survives a reboot)"
                                        : "ADVISORY — not stored, lost at power-off");
            if (!m->stores_ownership) any_advisory = 1;
            else                      any_stored = 1;
        }
        if (any_advisory)
            console_write("mount: permissions on an ADVISORY volume are enforced by "
                          "this kernel while it runs and are invisible to every "
                          "other system.\n");
        if (any_stored) console_write("mount: a STORED volume keeps owner and mode on disk; other "
                      "operating systems do not enforce them (no filesystem's "
                      "permissions survive being read by a system that ignores them).\n");
        return;
    }
    char fs[32];   int fi = 0;
    char path[64]; int pi = 0;
    char dev[32];  int di = 0;
    const char* p = args;
    /* fs name */
    while (*p && *p != ' ' && fi < (int)sizeof fs - 1) fs[fi++] = *p++;
    fs[fi] = 0;
    while (*p == ' ') p++;
    if (!*p) { console_write("mount: missing path\n"); return; }
    /* mountpoint */
    while (*p && *p != ' ' && pi < (int)sizeof path - 1) path[pi++] = *p++;
    path[pi] = 0;
    while (*p == ' ') p++;
    /* optional dev */
    while (*p && *p != ' ' && di < (int)sizeof dev - 1) dev[di++] = *p++;
    dev[di] = 0;

    int r = vfs_mount(fs, path, di ? dev : NULL);
    if (r != 0) kprintf("mount: failed (%d)\n", r);
}

/* `write <path> <text>` — writes `text` to `path`, creating the file
 * if necessary.  Wonky parsing: we trust the caller to provide exactly
 * one space between path and text, and we don't yet honor quoting. */
static void cmd_write(const char* args) {
    if (!args || !*args) { console_write("write: missing args\n"); return; }
    /* Split at the first space. */
    const char* p = args;
    while (*p && *p != ' ') p++;
    if (!*p)               { console_write("write: missing text\n"); return; }
    char path[128];
    int  i = 0;
    while (args + i < p && i < (int)sizeof path - 1) { path[i] = args[i]; i++; }
    path[i] = 0;
    const char* text = p + 1;

    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) { kprintf("write: %s: open failed\n", path); return; }

    /* Compute text length manually. */
    size_t n = 0;
    while (text[n]) n++;
    ssize_t w = vfs_write(f, text, n);
    if (w < 0) kprintf("write: failed\n");
    vfs_close(f);
}

/* §M12 completion — `rm [-r] <path>`.  The x86 shell never had one: exFAT
 * could not delete (`.unlink = NULL`) and nobody missed it on ramfs.  With
 * unlink/rmdir implemented there is something to remove, and a filesystem you
 * can only add to is not a filesystem you can use. */
/* §M12 — `mv <old> <new>`, same directory.  Added with exFAT's rename for the
 * reason `rm` was added with its unlink: a filesystem operation with no way to
 * invoke it is a filesystem operation nobody can test, and the file manager's
 * Rename button is not reachable from a machine with no display. */
static void cmd_mv(const char* args) {
    char oldp[192], newp[192];
    int n = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && n < (int)sizeof oldp - 1) oldp[n++] = *args++;
    oldp[n] = '\0';
    while (*args == ' ') args++;
    n = 0;
    while (*args && *args != ' ' && n < (int)sizeof newp - 1) newp[n++] = *args++;
    newp[n] = '\0';
    if (!oldp[0] || !newp[0]) { kprintf("usage: mv <old> <new>\n"); return; }

    int rc = vfs_rename(oldp, newp);
    if (rc == 0)       kprintf("renamed %s -> %s\n", oldp, newp);
    else if (rc == -2) kprintf("mv: %s already exists\n", newp);
    else if (rc == -5) kprintf("mv: name too long for this filesystem\n");
    else if (rc == -3) kprintf("mv: the directory is full\n");
    else               kprintf("mv: failed (%d) — same directory only?\n", rc);
}

/* §M67 — `cp <src> <dst>`.
 *
 * Added here because the shell could `rm` and could not `cp`, which §4.73 had
 * already argued about deletion: a filesystem you can only add to is not one
 * you can use.  The immediate need is a module: `insmod` reads a FILE, and
 * putting that file on the persistent volume is how a module survives a reboot
 * — but with no copy command there was no way to move one anywhere at all.
 *
 * vfs_copy already existed for the file manager's Copy button; this is the same
 * call reachable without a mouse, which is also what makes it testable. */
static void cmd_cp(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { console_write("cp: usage: cp <src> <dst>\n"); return; }

    char src[128];
    int n = 0;
    while (args[n] && args[n] != ' ' && n < (int)sizeof src - 1) { src[n] = args[n]; n++; }
    src[n] = '\0';
    const char* dst = args + n;
    while (*dst == ' ') dst++;
    if (!*dst) { console_write("cp: usage: cp <src> <dst>\n"); return; }

    if (vfs_copy(src, dst) == 0) kprintf("copied %s -> %s\n", src, dst);
    else                         kprintf("cp: cannot copy %s to %s\n", src, dst);
}

static void cmd_rm(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { console_write("rm: usage: rm [-r] <path>\n"); return; }

    int recursive = 0;
    if (args[0] == '-' && args[1] == 'r') {
        recursive = 1;
        args += 2;
        while (*args == ' ') args++;
        if (!*args) { console_write("rm: usage: rm [-r] <path>\n"); return; }
    }

    int rc = recursive ? vfs_unlink_recursive(args) : vfs_unlink(args);
    if (rc == 0)       kprintf("removed %s\n", args);
    else if (rc == -2) kprintf("rm: %s is not empty (use -r)\n", args);
    else               kprintf("rm: cannot remove %s\n", args);
}

/* --- registrations ---------------------------------------------------------
 * `ls` with no argument means `/`.  That default lives HERE and not in the
 * dispatcher: the dispatcher's job is to find the verb, and a default argument
 * is a property of the command. */
static void fs_ls(const char* a) { cmd_ls(a[0] ? a : "/"); }

SHELL_CMD(ls)    = { "ls",    "[path]",            "list a directory",        SHELL_G_FS, fs_ls, SHELL_P_ANY };
SHELL_CMD(cat)   = { "cat",   "<path>",            "print a file",            SHELL_G_FS, cmd_cat, SHELL_P_ANY };
SHELL_CMD(mkdir) = { "mkdir", "<path>",            "create a directory",      SHELL_G_FS, cmd_mkdir, SHELL_P_ANY };
SHELL_CMD(touch) = { "touch", "<path>",            "create an empty file",    SHELL_G_FS, cmd_touch, SHELL_P_ANY };
SHELL_CMD(write) = { "write", "<path> <text>",     "write text to a file",    SHELL_G_FS, cmd_write, SHELL_P_ANY };
SHELL_CMD(mount) = { "mount", "<fs> <path> [dev]", "mount a filesystem",      SHELL_G_FS, cmd_mount, SHELL_P_ADMIN };
SHELL_CMD(cp)    = { "cp",    "<src> <dst>",       "copy a file",             SHELL_G_FS, cmd_cp, SHELL_P_ANY };
SHELL_CMD(rm)    = { "rm",    "[-r] <path>",       "remove a file or a tree", SHELL_G_FS, cmd_rm, SHELL_P_ANY };
SHELL_CMD(mv)    = { "mv",    "<src> <dst>",       "rename a file",           SHELL_G_FS, cmd_mv, SHELL_P_ANY };

/* ---------------------------------------------------------------------------
 * §M75 — `blkstat`: the I/O counters, and the only way to check them headlessly.
 *
 * The Task Manager's I/O chart is drawn from these, and a chart is the one
 * surface on which a broken counter still looks like a plausible measurement:
 * a flat line reads as "the disk is idle", which is exactly what a counter
 * that never increments produces.  So the numbers get a text surface too, and
 * the test is a DIFFERENCE across a known amount of work — the same shape as
 * §M75's memory column, for the same reason. */
static void cmd_blkstat(const char* a) {
    (void)a;
    struct blk_stats s;
    blk_get_stats(&s);
    kprintf("blkstat: %u reads (%u sectors), %u writes (%u sectors), "
            "%u flushes, %u errors\n",
            (unsigned)s.reads, (unsigned)s.sectors_read,
            (unsigned)s.writes, (unsigned)s.sectors_written,
            (unsigned)s.flushes, (unsigned)s.errors);
}

SHELL_CMD(blkstat) = { "blkstat", "", "block-layer I/O counters since boot",
                       SHELL_G_FS, cmd_blkstat, SHELL_P_ANY };

/* ---------------------------------------------------------------------------
 * §M32 stage 5 — chmod / chown.
 *
 * The mode is typed in OCTAL, which is how every user of these commands has
 * thought about it for fifty years, and it is parsed as octal rather than
 * being "helpfully" accepted in decimal: `chmod 644` meaning 0o644 and
 * `chmod 644` meaning 644 decimal (0o1204) differ in every bit that matters,
 * and the second silently grants setuid-shaped bits nobody asked for.
 * ------------------------------------------------------------------------- */

static void cmd_chmod(const char* args) {
    char path[128]; int pi = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && pi < (int)sizeof path - 1) path[pi++] = *args++;
    path[pi] = 0;
    while (*args == ' ') args++;
    if (!pi || !*args) { console_write("usage: chmod <path> <octal-mode>\n"); return; }

    uint32_t mode = 0;
    int digits = 0;
    for (; *args >= '0' && *args <= '7'; args++) { mode = mode * 8 + (uint32_t)(*args - '0'); digits++; }
    if (!digits || digits > 4) {
        console_write("chmod: the mode is one to four OCTAL digits (e.g. 644, 700)\n");
        return;
    }
    int r = vfs_chmod(path, mode);
    if (r == -1)      kprintf("chmod: %s: no such path\n", path);
    else if (r == -2) kprintf("chmod: %s: only the owner or an administrator may change the mode\n", path);
    else if (r == -3) kprintf("chmod: %s: the volume did not take the change - left as it was\n", path);
    else              kprintf("chmod: %s is now %d%d%d%d\n", path,
                              (int)((mode >> 9) & 7), (int)((mode >> 6) & 7),
                              (int)((mode >> 3) & 7), (int)(mode & 7));
}

static void cmd_chown(const char* args) {
    char path[128]; int pi = 0;
    char who[64];   int wi = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && pi < (int)sizeof path - 1) path[pi++] = *args++;
    path[pi] = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && wi < (int)sizeof who - 1) who[wi++] = *args++;
    who[wi] = 0;
    if (!pi || !wi) { console_write("usage: chown <path> <user>\n"); return; }

    const struct user_account* u = user_by_name(who);
    if (!u) { kprintf("chown: no such account '%s'\n", who); return; }
    int r = vfs_chown(path, u->uid, u->gid);
    if (r == -1)      kprintf("chown: %s: no such path\n", path);
    else if (r == -2) console_write("chown: only an administrator may give a file away\n");
    else if (r == -3) kprintf("chown: %s: the volume did not take the change - left as it was\n", path);
    else              kprintf("chown: %s now belongs to %s (uid %d)\n", path, u->name, u->uid);
}

/* §M32 — `stat <path>`: owner, group, mode, size, and whether the volume
 * keeps them.  The test for on-disk ownership reads it after a reboot. */
static void cmd_stat(const char* args) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { console_write("usage: stat <path>\n"); return; }
    struct vfs_stat st;
    if (vfs_stat(args, &st) != 0) { kprintf("stat: %s: no such path\n", args); return; }
    const struct user_account* u = user_by_uid(st.uid);
    const struct vfs_mount* m = vfs_mount_for(args);
    kprintf("%s: %s, owner %s (uid %d) gid %d, mode %d%d%d%d, %u bytes, %s\n",
            args, st.is_dir ? "directory" : "file", u ? u->name : "?", st.uid, st.gid,
            (int)((st.mode >> 9) & 7), (int)((st.mode >> 6) & 7),
            (int)((st.mode >> 3) & 7), (int)(st.mode & 7), (unsigned)st.size,
            (m && m->stores_ownership) ? "stored on the volume" : "not stored (lost at power-off)");
}
SHELL_CMD(stat)  = { "stat", "<path>", "owner, mode and size of a file",
                     SHELL_G_FS, cmd_stat, SHELL_P_ANY };
SHELL_CMD(chmod) = { "chmod", "<path> <octal-mode>", "change a file's permission bits",
                     SHELL_G_FS, cmd_chmod, SHELL_P_ANY };
SHELL_CMD(chown) = { "chown", "<path> <user>", "change a file's owner (admin only)",
                     SHELL_G_FS, cmd_chown, SHELL_P_ADMIN };
