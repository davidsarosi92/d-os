/* =============================================================================
 * shell.c — the interactive REPL for d-os.
 *
 * §M70 SPLIT THIS FILE.  It was 4467 lines carrying ~80 command bodies whose
 * only common property was that somebody had typed them, plus a 172-arm
 * dispatch chain and a hand-written `help` string that listed about a third of
 * what existed.  What is left here is the REPL and nothing else: read a line,
 * hand it to the registry, print the prompt again.
 *
 * WHERE THE COMMANDS WENT — and the rule for a new one: a command lives NEXT
 * TO THE CODE IT DRIVES and registers itself with SHELL_CMD() (see
 * shellcmd.h).  `wallpaper` is in gui/wallpaper.c, `play` in core/audio.c,
 * `drv` in core/driver.c.  The `kernel/core/cmd_*.c` files exist for the
 * commands whose subject is the kernel itself — files, tasks, memory, the
 * userland test battery — and therefore have no more specific home.
 *
 * WHAT THAT BUYS, concretely: `help` is GENERATED, so it cannot drift from
 * what exists; a verb is looked up exactly, so no arm can shadow another; and
 * the aarch64 serial REPL walks the same registry, so a command is no longer
 * on x86 only because shell.c was the cheapest place to write it.
 *
 * No external dependencies (no libc), so `streq`/`starts_with` are rolled here
 * — the shared copies for command files live in cmd_util.h.
 * =========================================================================== */

#include "shell.h"
#include "shellcmd.h"   /* §M70 — the command registry gets first refusal */
#include "console.h"
#include "kmalloc.h"
#include "printf.h"
void edu_test(void);
void edu_escape(uint64_t phys);
#include "vfs.h"
#include "config.h"
#include "task.h"
#include "pkg.h"
#include "vc.h"
#include "shell_provider.h"
#include "proc.h"

#define LINE_MAX        128             /* max accepted bytes per command line */
#define DEFAULT_PROMPT  "d-os> "        /* fallback when config is unavailable */

/* Exact string equality.  Walks until either string diverges or the shorter
 * string ends; the terminator check at the end catches the case where one
 * string is a strict prefix of the other (those are NOT equal). */
static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
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

/* §M70 — `cmd_help` is GONE.  It was a hand-written string listing ~60 of the
 * 172 commands that existed, kept in step with the dispatch chain by memory
 * alone; over a hundred commands were reachable and documented nowhere.  Help
 * is generated from the SHELL_CMD registry now (shellcmd.c), so a registered
 * command is a listed command and there is no second place to forget. */

/* -------------------------------------------------------------------- */
/* Filesystem commands.  Args are passed as raw strings; arg parsing    */
/* is intentionally tiny.                                                */
/* -------------------------------------------------------------------- */







/* -------------------------------------------------------------------- */
/* Task / scheduler demo.                                                */
/*                                                                      */
/* `ticker` is a kernel-mode task that prints `[ticker N]` every second  */
/* (busy-waited via timer ticks), then yields.  Each spawn adds a new   */
/* one with its own pid so `ps` shows multiple entries.                  */
/* -------------------------------------------------------------------- */



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











/* -------------------------------------------------------------------- */
/* Memory — `slabinfo` and `buddyinfo` (M19).                            */
/* -------------------------------------------------------------------- */




/* -------------------------------------------------------------------- */
/* Block layer test — writes a recognizable pattern to sector 1 of      */
/* /dev/vda, reads it back, prints a verdict.  Sector 0 is left alone   */
/* so we don't trample a future partition table or MBR.                  */
/* -------------------------------------------------------------------- */

/* ----------------------- §M24.1 network commands -------------------------- */




/* §M39 — the userland musl+mbedTLS `wget` (URL + optional outfile from argv).
 * When embedded it handles BOTH http:// and https:// (real TLS + CA verify); the
 * kernel HTTP-only path below is kept as a fallback for builds without musl. */




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






/* -------------------------------------------------------------------- */
/* Block cache test — exercises bcache_get/release/mark_dirty/sync on   */
/* sector 2 of /dev/vda.  Sector 0 belongs to a future MBR/boot sector  */
/* and sector 1 is owned by blktest; using a separate sector avoids    */
/* cross-test interference.                                             */
/* -------------------------------------------------------------------- */


/* -------------------------------------------------------------------- */
/* Ring-3 demo.                                                         */
/*                                                                      */
/* Allocates two physical frames, USER-maps them into the kernel's      */
/* address space at 0x40000000 (code+data) and 0x40001000 (stack),      */
/* hand-codes a small i386 program that calls SYS_PRINT followed by     */
/* SYS_EXIT, drops the CPU to ring 3 at the entry point, and returns    */
/* via the SYS_EXIT teleport in usermode.s when the program is done.    */
/* -------------------------------------------------------------------- */









/* ---- Tier A: wait-queue / task_wait / blocking read self-test ------------- */




/* ---- M29: services + service bus -------------------------------------- */





/* M25 stage 7 — run the in-tree-libc compiled-C user program embedded as a
 * blob (user/hello.c → static ELF → objcopy).  Weak symbols so the command
 * still links on arches that don't embed the blob yet (i386 is the reference
 * port today). */
extern const unsigned char _binary_user_hello_elf_start[]    __attribute__((weak));
extern const unsigned char _binary_user_hello_elf_end[]      __attribute__((weak));



/* Tier B — `procspawn`: launch TWO copies of the spin demo as independent,
 * preemptible user processes; their interleaved output proves concurrent
 * ring-3 tasks time-sliced by the scheduler, each exiting on its own SYS_EXIT. */



/* M34 slice A — `runargs [a b c ...]`: exec the args test program with an
 * argv built by the kernel; it prints argc + each argv from ring 3. */
extern const unsigned char _binary_user_args_elf_start[] __attribute__((weak));
extern const unsigned char _binary_user_args_elf_end[]   __attribute__((weak));


/* M34 slice B — `forktest`: exec a user program that fork()s, the child exits
 * with a code, and the parent waitpid()s for it. */


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
    /* §M32 stage 6 — THIS IS A PROGRAM, so it carries the execute bit.
     *
     * The default for a new file is 0644, deliberately: if everything were
     * created executable the bit would mean "is a file" and the loader's check
     * would be theatre.  So the places that install a PROGRAM say so, and
     * there are exactly three — here, the package store, and a user's own
     * chmod. */
    vfs_chmod(path, 0755);
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


/* M34 slice D — `pipetest`: pipe()+dup2()+fork() from ring 3. */


/* M34 slice E — `sigtest`: signal()+raise()+handler from ring 3. */


/* M24 socket API — `dnstest`: resolve a hostname over a UDP socket from ring 3. */


/* M24 socket API — `httptest`: DNS + TCP-socket HTTP GET from ring 3. */


/* M35 — `threadtest`: threads + a futex mutex from ring 3. */


/* M35 — `tlstest`: thread-local storage via %gs from ring 3. */


/* M36 — `posixtest`: broader POSIX syscalls (uname/stat/getdents/clock) from ring 3. */


/* §M65 — `uidemo`: the widget toolkit driven from RING 3.  It builds a label,
 * a checkbox, a radio group and a slider by sending a DESCRIPTION over the
 * display bridge, and prints the events they raise — the proof that the
 * toolkit's data-shaped API buys what it was designed for. */


/* §M59 — `redirtest`: does STDOUT REDIRECTION work from ring 3?  It exists
 * because a bug reported as "the clipboard will not take a ring-3 write" was
 * really `dup2(fd, 1)` being refused — fds 0/1/2 were not table entries, so no
 * shell could redirect anything, and the failure was silent (the bytes went to
 * the terminal and the exit status said success). */


/* §M33 Tier 1 — `drvtest`: prove a ring-3 process gets exactly its granted
 * ports, and faults on anything else.
 *
 * The process is ATTACHED to the ps2_mouse manifest before it runs and detached
 * after: without that it holds no manifest and every resource syscall refuses,
 * which is the default and is what every other ring-3 program sees. */


/* M36 — `linuxtest`: run a Linux-ABI program under the Linux personality
 * (task->linux_abi), routing its syscalls through the linux_abi translator. */


/* M36 stage 2 — `musltest`: run a REAL, unmodified musl-linked ELF under the
 * Linux personality.  Embedded only when `make musl` produced the binary. */

/* §M46/§M47.1 — `wedgewin`: open a GUI window with a client that then FREEZES.
 * The point is the title-bar X: a frozen client can never observe the close
 * event, so the window must still go away through the compositor's force-kill
 * fallback (after gui.close_grace_ms).  Spawned as an independent Linux-ABI
 * task, never as an excursion — a wedged excursion would take this shell with
 * it, which is exactly the failure mode M46 exists to prevent. */


/* §M40 — `pthreadtest`: REAL musl pthreads (clone + futex join), the threading
 * every toolkit and Mesa is built on. */





/* §M37 — `musldyntest`: run a DYNAMICALLY-linked musl ELF (PT_INTERP set) under
 * the Linux personality.  proc_exec_elf → load_program maps the PIE main + the
 * interpreter (/lib/ld-musl-i386.so.1, provisioned by pkg_init) and starts in
 * ld.so, which relocates + resolves symbols in ring 3 before calling main.
 * If this prints, the whole dynamic-linking path works. */



/* §M38 — `cpptest`: run a DYNAMICALLY-linked C++ program (libstdc++ + libgcc_s)
 * that throws + catches an exception across a .so boundary (libcpplib.so) — the
 * M38 definition-of-done.  Embedded only when the musl C++ toolchain was built
 * (make musl-cross-i686) + a rebuild. */


/* §M39 stage 2 — `crypttest`: run the Mbed TLS crypto self-test (SHA-256 KAT +
 * AES-256-GCM round-trip) in ring 3 under the Linux personality.  Embedded only
 * when `make mbedtls` + a rebuild produced it. */


/* §M39 stage 3 — `ssltest`: an in-memory TLS handshake (client+server) via
 * mbedTLS, seeded from our CSPRNG, with a real (trusted self-signed) cert. */


/* §M39 stage 3b — `httpstest`: REAL HTTPS from an unmodified musl binary.
 * DNS + TCP :443 + a full mbedTLS handshake over the live socket, verified
 * against the provisioned CA bundle (/etc/ssl/cert.pem), then an HTTP GET over
 * TLS.  Needs QEMU user networking. */


/* §M39 stage 3b — `netmusl`: ring-3 networking from an UNMODIFIED musl binary.
 * DNS-resolves example.com over a UDP socket then fetches "/" over TCP — the
 * whole BSD-sockets surface driven through musl's socketcall path (translated
 * by linux_abi.c onto the M24 stack).  Needs QEMU user networking. */


/* §M37 stage 5 — `solibtest`: run a program that links against a SEPARATE
 * shared library (libgreet.so, at /lib).  Exercises ld.so's real work: locate
 * a genuinely separate .so via the search path and resolve symbols across
 * three objects (main → libgreet → libc). */




/* §M37 stage 7 — `dlopentest`: runtime dlopen/dlsym/dlclose of /lib/libgreet.so. */


/* §M26 — `wayclient`: a REAL ring-3 Wayland client.  Set up a usock_pair, hand
 * one end to a spawned server task (wl_conn_serve) and install the other as the
 * shell's fd 3, then exec user/wlclient.c — which speaks the Wayland wire
 * protocol over fd 3 from user space.  The client blocks on read(3), the server
 * task runs concurrently and answers; on exit fd_close_all() closes fd 3 and the
 * server sees EOF + tears down. */


/* §M40 — `wayupstream`: the same fd-3 handshake, but the client is a musl binary
 * linked against the REAL libwayland-client.  Two differences from the native
 * clients above: it runs under the Linux personality (it is a musl program), and
 * it needs WAYLAND_SOCKET=3 in its environment — that is upstream libwayland's
 * documented "already-connected fd" mechanism, the same one a real compositor
 * uses to launch its own clients, so no named UNIX socket is required. */

/* §M40 — `simpleshm [win]`: run weston-simple-shm, an UNMODIFIED upstream
 * Wayland application (weston's own reference client, compiled straight out of
 * its tree).  Identical plumbing to wayupstream — the point is precisely that
 * nothing about it is special-cased. */



/* §M40 — `egltri [win]`: the EGL + GLES2 triangle, the milestone's DoD.
 * Dynamically linked (Mesa is shared objects), so it needs LIBGL_DRIVERS_PATH
 * pointing at the provisioned rasteriser in addition to WAYLAND_SOCKET. */






/* §M43 — `tcc <args>`: the on-device C compiler.  Runs the embedded tcc binary
 * (a musl ELF, under the Linux personality) with the shell args as argv, e.g.
 * `tcc /tmp/hello.c -o /tmp/hello`.  tcc reads the source + headers (/usr/
 * include, /usr/lib/tcc/include) and writes a runnable ELF, all on the VFS. */









/* -------------------------------------------------------------------- */
/* Configuration commands.                                              */
/* -------------------------------------------------------------------- */

/* §M63 stage 0 — these now delegate to config.c so the ARM serial shell runs
 * the SAME implementation (it had none at all before). */











/* Dispatch a parsed line.  Each branch handles its own echo / newline —
 * there is no implicit trailing newline so commands like `echo` with no
 * argument can control output precisely.
 *
 * `my_vc` is the VC this shell instance owns — needed by pane commands
 * so a split knows which leaf to operate on. */
/* Dispatch one line.
 *
 * §M70 — THE WHOLE OF IT.  This used to be 172 ordered `if` arms testing
 * exact strings and prefixes, and the ordering was load-bearing in a way
 * nothing checked: §4.67.1's regression was a `gui ` prefix arm placed above
 * the exact `gui stats`, after which `gui stats` answered "already running".
 *
 * The registry answers by VERB and the verb owns its whole argument tail, so
 * there is nothing left to order.  The fallback chain that carried the
 * migration is gone in the same change as its last arm — a fallback kept past
 * its usefulness is the §M56.1 defect: a stale path that silently answers
 * instead of the real one. */
static void dispatch(struct vc* my_vc, const char* line) {
    if (line[0] == '\0') return;                       /* empty line → no-op */

    /* The VC is set BEFORE dispatch, not passed as an argument: three of ~170
     * commands address the terminal rather than the machine, and it is NULL on
     * the ARM serial REPL.  See shellcmd.h. */
    shell_set_current_vc(my_vc);

    if (shell_cmd_dispatch(line)) return;

    /* Make the failure visible rather than mysterious — and name the way out,
     * because a shell that only says "no" leaves the user with nowhere to look. */
    console_write("unknown: ");
    console_write(line);
    console_write("  (try `help`)\n");
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
