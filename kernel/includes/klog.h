/* =============================================================================
 * klog.h — kernel message ring buffer (M28).
 *
 * The kernel's structured log.  Historically every diagnostic went out
 * through `kprintf` straight to the console sinks (serial + framebuffer)
 * with no levels, no history, and no per-source tag: once a line
 * scrolled off screen it was gone, and there was no `dmesg` to review a
 * boot.  klog fixes that with a fixed-size in-memory ring of records.
 *
 * Two ways in:
 *
 *   1. `kprintf(...)` — every existing call site tees automatically.
 *      printf.c feeds each emitted byte to `klog_feed_char`, which
 *      assembles a line and commits a record on the terminating '\n'.
 *      These lines get the default level (KLOG_INFO) and tag ("kernel").
 *
 *   2. `klog(level, tag, fmt, ...)` — the richer entry point.  Formats
 *      through the very same kprintf machinery (so it still reaches the
 *      console), but stamps the committed record with the given severity
 *      and a short source tag.  Message text should end with '\n', just
 *      like a kprintf line.
 *
 * Design notes:
 *   - The ring is a *static* array (no kmalloc): usable from the very
 *     first boot kprintf, long before the heap exists.
 *   - Timestamps are monotonic milliseconds since boot (`timer_ticks_ms`),
 *     rendered dmesg-style as `[    t.mmm]`.  Absolute wall-clock stamping
 *     off the CMOS RTC is a noted follow-up, not needed for v1.
 *   - Lock-light + SMP-safe at the *record* granularity: the commit that
 *     copies an assembled line into the ring runs under a spinlock, so two
 *     CPUs never corrupt the ring.  The line-assembly staging buffer and
 *     the "pending level/tag" carry the same single-writer assumption
 *     `kprintf` already documents (not reentrant); interleaved emitters
 *     can garble a line, exactly as they already garble console output.
 *
 * Severity levels mirror the classic printk / syslog ordering: a *smaller*
 * number is *more* severe.  `dmesg -l <level>` shows records at least as
 * severe as the threshold (level <= threshold).
 * ============================================================================= */

#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>
#include <stddef.h>

/* printk-style severities (0 = most severe). */
enum klog_level {
    KLOG_EMERG  = 0,        /* system is unusable */
    KLOG_ALERT  = 1,        /* action must be taken immediately */
    KLOG_CRIT   = 2,        /* critical conditions */
    KLOG_ERR    = 3,        /* error conditions */
    KLOG_WARN   = 4,        /* warning conditions */
    KLOG_NOTICE = 5,        /* normal but significant */
    KLOG_INFO   = 6,        /* informational (kprintf default) */
    KLOG_DEBUG  = 7,        /* debug-level chatter */
    KLOG_NLEVELS = 8,
};

#define KLOG_TAG_MAX  16    /* incl. NUL — longer tags are truncated */
#define KLOG_MSG_MAX  200   /* incl. NUL — longer lines are truncated */

/* One committed log line. */
struct klog_record {
    uint64_t seq;                   /* monotonic, starts at 1 */
    uint64_t t_ms;                  /* ms since boot at commit time */
    uint8_t  level;                 /* enum klog_level */
    char     tag[KLOG_TAG_MAX];     /* source, NUL-terminated */
    char     msg[KLOG_MSG_MAX];     /* line text (no trailing '\n'), NUL-term */
};

/* Structured emit: format through kprintf (so it also hits the console)
 * and stamp the committed record with `level` + `tag`.  End `fmt` with a
 * '\n' so the line commits with the intended level/tag. */
void klog(int level, const char* tag, const char* fmt, ...);

/* Tee hook, called by printf.c's emit() for every output byte.  Assembles
 * a line in the staging buffer; on '\n' commits a record.  Not called by
 * anyone else. */
void klog_feed_char(char c);

/* Level name (uppercase, e.g. "WARN") for rendering; NULL-safe range. */
const char* klog_level_name(int level);

/* Snapshot iteration, oldest → newest.  Each live record is copied out
 * under the ring lock and handed to `fn`; safe against concurrent
 * commits (the callback sees a stable copy).  Returns records emitted. */
typedef void (*klog_iter_fn)(const struct klog_record* r, void* ctx);
int  klog_for_each(klog_iter_fn fn, void* ctx);

#endif
