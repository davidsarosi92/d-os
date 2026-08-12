/* =============================================================================
 * console.h — output sink registry.
 *
 * Replaces the old fixed terminal_* dispatcher with a registry of output
 * sinks.  Every driver that can emit characters (serial UART, framebuffer
 * terminal, legacy VGA text terminal, future virtual consoles, future
 * loggers) registers a `struct console_sink` here.
 *
 * `console_putchar` broadcasts each character to every registered sink
 * whose `active` flag is set.  The `category` field exists so mutually
 * exclusive sinks (e.g. only one "screen" sink at a time) can coordinate
 * via `console_sink_any_active`.  Serial sinks have `category = "serial"`
 * and are independent of screen sinks — both can coexist (screen for
 * the user, serial for debug).
 *
 * Sinks own their backing storage; the registry uses an intrusive
 * linked list (the `next` field on each `struct console_sink`) so we
 * don't need kmalloc for the registry itself.
 * ============================================================================= */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

struct console_sink {
    const char* name;                       /* short identifier */
    const char* category;                   /* e.g. "screen", "serial" */
    void      (*putchar)(char c);
    void      (*clear)(void);               /* may be NULL */
    int         active;                     /* 1 = receiving output */
    struct console_sink* next;              /* registry link, set by registrar */
};

/* Add `s` to the registry.  Idempotent: re-registering the same sink is
 * a no-op.  Drivers typically register early, then flip `active` based
 * on probe results. */
void console_sink_register(struct console_sink* s);

/* Returns non-zero if any sink with matching `category` is currently
 * active.  Used by mutually-exclusive sinks (e.g. VGA defers to FB). */
int  console_sink_any_active(const char* category);

/* Broadcast helpers — every kprintf byte funnels through these.  No-op
 * if no sinks are active. */
void console_putchar(char c);
void console_write(const char* s);
void console_clear(void);

/* M14: install a per-task emit hook called from console_putchar with
 * the running task's `out_console` opaque pointer.  vc_init installs
 * the VC's putchar adapter here.  Pass NULL to detach. */
void console_set_per_task_emit(void (*fn)(void* console, char c));

/* Diag: print the registry to the active sinks.  Used by `lsmod`-style
 * commands and boot logs. */
void console_list(void);

/* Iterate every registered sink.  Used by procfs to render
 * `/proc/console`. */
typedef void (*console_iter_fn)(const struct console_sink* s, void* ctx);
void console_for_each(console_iter_fn fn, void* ctx);

/* =============================================================================
 * §M57 — OUTPUT SERIALISATION.  One message at a time, per machine.
 *
 * Two CPUs writing at once interleave character by character, which turns a
 * line into noise.  That is not a cosmetic problem here: every automated test
 * in this project is a grep over the serial log, so a shredded line reports a
 * PASSING test as a hang.  See printf.c's header for the run that made the
 * point.
 *
 * Bracket a whole message with begin/end.  Two properties matter more than the
 * ordering itself:
 *
 *   - PREEMPTION is disabled while held, not interrupts.  A message can be
 *     slow (a framebuffer scroll moves megabytes), and holding it with
 *     interrupts off drops timer ticks and can trip the softlockup watchdog —
 *     a logging path that makes the machine look wedged is a poor trade for
 *     tidy output.
 *   - RE-ENTRY-SAFE: if this CPU already holds it (an ISR, an NMI, or a sink
 *     that itself prints), begin returns 0 = "not held, do not release" and
 *     the message goes out unlocked rather than deadlocking.  A diagnostic
 *     path that can hang is one that will hang exactly when it is needed.
 *
 * `held` is whatever begin returned; pass it and `flags` back verbatim.
 * `flags` is reserved (unused today) so the discipline at call sites does not
 * have to change if this ever needs to save interrupt state after all.
 * ============================================================================= */
int  console_out_begin(uint32_t* flags);
void console_out_end(uint32_t flags, int held);

#endif
