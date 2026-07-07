/* =============================================================================
 * serial_shell.c — interactive PL011 serial shell for the AArch64 port
 * (M21 Phase D).
 *
 * The x86 shell (kernel/core/shell.c) reads its input from a framebuffer-
 * backed VC (vc_getchar) and its command set is welded to subsystems that are
 * themselves x86-specific or not-yet-ported on ARM (the GUI compositor, the
 * ring-3 usermode path, vmm.c, the block/USB drivers).  Reaching THAT shell
 * verbatim needs the framebuffer + VC + driver ports — several later phases.
 *
 * So Phase D brings up a genuine interactive REPL over the UART instead: it
 * runs as an ordinary scheduler task, reads lines from the PL011 (polling +
 * task_yield, so the timer keeps preempting underneath), and drives the
 * PORTABLE kernel services already up on ARM — the PMM, the scheduler, and
 * the VFS/ramfs — with a core command set (help, meminfo, ps, uptime, ls,
 * cat, mkdir, write, rm, echo, clear).  This proves an interactive shell +
 * a real in-memory filesystem on ARM64; growing it into the full shell.c is
 * gated on the framebuffer/driver ports.
 * ============================================================================= */

#include "printf.h"
#include "pmm.h"
#include "task.h"
#include "vfs.h"
#include "block.h"
#include <stdint.h>
#include <stddef.h>

int      uart_early_getchar(void);   /* uart.c — non-blocking RX             */
void     uart_early_putc(char c);
uint64_t timer_ticks_ms(void);       /* timer.c                              */

/* ---- tiny string helpers (no libc) ----------------------------------------- */
static size_t s_len(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static int s_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Split `line` in place into a command token + argument tail (leading spaces
 * trimmed).  Returns the command; *args points at the remainder (or ""). */
static char* split_cmd(char* line, char** args) {
    char* p = line;
    while (*p == ' ') p++;
    char* cmd = p;
    while (*p && *p != ' ') p++;
    if (*p) { *p++ = 0; while (*p == ' ') p++; }
    *args = p;
    return cmd;
}

/* ---- line editor ----------------------------------------------------------- */
#define LINE_MAX 128

static void read_line(char* buf, int cap) {
    int n = 0;
    for (;;) {
        int c = uart_early_getchar();
        if (c < 0) { task_yield(); continue; }     /* idle: let others run   */

        if (c == '\r' || c == '\n') {
            kprintf("\n");
            buf[n] = 0;
            return;
        }
        if (c == 0x7f || c == 0x08) {              /* DEL / Backspace        */
            if (n > 0) { n--; kprintf("\b \b"); }
            continue;
        }
        if (c >= 32 && c < 127 && n < cap - 1) {
            buf[n++] = (char)c;
            uart_early_putc((char)c);              /* echo                   */
        }
    }
}

/* ---- commands -------------------------------------------------------------- */

static void cmd_help(void) {
    kprintf("d-os AArch64 serial shell (M21 Phase D) — commands:\n"
            "  help              this list\n"
            "  echo <text>       print text\n"
            "  meminfo           physical memory summary\n"
            "  uptime            milliseconds since boot\n"
            "  ps                list tasks\n"
            "  ls [path]         list a directory (default /)\n"
            "  cat <path>        print a file\n"
            "  mkdir <path>      create a directory\n"
            "  write <path> <t>  create <path> and write <t>\n"
            "  rm <path>         remove a file\n"
            "  blk [lba]         hexdump a sector of /dev/vda\n"
            "  clear             clear the screen\n");
}

static void cmd_meminfo(void) {
    uint32_t managed = pmm_managed_frames();
    uint32_t freef   = pmm_free_frames();
    uint32_t used    = managed - freef;
    kprintf("memory: managed %u KiB, used %u KiB, free %u KiB (frame=%u B)\n",
            (unsigned)(managed * (PMM_FRAME_SIZE / 1024)),
            (unsigned)(used    * (PMM_FRAME_SIZE / 1024)),
            (unsigned)(freef   * (PMM_FRAME_SIZE / 1024)),
            (unsigned)PMM_FRAME_SIZE);
}

static const char* state_name(enum task_state s) {
    switch (s) {
        case TASK_RUNNABLE: return "RUN ";
        case TASK_SLEEPING: return "SLP ";
        case TASK_DEAD:     return "DEAD";
        default:            return "????";
    }
}

static void ps_cb(const struct task* t, int is_current, void* ctx) {
    (void)ctx;
    kprintf("  %s pid=%d ppid=%d %s cpu_ms=%u %s\n",
            is_current ? "*" : " ", t->pid, t->ppid,
            state_name(t->state), (unsigned)t->cpu_ms, t->name);
}

static void cmd_ps(void) {
    kprintf("tasks (%d):\n", task_count());
    task_for_each(ps_cb, NULL);
}

static void cmd_ls(const char* path) {
    if (!*path) path = "/";
    struct file* d = vfs_open(path, VFS_RDONLY);
    if (!d) { kprintf("ls: cannot open '%s'\n", path); return; }
    struct dirent de;
    int any = 0;
    /* vfs_readdir returns 1 per entry, 0 at end-of-directory, <0 on error. */
    while (vfs_readdir(d, &de) > 0) {
        kprintf("  %s%s\n", de.name, de.type == INODE_DIR ? "/" : "");
        any = 1;
    }
    if (!any) kprintf("  (empty)\n");
    vfs_close(d);
}

static void cmd_cat(const char* path) {
    if (!*path) { kprintf("usage: cat <path>\n"); return; }
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) { kprintf("cat: cannot open '%s'\n", path); return; }
    char buf[128];
    ssize_t got;
    while ((got = vfs_read(f, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < got; i++) uart_early_putc(buf[i]);
    }
    kprintf("\n");
    vfs_close(f);
}

static void cmd_mkdir(const char* path) {
    if (!*path) { kprintf("usage: mkdir <path>\n"); return; }
    if (vfs_mkdir(path) == 0) kprintf("created '%s'\n", path);
    else                      kprintf("mkdir: failed '%s'\n", path);
}

static void cmd_write(char* args) {
    /* args = "<path> <text...>" */
    char* text;
    char* path = split_cmd(args, &text);
    if (!*path) { kprintf("usage: write <path> <text>\n"); return; }
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) { kprintf("write: cannot create '%s'\n", path); return; }
    size_t len = s_len(text);
    if (len) vfs_write(f, text, len);
    vfs_write(f, "\n", 1);
    vfs_close(f);
    kprintf("wrote %u bytes to '%s'\n", (unsigned)(len + 1), path);
}

static void cmd_rm(const char* path) {
    if (!*path) { kprintf("usage: rm <path>\n"); return; }
    if (vfs_unlink(path) == 0) kprintf("removed '%s'\n", path);
    else                       kprintf("rm: failed '%s'\n", path);
}

/* Parse a small non-negative decimal; returns 0 for empty/invalid. */
static uint64_t parse_u64(const char* s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    return v;
}

/* kprintf() supports no field width, so emit two-digit hex by hand. */
static void put_hex8(uint8_t v) {
    static const char d[] = "0123456789abcdef";
    uart_early_putc(d[(v >> 4) & 0xf]);
    uart_early_putc(d[v & 0xf]);
}

/* Read one sector from /dev/vda and hexdump the first 64 bytes — proof the
 * virtio-mmio block driver does real disk I/O (Phase F). */
static void cmd_blk(const char* args) {
    struct block_device* dev = blk_find("vda");
    if (!dev) { kprintf("blk: no /dev/vda (attach a virtio-blk-device)\n"); return; }

    uint64_t lba = parse_u64(args);
    static uint8_t sec[512];
    if (dev->read(dev, lba, 1, sec) != 0) { kprintf("blk: read LBA %u failed\n", (unsigned)lba); return; }

    kprintf("vda LBA %u (%u sectors total), first 64 bytes:\n",
            (unsigned)lba, (unsigned)dev->sector_count);
    for (int row = 0; row < 4; row++) {
        uart_early_putc(' '); uart_early_putc(' ');
        put_hex8((uint8_t)(row * 16)); uart_early_putc(':');
        for (int i = 0; i < 16; i++) { uart_early_putc(' '); put_hex8(sec[row * 16 + i]); }
        uart_early_putc(' '); uart_early_putc(' ');
        for (int i = 0; i < 16; i++) {
            uint8_t c = sec[row * 16 + i];
            uart_early_putc((c >= 32 && c < 127) ? (char)c : '.');
        }
        uart_early_putc('\n');
    }
}

/* ---- REPL ------------------------------------------------------------------ */

void serial_shell_entry(void) {
    kprintf("\nWelcome to d-os on AArch64.  Type 'help'.\n");

    char line[LINE_MAX];
    for (;;) {
        kprintf("d-os> ");
        read_line(line, LINE_MAX);

        char* args;
        char* cmd = split_cmd(line, &args);
        if (!*cmd)                    continue;
        else if (s_eq(cmd, "help"))   cmd_help();
        else if (s_eq(cmd, "echo"))   kprintf("%s\n", args);
        else if (s_eq(cmd, "meminfo"))cmd_meminfo();
        else if (s_eq(cmd, "free"))   cmd_meminfo();
        else if (s_eq(cmd, "uptime")) kprintf("up %u ms\n", (unsigned)timer_ticks_ms());
        else if (s_eq(cmd, "ps"))     cmd_ps();
        else if (s_eq(cmd, "ls"))     cmd_ls(args);
        else if (s_eq(cmd, "cat"))    cmd_cat(args);
        else if (s_eq(cmd, "mkdir"))  cmd_mkdir(args);
        else if (s_eq(cmd, "write"))  cmd_write(args);
        else if (s_eq(cmd, "rm"))     cmd_rm(args);
        else if (s_eq(cmd, "blk"))    cmd_blk(args);
        else if (s_eq(cmd, "clear"))  kprintf("\033[2J\033[H");
        else kprintf("unknown command '%s' (try 'help')\n", cmd);
    }
}
