/* =============================================================================
 * shell.c — tiny interactive REPL for d-os.
 *
 * A read-dispatch-print loop on top of the terminal + keyboard drivers.
 * No external dependencies (no libc, no strcmp), so we roll two helpers:
 * `streq` for exact string equality and `starts_with` for prefix match.
 *
 * The built-in command set is deliberately small; every new command added
 * here needs both a dispatch branch and a mention in `cmd_help`.
 * =========================================================================== */

#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "hal.h"
#include "multiboot.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "printf.h"
#include "module.h"
#include "driver.h"
#include "timer.h"
#include "vfs.h"
#include "config.h"
#include "usermode.h"
#include "task.h"
#include "block.h"
#include "block_cache.h"
#include "vc.h"
#include "lock.h"
#include "keymap.h"

#define LINE_MAX        128             /* max accepted bytes per command line */
#define DEFAULT_PROMPT  "d-os> "        /* fallback when config is unavailable */

/* Exact string equality.  Walks until either string diverges or the shorter
 * string ends; the terminator check at the end catches the case where one
 * string is a strict prefix of the other (those are NOT equal). */
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* True iff `s` begins with `p` (including `p` == ""). */
static int starts_with(const char* s, const char* p) {
    while (*p) {
        if (*s != *p) return 0;
        s++; p++;
    }
    return 1;
}

/* Interactive line reader (per-VC).
 *
 * Reads one keypress at a time from the owning VC's input ring, echoes
 * printable characters back into that VC, and returns a NUL-terminated
 * buffer.
 *
 * Buffer safety: we reserve one byte for the terminator, so we only
 * accept up to (cap - 1) input characters.  Input beyond that is silently
 * dropped — the user sees nothing get echoed and learns not to paste
 * novels at the prompt.
 *
 * Echo path: vc_putchar writes directly to this VC's rect, NOT to
 * console_putchar.  That way the echo is visible even when focus has
 * shifted to a different pane (vc_kbd_push targets the focused VC,
 * but our shell's chars still land in our ring because they were
 * pushed while we were focused; the echo follows the same VC). */
static void read_line(struct vc* v, char* buf, int cap) {
    int len = 0;
    for (;;) {
        char c = vc_getchar(v);

        if (c == '\n') {
            vc_putchar(v, '\n');
            buf[len] = '\0';
            return;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                vc_putchar(v, '\b');
            }
            continue;
        }
        if (len < cap - 1) {
            buf[len++] = c;
            vc_putchar(v, c);
        }
    }
}

/* --- Individual command implementations.  Kept small; if one grows large,
 *     move it to its own file. ---------------------------------------------- */

static void cmd_help(void) {
    console_write("commands:\n"
                  "  help, clear, echo <text>, about, uptime\n"
                  "  meminfo, lsmod, lsdrv, lsconsole, lsblk, blktest, bctest\n"
                  "  ls <path>, cat <path>, mkdir <path>, touch <path>,\n"
                  "  write <path> <text>, mount <fs> <path> [dev]\n"
                  "  config, getconf <key>, setconf <key> <value>, saveconf\n"
                  "  ringtest, ps, spawn, yield, loop\n"
                  "  pane, pane split horizontal|vertical\n"
                  "  lslayout, setlayout <us|hu|...>\n"
                  "  shutdown, reboot\n");
}

/* -------------------------------------------------------------------- */
/* Filesystem commands.  Args are passed as raw strings; arg parsing    */
/* is intentionally tiny.                                                */
/* -------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------- */
/* Task / scheduler demo.                                                */
/*                                                                      */
/* `ticker` is a kernel-mode task that prints `[ticker N]` every second  */
/* (busy-waited via timer ticks), then yields.  Each spawn adds a new   */
/* one with its own pid so `ps` shows multiple entries.                  */
/* -------------------------------------------------------------------- */

static void ticker_main(void) {
    int i = 0;
    for (;;) {
        kprintf("[tick %d]\n", i++);
        /* Busy-wait ~1 second using the millisecond tick. */
        uint64_t end = timer_ticks_ms() + 1000;
        while (timer_ticks_ms() < end) {
            task_yield();
        }
        if (i > 5) break;               /* finite demo */
    }
    kprintf("[ticker done]\n");
}

static void cmd_spawn(void) {
    struct task* t = task_spawn("ticker", ticker_main);
    if (!t) console_write("spawn: failed (OOM?)\n");
    else    kprintf("spawned pid %d\n", t->pid);
}

/* -------------------------------------------------------------------- */
/* `loop` — spawn a tight-loop CPU hog that never yields, to demonstrate */
/* preemption.  With cooperative scheduling this would freeze the       */
/* shell forever; under M13 preemption the timer IRQ rescues us every   */
/* SCHED_QUANTUM_TICKS ms and the prompt stays responsive.              */
/*                                                                      */
/* The hog watches `loop_stop_flag` so the user can shut it down later  */
/* (todo: a real `kill` command).  Until that lands, `setconf` or a    */
/* reboot are the only ways to stop the hog.                            */
/* -------------------------------------------------------------------- */

static volatile int loop_stop_flag = 0;

static void loop_hog_main(void) {
    volatile uint32_t counter = 0;
    while (!loop_stop_flag) {
        counter++;
        /* Deliberately no yield: this is the whole point of the test. */
    }
}

static void cmd_loop(void) {
    loop_stop_flag = 0;
    struct task* t = task_spawn("cpu-hog", loop_hog_main);
    if (!t) {
        console_write("loop: spawn failed (OOM?)\n");
        return;
    }
    kprintf("loop: spawned pid %d — should NOT freeze the shell\n", t->pid);
}

/* -------------------------------------------------------------------- */
/* Pane / multi-session shell commands (M14).                           */
/*                                                                      */
/* `pane`                    → list VCs (id, rect, owner pid, focus)    */
/* `pane split horizontal`   → split current pane top/bottom            */
/* `pane split vertical`     → split current pane left/right            */
/*                                                                      */
/* The split commands spawn a fresh shell task on the new pane and      */
/* hand it the new VC via task->out_console (set BEFORE the task        */
/* actually runs, under preempt_disable, so the new task's first        */
/* kprintf already routes correctly).                                   */
/* -------------------------------------------------------------------- */

/* Forward decl — defined at the bottom alongside shell_run.  Exported
 * so kernel.c (and pane split) can pass it to task_spawn. */
void shell_task_entry(void);

static void pane_list_one(struct vc* v, void* ctx) {
    (void)ctx;
    int x, y, w, h;
    if (vc_get_rect(v, &x, &y, &w, &h) != 0) return;
    const char* tag = (vc_focused() == v) ? " <focus>" : "";
    int pid = v->task ? v->task->pid : -1;
    kprintf("  [%d] rect=(%d,%d %dx%d) pid=%d%s\n",
            v->id, x, y, w, h, pid, tag);
}

static void cmd_pane_list(void) {
    kprintf("panes (%d):\n", vc_count());
    vc_for_each(pane_list_one, NULL);
    kprintf("(Alt-1..Alt-9 to switch focus)\n");
}

static void cmd_pane_split(struct vc* my_vc, enum vc_split_dir dir) {
    struct vc* nv = vc_split(my_vc, dir);
    if (!nv) {
        kprintf("pane: split failed (max %d panes?)\n", VC_MAX);
        return;
    }
    /* Spawn the shell task for the new pane.  Bind the VC BEFORE the
     * runqueue can pick it (preempt_disable) so the task's first
     * kprintf already routes through vc_putchar(nv, ...). */
    preempt_disable();
    struct task* t = task_spawn("shell", shell_task_entry);
    if (t) {
        task_set_out_console(t, nv);
        nv->task = t;
    }
    preempt_enable();

    if (!t) {
        kprintf("pane: spawn failed for new pane id=%d\n", nv->id);
        return;
    }
    /* The new pane's shell will draw its own prompt as soon as it runs. */
}

/* -------------------------------------------------------------------- */
/* Keyboard layout commands (M16).                                       */
/*                                                                      */
/* `lslayout`        → list registered layouts + show the active one   */
/* `setlayout <name>` → switch active layout (e.g. `setlayout hu`)      */
/*                                                                      */
/* Equivalent to `setconf keyboard.layout <name>` followed by reload,   */
/* but a one-shot command is friendlier and doesn't persist to disk.    */
/* -------------------------------------------------------------------- */

static void lslayout_one(const struct kbd_layout* l, void* ctx) {
    (void)ctx;
    const char* tag = streq(l->name, keymap_current()) ? " <active>" : "";
    kprintf("  %s%s\n", l->name, tag);
}

static void cmd_lslayout(void) {
    kprintf("keyboard layouts:\n");
    keymap_for_each(lslayout_one, NULL);
}

static void cmd_setlayout(const char* name) {
    if (!name || !*name) { console_write("setlayout: missing name\n"); return; }
    if (keymap_select(name) == 0) {
        kprintf("layout: now '%s'\n", keymap_current());
    } else {
        kprintf("setlayout: unknown layout '%s'\n", name);
    }
}

/* `pane [split horizontal|vertical]` argument parser. */
static void cmd_pane(struct vc* my_vc, const char* args) {
    if (!args || !*args) { cmd_pane_list(); return; }

    /* skip leading spaces */
    while (*args == ' ') args++;
    if (!*args) { cmd_pane_list(); return; }

    /* expect "split <dir>" */
    if (starts_with(args, "split ")) {
        const char* dir = args + 6;
        while (*dir == ' ') dir++;
        if (starts_with(dir, "horiz") || streq(dir, "h")) {
            cmd_pane_split(my_vc, VC_SPLIT_HORIZ);
        } else if (starts_with(dir, "vert") || streq(dir, "v")) {
            cmd_pane_split(my_vc, VC_SPLIT_VERT);
        } else {
            console_write("pane: split direction must be horizontal or vertical\n");
        }
        return;
    }

    console_write("pane: unknown subcommand (try: pane, pane split horizontal)\n");
}

/* -------------------------------------------------------------------- */
/* Block layer test — writes a recognizable pattern to sector 1 of      */
/* /dev/vda, reads it back, prints a verdict.  Sector 0 is left alone   */
/* so we don't trample a future partition table or MBR.                  */
/* -------------------------------------------------------------------- */

static void cmd_blktest(void) {
    struct block_device* dev = blk_find("vda");
    if (!dev) {
        console_write("blktest: /dev/vda not registered (no virtio-blk?)\n");
        return;
    }

    /* Use PMM-allocated frames as DMA buffers.  A kmalloc'd 512-byte
     * buffer could land at an offset that straddles a virtual page,
     * splitting its physical backing across two non-adjacent frames —
     * fatal for a single-descriptor DMA.  A whole frame is over-
     * allocated for 512 bytes but trivially correct. */
    uint32_t wf = pmm_alloc_frame();
    uint32_t rf = pmm_alloc_frame();
    if (!wf || !rf) {
        console_write("blktest: PMM OOM\n");
        if (wf) pmm_free_frame(wf);
        if (rf) pmm_free_frame(rf);
        return;
    }
    uint8_t* wbuf = (uint8_t*)(uintptr_t)wf;
    uint8_t* rbuf = (uint8_t*)(uintptr_t)rf;

    /* Fill write buffer with a recognizable pattern: 0xA5 0x5A 0xA5 ... */
    for (int i = 0; i < 512; i++) wbuf[i] = (i & 1) ? 0x5A : 0xA5;
    for (int i = 0; i < 512; i++) rbuf[i] = 0x00;

    kprintf("blktest: writing 512 bytes of pattern to sector 1...\n");
    if (dev->write(dev, 1, 1, wbuf) != 0) {
        console_write("blktest: write failed\n");
        goto out;
    }
    kprintf("blktest: reading back...\n");
    if (dev->read(dev, 1, 1, rbuf) != 0) {
        console_write("blktest: read failed\n");
        goto out;
    }

    int ok = 1;
    int first_bad = -1;
    for (int i = 0; i < 512; i++) {
        if (rbuf[i] != wbuf[i]) { ok = 0; first_bad = i; break; }
    }
    if (ok) {
        console_write("blktest: PASS (512 bytes round-tripped)\n");
    } else {
        kprintf("blktest: FAIL — first mismatch at offset %d (wrote %x, got %x)\n",
                first_bad, wbuf[first_bad], rbuf[first_bad]);
    }

out:
    pmm_free_frame(wf);
    pmm_free_frame(rf);
}

/* -------------------------------------------------------------------- */
/* Block cache test — exercises bcache_get/release/mark_dirty/sync on   */
/* sector 2 of /dev/vda.  Sector 0 belongs to a future MBR/boot sector  */
/* and sector 1 is owned by blktest; using a separate sector avoids    */
/* cross-test interference.                                             */
/* -------------------------------------------------------------------- */

static void cmd_bctest(void) {
    struct block_device* dev = blk_find("vda");
    if (!dev) {
        console_write("bctest: /dev/vda not registered (no virtio-blk?)\n");
        return;
    }

    /* First get — expect a miss (or a hit if a previous run cached it). */
    struct bcache_buf* b = bcache_get(dev, 2);
    if (!b) { console_write("bctest: bcache_get sector 2 failed\n"); return; }

    /* Mutate: write 0xC3 0x3C ... pattern and mark dirty. */
    for (uint32_t i = 0; i < dev->sector_size; i++) {
        b->data[i] = (i & 1) ? 0x3C : 0xC3;
    }
    bcache_mark_dirty(b);
    bcache_release(b);

    /* Re-get — should be an instant cache hit on the same slot. */
    struct bcache_buf* b2 = bcache_get(dev, 2);
    if (b2 != b) {
        console_write("bctest: WARN cache returned a different slot on re-get\n");
    }
    int ok = 1;
    for (uint32_t i = 0; i < dev->sector_size; i++) {
        uint8_t want = (i & 1) ? 0x3C : 0xC3;
        if (b2->data[i] != want) { ok = 0; break; }
    }
    bcache_release(b2);
    console_write(ok ? "bctest: in-cache content matches written pattern\n"
                     : "bctest: FAIL — cached content diverges from write\n");

    /* Flush dirty entries to disk so a subsequent reboot sees the pattern. */
    if (bcache_sync(dev) == 0) console_write("bctest: sync OK\n");
    else                       console_write("bctest: sync FAILED\n");

    bcache_print_stats();
}

/* -------------------------------------------------------------------- */
/* Ring-3 demo.                                                         */
/*                                                                      */
/* Allocates two physical frames, USER-maps them into the kernel's      */
/* address space at 0x40000000 (code+data) and 0x40001000 (stack),      */
/* hand-codes a small i386 program that calls SYS_PRINT followed by     */
/* SYS_EXIT, drops the CPU to ring 3 at the entry point, and returns    */
/* via the SYS_EXIT teleport in usermode.s when the program is done.    */
/* -------------------------------------------------------------------- */

#include "syscall.h"

static void cmd_ringtest(void) {
    uint32_t code_phys  = pmm_alloc_frame();
    uint32_t stack_phys = pmm_alloc_frame();
    if (!code_phys || !stack_phys) {
        console_write("ringtest: pmm OOM\n");
        if (code_phys)  pmm_free_frame(code_phys);
        if (stack_phys) pmm_free_frame(stack_phys);
        return;
    }
    if (vmm_map(0x40000000, code_phys,  VMM_WRITABLE | VMM_USER) != 0 ||
        vmm_map(0x40001000, stack_phys, VMM_WRITABLE | VMM_USER) != 0) {
        console_write("ringtest: vmm_map failed\n");
        return;
    }

    /* Build the user program.  Layout:
     *   0x40000000 [21B]  code: mov ebx,msg; mov eax,SYS_PRINT; int 0x80;
     *                           mov eax,SYS_EXIT; int 0x80; jmp $
     *   0x40000100        msg:  "hello from ring 3!\n\0"
     */
    uint8_t* code = (uint8_t*)0x40000000;
    code[0]  = 0xBB;                              /* mov ebx, imm32 */
    *(uint32_t*)&code[1]  = 0x40000100u;          /* msg address */
    code[5]  = 0xB8;                              /* mov eax, imm32 */
    *(uint32_t*)&code[6]  = SYS_PRINT;
    code[10] = 0xCD; code[11] = 0x80;             /* int 0x80 */
    code[12] = 0xB8;                              /* mov eax, imm32 */
    *(uint32_t*)&code[13] = SYS_EXIT;
    code[17] = 0xCD; code[18] = 0x80;             /* int 0x80 */
    code[19] = 0xEB; code[20] = 0xFE;             /* jmp $ */

    char* msg = (char*)0x40000100;
    const char* src = "hello from ring 3!\n";
    int i = 0;
    while (src[i]) { msg[i] = src[i]; i++; }
    msg[i] = 0;

    /* Drop to ring 3.  Stack top is 0x40002000 (top of stack frame). */
    console_write("ringtest: dropping to ring 3...\n");
    enter_user_mode_wrap(0x40000000u, 0x40002000u);
    console_write("ringtest: back in ring 0\n");

    /* Cleanup. */
    vmm_unmap(0x40000000); pmm_free_frame(code_phys);
    vmm_unmap(0x40001000); pmm_free_frame(stack_phys);
}

/* -------------------------------------------------------------------- */
/* Configuration commands.                                              */
/* -------------------------------------------------------------------- */

static void cmd_getconf(const char* key) {
    if (!key || !*key) { console_write("getconf: missing key\n"); return; }
    const char* v = config_get(key, NULL);
    if (v) kprintf("%s = %s\n", key, v);
    else   kprintf("%s: not set\n", key);
}

/* `setconf <key> <value>` — same wonky "split at first space" parser as
 * `write`.  Doesn't honor quoting yet. */
static void cmd_setconf(const char* args) {
    if (!args || !*args) { console_write("setconf: missing args\n"); return; }
    const char* p = args;
    while (*p && *p != ' ') p++;
    if (!*p)              { console_write("setconf: missing value\n"); return; }
    char key[64];
    int  i = 0;
    while (args + i < p && i < (int)sizeof key - 1) { key[i] = args[i]; i++; }
    key[i] = 0;
    const char* val = p + 1;
    if (config_set(key, val) == 0) kprintf("%s = %s\n", key, val);
    else                           console_write("setconf: failed\n");
}

static void cmd_uptime(void) {
    /* Format ms as h:mm:ss.mmm.  No %02u in our tiny printf, so we
     * hand-roll the leading zeros. */
    uint64_t total_ms = timer_ticks_ms();
    uint32_t ms  = (uint32_t)(total_ms % 1000);
    uint32_t sec = (uint32_t)((total_ms / 1000) % 60);
    uint32_t min = (uint32_t)((total_ms / 60000) % 60);
    uint32_t hr  = (uint32_t)(total_ms / 3600000);
    kprintf("uptime: %u:%s%u:%s%u.%s%s%u\n",
            hr,
            min < 10 ? "0" : "", min,
            sec < 10 ? "0" : "", sec,
            ms  < 100 ? "0" : "", ms < 10 ? "0" : "", ms);
}

static void cmd_about(void) {
    console_write("d-os — toy x86 kernel. multiboot1, polled PS/2, VGA text mode.\n");
}

/* Dispatch a parsed line.  Each branch handles its own echo / newline —
 * there is no implicit trailing newline so commands like `echo` with no
 * argument can control output precisely.
 *
 * `my_vc` is the VC this shell instance owns — needed by pane commands
 * so a split knows which leaf to operate on. */
static void dispatch(struct vc* my_vc, const char* line) {
    if (line[0] == '\0')       return;                  /* empty line → no-op */

    if (streq(line, "help"))   { cmd_help();       return; }
    if (streq(line, "clear"))  { console_clear(); return; }
    if (streq(line, "about"))  { cmd_about();      return; }
    if (streq(line, "lsmod"))  { module_list();    return; }
    if (streq(line, "lsdrv"))  { driver_list();    return; }
    if (streq(line, "lsconsole")) { console_list(); return; }
    if (streq(line, "uptime")) { cmd_uptime();      return; }

    /* Filesystem commands — single-token first, then prefix matches. */
    if (streq(line, "ls"))     { cmd_ls("/");       return; }
    if (starts_with(line, "ls "))    { cmd_ls   (line + 3); return; }
    if (starts_with(line, "cat "))   { cmd_cat  (line + 4); return; }
    if (starts_with(line, "mkdir ")) { cmd_mkdir(line + 6); return; }
    if (starts_with(line, "touch "))  { cmd_touch(line + 6); return; }
    if (starts_with(line, "write "))  { cmd_write(line + 6); return; }
    if (starts_with(line, "mount "))  { cmd_mount(line + 6); return; }

    /* Config commands. */
    if (streq(line, "config"))         { config_dump(); return; }
    if (streq(line, "ringtest"))       { cmd_ringtest(); return; }
    if (streq(line, "blktest"))        { cmd_blktest();  return; }
    if (streq(line, "bctest"))         { cmd_bctest();   return; }
    if (streq(line, "lsblk"))          { blk_list();     return; }
    if (streq(line, "ps"))             { task_list();    return; }
    if (streq(line, "spawn"))          { cmd_spawn();    return; }
    if (streq(line, "yield"))          { task_yield();   return; }
    if (streq(line, "loop"))           { cmd_loop();     return; }
    if (streq(line, "pane"))           { cmd_pane(my_vc, "");      return; }
    if (starts_with(line, "pane "))    { cmd_pane(my_vc, line + 5); return; }
    if (streq(line, "lslayout"))       { cmd_lslayout();             return; }
    if (starts_with(line, "setlayout ")) { cmd_setlayout(line + 10); return; }
    if (streq(line, "saveconf"))       {
        if (config_save() == 0) console_write("config saved.\n");
        else                    console_write("saveconf: failed\n");
        return;
    }
    if (starts_with(line, "getconf ")) { cmd_getconf(line + 8); return; }
    if (starts_with(line, "setconf ")) { cmd_setconf(line + 8); return; }
    if (streq(line, "meminfo")){
        mboot_print_meminfo();
        pmm_print_stats();
        vmm_print_status();
        struct kmstat ks;
        kmalloc_stats(&ks);
        kprintf("kheap: %u/%u bytes used (%u chunks, %u free)\n",
                (unsigned)ks.used_bytes, (unsigned)ks.total_bytes,
                ks.chunk_count, ks.free_chunk_count);
        return;
    }
    if (streq(line, "echo"))   { console_putchar('\n'); return; }   /* bare `echo` */
    if (streq(line, "shutdown")) {
        console_write("shutting down...\n");
        hal_shutdown();                                 /* normally never returns */
        return;
    }
    if (streq(line, "reboot")) {
        console_write("rebooting...\n");
        hal_reboot();                                   /* normally never returns */
        return;
    }
    if (starts_with(line, "echo ")) {
        /* Skip past "echo " (5 chars) and print the rest. */
        console_write(line + 5);
        console_putchar('\n');
        return;
    }

    /* Fallback: echo what we didn't understand, to make the failure visible
     * rather than mysterious. */
    console_write("unknown: ");
    console_write(line);
    console_putchar('\n');
}

/* Top-level REPL — one shell instance per VC, runs forever in its own
 * task.  All output (prompts, command results, errors) flows through
 * kprintf → console_putchar → per-task hook → vc_putchar(my_vc, ...).
 *
 * The first prompt that prints is the user's only signal that the new
 * pane is alive, so we print it before any blocking read. */
void shell_run(struct vc* v) {
    char line[LINE_MAX];
    /* Announce ourselves once in case this pane was just spawned. */
    kprintf("[pane %d ready, pid %d]\n",
            v->id, task_current() ? task_current()->pid : -1);
    for (;;) {
        kprintf("%s", config_get("shell.prompt", DEFAULT_PROMPT));
        read_line(v, line, LINE_MAX);
        dispatch(v, line);
    }
}

/* Task entry-point wrapper.  task_spawn doesn't pass arguments, so we
 * read the bound VC out of our own task->out_console (set by the spawner
 * under preempt_disable before we were first scheduled). */
void shell_task_entry(void) {
    struct task* me = task_current();
    struct vc*   v  = me ? (struct vc*)me->out_console : NULL;
    if (!v) {
        kprintf("shell_task_entry: no VC bound — exiting\n");
        return;
    }
    shell_run(v);
}
