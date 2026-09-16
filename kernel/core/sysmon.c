/* =============================================================================
 * sysmon.c — the once-a-second sampler behind §M75's charts.
 *
 * See sysmon.h for why the history lives in the kernel rather than in the
 * window that draws it.  This file is the collection half, and it has three
 * rules that are easy to get wrong and invisible when you do.
 * ============================================================================= */

#include "sysmon.h"
#include "service.h"
#include "task.h"
#include "timer.h"
#include "percpu.h"
#include "smp.h"
#include "pmm.h"
#include "block.h"
#include "net.h"
#include "printf.h"
#include "shellcmd.h"
#include "console.h"

/* ---------------------------------------------------------------------------
 * Storage.  A plain array plus a head index: at four series of sixty 32-bit
 * samples this is 960 bytes, so there is nothing to allocate and therefore
 * nothing that can fail to be allocated when memory is short — which is one of
 * the moments the history is most worth having.
 * ------------------------------------------------------------------------- */
static uint32_t g_ring[SYSMON_NSERIES][SYSMON_SAMPLES];
static uint32_t g_head;        /* index of the NEXT slot to write */
static uint32_t g_count;       /* samples taken since boot (saturates)     */

/* Previous absolutes, for the two series that are RATES. */
static uint64_t p_busy_ms, p_io_ops, p_net_pkts, p_ns;

static const char* const k_names[SYSMON_NSERIES] = { "CPU", "MEMORY", "I/O", "NETWORK" };
static const char* const k_units[SYSMON_NSERIES] = { "%",   "%",      "ops/s", "pkt/s" };

int         sysmon_is_percent(int s)  { return s == SYSMON_CPU || s == SYSMON_MEM; }
const char* sysmon_series_name(int s) { return (s >= 0 && s < SYSMON_NSERIES) ? k_names[s] : "?"; }
const char* sysmon_series_unit(int s) { return (s >= 0 && s < SYSMON_NSERIES) ? k_units[s] : ""; }
uint32_t    sysmon_sample_count(void) { return g_count; }

uint32_t sysmon_latest(int s) {
    if (s < 0 || s >= SYSMON_NSERIES || g_count == 0) return 0;
    uint32_t last = (g_head + SYSMON_SAMPLES - 1) % SYSMON_SAMPLES;
    return g_ring[s][last];
}

int sysmon_history(int s, uint32_t* out, int max_out, uint32_t* out_max) {
    if (out_max) *out_max = 0;
    if (s < 0 || s >= SYSMON_NSERIES || !out || max_out <= 0) return 0;

    int have = (g_count < SYSMON_SAMPLES) ? (int)g_count : SYSMON_SAMPLES;
    if (have > max_out) have = max_out;

    /* Oldest first.  The oldest valid sample sits `have` slots behind the
     * head, which is the same expression whether or not the ring has wrapped —
     * writing the two cases separately is how a ring reader comes to have one
     * that is only exercised after the first minute of uptime. */
    uint32_t start = (g_head + SYSMON_SAMPLES - (uint32_t)have) % SYSMON_SAMPLES;
    uint32_t mx = 0;
    for (int i = 0; i < have; i++) {
        uint32_t v = g_ring[s][(start + (uint32_t)i) % SYSMON_SAMPLES];
        out[i] = v;
        if (v > mx) mx = v;
    }
    if (out_max) *out_max = mx;
    return have;
}

/* ---------------------------------------------------------------------------
 * Collection.
 *
 * RULE 1 — A RATE IS DIVIDED BY THE TIME THAT ACTUALLY PASSED.  The sampler
 * aims at a one-second deadline and will sometimes be late (an emulated box
 * under load, a long IRQ-off section).  Dividing by a nominal 1000 ms then
 * reports a spike that never happened — and a chart of spikes that are
 * artefacts of the sampler is worse than no chart, because it sends somebody
 * looking for a cause.  §M61's counter-versus-clock lesson, in the one place
 * where the sampler's own lateness is the error.
 *
 * RULE 2 — CPU BUSY MUST INCLUDE THE SLICE IN FLIGHT, for the same reason
 * `task_cpu_ms_now` does: `busy_ms` is credited at a context switch, so a core
 * running one task that is never preempted contributes nothing.  §M49's
 * `sched` shipped with exactly this bug and reported a fully loaded core at
 * 0 %.
 *
 * RULE 3 — EVERY SERIES IS SAMPLED FROM THE SAME INSTANT.  Reading the clock
 * once and using it for all of them keeps the four charts describing one
 * moment; re-reading it per series would give each a slightly different
 * denominator, which shows up as charts that disagree about when something
 * happened.
 * ------------------------------------------------------------------------- */
static uint64_t cpu_busy_total_ms(uint64_t now_ms) {
    uint64_t total = 0;
    int n = smp_ncpus();
    for (int i = 0; i < n; i++) {
        struct percpu* p = percpu_at(i);
        if (!p) continue;
        /* Rule 2: the slice this core is inside right now — added BEFORE the
         * halt is subtracted, because a poll loop that owns its core is never
         * switched out and therefore has all of its time in this term rather
         * than in `busy_ms`.  See taskstat.c for the version of this bug that
         * shipped inert for an afternoon. */
        uint64_t occupied = p->busy_ms;
        if (p->current && !p->current->is_idle && now_ms > p->current->sched_in_ms)
            occupied += now_ms - p->current->sched_in_ms;

        /* §M75.2 — occupancy minus waiting = execution.  See percpu.h. */
        uint64_t hm = p->halt_ns / 1000000ull;
        total += (occupied > hm) ? occupied - hm : 0;
    }
    return total;
}

static void net_totals(struct net_device* d, void* ctx) {
    uint64_t* acc = (uint64_t*)ctx;
    *acc += (uint64_t)d->rx_packets + (uint64_t)d->tx_packets;
}

static void sysmon_sample(void) {
    uint64_t now_ns = timer_now_ns();
    uint64_t now_ms = timer_ticks_ms();

    /* --- absolutes ------------------------------------------------------- */
    uint64_t busy = cpu_busy_total_ms(now_ms);

    struct blk_stats bs;
    blk_get_stats(&bs);
    uint64_t io = bs.reads + bs.writes + bs.flushes;

    uint64_t pkts = 0;
    net_for_each(net_totals, &pkts);

    /* --- the first sample establishes a baseline and reports nothing -------
     * A rate needs two points.  Publishing the first absolute AS a rate is how
     * a chart comes to open with a spike the size of the machine's uptime. */
    if (p_ns == 0) {
        p_ns = now_ns; p_busy_ms = busy; p_io_ops = io; p_net_pkts = pkts;
        return;
    }

    uint64_t elapsed_ms = (now_ns - p_ns) / 1000000ull;
    if (elapsed_ms == 0) return;              /* called twice in one ms: skip  */

    /* --- CPU: busy time as a share of the capacity that actually elapsed --- */
    int      ncpu     = smp_ncpus() > 0 ? smp_ncpus() : 1;
    uint64_t capacity = elapsed_ms * (uint64_t)ncpu;
    uint64_t dbusy    = busy - p_busy_ms;
    if (dbusy > capacity) dbusy = capacity;   /* see task_cpu_permille         */
    uint32_t cpu_pct  = (uint32_t)((dbusy * (uint64_t)SYSMON_PERCENT_FULL) / capacity);

    /* --- memory: a level, not a rate; no delta needed --------------------- */
    uint32_t managed = pmm_managed_frames();
    uint32_t mem_pct = managed ? (uint32_t)(((uint64_t)pmm_used_frames()
                                          * (uint64_t)SYSMON_PERCENT_FULL) / managed) : 0;

    /* --- the two rates, per second, over the REAL interval (rule 1) ------- */
    uint32_t io_rate  = (uint32_t)(((io   - p_io_ops)   * 1000ull) / elapsed_ms);
    uint32_t net_rate = (uint32_t)(((pkts - p_net_pkts) * 1000ull) / elapsed_ms);

    g_ring[SYSMON_CPU][g_head] = cpu_pct;
    g_ring[SYSMON_MEM][g_head] = mem_pct;
    g_ring[SYSMON_IO ][g_head] = io_rate;
    g_ring[SYSMON_NET][g_head] = net_rate;

    g_head = (g_head + 1) % SYSMON_SAMPLES;
    if (g_count < 0xFFFFFFFFu) g_count++;

    p_ns = now_ns; p_busy_ms = busy; p_io_ops = io; p_net_pkts = pkts;
}

/* ---------------------------------------------------------------------------
 * The service.
 *
 * THE DEADLINE IS ABSOLUTE AND IS NEVER RE-DERIVED FROM `now`.  Sleeping "one
 * second from whenever I woke up" adds each wake-up's lateness to every period
 * after it, so the sampler drifts away from the wall clock and an hour of
 * history covers rather less than an hour.  Lateness is bounded by the tick;
 * drift is not.  §M53 stage 3 wrote this rule down for periodic timers, and
 * this is the same rule for a periodic task.
 * ------------------------------------------------------------------------- */
static void sysmon_main(void) {
    uint64_t next = timer_now_ns() + (uint64_t)SYSMON_PERIOD_MS * 1000000ull;
    while (!task_should_stop()) {
        task_sleep_until_ns(next);
        if (task_should_stop()) break;
        sysmon_sample();
        next += (uint64_t)SYSMON_PERIOD_MS * 1000000ull;

        /* If we fell so far behind that the next deadline is already past,
         * catch up to the present rather than firing a burst of samples with
         * no time between them — §M30's "a cron that was starved does not
         * stampede", and here it would also divide by an elapsed_ms of 0. */
        uint64_t now = timer_now_ns();
        if (next < now) next = now + (uint64_t)SYSMON_PERIOD_MS * 1000000ull;
    }
}

SERVICE("sysmon", sysmon_main, /*autostart*/1, SVC_RESTART_ALWAYS);

/* ---------------------------------------------------------------------------
 * `sysmon` — the text surface.
 *
 * The charts are the point of this data and they are also the surface on which
 * a broken sampler looks healthiest: a flat line reads as "the machine is
 * idle", which is precisely what a counter that never moves draws.  So the
 * numbers get a text form too, and it is what the headless tests read.
 * ------------------------------------------------------------------------- */
static void cmd_sysmon(const char* args) {
    (void)args;
    uint32_t n = sysmon_sample_count();
    if (n == 0) {
        console_write("sysmon: no samples yet (the first is a baseline; wait ~2 s)\n");
        return;
    }
    kprintf("sysmon: %u samples, %u s of history\n",
            (unsigned)n, (unsigned)(n < SYSMON_SAMPLES ? n : SYSMON_SAMPLES));

    for (int s = 0; s < SYSMON_NSERIES; s++) {
        uint32_t hist[SYSMON_SAMPLES], mx = 0;
        int have = sysmon_history(s, hist, SYSMON_SAMPLES, &mx);
        /* A percent series is stored in tenths, so it is PRINTED as one — this
         * kernel's printf has no float, and a bare tenths figure ("512")
         * would be read as a number a hundred times too large. */
        if (sysmon_is_percent(s)) {
            uint32_t v = sysmon_latest(s);
            kprintf("  %s (%s): now %u.%u, max %u.%u, last:",
                    sysmon_series_name(s), sysmon_series_unit(s),
                    (unsigned)(v / 10u), (unsigned)(v % 10u),
                    (unsigned)(mx / 10u), (unsigned)(mx % 10u));
        } else {
            kprintf("  %s (%s): now %u, max %u, last:",
                    sysmon_series_name(s), sysmon_series_unit(s),
                    (unsigned)sysmon_latest(s), (unsigned)mx);
        }
        /* The tail, newest last — enough to see movement without printing a
         * minute of numbers into a serial log. */
        int from = have > 12 ? have - 12 : 0;
        /* The tail prints in the SAME unit as the summary above it.  Mixing
         * them ("now 100.0 … last: 1000") is the "a number is not a unit"
         * trap that §4.85.6 paid for one layer down, and on one line it is
         * even easier to misread. */
        for (int i = from; i < have; i++) {
            if (sysmon_is_percent(s))
                kprintf(" %u.%u", (unsigned)(hist[i] / 10u), (unsigned)(hist[i] % 10u));
            else
                kprintf(" %u", (unsigned)hist[i]);
        }
        console_putchar('\n');
    }
}

SHELL_CMD(sysmon) = { "sysmon", "", "recent CPU / memory / I/O / network history",
                      SHELL_G_SYS, cmd_sysmon, SHELL_P_ANY };
