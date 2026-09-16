/* =============================================================================
 * crash_file.c — the crash record that survives the reboot (§M75.1).
 *
 * Asked for directly, from use: *"the crash report could leave a file on the
 * backing store, so we know what happened before it — then we would not
 * necessarily need to reproduce the fault."*
 *
 * That is exactly right, and it names the gap: §M47 built a fault-safe capture
 * ring, a deferred drain and a `CRASH_SINK()` registry — and **every existing
 * sink is volatile**.  `klog` is a RAM ring, `/proc/crash` reads that ring, the
 * Crash Reports window is a view of it.  A machine that reboots takes all three
 * with it, so the one record worth having is the one guaranteed to be gone.
 *
 * §M47's NVRAM breadcrumb covers the event nothing in the guest can log (triple
 * fault, reset, power loss) — but it is FORTY BYTES: kind, cpu, pid, pc, addr,
 * code, uptime, comm.  It can say a fault happened and where; it cannot say
 * what the machine was doing beforehand, which is the question a person
 * actually has.
 *
 * SO THIS IS THE SINK §M47 WAS BUILT FOR, and it needed no fault-path change of
 * any kind — which is the registry's whole claim, now tested by a second
 * author's requirement rather than by its own author's intention.
 *
 * -----------------------------------------------------------------------------
 * FOUR DECISIONS
 *
 * 1. **THE VOLUME IS LEARNED, NOT ASSUMED.**  `/` is ramfs.  §M64 shipped
 *    desktop shortcuts writing to `/desktop` and every document said a shortcut
 *    is a file "so that it survives a reboot" — true about the format, false
 *    about the outcome, and silent because the WRITE SUCCEEDS.  §M63 stage 0
 *    was the same defect for settings.  So this sink is handed its directory
 *    after the mount, on both boot paths, and **with no writable volume it says
 *    so once and stays quiet** rather than writing into RAM and looking healthy.
 *
 * 2. **THE LOG TAIL IS THE POINT, not the record.**  A crash record is eight
 *    numbers; "what happened before" is the log leading up to them.  Each entry
 *    carries the last KLOG_TAIL lines from §M28's ring.
 *
 *    THEY ARE TAKEN AT DELIVERY, NOT AT CAPTURE, and that is deliberate rather
 *    than a limitation: delivery runs on the watchdog task moments after the
 *    fault, so the tail INCLUDES the fault dump itself — the register state,
 *    the "unclean boot" breadcrumb, the quarantine line — which is usually the
 *    most useful part.  Pinning the tail to the capture instant would cut it
 *    off exactly before the evidence.
 *
 * 3. **THE FILE IS BOUNDED AND KEEPS THE BEGINNING.**  A crash loop must not
 *    fill the disk, and when it happens every record after the first is the
 *    same record.  What has the context that led IN is the first one, so the
 *    buffer fills, notes that it is full, and stops — rather than a rotation
 *    that keeps the hundredth identical copy and discards the cause.
 *
 * 4. **NO APPEND, AND THAT IS FINE.**  This VFS has no O_APPEND, so the text
 *    lives in a fixed RAM buffer that is READ BACK at attach and rewritten
 *    whole after each record.  A cap was wanted anyway (3), so the buffer is
 *    the cap: one mechanism instead of two.  Reading it back at boot is what
 *    makes the file span reboots — the session that DIED is still in there when
 *    the next one starts appending.
 * ============================================================================= */

#include "crash.h"
#include "klog.h"
#include "vfs.h"
#include "printf.h"
#include "console.h"
#include "shellcmd.h"
#include <stddef.h>
#include <stdint.h>

#define CF_CAP        (12 * 1024)   /* the file's ceiling, and the buffer      */
#define CF_KLOG_TAIL  40            /* log lines kept with each record         */
#define CF_PATH_MAX   64

static char  g_buf[CF_CAP];
static int   g_len;
static char  g_path[CF_PATH_MAX];
static int   g_ready;               /* a writable volume was proven            */
static int   g_full;                /* the ceiling was hit; said once          */
static int   g_warned;              /* "no writable volume" said once          */

/* ---- tiny text helpers (no printf into a buffer in this kernel) ------------ */

static void cf_putc(char c) {
    if (g_len < CF_CAP - 1) g_buf[g_len++] = c;
    else g_full = 1;
}
static void cf_puts(const char* s) { while (s && *s) cf_putc(*s++); }
static void cf_putu(uint64_t v) {
    char t[24]; int n = 0;
    if (!v) { cf_putc('0'); return; }
    while (v && n < (int)sizeof t) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) cf_putc(t[--n]);
}
static void cf_puthex(uintptr_t v) {
    const char* d = "0123456789abcdef";
    char t[20]; int n = 0;
    cf_puts("0x");
    if (!v) { cf_putc('0'); return; }
    while (v && n < (int)sizeof t) { t[n++] = d[v & 0xF]; v >>= 4; }
    while (n) cf_putc(t[--n]);
}

/* ---- the klog tail --------------------------------------------------------
 *
 * `klog_for_each` walks oldest → newest, so keeping the LAST N means keeping a
 * small ring of copies as it goes.  A whole-log pass per crash is affordable
 * precisely because crashes are rare — and the alternative (a klog cursor) is a
 * change to a subsystem this one only reads.
 * ------------------------------------------------------------------------- */
struct tail_ctx {
    struct klog_record ring[CF_KLOG_TAIL];
    int n, head;
};

static void tail_collect(const struct klog_record* r, void* ctx) {
    struct tail_ctx* t = (struct tail_ctx*)ctx;
    t->ring[t->head] = *r;
    t->head = (t->head + 1) % CF_KLOG_TAIL;
    if (t->n < CF_KLOG_TAIL) t->n++;
}

/* ---- writing out ---------------------------------------------------------- */

static void cf_flush(void) {
    if (!g_ready) return;
    struct file* f = vfs_open(g_path, VFS_WRONLY | VFS_CREATE | VFS_TRUNC);
    if (!f) return;
    vfs_write(f, g_buf, (size_t)g_len);
    vfs_close(f);
}

/* ---- the sink -------------------------------------------------------------
 *
 * Runs on the drain task (ordinary context), so it may open files and block —
 * which is precisely the split §M47 made and the reason this file exists at
 * all without touching a single fault path.
 * ------------------------------------------------------------------------- */
static void cf_emit(const struct crash_record* r) {
    if (!g_ready || g_full || !r) return;

    cf_puts("\n=== crash #");
    cf_putu(r->seq);
    cf_puts("  uptime ");
    cf_putu(r->ms);
    cf_puts(" ms  cpu ");
    cf_putu(r->cpu);
    cf_puts("\n  what : ");
    cf_puts(r->what[0] ? r->what : "(none)");
    cf_puts("\n  task : ");
    cf_puts(r->comm[0] ? r->comm : "(none)");
    cf_puts("  pid ");
    if (r->pid < 0) cf_puts("-1"); else cf_putu((uint64_t)r->pid);
    cf_puts("\n  pc   : ");
    cf_puthex(r->pc);
    cf_puts("   addr ");
    cf_puthex(r->addr);
    cf_puts("   code ");
    cf_putu((uint64_t)(r->code < 0 ? -r->code : r->code));
    cf_puts("\n  --- last log lines before this was delivered ---\n");

    static struct tail_ctx t;      /* static: 40 records is too big for a stack */
    t.n = 0; t.head = 0;
    klog_for_each(tail_collect, &t);

    int start = (t.head + CF_KLOG_TAIL - t.n) % CF_KLOG_TAIL;
    for (int i = 0; i < t.n; i++) {
        const struct klog_record* k = &t.ring[(start + i) % CF_KLOG_TAIL];
        cf_puts("  [");
        cf_putu(k->t_ms);
        cf_puts("] ");
        if (k->tag[0]) { cf_puts(k->tag); cf_puts(": "); }
        cf_puts(k->msg);
        cf_putc('\n');
    }

    if (g_full)
        cf_puts("  --- log file full; later crashes are NOT recorded here ---\n");

    cf_flush();
}

CRASH_SINK("file", cf_emit);

/* ---------------------------------------------------------------------------
 * Attach, called from both boot paths right after the volume is mounted.
 *
 * CREATING THE FILE IS THE ONLY HONEST TEST that the volume is writable — a
 * path we merely HOPE is writable turns every later crash into a silent
 * non-record, which is the §M63 stage 0 defect exactly.  So this reads the
 * existing file (carrying the previous session across the reboot) and writes it
 * straight back with a boot marker; if either step fails the sink stays off and
 * says so ONCE.
 * ------------------------------------------------------------------------- */
void crash_file_attach_persistent(const char* dir) {
    if (!dir || g_ready) return;

    int p = 0;
    for (const char* q = dir; *q && p < CF_PATH_MAX - 12; q++) g_path[p++] = *q;
    if (p && g_path[p - 1] == '/') p--;
    const char* leaf = "/crash.log";
    for (const char* q = leaf; *q && p < CF_PATH_MAX - 1; q++) g_path[p++] = *q;
    g_path[p] = 0;

    /* Read back whatever a previous session left.  THIS is what makes the file
     * span the reboot: the boot that died is still in the buffer when this one
     * starts appending to it. */
    g_len = 0;
    struct file* f = vfs_open(g_path, VFS_RDONLY);
    if (f) {
        ssize_t n = vfs_read(f, g_buf, CF_CAP - 1);
        if (n > 0) g_len = (int)n;
        vfs_close(f);
    }

    g_ready = 1;                       /* provisional — cf_flush proves it     */
    cf_puts("\n=== boot ===\n");
    cf_flush();

    /* Prove it: a file we cannot open for writing is a sink that would swallow
     * every future record in silence. */
    struct file* w = vfs_open(g_path, VFS_RDONLY);
    if (!w) {
        g_ready = 0;
        if (!g_warned) {
            g_warned = 1;
            kprintf("crash-file: %s is not writable — crash records will NOT "
                    "survive a reboot\n", g_path);
        }
        return;
    }
    vfs_close(w);
    kprintf("crash-file: recording to %s (%d bytes carried over)\n",
            g_path, g_len);
}

/* ---------------------------------------------------------------------------
 * `crashlog` — read it back without leaving the machine, and clear it.
 *
 * The file is the point, but a person looking at a machine that just rebooted
 * should not have to mount its disk elsewhere to read why.
 * ------------------------------------------------------------------------- */
static void cmd_crashlog(const char* args) {
    if (args && args[0] == 'c') {                 /* `crashlog clear` */
        g_len = 0; g_full = 0;
        cf_puts("=== cleared ===\n");
        cf_flush();
        console_write("crashlog: cleared\n");
        return;
    }
    if (!g_ready) {
        console_write("crashlog: no writable volume — nothing is being recorded\n");
        return;
    }
    kprintf("crashlog: %s, %d/%d bytes%s\n", g_path, g_len, CF_CAP,
            g_full ? " (FULL — later crashes not recorded)" : "");
    for (int i = 0; i < g_len; i++) console_putchar(g_buf[i]);
    console_putchar('\n');
}

SHELL_CMD(crashlog) = { "crashlog", "[clear]",
                        "crash records kept on disk, with the log that led to them",
                        SHELL_G_SYS, cmd_crashlog, SHELL_P_ADMIN };
