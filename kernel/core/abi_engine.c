/* =============================================================================
 * abi_engine.c — canonical operations + dispatch (§M50).  See abi.h for why
 * this exists; this file is the arch-neutral middle of the pipeline.
 *
 * Every handler here is written ONCE and serves every architecture and every
 * guest ABI that names the operation.  Nothing in this file may reference a
 * register, a trap frame, or an architecture — if a handler ever needs to, the
 * translation belongs in the shim or in the number map instead, and the fact
 * that it does not fit is the design telling you something.
 * ============================================================================= */

#include "abi.h"
#include "syscall.h"
#include "task.h"
#include <stddef.h>

/* --- canonical handlers ---------------------------------------------------
 *
 * These are the operations whose translation is genuinely mechanical: the
 * guest's arguments are the native arguments, and the native result is already
 * in the guest's convention (d-os's sys_* return negative on failure, which is
 * the Linux-shaped convention every guest ABI supported so far uses).  An
 * operation whose translation is NOT mechanical still belongs here — as a
 * named handler — rather than being inlined into an arch's switch, because
 * that is what makes it shareable.
 * ------------------------------------------------------------------------- */

static long h_read(struct abi_ctx* c) {
    return sys_read((int)c->a[0], (void*)c->a[1], (size_t)c->a[2]);
}
static long h_write(struct abi_ctx* c) {
    return sys_write((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2]);
}
static long h_close(struct abi_ctx* c) {
    return sys_close((int)c->a[0]);
}
static long h_seek(struct abi_ctx* c) {
    return sys_lseek((int)c->a[0], (long)c->a[1], (int)c->a[2]);
}
static long h_mprotect(struct abi_ctx* c) {
    return sys_mprotect((uintptr_t)c->a[0], (size_t)c->a[1], (int)c->a[2]);
}
static long h_munmap(struct abi_ctx* c) {
    /* There is no sys_munmap: user mmap is a bump allocator that does not
     * reclaim yet, so unmapping succeeds and leaks (a small, bounded leak the
     * x86 layers have always had).  Encoded here as the truth rather than
     * dressed up as a call, so the gap stays visible in one place instead of
     * being rediscovered per arch. */
    (void)c;
    return 0;
}
static long h_getpid(struct abi_ctx* c) {
    (void)c;
    struct task* t = task_current();
    return t ? t->pid : 0;
}
static long h_getppid(struct abi_ctx* c) {
    (void)c;
    struct task* t = task_current();
    return t ? t->ppid : 0;
}

/* The operation table, indexed by `enum abi_op`.  A NULL slot means "declared
 * in the vocabulary, no handler yet" — abi_invoke reports that as unhandled so
 * the caller can fall back, which is what lets an existing hand-written layer
 * migrate one operation at a time instead of in one risky jump. */
static const struct {
    const char*    name;
    abi_handler_fn fn;
} g_ops[ABI_OP_MAX] = {
    [ABI_OP_NONE]  = { "none",     NULL },
    [ABI_READ]     = { "read",     h_read },
    [ABI_WRITE]    = { "write",    h_write },
    [ABI_CLOSE]    = { "close",    h_close },
    [ABI_SEEK]     = { "seek",     h_seek },
    [ABI_MPROTECT] = { "mprotect", h_mprotect },
    [ABI_MUNMAP]   = { "munmap",   h_munmap },
    [ABI_GETPID]   = { "getpid",   h_getpid },
    [ABI_GETPPID]  = { "getppid",  h_getppid },
};

enum abi_op abi_lookup(const struct abi_map* map, unsigned long nr) {
    if (!map || !map->ents) return ABI_OP_NONE;
    for (uint32_t i = 0; i < map->n_ents; i++)
        if (map->ents[i].nr == (uint32_t)nr)
            return (enum abi_op)map->ents[i].op;
    return ABI_OP_NONE;
}

int abi_invoke(enum abi_op op, struct abi_ctx* c, long* out) {
    if (op <= ABI_OP_NONE || op >= ABI_OP_MAX) return 0;
    abi_handler_fn fn = g_ops[op].fn;
    if (!fn) return 0;
    long r = fn(c);
    if (out) *out = r;
    return 1;
}

int abi_dispatch(const struct abi_map* map, unsigned long nr,
                 unsigned long a0, unsigned long a1, unsigned long a2,
                 unsigned long a3, unsigned long a4, unsigned long a5,
                 long* out) {
    enum abi_op op = abi_lookup(map, nr);
    if (op == ABI_OP_NONE) return 0;
    struct abi_ctx c;
    c.a[0] = a0; c.a[1] = a1; c.a[2] = a2;
    c.a[3] = a3; c.a[4] = a4; c.a[5] = a5;
    c.nr   = nr;
    c.map  = map;
    return abi_invoke(op, &c, out);
}

void abi_stats(int* ops_with_handlers, int* ops_total) {
    int n = 0;
    for (int i = 1; i < ABI_OP_MAX; i++) if (g_ops[i].fn) n++;
    if (ops_with_handlers) *ops_with_handlers = n;
    if (ops_total)         *ops_total = ABI_OP_MAX - 1;
}
