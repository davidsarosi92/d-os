/* =============================================================================
 * procfs.h — kernel introspection as files under /proc.
 *
 * Same overall pattern as devfs (M9): synthetic files attached under an
 * existing host directory (`/proc`, pre-created by ramfs).  The
 * difference is the call shape: procfs files are content-on-demand —
 * they have a `gen` callback that fills a `procfs_writer` with the
 * formatted state.  The contents are produced once per `open` and
 * sliced by subsequent `read` calls.
 *
 * Why not just call `meminfo` / `uptime` / `ps` etc. as shell commands?
 *   - One source of truth per fact: a future `cat /proc/meminfo` and
 *     the legacy `meminfo` command should both pull from the same data.
 *   - Files compose: shell scripts and (eventually) other tools can
 *     read kernel state through the same VFS handle they use for
 *     anything else.
 *   - Linux pattern, well-understood.
 *
 * What we don't import from Linux: writable /proc tunables (yet),
 * per-PID subdirectories with hundreds of files, or sysfs's separate
 * tree.  When the need lands we'll add them; for now /proc is read-only
 * and flat.
 * ============================================================================= */

#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include <stddef.h>

/* Tiny growing-string sink passed to gen callbacks.  All append ops use
 * `kmalloc` to grow; on OOM further appends are silently dropped.  Final
 * buffer is owned by procfs and freed on `vfs_close`. */
struct procfs_writer {
    char*   buf;
    size_t  cap;
    size_t  len;
};

/* Append helpers — all are O(1) amortized. */
void pw_putc    (struct procfs_writer*, char c);
void pw_puts    (struct procfs_writer*, const char* s);
void pw_put_uint(struct procfs_writer*, unsigned int v);
void pw_put_hex (struct procfs_writer*, unsigned int v, int min_digits);

/* Per-file generator.  Called at open; content is then served by reads. */
typedef void (*procfs_gen_fn)(struct procfs_writer* w);

struct procfs_node {
    const char*   name;
    procfs_gen_fn gen;
    struct procfs_node* _next;          /* internal: pre-init queue link */
};

/* Register a node — same queue/flush pattern as devfs.  Pre-init
 * registrations queue and are attached when `procfs_init` runs. */
int  procfs_register(struct procfs_node* n);

/* Resolves /proc, attaches all built-in nodes (version, uptime,
 * meminfo, modules, drivers, console, tasks, config), then flushes the
 * pre-init queue.  Must run after the FS is up. */
void procfs_init(void);

#endif
