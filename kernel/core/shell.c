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
#include "net.h"
#include "audio.h"
#include "vc.h"
#include "lock.h"
#include "keymap.h"
#include "percpu.h"
#include "slab.h"
#include "gui.h"
#include "gui_app.h"
#include "shell_provider.h"
#include "basic.h"
#include "klog.h"
#include "elf.h"
#include "proc.h"
#include "syscall.h"
#include "fd.h"
#include "service.h"
#include "cron.h"

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
                  "  dmesg [-l <level>] (kernel log)\n"
                  "  ls <path>, cat <path>, mkdir <path>, touch <path>,\n"
                  "  write <path> <text>, mount <fs> <path> [dev]\n"
                  "  config, getconf <key>, setconf <key> <value>, saveconf\n"
                  "  ringtest, ps, spawn, yield, loop, kill <pid>\n"
                  "  pane, pane split horizontal|vertical\n"
                  "  gui (compositor + desktop), gui stats, launch [app]\n"
                  "  run <path.bas> (Tiny-BASIC)\n"
                  "  lslayout, setlayout <us|hu|...>, lscpu, taskset <pid> <mask>\n"
                  "  slabinfo, buddyinfo\n"
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
    /* Deliberately no yield (that is the point of the preemption test),
     * but M22.3 adds the kthread contract: CPU-bound kernel threads
     * MUST poll task_should_stop() so `kill` / the task manager can
     * terminate them — same rule as Linux kthread_should_stop(). */
    while (!loop_stop_flag && !task_should_stop()) {
        counter++;
    }
}

/* `kill <pid>` — cooperative task termination (M22.3).  The victim
 * dies at its next yield point / task_should_stop() poll; reaping is
 * lazy (the GUI window teardown reaps its own shells; CLI kills stay
 * as DEAD entries in `ps` until something reaps them — good enough
 * for a teaching kernel, and visible state is a feature here). */
static void cmd_kill(const char* args) {
    int pid = 0, any = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) { pid = pid * 10 + (*args - '0'); any = 1; }
    if (!any) { console_write("kill: usage: kill <pid>\n"); return; }
    if (task_current() && task_current()->pid == pid) {
        console_write("kill: refusing to kill the calling shell\n");
        return;
    }
    if (task_kill(pid) == 0) kprintf("kill: pid %d flagged (dies at next yield)\n", pid);
    else                     kprintf("kill: pid %d not found or protected\n", pid);
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
    struct task* t = task_spawn("shell", shell_provider_active()->entry);
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

/* -------------------------------------------------------------------- */
/* CPU topology — `lscpu` (M18).                                         */
/* -------------------------------------------------------------------- */

/* `launch [app]` — walk the GUI_APP registry (M22.2).  Without an
 * argument it lists the registered apps; with one it launches the
 * (case-insensitive, prefix-matched) app.  This is how apps start
 * under chromeless desktop shells (gui.shell=bare), and it runs the
 * app on THIS shell task — the gui/widget APIs are task-agnostic. */
static void cmd_launch(const char* args) {
    if (!gui_is_active()) {
        console_write("launch: GUI not running (start it with 'gui')\n");
        return;
    }
    if (!args || !*args) {
        kprintf("registered GUI apps (%d):\n", gui_app_count());
        for (int i = 0; i < gui_app_count(); i++)
            kprintf("  %s\n", gui_app_at(i)->name);
        return;
    }
    const struct gui_app_def* app = gui_app_find(args);
    if (!app) { kprintf("launch: no app matching '%s'\n", args); return; }
    /* M22.7 — hand it to the compositor, which spawns the app-host task.
     * Calling app->launch() here would run the app on this shell's task
     * with no event loop. */
    gui_queue_launch(app);
}

/* `run <path>` — batch-run a Tiny-BASIC program on this shell's VC
 * (M22.5).  The interpreter state is ~22 KiB, so it lives on the heap
 * — never on the 4 KiB task stack.  If the shell is killed mid-run
 * the block leaks; acceptable for a hand-driven command (the GUI
 * BASIC window uses a static instance instead). */
static void cmd_run(struct vc* my_vc, const char* path) {
    while (*path == ' ') path++;
    if (!*path) { kprintf("run: usage: run <path.bas>\n"); return; }
    struct basic* b = (struct basic*)kmalloc(sizeof *b);
    if (!b) { kprintf("run: OOM\n"); return; }
    basic_init(b, my_vc);
    if (basic_load(b, path) != 0)
        kprintf("run: cannot load %s (missing? unnumbered lines?)\n", path);
    else
        basic_run(b);
    kfree(b);
}

/* `gui stats` — damage-rect effectiveness counters (M22.3). */
static void cmd_gui_stats(void) {
    if (!gui_is_active()) { console_write("gui stats: GUI not running\n"); return; }
    unsigned full = 0, partial = 0, avg_kb = 0;
    gui_get_stats(&full, &partial, &avg_kb);
    kprintf("frames: %u full, %u partial (dirty-rect), avg %u KB blitted/frame\n",
            full, partial, avg_kb);
}

/* `gui` — start the M22 compositor.  The calling shell keeps running in
 * its (now invisible) pane; two fresh shells come up in windows.  A
 * second invocation is a no-op — the compositor is a singleton. */
static void cmd_gui(void) {
    if (gui_is_active()) {
        console_write("gui: already running\n");
        return;
    }
    if (gui_start() != 0)
        console_write("gui: start failed (no framebuffer?)\n");
}

static void cmd_lscpu(void) {
    int n = smp_ncpus();
    int me = this_cpu_id();
    kprintf("CPU  APIC_ID  NODE  STATE   RQ\n");
    for (int i = 0; i < n; i++) {
        struct percpu* p = percpu_at(i);
        if (!p) continue;
        kprintf("%d    %u        %d     %s   %d%s\n",
                i, p->apic_id, p->numa_node,
                p->online ? "online " : "offline",
                p->rq_count,
                (i == me) ? " <this>" : "");
    }
}

/* `taskset <pid> <hex_mask>` — pin task to a CPU set (M18.6.3).
 * Mask is parsed as hex (with or without 0x prefix).  0xFF = any of
 * CPUs 0..7.  Errors print but never crash. */
static int parse_hex(const char* s, uint32_t* out) {
    if (!s || !*s) return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    int any = 0;
    while (*s) {
        int d;
        if      (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = 10 + (*s - 'a');
        else if (*s >= 'A' && *s <= 'F') d = 10 + (*s - 'A');
        else return -1;
        v = (v << 4) | (uint32_t)d;
        any = 1;
        s++;
    }
    if (!any) return -1;
    *out = v;
    return 0;
}

static int parse_uint(const char* s, uint32_t* out) {
    if (!s || !*s) return -1;
    uint32_t v = 0;
    int any = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        any = 1;
        s++;
    }
    if (!any || *s != 0) return -1;
    *out = v;
    return 0;
}

static void cmd_taskset(const char* args) {
    if (!args || !*args) {
        console_write("taskset: usage: taskset <pid> <hex-mask>  (e.g. taskset 5 0x2)\n");
        return;
    }
    /* Parse pid (decimal), then mask (hex, may have 0x prefix). */
    char pid_buf[16];
    int pi = 0;
    while (*args && *args != ' ' && pi < (int)sizeof pid_buf - 1) {
        pid_buf[pi++] = *args++;
    }
    pid_buf[pi] = 0;
    while (*args == ' ') args++;
    if (!*args) {
        console_write("taskset: missing mask\n");
        return;
    }
    uint32_t pid;
    if (parse_uint(pid_buf, &pid) != 0) {
        kprintf("taskset: bad pid '%s'\n", pid_buf);
        return;
    }
    uint32_t mask;
    if (parse_hex(args, &mask) != 0) {
        kprintf("taskset: bad mask '%s'\n", args);
        return;
    }
    struct task* t = task_find((int)pid);
    if (!t) {
        kprintf("taskset: no task with pid %u\n", pid);
        return;
    }
    if (task_set_affinity(t, mask) != 0) {
        kprintf("taskset: rejected (mask=0 is not allowed)\n");
        return;
    }
    kprintf("taskset: pid %u mask now 0x%x (home cpu=%d)\n",
            pid, mask, t->cpu_home);
}

/* -------------------------------------------------------------------- */
/* Memory — `slabinfo` and `buddyinfo` (M19).                            */
/* -------------------------------------------------------------------- */

static void cmd_slabinfo(void) {
    int n = slab_cache_count();
    kprintf("NAME           OBJSZ  SLOT  SLABS  IN_USE  FREE  MAG  CACHED-EMPTY\n");
    for (int i = 0; i < n; i++) {
        struct slab_stats s;
        slab_cache_get_stats(i, &s);
        kprintf("%s  %u  %u  %u  %u  %u  %u  %u\n",
                s.name, (unsigned)s.obj_size, (unsigned)s.slot_size,
                s.slabs, s.in_use_objs, s.free_objs, s.mag_total,
                s.cached_empty);
    }
}

static void cmd_buddyinfo(void) {
    const char* zone_names[NR_ZONES] = { "DMA", "NORMAL", "HIGHMEM" };
    uint32_t order_counts[BUDDY_MAX_ORDER + 1];
    kprintf("ZONE     MANAGED  FREE-BLOCKS-PER-ORDER (0..%u)\n",
            BUDDY_MAX_ORDER);
    for (int z = 0; z < NR_ZONES; z++) {
        uint32_t managed = 0;
        pmm_zone_stats(z, order_counts, &managed);
        kprintf("%s ", zone_names[z]);
        kprintf("m=%u  ", managed);
        for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
            kprintf("%u ", order_counts[o]);
        }
        kprintf("\n");
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

/* ----------------------- §M24.1 network commands -------------------------- */

/* `ping <ip> [count]` — ARP-resolve then ICMP-echo the target. */
static void cmd_ping(const char* args) {
    struct net_device* dev = net_primary();
    if (!dev) { console_write("ping: no network device (no virtio-net?)\n"); return; }

    /* Parse "<ip>" and an optional trailing count. */
    char ipbuf[32]; int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) { ipbuf[i] = args[i]; i++; }
    ipbuf[i] = '\0';
    if (i == 0) { console_write("usage: ping <ip> [count]\n"); return; }

    uint32_t ip;
    if (net_parse_ip(ipbuf, &ip) != 0) { console_write("ping: bad IP\n"); return; }

    int count = 3;
    while (args[i] == ' ') i++;
    if (args[i]) {
        int c = 0; for (int j = i; args[j] >= '0' && args[j] <= '9'; j++) c = c * 10 + (args[j] - '0');
        if (c > 0 && c <= 16) count = c;
    }
    net_ping(dev, ip, count);
}

/* `arp <ip>` — resolve and print the MAC. */
static void cmd_arp(const char* args) {
    struct net_device* dev = net_primary();
    if (!dev) { console_write("arp: no network device\n"); return; }
    uint32_t ip;
    if (net_parse_ip(args, &ip) != 0) { console_write("usage: arp <ip>\n"); return; }
    uint8_t mac[6];
    if (net_arp_resolve(dev, ip, mac) == 0) {
        char ipb[16], macb[18]; net_fmt_ip(ip, ipb); net_fmt_mac(mac, macb);
        kprintf("%s is at %s\n", ipb, macb);
    } else {
        console_write("arp: no reply (timeout)\n");
    }
}

/* `nslookup <host>` — resolve a hostname to an IPv4 via the SLIRP DNS proxy. */
static void cmd_dns(const char* args) {
    struct net_device* dev = net_primary();
    if (!dev) { console_write("nslookup: no network device\n"); return; }
    if (!args[0]) { console_write("usage: nslookup <hostname>\n"); return; }
    uint32_t ip;
    if (net_dns_query(dev, args, &ip) == 0) {
        char ipb[16]; net_fmt_ip(ip, ipb);
        kprintf("%s has address %s\n", args, ipb);
    } else {
        kprintf("nslookup: could not resolve %s\n", args);
    }
}

/* `wget http://host[/path]` — fetch a URL over TCP (§M24.3) and print it. */
static void cmd_wget(const char* url) {
    struct net_device* dev = net_primary();
    if (!dev) { console_write("wget: no network device\n"); return; }

    /* Strip an optional "http://" scheme. */
    const char* p = url;
    if (starts_with(p, "http://")) p += 7;

    /* Split host[:port] and path. */
    char host[128]; int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < 127) host[hi++] = *p++;
    host[hi] = '\0';
    uint16_t port = 80;
    if (*p == ':') { p++; int v = 0; while (*p >= '0' && *p <= '9') v = v*10 + (*p++ - '0'); port = (uint16_t)v; }
    const char* path = (*p == '/') ? p : "/";
    if (hi == 0) { console_write("usage: wget http://host[:port][/path]\n"); return; }

    /* Resolve host: accept a literal dotted-quad, else DNS. */
    uint32_t ip;
    if (net_parse_ip(host, &ip) != 0) {
        if (net_dns_query(dev, host, &ip) != 0) { kprintf("wget: cannot resolve %s\n", host); return; }
    }
    char ipb[16]; net_fmt_ip(ip, ipb);
    kprintf("wget: connecting to %s (%s):%u ...\n", host, ipb, port);

    int n = net_http_get(dev, ip, port, host, path);
    if (n < 0) { console_write("wget: connection failed\n"); return; }

    uint32_t blen; const uint8_t* body = net_http_body(&blen);
    /* Print up to ~1 KiB of the response so a big page doesn't flood. */
    uint32_t show = blen < 1024 ? blen : 1024;
    for (uint32_t i = 0; i < show; i++) console_putchar((char)body[i]);
    if (show < blen) kprintf("\n... [%u bytes total]\n", blen);
    else             kprintf("\n[%u bytes]\n", blen);
}

/* `nettest` — self-contained §M24 check: ARP + ping the SLIRP gateway (§M24.1),
 * a DNS resolve over UDP (§M24.2), and an HTTP GET over TCP (§M24.3).  Prints
 * PASS/FAIL lines so a headless boot can grep the serial log. */
static void cmd_nettest(void) {
    struct net_device* dev = net_primary();
    if (!dev) { console_write("nettest: FAIL (no net device)\n"); return; }
    uint32_t gw = dev->gateway;
    uint8_t mac[6];
    if (net_arp_resolve(dev, gw, mac) != 0) { console_write("nettest: FAIL (ARP)\n"); return; }
    int got = net_ping(dev, gw, 3);
    if (got > 0) kprintf("nettest: PASS icmp (%d/3 echo replies)\n", got);
    else         console_write("nettest: FAIL (no echo reply)\n");

    /* §M24.2 — resolve a well-known name over UDP/DNS. */
    uint32_t ip = 0;
    if (net_dns_query(dev, "example.com", &ip) == 0) {
        char ipb[16]; net_fmt_ip(ip, ipb);
        kprintf("nettest: PASS dns (example.com -> %s)\n", ipb);
    } else {
        console_write("nettest: FAIL (dns)\n");
    }

    /* §M24.3 — HTTP GET over TCP to the resolved address. */
    if (ip) {
        int n = net_http_get(dev, ip, 80, "example.com", "/");
        if (n > 0) {
            uint32_t blen; const uint8_t* body = net_http_body(&blen);
            /* Show just the status line. */
            char status[64]; int si = 0;
            for (uint32_t i = 0; i < blen && body[i] != '\r' && body[i] != '\n' && si < 63; i++)
                status[si++] = (char)body[i];
            status[si] = '\0';
            kprintf("nettest: PASS tcp (%d bytes, \"%s\")\n", n, status);
        } else {
            console_write("nettest: FAIL (tcp)\n");
        }
    }
}

/* ----------------------- §M23 audio commands ------------------------------ */

/* `beep` — play a short 440 Hz tone (the §M23 smoke test). */
static void cmd_beep(void) {
    if (!audio_primary()) { console_write("beep: no audio device (no AC97?)\n"); return; }
    audio_play_tone(440, 400);
}

/* `tone <freq> <ms>` — play an arbitrary square-wave tone. */
static void cmd_tone(const char* args) {
    if (!audio_primary()) { console_write("tone: no audio device\n"); return; }
    int freq = 0, ms = 0, i = 0;
    while (args[i] >= '0' && args[i] <= '9') freq = freq*10 + (args[i++] - '0');
    while (args[i] == ' ') i++;
    while (args[i] >= '0' && args[i] <= '9') ms = ms*10 + (args[i++] - '0');
    if (freq <= 0) freq = 440;
    if (ms   <= 0) ms   = 400;
    audio_play_tone((uint32_t)freq, (uint32_t)ms);
}

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

/* The ring-3/EL0 self-test is arch-specific (see usermode.h / the per-arch
 * arch_ringtest implementations); shell.c just invokes it. */
static void cmd_ringtest(void) { arch_ringtest(); }

/* M25 stage 1 — per-process address space self-test.  Creates a fresh
 * vmm_space, maps one user page carrying a sentinel, switches this CPU to
 * that space, reads the page back, then switches to the kernel space and
 * proves the mapping is PRIVATE (not visible in the kernel directory).
 * IRQs are held off across the CR3 excursion so no reschedule ever runs
 * with our non-standard address space loaded (the scheduler doesn't switch
 * CR3 yet — that's the next stage-1 step). */
static void cmd_mmtest(void) {
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("mmtest: vmm_space_create failed\n"); return; }

    uint32_t frame = pmm_alloc_frame();          /* backing for the user page */
    if (!frame) { console_write("mmtest: no frame\n"); vmm_space_destroy(s); return; }

    /* Seed the sentinel through the identity map (frame < 256 MiB). */
    *(volatile uint32_t*)(uintptr_t)frame = 0xC0FFEE42u;

    const uintptr_t UVA = vmm_user_base();        /* arch's user-region base */
    if (vmm_space_map(s, UVA, frame, VMM_WRITABLE | VMM_USER) != 0) {
        console_write("mmtest: vmm_space_map failed\n");
        pmm_free_frame(frame); vmm_space_destroy(s); return;
    }

    spinlock_t lk = SPINLOCK_INIT;
    uint32_t flags = spin_lock_irqsave(&lk);
    vmm_space_switch(s);
    uint32_t got = *(volatile uint32_t*)UVA;      /* read via the space's map */
    vmm_space_switch(NULL);                        /* back to kernel space */
    spin_unlock_irqrestore(&lk, flags);

    uintptr_t kview = vmm_translate(UVA);          /* kernel-space view of UVA */

    kprintf("mmtest: read 0x%x @ %p (want 0xc0ffee42) -> %s; "
            "kernel translate(UVA)=%p (want 0x0, private) -> %s\n",
            got, (void*)UVA, got == 0xC0FFEE42u ? "PASS" : "FAIL",
            (void*)kview, kview == 0 ? "PASS" : "FAIL");

    vmm_space_destroy(s);                          /* frees PT + user frame + PD */
}

/* M25 stage 2a — ELF loader self-test.  Synthesises a minimal static ELF of
 * this arch's native class (one PT_LOAD segment carrying a known payload at
 * the user-region base), loads it into a fresh vmm_space via elf_load(), then
 * switches to that space to confirm the segment bytes + entry landed where the
 * program headers said — and that the mapping is PRIVATE (invisible to the
 * kernel space).  This exercises the loader end-to-end without needing a
 * userland toolchain in the tree yet; actually *running* the loaded image in
 * ring 3 is stage 2b. */
static void cmd_elftest(void) {
    const uintptr_t base = vmm_user_base();
    const char payload[] = "ELF-LOAD-OK";           /* the segment's contents */

    static uint8_t image[2 * 4096];                 /* scratch ELF image */
    size_t ilen = elf_build_selftest(image, sizeof image, base,
                                     payload, sizeof payload);
    if (!ilen) { console_write("elftest: build failed\n"); return; }

    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("elftest: vmm_space_create failed\n"); return; }

    uintptr_t entry = 0;
    int rc = elf_load(s, image, ilen, &entry);
    if (rc != ELF_OK) {
        kprintf("elftest: elf_load failed (%d)\n", rc);
        vmm_space_destroy(s); return;
    }

    /* Switch into the space, read the loaded payload back at its vaddr. */
    char got[sizeof payload];
    spinlock_t lk = SPINLOCK_INIT;
    uint32_t flags = spin_lock_irqsave(&lk);
    vmm_space_switch(s);
    for (size_t i = 0; i < sizeof payload; i++)
        got[i] = ((volatile char*)base)[i];
    vmm_space_switch(NULL);
    spin_unlock_irqrestore(&lk, flags);

    int match = 1;
    for (size_t i = 0; i < sizeof payload; i++)
        if (got[i] != payload[i]) { match = 0; break; }

    uintptr_t kview = vmm_translate(base);          /* kernel-space view */

    kprintf("elftest: loaded, entry=%p (want %p) -> %s; segment='%s' -> %s; "
            "kernel translate(base)=%p -> %s\n",
            (void*)entry, (void*)base, entry == base ? "PASS" : "FAIL",
            got, match ? "PASS" : "FAIL",
            (void*)kview, kview == 0 ? "PASS" : "FAIL");

    vmm_space_destroy(s);
}

/* M25 stage 2b — build the arch's hello program, wrap it in a static ELF, and
 * actually RUN it in ring 3 / EL0 in its own address space via proc_exec_elf.
 * The program SYS_PRINTs a greeting then SYS_EXITs (returning here).  This is
 * the ELF-loader path's payoff: a loaded-from-image user program executing,
 * isolated in a private space — not hand-poked machine code in the shared
 * kernel map (that's the older `ringtest`). */
static void cmd_userrun(void) {
    const uintptr_t base = vmm_user_base();

    static uint8_t payload[512];
    size_t plen = arch_user_hello(payload, sizeof payload, base);
    if (!plen) { console_write("userrun: hello build failed\n"); return; }

    static uint8_t image[3 * 4096];
    size_t ilen = elf_build_selftest(image, sizeof image, base, payload, plen);
    if (!ilen) { console_write("userrun: elf build failed\n"); return; }

    console_write("userrun: exec'ing user ELF...\n");
    int rc = proc_exec_elf(image, ilen);
    kprintf("userrun: returned from user program (rc=%d)\n", rc);
}

/* M25 stage 3 — per-process fd table + open/read/write/close/lseek.  Drives
 * the SAME sys_* handlers the ring-3 syscall dispatchers call: create a ramfs
 * file, then open/read/lseek/close it through the fd layer and echo it via
 * sys_write(1, …).  (userrun already proves the ring-3 → syscall trap; this
 * validates the fd-table semantics directly.) */
static void cmd_fdtest(void) {
    const char* path    = "/fdtest.txt";
    const char* content = "M25 fd table works";
    size_t clen = 0; while (content[clen]) clen++;

    struct file* wf = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!wf) { console_write("fdtest: create failed\n"); return; }
    vfs_write(wf, content, clen);
    vfs_close(wf);

    int fd = sys_open(path, VFS_RDONLY);
    if (fd < 0) { console_write("fdtest: sys_open failed\n"); return; }

    char buf[64];
    long n = sys_read(fd, buf, sizeof buf - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    int read_ok = ((size_t)n == clen);
    for (long i = 0; i < n; i++) if (buf[i] != content[i]) read_ok = 0;

    long pos = sys_lseek(fd, 0, SEEK_SET);          /* rewind */
    char b2[4];
    long n2 = sys_read(fd, b2, 3);
    int seek_ok = (pos == 0 && n2 == 3 && b2[0] == content[0]);

    console_write("fdtest: sys_write(1) echo: ");
    sys_write(1, content, clen);
    console_putchar('\n');

    int close_ok = (sys_close(fd) == 0);
    int reuse    = sys_open(path, VFS_RDONLY);      /* freed slot reused? */
    int reuse_ok = (reuse == fd);
    if (reuse >= 0) sys_close(reuse);

    kprintf("fdtest: open=%d read=%s(%ld) lseek=%s close=%s reuse=%s\n",
            fd, read_ok ? "PASS" : "FAIL", n, seek_ok ? "PASS" : "FAIL",
            close_ok ? "PASS" : "FAIL", reuse_ok ? "PASS" : "FAIL");
}

/* M25 stage 4 — anonymous mmap + memfd shared memory.  Borrows a private
 * address space (like proc_exec_elf) so sys_mmap has a user space to map
 * into, then: (1) mmaps an anonymous region and read/writes it; (2) creates a
 * memfd and mmaps it TWICE — a write through one mapping is visible through
 * the other, proving one backing frame set behind two VAs (the shm-sharing
 * mechanism; cross-process sharing is stage 5).  VMM_SHARED keeps the space
 * teardown from double-freeing the shm frames. */
static void cmd_shmtest(void) {
    struct task* me = task_current();
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("shmtest: no space\n"); return; }
    struct vmm_space* prev = me->mm;
    me->mm = s; me->mmap_cursor = 0;
    vmm_space_switch(s);

    long a = sys_mmap(8192, -1);                     /* anonymous, 2 pages */
    int anon_ok = 0;
    if (a > 0) {
        volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)a;
        p[0] = 0xABCD1234u; p[1500] = 0x5678u;       /* touch both pages */
        anon_ok = (p[0] == 0xABCD1234u && p[1500] == 0x5678u);
    }

    int  fd = sys_memfd(4096);
    long m1 = (fd >= 0) ? sys_mmap(4096, fd) : -1;
    long m2 = (fd >= 0) ? sys_mmap(4096, fd) : -1;   /* second mapping, same object */
    int shm_ok = 0;
    if (m1 > 0 && m2 > 0 && m1 != m2) {
        *(volatile uint32_t*)(uintptr_t)m1 = 0xFEEDFACEu;
        shm_ok = (*(volatile uint32_t*)(uintptr_t)m2 == 0xFEEDFACEu);
    }
    if (fd >= 0) sys_close(fd);                       /* frees shm frames once */

    vmm_space_switch(prev);
    me->mm = prev; me->mmap_cursor = 0;
    vmm_space_destroy(s);                             /* frees anon; skips shm */

    kprintf("shmtest: anon-mmap=%s shm-shared=%s (a=%p m1=%p m2=%p)\n",
            anon_ok ? "PASS" : "FAIL", shm_ok ? "PASS" : "FAIL",
            (void*)(uintptr_t)a, (void*)(uintptr_t)m1, (void*)(uintptr_t)m2);
}

/* M25 stage 5 — unix socketpair + fd passing (SCM_RIGHTS).  Creates a
 * connected pair, sends bytes one way and receives them the other, then the
 * payoff: creates a memfd, writes a sentinel through a mapping, PASSES the fd
 * across the socket, and on the receiving side maps the received fd and reads
 * the sentinel back — one shm object reached via a descriptor that travelled
 * over a socket (the Wayland wl_shm / keymap handover).  Borrows a private
 * space so mmap has somewhere to map. */
static void cmd_socktest(void) {
    struct task* me = task_current();
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("socktest: no space\n"); return; }
    struct vmm_space* prev = me->mm;
    me->mm = s; me->mmap_cursor = 0;
    vmm_space_switch(s);

    int fds[2] = { -1, -1 };
    int sp = sys_socketpair(fds);

    /* byte stream one way */
    long sent = sys_send(fds[0], "ping", 4, -1);
    char rb[8]; int pf = -2;
    long got = sys_recv(fds[1], rb, sizeof rb, &pf);
    int data_ok = (sp == 0 && sent == 4 && got == 4 &&
                   rb[0] == 'p' && rb[3] == 'g' && pf == -1);

    /* fd passing → shared memory across the socket */
    int  fdshm = sys_memfd(4096);
    long m1 = (fdshm >= 0) ? sys_mmap(4096, fdshm) : -1;
    if (m1 > 0) *(volatile uint32_t*)(uintptr_t)m1 = 0xCAFEBABEu;

    long sent2 = sys_send(fds[0], "fd", 2, fdshm);      /* pass the memfd */
    char rb2[8]; int passed = -2;
    long got2 = sys_recv(fds[1], rb2, sizeof rb2, &passed);   /* receive new fd */

    int m2_ok = 0;
    if (passed >= 3) {
        long m2 = sys_mmap(4096, passed);
        if (m2 > 0) m2_ok = (*(volatile uint32_t*)(uintptr_t)m2 == 0xCAFEBABEu);
        sys_close(passed);
    }
    int pass_ok = (sent2 == 2 && got2 == 2 && passed >= 3 && m2_ok);

    if (fdshm >= 0) sys_close(fdshm);
    if (fds[0] >= 0) sys_close(fds[0]);
    if (fds[1] >= 0) sys_close(fds[1]);

    vmm_space_switch(prev);
    me->mm = prev; me->mmap_cursor = 0;
    vmm_space_destroy(s);

    kprintf("socktest: pair+data=%s fd-passing(shared mem)=%s (passed fd=%d)\n",
            data_ok ? "PASS" : "FAIL", pass_ok ? "PASS" : "FAIL", passed);
}

/* M25 stage 6 — poll readiness.  On a socketpair: poll reports NOT-readable
 * before a send, readable after, and NOT-readable again once drained.  (This
 * is the non-blocking readiness snapshot; true sleep-until-ready arrives with
 * the concurrent-process scheduler.) */
static void cmd_polltest(void) {
    int fds[2] = { -1, -1 };
    if (sys_socketpair(fds) != 0) { console_write("polltest: pair failed\n"); return; }

    struct pollfd pf = { .fd = fds[1], .events = POLLIN, .revents = 0 };
    int r_before = sys_poll(&pf, 1, 0);
    int before_ok = (r_before == 0 && !(pf.revents & POLLIN));

    sys_send(fds[0], "x", 1, -1);
    pf.revents = 0;
    int r_after = sys_poll(&pf, 1, 0);
    int after_ok = (r_after == 1 && (pf.revents & POLLIN));

    char c; sys_recv(fds[1], &c, 1, NULL);          /* drain */
    pf.revents = 0;
    int r_drain = sys_poll(&pf, 1, 0);
    int drain_ok = (r_drain == 0);

    sys_close(fds[0]); sys_close(fds[1]);

    kprintf("polltest: before-send=%s after-send=%s after-drain=%s\n",
            before_ok ? "PASS" : "FAIL", after_ok ? "PASS" : "FAIL",
            drain_ok ? "PASS" : "FAIL");
}

/* ---- Tier A: wait-queue / task_wait / blocking read self-test ------------- */

/* Part 1 — task_wait.  The child burns a little CPU (so the parent reaches
 * task_wait and truly BLOCKS before the child exits — exercising the sleep
 * path, not a fast-path pickup), stamps a marker, then exits with code 42. */
static volatile int g_waitkid_marker;
static void waitkid_entry(void) {
    for (volatile int i = 0; i < 3000000; i++) { }
    g_waitkid_marker = 0x1234;
    task_exit_code(42);
}

/* Part 2 — blocking socket read across two tasks.  The producer runs on its
 * own task; the shell task is the consumer and does a BLOCKING usock_recv on
 * the empty endpoint, so it parks on the socket's read wait-queue until the
 * producer's send wakes it.  Raw usock_* (not fds) because fd numbers are
 * per-task — the shared object is the endpoint pointer. */
static struct usock*    g_bt_prod_ep;
static void blockprod_entry(void) {
    for (volatile int i = 0; i < 4000000; i++) { }   /* let the consumer block first */
    usock_send(g_bt_prod_ep, "PONG", 4, NULL);
    task_exit();
}

static void cmd_waittest(void) {
    /* --- Part 1: task_wait blocks until the child exits, returns its code. */
    g_waitkid_marker = 0;
    struct task* c = task_spawn("waitkid", waitkid_entry);
    if (!c) { console_write("waittest: spawn failed\n"); return; }
    int kpid = c->pid;
    task_set_reap_owned(c, 1);         /* claim the reap so init won't harvest first */
    int code = -1;
    int r = task_wait(kpid, &code);
    int wait_ok = (r == kpid && code == 42 && g_waitkid_marker == 0x1234);

    /* --- Part 2: blocking recv parks until a producer task sends. */
    struct usock *a = NULL, *b = NULL;
    int read_ok = 0, eof_ok = 0;
    if (usock_pair(&a, &b) == 0) {
        g_bt_prod_ep = a;              /* producer sends on a → fills b's ring */
        struct task* p = task_spawn("blockprod", blockprod_entry);
        int ppid = p ? p->pid : -1;
        if (p) task_set_reap_owned(p, 1);

        char rb[8];
        long got = usock_recv(b, rb, sizeof rb, 1, NULL);   /* BLOCKS until send */
        read_ok = (got == 4 && rb[0] == 'P' && rb[1] == 'O' &&
                   rb[2] == 'N' && rb[3] == 'G');

        if (ppid >= 0) task_wait(ppid, NULL);               /* reap the producer */

        /* Blocking recv on a now-empty endpoint whose peer we close returns
         * 0 (EOF) rather than hanging — the close wakes the reader. */
        usock_close(a);                                     /* peer of b closes */
        long eof = usock_recv(b, rb, sizeof rb, 1, NULL);
        eof_ok = (eof == 0);
        usock_close(b);
    }

    kprintf("waittest: task_wait(block+code)=%s blocking-recv=%s peer-close-EOF=%s\n",
            wait_ok ? "PASS" : "FAIL", read_ok ? "PASS" : "FAIL",
            eof_ok ? "PASS" : "FAIL");
}

/* ---- M29: services + service bus -------------------------------------- */

/* `service [list | start|stop|restart|status <name>]` — supervisor control. */
static void cmd_service(const char* args) {
    if (!args || !*args || starts_with(args, "list")) { service_list(); return; }

    char sub[16]; int i = 0;
    while (args[i] && args[i] != ' ' && i < 15) { sub[i] = args[i]; i++; }
    sub[i] = 0;
    const char* name = args + i;
    while (*name == ' ') name++;

    if (streq(sub, "status")) {
        if (*name) service_status(name);
        else console_write("service: usage: service status <name>\n");
        return;
    }
    if (streq(sub, "start")) {
        int r = service_start(name);
        kprintf("service start %s: %s\n", name,
                r == 0 ? "ok" : (r == -2 ? "already running" : "no such service"));
        return;
    }
    if (streq(sub, "stop")) {
        int r = service_stop(name);
        kprintf("service stop %s: %s\n", name,
                r == 0 ? "ok" : (r == -2 ? "not running" : "no such service"));
        return;
    }
    if (streq(sub, "restart")) {
        int r = service_restart(name);
        kprintf("service restart %s: %s\n", name, r == 0 ? "ok" : "no such service");
        return;
    }
    console_write("service: usage: service [list | start|stop|restart|status <name>]\n");
}

/* `bustest` — service-bus self-test: exact bind, strict miss, adapted bind. */
extern void svc_demo_bustest(void);
static void cmd_bustest(void) { svc_demo_bustest(); }

/* `wdtest` — watchdog self-test: a task that stops petting its heartbeat is
 * detected + killed by the M31 watchdog. */
extern void svc_demo_wdtest(void);
static void cmd_wdtest(void) { svc_demo_wdtest(); }

/* `crontab -l` / `cron [list|status|reload]` — M30 cron control. */
static void cmd_cron(const char* args) {
    if (!args || !*args || starts_with(args, "list") || starts_with(args, "status"))
        { cron_list(); return; }
    if (starts_with(args, "reload")) { cron_reload(); console_write("cron: reloaded\n"); return; }
    console_write("cron: usage: cron [list|status|reload]\n");
}

/* M25 stage 7 — run the in-tree-libc compiled-C user program embedded as a
 * blob (user/hello.c → static ELF → objcopy).  Weak symbols so the command
 * still links on arches that don't embed the blob yet (i386 is the reference
 * port today). */
extern const unsigned char _binary_user_hello_i386_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_i386_elf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_hello_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_hello_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_hello_aarch64_elf_end[]   __attribute__((weak));

static const unsigned char* hello_blob(size_t* len) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_hello_i386_elf_start)    { s = _binary_user_hello_i386_elf_start;    e = _binary_user_hello_i386_elf_end; }
    else if (_binary_user_hello_x86_64_elf_start)  { s = _binary_user_hello_x86_64_elf_start;  e = _binary_user_hello_x86_64_elf_end; }
    else if (_binary_user_hello_aarch64_elf_start) { s = _binary_user_hello_aarch64_elf_start; e = _binary_user_hello_aarch64_elf_end; }
    if (!s || !e) return 0;
    *len = (size_t)(e - s);
    return s;
}

static void cmd_libctest(void) {
    size_t len = 0;
    const unsigned char* start = hello_blob(&len);
    if (!start) {
        console_write("libctest: no user ELF embedded for this arch\n");
        return;
    }
    console_write("libctest: exec'ing compiled-C user ELF (in-tree libc)...\n");
    int rc = proc_exec_elf(start, len);
    kprintf("libctest: returned (rc=%d, %u bytes)\n", rc, (unsigned)len);
}

/* Tier B — `procspawn`: launch TWO copies of the spin demo as independent,
 * preemptible user processes; their interleaved output proves concurrent
 * ring-3 tasks time-sliced by the scheduler, each exiting on its own SYS_EXIT. */
extern const unsigned char _binary_user_spin_i386_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_spin_i386_elf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_spin_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_spin_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_spin_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_spin_aarch64_elf_end[]   __attribute__((weak));

/* Pick this arch's embedded spin blob (only one arch's is linked into the
 * image; the other weak symbols resolve to NULL). */
static const unsigned char* spin_blob(size_t* len) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_spin_i386_elf_start)    { s = _binary_user_spin_i386_elf_start;    e = _binary_user_spin_i386_elf_end; }
    else if (_binary_user_spin_x86_64_elf_start)  { s = _binary_user_spin_x86_64_elf_start;  e = _binary_user_spin_x86_64_elf_end; }
    else if (_binary_user_spin_aarch64_elf_start) { s = _binary_user_spin_aarch64_elf_start; e = _binary_user_spin_aarch64_elf_end; }
    if (!s || !e) return 0;
    *len = (size_t)(e - s);
    return s;
}

static void cmd_procspawn(void) {
    size_t len = 0;
    const unsigned char* start = spin_blob(&len);
    if (!start) {
        console_write("procspawn: no spin ELF embedded for this arch\n");
        return;
    }
    int a = proc_spawn("spin-a", start, len);
    int b = proc_spawn("spin-b", start, len);
    kprintf("procspawn: launched two user processes (pids %d, %d) — watch them interleave\n",
            a, b);
}

/* M34 slice A — `runargs [a b c ...]`: exec the args test program with an
 * argv built by the kernel; it prints argc + each argv from ring 3. */
extern const unsigned char _binary_user_args_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_args_i386_elf_end[]   __attribute__((weak));

static void cmd_runargs(const char* line) {
    if (!_binary_user_args_i386_elf_start) {
        console_write("runargs: args ELF not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_args_i386_elf_end - _binary_user_args_i386_elf_start);

    /* Split `line` into up to 15 whitespace-separated argv strings, in place
     * (a scratch copy).  argv[0] is the program name. */
    static char scratch[256];
    const char* argv[16];
    int argc = 0;
    argv[argc++] = "args";               /* argv[0] */

    int n = 0;
    while (line[n] && n < 255) { scratch[n] = line[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        argv[argc++] = &scratch[i];
        while (scratch[i] && scratch[i] != ' ') i++;
        if (scratch[i]) scratch[i++] = '\0';
    }

    kprintf("runargs: exec'ing args program with %d argv...\n", argc);
    int rc = proc_exec_elf_argv(_binary_user_args_i386_elf_start, len,
                                argc, (const char* const*)argv);
    kprintf("runargs: returned rc=%d\n", rc);
}

/* M34 slice B — `forktest`: exec a user program that fork()s, the child exits
 * with a code, and the parent waitpid()s for it. */
extern const unsigned char _binary_user_forktest_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forktest_i386_elf_end[]   __attribute__((weak));

static void cmd_forktest(void) {
    if (!_binary_user_forktest_i386_elf_start) {
        console_write("forktest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_forktest_i386_elf_end -
                          _binary_user_forktest_i386_elf_start);
    console_write("forktest: exec'ing fork()+waitpid() program...\n");
    int rc = proc_exec_elf(_binary_user_forktest_i386_elf_start, len);
    kprintf("forktest: returned rc=%d\n", rc);
}

/* M34 slice C — install the embedded user ELFs into the ramfs as /bin/<name>
 * so execve(path) can load them via the VFS.  Idempotent; called once from the
 * shell entry.  (The first real step toward a populated /bin.) */
static void bin_install_one(const char* path, const unsigned char* s,
                            const unsigned char* e) {
    if (!s || !e || e <= s) return;
    struct file* f = vfs_open(path, VFS_WRONLY | VFS_CREATE);
    if (!f) return;
    vfs_write(f, s, (size_t)(e - s));
    vfs_close(f);
}

void bin_install(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    vfs_mkdir("/bin");
    bin_install_one("/bin/args",  _binary_user_args_i386_elf_start,
                                  _binary_user_args_i386_elf_end);
    bin_install_one("/bin/hello", _binary_user_hello_i386_elf_start,
                                  _binary_user_hello_i386_elf_end);
}

/* M34 slice C — `forkexec`: fork()+execv(/bin/args)+waitpid() from ring 3. */
extern const unsigned char _binary_user_forkexec_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forkexec_i386_elf_end[]   __attribute__((weak));

static void cmd_forkexec(void) {
    if (!_binary_user_forkexec_i386_elf_start) {
        console_write("forkexec: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_forkexec_i386_elf_end -
                          _binary_user_forkexec_i386_elf_start);
    console_write("forkexec: exec'ing fork()+execv()+waitpid() program...\n");
    int rc = proc_exec_elf(_binary_user_forkexec_i386_elf_start, len);
    kprintf("forkexec: returned rc=%d\n", rc);
}

/* M34 slice D — `pipetest`: pipe()+dup2()+fork() from ring 3. */
extern const unsigned char _binary_user_pipetest_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pipetest_i386_elf_end[]   __attribute__((weak));

static void cmd_pipetest(void) {
    if (!_binary_user_pipetest_i386_elf_start) {
        console_write("pipetest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_pipetest_i386_elf_end -
                          _binary_user_pipetest_i386_elf_start);
    console_write("pipetest: exec'ing pipe()+dup2()+fork() program...\n");
    int rc = proc_exec_elf(_binary_user_pipetest_i386_elf_start, len);
    kprintf("pipetest: returned rc=%d\n", rc);
}

/* M34 slice E — `sigtest`: signal()+raise()+handler from ring 3. */
extern const unsigned char _binary_user_sigtest_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_sigtest_i386_elf_end[]   __attribute__((weak));

static void cmd_sigtest(void) {
    if (!_binary_user_sigtest_i386_elf_start) {
        console_write("sigtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_sigtest_i386_elf_end -
                          _binary_user_sigtest_i386_elf_start);
    console_write("sigtest: exec'ing signal()+raise() program...\n");
    int rc = proc_exec_elf(_binary_user_sigtest_i386_elf_start, len);
    kprintf("sigtest: returned rc=%d\n", rc);
}

/* M24 socket API — `dnstest`: resolve a hostname over a UDP socket from ring 3. */
extern const unsigned char _binary_user_dnstest_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_dnstest_i386_elf_end[]   __attribute__((weak));

static void cmd_dnstest(void) {
    if (!_binary_user_dnstest_i386_elf_start) {
        console_write("dnstest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_dnstest_i386_elf_end -
                          _binary_user_dnstest_i386_elf_start);
    console_write("dnstest: exec'ing UDP-socket DNS resolver...\n");
    int rc = proc_exec_elf(_binary_user_dnstest_i386_elf_start, len);
    kprintf("dnstest: returned rc=%d\n", rc);
}

/* M24 socket API — `httptest`: DNS + TCP-socket HTTP GET from ring 3. */
extern const unsigned char _binary_user_httptest_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_httptest_i386_elf_end[]   __attribute__((weak));

static void cmd_httptest(void) {
    if (!_binary_user_httptest_i386_elf_start) {
        console_write("httptest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_httptest_i386_elf_end -
                          _binary_user_httptest_i386_elf_start);
    console_write("httptest: exec'ing TCP-socket HTTP client...\n");
    int rc = proc_exec_elf(_binary_user_httptest_i386_elf_start, len);
    kprintf("httptest: returned rc=%d\n", rc);
}

/* M35 — `threadtest`: threads + a futex mutex from ring 3. */
extern const unsigned char _binary_user_threadtest_i386_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_threadtest_i386_elf_end[]   __attribute__((weak));

static void cmd_threadtest(void) {
    if (!_binary_user_threadtest_i386_elf_start) {
        console_write("threadtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_threadtest_i386_elf_end -
                          _binary_user_threadtest_i386_elf_start);
    console_write("threadtest: exec'ing threads + futex-mutex program...\n");
    int rc = proc_exec_elf(_binary_user_threadtest_i386_elf_start, len);
    kprintf("threadtest: returned rc=%d\n", rc);
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

/* M28 — dmesg: dump the klog ring, oldest → newest.  We render straight
 * to the console (NOT via kprintf) on purpose: kprintf tees into klog, so
 * printing the log with it would append every rendered line back into the
 * ring and evict the very boot messages we came to read. */
static void dmesg_put_uint(unsigned v) {
    char b[12];
    int n = 0;
    if (v == 0) { console_putchar('0'); return; }
    while (v) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) console_putchar(b[n]);
}

struct dmesg_ctx { int max_level; };

static void dmesg_line(const struct klog_record* r, void* ctx) {
    struct dmesg_ctx* d = (struct dmesg_ctx*)ctx;
    if ((int)r->level > d->max_level) return;          /* severity filter */
    unsigned sec = (unsigned)(r->t_ms / 1000);
    unsigned ms  = (unsigned)(r->t_ms % 1000);
    console_putchar('[');
    dmesg_put_uint(sec); console_putchar('.');
    if (ms < 100) console_putchar('0');
    if (ms <  10) console_putchar('0');
    dmesg_put_uint(ms);
    console_write("] ");
    console_write(klog_level_name(r->level)); console_putchar(' ');
    console_write(r->tag); console_write(": ");
    console_write(r->msg); console_putchar('\n');
}

/* Accept a level as a name (emerg..debug) or a digit (0..7). -1 = bad. */
static int dmesg_parse_level(const char* s) {
    static const char* const names[KLOG_NLEVELS] = {
        "emerg", "alert", "crit", "err", "warn", "notice", "info", "debug"
    };
    if (s[0] >= '0' && s[0] <= '7' && s[1] == '\0') return s[0] - '0';
    for (int i = 0; i < KLOG_NLEVELS; i++)
        if (streq(s, names[i])) return i;
    return -1;
}

static void cmd_dmesg(const char* args) {
    struct dmesg_ctx d = { .max_level = KLOG_DEBUG };  /* show everything */
    if (args && *args) {
        if (starts_with(args, "-l ")) {
            int lv = dmesg_parse_level(args + 3);
            if (lv < 0) {
                kprintf("dmesg: unknown level '%s' (emerg..debug or 0..7)\n",
                        args + 3);
                return;
            }
            d.max_level = lv;                          /* show <= this severity */
        } else {
            kprintf("usage: dmesg [-l <level>]   level: emerg..debug or 0..7\n");
            return;
        }
    }
    klog_for_each(dmesg_line, &d);
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
    /* M22: clear OUR VC, not the global sinks — after vc_init the fb
     * sink is inactive, so console_clear() had no visible effect in a
     * pane anyway; vc_clear also reaches GUI windows via the emit hook. */
    if (streq(line, "clear"))  { vc_clear(my_vc);  return; }
    /* M22: bring up the compositor + two shell windows.  Idempotent. */
    if (streq(line, "gui"))       { cmd_gui();       return; }
    if (streq(line, "gui stats")) { cmd_gui_stats(); return; }
    /* M22.2: GUI app registry access from any shell. */
    if (streq(line, "launch"))          { cmd_launch("");        return; }
    if (starts_with(line, "launch "))   { cmd_launch(line + 7);  return; }
    if (streq(line, "about"))  { cmd_about();      return; }
    if (streq(line, "lsmod"))  { module_list();    return; }
    if (streq(line, "lsdrv"))  { driver_list();    return; }
    if (streq(line, "lsconsole")) { console_list(); return; }
    if (streq(line, "uptime")) { cmd_uptime();      return; }
    if (streq(line, "dmesg"))         { cmd_dmesg("");         return; }
    if (starts_with(line, "dmesg "))  { cmd_dmesg(line + 6);   return; }

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
    if (streq(line, "mmtest"))         { cmd_mmtest();   return; }
    if (streq(line, "elftest"))        { cmd_elftest();  return; }
    if (streq(line, "userrun"))        { cmd_userrun();  return; }
    if (streq(line, "fdtest"))         { cmd_fdtest();   return; }
    if (streq(line, "shmtest"))        { cmd_shmtest();  return; }
    if (streq(line, "socktest"))       { cmd_socktest(); return; }
    if (streq(line, "polltest"))       { cmd_polltest(); return; }
    if (streq(line, "libctest"))       { cmd_libctest(); return; }
    if (streq(line, "procspawn"))      { cmd_procspawn(); return; }
    if (streq(line, "runargs"))        { cmd_runargs(""); return; }
    if (starts_with(line, "runargs ")) { cmd_runargs(line + 8); return; }
    if (streq(line, "forktest"))       { cmd_forktest(); return; }
    if (streq(line, "forkexec"))       { cmd_forkexec(); return; }
    if (streq(line, "pipetest"))       { cmd_pipetest(); return; }
    if (streq(line, "sigtest"))        { cmd_sigtest(); return; }
    if (streq(line, "dnstest"))        { cmd_dnstest(); return; }
    if (streq(line, "httptest"))       { cmd_httptest(); return; }
    if (streq(line, "threadtest"))     { cmd_threadtest(); return; }
    if (streq(line, "waittest"))       { cmd_waittest(); return; }
    if (streq(line, "service"))        { cmd_service("");        return; }
    if (starts_with(line, "service ")) { cmd_service(line + 8);  return; }
    if (streq(line, "bustest"))        { cmd_bustest(); return; }
    if (streq(line, "wdtest"))         { cmd_wdtest(); return; }
    if (streq(line, "cron"))           { cmd_cron("");         return; }
    if (starts_with(line, "cron "))    { cmd_cron(line + 5);   return; }
    if (streq(line, "crontab"))        { cron_list();          return; }
    if (starts_with(line, "crontab ")) { cron_list();          return; }  /* -l */
    if (streq(line, "blktest"))        { cmd_blktest();  return; }
    if (streq(line, "bctest"))         { cmd_bctest();   return; }
    if (streq(line, "lsblk"))          { blk_list();     return; }
    if (streq(line, "lsnic"))          { net_list();     return; }
    if (starts_with(line, "ping "))    { cmd_ping(line + 5); return; }
    if (starts_with(line, "arp "))     { cmd_arp(line + 4);  return; }
    if (starts_with(line, "nslookup ")){ cmd_dns(line + 9);  return; }
    if (starts_with(line, "wget "))    { cmd_wget(line + 5); return; }
    if (streq(line, "nettest"))        { cmd_nettest();  return; }
    if (streq(line, "lsaudio"))        { audio_list();   return; }
    if (streq(line, "beep"))           { cmd_beep();     return; }
    if (starts_with(line, "tone "))    { cmd_tone(line + 5); return; }
    if (streq(line, "ps"))             { task_list();    return; }
    if (starts_with(line, "kill "))    { cmd_kill(line + 5); return; }
    if (streq(line, "spawn"))          { cmd_spawn();    return; }
    if (streq(line, "yield"))          { task_yield();   return; }
    if (streq(line, "loop"))           { cmd_loop();     return; }
    if (starts_with(line, "run "))     { cmd_run(my_vc, line + 4); return; }
    if (streq(line, "pane"))           { cmd_pane(my_vc, "");      return; }
    if (starts_with(line, "pane "))    { cmd_pane(my_vc, line + 5); return; }
    if (streq(line, "lslayout"))       { cmd_lslayout();             return; }
    if (starts_with(line, "setlayout ")) { cmd_setlayout(line + 10); return; }
    if (streq(line, "lscpu"))          { cmd_lscpu();                return; }
    if (starts_with(line, "taskset "))  { cmd_taskset(line + 8);     return; }
    if (streq(line, "slabinfo"))       { cmd_slabinfo();             return; }
    if (streq(line, "buddyinfo"))      { cmd_buddyinfo();            return; }
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
void bin_install(void);   /* defined above — installs the /bin entries */

void shell_run(struct vc* v) {
    char line[LINE_MAX];
    bin_install();                       /* M34 — populate /bin for execve() */
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
/* §S.1 — this full-featured shell is just one registered provider.
 * Alternatives (rescue_shell.c) register the same way; spawn sites
 * pick via shell_provider_active(). */
SHELL_PROVIDER("d-os", shell_task_entry);

const struct shell_provider* shell_provider_active(void) {
    const char* want = config_get("shell.provider", "d-os");
    for (int pass = 0; pass < 2; pass++) {
        const char* name = pass == 0 ? want : "d-os";
        for (int i = 0; i < shell_provider_count(); i++) {
            const struct shell_provider* p = shell_provider_at(i);
            if (streq(p->name, name)) return p;
        }
    }
    return shell_provider_at(0);        /* shell.c is linked → never NULL */
}

void shell_task_entry(void) {
    struct task* me = task_current();
    struct vc*   v  = me ? (struct vc*)me->out_console : NULL;
    if (!v) {
        kprintf("shell_task_entry: no VC bound — exiting\n");
        return;
    }
    shell_run(v);
}
