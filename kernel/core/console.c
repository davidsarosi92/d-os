/* =============================================================================
 * console.c — output sink registry implementation.
 *
 * One singleton: a head pointer to an intrusive linked list of registered
 * `console_sink`s.  Drivers call `console_sink_register` once during
 * their module init.  `console_putchar` walks the list and forwards to
 * every active sink.
 *
 * The list is single-linked because the iteration is read-mostly: we
 * traverse it on every output character but only mutate it during init.
 * If a driver ever needs to deregister at runtime (e.g. hot-unplug), a
 * doubly-linked variant or a generation counter is the next step.
 * ============================================================================= */

#include "console.h"
#include "printf.h"
#include "task.h"
#include <stddef.h>

static struct console_sink* head = NULL;

/* Per-task output routing (M14).  vc_init installs `vc_putchar` here so
 * console_putchar can deliver to the running task's bound VC without the
 * console core knowing what a VC is.  Opaque pointer + opaque callback. */
static void (*per_task_emit)(void* console, char c) = NULL;

void console_set_per_task_emit(void (*fn)(void*, char)) {
    per_task_emit = fn;
}

static int already_registered(struct console_sink* s) {
    for (struct console_sink* n = head; n; n = n->next) {
        if (n == s) return 1;
    }
    return 0;
}

void console_sink_register(struct console_sink* s) {
    if (!s || already_registered(s)) return;
    /* Push to head — order doesn't matter for broadcast semantics. */
    s->next = head;
    head    = s;
}

int console_sink_any_active(const char* category) {
    for (struct console_sink* n = head; n; n = n->next) {
        if (!n->active) continue;
        if (!category) return 1;
        /* Manual streq — no libc. */
        const char *a = n->category, *b = category;
        if (!a) continue;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return 1;
    }
    return 0;
}

void console_putchar(char c) {
    /* Broadcast to every registered active sink.  Serial sinks stay on
     * for the entire boot — debug logs always reach the host.  Once
     * vc_init runs, the legacy fb sink is deactivated, so only serial
     * receives this broadcast and visible per-shell output flows via
     * the per-task hook below. */
    for (struct console_sink* n = head; n; n = n->next) {
        if (n->active && n->putchar) n->putchar(c);
    }
    /* Per-task routing: if the currently scheduled task has an
     * out_console bound, deliver there too.  This is how each shell
     * task's output lands inside its own pane. */
    if (per_task_emit) {
        struct task* t = task_current();
        if (t && t->out_console) per_task_emit(t->out_console, c);
    }
}

void console_write(const char* s) {
    while (*s) console_putchar(*s++);
}

void console_clear(void) {
    for (struct console_sink* n = head; n; n = n->next) {
        if (n->active && n->clear) n->clear();
    }
}

void console_for_each(console_iter_fn fn, void* ctx) {
    if (!fn) return;
    for (struct console_sink* n = head; n; n = n->next) fn(n, ctx);
}

void console_list(void) {
    int n = 0;
    for (struct console_sink* x = head; x; x = x->next) n++;
    kprintf("console sinks (%d registered):\n", n);
    for (struct console_sink* x = head; x; x = x->next) {
        kprintf("  [%s] %s — %s\n",
                x->category ? x->category : "?",
                x->name,
                x->active   ? "active" : "inactive");
    }
}
