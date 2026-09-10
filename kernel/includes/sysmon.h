/* =============================================================================
 * sysmon.h — the machine's recent history, kept in the kernel (§M75).
 *
 * WHY THIS IS NOT IN THE TASK MANAGER.  The obvious place for a chart's
 * samples is the window that draws them, and it is wrong for one reason that
 * decides the whole design: **closing the window would discard the history,
 * and the moment you most want a task manager is right after something went
 * odd — which is exactly when its history would be empty.**  A chart that can
 * only ever show you the future is a chart you have to have predicted needing.
 *
 * So the samples are taken whether or not anybody is looking, and the window
 * is a VIEW of them.  §M47 settled the same question for crash records — the
 * window is never the storage — and this is that rule one subsystem over.
 *
 * THE COST OF ALWAYS-ON IS STATED RATHER THAN ASSUMED: one task that wakes
 * once a second, reads six counters and writes them into a fixed array.  It
 * allocates nothing and takes no lock the sampled subsystems do not already
 * take.  **This deliberately inverts §M55's rule** ("netd runs exactly while
 * somebody is waiting for the network"), and the inversion is the feature: a
 * history that only exists while it is being watched is not a history.
 *
 * WHAT IS DELIBERATELY NOT HERE: persistence across a reboot.  The ring is in
 * RAM and a reboot empties it.  Keeping it would need a place to put it and a
 * policy for how much, which is a different feature with a different cost —
 * and §M47's NVRAM breadcrumb already covers the one event that a running
 * kernel cannot record for itself.
 * ============================================================================= */

#ifndef SYSMON_H
#define SYSMON_H

#include <stdint.h>

/* 60 samples at 1 Hz = one minute of history.
 *
 * The number is a UX choice and not an arbitrary one: a minute is long enough
 * to show what a command you just typed did, and short enough that every
 * sample is still legible in a chart a few hundred pixels wide.  Four series
 * at 60 x 4 bytes is under a kilobyte, so the size is not what limits it. */
#define SYSMON_SAMPLES  60
#define SYSMON_PERIOD_MS 1000u

/* The four series, in the order the design's §06 lays the charts out.
 *
 * TWO OF THEM ARE PERCENTAGES AND TWO ARE RATES, and that difference is not
 * cosmetic — it decides how each may be drawn.  A percentage has a natural
 * maximum, so its chart uses a FIXED 0..100 axis and two moments are
 * comparable at a glance.  A rate has none, so its chart must autoscale AND
 * PRINT ITS CURRENT MAXIMUM: a flat line at 10 KB/s and one at 10 MB/s are
 * otherwise the same picture.  *A graph whose scale is invisible is a shape,
 * not a measurement.* */
enum sysmon_series {
    SYSMON_CPU = 0,     /* busy across all CPUs, TENTHS of a percent (0..1000) */
    SYSMON_MEM,         /* managed frames in use,  TENTHS of a percent (0..1000) */
    SYSMON_IO,          /* block-layer operations per second      (rate)        */
    SYSMON_NET,         /* network packets per second, rx+tx      (rate)        */
    SYSMON_NSERIES
};

/* A percent series is stored in TENTHS OF A PERCENT, and that is not fussiness.
 * Whole percents floor to ZERO on any machine with room to spare: the ARM box
 * here has 3 GiB and uses about 18 MB of it, so its memory chart was a flat
 * line at the bottom for ever — truthful, and useless, which is the pair of
 * properties a chart must never have.  Tenths draw a visible line at 0.6 %
 * while still fitting a fixed axis. */
#define SYSMON_PERCENT_FULL 1000u

/* Is this series a percentage (fixed 0..100 axis) or a rate (autoscaled)? */
int         sysmon_is_percent(int series);
const char* sysmon_series_name(int series);   /* "CPU", "MEMORY", "I/O", "NETWORK" */
const char* sysmon_series_unit(int series);   /* "%", "ops/s", "pkt/s"             */

/* Copy the history for one series, OLDEST FIRST, into `out` (SYSMON_SAMPLES
 * entries).  Returns how many are valid — fewer than SYSMON_SAMPLES only
 * during the first minute after boot.
 *
 * OLDEST FIRST because that is the order a chart draws in, and converting a
 * ring index to a screen x is the one piece of arithmetic every caller would
 * otherwise write for itself.  Four charts would be four chances to get the
 * wrap wrong, in the direction that looks like the data is jumping about.
 *
 * `out_max` receives the largest value present, which an autoscaled chart
 * needs and must display.  It is computed here rather than by the caller so
 * that the number drawn on the axis and the number the line is scaled against
 * cannot disagree (§4.79's title buttons, avoided by construction). */
int sysmon_history(int series, uint32_t* out, int max_out, uint32_t* out_max);

/* The most recent sample, or 0 before the first one has been taken. */
uint32_t sysmon_latest(int series);

/* How many samples the monitor has taken since boot.  A chart that reads 0
 * here knows the difference between "the machine is idle" and "nothing has
 * been measured yet" — §M71 rule 3, at the size of a single integer. */
uint32_t sysmon_sample_count(void);

#endif
