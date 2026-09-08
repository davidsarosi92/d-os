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
#include "vfs.h"
#include "kmalloc.h"
#include <stdint.h>
#include <stddef.h>

static void cmd_ls(const char* path) {
    if (!path || !*path) path = "/";
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { kprintf("ls: %s: not found\n", path); return; }

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
    if (!f) { kprintf("cat: %s: not found\n", path); return; }

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
    if (!args || !*args) { console_write("mount: missing args\n"); return; }
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

SHELL_CMD(ls)    = { "ls",    "[path]",            "list a directory",        SHELL_G_FS, fs_ls };
SHELL_CMD(cat)   = { "cat",   "<path>",            "print a file",            SHELL_G_FS, cmd_cat };
SHELL_CMD(mkdir) = { "mkdir", "<path>",            "create a directory",      SHELL_G_FS, cmd_mkdir };
SHELL_CMD(touch) = { "touch", "<path>",            "create an empty file",    SHELL_G_FS, cmd_touch };
SHELL_CMD(write) = { "write", "<path> <text>",     "write text to a file",    SHELL_G_FS, cmd_write };
SHELL_CMD(mount) = { "mount", "<fs> <path> [dev]", "mount a filesystem",      SHELL_G_FS, cmd_mount };
SHELL_CMD(cp)    = { "cp",    "<src> <dst>",       "copy a file",             SHELL_G_FS, cmd_cp };
SHELL_CMD(rm)    = { "rm",    "[-r] <path>",       "remove a file or a tree", SHELL_G_FS, cmd_rm };
SHELL_CMD(mv)    = { "mv",    "<src> <dst>",       "rename a file",           SHELL_G_FS, cmd_mv };
