/* =============================================================================
 * cron.h — time-based task scheduling (M30): the first real service.
 *
 * The capstone of the M27–M29 cluster.  cron is *itself* an M29 service
 * (autostart, restart=always) — it shows up in `service list`, is supervised,
 * and survives a restart — whose job is to spawn (and let init reap) child
 * tasks on a schedule.  Small precisely because M27 (parent/child + reaping),
 * M28 (klog), M29 (services) and Tier A (`task_msleep`) already exist.
 *
 * A cron JOB is a self-registered entry (same linker-section story as
 * DRIVER()/SERVICE()): a name, a task-entry function, and a default interval.
 * The cron service walks the registry each tick and, when a job is due, spawns
 * it as one of its children and logs the run.  Intervals can be overridden per
 * job from `/etc/crontab` (`every <N> <s|m|h> <name>` lines) or disabled via
 * the `cron.<name>.disabled` config key; absent config falls back to the
 * registered default, so a job fires out of the box.
 *
 * Missed-tick policy: run-once-on-catch-up (next-due is set to now+interval
 * after a fire), never backfill — a cron that was starved does not stampede.
 *
 * Out of scope (see PLAN §M30): wall-clock cron fields beyond intervals,
 * per-user crontabs, at/batch one-shots, persistence of last-run across reboot.
 * ============================================================================= */

#ifndef CRON_H
#define CRON_H

#include <stdint.h>

struct cron_job {
    const char* name;                 /* unique job id, e.g. "tick-log"      */
    void      (*fn)(void);            /* job body — a task entry, runs to end */
    uint32_t    default_every_ms;     /* default interval (config/crontab can
                                       * override); 0 → treated as 1000 ms    */
};

extern struct cron_job __start_cron_jobs[];
extern struct cron_job __stop_cron_jobs[];

/* Self-registration.  `_fn` doubles as the unique symbol suffix. */
#define CRON_JOB(_name, _fn, _every_ms)                                  \
    static const struct cron_job                                         \
    __attribute__((used, section("cron_jobs"), aligned(4)))              \
    __cron_job_##_fn = {                                                \
        .name = (_name), .fn = (_fn), .default_every_ms = (_every_ms),    \
    }

static inline int cron_job_count(void) {
    return (int)(__stop_cron_jobs - __start_cron_jobs);
}
static inline const struct cron_job* cron_job_at(int i) {
    if (i < 0 || i >= cron_job_count()) return (const struct cron_job*)0;
    return &__start_cron_jobs[i];
}

/* Boot: register /proc/cron.  (cron itself autostarts as an M29 service — no
 * explicit start call.)  Safe before procfs_init (queues). */
void cron_init(void);

/* Control surface (the `cron` / `crontab` shell command). */
void cron_list(void);                 /* `crontab -l` — jobs + schedule + next */
int  cron_reload(void);               /* re-read /etc/crontab + config          */

#endif
