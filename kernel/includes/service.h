/* =============================================================================
 * service.h — supervised services registry + supervisor (M29, lifecycle half).
 *
 * The "upward" answer to child death.  M27 propagates termination downward
 * (kill-tree) but deliberately does not let a child's death kill its parent;
 * the established convention for what happens up the tree is a *supervisor*
 * (Erlang/OTP, systemd, runit, s6): the parent is notified of a child's exit
 * and applies a restart policy.  M29 is exactly that — a systemd-lite.
 *
 * A service is a self-registered entry (same linker-section story as
 * DRIVER() / GUI_APP() / SHELL_PROVIDER() — no kernel_main edits): a name, a
 * task entry (a normal kernel-thread entry that runs until killed or done),
 * an autostart flag, and a restart policy.  The supervisor task:
 *   - at boot, starts every autostart service that config hasn't disabled;
 *   - blocks in task_wait() (Tier A) for any service child to exit;
 *   - on exit applies the policy (with a crash-loop backoff) — restart-always,
 *     restart-on-failure (code != 0), or never.
 *
 * A hand-issued `service stop` is distinguished from a crash (a `stopping`
 * flag) so a deliberate stop is never "restarted".  Config gate: a service is
 * disabled by `service.<name>.disabled = 1` in /etc, so a service can be kept
 * down across boots.
 *
 * The entry honours the kthread contract (M22.3): a long-running service polls
 * task_should_stop() and exits promptly so `service stop` / kill lands.
 * ============================================================================= */

#ifndef SERVICE_H
#define SERVICE_H

enum svc_restart {
    SVC_RESTART_NO,          /* run to completion; never restart              */
    SVC_RESTART_ON_FAILURE,  /* restart only if the exit code is non-zero     */
    SVC_RESTART_ALWAYS,      /* always restart (a daemon that should stay up) */
};

struct service {
    const char*      name;        /* unique id, e.g. "heartbeat"              */
    void           (*entry)(void);/* task entry — kthread contract (§M22.3)   */
    int              autostart;   /* start at boot unless config-disabled     */
    enum svc_restart restart;     /* what to do when it exits                 */
};

extern struct service __start_services[];
extern struct service __stop_services[];

/* Self-registration.  `_entryfn` doubles as the unique symbol suffix. */
#define SERVICE(_name, _entryfn, _autostart, _restart)                   \
    static const struct service                                          \
    __attribute__((used, section("services"), aligned(4)))               \
    __service_##_entryfn = {                                             \
        .name      = (_name),                                            \
        .entry     = (_entryfn),                                         \
        .autostart = (_autostart),                                       \
        .restart   = (_restart),                                         \
    }

/* Registry walk (boundary arithmetic in one place). */
static inline int service_count(void) {
    return (int)(__stop_services - __start_services);
}
static inline const struct service* service_at(int i) {
    if (i < 0 || i >= service_count()) return (const struct service*)0;
    return &__start_services[i];
}

/* Boot: build the runtime table + register /proc/services.  Safe to call
 * before procfs_init (the node queues). */
void service_init(void);

/* Boot: spawn the supervisor task (as a child of init) and autostart the
 * enabled services.  Call after task_start_init so the supervisor's parent
 * (init) already exists. */
void service_start_supervisor(void);

/* Control surface (the `service` shell command / future bus calls).  Each
 * returns 0 on success, negative on "no such service" / already-in-state. */
int  service_start  (const char* name);
int  service_stop   (const char* name);
int  service_restart(const char* name);

/* Diagnostics for `service list` / `service status <name>`. */
void service_list(void);
int  service_status(const char* name);

#endif
