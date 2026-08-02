/* =============================================================================
 * crash.c — crash records + pluggable sinks (§M47).  See crash.h for the design
 * (two phases: fault-safe capture, deferred delivery).
 *
 * Everything here is written for one hostile caller: an exception handler.  The
 * ring is static, the writer is lock-free, and nothing in the capture path
 * allocates, formats or does I/O.  If two CPUs report at the same instant the
 * loser's slot may be overwritten — losing one record of a simultaneous double
 * fault is a far better outcome than taking a lock in a fault handler and
 * deadlocking against whoever already held it.
 * ============================================================================= */

#include "crash.h"
#include "printf.h"
#include "klog.h"
#include "timer.h"
#include "percpu.h"
#include "hal_api.h"
#include <stdint.h>
#include <stddef.h>

/* Ring size: enough to survive a burst (a crash loop restarting a service
 * several times) without pushing the interesting first record out. */
#define CRASH_RING  32

static struct crash_record g_ring[CRASH_RING];
static volatile uint32_t   g_next_seq = 1;      /* 0 means "empty slot"        */
static volatile uint32_t   g_captured = 0;      /* total ever captured         */
static volatile uint32_t   g_delivered = 0;     /* how many have reached sinks */

/* Sink registry (linker section, same pattern as DRIVER()/SERVICE()). */
extern struct crash_sink __start_crashsinks[];
extern struct crash_sink __stop_crashsinks[];

static const char* const kind_names[CRASH_KIND_MAX] = {
    "user-fault", "kernel-fault", "hard-lockup",
    "task-hang", "deadlock", "forced-kill", "unclean-boot",
};

const char* crash_kind_name(int kind) {
    if (kind < 0 || kind >= CRASH_KIND_MAX) return "?";
    return kind_names[kind];
}

static void copy_str(char* dst, const char* src, int cap) {
    int i = 0;
    if (src) while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void crash_report(int kind, int pid, const char* comm,
                  uintptr_t pc, uintptr_t addr, int code, const char* what) {
    /* Claim a slot.  Not atomic across CPUs, and deliberately so: see the file
     * header — a lock here could deadlock against the very fault we are
     * recording. */
    uint32_t seq = g_next_seq++;
    struct crash_record* r = &g_ring[seq % CRASH_RING];

    r->seq  = seq;
    r->ms   = timer_ticks_ms();
    r->kind = (uint8_t)((kind < 0 || kind >= CRASH_KIND_MAX) ? CRASH_KIND_MAX - 1 : kind);
    r->cpu  = (uint8_t)this_cpu_id();
    r->pid  = pid;
    r->code = code;
    r->pc   = pc;
    r->addr = addr;
    copy_str(r->comm, comm, CRASH_COMM_MAX);
    copy_str(r->what, what, CRASH_WHAT_MAX);

    g_captured++;
}

int crash_count(void) { return (int)g_captured; }

/* Newest-first indexing: crash_at(0) is the most recent record. */
const struct crash_record* crash_at(int i) {
    if (i < 0 || i >= CRASH_RING) return NULL;
    uint32_t newest = g_next_seq - 1;            /* seq of the last captured  */
    if (newest == 0 || (uint32_t)i >= newest) return NULL;
    struct crash_record* r = &g_ring[(newest - (uint32_t)i) % CRASH_RING];
    return r->seq ? r : NULL;
}

/* ---------------------------------------------------------------------------
 * Delivery.  Runs from an ordinary task, so a sink may do real work.
 * --------------------------------------------------------------------------- */
int crash_drain(void) {
    int n = 0;
    while (g_delivered < g_captured) {
        /* If the ring wrapped while records were undelivered, skip what was
         * overwritten rather than emitting garbage — and say so once, because
         * silently dropping crash reports is exactly the failure this
         * subsystem exists to prevent. */
        uint32_t behind = g_captured - g_delivered;
        if (behind > CRASH_RING) {
            klog(KLOG_WARN, "crash", "%u crash record(s) lost before delivery "
                 "(ring holds %d)", behind - CRASH_RING, CRASH_RING);
            g_delivered = g_captured - CRASH_RING;
            continue;
        }
        const struct crash_record* r = &g_ring[(g_delivered + 1) % CRASH_RING];
        g_delivered++;
        if (!r->seq) continue;
        for (struct crash_sink* s = __start_crashsinks; s < __stop_crashsinks; s++)
            if (s->emit) s->emit(r);
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * Built-in sink: the system log.  Always present, so a crash is recorded even
 * when no other reporting mechanism has been armed — the baseline guarantee
 * that nothing goes unreported.
 * --------------------------------------------------------------------------- */
static void klog_sink(const struct crash_record* r) {
    klog(r->kind == CRASH_USER_FAULT ? KLOG_ERR : KLOG_CRIT, "crash",
         "%s pid=%d '%s' cpu=%u pc=%p addr=%p code=%d %s",
         crash_kind_name(r->kind), r->pid, r->comm, (unsigned)r->cpu,
         (void*)r->pc, (void*)r->addr, r->code, r->what);
}
CRASH_SINK("klog", klog_sink);

/* ---------------------------------------------------------------------------
 * Unclean-shutdown marker.
 *
 * A triple fault, a hardware reset or a power loss leaves NO opportunity to run
 * code — so the only way such an event is ever reported is to notice, on the
 * next boot, that the previous one never said goodbye.  We keep one byte in
 * battery-backed CMOS NVRAM (via the HAL): armed early in boot, disarmed on an
 * orderly shutdown.  Finding it still armed means the previous boot died where
 * nothing could log it.
 *
 * NVRAM rather than a file on purpose: the marker has to survive exactly the
 * events during which no filesystem write can be trusted to have completed.
 * An arch with no NVRAM (aarch64 on QEMU `virt`) reports "unavailable" from the
 * HAL and simply loses this one capability — documented, not silently skipped.
 * --------------------------------------------------------------------------- */
#define CRASH_NVRAM_SLOT   0x50      /* CMOS byte unused by the BIOS clock/RAM */
#define CRASH_MARK_ARMED   0xC5      /* "a boot is in progress"                */
#define CRASH_MARK_CLEAN   0x00

void crash_boot_begin(void) {
    uint8_t prev = 0;
    int have = hal_nvram_read(CRASH_NVRAM_SLOT, &prev);
    if (have && prev == CRASH_MARK_ARMED) {
        /* The previous boot armed the marker and never cleared it. */
        crash_report(CRASH_UNCLEAN_BOOT, -1, "system", 0, 0, 0,
                     "previous boot ended without a clean shutdown");
        kprintf("crash: PREVIOUS BOOT ENDED UNCLEANLY "
                "(reset/triple-fault/power loss) — see `crash`\n");
    }
    if (have) hal_nvram_write(CRASH_NVRAM_SLOT, CRASH_MARK_ARMED);
    else      kprintf("crash: no NVRAM on this platform — "
                      "unclean-shutdown detection unavailable\n");
}

void crash_boot_clean(void) {
    hal_nvram_write(CRASH_NVRAM_SLOT, CRASH_MARK_CLEAN);
}
