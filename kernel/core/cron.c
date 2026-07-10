/* =============================================================================
 * cron.c — time-based task scheduling (M30).  See cron.h.
 *
 * cron is itself an M29 service (registered at the bottom of this file):
 * autostart + restart=always, so it appears in `service list`, is supervised,
 * and comes back if it dies.  Its entry loops on task_msleep at a fixed tick
 * granularity, and each tick spawns every job whose interval has elapsed as
 * one of its children (init reaps them), logging the run.
 *
 * Job intervals come from three layers, lowest priority first: the CRON_JOB
 * registered default, an `/etc/crontab` line, then `cron.<name>.disabled` /
 * `cron.<name>.every_ms` config keys.  `/etc/crontab` is optional — absent, the
 * registered defaults fire, so a job works out of the box.
 * ============================================================================= */

#include "cron.h"
#include "service.h"
#include "task.h"
#include "klog.h"
#include "printf.h"
#include "timer.h"
#include "config.h"
#include "vfs.h"
#include "procfs.h"
#include <stddef.h>
#include <stdint.h>

#define CRON_MAX      32       /* runtime slots — >= cron_job_count()     */
#define CRON_TICK_MS  500      /* scheduler granularity                   */

struct cron_rt {
    uint32_t interval_ms;
    uint64_t next_due;
    int      disabled;
    uint64_t runs;
};

static struct cron_rt rt[CRON_MAX];
static int            g_cron_pid = 0;

/* ------------------------------------------------------------------- */
/* Small string helpers (freestanding).                                 */
/* ------------------------------------------------------------------- */

static int cstreq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int find_job(const char* name) {
    int n = cron_job_count();
    for (int i = 0; i < n && i < CRON_MAX; i++)
        if (cstreq(cron_job_at(i)->name, name)) return i;
    return -1;
}

/* Config gate + per-job interval override (cron.<name>.disabled /
 * cron.<name>.every_ms).  Builds the key on a stack buffer. */
static void build_key(char* out, int cap, const char* name, const char* suf) {
    int k = 0;
    const char* pre = "cron.";
    for (const char* p = pre;  *p && k < cap - 1; p++) out[k++] = *p;
    for (const char* p = name; *p && k < cap - 1; p++) out[k++] = *p;
    for (const char* p = suf;  *p && k < cap - 1; p++) out[k++] = *p;
    out[k] = 0;
}
static uint32_t parse_u32(const char* s) {
    uint32_t v = 0;
    while (s && *s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    return v;
}

/* ------------------------------------------------------------------- */
/* /etc/crontab parsing.                                                */
/* ------------------------------------------------------------------- */

/* One line: `every <N> <s|m|h> <jobname>` (or `# comment` / blank).  Sets the
 * named job's interval.  Tokenised in place on a scratch copy. */
static void crontab_apply_line(char* line) {
    /* tokenise on spaces/tabs */
    char* tok[4]; int nt = 0;
    char* p = line;
    while (*p && nt < 4) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        tok[nt++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (nt < 4) return;
    if (!cstreq(tok[0], "every")) return;
    uint32_t n = parse_u32(tok[1]);
    uint32_t mult = 1000;                       /* default: seconds */
    if      (tok[2][0] == 'm') mult = 60u * 1000u;
    else if (tok[2][0] == 'h') mult = 3600u * 1000u;
    int idx = find_job(tok[3]);
    if (idx >= 0 && n > 0) rt[idx].interval_ms = n * mult;
}

static void crontab_parse(void) {
    struct file* f = vfs_open("/etc/crontab", VFS_RDONLY);
    if (!f) return;                              /* optional file */
    static char buf[1024];
    ssize_t got = vfs_read(f, buf, sizeof buf - 1);
    vfs_close(f);
    if (got <= 0) return;
    buf[got] = 0;

    /* Split into lines, apply each. */
    int start = 0;
    for (int i = 0; i <= (int)got; i++) {
        if (buf[i] == '\n' || buf[i] == 0) {
            buf[i] = 0;
            if (i > start) crontab_apply_line(&buf[start]);
            start = i + 1;
        }
    }
}

/* ------------------------------------------------------------------- */
/* (Re)load the schedule.                                               */
/* ------------------------------------------------------------------- */

static void cron_build_schedule(void) {
    int n = cron_job_count();
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < n && i < CRON_MAX; i++) {
        const struct cron_job* j = cron_job_at(i);
        uint32_t iv = j->default_every_ms ? j->default_every_ms : 1000;

        char key[64];
        build_key(key, sizeof key, j->name, ".every_ms");
        const char* ov = config_get(key, "");
        if (ov && ov[0]) { uint32_t v = parse_u32(ov); if (v) iv = v; }

        rt[i].interval_ms = iv;
        build_key(key, sizeof key, j->name, ".disabled");
        const char* dis = config_get(key, "0");
        rt[i].disabled = (dis && dis[0] == '1');
    }
    crontab_parse();                             /* /etc/crontab overrides intervals */

    /* Arm next-due for all (preserve run counts). */
    for (int i = 0; i < n && i < CRON_MAX; i++)
        rt[i].next_due = now + rt[i].interval_ms;
}

int cron_reload(void) {
    cron_build_schedule();
    klog(KLOG_INFO, "cron", "reloaded (%d job(s))\n", cron_job_count());
    return 0;
}

/* ------------------------------------------------------------------- */
/* Firing a job.                                                        */
/* ------------------------------------------------------------------- */

/* Spawn the job as a child of cron (init reaps it — not reap_owned — and logs
 * the reap; cron itself never blocks waiting on it). */
static void cron_fire(int i) {
    const struct cron_job* j = cron_job_at(i);
    klog(KLOG_INFO, "cron", "run '%s'\n", j->name);
    task_spawn_under(j->name, j->fn, g_cron_pid > 0 ? g_cron_pid : -1);
    rt[i].runs++;
}

/* ------------------------------------------------------------------- */
/* The cron service task.                                               */
/* ------------------------------------------------------------------- */

static void cron_service_entry(void) {
    g_cron_pid = task_current() ? task_current()->pid : 0;
    cron_build_schedule();
    klog(KLOG_INFO, "cron", "up as pid %d (%d job(s), tick %ums)\n",
         g_cron_pid, cron_job_count(), (unsigned)CRON_TICK_MS);

    int n = cron_job_count();
    for (;;) {
        if (task_should_stop()) task_exit();
        uint64_t now = timer_ticks_ms();
        for (int i = 0; i < n && i < CRON_MAX; i++) {
            if (rt[i].disabled) continue;
            if (now >= rt[i].next_due) {
                cron_fire(i);
                rt[i].next_due = now + rt[i].interval_ms;   /* no backfill */
            }
        }
        task_msleep(CRON_TICK_MS);
    }
}
SERVICE("cron", cron_service_entry, 1, SVC_RESTART_ALWAYS);

/* ------------------------------------------------------------------- */
/* Control surface + /proc/cron.                                        */
/* ------------------------------------------------------------------- */

void cron_list(void) {
    int n = cron_job_count();
    uint64_t now = timer_ticks_ms();
    kprintf("JOB            EVERY(ms)  NEXT(ms)  RUNS  STATE\n");
    for (int i = 0; i < n && i < CRON_MAX; i++) {
        const struct cron_job* j = cron_job_at(i);
        uint32_t due = (rt[i].next_due > now) ? (uint32_t)(rt[i].next_due - now) : 0;
        kprintf("%s   %u   %u   %u   %s\n",
                j->name, (unsigned)rt[i].interval_ms, (unsigned)due,
                (unsigned)rt[i].runs, rt[i].disabled ? "disabled" : "enabled");
    }
}

static void gen_cron(struct procfs_writer* w) {
    pw_puts(w, "# job  every_ms  next_ms  runs  state\n");
    int n = cron_job_count();
    uint64_t now = timer_ticks_ms();
    for (int i = 0; i < n && i < CRON_MAX; i++) {
        const struct cron_job* j = cron_job_at(i);
        uint32_t due = (rt[i].next_due > now) ? (uint32_t)(rt[i].next_due - now) : 0;
        pw_puts(w, j->name); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)rt[i].interval_ms); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)due); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)rt[i].runs); pw_putc(w, ' ');
        pw_puts(w, rt[i].disabled ? "disabled" : "enabled");
        pw_putc(w, '\n');
    }
}

static struct procfs_node nd_cron = { .name = "cron", .gen = gen_cron };

void cron_init(void) {
    procfs_register(&nd_cron);
}

/* ------------------------------------------------------------------- */
/* Demo job — proves scheduling: fires every 5s, logs to klog/dmesg.    */
/* ------------------------------------------------------------------- */

static void job_tick_log(void) {
    klog(KLOG_INFO, "cron.tick", "scheduled job fired\n");
}
CRON_JOB("tick-log", job_tick_log, 5000);
