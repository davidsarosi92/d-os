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
#include "hal_api.h"   /* phys_to_virt / virt_to_phys — kernel direct map */
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
#include "drvuser.h"   /* §M33 Tier 1 — drvtest */
#include "iommu.h"     /* §M33 stage 5 — the `iommu` report */
void edu_test(void);
void edu_escape(uint64_t phys);
#include "modload.h"   /* §M67 — insmod / rmmod / lsmod */
#include "ksym.h"      /* §M67 — ksyms */
#include "timer.h"
#include "ktimer.h"
#include "vfs.h"
#include "random.h"
#include "devtools.h"
#include "config.h"
#include "usermode.h"
#include "task.h"
#include "block.h"
#include "block_cache.h"
#include "net.h"
#include "dhcp.h"
#include "net_cmds.h"
#include "audio.h"
#include "pkg.h"
#include "wayland.h"
#include "vc.h"
#include "lock.h"
#include "keymap.h"
#include "percpu.h"
#include "slab.h"
#include "gui.h"
#include "ui.h"            /* §M65 — `ui` prints the class registry */
#include "uaccess.h"   /* §1.1 — faulttest exercises the exception table */
#include "gui_app.h"
#include "wallpaper.h"   /* §M60 — the `wallpaper` command lives in wallpaper.c */
#include "shortcut.h"    /* §M64 — desktop shortcuts, likewise shared */
#include "settings.h"    /* §M63 — `conf`: declared keys, validated */
#include "splash.h"
#include "clipboard.h"   /* §M58/§M59 — `clip` */
#include "shell_provider.h"
#include "basic.h"
#include "klog.h"
#include "watchdog.h"
#include "elf.h"
#include "crash.h"     /* §M47 — the `crash` report list */
#include "proc.h"
#include "syscall.h"
#include "epoll.h"       /* §M56 — EPOLL_CTL_*, EPOLLET */
#include "workqueue.h"
#include "abi.h"        /* §M50 — guest-ABI translation tables */  /* §M49 — deferred-work pool + wqtest */
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
                  "  ringtest, ps, spawn, yield, loop, kill <pid>, fkill <pid>\n"
                  "  wedge (runaway ring-3 task), faulttest (bad user pointers)\n"
                  "  killstorm [rounds] [tasks] (kill blocked tasks under SMP)\n"
                  "  rqcheck                (check the runqueue invariant now)\n"
                  "  schedstorm [rounds]    (affinity+nice churn, audited)\n"
                  "  timerfdtest [ms], alarmtest [ms] (timing, M53 stage 3)\n"
                  "  netstorm [n] (n tasks waiting on the network at once)\n"
                  "  tcptest [n] (echo server + n concurrent clients over lo)\n"
                  "  netstat (the TCP connection table), lo drop <permille>\n"
                  "  dhcp [dev|status] (ask the network for an address)\n"
                  "  tcploss [permille] [kb] (a stream that survives loss)\n"
                  "  epolltest, epollmusltest (readiness sets, M56)\n"
                  "  netmuslserv (bind/listen/accept through real musl)\n"
                  "  fputest (per-task FP/SIMD register file)\n"
                  "  archtest (ELF arch gate), crash (what has gone wrong)\n"
                  "  pane, pane split horizontal|vertical\n"
                  "  gui (compositor + desktop), gui stop, gui stats, launch [app]\n"
                  "  termcheck (terminal scrollback self-test, in a GUI window)\n"
                  "  wallpaper [gradient|solid:RRGGBB|<path.bmp>|fit <mode>]\n"
                  "  shortcut [list|add <name> <target> [icon]|rm <name>]\n"
                  "  conf [list|show <key>|set <key> <value>]\n"
                  "  clip [show|paste [primary]|copy <text>|promote]\n"
                  "  mode [list|<w>x<h> [--force]|confirm|revert]\n"
                  "  splash [on|off|status]\n"
                  "  run <path.bas> (Tiny-BASIC)\n"
                  "  lslayout, setlayout <us|hu|...>, lscpu, taskset <pid> <mask>\n"
                  "  sched [ms] (how work is spread over the CPUs), loop [n], loopstop\n"
                  "  nice <pid> <-20..19> (scheduling priority), wqtest [n], abi\n"
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
/* §M54 — `killstorm [rounds] [tasks]`: kill BLOCKED tasks, hard and     */
/* often, on every CPU at once.                                          */
/*                                                                       */
/* WHAT IT IS FOR.  Killing a task that is asleep is a three-party       */
/* operation: the killer wakes it (state → RUNNABLE, enqueue), some CPU  */
/* picks it up and it exits (DEAD, dequeue), and the reaper frees it.    */
/* Those run concurrently on different CPUs, and if a wake lands between */
/* the exit and the free, the freed task stays linked in a runqueue —    */
/* after which the next schedule() on that CPU walks into freed memory   */
/* and the machine dies in the scheduler, with nothing left to say why.  */
/* That is the fault this test exists to make ordinary and repeatable:   */
/* it took a crashed browser and a reboot to produce it by accident.     */
/*                                                                       */
/* Each victim parks in task_msleep (a real block since §M49), so the    */
/* kill has to go through the wake path rather than being noticed by a   */
/* task that was runnable all along.  A pass is silence: no fault, and   */
/* no "STILL QUEUED" report from task_reap's sweep.                      */
/* -------------------------------------------------------------------- */

static volatile int g_ks_alive;
static volatile int g_ks_round;          /* progress, readable from outside */
static volatile int g_ks_done;
static volatile int g_ks_rounds, g_ks_per;
static volatile int g_ks_spawned, g_ks_killed;

static void killstorm_victim(void) {
    while (!task_should_stop()) task_msleep(2);
    __atomic_sub_fetch(&g_ks_alive, 1, __ATOMIC_RELAXED);
}

/* The storm runs on its OWN task, not on the shell.  If it stalls, the shell
 * is still there to say so and to dump the task table — a test that hangs
 * along with the thing it is testing reports nothing at all, which is exactly
 * how two runs of this were wasted. */
static void killstorm_driver(void) {
    int rounds = g_ks_rounds, per = g_ks_per;
    for (int r = 0; r < rounds; r++) {
        int pids[32];
        int n = 0;
        for (int i = 0; i < per; i++) {
            struct task* t = task_spawn("ks-victim", killstorm_victim);
            if (!t) break;
            pids[n++] = t->pid;
            __atomic_add_fetch(&g_ks_alive, 1, __ATOMIC_RELAXED);
        }
        __atomic_add_fetch(&g_ks_spawned, n, __ATOMIC_RELAXED);
        /* Let them actually reach the blocked state — killing a task that has
         * not parked yet exercises a different (easier) path. */
        task_msleep(6);
        for (int i = 0; i < n; i++) task_kill(pids[i]);
        __atomic_add_fetch(&g_ks_killed, n, __ATOMIC_RELAXED);
        task_msleep(10);
        __atomic_store_n(&g_ks_round, r + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_ks_done, 1, __ATOMIC_RELEASE);
}

static void cmd_killstorm(const char* args) {
    int rounds = 0, per = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) rounds = rounds * 10 + (*args - '0');
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) per = per * 10 + (*args - '0');
    if (rounds <= 0) rounds = 20;
    if (per    <= 0) per    = 8;
    if (per > 32) per = 32;

    g_ks_round = g_ks_done = g_ks_spawned = g_ks_killed = 0;
    g_ks_rounds = rounds; g_ks_per = per;
    kprintf("killstorm: %d rounds x %d blocked tasks (cpus=%d)\n",
            rounds, per, smp_ncpus());
    struct task* drv = task_spawn("ks-driver", killstorm_driver);
    if (!drv) { console_write("killstorm: cannot spawn driver\n"); return; }
    int drvpid = drv->pid;

    /* Poll for completion, and give up on NO PROGRESS rather than on a total
     * time budget: a slow machine finishes late, a broken one stops moving. */
    int last = -1, stuck = 0;
    while (!__atomic_load_n(&g_ks_done, __ATOMIC_ACQUIRE)) {
        task_msleep(10);
        int now = __atomic_load_n(&g_ks_round, __ATOMIC_ACQUIRE);
        if (now == last) { if (++stuck > 400) break; }   /* ~4 s of silence */
        else             { last = now; stuck = 0; }
    }

    int done = __atomic_load_n(&g_ks_done, __ATOMIC_ACQUIRE);
    kprintf("killstorm: %s — %d/%d rounds, %d spawned, %d killed, %d alive\n",
            done ? "done" : "STALLED",
            __atomic_load_n(&g_ks_round, __ATOMIC_ACQUIRE), rounds,
            g_ks_spawned, g_ks_killed,
            __atomic_load_n(&g_ks_alive, __ATOMIC_RELAXED));
    if (!done) {
        /* The whole point of running the storm elsewhere: report what the
         * scheduler was holding when it stopped making progress. */
        console_write("killstorm: task table at the stall —\n");
        task_list();
        task_kill(drvpid);
    }
}

/* -------------------------------------------------------------------- */
/* §M57 — `rqcheck` and `schedstorm [rounds]`.                           */
/*                                                                       */
/* WHY THESE EXIST.  Every defect in the §M54/§M57 family is a           */
/* disagreement between where a task IS and where the kernel thinks it   */
/* is, and none of them announces itself: the symptom is a fault in the  */
/* scheduler, a shell that stops, or a "STILL QUEUED" line at reap time  */
/* — each arbitrarily far from the code that caused it, and each rare    */
/* enough to be dismissed as a one-off.  §M54's own notes close with     */
/* "roughly once in several hundred kills", which is a confession that   */
/* the evidence was a log line rather than a measurement.                */
/*                                                                       */
/* So the invariant is stated in code (task_rq_audit) and CHECKED.  A    */
/* violation is then a fact, reproducible in seconds, that names the     */
/* rule it broke — instead of a crash three subsystems downstream.       */
/*                                                                       */
/* `killstorm` already churns spawn/kill/wake.  It does NOT touch the    */
/* two paths §M57 found broken (`taskset` and `nice` both act on the     */
/* queue an UNLOCKED cpu_home read names), which is precisely why those  */
/* two survived §M54 untouched: nothing exercised them under load.       */
/* schedstorm does, from several CPUs at once, against tasks that are    */
/* simultaneously being migrated by the balancer.                        */
/*                                                                       */
/* The final audit is taken AT REST.  Rule 5 (ready, unqueued, running   */
/* nowhere) has a legitimate transient while a task is being re-homed,   */
/* so reading it mid-churn would produce a test that fails for a reason  */
/* that is not a bug — the fastest way to teach everyone to ignore it.   */
/* -------------------------------------------------------------------- */

static void rq_audit_print(const char* tag, const struct rq_audit* a, int bad) {
    /* Structural verdict first, because that is the pass/fail one; the two
     * soft counters follow, labelled, so a stale estimate can never be
     * mistaken for a corrupted runqueue. */
    kprintf("%s: %d queued | struct: home %d orphan %d count %d ring %d -> %s"
            " | soft: lost %d load %d\n",
            tag, a->tasks_queued, a->bad_home, a->orphan_home, a->bad_count,
            a->broken_ring, bad ? "VIOLATIONS" : "consistent",
            a->lost, a->bad_load);
}

static void cmd_rqcheck(const char* args) {
    (void)args;
    struct rq_audit a;
    int bad = task_rq_audit(&a);
    rq_audit_print("rqcheck", &a, bad);
}

static volatile int g_ss_rounds, g_ss_done, g_ss_worst, g_ss_churn;
static volatile int g_ss_pids[12], g_ss_npids, g_ss_stop, g_ss_churners;
static struct rq_audit g_ss_worst_snap;

/* SEVERAL churners, not one.  The first version of this test drove affinity
 * and nice from a single task and reported `ok` even against the PRE-§M57
 * code — because the only thing it could race with was the periodic balancer,
 * which fires every LOAD_BALANCE_INTERVAL_MS, so a 240 ms run offered about
 * two chances to hit a window measured in instructions.  A test that cannot
 * fail is not evidence.
 *
 * Two tasks re-homing the SAME task is the same bug with a window orders of
 * magnitude wider: both read cpu_home, both decide, and the second acts on a
 * queue the first has already moved the task off — which is precisely what
 * "act on the queue an unlocked read named" means.  It is also the realistic
 * case: a shell, the Task Manager and a script can all call taskset. */
static void schedstorm_churner(void) {
    struct task* self = task_current();
    unsigned seed = (unsigned)(self ? self->pid : 1) * 2654435761u;
    uint32_t ncpu = (uint32_t)smp_ncpus();
    while (!__atomic_load_n(&g_ss_stop, __ATOMIC_ACQUIRE) && !task_should_stop()) {
        int n = __atomic_load_n(&g_ss_npids, __ATOMIC_ACQUIRE);
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            int pid = g_ss_pids[i];
            struct task* t = task_find(pid);
            if (!t) continue;
            uint32_t mask = ((seed >> 8) & ((1u << ncpu) - 1u));
            if (!mask) mask = 1u;
            task_set_affinity(t, mask);
            task_set_nice(pid, (int)((seed >> 16) % 21u) - 10);
            __atomic_add_fetch(&g_ss_churn, 2, __ATOMIC_RELAXED);
        }
        task_yield();
    }
    __atomic_sub_fetch(&g_ss_churners, 1, __ATOMIC_ACQ_REL);
}

/* A hog, so the balancer has something worth migrating.  A queue of sleepers
 * is never rebalanced (§M49: load is demand, not queue length), and an
 * unmigrated task cannot expose a migration bug. */
static void schedstorm_hog(void) {
    volatile unsigned x = 0;
    while (!task_should_stop()) { for (int i = 0; i < 20000; i++) x += i; task_yield(); }
}

#define SS_CHURNERS 4

static void schedstorm_driver(void) {
    int rounds = g_ss_rounds;
    int pids[12];
    int n = 0;
    for (int i = 0; i < 12; i++) {
        struct task* t = task_spawn("ss-hog", schedstorm_hog);
        if (!t) break;
        pids[n++] = t->pid;
        g_ss_pids[i] = t->pid;
    }
    __atomic_store_n(&g_ss_npids, n, __ATOMIC_RELEASE);

    __atomic_store_n(&g_ss_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ss_churners, 0, __ATOMIC_RELEASE);
    for (int i = 0; i < SS_CHURNERS; i++) {
        if (task_spawn("ss-churn", schedstorm_churner))
            __atomic_add_fetch(&g_ss_churners, 1, __ATOMIC_ACQ_REL);
    }

    for (int r = 0; r < rounds; r++) {
        /* One transient task per round, so spawn/exit/reap runs alongside the
         * affinity churn — the reaper's sweep and the balancer's migration are
         * the two things that must not disagree about where a task is. */
        struct task* tmp = task_spawn("ss-tmp", schedstorm_hog);
        if (tmp) { int p = tmp->pid; task_msleep(2); task_kill(p); }

        struct rq_audit a;
        /* task_rq_audit returns the STRUCTURAL count; rules 5 and 6 are
         * reported in `a` and checked at rest below. */
        int hard = task_rq_audit(&a);
        if (hard > __atomic_load_n(&g_ss_worst, __ATOMIC_RELAXED)) {
            __atomic_store_n(&g_ss_worst, hard, __ATOMIC_RELAXED);
            /* Keep the WORST SNAPSHOT, not just its size.  "4 violations" does
             * not say which rule, and the rule is the entire diagnosis. */
            g_ss_worst_snap = a;
        }
        task_msleep(4);
    }

    /* Stop the churners and WAIT for them: an affinity call still in flight
     * while the audit is taken at rest would be the transient the "at rest"
     * reading exists to exclude. */
    __atomic_store_n(&g_ss_stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 200 && __atomic_load_n(&g_ss_churners, __ATOMIC_ACQUIRE); i++)
        task_msleep(10);

    for (int i = 0; i < n; i++) {
        struct task* t = task_find(pids[i]);
        if (t) task_set_affinity(t, 0xFFFFFFFFu);
        task_set_nice(pids[i], 0);
    }
    for (int i = 0; i < n; i++) task_kill(pids[i]);
    __atomic_store_n(&g_ss_done, 1, __ATOMIC_RELEASE);
}

static void cmd_schedstorm(const char* args) {
    int rounds = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) rounds = rounds * 10 + (*args - '0');
    if (rounds <= 0) rounds = 60;

    struct rq_audit before;
    int bad0 = task_rq_audit(&before);
    rq_audit_print("schedstorm: before", &before, bad0);

    g_ss_rounds = rounds; g_ss_done = 0; g_ss_worst = 0; g_ss_churn = 0;
    kprintf("schedstorm: %d rounds, %d concurrent churners, %d cpus\n",
            rounds, SS_CHURNERS, smp_ncpus());

    struct task* drv = task_spawn("ss-driver", schedstorm_driver);
    if (!drv) { console_write("schedstorm: cannot spawn driver\n"); return; }

    /* Same no-progress rule as killstorm: a slow box finishes late, a broken
     * one stops moving. */
    int last = -1, stuck = 0;
    while (!__atomic_load_n(&g_ss_done, __ATOMIC_ACQUIRE)) {
        task_msleep(10);
        int now = __atomic_load_n(&g_ss_churn, __ATOMIC_RELAXED);
        if (now == last) { if (++stuck > 800) break; }      /* ~8 s of silence */
        else             { last = now; stuck = 0; }
    }
    int done = __atomic_load_n(&g_ss_done, __ATOMIC_ACQUIRE);

    /* AT REST — every rule must hold now, rule 5 included.  Give the reaper a
     * moment first: a task that has just exited is legitimately not on a queue
     * and not yet freed. */
    task_msleep(200);
    struct rq_audit after;
    int bad1 = task_rq_audit(&after);
    rq_audit_print("schedstorm: after ", &after, bad1);

    int worst = __atomic_load_n(&g_ss_worst, __ATOMIC_RELAXED);
    if (worst) rq_audit_print("schedstorm: worst ", &g_ss_worst_snap, worst);
    kprintf("schedstorm: %s — %d churn ops, worst structural mid-churn %d\n",
            done ? "done" : "STALLED",
            __atomic_load_n(&g_ss_churn, __ATOMIC_RELAXED), worst);
    /* Pass = structurally perfect THROUGHOUT, and everything (rules 5 and 6
     * included) exact once the churn has stopped. */
    int pass = done && worst == 0 && bad1 == 0 && after.lost == 0 && after.bad_load == 0;
    kprintf("schedstorm: %s\n", pass ? "ok" : "FAIL");
}

/* -------------------------------------------------------------------- */
/* §M53 stage 3 — `timerfdtest`: prove that a deadline can be WAITED FOR  */
/* alongside I/O, and that a periodic one does not drift.                */
/*                                                                       */
/* Two things are measured rather than asserted.  (1) The wait goes       */
/* through poll(2), not through a sleep — that is the whole point of the  */
/* descriptor, and a timerfd that woke only its own reader would pass a   */
/* read-based test and still be useless to an event loop.  (2) The error  */
/* reported is the error against the ORIGINAL start, not against the      */
/* previous tick: a timer that re-arms from "now" looks perfect tick to   */
/* tick and drifts without bound over a minute, and only the cumulative   */
/* number shows it.                                                      */
/* -------------------------------------------------------------------- */
static void cmd_timerfdtest(const char* args) {
    unsigned period_ms = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) period_ms = period_ms * 10 + (unsigned)(*args - '0');
    if (period_ms == 0) period_ms = 50;

    int fd = sys_timerfd_create();
    if (fd < 0) { console_write("timerfd: create failed\n"); return; }

    uint64_t period_ns = (uint64_t)period_ms * 1000000ull;
    uint64_t t0 = timer_now_ns();
    if (sys_timerfd_settime(fd, 0 /* relative */, period_ns, period_ns) != 0) {
        console_write("timerfd: settime failed\n");
        sys_close(fd);
        return;
    }
    kprintf("timerfd: fd %d, period %u ms — waiting via poll(2)\n", fd, period_ms);

    for (int i = 1; i <= 5; i++) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = sys_poll_k(&pf, 1, -1);         /* block until readable */
        uint64_t now = timer_now_ns();
        uint64_t buf = 0;
        long got = sys_read_k(fd, &buf, sizeof buf);

        /* Error against the START, not against the previous tick — see above. */
        uint64_t want = t0 + (uint64_t)i * period_ns;
        long long err_us = (long long)((now - want) / 1000ull);
        if (now < want) err_us = -(long long)((want - now) / 1000ull);

        kprintf("  tick %d: poll=%d revents=%x read=%d expirations=%u  drift %s%u us\n",
                i, r, (unsigned)pf.revents, (int)got, (unsigned)buf,
                err_us < 0 ? "-" : "+",
                (unsigned)(err_us < 0 ? -err_us : err_us));
    }

    uint64_t rem = 0, iv = 0;
    sys_timerfd_gettime_k(fd, &rem, &iv);
    kprintf("  gettime: %u us to go, interval %u us\n",
            (unsigned)(rem / 1000), (unsigned)(iv / 1000));
    sys_close(fd);
    console_write("timerfd: ok\n");
}

/* -------------------------------------------------------------------- */
/* §M56 — `epolltest`: a readiness set, and a timeout that is real.      */
/*                                                                       */
/* Two claims, both falsifiable by the clock:                            */
/*                                                                       */
/*  1. A FINITE TIMEOUT WAITS.  poll(2) with `timeout > 0` used to be     */
/*     treated as a snapshot — documented as such, but a program asking   */
/*     to wait 200 ms got an immediate 0, so every correct event loop     */
/*     written against it became a busy loop.  The test measures the      */
/*     elapsed time and fails if the call came back early.                */
/*                                                                       */
/*  2. ONE WAIT SERVES SEVERAL SOURCES.  A timerfd and a pipe go into one */
/*     epoll set, and the loop is woken by whichever is ready — which is  */
/*     the entire point of registering a set instead of asking about one  */
/*     descriptor at a time.                                              */
/* -------------------------------------------------------------------- */

static void cmd_epolltest(void) {
    /* --- 1. a finite timeout on an empty set must actually wait --------- */
    int ep = sys_epoll_create();
    if (ep < 0) { console_write("epoll: create failed\n"); return; }

    uint64_t t0 = timer_now_ns();
    uint64_t evbuf[2 * 8];
    int n = sys_epoll_wait_k(ep, evbuf, 8, 200);
    uint64_t waited_us = (timer_now_ns() - t0) / 1000ull;
    kprintf("epoll: empty set, 200 ms timeout -> n=%d after %u us\n",
            n, (unsigned)waited_us);
    int ok = (n == 0 && waited_us >= 150000ull);
    if (!ok) console_write("epoll: FAIL (a finite timeout returned early)\n");

    /* --- 2. a timerfd in the set wakes the wait ------------------------- */
    int tfd = sys_timerfd_create();
    if (tfd < 0) { console_write("epoll: timerfd failed\n"); sys_close(ep); return; }
    sys_timerfd_settime(tfd, 0, 80000000ull, 0);         /* one-shot, 80 ms */

    uint64_t ev[2] = { POLLIN, 0xABCDEF01ull };          /* events, cookie */
    if (sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, tfd, (uint32_t)ev[0], ev[1]) != 0)
        console_write("epoll: FAIL (ctl ADD)\n");

    t0 = timer_now_ns();
    n = sys_epoll_wait_k(ep, evbuf, 8, 5000);
    waited_us = (timer_now_ns() - t0) / 1000ull;
    kprintf("epoll: timerfd -> n=%d events=%x data=%x%x after %u us\n",
            n, (unsigned)evbuf[0],
            (unsigned)(evbuf[1] >> 32), (unsigned)evbuf[1],
            (unsigned)waited_us);
    if (n != 1 || evbuf[1] != 0xABCDEF01ull) {
        console_write("epoll: FAIL (timerfd not reported, or cookie mangled)\n");
        ok = 0;
    }
    /* The cookie is the whole ergonomics of epoll: the kernel hands back the
     * caller's own pointer-sized token, so a loop does not have to search its
     * own tables for which connection an fd belongs to. */

    uint64_t drained = 0;
    sys_read_k(tfd, &drained, sizeof drained);

    /* --- 3. a pipe in the SAME set ------------------------------------- */
    int pfds[2];
    if (sys_pipe(pfds) == 0) {
        if (sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, pfds[0], POLLIN, 0x2222ull) != 0)
            console_write("epoll: FAIL (ctl ADD pipe)\n");

        /* Nothing written yet: the set must NOT report the pipe. */
        n = sys_epoll_wait_k(ep, evbuf, 8, 50);
        if (n != 0) { kprintf("epoll: FAIL (spurious ready, n=%d)\n", n); ok = 0; }

        sys_write_k(pfds[1], "x", 1);
        n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
        kprintf("epoll: pipe -> n=%d data=%x\n", n, (unsigned)evbuf[1]);
        if (n != 1 || evbuf[1] != 0x2222ull) {
            console_write("epoll: FAIL (pipe not reported)\n"); ok = 0;
        }
        /* DEL before close, always.  A set watches descriptor NUMBERS, so a
         * closed-and-reused fd silently inherits the old registration — the
         * first version of this test skipped the DEL, the next pipe got the
         * same fd number, its ADD failed with -EEXIST, and the stale entry's
         * narrower event mask made the kernel look like it was dropping
         * POLLRDHUP.  The hazard is real; it just belonged to the test. */
        sys_epoll_ctl_k(ep, EPOLL_CTL_DEL, pfds[0], 0, 0);
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 3b. hangup: closing the writer must be VISIBLE without reading -- */
    if (sys_pipe(pfds) == 0) {
        if (sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, pfds[0],
                            POLLIN | EPOLLRDHUP, 0x3333ull) != 0) {
            console_write("epoll: FAIL (ctl ADD for the hangup case)\n"); ok = 0;
        }
        sys_write_k(pfds[1], "z", 1);
        sys_close(pfds[1]);                      /* writer gone, one byte left */

        n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
        kprintf("epoll: after writer close -> n=%d events=%x\n",
                n, n > 0 ? (unsigned)evbuf[0] : 0);
        /* The byte is still there, so POLLIN AND RDHUP — but NOT yet HUP:
         * a reader must be able to drain the tail before it shuts down. */
        if (n != 1 || !(evbuf[0] & POLLIN) || !(evbuf[0] & POLLRDHUP)
                   || (evbuf[0] & POLLHUP)) {
            console_write("epoll: FAIL (hangup reported wrong with data left)\n");
            ok = 0;
        }
        char c = 0;
        sys_read_k(pfds[0], &c, 1);              /* drain it */
        n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
        kprintf("epoll: after drain      -> n=%d events=%x\n",
                n, n > 0 ? (unsigned)evbuf[0] : 0);
        if (n != 1 || !(evbuf[0] & POLLHUP)) {
            console_write("epoll: FAIL (POLLHUP not reported once drained)\n");
            ok = 0;
        }
        sys_epoll_ctl_k(ep, EPOLL_CTL_DEL, pfds[0], 0, 0);
        sys_close(pfds[0]);
    }

    /* --- 3c. O_NONBLOCK on a PIPE, which used to be silently ignored ---- */
    if (sys_pipe(pfds) == 0) {
        sys_socket_setnonblock(pfds[0], 1);
        char c = 0;
        long r = sys_read_k(pfds[0], &c, 1);     /* empty, writer alive */
        kprintf("epoll: nonblocking empty pipe read -> %d (want EAGAIN=%d)\n",
                (int)r, -SOCK_EAGAIN);
        /* Must be EAGAIN and NOT 0: zero means end of file, and a drain loop
         * told "EOF" by a live-but-empty pipe stops for good. */
        if (r != -SOCK_EAGAIN) {
            console_write("epoll: FAIL (O_NONBLOCK ignored on a pipe)\n");
            ok = 0;
        }
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 4. edge-triggered is REFUSED, not silently downgraded --------- */
    int rc = sys_epoll_ctl_k(ep, EPOLL_CTL_MOD, tfd, POLLIN | EPOLLET, 0);
    if (rc >= 0) {
        console_write("epoll: FAIL (EPOLLET accepted — a program written for "
                      "it would spin)\n");
        ok = 0;
    } else {
        kprintf("epoll: EPOLLET refused with %d (correct)\n", rc);
    }

    /* --- 4b. the readiness memo must never hide a transition ------------ */
    /*
     * §M56.2 caches each item's readiness against the description's generation
     * counter, so a scan re-evaluates only the descriptors that moved.  The
     * failure mode of a missing generation bump is not slowness — it is an
     * event that never arrives, and it would show up long after the change
     * that caused it.  So: drive a pipe through many ready/not-ready
     * transitions and insist that every single one is seen.
     */
    if (sys_pipe(pfds) == 0) {
        sys_epoll_ctl_k(ep, EPOLL_CTL_ADD, pfds[0], POLLIN, 0x4444ull);
        int seen_ready = 0, seen_idle = 0;
        for (int round = 0; round < 20; round++) {
            char c = 'a';
            sys_write_k(pfds[1], &c, 1);
            n = sys_epoll_wait_k(ep, evbuf, 8, 1000);
            if (n == 1 && (evbuf[0] & POLLIN) && evbuf[1] == 0x4444ull) seen_ready++;

            sys_read_k(pfds[0], &c, 1);          /* drain → not readable again */
            n = sys_epoll_wait_k(ep, evbuf, 8, 0);
            if (n == 0) seen_idle++;
        }
        kprintf("epoll: memo check — %d/20 ready, %d/20 idle transitions seen\n",
                seen_ready, seen_idle);
        if (seen_ready != 20 || seen_idle != 20) {
            console_write("epoll: FAIL (the readiness cache hid a transition)\n");
            ok = 0;
        }
        sys_epoll_ctl_k(ep, EPOLL_CTL_DEL, pfds[0], 0, 0);
        sys_close(pfds[0]); sys_close(pfds[1]);
    }

    /* --- 5. what does the SCAN actually cost? --------------------------- */
    /*
     * epoll's reputation is O(ready); ours scans the registered set.  Rather
     * than argue about whether that matters here, measure it: fill the set
     * with timerfds that will never fire and time a non-blocking wait.  The
     * number is what decides whether per-fd wakeups are worth the cross-object
     * lifetime coupling they require — and it is printed rather than asserted
     * so a future reader can re-decide with their own workload.
     */
    int bench = sys_epoll_create();
    if (bench >= 0) {
        int fds[48]; int nf = 0;
        for (int i = 0; i < 48; i++) {
            int t = sys_timerfd_create();
            if (t < 0) break;
            fds[nf++] = t;
            sys_epoll_ctl_k(bench, EPOLL_CTL_ADD, t, POLLIN, (uint64_t)i);
        }
        /* One warm-up wait so the memo is populated, then measure the steady
         * state: this is what a real loop pays on every iteration where most
         * of its descriptors have not moved. */
        sys_epoll_wait_k(bench, evbuf, 8, 0);
        uint64_t b0 = timer_now_ns();
        for (int i = 0; i < 100; i++) sys_epoll_wait_k(bench, evbuf, 8, 0);
        uint64_t per_ns = (timer_now_ns() - b0) / 100ull;
        kprintf("epoll: scan of %d registered fds costs %u ns per wait\n",
                nf, (unsigned)per_ns);
        for (int i = 0; i < nf; i++) sys_close(fds[i]);
        sys_close(bench);
    }

    sys_close(tfd);
    sys_close(ep);
    console_write(ok ? "epoll: ok\n" : "epoll: FAILED\n");
}

/* §M53 stage 3 — `alarmtest`: the OTHER delivery.  A program with nothing to
 * poll wants to be interrupted, not woken, so this arms an interval timer and
 * watches SIGALRM land in the caller's pending set. */
static void cmd_alarmtest(const char* args) {
    unsigned ms = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) ms = ms * 10 + (unsigned)(*args - '0');
    if (ms == 0) ms = 100;

    struct task* me = task_current();
    if (!me) return;
    me->sig_pending &= ~(1u << SIGALRM);

    uint64_t t0 = timer_now_ns();
    if (sys_setitimer_ns((uint64_t)ms * 1000000ull, 0) != 0) {
        console_write("alarm: setitimer failed\n");
        return;
    }
    kprintf("alarm: armed for %u ms — waiting for SIGALRM to be posted\n", ms);

    /* A kernel task never takes a return-to-user trip, so it observes the
     * PENDING BIT rather than a handler call.  Same signal, same posting
     * path — only the delivery point differs, and that difference is what
     * this test deliberately does not pretend to cover. */
    for (int i = 0; i < 400; i++) {
        if (me->sig_pending & (1u << SIGALRM)) break;
        task_msleep(5);
    }
    uint64_t dt = timer_now_ns() - t0;
    if (me->sig_pending & (1u << SIGALRM)) {
        me->sig_pending &= ~(1u << SIGALRM);
        kprintf("alarm: SIGALRM posted after %u us (asked for %u us)\n",
                (unsigned)(dt / 1000), ms * 1000);
        console_write("alarm: ok\n");
    } else {
        console_write("alarm: FAIL — SIGALRM never arrived\n");
    }
    sys_setitimer_ns(0, 0);
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

/* §M46 — force-kill: reclaims a WEDGED ring-3 task (one spinning in userland
 * that never reaches a cooperative yield, so plain `kill` can't touch it).  It
 * dies at its next timer preemption in user mode. */
static void cmd_fkill(const char* args) {
    int pid = 0, any = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) { pid = pid * 10 + (*args - '0'); any = 1; }
    if (!any) { console_write("fkill: usage: fkill <pid>\n"); return; }
    if (task_current() && task_current()->pid == pid) {
        console_write("fkill: refusing to kill the calling shell\n");
        return;
    }
    if (task_force_kill(pid) == 0) kprintf("fkill: pid %d force-killed\n", pid);
    else                          kprintf("fkill: pid %d not found or protected\n", pid);
}

/* §1.1 — `faulttest`: prove that a BAD user pointer can no longer take the box
 * down.  Two layers are checked:
 *   (a) the GATE — a syscall handed a kernel/unmapped address must return an
 *       error (the address never gets dereferenced at all);
 *   (b) the FAULT FIXUP — the uaccess primitive is called DIRECTLY on unmapped
 *       memory, bypassing the pre-check, so the copy really does fault in ring 0.
 *       The exception table must catch it and return -1.  Before this existed,
 *       (b) was a kernel #PF → halt policy → the whole machine froze.
 * Borrows a private address space so the "in a user syscall" gate is realistic. */
static void cmd_faulttest(void) {
    struct task* me = task_current();
    if (!me) return;
    struct vmm_space* s = vmm_space_create();
    if (!s) { console_write("faulttest: no space\n"); return; }
    struct vmm_space* prev = me->mm;
    int prev_gate = me->in_user_syscall;
    me->mm = s;
    vmm_space_switch(s);
    me->in_user_syscall = 1;                 /* pretend we came from ring 3 */

    /* (a) gate: a kernel address and an unmapped user address as syscall args. */
    long w_kern   = sys_write(1, (const void*)(uintptr_t)0x00100000u, 8);   /* kernel text */
    int  o_unmap  = sys_open((const char*)(uintptr_t)(vmm_user_base() + 0x123000u), 0);
    struct kstat st;
    int  s_unmap  = sys_stat((const char*)(uintptr_t)(vmm_user_base() + 0x123000u), &st);

    me->in_user_syscall = prev_gate;

    /* (b) fixup: unmapped, checked by nothing — only the exception table can
     *     save us here.  If the table were missing this line would panic. */
    char buf[8];
    int  cp_in    = uaccess_copy_in(buf, (uintptr_t)(vmm_user_base() + 0x456000u), sizeof buf);
    int  cp_out   = uaccess_copy_out((uintptr_t)(vmm_user_base() + 0x456000u), buf, sizeof buf);
    long str_in   = uaccess_str_in(buf, (uintptr_t)(vmm_user_base() + 0x456000u), sizeof buf);

    /* (c) BOUNCE (§1.1 layer 3).  Map exactly ONE user page, so the page right
     *     after it is guaranteed unmapped.  Two things get proven here:
     *       - a valid ring-3 payload still round-trips correctly through the
     *         kernel staging chunk (`sys_write` of "bnce" prints it), and
     *       - the TOCTOU the pre-check CANNOT catch: a copy whose range goes bad
     *         PART-WAY THROUGH.  Straddling the page boundary reproduces exactly
     *         that — the first bytes copy, then the next page faults.  We call
     *         uaccess_copy_in directly (no pre-check in the way) so the only
     *         thing standing between us and a ring-0 #PF is the exception table.
     *         Before bounce buffers, this shape reached the VFS/socket layers as
     *         a raw pointer, where no fixup entry covers the dereference. */
    long upage = sys_mmap(4096, -1);
    long w_ok = -1, w_strad = 0, cp_partial = 0; int partial_ok = 0;
    if (upage > 0) {
        char* up = (char*)(uintptr_t)upage;
        up[0] = 'b'; up[1] = 'n'; up[2] = 'c'; up[3] = 'e'; up[4] = ' ';
        for (int i = 4088; i < 4096; i++) up[i] = (char)0xA5;   /* tail pattern */

        me->in_user_syscall = 1;
        w_ok    = sys_write(1, (const void*)(uintptr_t)upage, 5);        /* valid  */
        w_strad = sys_write(1, (const void*)(uintptr_t)(upage + 4088), 32); /* runs off */
        me->in_user_syscall = prev_gate;

        /* Mid-copy fault: 8 good bytes then unmapped memory. */
        uint8_t k[16];
        for (int i = 0; i < 16; i++) k[i] = 0;
        cp_partial = uaccess_copy_in(k, (uintptr_t)upage + 4088, 16);
        partial_ok = (k[0] == 0xA5 && k[7] == 0xA5);   /* the good half DID land */
    }

    vmm_space_switch(prev);
    me->mm = prev;
    vmm_space_destroy(s);

    kprintf("faulttest: gate   write(kernel ptr)=%ld open(unmapped)=%d stat(unmapped)=%d -> %s\n",
            w_kern, o_unmap, s_unmap,
            (w_kern < 0 && o_unmap < 0 && s_unmap < 0) ? "PASS" : "FAIL");
    kprintf("faulttest: fixup  copy_in=%d copy_out=%d str_in=%ld -> %s\n",
            cp_in, cp_out, str_in,
            (cp_in < 0 && cp_out < 0 && str_in < 0) ? "PASS" : "FAIL");
    kprintf("faulttest: bounce write(valid)=%ld write(straddle)=%ld mid-copy=%d partial=%d -> %s\n",
            w_ok, w_strad, cp_partial, partial_ok,
            (w_ok == 5 && w_strad < 0 && cp_partial < 0 && partial_ok) ? "PASS" : "FAIL");
    console_write("faulttest: the box is still running — that IS the test.\n");
}


/* ---------------------------------------------------------------------------
 * `crash` — what has gone wrong on this machine (§M47).
 *
 * Every fault, lockup, hang, forced kill and unclean shutdown lands in the
 * crash ring; this prints it newest-first.  The point is that a user who saw
 * something misbehave can find out WHAT afterwards, instead of the event
 * existing only as a line that scrolled past on a serial console they were not
 * watching.
 * --------------------------------------------------------------------------- */
static void cmd_crash(void) {
    int n = crash_count();
    if (n == 0) {
        console_write("crash: no crashes recorded on this boot\n");
        return;
    }
    kprintf("%d crash record(s), newest first:\n", n);
    kprintf("  UPTIME    KIND          CPU  PID  NAME              DETAIL\n");
    for (int i = 0; i < n; i++) {
        const struct crash_record* r = crash_at(i);
        if (!r) break;
        kprintf("  %us  %s  %u  %d  %s  pc=%p addr=%p code=%d %s\n",
                (unsigned)(r->ms / 1000), crash_kind_name(r->kind),
                (unsigned)r->cpu, r->pid, r->comm,
                (void*)r->pc, (void*)r->addr, r->code, r->what);
    }
}
/* ---------------------------------------------------------------------------
 * `archtest` — prove the ELF loader refuses a FOREIGN-architecture image.
 *
 * Before this check existed, a wrong-arch binary loaded "successfully": the
 * segments mapped and the CPU was handed an entry point full of foreign
 * instruction encodings, so the failure surfaced later as an unrelated-looking
 * fault (and on a 32-bit kernel, a 64-bit image had its p_vaddr fields silently
 * truncated, mapping segments at the wrong addresses entirely).
 *
 * The test synthesises bare ELF headers — no address space needed, because both
 * outcomes are decided in the header:
 *   - a foreign (class, machine) pair must be rejected with ELF_EBADARCH;
 *   - the NATIVE pair must get PAST the arch gate, which we observe as the next
 *     error along (ELF_ENOLOAD — the synthetic header has no PT_LOAD).  That
 *     second half is the important one: it proves the gate is discriminating
 *     rather than just refusing everything.
 * --------------------------------------------------------------------------- */
static void put16le(uint8_t* p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void make_ehdr(uint8_t* b, unsigned cls, unsigned machine) {
    for (int i = 0; i < 64; i++) b[i] = 0;
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = (uint8_t)cls;            /* EI_CLASS  */
    b[5] = 1;                       /* EI_DATA = little-endian */
    b[6] = 1;                       /* EI_VERSION */
    put16le(b + 16, 2);             /* e_type = ET_EXEC */
    put16le(b + 18, machine);       /* e_machine */
    /* e_phnum stays 0 → a native header falls through to ELF_ENOLOAD. */
}

static void cmd_archtest(void) {
    uint8_t hdr[64];
    /* Two foreign shapes: the "other" word size and a plainly alien machine. */
    int is64 = (sizeof(void*) == 8);
    make_ehdr(hdr, is64 ? 1u : 2u, is64 ? 3u : 62u);      /* other class+machine */
    int foreign_other = elf_load_ex(NULL, hdr, sizeof hdr, 0, NULL);

    make_ehdr(hdr, is64 ? 2u : 1u, 183u);                 /* EM_AARCH64 */
    int foreign_arm = elf_load_ex(NULL, hdr, sizeof hdr, 0, NULL);

    /* Native class+machine — must pass the gate and fail for the NEXT reason. */
    make_ehdr(hdr, is64 ? 2u : 1u, is64 ? 62u : 3u);
    int native = elf_load_ex(NULL, hdr, sizeof hdr, 0, NULL);

    kprintf("archtest: arch=%s foreign=%d/%d (want %d) native=%d (want %d) -> %s\n",
            hal_arch_name(), foreign_other, foreign_arm, ELF_EBADARCH,
            native, ELF_ENOLOAD,
            (foreign_other == ELF_EBADARCH && foreign_arm == ELF_EBADARCH &&
             native == ELF_ENOLOAD) ? "PASS" : "FAIL");
}

/* ---------------------------------------------------------------------------
 * `fputest` — prove the FP/SIMD register file is PER-TASK.
 *
 * Two kernel tasks each stamp a different pattern into a live FP register, then
 * spend a few thousand yields checking it is still theirs.  Without per-task
 * FXSAVE/FXRSTOR the two tasks share one physical register file, so each sees
 * the other's value within a yield or two and the mismatch counters explode —
 * i.e. this test FAILS on the code that existed before the fix, which is the
 * only kind of regression test worth having.
 *
 * The patterns are exactly-representable doubles on purpose: i386 holds the
 * value in an 80-bit x87 register and converts back on read, so an arbitrary
 * bit pattern (an SNaN, say) would not survive the round trip and the test
 * would report a corruption that never happened.
 * --------------------------------------------------------------------------- */
#define FPUTEST_ROUNDS 3000
static volatile uint64_t g_fpu_mismatch[2];
static volatile int      g_fpu_done[2];

static void fputest_worker(void) {
    int slot = (int)(uintptr_t)task_start_arg();
    uint64_t pattern = slot ? 0x4004000000000000ull    /* 2.5 */
                            : 0x3FF8000000000000ull;   /* 1.5 */
    uint64_t bad = 0;
    hal_fpu_test_stamp(pattern);
    for (int i = 0; i < FPUTEST_ROUNDS; i++) {
        task_yield();
        if (hal_fpu_test_read() != pattern) bad++;
    }
    g_fpu_mismatch[slot] = bad;
    g_fpu_done[slot] = 1;
}

static void cmd_fputest(void) {
    if (!hal_fpu_present()) {
        console_write("fputest: SKIP — this arch has no reachable FP unit "
                      "(see kernel/hal/aarch64/fpu.c)\n");
        return;
    }
    g_fpu_mismatch[0] = g_fpu_mismatch[1] = 0;
    g_fpu_done[0] = g_fpu_done[1] = 0;

    task_spawn_arg("fpu-a", fputest_worker, (void*)(uintptr_t)0);
    task_spawn_arg("fpu-b", fputest_worker, (void*)(uintptr_t)1);

    /* Wait for both, bounded so a wedged worker cannot hang the shell. */
    for (int spins = 0; spins < 200000 && !(g_fpu_done[0] && g_fpu_done[1]); spins++)
        task_yield();

    if (!(g_fpu_done[0] && g_fpu_done[1])) {
        console_write("fputest: workers did not finish\n");
        return;
    }
    kprintf("fputest: %d rounds x 2 tasks — mismatches a=%u b=%u -> %s\n",
            FPUTEST_ROUNDS, (unsigned)g_fpu_mismatch[0], (unsigned)g_fpu_mismatch[1],
            (g_fpu_mismatch[0] == 0 && g_fpu_mismatch[1] == 0) ? "PASS" : "FAIL");
}

/* §M46 — spawn the WEDGE test app: a ring-3 task that spins forever without ever
 * yielding.  `kill` (cooperative) cannot reclaim it; `fkill` (force) can. */
extern const unsigned char _binary_user_wedge_elf_start[]   __attribute__((weak));
extern const unsigned char _binary_user_wedge_elf_end[]     __attribute__((weak));
extern const unsigned char _binary_user_wedge_x86_64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wedge_x86_64_elf_end[]   __attribute__((weak));
static void cmd_wedge(void) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_wedge_elf_start)        { s = _binary_user_wedge_elf_start;   e = _binary_user_wedge_elf_end; }
    else if (_binary_user_wedge_x86_64_elf_start) { s = _binary_user_wedge_x86_64_elf_start; e = _binary_user_wedge_x86_64_elf_end; }
    if (!s || !e) { console_write("wedge: no wedge ELF embedded for this arch\n"); return; }
    int pid = proc_spawn("wedge", s, (size_t)(e - s));
    if (pid < 0) { console_write("wedge: spawn failed\n"); return; }
    /* §M46 — apply the runaway auto-fkill policy (off unless configured), so a
     * `package.auto_fkill_ms` setting makes the watchdog reclaim this hog on its
     * own; with no config it stays wedged for manual `fkill` testing. */
    long ms = config_get_long("package.wedge.auto_fkill_ms",
                              config_get_long("package.auto_fkill_ms", 0));
    if (ms > 0) task_set_auto_fkill(pid, (uint32_t)ms);
    kprintf("wedge: spawned a WEDGED ring-3 task (pid %d) — `kill %d` can't stop it, `fkill %d` can%s\n",
            pid, pid, pid, ms > 0 ? " (auto-fkill armed)" : "");
}

/* `loop [n]` — spawn n CPU hogs (default 1).  The count exists for §M49:
 * one hog cannot show a load-distribution problem, because there is
 * nothing to distribute.  `loop 8` on a 4-CPU box is the shape that makes
 * an imbalance visible under `sched`. */
static void cmd_loop(const char* args) {
    while (*args == ' ') args++;
    int count = 0;
    for (; *args >= '0' && *args <= '9'; args++) count = count * 10 + (*args - '0');
    if (count <= 0) count = 1;
    if (count > 32) count = 32;             /* a shell typo shouldn't OOM the box */
    loop_stop_flag = 0;
    int spawned = 0;
    for (int i = 0; i < count; i++) {
        struct task* t = task_spawn("cpu-hog", loop_hog_main);
        if (!t) break;
        spawned++;
    }
    if (!spawned) { console_write("loop: spawn failed (OOM?)\n"); return; }
    kprintf("loop: spawned %d cpu-hog(s) — should NOT freeze the shell; "
            "`sched` to see the spread, `loopstop` to stop them all\n", spawned);
}

/* `loopstop` — release every cpu-hog spawned by `loop`.  They poll the
 * flag, so they exit at their next iteration.  Without this, measuring
 * twice in one boot means measuring the first run's leftovers too. */
static void cmd_loopstop(void) {
    loop_stop_flag = 1;
    console_write("loopstop: all cpu-hogs asked to exit\n");
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
    /* Spawn the shell task for the new pane with the VC already bound, so
     * the task's first kprintf routes through vc_putchar(nv, ...).
     * §M49 — this was a set-after-spawn under preempt_disable, which is
     * not a barrier against another CPU picking the task up (that counter
     * is per-CPU).  The binding belongs in the spawn. */
    struct task* t = task_spawn_console("shell", shell_provider_active()->entry,
                                        -1, nv);
    if (t) nv->task = t;

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
    struct gui_desktop_stats d;
    gui_get_desktop_stats(&d);
    kprintf("desktop: %u loop iterations, %u panel repaints, %u chrome events\n",
            d.iters, d.draws, d.events);
    kprintf("         %u half-second ticks, %u changed the chrome, clock %u ms\n",
            d.ticks, d.tick_dirty, d.clock_ms);
}

/* `gui` — start the M22 compositor.  The calling shell keeps running in
 * its (now invisible) pane; two fresh shells come up in windows.  A
 * second invocation is a no-op — the compositor is a singleton. */
static void cmd_gui(const char* args) {
    while (args && *args == ' ') args++;
    if (args && starts_with(args, "stop")) {
        /* The other direction, from the shell side.  It exists because the
         * Start menu's "Exit GUI" is unreachable when the desktop is what went
         * wrong — and a way out that only works while everything works is not
         * a way out. */
        if (gui_stop() != 0) console_write("gui: not running\n");
        return;
    }
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

/* --------------------------------------------------------------------
 * `sched [ms]` — measure how work is actually spread across the CPUs
 * (§M49).
 *
 * Why a sampling command and not a cumulative dump: since-boot totals
 * average away exactly what we need to see.  A CPU that was hammered
 * during boot and is idle now looks "half busy" forever, and a balancer
 * fix would be indistinguishable from no fix.  So this takes two
 * snapshots `ms` apart and reports the DELTA — the picture right now.
 *
 * BUSY% is the share of the window this CPU spent running a non-idle
 * task.  RQ is the instantaneous queue depth.  The two together are the
 * whole point: rq 1 + busy 100% is a saturated CPU with nothing to give
 * away, rq 4 + busy 100% is a CPU with three tasks' worth of work that
 * belongs somewhere else.  MIGR counts tasks pulled here by the
 * balancer — the cost side, and the way to catch a balancer that merely
 * shuttles the same task around.
 * -------------------------------------------------------------------- */

#define SCHED_SNAP_MAX 64          /* tasks tracked per sample; plenty here */

struct sched_snap_task {
    int      pid;
    uint64_t cpu_ms;
    int      cpu_home;
    uint32_t demand;
    int      nice;
    char     name[24];
};

struct sched_snap {
    struct sched_snap_task t[SCHED_SNAP_MAX];
    int      n;
    uint64_t now;
};

static void sched_snap_one(const struct task* t, int is_current, void* ctx) {
    (void)is_current;   /* only true for the CALLING task — useless here */
    struct sched_snap* s = (struct sched_snap*)ctx;
    if (s->n >= SCHED_SNAP_MAX) return;
    /* Idle tasks would each report ~100% of their core and drown the real
     * tasks out; per-CPU BUSY% already says how idle a core is. */
    if (t->is_idle) return;
    struct sched_snap_task* e = &s->t[s->n++];
    e->pid      = t->pid;
    /* Same switch-boundary gap as the per-CPU busy counter: a task that is
     * on a CPU right now has not been credited for the slice it is in the
     * middle of.  Find out whether it is running anywhere and add it. */
    e->cpu_ms   = t->cpu_ms;
    for (int c = 0; c < smp_ncpus() && c < 32; c++) {
        struct percpu* p = percpu_at(c);
        if (p && p->current == t) {
            if (s->now > t->sched_in_ms) e->cpu_ms += s->now - t->sched_in_ms;
            break;
        }
    }
    e->cpu_home = t->cpu_home;
    e->demand   = t->demand;
    e->nice     = t->nice;
    int i = 0;
    for (; i < (int)sizeof(e->name) - 1 && t->name[i]; i++) e->name[i] = t->name[i];
    e->name[i] = '\0';
}

/* §M53 — `ktime`: what clock is actually backing timer_now_ns, and does it
 * resolve better than the tick?  The measurement matters more than the name:
 * a source can be present and mis-calibrated, and the only way to see that is
 * to compare a measured interval against the tick that was used to calibrate
 * it.  Printing "hires counter" without proving it advances would be exactly
 * the kind of claim this project keeps learning not to trust. */
/* §M53 — `ktimer`: does the timer service actually meet its deadlines?
 *
 * The list is easy to get right and easy to believe in; the number that
 * matters is LATENESS, because the deadline is kept in nanoseconds while the
 * moment we notice it is bounded by the tick.  Measuring a spread of sleeps
 * against the clock is what turns "we have timers" into a figure someone can
 * decide on — specifically, whether replacing the periodic tick with a
 * one-shot hardware deadline is worth doing. */
static void cmd_ktimer(void) {
    uint32_t pending; uint64_t fired, late;
    ktimer_stats(&pending, &fired, &late);
    kprintf("ktimer: %u pending, %u fired, worst lateness %u us\n",
            pending, (unsigned)fired, (unsigned)(late / 1000ull));

    static const unsigned req_us[] = { 500, 1000, 5000, 20000, 100000 };
    kprintf("  requested   actual    error\n");
    for (unsigned i = 0; i < sizeof req_us / sizeof req_us[0]; i++) {
        uint64_t want = (uint64_t)req_us[i] * 1000ull;
        uint64_t t0 = timer_now_ns();
        task_sleep_until_ns(t0 + want);
        uint64_t got = timer_now_ns() - t0;
        long err = (long)((int64_t)got - (int64_t)want) / 1000;
        kprintf("  %u us      %u us     %s%u us\n", req_us[i],
                (unsigned)(got / 1000ull), err < 0 ? "-" : "+",
                (unsigned)(err < 0 ? -err : err));
    }
    ktimer_stats(&pending, &fired, &late);
    kprintf("  after: %u fired, worst lateness %u us (floor = one tick)\n",
            (unsigned)fired, (unsigned)(late / 1000ull));
}

static void cmd_ktime(void) {
    kprintf("clock source : %s", timer_source_name());
    if (timer_source_hz())
        kprintf(" (%u kHz)", (unsigned)(timer_source_hz() / 1000u));
    kprintf("\n resolution  : %u ns\n", (unsigned)timer_res_ns());

    /* Back-to-back reads: with the tick these are IDENTICAL most of the time,
     * which is the whole problem in one number. */
    uint64_t a = timer_now_ns(), b = timer_now_ns(), c = timer_now_ns();
    kprintf(" back-to-back: %u ns, %u ns apart\n",
            (unsigned)(b - a), (unsigned)(c - b));

    /* Measure one tick-based sleep with the new clock.  Agreement to within a
     * tick is the calibration check; a wildly different number means the
     * counter frequency is wrong, not that the sleep is. */
    uint64_t t0 = timer_now_ns();
    task_msleep(100);
    uint64_t t1 = timer_now_ns();
    kprintf(" 100 ms sleep: measured %u us (%u ms) by the ns clock\n",
            (unsigned)((t1 - t0) / 1000ull), (unsigned)((t1 - t0) / 1000000ull));
    kprintf(" uptime      : %u ms\n", (unsigned)(t1 / 1000000ull));
}

static void cmd_sched(const char* args) {
    while (*args == ' ') args++;
    uint32_t window = 0;
    for (; *args >= '0' && *args <= '9'; args++) window = window * 10 + (uint32_t)(*args - '0');
    if (window < 100)   window = 1000;      /* too short to measure anything */
    if (window > 10000) window = 10000;

    int n = smp_ncpus();
    /* Snapshot A. */
    static uint64_t busy0[32], sw0[32], mig0[32];
    /* `busy_ms` is only credited at a context switch, so a task that
     * monopolises a core without ever being switched out contributes
     * NOTHING to it — the first version of this command reported a core
     * running one busy task at 0%.  Add the in-flight slice at both
     * snapshots to close that gap. */
    #define BUSY_NOW(p, now) \
        ((p)->busy_ms + (((p)->current && !(p)->current->is_idle && \
                          (now) > (p)->current->sched_in_ms) \
                         ? (now) - (p)->current->sched_in_ms : 0))
    struct sched_snap* a = (struct sched_snap*)kmalloc(sizeof *a);
    struct sched_snap* b = (struct sched_snap*)kmalloc(sizeof *b);
    if (!a || !b) { console_write("sched: OOM\n"); kfree(a); kfree(b); return; }
    a->n = b->n = 0;
    uint64_t t0 = timer_ticks_ms();
    a->now = t0;
    for (int i = 0; i < n && i < 32; i++) {
        struct percpu* p = percpu_at(i);
        busy0[i] = p ? BUSY_NOW(p, t0)  : 0;
        sw0[i]   = p ? p->switches      : 0;
        mig0[i]  = p ? p->migrations    : 0;
    }
    task_for_each(sched_snap_one, a);

    kprintf("sched: sampling %u ms across %d CPU(s)...\n", window, n);
    task_msleep(window);

    uint64_t t1 = timer_ticks_ms();
    b->now = t1;
    uint64_t elapsed = t1 - t0;
    if (elapsed == 0) elapsed = 1;          /* never divide by a stopped clock */
    task_for_each(sched_snap_one, b);

    kprintf("CPU  RQ  LOAD  BUSY%%  SWITCH  MIGR  CURRENT\n");
    unsigned lo = 100, hi = 0;
    int rq_lo = 1 << 30, rq_hi = 0;
    int ld_lo = 1 << 30, ld_hi = 0;
    for (int i = 0; i < n && i < 32; i++) {
        struct percpu* p = percpu_at(i);
        if (!p || !p->online) continue;
        uint64_t bnow = BUSY_NOW(p, t1);
        uint64_t db = (bnow > busy0[i]) ? bnow - busy0[i] : 0;
        if (db > elapsed) db = elapsed;     /* clamp a torn/racy sample */
        unsigned pct = (unsigned)((db * 100) / elapsed);
        if (pct < lo) lo = pct;
        if (pct > hi) hi = pct;
        if (p->rq_count < rq_lo) rq_lo = p->rq_count;
        if (p->rq_count > rq_hi) rq_hi = p->rq_count;
        if (p->rq_load < ld_lo) ld_lo = p->rq_load;
        if (p->rq_load > ld_hi) ld_hi = p->rq_load;
        kprintf("%d    %d   %d   %u      %u       %u     %s\n",
                i, p->rq_count, p->rq_load, pct,
                (unsigned)(p->switches   - sw0[i]),
                (unsigned)(p->migrations - mig0[i]),
                (p->current && p->current->name) ? p->current->name : "?");
    }
    if (rq_lo > rq_hi) rq_lo = rq_hi = 0;
    if (ld_lo > ld_hi) ld_lo = ld_hi = 0;
    /* LOAD spread is the number that matters: it is what the balancer
     * equalises, and unlike BUSY% it stays informative when every core is
     * saturated (four hogs and one hog are both 100% busy). */
    kprintf("spread: load %d..%d (delta %d), busy %u%%..%u%% (delta %u), rq %d..%d\n",
            ld_lo, ld_hi, ld_hi - ld_lo, lo, hi, hi - lo, rq_lo, rq_hi);

    /* Per-task CPU time consumed during the window, and where it ran.
     * This is the line that shows an imbalance as a human sees it: two
     * hogs at 99% and two at 1% is a scheduling failure no aggregate
     * per-CPU number makes obvious. */
    /* DEM is the balancer's own view of the task (how much CPU it WANTS);
     * CPU%% is what it actually got.  The two diverging is the signature
     * of contention: a hog reads DEM 100 and CPU%% 25 on a crowded core. */
    kprintf("PID  CPU  NI  DEM  CPU%%  NAME\n");
    for (int i = 0; i < b->n; i++) {
        uint64_t before = 0;
        int seen = 0;
        for (int j = 0; j < a->n; j++)
            if (a->t[j].pid == b->t[i].pid) { before = a->t[j].cpu_ms; seen = 1; break; }
        uint64_t used = seen ? (b->t[i].cpu_ms - before) : b->t[i].cpu_ms;
        if (used > elapsed) used = elapsed;
        unsigned pct = (unsigned)((used * 100) / elapsed);
        if (pct == 0) continue;             /* idle/blocked tasks: not the story */
        kprintf("%d    %d    %d   %u   %u     %s%s\n",
                b->t[i].pid, b->t[i].cpu_home, b->t[i].nice,
                b->t[i].demand, pct, b->t[i].name, seen ? "" : " (new)");
    }
    kfree(a);
    kfree(b);
    #undef BUSY_NOW
}

/* --------------------------------------------------------------------
 * `wqtest [n]` — submit n work items and prove they ran (§M49).
 *
 * Each item spins for a few milliseconds so the run is long enough to be
 * observed, then records WHICH CPU executed it.  With a worker per core
 * and the §M49 balancer spreading them, the items should land on more
 * than one CPU — that spread is the claim being tested, and a version of
 * this that only counted completions would pass just as happily on a
 * single core.
 *
 * Also exercises the two contracts that are easy to get wrong: work_flush
 * must not return until every callback has RETURNED (not merely been
 * dequeued), and a re-submit of a still-queued item must collapse into
 * one run.
 * -------------------------------------------------------------------- */

#define WQTEST_MAX 32

struct wqtest_item {
    struct work w;
    int         idx;
    volatile int ran;
    volatile int cpu;
};

static struct wqtest_item wqt[WQTEST_MAX];

static void wqtest_fn(struct work* w) {
    struct wqtest_item* it = (struct wqtest_item*)w;   /* w is the first member */
    /* Busy for a few ms — long enough that concurrent items overlap, which
     * is what makes the CPU spread meaningful. */
    uint64_t end = timer_ticks_ms() + 5;
    while (timer_ticks_ms() < end) { /* spin */ }
    it->cpu = this_cpu_id();
    it->ran++;
}

static void cmd_wqtest(const char* args) {
    while (*args == ' ') args++;
    int n = 0;
    for (; *args >= '0' && *args <= '9'; args++) n = n * 10 + (*args - '0');
    if (n <= 0) n = 8;
    if (n > WQTEST_MAX) n = WQTEST_MAX;

    int workers = 0, pending = 0;
    uint64_t done0 = 0;
    workqueue_stats(&workers, &pending, &done0);
    if (workers == 0) { console_write("wqtest: no workers (pool not up)\n"); return; }

    for (int i = 0; i < n; i++) {
        wqt[i].idx = i;
        wqt[i].ran = 0;
        wqt[i].cpu = -1;
        work_init(&wqt[i].w, wqtest_fn);
    }

    kprintf("wqtest: submitting %d items to %d worker(s)...\n", n, workers);
    uint64_t t0 = timer_ticks_ms();
    for (int i = 0; i < n; i++) work_submit(&wqt[i].w);
    /* Re-submitting the same items immediately must NOT double them. */
    for (int i = 0; i < n; i++) work_submit(&wqt[i].w);
    work_flush();
    uint64_t elapsed = timer_ticks_ms() - t0;

    int ran = 0, extra = 0, runs = 0;
    int per_cpu[32];
    for (int i = 0; i < 32; i++) per_cpu[i] = 0;
    for (int i = 0; i < n; i++) {
        runs += wqt[i].ran;
        if (wqt[i].ran >= 1) ran++;
        if (wqt[i].ran >  1) extra++;      /* re-queued while running — legal */
        if (wqt[i].cpu >= 0 && wqt[i].cpu < 32) per_cpu[wqt[i].cpu]++;
    }
    int cpus_used = 0;
    for (int i = 0; i < 32; i++) if (per_cpu[i]) cpus_used++;

    kprintf("wqtest: %d/%d items ran in %u ms across %d CPU(s):", ran, n,
            (unsigned)elapsed, cpus_used);
    for (int i = 0; i < smp_ncpus() && i < 32; i++) kprintf(" cpu%d=%d", i, per_cpu[i]);
    kprintf("\n");

    uint64_t done1 = 0;
    workqueue_stats(&workers, &pending, &done1);
    /* Serial work would take n*5 ms; real overlap should beat that
     * noticeably once there is more than one worker. */
    kprintf("wqtest: completed counter +%u, still pending %d, serial would be ~%d ms\n",
            (unsigned)(done1 - done0), pending, n * 5);
    /* `extra` is NOT a failure.  The second submit loop collapses into the
     * first only for items still WAITING; an item a worker has already
     * picked up is legitimately queued again, which is exactly how a
     * driver says "more arrived while you were draining".  The first
     * version of this test asserted zero duplicates and failed against its
     * own documented contract — the assertion was wrong, not the queue.
     * What must hold: every item ran, nothing was left pending after the
     * flush, and the completion counter agrees with the runs observed. */
    /* The completion counter is GLOBAL, so it may exceed our own runs: the
     * xHCI event-ring drain submits work from the timer tick, and any
     * future consumer will too.  This check originally demanded exact
     * equality and started failing the moment the queue got a real
     * production user — the counter read +12 against 11 of our runs, the
     * difference being one USB drain.  `>=` is what was actually meant. */
    int ok = (ran == n) && (pending == 0) && (done1 - done0 >= (uint64_t)runs);
    if (ok)
        kprintf("wqtest: PASS (all %d ran, %d re-queued while running, "
                "flush drained everything)\n", n, extra);
    else
        kprintf("wqtest: FAIL (ran=%d/%d, runs=%d, counter=+%u, pending=%d)\n",
                ran, n, runs, (unsigned)(done1 - done0), pending);
    if (done1 - done0 > (uint64_t)runs)
        kprintf("wqtest: (+%u completions from other submitters — the xHCI "
                "drain runs on this pool)\n",
                (unsigned)(done1 - done0 - (uint64_t)runs));
}

/* --------------------------------------------------------------------
 * `abi` — show the guest-ABI translation tables (§M50).
 *
 * The point of the engine is that a platform's syscall numbering is DATA,
 * so the data should be readable.  Printing the three Linux number spaces
 * side by side is also the clearest statement of what the engine does:
 * one column per platform, one row per meaning.
 * -------------------------------------------------------------------- */
static void cmd_abi(void) {
    int have = 0, total = 0;
    abi_stats(&have, &total);
    kprintf("abi: %d/%d canonical operations have handlers\n", have, total);

    const struct abi_map* maps[] = {
        &abi_map_linux_i386, &abi_map_linux_amd64, &abi_map_linux_arm64,
    };
    const unsigned nmaps = sizeof(maps) / sizeof(maps[0]);

    /* kprintf is a minimal formatter with no width specifiers, so columns are
     * padded by hand rather than by "%-14s". */
    console_write("MEANING     ");
    for (unsigned m = 0; m < nmaps; m++) { kprintf("%s   ", maps[m]->name); }
    console_write("\n");

    /* One row per MEANING, one column per platform: the same operation under
     * three different numbers is the whole point. */
    for (uint32_t i = 0; i < maps[0]->n_ents; i++) {
        uint16_t op = maps[0]->ents[i].op;
        kprintf("op %u", (unsigned)op);
        console_write(op < 10 ? "         " : "        ");
        for (unsigned m = 0; m < nmaps; m++) {
            int found = -1;
            for (uint32_t j = 0; j < maps[m]->n_ents; j++)
                if (maps[m]->ents[j].op == op) { found = (int)maps[m]->ents[j].nr; break; }
            if (found >= 0) kprintf("%d", found); else console_write("-");
            console_write("            ");
        }
        console_write("\n");
    }
    console_write("abi: one meaning per row, one platform per column — the "
                  "difference between platforms is the table, not the code\n");
}

/* `nice <pid> <value>` — scheduling priority, -20 (strongest) .. +19
 * (weakest), 0 default (§M49).  Unprivileged in both directions: d-os has
 * no user model yet (§M32), so there is nobody to protect the setting
 * from. */
static void cmd_nice(const char* args) {
    while (*args == ' ') args++;
    int pid = 0, any = 0;
    for (; *args >= '0' && *args <= '9'; args++) { pid = pid * 10 + (*args - '0'); any = 1; }
    if (!any) { console_write("nice: usage: nice <pid> <-20..19>\n"); return; }
    while (*args == ' ') args++;
    int neg = 0;
    if (*args == '-') { neg = 1; args++; }
    else if (*args == '+') args++;
    int val = 0, hasval = 0;
    for (; *args >= '0' && *args <= '9'; args++) { val = val * 10 + (*args - '0'); hasval = 1; }
    if (!hasval) { console_write("nice: missing value (-20..19)\n"); return; }
    if (neg) val = -val;
    if (task_set_nice(pid, val) != 0) { kprintf("nice: no task with pid %u\n", pid); return; }
    struct task* t = task_find(pid);
    kprintf("nice: pid %d nice=%d weight=%u (%u%% of a default task's share)\n",
            pid, t ? t->nice : val, t ? t->weight : 0, t ? t->weight : 0);
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
    const char* zone_names[NR_ZONES] = { "DMA", "DMA32", "NORMAL" };
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
    /* Parse "<ip>" and an optional trailing count. */
    char ipbuf[32]; int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) { ipbuf[i] = args[i]; i++; }
    ipbuf[i] = '\0';
    if (i == 0) { console_write("usage: ping <ip> [count]\n"); return; }

    uint32_t ip;
    if (net_parse_ip(ipbuf, &ip) != 0) { console_write("ping: bad IP\n"); return; }

    /* §M24.8 — the device follows from the DESTINATION now, not from "the only
     * one we have": 127.0.0.1 must go to `lo` even on a box that also has a
     * NIC, and on a box with no NIC at all it is the only reachable address. */
    struct net_device* dev = net_route(ip);
    if (!dev) { console_write("ping: no route to host\n"); return; }

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
    uint32_t ip;
    if (net_parse_ip(args, &ip) != 0) { console_write("usage: arp <ip>\n"); return; }
    struct net_device* dev = net_route(ip);
    if (!dev) { console_write("arp: no route to host\n"); return; }
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

/* §M39 — the userland musl+mbedTLS `wget` (URL + optional outfile from argv).
 * When embedded it handles BOTH http:// and https:// (real TLS + CA verify); the
 * kernel HTTP-only path below is kept as a fallback for builds without musl. */
extern const unsigned char _binary_user_wget_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wget_muslelf_end[]   __attribute__((weak));

extern const unsigned char _binary_user_netsurf_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netsurf_dynelf_end[]   __attribute__((weak));

/* `netsurf [url]` — the NetSurf web browser (§M42).  Execs the musl dynamic PIE
 * in ring 3 under the linux-abi personality (like wget); ld.so resolves its
 * DT_NEEDED store .so's from /lib, and its runtime resources come from /res
 * (provisioned into the VFS at boot).  With no argument it opens about:welcome. */
static void cmd_netsurf(const char* args) {
    /* §M62 follow-up — ~9 MiB of browser resources, unpacked on first launch
     * rather than on every boot (see pkg.h for the measurement). */
    pkg_ensure_netsurf_res();
    if (!_binary_user_netsurf_dynelf_start) {
        console_write("netsurf: not built — run `make ARCH=x86_64 netsurf`\n");
        return;
    }
    /* The libnsfb "dos" surface needs the compositor; bring it up if the user
     * hasn't run `gui` yet (idempotent), then give it a moment to be ready. */
    gui_start();
    task_msleep(300);
    static char abuf[512];
    int n = 0; while (args[n] && n < (int)sizeof abuf - 1) { abuf[n] = args[n]; n++; }
    abuf[n] = '\0';
    const char* argv[10]; int argc = 0;
    argv[argc++] = "netsurf";
    argv[argc++] = "-f";            /* select the d-os windowed surface backend */
    argv[argc++] = "dos";
    char* q = abuf;
    while (*q && argc < 9) {
        while (*q == ' ') q++;
        if (!*q) break;
        argv[argc++] = q;
        while (*q && *q != ' ') q++;
        if (*q) *q++ = '\0';
    }
    size_t len = (size_t)(_binary_user_netsurf_dynelf_end -
                          _binary_user_netsurf_dynelf_start);
    /* Spawn as an independent Linux-ABI user task (like the Start-menu launcher),
     * NOT a synchronous excursion on the shell task — so a browser crash/wedge is
     * torn down on its own and never takes down this shell. */
    int pid = proc_spawn_argv("netsurf", _binary_user_netsurf_dynelf_start, len,
                              argc, argv, 1 /* linux_abi */);
    if (pid < 0) { kprintf("netsurf: failed to spawn (rc=%d)\n", pid); return; }
    /* §M46 — per-package runaway auto-fkill policy (see netsurf_app.c). */
    long ms = config_get_long("package.netsurf.auto_fkill_ms",
                              config_get_long("package.auto_fkill_ms", 0));
    if (ms > 0) task_set_auto_fkill(pid, (uint32_t)ms);
    kprintf("netsurf: started as pid %d\n", pid);
}

/* `wget <url> [outfile]` — download over HTTP/HTTPS. */
static void cmd_wget(const char* args) {
    /* Prefer the userland musl wget (does TLS); fall back to kernel HTTP. */
    if (_binary_user_wget_muslelf_start) {
        /* Tokenize "<url> [outfile]" into an argv the program's crt0 reads. */
        static char abuf[512];
        int n = 0; while (args[n] && n < (int)sizeof abuf - 1) { abuf[n] = args[n]; n++; }
        abuf[n] = '\0';
        const char* argv[4]; int argc = 0;
        argv[argc++] = "wget";
        char* q = abuf;
        while (*q && argc < 4) {
            while (*q == ' ') q++;
            if (!*q) break;
            argv[argc++] = q;
            while (*q && *q != ' ') q++;
            if (*q) *q++ = '\0';
        }
        if (argc < 2) { console_write("usage: wget <url> [outfile]\n"); return; }
        size_t len = (size_t)(_binary_user_wget_muslelf_end -
                              _binary_user_wget_muslelf_start);
        struct task* me = task_current();
        int prev = me ? me->linux_abi : 0;
        if (me) me->linux_abi = 1;
        int rc = proc_exec_elf_argv(_binary_user_wget_muslelf_start, len, argc, argv);
        if (me) me->linux_abi = prev;
        kprintf("\nwget: exit rc=%d\n", rc);
        return;
    }

    const char* url = args;
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

/* -------------------------------------------------------------------- */
/* §M55 — `netstorm [n]`: prove that N tasks can wait for the network at  */
/* the same time, and that waiting is FREE.                              */
/*                                                                       */
/* The old stack could not do this even in principle: every waiter drove */
/* dev->poll() itself, so N waiters were N tasks mutating one RX ring     */
/* while each burned a CPU.  It survived only because nothing ever waited */
/* on two things at once — which is a statement about the workload, not   */
/* about the code.                                                       */
/*                                                                       */
/* Each probe asks for an address nothing will answer for, so it really   */
/* has to WAIT — a cache hit would prove nothing.  Every probe therefore  */
/* takes ARP_ATTEMPTS × ARP_TIMEOUT_MS ≈ 3 s, and the measurement is the  */
/* elapsed time: ~3 s means they waited in PARALLEL, ~3 s × n would mean  */
/* they had serialised behind each other.  The peak waiter count and the  */
/* per-CPU busy figure from `sched` say the rest.                        */
/* -------------------------------------------------------------------- */

static volatile int g_nst_idx, g_nst_done;

static void netstorm_probe(void) {
    int i = __atomic_fetch_add(&g_nst_idx, 1, __ATOMIC_ACQ_REL);
    struct net_device* dev = net_primary();
    if (dev) {
        uint8_t mac[6];
        /* Distinct unassigned addresses on our own subnet: on-link, so this
         * really does emit an ARP request and really does wait for a reply
         * that is never coming. */
        net_arp_resolve(dev, IPV4(10, 0, 2, 200) + (uint32_t)(i & 31), mac);
    }
    __atomic_add_fetch(&g_nst_done, 1, __ATOMIC_ACQ_REL);
}

static void cmd_netstorm(const char* args) {
    int n = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) n = n * 10 + (*args - '0');
    if (n <= 0) n = 6;
    if (n > 16) n = 16;

    if (!net_primary()) { console_write("netstorm: no net device\n"); return; }

    __atomic_store_n(&g_nst_idx, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_nst_done, 0, __ATOMIC_RELEASE);

    /* Sample aggregate CPU-busy across the storm.  This is the claim that
     * matters — "waiting for the network is free" — and asserting it without
     * measuring it is exactly the kind of comment §M52 was about. */
    int ncpu = smp_ncpus(); if (ncpu > 32) ncpu = 32;
    uint64_t busy0 = 0;
    for (int i = 0; i < ncpu; i++) {
        struct percpu* p = percpu_at(i);
        if (p) busy0 += p->busy_ms;
    }
    struct net_poller_stats st0;
    net_poller_stats(&st0);

    uint64_t t0 = timer_ticks_ms();
    int spawned = 0;
    for (int i = 0; i < n; i++)
        if (task_spawn_detached("net-probe", netstorm_probe)) spawned++;
    kprintf("netstorm: %d probe(s) waiting on unanswerable addresses...\n", spawned);

    /* Watch from the shell — which is NOT one of the waiters, so it can still
     * report if they all wedge (the killstorm lesson: a test that hangs with
     * the thing it tests reports nothing). */
    int peak = 0;
    for (int ms = 0; ms < 20000; ms += 50) {
        struct net_poller_stats sn;
        net_poller_stats(&sn);
        int w = sn.waiters;
        if (w > peak) peak = w;
        if (__atomic_load_n(&g_nst_done, __ATOMIC_ACQUIRE) >= spawned) break;
        task_msleep(50);
    }
    uint64_t elapsed = timer_ticks_ms() - t0;
    int done = __atomic_load_n(&g_nst_done, __ATOMIC_ACQUIRE);

    uint64_t busy1 = 0;
    for (int i = 0; i < ncpu; i++) {
        struct percpu* p = percpu_at(i);
        if (p) busy1 += p->busy_ms;
    }
    uint64_t db  = busy1 > busy0 ? busy1 - busy0 : 0;
    uint32_t pct = elapsed ? (uint32_t)((db * 100) / (elapsed * (uint64_t)ncpu)) : 0;

    struct net_poller_stats st1;
    net_poller_stats(&st1);

    kprintf("netstorm: %d/%d finished in %u ms, peak waiters %d, "
            "%u%% of %d CPUs busy while waiting, %u pumps, %u irqs\n",
            done, spawned, (uint32_t)elapsed, peak, pct, ncpu,
            st1.pumps - st0.pumps, st1.irqs - st0.irqs);
    if (done < spawned)
        console_write("netstorm: FAIL (a probe never returned)\n");
    else if (peak < 2 && spawned > 1)
        console_write("netstorm: FAIL (never more than one waiter — serialised)\n");
    else
        console_write("netstorm: PASS (concurrent waiters, one poller)\n");
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
    pmm_phys_t wf = pmm_alloc_frame();
    pmm_phys_t rf = pmm_alloc_frame();
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

    pmm_phys_t frame = pmm_alloc_frame();          /* backing for the user page */
    if (!frame) { console_write("mmtest: no frame\n"); vmm_space_destroy(s); return; }

    /* Seed the sentinel through the identity map (frame < 256 MiB). */
    *(volatile uint32_t*)phys_to_virt(frame) = 0xC0FFEE42u;

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
    me->mm = s;
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
    me->mm = prev;
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
    me->mm = s;
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
    me->mm = prev;
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
extern const unsigned char _binary_user_hello_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_elf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_hello_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_hello_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_hello_aarch64_elf_end[]   __attribute__((weak));

static const unsigned char* hello_blob(size_t* len) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_hello_elf_start)    { s = _binary_user_hello_elf_start;    e = _binary_user_hello_elf_end; }
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
extern const unsigned char _binary_user_spin_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_spin_elf_end[]      __attribute__((weak));
extern const unsigned char _binary_user_spin_x86_64_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_spin_x86_64_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_spin_aarch64_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_spin_aarch64_elf_end[]   __attribute__((weak));

/* Pick this arch's embedded spin blob (only one arch's is linked into the
 * image; the other weak symbols resolve to NULL). */
static const unsigned char* spin_blob(size_t* len) {
    const unsigned char *s = 0, *e = 0;
    if (_binary_user_spin_elf_start)    { s = _binary_user_spin_elf_start;    e = _binary_user_spin_elf_end; }
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
extern const unsigned char _binary_user_args_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_args_elf_end[]   __attribute__((weak));

static void cmd_runargs(const char* line) {
    if (!_binary_user_args_elf_start) {
        console_write("runargs: args ELF not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_args_elf_end - _binary_user_args_elf_start);

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
    int rc = proc_exec_elf_argv(_binary_user_args_elf_start, len,
                                argc, (const char* const*)argv);
    kprintf("runargs: returned rc=%d\n", rc);
}

/* M34 slice B — `forktest`: exec a user program that fork()s, the child exits
 * with a code, and the parent waitpid()s for it. */
extern const unsigned char _binary_user_forktest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forktest_elf_end[]   __attribute__((weak));

static void cmd_forktest(void) {
    if (!_binary_user_forktest_elf_start) {
        console_write("forktest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_forktest_elf_end -
                          _binary_user_forktest_elf_start);
    console_write("forktest: exec'ing fork()+waitpid() program...\n");
    int rc = proc_exec_elf(_binary_user_forktest_elf_start, len);
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
    bin_install_one("/bin/args",  _binary_user_args_elf_start,
                                  _binary_user_args_elf_end);
    bin_install_one("/bin/hello", _binary_user_hello_elf_start,
                                  _binary_user_hello_elf_end);
}

/* M34 slice C — `forkexec`: fork()+execv(/bin/args)+waitpid() from ring 3. */
extern const unsigned char _binary_user_forkexec_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forkexec_elf_end[]   __attribute__((weak));

static void cmd_forkexec(void) {
    if (!_binary_user_forkexec_elf_start) {
        console_write("forkexec: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_forkexec_elf_end -
                          _binary_user_forkexec_elf_start);
    console_write("forkexec: exec'ing fork()+execv()+waitpid() program...\n");
    int rc = proc_exec_elf(_binary_user_forkexec_elf_start, len);
    kprintf("forkexec: returned rc=%d\n", rc);
}

/* M34 slice D — `pipetest`: pipe()+dup2()+fork() from ring 3. */
extern const unsigned char _binary_user_pipetest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_end[]   __attribute__((weak));

static void cmd_pipetest(void) {
    if (!_binary_user_pipetest_elf_start) {
        console_write("pipetest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_pipetest_elf_end -
                          _binary_user_pipetest_elf_start);
    console_write("pipetest: exec'ing pipe()+dup2()+fork() program...\n");
    int rc = proc_exec_elf(_binary_user_pipetest_elf_start, len);
    kprintf("pipetest: returned rc=%d\n", rc);
}

/* M34 slice E — `sigtest`: signal()+raise()+handler from ring 3. */
extern const unsigned char _binary_user_sigtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_end[]   __attribute__((weak));

static void cmd_sigtest(void) {
    if (!_binary_user_sigtest_elf_start) {
        console_write("sigtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_sigtest_elf_end -
                          _binary_user_sigtest_elf_start);
    console_write("sigtest: exec'ing signal()+raise() program...\n");
    int rc = proc_exec_elf(_binary_user_sigtest_elf_start, len);
    kprintf("sigtest: returned rc=%d\n", rc);
}

/* M24 socket API — `dnstest`: resolve a hostname over a UDP socket from ring 3. */
extern const unsigned char _binary_user_dnstest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_dnstest_elf_end[]   __attribute__((weak));

static void cmd_dnstest(void) {
    if (!_binary_user_dnstest_elf_start) {
        console_write("dnstest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_dnstest_elf_end -
                          _binary_user_dnstest_elf_start);
    console_write("dnstest: exec'ing UDP-socket DNS resolver...\n");
    int rc = proc_exec_elf(_binary_user_dnstest_elf_start, len);
    kprintf("dnstest: returned rc=%d\n", rc);
}

/* M24 socket API — `httptest`: DNS + TCP-socket HTTP GET from ring 3. */
extern const unsigned char _binary_user_httptest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_httptest_elf_end[]   __attribute__((weak));

static void cmd_httptest(void) {
    if (!_binary_user_httptest_elf_start) {
        console_write("httptest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_httptest_elf_end -
                          _binary_user_httptest_elf_start);
    console_write("httptest: exec'ing TCP-socket HTTP client...\n");
    int rc = proc_exec_elf(_binary_user_httptest_elf_start, len);
    kprintf("httptest: returned rc=%d\n", rc);
}

/* M35 — `threadtest`: threads + a futex mutex from ring 3. */
extern const unsigned char _binary_user_threadtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_threadtest_elf_end[]   __attribute__((weak));

static void cmd_threadtest(void) {
    if (!_binary_user_threadtest_elf_start) {
        console_write("threadtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_threadtest_elf_end -
                          _binary_user_threadtest_elf_start);
    console_write("threadtest: exec'ing threads + futex-mutex program...\n");
    int rc = proc_exec_elf(_binary_user_threadtest_elf_start, len);
    kprintf("threadtest: returned rc=%d\n", rc);
}

/* M35 — `tlstest`: thread-local storage via %gs from ring 3. */
extern const unsigned char _binary_user_tlstest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_tlstest_elf_end[]   __attribute__((weak));

static void cmd_tlstest(void) {
    if (!_binary_user_tlstest_elf_start) {
        console_write("tlstest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_tlstest_elf_end -
                          _binary_user_tlstest_elf_start);
    console_write("tlstest: exec'ing thread-local-storage program...\n");
    int rc = proc_exec_elf(_binary_user_tlstest_elf_start, len);
    kprintf("tlstest: returned rc=%d\n", rc);
}

/* M36 — `posixtest`: broader POSIX syscalls (uname/stat/getdents/clock) from ring 3. */
extern const unsigned char _binary_user_posixtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_posixtest_elf_end[]   __attribute__((weak));

static void cmd_posixtest(void) {
    if (!_binary_user_posixtest_elf_start) {
        console_write("posixtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_posixtest_elf_end -
                          _binary_user_posixtest_elf_start);
    console_write("posixtest: exec'ing POSIX-surface program...\n");
    int rc = proc_exec_elf(_binary_user_posixtest_elf_start, len);
    kprintf("posixtest: returned rc=%d\n", rc);
}

/* §M65 — `uidemo`: the widget toolkit driven from RING 3.  It builds a label,
 * a checkbox, a radio group and a slider by sending a DESCRIPTION over the
 * display bridge, and prints the events they raise — the proof that the
 * toolkit's data-shaped API buys what it was designed for. */
extern const unsigned char _binary_user_uidemo_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_uidemo_elf_end[]   __attribute__((weak));

static void cmd_uidemo(void) {
    if (!_binary_user_uidemo_elf_start) {
        console_write("uidemo: not embedded for this arch\n");
        return;
    }
    if (!gui_is_active()) { console_write("uidemo: the GUI is not running\n"); return; }
    size_t len = (size_t)(_binary_user_uidemo_elf_end - _binary_user_uidemo_elf_start);
    const char* argv[] = { "uidemo" };
    int pid = proc_spawn_argv_under("uidemo", _binary_user_uidemo_elf_start, len,
                                    1, argv, 0, gui_desktop_pid());
    kprintf("uidemo: spawned pid %d\n", pid);
}

/* §M59 — `redirtest`: does STDOUT REDIRECTION work from ring 3?  It exists
 * because a bug reported as "the clipboard will not take a ring-3 write" was
 * really `dup2(fd, 1)` being refused — fds 0/1/2 were not table entries, so no
 * shell could redirect anything, and the failure was silent (the bytes went to
 * the terminal and the exit status said success). */
extern const unsigned char _binary_user_redirtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_redirtest_elf_end[]   __attribute__((weak));

static void cmd_redirtest(void) {
    if (!_binary_user_redirtest_elf_start) {
        console_write("redirtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_redirtest_elf_end -
                          _binary_user_redirtest_elf_start);
    int rc = proc_exec_elf(_binary_user_redirtest_elf_start, len);
    kprintf("redirtest: returned rc=%d\n", rc);
}

/* §M33 Tier 1 — `drvtest`: prove a ring-3 process gets exactly its granted
 * ports, and faults on anything else.
 *
 * The process is ATTACHED to the ps2_mouse manifest before it runs and detached
 * after: without that it holds no manifest and every resource syscall refuses,
 * which is the default and is what every other ring-3 program sees. */
extern const unsigned char _binary_user_drvtest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_drvtest_elf_end[]   __attribute__((weak));

static void cmd_drvtest(void) {
    if (!_binary_user_drvtest_elf_start) {
        console_write("drvtest: not embedded for this arch\n");
        return;
    }
    /* STOP THE IN-KERNEL MOUSE DRIVER FIRST, and this is a bug fix rather than
     * tidiness: the manifest's window is the 8042's, the built-in ps2_mouse
     * holds exactly that window, and drvrt.c's conflict check refused the test
     * every time — so `drvtest` has FAILED on every ordinary boot since it
     * shipped, with a message ("granted request refused") that reads like the
     * grant machinery is broken rather than like the test asked for something
     * already taken.
     *
     * A test that cannot pass in the default configuration is a test nobody
     * runs, and this one is what proves the port bitmap works at all. */
    struct driver* mdrv = driver_find("ps2_mouse");
    int was_running = (mdrv && (driver_state(mdrv) & DRV_S_INITED)) ? 1 : 0;
    if (was_running) driver_stop("ps2_mouse");

    /* A SEPARATE PROCESS, NOT AN EXCURSION ON THIS TASK.
     *
     * The test's last step FAULTS ON PURPOSE — that is its pass condition — and
     * as an excursion the faulting task was the SHELL.  So the machine lost its
     * shell every time the test succeeded, and nothing after `proc_exec_elf`
     * ever ran, including putting the mouse driver back.  A test whose success
     * costs you the terminal you ran it from is one people learn not to run.
     *
     * Reserved BEFORE the spawn and claimed by the child by name (see
     * drvuser.c): the child's first act is to ask for its ports, and attaching
     * after the spawn returned would be a race whose loser is a confusing
     * failure in the test rather than in the thing being tested. */
    if (drvuser_attach(0, "ps2_mouse") != 0) {
        console_write("drvtest: could not attach the manifest\n");
        if (was_running) driver_start("ps2_mouse");
        return;
    }
    size_t len = (size_t)(_binary_user_drvtest_elf_end -
                          _binary_user_drvtest_elf_start);
    int pid = proc_spawn_argv("ps2_mouse", _binary_user_drvtest_elf_start,
                              len, 0, NULL, 0);
    if (pid < 0) {
        console_write("drvtest: could not spawn\n");
        drvuser_detach(drvuser_pid("ps2_mouse"));
        if (was_running) driver_start("ps2_mouse");
        return;
    }
    /* Poll for DISAPPEARANCE — §M57: init is a universal reaper and may collect
     * it first, so a wait would never complete. */
    for (int k = 0; k < 200; k++) {
        struct task* t = task_find(pid);
        if (!t || t->state == TASK_DEAD) break;
        task_msleep(25);
    }
    drvuser_detach(pid);
    if (was_running) driver_start("ps2_mouse");
    kprintf("drvtest: done (the fault above is the pass)\n");
}

/* M36 — `linuxtest`: run a Linux-ABI program under the Linux personality
 * (task->linux_abi), routing its syscalls through the linux_abi translator. */
extern const unsigned char _binary_user_linuxhello_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_linuxhello_elf_end[]   __attribute__((weak));

static void cmd_linuxtest(void) {
    if (!_binary_user_linuxhello_elf_start) {
        console_write("linuxtest: not embedded for this arch\n");
        return;
    }
    size_t len = (size_t)(_binary_user_linuxhello_elf_end -
                          _binary_user_linuxhello_elf_start);
    console_write("linuxtest: exec'ing a Linux-ABI program (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;                 /* Linux syscall ABI for the excursion */
    int rc = proc_exec_elf(_binary_user_linuxhello_elf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("linuxtest: returned rc=%d\n", rc);
}

/* M36 stage 2 — `musltest`: run a REAL, unmodified musl-linked ELF under the
 * Linux personality.  Embedded only when `make musl` produced the binary. */
extern const unsigned char _binary_user_netmuslserv_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netmuslserv_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_end[]   __attribute__((weak));

/* §M46/§M47.1 — `wedgewin`: open a GUI window with a client that then FREEZES.
 * The point is the title-bar X: a frozen client can never observe the close
 * event, so the window must still go away through the compositor's force-kill
 * fallback (after gui.close_grace_ms).  Spawned as an independent Linux-ABI
 * task, never as an excursion — a wedged excursion would take this shell with
 * it, which is exactly the failure mode M46 exists to prevent. */
extern const unsigned char _binary_user_wedgewin_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wedgewin_muslelf_end[]   __attribute__((weak));

static void cmd_wedgewin(void) {
    if (!_binary_user_wedgewin_muslelf_start) {
        console_write("wedgewin: not embedded — run `make musl` then rebuild\n");
        return;
    }
    gui_start();
    task_msleep(300);
    size_t len = (size_t)(_binary_user_wedgewin_muslelf_end -
                          _binary_user_wedgewin_muslelf_start);
    const char* argv[] = { "wedgewin" };
    int pid = proc_spawn_argv_under("wedgewin", _binary_user_wedgewin_muslelf_start,
                                    len, 1, argv, 1, gui_desktop_pid());
    kprintf("wedgewin: spawned pid %d — its window's X must still close it\n", pid);
}

/* §M40 — `pthreadtest`: REAL musl pthreads (clone + futex join), the threading
 * every toolkit and Mesa is built on. */
extern const unsigned char _binary_user_pthreadtest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pthreadtest_muslelf_end[]   __attribute__((weak));

static void cmd_pthreadtest(void) {
    const unsigned char* sp = _binary_user_pthreadtest_muslelf_start;
    if (!sp) { console_write("pthreadtest: not embedded\n"); return; }
    console_write("pthreadtest: exec'ing a REAL musl pthread program...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(sp, (size_t)(_binary_user_pthreadtest_muslelf_end - sp));
    if (me) me->linux_abi = prev;
    kprintf("pthreadtest: returned rc=%d\n", rc);
}

/* §M56 — `epollmusltest`: epoll through UNMODIFIED musl.  `epolltest` proves
 * the mechanism; this proves the TRANSLATION, which is where the trap is —
 * `struct epoll_event` is 12 bytes on i386 AND amd64 but 16 on arm64, so its
 * size does not follow the word size and cannot be derived from it.  The
 * cookie carries bits in both halves of the u64 precisely so an offset error
 * of four bytes shows up instead of looking plausible. */
static void cmd_epollmusltest(void) {
    const unsigned char* a = _binary_user_epollmusl_muslelf_start;
    const unsigned char* b = _binary_user_epollmusl_muslelf_end;
    if (!a || !b) { console_write("epollmusl: not embedded for this arch\n"); return; }
    console_write("epollmusl: exec'ing a REAL musl binary (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("epollmusl: returned rc=%d\n", rc);
}

/* §M24 — `netmuslserv`: the SERVER socket API through an unmodified musl
 * binary.  `tcptest` proves the stack; this proves the ABI — sockaddr_in in
 * network byte order, socklen_t in and out, accept/getpeername agreeing, and
 * a shutdown that really sends a FIN.  One program, and it runs on all three
 * architectures because the handlers behind it are shared (§M50). */
static void cmd_netmuslserv(void) {
    const unsigned char* a = _binary_user_netmuslserv_muslelf_start;
    const unsigned char* b = _binary_user_netmuslserv_muslelf_end;
    if (!a || !b) { console_write("netmuslserv: not embedded for this arch\n"); return; }
    console_write("netmuslserv: exec'ing a REAL musl binary (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("netmuslserv: returned rc=%d\n", rc);
}

static void cmd_musltest(void) {
    if (!_binary_user_muslhello_muslelf_start) {
        console_write("musltest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_muslhello_muslelf_end -
                          _binary_user_muslhello_muslelf_start);
    console_write("musltest: exec'ing a REAL musl binary (Linux personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_muslhello_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("musltest: returned rc=%d\n", rc);
}

/* §M37 — `musldyntest`: run a DYNAMICALLY-linked musl ELF (PT_INTERP set) under
 * the Linux personality.  proc_exec_elf → load_program maps the PIE main + the
 * interpreter (/lib/ld-musl-i386.so.1, provisioned by pkg_init) and starts in
 * ld.so, which relocates + resolves symbols in ring 3 before calling main.
 * If this prints, the whole dynamic-linking path works. */
extern const unsigned char _binary_user_muslhellodyn_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_muslhellodyn_dynelf_end[]   __attribute__((weak));

static void cmd_musldyntest(void) {
    if (!_binary_user_muslhellodyn_dynelf_start) {
        console_write("musldyntest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_muslhellodyn_dynelf_end -
                          _binary_user_muslhellodyn_dynelf_start);
    console_write("musldyntest: exec'ing a DYNAMICALLY-linked musl binary...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_muslhellodyn_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("musldyntest: returned rc=%d\n", rc);
}

/* §M39 stage 1 — `randtest`: exercise the kernel CSPRNG + /dev/urandom.  Prints
 * two 16-byte draws (must differ) and reads /dev/urandom through the VFS. */
static void cmd_randtest(void) {
    uint8_t a[16], b[16];
    random_bytes(a, sizeof a);
    random_bytes(b, sizeof b);
    int differ = 0;
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) { differ = 1; break; }
    console_write("randtest: draw1 =");
    for (int i = 0; i < 16; i++) kprintf(" %x", a[i]);
    console_write("\nrandtest: draw2 =");
    for (int i = 0; i < 16; i++) kprintf(" %x", b[i]);
    kprintf("\nrandtest: two draws %s\n", differ ? "DIFFER (ok)" : "MATCH (BAD)");

    struct file* f = vfs_open("/dev/urandom", VFS_RDONLY);
    if (!f) { console_write("randtest: /dev/urandom open FAILED\n"); return; }
    uint8_t c[8];
    ssize_t r = vfs_read(f, c, sizeof c);
    vfs_close(f);
    kprintf("randtest: /dev/urandom read %d bytes:", (int)r);
    for (int i = 0; i < (int)r && i < 8; i++) kprintf(" %x", c[i]);
    console_write("\n");
}

/* §M38 — `cpptest`: run a DYNAMICALLY-linked C++ program (libstdc++ + libgcc_s)
 * that throws + catches an exception across a .so boundary (libcpplib.so) — the
 * M38 definition-of-done.  Embedded only when the musl C++ toolchain was built
 * (make musl-cross-i686) + a rebuild. */
extern const unsigned char _binary_user_cpptest_cxxelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_cpptest_cxxelf_end[]   __attribute__((weak));

static void cmd_cpptest(void) {
    if (!_binary_user_cpptest_cxxelf_start) {
        console_write("cpptest: not embedded — run `make musl-cross-i686` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_cpptest_cxxelf_end -
                          _binary_user_cpptest_cxxelf_start);
    console_write("cpptest: exec'ing a C++ program (exceptions across a .so)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_cpptest_cxxelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("cpptest: returned rc=%d\n", rc);
}

/* §M39 stage 2 — `crypttest`: run the Mbed TLS crypto self-test (SHA-256 KAT +
 * AES-256-GCM round-trip) in ring 3 under the Linux personality.  Embedded only
 * when `make mbedtls` + a rebuild produced it. */
extern const unsigned char _binary_user_crypttest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_crypttest_muslelf_end[]   __attribute__((weak));

static void cmd_crypttest(void) {
    if (!_binary_user_crypttest_muslelf_start) {
        console_write("crypttest: not embedded — run `make mbedtls` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_crypttest_muslelf_end -
                          _binary_user_crypttest_muslelf_start);
    console_write("crypttest: exec'ing the Mbed TLS crypto self-test...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_crypttest_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("crypttest: returned rc=%d\n", rc);
}

/* §M39 stage 3 — `ssltest`: an in-memory TLS handshake (client+server) via
 * mbedTLS, seeded from our CSPRNG, with a real (trusted self-signed) cert. */
extern const unsigned char _binary_user_ssltest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_ssltest_muslelf_end[]   __attribute__((weak));

static void cmd_ssltest(void) {
    if (!_binary_user_ssltest_muslelf_start) {
        console_write("ssltest: not embedded — run `make mbedtls` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_ssltest_muslelf_end -
                          _binary_user_ssltest_muslelf_start);
    console_write("ssltest: exec'ing an in-memory TLS handshake...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_ssltest_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("ssltest: returned rc=%d\n", rc);
}

/* §M39 stage 3b — `httpstest`: REAL HTTPS from an unmodified musl binary.
 * DNS + TCP :443 + a full mbedTLS handshake over the live socket, verified
 * against the provisioned CA bundle (/etc/ssl/cert.pem), then an HTTP GET over
 * TLS.  Needs QEMU user networking. */
extern const unsigned char _binary_user_httpstest_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_httpstest_muslelf_end[]   __attribute__((weak));

static void cmd_httpstest(void) {
    if (!_binary_user_httpstest_muslelf_start) {
        console_write("httpstest: not embedded — run `make mbedtls` + `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_httpstest_muslelf_end -
                          _binary_user_httpstest_muslelf_start);
    console_write("httpstest: exec'ing a musl HTTPS fetch w/ CA verify (needs QEMU net)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_httpstest_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("httpstest: returned rc=%d\n", rc);
}

/* §M39 stage 3b — `netmusl`: ring-3 networking from an UNMODIFIED musl binary.
 * DNS-resolves example.com over a UDP socket then fetches "/" over TCP — the
 * whole BSD-sockets surface driven through musl's socketcall path (translated
 * by linux_abi.c onto the M24 stack).  Needs QEMU user networking. */
extern const unsigned char _binary_user_netmusl_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netmusl_muslelf_end[]   __attribute__((weak));

static void cmd_netmusl(void) {
    if (!_binary_user_netmusl_muslelf_start) {
        console_write("netmusl: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_netmusl_muslelf_end -
                          _binary_user_netmusl_muslelf_start);
    console_write("netmusl: exec'ing a musl ring-3 HTTP fetch (needs QEMU net)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_netmusl_muslelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("netmusl: returned rc=%d\n", rc);
}

/* §M37 stage 5 — `solibtest`: run a program that links against a SEPARATE
 * shared library (libgreet.so, at /lib).  Exercises ld.so's real work: locate
 * a genuinely separate .so via the search path and resolve symbols across
 * three objects (main → libgreet → libc). */
extern const unsigned char _binary_user_thrdyn_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_thrdyn_dynelf_end[]   __attribute__((weak));

/* `pthreadtest` covers threads in a STATIC binary.  This covers them in a
 * DYNAMIC one, which is where NetSurf died the moment it grew a worker. */
static void cmd_thrdyn(void) {
    if (!_binary_user_thrdyn_dynelf_start) {
        console_write("thrdyn: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_thrdyn_dynelf_end -
                          _binary_user_thrdyn_dynelf_start);
    console_write("thrdyn: threads inside a DYNAMIC musl binary...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_thrdyn_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("thrdyn: returned rc=%d\n", rc);
}

extern const unsigned char _binary_user_solibtest_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_solibtest_dynelf_end[]   __attribute__((weak));

static void cmd_solibtest(void) {
    if (!_binary_user_solibtest_dynelf_start) {
        console_write("solibtest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_solibtest_dynelf_end -
                          _binary_user_solibtest_dynelf_start);
    console_write("solibtest: exec'ing a program that needs a separate .so...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_solibtest_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("solibtest: returned rc=%d\n", rc);
}

/* §M37 stage 7 — `dlopentest`: runtime dlopen/dlsym/dlclose of /lib/libgreet.so. */
extern const unsigned char _binary_user_dlopentest_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_dlopentest_dynelf_end[]   __attribute__((weak));

static void cmd_dlopentest(void) {
    if (!_binary_user_dlopentest_dynelf_start) {
        console_write("dlopentest: not embedded — run `make musl` then rebuild\n");
        return;
    }
    size_t len = (size_t)(_binary_user_dlopentest_dynelf_end -
                          _binary_user_dlopentest_dynelf_start);
    console_write("dlopentest: exec'ing a program that dlopen's a .so...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(_binary_user_dlopentest_dynelf_start, len);
    if (me) me->linux_abi = prev;
    kprintf("dlopentest: returned rc=%d\n", rc);
}

/* §M26 — `wayclient`: a REAL ring-3 Wayland client.  Set up a usock_pair, hand
 * one end to a spawned server task (wl_conn_serve) and install the other as the
 * shell's fd 3, then exec user/wlclient.c — which speaks the Wayland wire
 * protocol over fd 3 from user space.  The client blocks on read(3), the server
 * task runs concurrently and answers; on exit fd_close_all() closes fd 3 and the
 * server sees EOF + tears down. */
extern const unsigned char _binary_user_wlclient_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wlclient_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_wlapp_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_wlapp_elf_end[]      __attribute__((weak));

/* Run a ring-3 Wayland client blob: hand it fd 3 = one end of a usock_pair, run
 * the server on its own task on the other end, exec the client. */
static void run_wayland_client(const char* what, const unsigned char* s,
                               const unsigned char* e) {
    if (!s) { console_write("wayland: client not embedded\n"); return; }
    struct task* me = task_current();
    if (me->fds[3]) { console_write("wayland: fd 3 already in use\n"); return; }

    struct usock *srv, *cli;
    if (usock_pair(&srv, &cli) != 0) { console_write("wayland: usock_pair failed\n"); return; }
    struct ofile* cli_of = ofile_from_sock(cli);
    struct wl_conn* conn = (struct wl_conn*)kmalloc(sizeof *conn);
    if (!cli_of || !conn) { console_write("wayland: out of memory\n");
        if (cli_of) ofile_unref(cli_of); else usock_close(cli);
        usock_close(srv); return; }

    me->fds[3] = cli_of;                          /* client socket = fd 3 */
    wl_conn_init(conn, srv);
    task_spawn_arg("wl-server", wl_server_task, conn);   /* server on its own task */

    kprintf("%s: launching a ring-3 Wayland client (fd 3)...\n", what);
    int rc = proc_exec_elf(s, (size_t)(e - s));
    kprintf("%s: client exited rc=%d\n", what, rc);      /* fd 3 auto-closed → server tears down */
}

/* §M40 — `wayupstream`: the same fd-3 handshake, but the client is a musl binary
 * linked against the REAL libwayland-client.  Two differences from the native
 * clients above: it runs under the Linux personality (it is a musl program), and
 * it needs WAYLAND_SOCKET=3 in its environment — that is upstream libwayland's
 * documented "already-connected fd" mechanism, the same one a real compositor
 * uses to launch its own clients, so no named UNIX socket is required. */
extern const unsigned char _binary_user_wlupstream_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_wlupstream_muslelf_end[]   __attribute__((weak));

/* §M40 — `simpleshm [win]`: run weston-simple-shm, an UNMODIFIED upstream
 * Wayland application (weston's own reference client, compiled straight out of
 * its tree).  Identical plumbing to wayupstream — the point is precisely that
 * nothing about it is special-cased. */
extern const unsigned char _binary_user_simpleshm_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_simpleshm_muslelf_end[]   __attribute__((weak));

static void run_upstream_wl(const char* what, const unsigned char* sp,
                            const unsigned char* ep, int windowed,
                            int argc, const char* const* argv) {
    struct task* me = task_current();
    if (me->fds[3]) { kprintf("%s: fd 3 already in use\n", what); return; }

    struct usock *srv, *cli;
    if (usock_pair(&srv, &cli) != 0) { kprintf("%s: usock_pair failed\n", what); return; }
    struct ofile* cli_of = ofile_from_sock(cli);
    struct wl_conn* conn = (struct wl_conn*)kmalloc(sizeof *conn);
    if (!cli_of || !conn) {
        kprintf("%s: out of memory\n", what);
        if (cli_of) ofile_unref(cli_of); else usock_close(cli);
        usock_close(srv); return;
    }
    me->fds[3] = cli_of;
    wl_conn_init(conn, srv);
    if (windowed) { gui_start(); task_msleep(300); conn->wm_mode = 1; }
    task_spawn_arg("wl-server", wl_server_task, conn);

    kprintf("%s: WAYLAND_SOCKET=3%s\n", what,
            windowed ? " — surface becomes a desktop window" : "");
    proc_set_exec_env("WAYLAND_SOCKET=3");
    int prev = me->linux_abi;
    me->linux_abi = 1;
    int rc = proc_exec_elf_argv(sp, (size_t)(ep - sp), argc, argv);
    me->linux_abi = prev;
    kprintf("%s: client exited rc=%d\n", what, rc);
}

/* §M40 — `waykeymap`: print the xkb keymap this system would hand a Wayland
 * client.  It exists to be VERIFIED, not admired: the text can be piped into a
 * real xkb compiler (`xkbcli compile-keymap`) to prove the generator produces
 * something xkbcommon actually accepts — a client reporting "the string starts
 * with xkb_keymap" proves nothing of the sort. */
static void cmd_waykeymap(void) {
    uint32_t size = 0;
    struct ofile* km = wl_keymap_make(&size);
    if (!km) { console_write("waykeymap: could not build a keymap\n"); return; }
    kprintf("---- BEGIN XKB KEYMAP (%u bytes) ----\n", size);
    struct shm* s = km->shm;
    for (uint32_t off = 0; off + 1 < size; off++) {
        uint32_t fi = off / 4096, fo = off % 4096;
        if ((int)fi >= s->nframes) break;
        console_putchar((char)*(volatile uint8_t*)(uintptr_t)(s->frames[fi] + fo));
    }
    console_write("---- END XKB KEYMAP ----\n");
    ofile_unref(km);
}

/* §M40 — `egltri [win]`: the EGL + GLES2 triangle, the milestone's DoD.
 * Dynamically linked (Mesa is shared objects), so it needs LIBGL_DRIVERS_PATH
 * pointing at the provisioned rasteriser in addition to WAYLAND_SOCKET. */
extern const unsigned char _binary_user_egltri_dynelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_egltri_dynelf_end[]   __attribute__((weak));

static void cmd_egltri(const char* args) {
    const unsigned char* sp = _binary_user_egltri_dynelf_start;
    if (!sp) {
        console_write("egltri: not embedded — build Mesa first "
                      "(see DOCS §4.40)\n");
        return;
    }
    int windowed = 0, dbg = 0;
    for (const char* a = args; a && *a; a++) {
        if (*a == 'w' || *a == 'W') windowed = 1;
        if (*a == 'd' || *a == 'D') dbg = 1;
    }
    const char* argv[] = { "egltri" };
    console_write("egltri: running an EGL + GLES2 client (Mesa swrast)...\n");
    proc_set_exec_env("LIBGL_DRIVERS_PATH=/lib/dri");
    /* `egltri d` — make Mesa narrate its own driver loading.  An EGL error code
     * says WHICH call failed but never why; EGL_BAD_ALLOC out of
     * eglCreateContext covers everything from a missing driver .so to a real
     * out-of-memory, and guessing between those is what this avoids. */
    if (dbg) {
        proc_set_exec_env("EGL_LOG_LEVEL=debug");
        proc_set_exec_env("MESA_DEBUG=1");
    }
    run_upstream_wl("egltri", sp, _binary_user_egltri_dynelf_end,
                    windowed, 1, argv);
}

static void cmd_simpleshm(const char* args) {
    const unsigned char* sp = _binary_user_simpleshm_muslelf_start;
    if (!sp) {
        console_write("simpleshm: not embedded — run "
                      "./scripts/fetch-wayland.sh + `make ARCH=<arch> wayland`\n");
        return;
    }
    int windowed = (args && (args[0] == 'w' || args[0] == 'W'));
    const char* argv[] = { "weston-simple-shm" };
    console_write("simpleshm: running UNMODIFIED weston-simple-shm...\n");
    run_upstream_wl("simpleshm", sp, _binary_user_simpleshm_muslelf_end,
                    windowed, 1, argv);
}

static void cmd_wayupstream(const char* args) {
    /* `wayupstream win` runs the server in SERVER-PER-SURFACE mode, so the
     * client's xdg_toplevel becomes a real desktop window and its commits are
     * blitted into it — the same wm_mode the native `waycomp` demo uses.  With
     * no argument the connection is headless, which keeps the protocol test
     * runnable without a GUI. */
    int windowed = (args && (args[0] == 'w' || args[0] == 'W'));
    const unsigned char* sp = _binary_user_wlupstream_muslelf_start;
    if (!sp) {
        console_write("wayupstream: not embedded — run "
                      "`make ARCH=<arch> wayland` then rebuild\n");
        return;
    }
    struct task* me = task_current();
    if (me->fds[3]) { console_write("wayupstream: fd 3 already in use\n"); return; }

    struct usock *srv, *cli;
    if (usock_pair(&srv, &cli) != 0) {
        console_write("wayupstream: usock_pair failed\n"); return;
    }
    struct ofile* cli_of = ofile_from_sock(cli);
    struct wl_conn* conn = (struct wl_conn*)kmalloc(sizeof *conn);
    if (!cli_of || !conn) {
        console_write("wayupstream: out of memory\n");
        if (cli_of) ofile_unref(cli_of); else usock_close(cli);
        usock_close(srv); return;
    }
    me->fds[3] = cli_of;
    wl_conn_init(conn, srv);
    if (windowed) { gui_start(); task_msleep(300); conn->wm_mode = 1; }
    task_spawn_arg("wl-server", wl_server_task, conn);

    kprintf("wayupstream: running a REAL libwayland-client (WAYLAND_SOCKET=3)%s\n",
            windowed ? " — surface becomes a desktop window" : "");
    proc_set_exec_env("WAYLAND_SOCKET=3");
    int prev = me->linux_abi;
    me->linux_abi = 1;
    const char* argv[] = { "wlupstream", "-w" };
    int rc = proc_exec_elf_argv(sp,
                 (size_t)(_binary_user_wlupstream_muslelf_end - sp),
                 windowed ? 2 : 1, argv);
    me->linux_abi = prev;
    kprintf("wayupstream: client exited rc=%d\n", rc);
}

static void cmd_wayclient(void) {
    run_wayland_client("wayclient", _binary_user_wlclient_elf_start,
                       _binary_user_wlclient_elf_end);
}
static void cmd_wayapp(void) {
    run_wayland_client("wayapp", _binary_user_wlapp_elf_start,
                       _binary_user_wlapp_elf_end);
}

/* §M35.5 + §M36 — `pkgrun <name> [args...]`: exec an INSTALLED package's binary
 * from the /store, with argv.  The package's declared .abi picks the exec
 * personality (pkg_run), so a musl/Linux coreutil and a native program run the
 * same way — the ABI is data, not a special case here. */
static void cmd_pkgrun(const char* line) {
    static char scratch[256];
    const char* argv[16];
    int argc = 0;

    int n = 0;
    while (line[n] && n < 255) { scratch[n] = line[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        char q = 0;
        if (scratch[i] == '"' || scratch[i] == '\'') { q = scratch[i]; i++; }
        argv[argc++] = &scratch[i];
        if (q) { while (scratch[i] && scratch[i] != q) i++; }   /* quoted arg */
        else   { while (scratch[i] && scratch[i] != ' ') i++; }
        if (scratch[i]) scratch[i++] = '\0';
    }
    if (argc == 0) { console_write("usage: pkgrun <name> [args...]\n"); return; }

    int rc = pkg_backend_active()->run(argc, (const char* const*)argv);
    kprintf("pkgrun: '%s' returned rc=%d\n", argv[0], rc);
}

/* §M43 — `tcc <args>`: the on-device C compiler.  Runs the embedded tcc binary
 * (a musl ELF, under the Linux personality) with the shell args as argv, e.g.
 * `tcc /tmp/hello.c -o /tmp/hello`.  tcc reads the source + headers (/usr/
 * include, /usr/lib/tcc/include) and writes a runnable ELF, all on the VFS. */
extern const unsigned char _binary_user_dostcc_start[] __attribute__((weak));
extern const unsigned char _binary_user_dostcc_end[]   __attribute__((weak));

static void cmd_tcc(const char* args) {
    /* §M62 follow-up — the compiler's headers/crt/libs are unpacked HERE, on
     * first use, instead of at every boot (see pkg.h). */
    pkg_ensure_tcc_rootfs();
    if (!_binary_user_dostcc_start) {
        console_write("tcc: not embedded — run `make tcc` then rebuild\n");
        return;
    }
    static char scratch[256];
    const char* argv[18];
    int argc = 0;
    argv[argc++] = "tcc";                        /* argv[0] */
    /* -B sets tcc's base dir explicitly: we run tcc from an embedded blob with
     * argv[0]="tcc" (no path), so tcc can't derive its dir → point it at where
     * pkg.c provisioned libtcc1.a + tcc's own headers. */
    argv[argc++] = "-B/usr/lib/tcc";
    int n = 0;
    while (args[n] && n < 255) { scratch[n] = args[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        argv[argc++] = &scratch[i];
        while (scratch[i] && scratch[i] != ' ') i++;
        if (scratch[i]) scratch[i++] = '\0';
    }
    size_t len = (size_t)(_binary_user_dostcc_end - _binary_user_dostcc_start);
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf_argv(_binary_user_dostcc_start, len, argc,
                                (const char* const*)argv);
    if (me) me->linux_abi = prev;
    kprintf("tcc: returned rc=%d\n", rc);
}

/* §M43 — `exec <path>`: load + run an ELF from the VFS (e.g. one tcc just
 * produced) under the Linux personality, as an excursion that returns to the
 * shell.  This is what closes the "compile → run" loop on d-os. */
static void cmd_exec(const char* path) {
    /* Run via the capturing engine (which opens/reads the ELF itself) so we can
     * also report the byte count — exercising the §M43 stdout-capture path the
     * editor's Output window uses.  Output still echoes to the console. */
    static char cap[4096];
    cap[0] = '\0';
    int rc = dos_run_elf_cap(path, cap, sizeof cap);
    if (rc == -1 && cap[0] == '\0') { kprintf("exec: '%s' not runnable\n", path); return; }
    int caplen = 0; while (cap[caplen]) caplen++;
    kprintf("exec: '%s' returned rc=%d [captured %d bytes]\n", path, rc, caplen);
}

/* §M43 — reusable compile+run engine (devtools.h), shared with the GUI editor's
 * "Compile & Run" button.  Runs the embedded tcc / loads a VFS ELF, both under
 * the Linux personality. */
int dos_tcc_available(void) { return _binary_user_dostcc_start != 0; }

int dos_tcc_compile(const char* src, const char* out) {
    if (!_binary_user_dostcc_start) return -1;
    const char* argv[5] = { "tcc", "-B/usr/lib/tcc", src, "-o", out };
    size_t len = (size_t)(_binary_user_dostcc_end - _binary_user_dostcc_start);
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    proc_exec_elf_argv(_binary_user_dostcc_start, len, 5, (const char* const*)argv);
    if (me) me->linux_abi = prev;
    /* Success proxy: tcc produced a non-empty output file. */
    struct file* f = vfs_open(out, VFS_RDONLY);
    if (!f) return -1;
    int ok = (f->inode && f->inode->size > 0) ? 0 : -1;
    vfs_close(f);
    return ok;
}

int dos_run_elf_cap(const char* path, char* cap, int caplen) {
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    size_t sz = f->inode ? (size_t)f->inode->size : 0;
    if (sz == 0 || sz > (16u << 20)) { vfs_close(f); return -1; }
    uint8_t* img = (uint8_t*)kmalloc(sz);
    if (!img) { vfs_close(f); return -1; }
    ssize_t rd = vfs_read(f, img, sz);
    vfs_close(f);
    if (rd < (ssize_t)sz) { kfree(img); return -1; }
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    if (me && cap && caplen > 0) {            /* §M43 capture stdout */
        cap[0] = '\0';
        me->cap_buf = cap; me->cap_len = 0; me->cap_cap = caplen;
    }
    int rc = proc_exec_elf(img, sz);
    if (me) { me->cap_buf = NULL; me->cap_len = me->cap_cap = 0; me->linux_abi = prev; }
    kfree(img);
    return rc;
}

int dos_run_elf(const char* path) { return dos_run_elf_cap(path, NULL, 0); }

/* §M35.5 — package manager.  Dispatches through the ACTIVE, swappable backend
 * (pkg_backend_active) rather than the store functions directly, so a different
 * pkg-manager implementation transparently serves the same commands. */
static void cmd_pkg(const char* args) {
    const struct pkg_ops* b = pkg_backend_active();
    if (starts_with(args, "build "))   { b->build(args + 6);   return; }
    if (starts_with(args, "install ")) { b->install(args + 8); return; }
    if (starts_with(args, "remove "))  { b->remove(args + 7);  return; }
    if (starts_with(args, "why "))     { b->why(args + 4);     return; }
    if (streq(args, "gc"))             { b->gc();              return; }
    if (streq(args, "list") || !*args) { b->list();            return; }
    if (streq(args, "backend")) {
        kprintf("pkg: active backend '%s' v%s\n", b->name, b->version);
        return;
    }
    console_write("usage: pkg build|install|remove|why <id> | list | gc | backend\n");
}

/* §M35.5 — scripted demo: two hello versions coexist, install hello-2 + args
 * (deps hello-2), gc reclaims the unreferenced hello-1. */
static void cmd_pkgtest(void) {
    console_write("pkgtest: content-addressed store demo\n");
    pkg_build("hello-1");                /* hello 1.0 */
    pkg_build("hello-2");                /* hello 2.0 — coexists (distinct hash) */
    pkg_install("hello-2");
    pkg_install("args");                 /* deps hello-2 → pinned closure */
    console_write("--- store before gc ---\n");
    pkg_list();
    pkg_gc();                            /* reclaims hello-1 (not in any closure) */
    console_write("--- store after gc (hello-1 gone; hello-2 + args kept) ---\n");
    pkg_list();
}

/* -------------------------------------------------------------------- */
/* Configuration commands.                                              */
/* -------------------------------------------------------------------- */

/* §M63 stage 0 — these now delegate to config.c so the ARM serial shell runs
 * the SAME implementation (it had none at all before). */
static void cmd_getconf(const char* key) { config_cmd_getconf(key); }
static void cmd_setconf(const char* args) { config_cmd_setconf(args); }

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
    if (streq(line, "gui"))       { cmd_gui("");     return; }
    if (streq(line, "gui stats")) { cmd_gui_stats(); return; }
    if (streq(line, "termcheck")) { gui_term_check(); return; }
    if (streq(line, "ui"))        { ui_cmd("");      return; }
    if (starts_with(line, "ui ")) { ui_cmd(line + 3); return; }
    if (streq(line, "redirtest")) { cmd_redirtest(); return; }
    if (streq(line, "uidemo"))    { cmd_uidemo();    return; }
    /* MATCH THE EXACT WORD, not the `gui ` prefix: a generic prefix arm added
     * above an existing exact one silently swallows it, and `gui stats` started
     * answering "already running". */
    if (streq(line, "gui stop"))  { cmd_gui("stop");  return; }
    /* §M60 — desktop background.  Implementation is shared with the ARM
     * serial shell (wallpaper.c), so both arches run the same command. */
    if (streq(line, "wallpaper"))        { wallpaper_cmd("");        return; }
    if (starts_with(line, "wallpaper ")) { wallpaper_cmd(line + 10); return; }
    /* §M64 — desktop shortcuts (shared with the ARM serial shell). */
    if (streq(line, "shortcut"))         { shortcut_cmd("");         return; }
    if (starts_with(line, "shortcut "))  { shortcut_cmd(line + 9);   return; }
    /* §M63 — declared settings.  `conf set` VALIDATES; `setconf` does not
     * (it must stay able to set undeclared keys). */
    if (streq(line, "conf"))             { settings_cmd("");         return; }
    if (starts_with(line, "conf "))      { settings_cmd(line + 5);   return; }
    /* §M58/§M59 — the clipboard and the primary selection. */
    if (streq(line, "clip"))             { clipboard_cmd("");        return; }
    if (starts_with(line, "clip "))      { clipboard_cmd(line + 5);  return; }
    /* §M61 — display mode. */
    if (streq(line, "mode"))             { display_cmd("");         return; }
    if (starts_with(line, "mode "))      { display_cmd(line + 5);   return; }
    /* §M62 — the boot screen, and the way to test that a fault removes it. */
    if (streq(line, "splash"))           { splash_cmd("");          return; }
    if (starts_with(line, "splash "))    { splash_cmd(line + 7);    return; }
    /* M22.2: GUI app registry access from any shell. */
    if (streq(line, "launch"))          { cmd_launch("");        return; }
    if (starts_with(line, "launch "))   { cmd_launch(line + 7);  return; }
    if (streq(line, "about"))  { cmd_about();      return; }
    if (streq(line, "lsmod"))  { modload_list();    return; }
    if (streq(line, "lsdrv"))  { driver_list();    return; }
    /* §M33 stage 5 — what this machine can enforce against a DEVICE.  Its own
     * verb rather than a line in `lsdrv`, because the answer is a property of
     * the machine and not of any driver: it is the same on a box with no
     * drivers placed at all, and that is exactly when somebody deciding whether
     * to place one wants to read it. */
    if (starts_with(line, "iommu")) { iommu_cmd(line + 5); return; }
    /* §M33 — the DMA client the milestone was waiting for.  `edutest` is a
     * round trip through a real bus-master engine; `eduescape <addr>` aims that
     * engine somewhere it was never granted, which is the only way to find out
     * whether the confinement is real. */
    if (streq(line, "edutest")) { edu_test(); return; }
    if (starts_with(line, "eduescape ")) {
        const char* p = line + 10;
        unsigned long long a = 0;
        if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) p += 2;
        while ((*p>='0'&&*p<='9')||((*p|32)>='a'&&(*p|32)<='f'))
            { int d = (*p<='9')?*p-'0':((*p|32)-'a'+10); a = a*16+(unsigned)d; p++; }
        edu_escape(a);
        return;
    }
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
    if (starts_with(line, "cp "))     { cmd_cp   (line + 3); return; }
    if (starts_with(line, "rm "))     { cmd_rm   (line + 3); return; }
    if (starts_with(line, "mv "))     { cmd_mv   (line + 3); return; }

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
    if (streq(line, "drvtest"))        { cmd_drvtest(); return; }
    if (streq(line, "forktest"))       { cmd_forktest(); return; }
    if (streq(line, "forkexec"))       { cmd_forkexec(); return; }
    if (streq(line, "pipetest"))       { cmd_pipetest(); return; }
    if (streq(line, "sigtest"))        { cmd_sigtest(); return; }
    if (streq(line, "dnstest"))        { cmd_dnstest(); return; }
    if (streq(line, "httptest"))       { cmd_httptest(); return; }
    if (streq(line, "threadtest"))     { cmd_threadtest(); return; }
    if (streq(line, "tlstest"))        { cmd_tlstest(); return; }
    if (streq(line, "linuxtest"))      { cmd_linuxtest(); return; }
    if (streq(line, "wedgewin"))       { cmd_wedgewin(); return; }
    if (streq(line, "pthreadtest"))    { cmd_pthreadtest(); return; }
    if (streq(line, "musltest"))       { cmd_musltest(); return; }
    if (streq(line, "musldyntest"))    { cmd_musldyntest(); return; }
    if (streq(line, "randtest"))       { cmd_randtest(); return; }
    if (streq(line, "crypttest"))      { cmd_crypttest(); return; }
    if (streq(line, "ssltest"))        { cmd_ssltest(); return; }
    if (streq(line, "netmusl"))        { cmd_netmusl(); return; }
    if (streq(line, "httpstest"))      { cmd_httpstest(); return; }
    if (streq(line, "cpptest"))        { cmd_cpptest(); return; }
    if (streq(line, "thrdyn"))        { cmd_thrdyn(); return; }
    if (streq(line, "solibtest"))      { cmd_solibtest(); return; }
    if (streq(line, "dlopentest"))     { cmd_dlopentest(); return; }
    if (streq(line, "pkg"))            { cmd_pkg("");        return; }
    if (starts_with(line, "pkg "))     { cmd_pkg(line + 4);  return; }
    if (streq(line, "pkgtest"))        { cmd_pkgtest();      return; }
    if (streq(line, "waytest"))        { wl_selftest();      return; }
    if (streq(line, "waydemo"))        { wl_visible_demo();  return; }
    if (streq(line, "waywin"))         { wl_window_demo();   return; }
    if (streq(line, "wayinput"))       { wl_input_demo();    return; }
    if (streq(line, "wayclient"))      { cmd_wayclient();    return; }
    if (streq(line, "egltri"))         { cmd_egltri(""); return; }
    if (starts_with(line, "egltri "))  { cmd_egltri(line + 7); return; }
    if (streq(line, "waykeymap"))      { cmd_waykeymap(); return; }
    if (streq(line, "simpleshm"))      { cmd_simpleshm(""); return; }
    if (starts_with(line, "simpleshm "))   { cmd_simpleshm(line + 10); return; }
    if (streq(line, "wayupstream"))    { cmd_wayupstream(""); return; }
    if (starts_with(line, "wayupstream ")) { cmd_wayupstream(line + 12); return; }
    if (streq(line, "wayapp"))         { cmd_wayapp();       return; }
    if (streq(line, "waycomp"))        { wl_compositor_demo(); return; }
    if (starts_with(line, "pkgrun "))  { cmd_pkgrun(line + 7); return; }
    if (starts_with(line, "tcc "))     { cmd_tcc(line + 4); return; }
    if (starts_with(line, "exec "))    { cmd_exec(line + 5); return; }
    if (streq(line, "tcc"))            { console_write("usage: tcc <src.c> -o <out> [args]\n"); return; }
    if (streq(line, "posixtest"))      { cmd_posixtest();    return; }
    if (streq(line, "waittest"))       { cmd_waittest(); return; }
    if (streq(line, "service"))        { cmd_service("");        return; }
    if (starts_with(line, "service ")) { cmd_service(line + 8);  return; }
    if (streq(line, "bustest"))        { cmd_bustest(); return; }
    if (streq(line, "faulttest"))     { cmd_faulttest(); return; }
    if (streq(line, "fputest"))       { cmd_fputest(); return; }
    if (streq(line, "archtest"))      { cmd_archtest(); return; }
    if (streq(line, "crash"))         { cmd_crash(); return; }
    if (streq(line, "wdtest"))         { cmd_wdtest(); return; }
    /* §M31 L3 — deliberately wedge CPU 0 with IRQs off; the ib700 NMI must
     * fire and reboot.  The box going down IS the pass. */
    if (streq(line, "hardlock"))       { watchdog_hardlock_test(); return; }
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
    if (streq(line, "netsurf"))        { cmd_netsurf(""); return; }
    if (starts_with(line, "netsurf ")) { cmd_netsurf(line + 8); return; }
    if (streq(line, "nettest"))        { cmd_nettest();  return; }
    /* §M24 — these live in kernel/core/net_cmds.c so the AArch64 serial shell
     * runs the SAME tests rather than a second copy of them. */
    if (streq(line, "dhcp"))           { netcmd_dhcp("");   return; }
    if (starts_with(line, "dhcp "))    { netcmd_dhcp(line + 5); return; }
    if (streq(line, "netstat"))        { netcmd_netstat();  return; }
    if (streq(line, "tcptest"))        { netcmd_tcptest(""); return; }
    if (starts_with(line, "tcptest ")) { netcmd_tcptest(line + 8); return; }
    if (streq(line, "tcploss"))        { netcmd_tcploss(""); return; }
    if (starts_with(line, "tcploss ")) { netcmd_tcploss(line + 8); return; }
    if (streq(line, "lo"))             { netcmd_lo("");     return; }
    if (starts_with(line, "lo "))      { netcmd_lo(line + 3); return; }
    if (streq(line, "netstorm"))       { cmd_netstorm(""); return; }
    if (starts_with(line, "netstorm ")) { cmd_netstorm(line + 9); return; }
    if (streq(line, "lsaudio"))        { audio_list();   return; }
    if (streq(line, "drv"))            { driver_cmd("");        return; }
    if (starts_with(line, "drv "))     { driver_cmd(line + 4);  return; }
    /* §M67 — loadable modules.  Note these go BELOW no generic prefix arm:
     * §4.67.1's regression was a `gui ` prefix dispatched above the exact
     * `gui stats`, which silently swallowed it. */
    if (streq(line, "lsmod"))          { modload_list();             return; }
    if (streq(line, "insmod"))         { modload_cmd_insmod("");     return; }
    if (starts_with(line, "insmod "))  { modload_cmd_insmod(line+7); return; }
    if (streq(line, "rmmod"))          { modload_cmd_rmmod("");      return; }
    if (starts_with(line, "rmmod "))   { modload_cmd_rmmod(line+6);  return; }
    if (streq(line, "ksyms"))          { ksym_list("");             return; }
    if (starts_with(line, "ksyms "))   { ksym_list(line + 6);       return; }
    if (streq(line, "beep"))           { cmd_beep();     return; }
    if (starts_with(line, "tone "))    { cmd_tone(line + 5); return; }
    /* §M23 stage 2 — the implementation lives in audio.c, not here, so the ARM
     * serial REPL runs the same one (§M24's rule). */
    if (starts_with(line, "play "))    { audio_cmd_play(line + 5); return; }
    if (starts_with(line, "rec "))     { audio_cmd_rec(line + 4);     return; }
    if (streq(line, "volume"))         { audio_cmd_volume("");        return; }
    if (starts_with(line, "volume "))  { audio_cmd_volume(line + 7);  return; }
    if (streq(line, "ps"))             { task_list();    return; }
    if (starts_with(line, "kill "))    { cmd_kill(line + 5); return; }
    if (starts_with(line, "fkill "))   { cmd_fkill(line + 6); return; }
    if (streq(line, "wedge"))          { cmd_wedge();     return; }
    if (streq(line, "spawn"))          { cmd_spawn();    return; }
    if (streq(line, "killstorm"))      { cmd_killstorm("");        return; }
    if (streq(line, "rqcheck"))        { cmd_rqcheck("");          return; }
    if (streq(line, "schedstorm"))     { cmd_schedstorm("");       return; }
    if (streq(line, "timerfdtest"))    { cmd_timerfdtest("");      return; }
    if (streq(line, "epolltest"))      { cmd_epolltest();          return; }
    if (streq(line, "epollmusltest"))  { cmd_epollmusltest();      return; }
    if (streq(line, "netmuslserv"))    { cmd_netmuslserv();        return; }
    if (starts_with(line, "timerfdtest ")) { cmd_timerfdtest(line + 12); return; }
    if (streq(line, "alarmtest"))      { cmd_alarmtest("");        return; }
    if (starts_with(line, "alarmtest ")) { cmd_alarmtest(line + 10); return; }
    if (starts_with(line, "killstorm ")) { cmd_killstorm(line + 10); return; }
    if (starts_with(line, "schedstorm ")) { cmd_schedstorm(line + 11); return; }
    if (streq(line, "yield"))          { task_yield();   return; }
    if (streq(line, "loop"))           { cmd_loop("");   return; }
    if (starts_with(line, "loop "))    { cmd_loop(line + 5); return; }
    if (streq(line, "loopstop"))       { cmd_loopstop(); return; }
    if (starts_with(line, "run "))     { cmd_run(my_vc, line + 4); return; }
    if (streq(line, "pane"))           { cmd_pane(my_vc, "");      return; }
    if (starts_with(line, "pane "))    { cmd_pane(my_vc, line + 5); return; }
    if (streq(line, "lslayout"))       { cmd_lslayout();             return; }
    if (starts_with(line, "setlayout ")) { cmd_setlayout(line + 10); return; }
    if (streq(line, "lscpu"))          { cmd_lscpu();                return; }
    if (starts_with(line, "nice "))    { cmd_nice(line + 5);         return; }
    if (streq(line, "wqtest"))         { cmd_wqtest("");             return; }
    if (streq(line, "abi"))            { cmd_abi();                  return; }
    if (starts_with(line, "wqtest "))  { cmd_wqtest(line + 7);       return; }
    if (streq(line, "ktime"))          { cmd_ktime();                return; }
    if (streq(line, "ktimer"))         { cmd_ktimer();               return; }
    if (streq(line, "sched"))          { cmd_sched("");              return; }
    if (starts_with(line, "sched "))   { cmd_sched(line + 6);        return; }
    if (starts_with(line, "taskset "))  { cmd_taskset(line + 8);     return; }
    if (streq(line, "slabinfo"))       { cmd_slabinfo();             return; }
    if (streq(line, "buddyinfo"))      { cmd_buddyinfo();            return; }
    if (streq(line, "saveconf"))       { config_cmd_saveconf(); return; }
    if (starts_with(line, "getconf ")) { cmd_getconf(line + 8); return; }
    if (starts_with(line, "setconf ")) { cmd_setconf(line + 8); return; }
    if (streq(line, "memcheck")){
        /* Walk every buddy free list and report the first inconsistency —
         * a diagnostic for the latent large-order corruption noted in
         * PLAN.md §M39.  "consistent" = links + page_state all agree. */
        pmm_validate("memcheck");
        return;
    }
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
        crash_boot_clean();     /* §M47 — an orderly exit: disarm the marker */
        system_power_off();                             /* normally never returns */
        return;
    }
    if (streq(line, "reboot")) {
        console_write("rebooting...\n");
        crash_boot_clean();     /* §M47 — an orderly exit: disarm the marker */
        system_reboot();                                /* normally never returns */
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
    { static int pkg_ready = 0; if (!pkg_ready) { pkg_ready = 1; pkg_init(); } }  /* §M35.5 store */
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
