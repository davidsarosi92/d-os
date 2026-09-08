/* =============================================================================
 * audit.c — the audit registry and the `audit` command (§M71).
 *
 * This file knows about no invariant.  It walks the section, runs what it
 * finds, and reports.  Every check lives next to the code that must maintain
 * it — the same rule that keeps controlpanel.c from naming a setting and
 * shell.c from naming a command.
 * =========================================================================== */

#include "audit.h"
#include "shellcmd.h"
#include "console.h"
#include "printf.h"
#include "config.h"
#include "cron.h"
#include "settings.h"

static int streq_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int audit_count(void) {
    return (int)(__stop_audits - __start_audits);
}

const struct audit_check* audit_at(int i) {
    if (i < 0 || i >= audit_count()) return 0;
    return &__start_audits[i];
}

/* Run one check and print its verdict line.
 *
 * THE THREE OUTCOMES ARE PRINTED DIFFERENTLY ON PURPOSE.  A pass, a skip and a
 * failure are three different facts, and a report that renders "checked and
 * clean" the same as "could not check" is the §M52 defect wearing a new hat:
 * it lets a check that never ran read as evidence. */
static int run_one(const struct audit_check* a, int verbose, int* skipped) {
    int rc = a->run ? a->run(verbose) : AUDIT_SKIP;

    if (rc == AUDIT_SKIP) {
        if (skipped) (*skipped)++;
        if (verbose) kprintf("  SKIP  %s — nothing to check on this machine\n", a->name);
        return 0;
    }
    if (rc == AUDIT_OK) {
        if (verbose) kprintf("  ok    %s\n", a->name);
        return 0;
    }
    /* A violation is printed whether or not anybody asked for detail: this is
     * the line the harness greps for, and the whole point of the mechanism is
     * that the next occurrence names itself. */
    kprintf("!! AUDIT FAIL: %s — %d violation(s)\n", a->name, rc);
    kprintf("   invariant: %s\n", a->what);
    return rc;
}

int audit_run_one(const char* name, int verbose) {
    int n = audit_count();
    for (int i = 0; i < n; i++) {
        if (__start_audits[i].name && streq_(__start_audits[i].name, name)) {
            int skipped = 0;
            return run_one(&__start_audits[i], verbose, &skipped);
        }
    }
    kprintf("audit: no such check: %s\n", name);
    return 0;                       /* not a violation — an unknown name */
}

int audit_run_all(int verbose) {
    int n = audit_count(), bad = 0, skipped = 0;
    if (verbose) kprintf("running %d audit(s):\n", n);
    for (int i = 0; i < n; i++)
        bad += run_one(&__start_audits[i], verbose, &skipped);

    if (bad)
        kprintf("!! AUDIT: %d violation(s) across %d check(s)\n", bad, n);
    else if (verbose)
        kprintf("audit: %d check(s) clean, %d skipped\n", n - skipped, skipped);
    return bad;
}

/* ---- the command ---------------------------------------------------------- */

static void cmd_audit(const char* args) {
    if (!args[0])            { audit_run_all(1);   return; }
    if (streq_(args, "-q"))  { audit_run_all(0);   return; }   /* harness form */
    if (streq_(args, "list")) {
        int n = audit_count();
        kprintf("%d audit(s) registered:\n", n);
        for (int i = 0; i < n; i++) {
            console_write("  ");
            console_write(__start_audits[i].name);
            console_write("\n    ");
            console_write(__start_audits[i].what);
            console_putchar('\n');
        }
        return;
    }
    audit_run_one(args, 1);
}

SHELL_CMD(audit) = {
    "audit", "[list|-q|<check>]",
    "run the runtime invariant checks",
    SHELL_G_SYS, cmd_audit
};

/* =============================================================================
 * THE PERIODIC AUDIT — why it is not simply a command somebody types.
 *
 * §4.74: this project's harness cannot type once a GUI window holds focus.  So
 * the runs most worth auditing — the ones that opened a window, placed a
 * driver, crashed something — are exactly the runs in which `audit` is
 * unreachable.  *Reaching the state to be checked destroys the means of
 * checking it*, which is the same wall `gui.stats_ms` was built to get around.
 *
 * A cron job has no such problem: it runs on its own task, on a schedule, and
 * reports through the same console the harness is already reading.  It is
 * §M30's registry doing the job it exists for.
 *
 * DEFAULT OFF, and that is deliberate rather than timid.  An audit walks every
 * driver and every resource under no lock it needs to hold for long, but it
 * PRINTS on violation — and a machine that is genuinely in a bad state would
 * print every interval, turning one finding into a flood that buries the
 * console the finding has to reach.  Switched on for a test run
 * (`setconf audit.interval_s 5`), it turns every subsequent second of that run
 * into a check.
 * =========================================================================== */

CONFIG_KEY(ck_audit_interval) = {
    .key = "audit.interval_s", .group = "System", .type = CFG_INT,
    .def = "0",
    .help = "run the runtime invariant checks every N seconds (0 = only on demand)",
    .min = 0, .max = 3600,
};

static void audit_cron(void) {
    long iv = config_get_long("audit.interval_s", 0);
    if (iv <= 0) return;                    /* switched off — do nothing, quietly */
    audit_run_all(0);                       /* quiet: prints only on a violation  */
}

/* The interval is read by the job itself rather than baked into the schedule,
 * so switching the key on takes effect at the next tick instead of at the next
 * boot — §M63's whole argument for CONFIG_WATCH, achieved here without one
 * because the job already runs. */
CRON_JOB("audit", audit_cron, 1000);
