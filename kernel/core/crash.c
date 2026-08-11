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
#include "procfs.h"
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

static void crash_nv_store(const struct crash_record* r);   /* fwd */

static void copy_str(char* dst, const char* src, int cap) {
    int i = 0;
    if (src) while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

void crash_report(int kind, int pid, const char* comm,
                  uintptr_t pc, uintptr_t addr, int code, const char* what) {
    /* Claim a slot.  No LOCK here — see the file header: a lock on this path
     * could deadlock against the very fault being recorded.  But `seq++` is a
     * read-modify-write, and two CPUs faulting at the same instant (which is
     * exactly what a shared-state bug looks like) then claimed the SAME slot
     * and interleaved their strings into it, so the one record describing the
     * failure was a blend of two.  §M54 — an atomic increment is still
     * lock-free; there was never a reason for this to be a plain `++`. */
    uint32_t seq = __atomic_fetch_add(&g_next_seq, 1, __ATOMIC_ACQ_REL);
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

    /* Leave a breadcrumb in NVRAM.  An UNCLEAN_BOOT record is itself the REPORT
     * of a previous breadcrumb, so persisting it would overwrite the evidence
     * with our own summary of it. */
    if (r->kind != CRASH_UNCLEAN_BOOT) crash_nv_store(r);
}

/* ---------------------------------------------------------------------------
 * §M54 — serialise the ring-0 fault DUMP (the kprintf, not the record).
 *
 * Two CPUs faulting within a few microseconds of each other interleave their
 * output one character at a time, and the result is unreadable — at precisely
 * the moment the kernel most needs to be read.  A real lock is out of the
 * question here (this runs in fault context, possibly holding the lock whose
 * misuse caused the fault), so this is a bare test-and-set with a BOUNDED
 * wait: a second faulter gives the first one a chance to finish and then
 * prints anyway.  Worst case we are back to today's interleaving; typical
 * case the two dumps come out one after the other, whole.
 * --------------------------------------------------------------------------- */
static volatile uint32_t g_dump_busy;

void crash_dump_begin(void) {
    for (int i = 0; i < 2000000; i++) {
        uint32_t expect = 0;
        if (__atomic_compare_exchange_n(&g_dump_busy, &expect, 1, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return;
        __asm__ volatile ("" ::: "memory");
    }
    /* Timed out — the holder may be halting and will never release.  Print
     * anyway: a garbled report beats no report. */
}

void crash_dump_end(void) {
    __atomic_store_n(&g_dump_busy, 0, __ATOMIC_RELEASE);
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

/* ---------------------------------------------------------------------------
 * Last-record breadcrumb (§M47 stage 2).
 *
 * The marker above records the FACT that a boot died silently.  These bytes
 * carry the DETAIL: a compact copy of the most recent crash record, rewritten
 * on every capture.  After a reset that nothing could log, the next boot can
 * therefore say not just "the last boot died" but "…and the last thing that
 * went wrong was a page fault in 'netsurf' at this address" — which is usually
 * the whole diagnosis.
 *
 * Layout at CRASH_NV_BASE (all little-endian, 40 bytes):
 *   +0  magic  (1)   0x5A — distinguishes a real record from uninitialised CMOS
 *   +1  kind   (1)
 *   +2  cpu    (1)
 *   +3  sum    (1)   XOR of every other byte, so garbage is not believed
 *   +4  pid    (4)
 *   +8  pc     (8)
 *   +16 addr   (8)   faulting address — with `pc`, the actual diagnosis
 *   +24 code   (2)   signal / exception number
 *   +26 upsec  (2)   uptime in seconds at capture, saturated (how far it got)
 *   +28 comm   (12)  truncated task name — enough to recognise the program
 *
 * Writing happens in fault context, so it is port I/O only: no locks, no
 * allocation.  CMOS is byte-at-a-time and slow-ish, but 40 bytes on a path that
 * only runs when something already went wrong is a bargain for the diagnosis.
 *
 * Size ceiling: the HAL exposes indices 0..0x7F, so this block must end at or
 * before 0x7F — 0x51 + 40 = 0x79 leaves a little headroom and stays clear of
 * the RTC/status registers in the low bytes.
 * --------------------------------------------------------------------------- */
#define CRASH_NV_BASE   0x51
#define CRASH_NV_LEN    40
#define CRASH_NV_MAGIC  0x5A

static void nv_put(int off, uint8_t v) { hal_nvram_write(CRASH_NV_BASE + off, v); }
static uint8_t nv_get(int off) { uint8_t v = 0; hal_nvram_read(CRASH_NV_BASE + off, &v); return v; }

/* Persist a compact copy of `r`.  Best-effort by design: on a platform with no
 * NVRAM every write is a no-op and the feature simply does not exist. */
static void crash_nv_store(const struct crash_record* r) {
    uint8_t b[CRASH_NV_LEN];
    for (int i = 0; i < CRASH_NV_LEN; i++) b[i] = 0;
    b[0] = CRASH_NV_MAGIC;
    b[1] = r->kind;
    b[2] = r->cpu;
    for (int i = 0; i < 4; i++) b[4 + i]  = (uint8_t)((uint32_t)r->pid  >> (8 * i));
    for (int i = 0; i < 8; i++) b[8 + i]  = (uint8_t)((uint64_t)r->pc   >> (8 * i));
    for (int i = 0; i < 8; i++) b[16 + i] = (uint8_t)((uint64_t)r->addr >> (8 * i));
    b[24] = (uint8_t)r->code; b[25] = (uint8_t)((unsigned)r->code >> 8);
    /* Saturate rather than wrap: "65535 s" reading as "0 s" would suggest the
     * machine died during boot when it had in fact been up for hours. */
    uint64_t secs = r->ms / 1000;
    if (secs > 0xFFFFu) secs = 0xFFFFu;
    b[26] = (uint8_t)secs; b[27] = (uint8_t)(secs >> 8);
    for (int i = 0; i < 12; i++) b[28 + i] = (uint8_t)r->comm[i];
    uint8_t sum = 0;
    for (int i = 0; i < CRASH_NV_LEN; i++) if (i != 3) sum ^= b[i];
    b[3] = sum;
    for (int i = 0; i < CRASH_NV_LEN; i++) nv_put(i, b[i]);
}

/* Read the breadcrumb back.  Returns 0 unless the magic AND the checksum agree
 * — uninitialised CMOS is full of plausible-looking bytes, and inventing a
 * crash that never happened would be worse than reporting nothing. */
struct crash_crumb {
    int       kind, pid, code;
    uintptr_t pc, addr;
    unsigned  upsec;
    char      comm[13];
};

static int crash_nv_load(struct crash_crumb* c) {
    uint8_t b[CRASH_NV_LEN];
    for (int i = 0; i < CRASH_NV_LEN; i++) b[i] = nv_get(i);
    if (b[0] != CRASH_NV_MAGIC) return 0;
    uint8_t sum = 0;
    for (int i = 0; i < CRASH_NV_LEN; i++) if (i != 3) sum ^= b[i];
    if (sum != b[3]) return 0;
    c->kind = b[1];
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) p |= (uint32_t)b[4 + i] << (8 * i);
    c->pid = (int)p;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[8 + i] << (8 * i);
    c->pc = (uintptr_t)v;
    v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[16 + i] << (8 * i);
    c->addr  = (uintptr_t)v;
    c->code  = (int)((unsigned)b[24] | ((unsigned)b[25] << 8));
    c->upsec = (unsigned)b[26] | ((unsigned)b[27] << 8);
    int n = 0;
    while (n < 12 && b[28 + n]) { c->comm[n] = (char)b[28 + n]; n++; }
    c->comm[n] = '\0';
    return 1;
}

void crash_boot_begin(void) {
    uint8_t prev = 0;
    int have = hal_nvram_read(CRASH_NVRAM_SLOT, &prev);
    if (have && prev == CRASH_MARK_ARMED) {
        /* The previous boot armed the marker and never cleared it.  Pull the
         * breadcrumb too, so the report names what was going wrong last. */
        struct crash_crumb c;
        char what[CRASH_WHAT_MAX];
        if (crash_nv_load(&c)) {
            copy_str(what, "previous boot died after: ", CRASH_WHAT_MAX);
            /* Append the kind name by hand — no snprintf in this file. */
            int n = 0; while (what[n]) n++;
            const char* k = crash_kind_name(c.kind);
            while (*k && n < CRASH_WHAT_MAX - 1) what[n++] = *k++;
            what[n] = '\0';
            /* Re-report the breadcrumb as a first-class record: `addr` and
             * `code` come across too, so the reconstructed entry is as usable
             * as one captured live — the point of widening the crumb. */
            crash_report(CRASH_UNCLEAN_BOOT, c.pid, c.comm[0] ? c.comm : "system",
                         c.pc, c.addr, c.code, what);
            kprintf("crash: PREVIOUS BOOT ENDED UNCLEANLY — last recorded event was "
                    "%s in '%s' (pid %d) at pc=%p addr=%p code=%d, %us into that "
                    "boot — see `crash`\n",
                    crash_kind_name(c.kind), c.comm[0] ? c.comm : "?", c.pid,
                    (void*)c.pc, (void*)c.addr, c.code, c.upsec);
        } else {
            crash_report(CRASH_UNCLEAN_BOOT, -1, "system", 0, 0, 0,
                         "previous boot ended without a clean shutdown");
            kprintf("crash: PREVIOUS BOOT ENDED UNCLEANLY (reset/triple-fault/power "
                    "loss) with NO prior recorded event — see `crash`\n");
        }
    }
    if (have) hal_nvram_write(CRASH_NVRAM_SLOT, CRASH_MARK_ARMED);
    else      kprintf("crash: no NVRAM on this platform — "
                      "unclean-shutdown detection unavailable\n");
}

void crash_boot_clean(void) {
    hal_nvram_write(CRASH_NVRAM_SLOT, CRASH_MARK_CLEAN);
}

/* ---------------------------------------------------------------------------
 * /proc/crash — the machine-readable face of the ring (§M47 stage 2).
 *
 * The `crash` command is for a human at a console; this node is for everything
 * else: a script, a log shipper, a future GUI panel, a `cat /proc/crash` over a
 * serial capture.  Same records, one line each, whitespace-separated with a
 * header comment naming the columns — the shape the rest of /proc already uses.
 *
 * It reads the ring WITHOUT a lock, exactly like the capture side and for the
 * same reason: a reader that could block a fault handler would invert the
 * priority this whole subsystem exists to protect.  A torn read of a record
 * being overwritten right now is possible in principle and harmless in practice
 * — the ring is 32 deep and only a crash storm could reach the entry a reader
 * is on.
 * --------------------------------------------------------------------------- */
static void pw_put_ptr(struct procfs_writer* w, uintptr_t v) {
    pw_puts(w, "0x");
    if (sizeof(uintptr_t) > 4) pw_put_hex(w, (unsigned)((uint64_t)v >> 32), 8);
    pw_put_hex(w, (unsigned)(v & 0xFFFFFFFFu), 8);
}

static void gen_crash(struct procfs_writer* w) {
    pw_puts(w, "captured "); pw_put_uint(w, (unsigned)g_captured); pw_putc(w, '\n');
    pw_puts(w, "ring "); pw_put_uint(w, (unsigned)CRASH_RING); pw_putc(w, '\n');
    pw_puts(w, "sinks ");
    pw_put_uint(w, (unsigned)(__stop_crashsinks - __start_crashsinks));
    pw_putc(w, '\n');
    for (struct crash_sink* s = __start_crashsinks; s < __stop_crashsinks; s++) {
        pw_puts(w, "sink "); pw_puts(w, s->name ? s->name : "?"); pw_putc(w, '\n');
    }
    pw_puts(w, "# records, newest first: "
               "seq uptime_ms kind cpu pid comm pc addr code what\n");
    for (int i = 0; i < CRASH_RING; i++) {
        const struct crash_record* r = crash_at(i);
        if (!r) break;
        pw_put_uint(w, (unsigned)r->seq);            pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)r->ms);             pw_putc(w, ' ');
        pw_puts(w, crash_kind_name(r->kind));        pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)r->cpu);            pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)r->pid);            pw_putc(w, ' ');
        pw_puts(w, r->comm[0] ? r->comm : "?");      pw_putc(w, ' ');
        pw_put_ptr(w, r->pc);                        pw_putc(w, ' ');
        pw_put_ptr(w, r->addr);                      pw_putc(w, ' ');
        pw_put_uint(w, (unsigned)r->code);           pw_putc(w, ' ');
        pw_puts(w, r->what[0] ? r->what : "-");      pw_putc(w, '\n');
    }
}

static struct procfs_node nd_crash = { .name = "crash", .gen = gen_crash };

void crash_init(void) {
    procfs_register(&nd_crash);
}
