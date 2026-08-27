/* =============================================================================
 * drvguard.h — §M33 Tier 0: a driver fault stops being a dead machine.
 *
 * Today a fault inside a driver is a RING-0 fault, which means `kernel.
 * fault_policy` — halt, reboot, or kill the kthread.  All three take the system
 * with them for what may be one broken sound card.  Tier 0 makes a fault inside
 * a driver entry point unwind back out of that entry point instead, so §M66's
 * quarantine can mark the driver dead and the machine keeps running.
 *
 * -----------------------------------------------------------------------------
 * WHAT THIS IS, EXACTLY — the honest limit, stated first
 *
 * **This is NOT memory isolation, and it must never be described as any.**  The
 * driver runs in ring 0, in the one address space, and by the time a fault is
 * taken the wild write has ALREADY HAPPENED.  What Tier 0 contains is the
 * CONSEQUENCE of the trap-style failures — a null dereference, a bad pointer, a
 * divide by zero — which are the common ones, and it converts them from "the
 * box is gone" into "that driver is gone".  §M33's plan says this in the same
 * words, and §M66 said it about quarantine; the reason to keep repeating it is
 * that a mechanism that catches faults LOOKS like isolation from outside.
 *
 * Real memory isolation is Tier 1 (the driver in ring 3) and Tier 2 (plus an
 * IOMMU).  Neither is built.
 *
 * -----------------------------------------------------------------------------
 * HOW IT WORKS — the uaccess fixup, one level up
 *
 * §M46 gave the kernel an exception table: a fault at a known instruction
 * resumes at a known fixup instead of panicking, which is how a user pointer
 * that goes bad DURING a copy returns -EFAULT.  Tier 0 is the same idea with a
 * bigger unit of recovery: instead of "resume at the next instruction", it is
 * "abandon this whole call and return an error to whoever made it".
 *
 * `drvguard_call()` saves the callee-saved registers, the stack pointer and a
 * landing address into a PER-CPU slot, then calls the driver.  The ring-0 fault
 * handler checks that slot before it applies any fault policy; if it is armed
 * and the fault is not itself inside the guard, it rewrites the trap frame to
 * resume in the landing pad, which restores the saved state and returns a
 * failure code up the ordinary C return path.
 *
 * -----------------------------------------------------------------------------
 * THE GUARD THAT MAKES UNWINDING SAFE — and it is load-bearing
 *
 * Unwinding out of a call that HOLDS A LOCK would leave that lock held forever,
 * and a deadlocked machine is worse than a panicked one: a panic says what
 * happened.  So the recovery is REFUSED when the preemption count has moved
 * since the guard was armed — this tree's spinlocks disable preemption, so a
 * changed count is exactly "the driver took a lock and has not released it".
 * In that case the fault falls through to the old policy, which is the correct
 * outcome: the system genuinely cannot continue, and it says so.
 *
 * The same test catches a fault taken with interrupts disabled inside the
 * driver, for the same reason.
 *
 * -----------------------------------------------------------------------------
 * WHAT IS NOT GUARDED, AND WHY
 *
 * IRQ handlers are not run through this.  A fault in interrupt context cannot
 * unwind to a caller — there is no caller, there is an interrupted victim who
 * has nothing to do with the driver — and pretending otherwise would return
 * control to a random stack.  A faulting IRQ handler is still a panic, and
 * making it not one is Tier 1's job (in ring 3 there is a process to kill).
 * Written down rather than silently unhandled.
 *
 * NOT REENTRANT, one slot per CPU, and the arm REFUSES to nest.  A driver whose
 * init calls into another driver's init would otherwise have the inner guard
 * overwrite the outer one, and a fault after the inner returned would unwind to
 * a stack frame that no longer exists — the §M54 defect class.
 * ============================================================================= */

#ifndef DRVGUARD_H
#define DRVGUARD_H

#include <stdint.h>

struct driver;

/* Saved state, per CPU.  The layout is READ BY ASSEMBLY (drvguard_<arch>.S) —
 * changing the order of the `regs` slots means changing that file. */
struct drvguard_ctx {
    /* Arch-defined: callee-saved registers + sp + return ip.  Sized for the
     * LARGEST of the three (aarch64 needs 13: x19..x28, FP, LR, SP; x86 needs
     * 6 and 8).  A shared struct whose array is sized for one arch and read by
     * another is §M56's `epoll_event` trap, and over-allocating a few words is
     * cheaper than making the struct arch-conditional. */
    uintptr_t regs[13];
    int       armed;
    int       preempt_at_arm;
    struct driver* drv;      /* whose entry point we are inside */
    const char*    what;     /* "probe" / "init" / "shutdown" — for the report */
};

/* Call `fn(ctx)` with a fault guard around it.
 *
 * Returns the function's own value on a normal return, or DRVGUARD_FAULTED if
 * the call faulted and was unwound.  The distinction matters to the caller:
 * "init returned -5" and "init died" are different facts and lead to different
 * messages.
 *
 * `fn` may be NULL, which returns 0 — every driver hook in this tree is
 * optional and the callers should not each re-check. */
#define DRVGUARD_FAULTED  (-9999)

int drvguard_call(struct driver* d, const char* what,
                  int (*fn)(void*), void* ctx);

/* Consulted by the ring-0 fault handler BEFORE any fault policy.
 *
 * If a guard is armed on this CPU and recovery is safe, fills `*resume_ip` and
 * `*resume_arg` with the landing pad and the context pointer to enter it with,
 * and returns 1.  The caller sets those into the trap frame and returns from
 * the exception.  Returns 0 if there is nothing to recover to, or if recovery
 * was refused (a held lock — see the header note), in which case the handler
 * proceeds exactly as it did before §M33.
 *
 * `fault_pc` is passed so the guard can DISARM before reporting: the report
 * itself runs driver-adjacent code, and a fault inside it must not be caught by
 * the same guard and loop. */
int drvguard_recover(uintptr_t fault_pc, uintptr_t* resume_ip,
                     uintptr_t* resume_arg);

/* CAPTURE the fault facts, from the fault handler, immediately after a
 * successful drvguard_recover.  Copies four scalars and does nothing else —
 * the printing, the §M47 record and the quarantine all happen later, in
 * drvguard_call, once the unwind has landed on an ordinary stack.
 *
 * That split is §M47's (capture in fault context, deliver in ordinary
 * context) and it is not stylistic: quarantining runs the driver's shutdown
 * hook, which re-enters drvguard_call and OVERWRITES the saved context the
 * landing pad is about to restore.  Doing it from the handler produced a jump
 * through a garbage pointer and an NMI hard lockup. */
void drvguard_report(int exc, const char* exc_name,
                     uintptr_t pc, uintptr_t addr);

/* Is this CPU currently inside a guarded driver call?  Used by the crash
 * reporter to name the driver in the record — an address alone never says
 * whose failure it was (§M54). */
struct driver* drvguard_current(const char** what);

/* Diagnostics: how many faults have been contained, and by whom.  Reported
 * because a system that quietly restarts a driver in a loop looks healthy from
 * outside, and §M29's crash-loop backoff reasoning applies here too. */
uint32_t drvguard_fault_count(void);

/* The landing pad, in assembly.  Declared here so the fault handler can take
 * its address; never called directly from C. */
void drvguard_land(void);

#endif /* DRVGUARD_H */
