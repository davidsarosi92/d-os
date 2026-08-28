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
#include "net.h"
#include "net_cmds.h"
#include "config.h"       /* §M63 stage 0 — setconf/getconf/saveconf */
#include "wallpaper.h"
#include "shortcut.h"
#include "settings.h"
#include "clipboard.h"
#include "gui.h"
#include "splash.h"
#include "watchdog.h"   /* §M60 — one implementation, both shells */
#include "pmm.h"
#include "task.h"
#include "percpu.h"    /* §M57 — smp_ncpus() for the runqueue audit + storm */
#include "timer.h"
#include "audio.h"
#include "driver.h"
#include "iommu.h"      /* §M23 stage 2 — lsaudio / play, from the core */
#include "modload.h"   /* §M67 — insmod / rmmod / lsmod */
#include "ksym.h"      /* §M67 — ksyms */
#include "syscall.h"   /* §M53 stage 3 — timerfd + setitimer self-tests */
#include "ktimer.h"
#include "pkg.h"
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
            "  usertest          drop to EL0 and run a userspace program\n"
            "  forktest          fork()+waitpid() self-test (§A1)\n"
            "  pipetest          pipe()+dup2() self-test (§A1)\n"
            "  sigtest           signal delivery self-test (§A1)\n"
            "  musltest          run an unmodified static musl binary (§A2)\n"
            "  pkg list | pkg install <name>   package store (§A3)\n"
            "  pkgrun <n> [args] run a store package (§A3)\n"
            "  rqcheck           check the runqueue invariant now (§M57)\n"
            "  schedstorm [n]    affinity+nice churn, audited (§M57)\n"
            "  lsnic             network devices + poller stats (§M24)\n"
            "  ping <ip> [n]     ICMP echo (127.0.0.1 works with no NIC)\n"
            "  netstat           the TCP connection table (§M24)\n"
            "  tcptest [n]       echo server + n concurrent clients over lo\n"
            "  tcploss [pm] [kb] a stream that survives a lossy link\n"
            "  dhcp [status]     ask the network for an address (§M24)\n"
            "  netmuslserv       bind/listen/accept through real musl (§M24)\n"
            "  clear             clear the screen\n");
}

/* §M24 — `ping <ip> [count]`.  The route decides the device, so 127.0.0.1
 * works on a board with no NIC attached at all — which is the usual headless
 * ARM run, and the reason this is worth having here. */
static void cmd_ping(char* args) {
    while (*args == ' ') args++;
    char ip[32]; int i = 0;
    while (args[i] && args[i] != ' ' && i < 31) { ip[i] = args[i]; i++; }
    ip[i] = '\0';
    if (!i) { kprintf("usage: ping <ip> [count]\n"); return; }
    uint32_t a;
    if (net_parse_ip(ip, &a) != 0) { kprintf("ping: bad IP\n"); return; }
    int count = 3;
    while (args[i] == ' ') i++;
    if (args[i]) { int c = 0;
        for (int j = i; args[j] >= '0' && args[j] <= '9'; j++) c = c * 10 + (args[j] - '0');
        if (c > 0 && c <= 16) count = c; }
    struct net_device* dev = net_route(a);
    if (!dev) { kprintf("ping: no route to host\n"); return; }
    net_ping(dev, a, count);
}

/* Phase L — the EL0/userspace self-test (syscall.c), the ARM analogue of the
 * x86 shell's `ringtest`.  Runs a tiny program at EL0 that SYS_PRINTs + SYS_EXITs. */
int aarch64_usertest(void);
static void cmd_usertest(void) { aarch64_usertest(); }

/* §A1 — the POSIX self-tests.  These live here rather than in shell.c because
 * the ARM serial console runs THIS minimal REPL; the full shell.c only comes up
 * on a VC once the framebuffer + virtio-input path is running, which a headless
 * boot has no way to drive.  PLAN_AARCH64's definition of done for A1 is
 * "passes over the serial shell" for exactly that reason. */
extern const unsigned char _binary_user_forktest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_forktest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_pipetest_elf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_start[]  __attribute__((weak));
extern const unsigned char _binary_user_sigtest_elf_end[]    __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_epollmusl_muslelf_end[]   __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_muslhello_muslelf_end[]   __attribute__((weak));

int proc_exec_elf(const unsigned char* image, unsigned long len);

static void run_blob(const char* name, const unsigned char* a,
                     const unsigned char* b) {
    if (!a || !b) { kprintf("%s: not embedded for this arch\n", name); return; }
    kprintf("%s: exec'ing...\n", name);
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    kprintf("%s: returned rc=%d\n", name, rc);
}
static void cmd_forktest(void) {
    run_blob("forktest", _binary_user_forktest_elf_start, _binary_user_forktest_elf_end);
}
static void cmd_pipetest(void) {
    run_blob("pipetest", _binary_user_pipetest_elf_start, _binary_user_pipetest_elf_end);
}
static void cmd_sigtest(void) {
    run_blob("sigtest", _binary_user_sigtest_elf_start, _binary_user_sigtest_elf_end);
}

/* A2 — run an UNMODIFIED static musl binary under the Linux/arm64 personality.
 * The flag is set around the excursion exactly as shell.c does it on x86: the
 * personality is a property of the running task, and this shell task borrows it
 * for the duration of the call. */
/* A3 — `pkg install <name>` / `pkg list`.  The store's recipes are REGISTERED
 * at boot (pkg_init) but not installed; installing is the explicit act that
 * builds the content-addressed path and links it into the profile. */
static void cmd_pkg(const char* args) {
    while (*args == ' ') args++;
    if (s_eq(args, "list") || !*args) { pkg_list(); return; }
    const char* p = args;
    if (p[0]=='i' && p[1]=='n' && p[2]=='s' && p[3]=='t' && p[4]=='a' &&
        p[5]=='l' && p[6]=='l' && p[7]==' ') {
        p += 8;
        while (*p == ' ') p++;
        int rc = pkg_install(p);
        kprintf("pkg: install '%s' rc=%d\n", p, rc);
        return;
    }
    kprintf("usage: pkg list | pkg install <name>\n");
}

/* A3 — run a STORE package: `pkgrun <name> [args...]`.  The backend is chosen
 * by data (pkg_backend_active), and the package's own recipe declares its ABI,
 * so nothing here knows or cares that these are musl/Linux binaries. */
static void cmd_pkgrun(const char* line) {
    static char scratch[256];
    const char* argv[16];
    int argc = 0, n = 0;
    while (line[n] && n < 255) { scratch[n] = line[n]; n++; }
    scratch[n] = '\0';
    int i = 0;
    while (scratch[i] && argc < 16) {
        while (scratch[i] == ' ') i++;
        if (!scratch[i]) break;
        char q = 0;
        if (scratch[i] == '"' || scratch[i] == '\'') { q = scratch[i]; i++; }
        argv[argc++] = &scratch[i];
        if (q) { while (scratch[i] && scratch[i] != q) i++; }
        else   { while (scratch[i] && scratch[i] != ' ') i++; }
        if (scratch[i]) scratch[i++] = '\0';
    }
    if (argc == 0) { kprintf("usage: pkgrun <name> [args...]\n"); return; }
    int rc = pkg_backend_active()->run(argc, (const char* const*)argv);
    kprintf("pkgrun: '%s' returned rc=%d\n", argv[0], rc);
}

/* §M53 — the same clock report the x86 shell prints.  On this arch the source
 * needs no calibration: the architecture defines CNTPCT_EL0's rate and hands it
 * over in CNTFRQ_EL0, so what is worth checking here is only that the numbers
 * agree with a tick-based sleep. */
/* §M53 stage 3 — the ARM half of `timerfdtest`.  Same code path, coarser
 * floor: this arch ticks at 100 Hz, so the error is ~10 ms rather than ~1 ms,
 * and the test says so instead of looking broken. */
static void cmd_timerfdtest(const char* args) {
    unsigned period_ms = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) period_ms = period_ms * 10 + (unsigned)(*args - '0');
    if (period_ms == 0) period_ms = 50;

    int fd = sys_timerfd_create();
    if (fd < 0) { kprintf("timerfd: create failed\n"); return; }
    uint64_t period_ns = (uint64_t)period_ms * 1000000ull;
    uint64_t t0 = timer_now_ns();
    if (sys_timerfd_settime(fd, 0, period_ns, period_ns) != 0) {
        kprintf("timerfd: settime failed\n"); sys_close(fd); return;
    }
    kprintf("timerfd: fd %d, period %u ms — waiting via poll(2)\n", fd, period_ms);
    for (int i = 1; i <= 5; i++) {
        struct pollfd pf = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = sys_poll_k(&pf, 1, -1);
        uint64_t now = timer_now_ns();
        uint64_t buf = 0;
        long got = sys_read_k(fd, &buf, sizeof buf);
        uint64_t want = t0 + (uint64_t)i * period_ns;
        long long err_us = (long long)((now - want) / 1000ull);
        if (now < want) err_us = -(long long)((want - now) / 1000ull);
        kprintf("  tick %d: poll=%d read=%d expirations=%u  drift %s%u us\n",
                i, r, (int)got, (unsigned)buf, err_us < 0 ? "-" : "+",
                (unsigned)(err_us < 0 ? -err_us : err_us));
    }
    sys_close(fd);
    kprintf("timerfd: ok\n");
}

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
        kprintf("alarm: setitimer failed\n"); return;
    }
    for (int i = 0; i < 400; i++) {
        if (me->sig_pending & (1u << SIGALRM)) break;
        task_msleep(5);
    }
    uint64_t dt = timer_now_ns() - t0;
    if (me->sig_pending & (1u << SIGALRM)) {
        me->sig_pending &= ~(1u << SIGALRM);
        kprintf("alarm: SIGALRM posted after %u us (asked for %u us)\nalarm: ok\n",
                (unsigned)(dt / 1000), ms * 1000);
    } else {
        kprintf("alarm: FAIL — SIGALRM never arrived\n");
    }
    sys_setitimer_ns(0, 0);
}

/* §M54 — the ARM half of `killstorm` (see the x86 shell for the full
 * rationale).  The scheduler is arch-independent core code, so the race this
 * exercises — a task killed while blocked, on several CPUs at once — is the
 * same race here; only the shell it is driven from differs.  Running it on
 * ARM is what makes "fixed on all three arches" a measurement rather than an
 * inference from shared source. */
static volatile int g_ks_alive;

static void killstorm_victim(void) {
    while (!task_should_stop()) task_msleep(2);
    __atomic_sub_fetch(&g_ks_alive, 1, __ATOMIC_RELAXED);
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

    kprintf("killstorm: %d rounds x %d blocked tasks\n", rounds, per);
    int spawned = 0, killed = 0;
    for (int r = 0; r < rounds; r++) {
        int pids[32];
        int n = 0;
        for (int i = 0; i < per; i++) {
            struct task* t = task_spawn("ks-victim", killstorm_victim);
            if (!t) break;
            pids[n++] = t->pid;
            __atomic_add_fetch(&g_ks_alive, 1, __ATOMIC_RELAXED);
        }
        spawned += n;
        task_msleep(6);
        for (int i = 0; i < n; i++) { task_kill(pids[i]); killed++; }
        task_msleep(10);
    }
    for (int i = 0; i < 200 && __atomic_load_n(&g_ks_alive, __ATOMIC_RELAXED); i++)
        task_msleep(10);
    kprintf("killstorm: done — %d spawned, %d killed, %d still alive\n",
            spawned, killed, __atomic_load_n(&g_ks_alive, __ATOMIC_RELAXED));
}

/* §M57 — the ARM half of the runqueue audit + scheduler storm.  Same argument
 * as killstorm above: the runqueue, cpu_home and the balancer are core code, so
 * the invariant being checked is literally the same code — but "the same code"
 * is an inference, and ARM has already produced two scheduler bugs x86 could
 * not have (SP_EL0, the per-CPU tick divisor).  Running the checker here is
 * what makes three arches a measurement. */
static volatile int g_ss_worst;

static void schedstorm_hog(void) {
    volatile unsigned x = 0;
    while (!task_should_stop()) { for (int i = 0; i < 20000; i++) x += i; task_yield(); }
}

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

static void cmd_rqcheck(void) {
    struct rq_audit a;
    int bad = task_rq_audit(&a);
    rq_audit_print("rqcheck", &a, bad);
}

static void cmd_schedstorm(const char* args) {
    int rounds = 0;
    while (*args == ' ') args++;
    for (; *args >= '0' && *args <= '9'; args++) rounds = rounds * 10 + (*args - '0');
    if (rounds <= 0) rounds = 40;

    int pids[8], n = 0;
    for (int i = 0; i < 8; i++) {
        struct task* t = task_spawn("ss-hog", schedstorm_hog);
        if (!t) break;
        pids[n++] = t->pid;
    }
    kprintf("schedstorm: %d rounds, %d hogs, %d cpus\n", rounds, n, smp_ncpus());

    g_ss_worst = 0;
    unsigned seed = 0x1234567u;
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            struct task* t = task_find(pids[i]);
            if (!t) continue;
            uint32_t mask = ((seed >> 8) & ((1u << (uint32_t)smp_ncpus()) - 1u));
            if (!mask) mask = 1u;
            task_set_affinity(t, mask);
            task_set_nice(pids[i], (int)((seed >> 16) % 21u) - 10);
        }
        struct rq_audit a;
        int hard = task_rq_audit(&a);   /* structural only; 5+6 at rest */
        if (hard > g_ss_worst) g_ss_worst = hard;
        task_msleep(4);
    }

    for (int i = 0; i < n; i++) {
        struct task* t = task_find(pids[i]);
        if (t) task_set_affinity(t, 0xFFFFFFFFu);
        task_set_nice(pids[i], 0);
        task_kill(pids[i]);
    }
    task_msleep(200);                     /* at rest: every rule, rule 5 too */
    struct rq_audit after;
    int bad1 = task_rq_audit(&after);
    rq_audit_print("schedstorm: after ", &after, bad1);
    kprintf("schedstorm: worst mid-churn %d, at rest %d -> %s\n",
            g_ss_worst, bad1, (g_ss_worst == 0 && bad1 == 0) ? "ok" : "FAIL");
}

/* §M53 — the ARM half of the timer-accuracy report.  Its floor is 10 ms here,
 * not 1 ms: this arch ticks at 100 Hz, and ktimer_expire runs on the tick. */
static void cmd_ktimer(void) {
    uint32_t pending; uint64_t fired, late;
    ktimer_stats(&pending, &fired, &late);
    kprintf("ktimer: %u pending, %u fired, worst lateness %u us\n",
            pending, (unsigned)fired, (unsigned)(late / 1000ull));
    static const unsigned req_us[] = { 500, 1000, 5000, 20000, 100000 };
    for (unsigned i = 0; i < sizeof req_us / sizeof req_us[0]; i++) {
        uint64_t want = (uint64_t)req_us[i] * 1000ull;
        uint64_t t0 = timer_now_ns();
        task_sleep_until_ns(t0 + want);
        kprintf("  %u us -> %u us\n", req_us[i],
                (unsigned)((timer_now_ns() - t0) / 1000ull));
    }
    ktimer_stats(&pending, &fired, &late);
    kprintf("  worst lateness %u us (floor = one 10 ms tick on this arch)\n",
            (unsigned)(late / 1000ull));
}

static void cmd_ktime(void) {
    kprintf("clock source : %s", timer_source_name());
    if (timer_source_hz())
        kprintf(" (%u kHz)", (unsigned)(timer_source_hz() / 1000u));
    kprintf("\n resolution  : %u ns\n", (unsigned)timer_res_ns());
    uint64_t a = timer_now_ns(), b = timer_now_ns(), c = timer_now_ns();
    kprintf(" back-to-back: %u ns, %u ns apart\n",
            (unsigned)(b - a), (unsigned)(c - b));
    uint64_t t0 = timer_now_ns();
    task_msleep(100);
    uint64_t t1 = timer_now_ns();
    kprintf(" 100 ms sleep: measured %u us by the ns clock\n",
            (unsigned)((t1 - t0) / 1000ull));
    kprintf(" uptime      : %u ms\n", (unsigned)(t1 / 1000000ull));
}

static void cmd_musltest(void) {
    const unsigned char* a = _binary_user_muslhello_muslelf_start;
    const unsigned char* b = _binary_user_muslhello_muslelf_end;
    if (!a || !b) { kprintf("musltest: not embedded for this arch\n"); return; }
    kprintf("musltest: exec'ing a REAL musl binary (Linux/arm64 personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("musltest: returned rc=%d\n", rc);
}

/* §M56 — the arm64 half of the epoll ABI proof, and the interesting half:
 * this is the arch where `struct epoll_event` is 16 bytes rather than 12,
 * because Linux packs it on x86_64 only.  A kernel that derived the size from
 * the word width would pass on i386, pass on amd64, and fail exactly here. */
static void cmd_epollmusltest(void) {
    const unsigned char* a = _binary_user_epollmusl_muslelf_start;
    const unsigned char* b = _binary_user_epollmusl_muslelf_end;
    if (!a || !b) { kprintf("epollmusl: not embedded for this arch\n"); return; }
    kprintf("epollmusl: exec'ing a REAL musl binary (Linux/arm64 personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("epollmusl: returned rc=%d\n", rc);
}

/* §M24 — the socket ABI through an unmodified musl binary, on ARM.  The same
 * program the x86 shells run: the handlers behind it are shared (§M50), so
 * this is the claim that a new architecture costs a table and not a port. */
extern const unsigned char _binary_user_netmuslserv_muslelf_start[] __attribute__((weak));
extern const unsigned char _binary_user_netmuslserv_muslelf_end[]   __attribute__((weak));
static void cmd_netmuslserv(void) {
    const unsigned char* a = _binary_user_netmuslserv_muslelf_start;
    const unsigned char* b = _binary_user_netmuslserv_muslelf_end;
    if (!a || !b) { kprintf("netmuslserv: not embedded for this arch\n"); return; }
    kprintf("netmuslserv: exec'ing a REAL musl binary (Linux/arm64 personality)...\n");
    struct task* me = task_current();
    int prev = me ? me->linux_abi : 0;
    if (me) me->linux_abi = 1;
    int rc = proc_exec_elf(a, (unsigned long)(b - a));
    if (me) me->linux_abi = prev;
    kprintf("netmuslserv: returned rc=%d\n", rc);
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
        /* §M23 stage 2 — the same implementation the x86 shell calls (§M24's
         * rule).  This arch has the audio CORE and no audio DEVICE, so today
         * they answer "no audio devices" — which is the honest failure, and
         * strictly better than "unknown command" telling the user the feature
         * does not exist.  virtio-sound is what fills the gap. */
        else if (s_eq(cmd, "lsaudio")) audio_list();
        else if (s_eq(cmd, "drv"))     driver_cmd(args);
        /* §M33 stage 5 — the same report both x86 shells give.  ONE copy, in
         * iommu.c, called from both: §M24's rule, and the case it was written
         * for is exactly this one — a diagnostic that answers on two arches out
         * of three teaches the third's users that the question has no answer. */
        else if (s_eq(cmd, "iommu"))   iommu_cmd(args);
        /* §M67 — the loader's own commands, same implementation as x86. */
        else if (s_eq(cmd, "lsmod"))   modload_list();
        else if (s_eq(cmd, "insmod"))  modload_cmd_insmod(args);
        else if (s_eq(cmd, "rmmod"))   modload_cmd_rmmod(args);
        else if (s_eq(cmd, "ksyms"))   ksym_list(args);
        else if (s_eq(cmd, "play"))    audio_cmd_play(args);
        else if (s_eq(cmd, "volume"))  audio_cmd_volume(args);
        else if (s_eq(cmd, "rec"))     audio_cmd_rec(args);
        else if (s_eq(cmd, "free"))   cmd_meminfo();
        else if (s_eq(cmd, "uptime")) kprintf("up %u ms\n", (unsigned)timer_ticks_ms());
        else if (s_eq(cmd, "ktime"))  cmd_ktime();
        else if (s_eq(cmd, "ktimer")) cmd_ktimer();
        else if (s_eq(cmd, "killstorm")) cmd_killstorm(args);
        else if (s_eq(cmd, "rqcheck"))   cmd_rqcheck();
        else if (s_eq(cmd, "schedstorm")) cmd_schedstorm(args);
        else if (s_eq(cmd, "timerfdtest")) cmd_timerfdtest(args);
        else if (s_eq(cmd, "alarmtest"))   cmd_alarmtest(args);
        else if (s_eq(cmd, "ps"))     cmd_ps();
        else if (s_eq(cmd, "ls"))     cmd_ls(args);
        else if (s_eq(cmd, "cat"))    cmd_cat(args);
        else if (s_eq(cmd, "mkdir"))  cmd_mkdir(args);
        else if (s_eq(cmd, "write"))  cmd_write(args);
        else if (s_eq(cmd, "rm"))     cmd_rm(args);
        else if (s_eq(cmd, "blk"))    cmd_blk(args);
        else if (s_eq(cmd, "usertest")) cmd_usertest();
        else if (s_eq(cmd, "forktest")) cmd_forktest();
        else if (s_eq(cmd, "pipetest")) cmd_pipetest();
        else if (s_eq(cmd, "sigtest"))  cmd_sigtest();
        else if (s_eq(cmd, "musltest")) cmd_musltest();
        else if (s_eq(cmd, "epollmusltest")) cmd_epollmusltest();
        else if (s_eq(cmd, "netmuslserv"))   cmd_netmuslserv();
        else if (s_eq(cmd, "pkgrun"))   cmd_pkgrun(args);
        else if (s_eq(cmd, "pkg"))      cmd_pkg(args);
        /* §M24 — the same implementations the x86 shell runs (net_cmds.c). */
        else if (s_eq(cmd, "lsnic"))    net_list();
        else if (s_eq(cmd, "ping"))     cmd_ping(args);
        else if (s_eq(cmd, "netstat"))  netcmd_netstat();
        else if (s_eq(cmd, "tcptest"))  netcmd_tcptest(args);
        else if (s_eq(cmd, "tcploss"))  netcmd_tcploss(args);
        else if (s_eq(cmd, "lo"))       netcmd_lo(args);
        else if (s_eq(cmd, "dhcp"))     netcmd_dhcp(args);
        /* §M60 — wallpaper.c hosts the command so ARM gets it too: the
         * framebuffer differs MOST on this arch (virtio-gpu scanout, not a
         * linear LFB), which is the last place a background test should be
         * missing. */
        else if (s_eq(cmd, "wallpaper")) wallpaper_cmd(args);
        else if (s_eq(cmd, "shortcut"))  shortcut_cmd(args);
        else if (s_eq(cmd, "conf"))      settings_cmd(args);
        else if (s_eq(cmd, "clip"))      clipboard_cmd(args);
        else if (s_eq(cmd, "mode"))      display_cmd(args);
        else if (s_eq(cmd, "splash"))    splash_cmd(args);
        else if (s_eq(cmd, "hardlock")) watchdog_hardlock_test();
        /* §M63 stage 0 — config from ARM too.  These lived in shell.c only,
         * so this arch could create a persistent store and then had no command
         * able to write to it. */
        else if (s_eq(cmd, "getconf"))  config_cmd_getconf(args);
        else if (s_eq(cmd, "setconf"))  config_cmd_setconf(args);
        else if (s_eq(cmd, "saveconf")) config_cmd_saveconf();
        else if (s_eq(cmd, "config"))   config_dump();
        else if (s_eq(cmd, "clear"))  kprintf("\033[2J\033[H");
        else kprintf("unknown command '%s' (try 'help')\n", cmd);
    }
}
