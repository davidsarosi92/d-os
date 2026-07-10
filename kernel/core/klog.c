/* =============================================================================
 * klog.c — kernel message ring buffer (M28).  See klog.h for the rationale.
 *
 * Mechanism: a static array of `struct klog_record` used as a circular
 * buffer.  Output arrives one byte at a time via `klog_feed_char` (teed
 * from printf.c's emit()); we assemble bytes into a staging line and, on
 * the terminating '\n', copy the line into the next ring slot as a
 * committed record.  `klog()` is a thin wrapper that stamps the pending
 * level + tag, then formats through kprintf so the same bytes reach both
 * the console and this ring.
 *
 * Concurrency: only the *commit* (slot write + sequence bump) and the
 * *read* (slot copy-out) touch the shared ring, and both run under
 * `klog_lock`.  The staging buffer + pending level/tag are single-writer
 * state, carrying the same non-reentrancy caveat `kprintf` already has —
 * two CPUs formatting into the log at once can interleave a line, exactly
 * as they can already interleave console output.  We never call a user
 * callback while holding the lock (a dmesg callback re-enters kprintf →
 * klog), so iteration copies one slot at a time under the lock and runs
 * the callback with it released.
 * ============================================================================= */

#include "klog.h"
#include "printf.h"
#include "timer.h"
#include "lock.h"
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */
/* Ring storage.                                                          */
/* ---------------------------------------------------------------------- */

#define KLOG_MAX_RECORDS 512            /* power of two; ~114 KiB of BSS */

static struct klog_record ring[KLOG_MAX_RECORDS];
static uint64_t committed = 0;          /* total records ever committed  */
static spinlock_t klog_lock = SPINLOCK_INIT;

/* Line-assembly staging + the level/tag that the next committed line will
 * carry.  Reset to the defaults after every commit so a plain kprintf
 * line (which never sets them) always logs as INFO/"kernel". */
#define DEFAULT_TAG "kernel"
static char   stag[KLOG_MSG_MAX];
static size_t staglen      = 0;
static int    pend_level   = KLOG_INFO;
static char   pend_tag[KLOG_TAG_MAX] = DEFAULT_TAG;

/* ---------------------------------------------------------------------- */
/* Small no-libc string helpers.                                          */
/* ---------------------------------------------------------------------- */

static void copy_str(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return;
    if (src) { for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; }
    dst[i] = '\0';
}

static void reset_pending(void) {
    pend_level = KLOG_INFO;
    copy_str(pend_tag, DEFAULT_TAG, sizeof pend_tag);
}

/* ---------------------------------------------------------------------- */
/* Commit + feed.                                                         */
/* ---------------------------------------------------------------------- */

/* Copy the staged line into the next ring slot as a committed record. */
static void commit_line(void) {
    if (staglen == 0) return;           /* skip blank separator lines */
    stag[staglen] = '\0';

    uint32_t flags = spin_lock_irqsave(&klog_lock);
    struct klog_record* r = &ring[committed % KLOG_MAX_RECORDS];
    r->seq   = ++committed;             /* seq starts at 1 */
    r->t_ms  = timer_ticks_ms();
    r->level = (uint8_t)pend_level;
    copy_str(r->tag, pend_tag, sizeof r->tag);
    /* staglen < KLOG_MSG_MAX by construction; copy incl. NUL. */
    for (size_t i = 0; i <= staglen; i++) r->msg[i] = stag[i];
    spin_unlock_irqrestore(&klog_lock, flags);

    staglen = 0;
    reset_pending();
}

void klog_feed_char(char c) {
    if (c == '\r') return;              /* keep records free of CR noise */
    if (c == '\n') { commit_line(); return; }
    if (staglen < KLOG_MSG_MAX - 1) stag[staglen++] = c;
    /* else: silently truncate the tail of an over-long line */
}

void klog(int level, const char* tag, const char* fmt, ...) {
    if (level < 0) level = 0;
    if (level >= KLOG_NLEVELS) level = KLOG_NLEVELS - 1;
    pend_level = level;
    copy_str(pend_tag, tag ? tag : DEFAULT_TAG, sizeof pend_tag);

    /* Format through the shared kprintf path: bytes reach the console AND
     * tee back into klog_feed_char, which commits on the '\n'. */
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    /* Defence in depth: if the caller forgot the trailing '\n', flush the
     * remainder now so this level/tag cannot bleed into the next line.
     * (Console already got the bytes; this only closes the record.) */
    if (staglen > 0) commit_line();
}

/* ---------------------------------------------------------------------- */
/* Read side.                                                             */
/* ---------------------------------------------------------------------- */

static const char* const level_names[KLOG_NLEVELS] = {
    "EMERG", "ALERT", "CRIT", "ERR", "WARN", "NOTICE", "INFO", "DEBUG"
};

const char* klog_level_name(int level) {
    if (level < 0 || level >= KLOG_NLEVELS) return "?";
    return level_names[level];
}

int klog_for_each(klog_iter_fn fn, void* ctx) {
    if (!fn) return 0;

    /* Snapshot the live range under the lock, then iterate with it
     * released so `fn` may safely re-enter kprintf/klog. */
    uint32_t flags = spin_lock_irqsave(&klog_lock);
    uint64_t total = committed;
    spin_unlock_irqrestore(&klog_lock, flags);

    uint64_t start = total > KLOG_MAX_RECORDS ? total - KLOG_MAX_RECORDS : 0;
    int emitted = 0;
    for (uint64_t i = start; i < total; i++) {
        struct klog_record snap;
        flags = spin_lock_irqsave(&klog_lock);
        snap = ring[i % KLOG_MAX_RECORDS];   /* copy the slot out */
        spin_unlock_irqrestore(&klog_lock, flags);
        /* A slot overwritten since the snapshot just yields a newer line;
         * harmless for a diagnostic dump.  Match by seq to stay in range. */
        if (snap.seq == 0) continue;
        fn(&snap, ctx);
        emitted++;
    }
    return emitted;
}
