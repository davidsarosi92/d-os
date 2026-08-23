/* =============================================================================
 * splash.h — the boot screen (§M62).
 *
 * Boot shows the kernel log scrolling up a framebuffer console.  That is the
 * right default for a kernel under development and the wrong one for showing
 * the machine to anybody — and the interesting part is that BOTH audiences are
 * right, so the answer is a switch, not a choice:
 *
 *   boot.splash = off    (default) the log, exactly as before
 *               | on     the splash; the log is SUPPRESSED, never discarded
 *               | quiet  the splash plus a one-line phase indicator
 *
 * FOUR RULES, and the third is the whole safety argument.
 *
 * 1. It paints straight into the framebuffer through `fb_present`, before the
 *    GUI, the heap-heavy paths or any filesystem exist.  It must not depend on
 *    anything that could be the thing that hangs — so the image is DRAWN, not
 *    loaded: no file to be missing, no decoder to refuse, no allocation to
 *    lose.  (The seam for real artwork later is one function, `splash_draw`.)
 *
 * 2. The log is suppressed by deactivating the SCREEN console sink only.  klog
 *    keeps recording and the serial sink keeps writing, so `dmesg` afterwards
 *    has every line the console would have shown.  *Suppressed, never
 *    discarded* — a boot screen that loses the boot log would trade the one
 *    artefact that makes a bad boot diagnosable for a picture.
 *
 * 3. ANY fault, panic, watchdog trip or NMI TEARS THE SPLASH DOWN before it
 *    prints.  A splash left up over a panic converts a diagnosable crash into
 *    "it froze at the logo", which is precisely the failure every distribution
 *    has shipped at least once.  The hook is `splash_abort()`, called from
 *    `crash_dump_begin()` — the one function every fault-dump path in this
 *    kernel already goes through, so a NEW fault path gets the behaviour for
 *    free rather than needing to remember it.
 *
 * 4. Any keypress drops to the log immediately.  A boot screen you cannot get
 *    out of is a boot screen that hides the answer exactly when it is needed.
 * ============================================================================= */

#ifndef SPLASH_H
#define SPLASH_H

/* Read `boot.splash` and, if enabled, paint the screen + suppress the console.
 * Safe to call before the GUI; a no-op without a usable framebuffer. */
void splash_begin(void);

/* Go quiet on the SCREEN as soon as the framebuffer exists, before the splash
 * decision can be made (the setting lives on a disk that is not mounted yet).
 * `splash_begin` then either keeps the screen (splash) or hands it back and
 * REPLAYS the missed lines from klog.  Serial and klog are never touched. */
void splash_early_quiet(void);

/* Called once the VIRTUAL CONSOLE owns the screen (after vc_init): if the
 * splash was declined, the lines the screen missed while it was quiet are
 * replayed here.  Earlier would be pointless — vc_init paints over the boot
 * log by design. */
void splash_screen_ready(void);

/* Report progress.  `phase` is a short label ("drivers", "filesystems", …) and
 * `pct` is 0..100.  Ignored when the splash is not up.  Honest or absent: the
 * caller passes real boot phases, because a bar driven by a timer is a lie the
 * log does not tell. */
void splash_progress(const char* phase, int pct);

/* Normal end of boot: restore the console.  Idempotent. */
void splash_end(void);

/* Tear the splash down NOW and restore the text console, from any context
 * including a fault.  Never allocates, never takes a lock. */
void splash_abort(void);

/* Non-zero while the splash owns the screen. */
int  splash_active(void);

/* Keyboard escape hatch: called from the key path; any key ends the splash.
 * Returns non-zero if the key was consumed by the splash. */
int  splash_key(void);

/* The `splash` command (both shells): show, dismiss, or report.  Present so the
 * fault-teardown rule can be tested on a running system. */
void splash_cmd(const char* args);

/* Deliberate ring-0 fault (`splash faultkernel`) — see splash.c: the teardown
 * rule is only a rule if it can be falsified. */
void splash_faultkernel(void);

#endif
