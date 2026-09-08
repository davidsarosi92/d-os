/* =============================================================================
 * audit.h — runtime invariant checks (§M71).
 *
 * WHAT THIS IS FOR, and why it is not a code review.
 *
 * The defects this exists to catch are the ones where the CODE READS
 * CORRECTLY and the RUNNING MACHINE is in a state nobody intended: a driver
 * configured for ring 3 that is actually executing in ring 0; a resource
 * grant that survived the thing that owned it; a pointer boundary that is
 * declared, documented, tested — and never armed on one of its paths.
 *
 * THIS TREE HAS PAID FOR EACH OF THOSE, and the receipts are the argument:
 *
 *   §M52  — `syscall_entry.s` documented itself as "UP-correct only … ring-3
 *           tasks only run on the BSP today.  Noted, not built."  Every word
 *           was true when written.  §M35 then gave x86_64 a per-CPU TSS and
 *           ring-3 tasks began running on APs; nothing went back to the note,
 *           and two CPUs ran the kernel on ONE STACK.  **A comment cannot fail
 *           a test.**  Both milestones that invalidated it were green.
 *
 *   §M47.2 — `linux_syscall_dispatch` never set `task->in_user_syscall` on
 *           EITHER arch, so §M46's first pointer boundary was OFF for every
 *           musl program: coreutils, sh, TLS, NetSurf, Wayland.  Nothing
 *           failed visibly, which is exactly why it survived two milestones.
 *
 *   §4.83  — a failed bring-up kept the MMIO grant it had taken, and a restart
 *           loop WIDENED an IOMMU boundary one grant at a time while
 *           `drv domain` went on reporting `isolation full`.  *A boundary that
 *           widens quietly every time a driver crashes is worse than no
 *           boundary, because the reports keep agreeing with the intention.*
 *
 * None of the three was found by reading, and none could have been: reading is
 * how they were introduced.  What finds them is a check that RUNS and can
 * FAIL — the shape §M57 already gave the scheduler (`task_rq_audit` states six
 * runqueue rules in code and `rqcheck` runs them) and §M39 gave the buddy
 * allocator (`pmm_validate`).  This generalises that shape so a subsystem can
 * add an invariant without anything else learning about it.
 *
 * -----------------------------------------------------------------------------
 * THE RULES AN AUDIT MUST FOLLOW
 *
 * 1. **IT MUST BE ABLE TO FAIL.**  A check nobody has seen fail is a check
 *    nobody has tested — §M31's `hardlock` and §M33's `drv crash` exist for
 *    exactly this reason.  Every audit here ships with a way to violate the
 *    thing it checks, named in its own comment.  §M57's first `schedstorm`
 *    reported `ok` against the very kernel it was written to break, because
 *    the only thing it could race was a 100 ms balancer: *a test that cannot
 *    fail is not evidence.*
 *
 * 2. **IT REPORTS WHAT IT OBSERVED, NOT WHAT IT BELIEVES.**  Where the
 *    hardware keeps the record, read the hardware — §M33 stage 5's refused-DMA
 *    count comes from the IOMMU's own fault registers and not from a counter
 *    we increment, because *a count our software keeps proves our software
 *    believes something.*
 *
 * 3. **IT DISTINGUISHES "CANNOT CHECK" FROM "CHECKED, CLEAN".**  An audit with
 *    nothing to look at returns AUDIT_SKIP and says so.  Reporting a pass for
 *    a check that never ran is the failure mode this whole file is against —
 *    it is §M52 in a new costume.
 *
 * 4. **IT DOES NOT FIX ANYTHING.**  An audit that repairs what it finds
 *    destroys the evidence and turns a reproducible defect into an
 *    intermittent one.  `pmm_validate` repairs a broken ring because a
 *    corrupt free list would fault the allocator; that is a deliberate
 *    exception, and it REPORTS the repair.
 *
 * THREADING: an audit runs on an ordinary task, in ring 0, with nothing held.
 * It may take locks and allocate.  It must NOT be called from a fault handler
 * or an ISR — the fault path already has §M47's lock-free capture, and an
 * audit is precisely the wrong thing to run on a machine that has just told
 * you its state is not trustworthy.
 * ============================================================================= */

#ifndef AUDIT_H
#define AUDIT_H

/* What an audit returns.  Negative is reserved. */
#define AUDIT_OK    0   /* checked, and every invariant held                   */
#define AUDIT_SKIP -1   /* nothing to check here on this machine/boot          */
/* Any POSITIVE value is the number of violations found.  A count rather than a
 * boolean because "one driver is misplaced" and "all eight are" call for
 * different reactions, and a bare FAIL hides which one you have. */

struct audit_check {
    const char* name;       /* "driver-placement" — the verb `audit <name>`    */
    const char* what;       /* one line: the invariant, stated as a claim      */
    int       (*run)(int verbose);
    /* `verbose` is 1 when a human asked and 0 when the harness did.  Both
     * print VIOLATIONS; only the verbose form prints the rows it checked and
     * found clean.  A silent pass is what makes this runnable after every test
     * without burying the log that the test itself produced. */
};

extern struct audit_check __start_audits[];
extern struct audit_check __stop_audits[];

/* Same linker-section shape as DRIVER(), CONFIG_KEY(), SHELL_CMD() and the
 * rest: an invariant ships next to the code that must maintain it, and
 * audit.c never names one.
 *
 *   static int au_placement(int v) { ... }
 *   AUDIT(placement) = { "driver-placement",
 *                        "every driver runs in the domain it was placed in",
 *                        au_placement };
 */
#define AUDIT(_var)                                                      \
    static const struct audit_check                                      \
    __attribute__((used, section("audits"), aligned(4)))                 \
    _var##_registration

int  audit_count(void);
const struct audit_check* audit_at(int i);

/* Run every audit (or one, by name).  Returns the TOTAL number of violations;
 * 0 means every check that could run, ran clean.
 *
 * The return value is what the harness reads, and it deliberately does NOT
 * count skips: a machine with no IOMMU is not a machine with a broken IOMMU,
 * and folding the two together is how a checker gets ignored (§M57's rules 5
 * and 6 are counted separately for the same reason). */
int  audit_run_all(int verbose);
int  audit_run_one(const char* name, int verbose);

#endif
