/* =============================================================================
 * service.c — supervised-services supervisor (M29, lifecycle half).  See
 * service.h for the model.
 *
 * One supervisor task (a child of init) owns every service task.  It spawns
 * autostart services at boot, then loops on task_wait(-1) (Tier A) — blocking
 * with zero CPU until a service child exits — and applies the restart policy.
 * Because the supervisor is the parent AND claims each child's reap
 * (task_set_reap_owned), init's universal reaper leaves them for task_wait to
 * harvest, so no service-exit is missed and the exit code is authoritative.
 *
 * A hand-issued `service stop` sets a `stopping` flag before killing, so the
 * subsequent exit is recognised as deliberate and NOT restarted.  A service
 * that crash-loops (dies quickly after start) is backed off before the
 * restart so it can't spin a core.
 * ============================================================================= */

#include "service.h"
#include "task.h"
#include "lock.h"
#include "printf.h"
#include "klog.h"
#include "timer.h"
#include "config.h"
#include "procfs.h"
#include "waitq.h"
#include <stddef.h>
#include <stdint.h>

#define SVC_MAX          64        /* runtime slots — >= service_count()      */
#define SVC_RAPID_MS     1000      /* died within this of start = crash-loop  */
#define SVC_BACKOFF_MS   1000      /* backoff before a crash-loop restart     */

enum svc_state { SVC_STOPPED, SVC_RUNNING, SVC_DONE, SVC_FAILED };

struct svc_rt {
    int            pid;            /* current task pid, or -1 if not running   */
    enum svc_state state;
    int            restarts;      /* how many times we've (re)started it      */
    int            stopping;      /* a hand-issued stop is in flight          */
    int            last_code;     /* last exit code observed                  */
    uint64_t       last_start_ms;
};

static struct svc_rt rt[SVC_MAX];
static spinlock_t    svc_lock = SPINLOCK_INIT;

static int          g_sup_pid = 0;             /* supervisor task pid          */
static struct waitq sup_wq    = WAITQ_INIT;    /* idle-wait when no children   */
static volatile int g_sup_kick = 0;            /* "a start happened" flag      */

/* ------------------------------------------------------------------- */
/* Helpers.                                                             */
/* ------------------------------------------------------------------- */

static int streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int find_by_name(const char* name) {
    int n = service_count();
    for (int i = 0; i < n && i < SVC_MAX; i++)
        if (streq(service_at(i)->name, name)) return i;
    return -1;
}

static int find_by_pid(int pid) {
    int n = service_count();
    for (int i = 0; i < n && i < SVC_MAX; i++)
        if (rt[i].pid == pid) return i;
    return -1;
}

static const char* state_name(enum svc_state s) {
    switch (s) {
        case SVC_STOPPED: return "stopped";
        case SVC_RUNNING: return "running";
        case SVC_DONE:    return "done";
        case SVC_FAILED:  return "failed";
    }
    return "?";
}

/* Config gate: `service.<name>.disabled = 1` keeps a service down (even if it
 * autostarts).  Build the key on a small stack buffer. */
static int svc_disabled(const char* name) {
    char key[64];
    const char* pre = "service.";
    const char* suf = ".disabled";
    int k = 0;
    for (const char* p = pre; *p && k < 63; p++) key[k++] = *p;
    for (const char* p = name; *p && k < 63; p++) key[k++] = *p;
    for (const char* p = suf; *p && k < 63; p++) key[k++] = *p;
    key[k] = 0;
    const char* v = config_get(key, "0");
    return v && v[0] == '1';
}

static void kick_supervisor(void) {
    uint32_t f = waitq_lock(&sup_wq);
    g_sup_kick = 1;
    waitq_wake_all(&sup_wq);
    waitq_unlock(&sup_wq, f);
}

/* Spawn service `idx` as a child of the supervisor, claim its reap, record the
 * pid.  Runs either on the supervisor task (autostart / restart) or on a
 * caller task (`service start`).  task_spawn_under itself takes master_lock;
 * we set the runtime fields under svc_lock afterwards. */
static void svc_spawn(int idx) {
    const struct service* s = service_at(idx);
    int parent = (g_sup_pid > 0) ? g_sup_pid : -1;
    struct task* t = task_spawn_under(s->name, s->entry, parent);

    uint32_t f = spin_lock_irqsave(&svc_lock);
    if (t) {
        task_set_reap_owned(t, 1);            /* supervisor reaps via task_wait */
        rt[idx].pid           = t->pid;
        rt[idx].state         = SVC_RUNNING;
        rt[idx].last_start_ms = timer_ticks_ms();
    } else {
        rt[idx].pid   = -1;
        rt[idx].state = SVC_FAILED;
    }
    spin_unlock_irqrestore(&svc_lock, f);

    if (t) klog(KLOG_INFO, "svc", "started '%s' (pid %d)\n", s->name, t->pid);
    else   klog(KLOG_ERR,  "svc", "FAILED to spawn '%s'\n", s->name);
}

/* Any service children currently tracked as running?  (Determines whether the
 * supervisor blocks in task_wait or idles on sup_wq.) */
static int have_children(void) {
    int n = service_count();
    uint32_t f = spin_lock_irqsave(&svc_lock);
    int any = 0;
    for (int i = 0; i < n && i < SVC_MAX; i++) if (rt[i].pid >= 0) { any = 1; break; }
    spin_unlock_irqrestore(&svc_lock, f);
    return any;
}

/* ------------------------------------------------------------------- */
/* Exit handling + restart policy.                                      */
/* ------------------------------------------------------------------- */

static void handle_exit(int pid, int code) {
    int idx = find_by_pid(pid);
    if (idx < 0) return;                        /* not a tracked service       */
    const struct service* s = service_at(idx);

    uint32_t f = spin_lock_irqsave(&svc_lock);
    rt[idx].pid       = -1;
    rt[idx].last_code = code;
    int stopping      = rt[idx].stopping;
    rt[idx].stopping  = 0;
    uint64_t last     = rt[idx].last_start_ms;
    spin_unlock_irqrestore(&svc_lock, f);

    int want_restart = 0;
    if (stopping)                                            want_restart = 0;
    else if (s->restart == SVC_RESTART_ALWAYS)               want_restart = 1;
    else if (s->restart == SVC_RESTART_ON_FAILURE && code)   want_restart = 1;

    if (!want_restart) {
        uint32_t f2 = spin_lock_irqsave(&svc_lock);
        rt[idx].state = stopping ? SVC_STOPPED : (code ? SVC_FAILED : SVC_DONE);
        spin_unlock_irqrestore(&svc_lock, f2);
        klog(KLOG_INFO, "svc", "'%s' exited (code %d)%s\n", s->name, code,
             stopping ? " — stopped" : " — no restart");
        return;
    }

    /* Crash-loop backoff: if it died quickly after starting, wait before the
     * restart so a persistently-broken service can't spin the CPU. */
    if (timer_ticks_ms() - last < SVC_RAPID_MS) {
        klog(KLOG_WARN, "svc", "'%s' crash-loop (code %d) — backoff %ums\n",
             s->name, code, (unsigned)SVC_BACKOFF_MS);
        task_msleep(SVC_BACKOFF_MS);
    }

    uint32_t f3 = spin_lock_irqsave(&svc_lock);
    rt[idx].restarts++;
    int nr = rt[idx].restarts;
    spin_unlock_irqrestore(&svc_lock, f3);
    klog(KLOG_NOTICE, "svc", "restarting '%s' (restart #%d, prev code %d)\n",
         s->name, nr, code);
    svc_spawn(idx);
}

/* ------------------------------------------------------------------- */
/* Supervisor task.                                                     */
/* ------------------------------------------------------------------- */

static void supervisor_entry(void) {
    klog(KLOG_INFO, "svc", "supervisor up as pid %d\n",
         task_current() ? task_current()->pid : -1);

    /* Autostart every enabled service. */
    int n = service_count();
    for (int i = 0; i < n && i < SVC_MAX; i++) {
        const struct service* s = service_at(i);
        if (s->autostart && !svc_disabled(s->name)) svc_spawn(i);
        else if (svc_disabled(s->name))
            klog(KLOG_INFO, "svc", "'%s' disabled by config\n", s->name);
    }

    for (;;) {
        if (have_children()) {
            int code = 0;
            int pid = task_wait(-1, &code);     /* blocks until a child exits  */
            if (pid >= 0) handle_exit(pid, code);
        } else {
            /* No children — park until a `service start` kicks us. */
            uint32_t f = waitq_lock(&sup_wq);
            if (!g_sup_kick) waitq_block(&sup_wq);
            g_sup_kick = 0;
            waitq_unlock(&sup_wq, f);
        }
    }
}

/* ------------------------------------------------------------------- */
/* Control surface.                                                     */
/* ------------------------------------------------------------------- */

int service_start(const char* name) {
    int idx = find_by_name(name);
    if (idx < 0) return -1;
    uint32_t f = spin_lock_irqsave(&svc_lock);
    if (rt[idx].pid >= 0) { spin_unlock_irqrestore(&svc_lock, f); return -2; } /* up */
    rt[idx].stopping = 0;
    spin_unlock_irqrestore(&svc_lock, f);
    svc_spawn(idx);
    kick_supervisor();
    return 0;
}

int service_stop(const char* name) {
    int idx = find_by_name(name);
    if (idx < 0) return -1;
    uint32_t f = spin_lock_irqsave(&svc_lock);
    int pid = rt[idx].pid;
    if (pid < 0) { spin_unlock_irqrestore(&svc_lock, f); return -2; }  /* not up */
    rt[idx].stopping = 1;                          /* deliberate → don't restart */
    spin_unlock_irqrestore(&svc_lock, f);
    task_kill(pid);                                /* lands at its next yield     */
    kick_supervisor();
    return 0;
}

int service_restart(const char* name) {
    int idx = find_by_name(name);
    if (idx < 0) return -1;
    int was_up;
    uint32_t f = spin_lock_irqsave(&svc_lock);
    was_up = rt[idx].pid >= 0;
    if (was_up) rt[idx].stopping = 1;   /* stop won't auto-restart; we respawn */
    int pid = rt[idx].pid;
    spin_unlock_irqrestore(&svc_lock, f);
    if (was_up) {
        task_kill(pid);
        /* Let the supervisor observe the exit + clear pid, then start fresh.
         * Poll briefly (bounded) so restart is synchronous for the caller. */
        for (int i = 0; i < 200; i++) {
            uint32_t f2 = spin_lock_irqsave(&svc_lock);
            int gone = rt[idx].pid < 0;
            spin_unlock_irqrestore(&svc_lock, f2);
            if (gone) break;
            task_msleep(5);
        }
    }
    return service_start(name);
}

/* ------------------------------------------------------------------- */
/* Diagnostics.                                                         */
/* ------------------------------------------------------------------- */

void service_list(void) {
    int n = service_count();
    kprintf("NAME              STATE     PID   RESTARTS  AUTO  POLICY\n");
    for (int i = 0; i < n && i < SVC_MAX; i++) {
        const struct service* s = service_at(i);
        const char* pol = s->restart == SVC_RESTART_ALWAYS ? "always" :
                          s->restart == SVC_RESTART_ON_FAILURE ? "on-fail" : "no";
        kprintf("%s   %s   %d   %d   %s   %s\n",
                s->name, state_name(rt[i].state), rt[i].pid, rt[i].restarts,
                s->autostart ? "yes" : "no", pol);
    }
}

int service_status(const char* name) {
    int idx = find_by_name(name);
    if (idx < 0) { kprintf("service: no such service '%s'\n", name); return -1; }
    const struct service* s = service_at(idx);
    kprintf("service '%s': state=%s pid=%d restarts=%d last-code=%d autostart=%s\n",
            s->name, state_name(rt[idx].state), rt[idx].pid, rt[idx].restarts,
            rt[idx].last_code, s->autostart ? "yes" : "no");
    return 0;
}

/* ------------------------------------------------------------------- */
/* /proc/services + boot wiring.                                        */
/* ------------------------------------------------------------------- */

static void gen_services(struct procfs_writer* w) {
    pw_puts(w, "# name  state  pid  restarts  autostart  policy\n");
    int n = service_count();
    for (int i = 0; i < n && i < SVC_MAX; i++) {
        const struct service* s = service_at(i);
        const char* pol = s->restart == SVC_RESTART_ALWAYS ? "always" :
                          s->restart == SVC_RESTART_ON_FAILURE ? "on-failure" : "no";
        pw_puts(w, s->name);            pw_putc(w, ' ');
        pw_puts(w, state_name(rt[i].state)); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)(rt[i].pid < 0 ? 0 : rt[i].pid)); pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)rt[i].restarts); pw_putc(w, ' ');
        pw_puts(w, s->autostart ? "yes" : "no"); pw_putc(w, ' ');
        pw_puts(w, pol);
        pw_putc(w, '\n');
    }
}

static struct procfs_node nd_services = { .name = "services", .gen = gen_services };

void service_init(void) {
    for (int i = 0; i < SVC_MAX; i++) { rt[i].pid = -1; rt[i].state = SVC_STOPPED; }
    procfs_register(&nd_services);
}

void service_start_supervisor(void) {
    if (g_sup_pid) return;                       /* singleton */
    struct task* t = task_spawn_detached("svc-supervisor", supervisor_entry);
    if (!t) { klog(KLOG_ERR, "svc", "supervisor spawn FAILED\n"); return; }
    g_sup_pid = t->pid;
}
