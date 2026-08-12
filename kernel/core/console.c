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
#include "lock.h"
#include "percpu.h"
#include "hal_api.h"
#include <stddef.h>

static struct console_sink* head = NULL;

/* ---------------------------------------------------------------------------
 * §M57 — output serialisation.  Contract + why: console.h.
 *
 * `owner` is written only while the lock is held, and read (unlocked) only to
 * detect THIS CPU re-entering.  That read is safe without an atomic: a stale
 * value can only be another CPU's id, which is not ours, so the worst outcome
 * is that we take the lock — the correct action anyway.  Only our OWN id can
 * make us skip it, and only we can write that.
 * --------------------------------------------------------------------------- */
static spinlock_t   out_lock;
static volatile int out_owner = -1;         /* cpu id holding it, -1 = free */

int console_out_begin(uint32_t* flags) {
    (void)flags;
    /* PREEMPTION off, not interrupts.
     *
     * The obvious implementation takes the lock with interrupts disabled, and
     * it is wrong for this particular lock: a console message can be slow —
     * a full-screen scroll on the framebuffer sink is a multi-megabyte move —
     * and doing that with interrupts off drops timer ticks and can trip the
     * softlockup watchdog.  A logging path that makes the machine look wedged
     * is a poor trade for tidy output.
     *
     * preempt_disable is enough, and it is per-CPU (§M18.6.2).  It keeps the
     * holder on this CPU so a second task here cannot spin against a holder
     * that has been scheduled away, while the timer keeps ticking.
     *
     * What it does NOT prevent is an interrupt on this CPU printing while we
     * hold the lock — which is exactly what the owner test below is for.  The
     * test comes BEFORE the acquire because spin_lock blocks: asking "do I
     * already hold this?" after waiting for it is a question whose only answer
     * is a hung machine.  That ordering is the whole difference between a
     * recursion guard and a deadlock. */
    preempt_disable();
    int self = this_cpu_id();
    if (out_owner == self) {
        /* Already ours further up this CPU's stack (an ISR, an NMI, or a sink
         * that itself prints).  Go on unlocked: interleaved output is a bad
         * outcome, a wedged diagnostic path is a fatal one — and this is the
         * rare case, not the common one the lock exists for. */
        return 0;
    }
    spin_lock(&out_lock);
    out_owner = self;
    return 1;
}

void console_out_end(uint32_t flags, int held) {
    (void)flags;
    if (held) {
        out_owner = -1;
        spin_unlock(&out_lock);
    }
    preempt_enable();
}

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
    /* §M57 — a whole string is one message, for the same reason a whole
     * kprintf is: half of one line inside another is not a smaller problem
     * than a shredded word. */
    uint32_t fl;
    int held = console_out_begin(&fl);
    while (*s) console_putchar(*s++);
    console_out_end(fl, held);
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
