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
#include "fd.h"
#include "proc.h"
#include "vmm.h"        /* vmm_user_access_ok — guest-pointer validation */
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

/* --- operations whose translation is NOT one-to-one ------------------------
 *
 * These are the ones that justify having a canonical vocabulary at all: each
 * needs a decision, and making it once here is the difference between one
 * engine and one hand-written layer per architecture.
 * ------------------------------------------------------------------------- */

/* Linux errno values the guests share.  Only the handful the engine itself
 * returns; a guest ABI whose errno space differs would translate in its own
 * adapter rather than here. */
#define ABI_ENOTTY 25
#define ABI_ENOSYS 38
#define ABI_EFAULT 14

/* Is [uptr, uptr+len) a writable GUEST address in the active space?  The check
 * lives here, in the handler, because this is where the pointer's ORIGIN is
 * known — the same rule the x86 layers arrived at the hard way (DOCS §M46:
 * putting it in a shared sys_* helper broke every in-kernel caller). */
static int abi_user_w_ok(unsigned long uptr, unsigned long len) {
    return uptr && vmm_user_access_ok((uintptr_t)uptr, (uintptr_t)len, 1);
}

/* §M53 stage 3 — read/write a guest `long` at the guest's own width.
 *
 * `struct itimerspec` is four longs; on a 32-bit guest that is 16 bytes and on
 * a 64-bit one 32.  These two helpers are the entire difference, and they take
 * the width from the MAP (the guest's description) rather than from sizeof on
 * the host — which would be right only when guest and kernel happen to agree. */
static unsigned long abi_get_word(const struct abi_ctx* c, unsigned long p, int i) {
    if (c->map && c->map->word_bytes == 4)
        return (unsigned long)((const uint32_t*)(uintptr_t)p)[i];
    return (unsigned long)((const uint64_t*)(uintptr_t)p)[i];
}

static void abi_put_word(const struct abi_ctx* c, unsigned long p, int i,
                         unsigned long v) {
    if (c->map && c->map->word_bytes == 4) ((uint32_t*)(uintptr_t)p)[i] = (uint32_t)v;
    else                                   ((uint64_t*)(uintptr_t)p)[i] = (uint64_t)v;
}

static unsigned long abi_itimerspec_bytes(const struct abi_ctx* c) {
    return (c->map && c->map->word_bytes == 4) ? 16u : 32u;
}

/* itimerspec layout, in guest words: [0]=it_interval.sec [1]=it_interval.nsec
 * [2]=it_value.sec [3]=it_value.nsec.  Note the INTERVAL comes first — a
 * detail worth stating, because getting it backwards produces a timer that
 * works exactly once and then never again, which reads like a different bug. */
#define ABI_ITS_IV_SEC   0
#define ABI_ITS_IV_NSEC  1
#define ABI_ITS_VAL_SEC  2
#define ABI_ITS_VAL_NSEC 3

static long h_timerfd_create(struct abi_ctx* c) {
    (void)c;
    /* The clockid and flags are accepted and ignored: there is one clock here
     * (timer_now_ns) and it is monotonic, so CLOCK_MONOTONIC is what every
     * caller gets whatever it asked for.  Failing instead would stop programs
     * that pass CLOCK_REALTIME out of habit and never depend on the
     * difference. */
    return sys_timerfd_create();
}

static long h_timerfd_settime(struct abi_ctx* c) {
    int fd  = (int)c->a[0];
    int abs = (c->a[1] & 1) != 0;               /* TFD_TIMER_ABSTIME */
    unsigned long newp = c->a[2], oldp = c->a[3];
    if (!newp) return -ABI_EFAULT;
    if (!vmm_user_access_ok((uintptr_t)newp, abi_itimerspec_bytes(c), 0))
        return -ABI_EFAULT;

    if (oldp) {
        if (!abi_user_w_ok(oldp, abi_itimerspec_bytes(c))) return -ABI_EFAULT;
        uint64_t rem = 0, iv = 0;
        sys_timerfd_gettime_k(fd, &rem, &iv);
        abi_put_word(c, oldp, ABI_ITS_IV_SEC,   (unsigned long)(iv / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_IV_NSEC,  (unsigned long)(iv % 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_VAL_SEC,  (unsigned long)(rem / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_VAL_NSEC, (unsigned long)(rem % 1000000000ull));
    }

    uint64_t iv_ns = (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_NSEC);
    uint64_t v_ns  = (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_NSEC);
    return sys_timerfd_settime(fd, abs, v_ns, iv_ns) == 0 ? 0 : -ABI_EFAULT;
}

static long h_timerfd_gettime(struct abi_ctx* c) {
    unsigned long p = c->a[1];
    if (!abi_user_w_ok(p, abi_itimerspec_bytes(c))) return -ABI_EFAULT;
    uint64_t rem = 0, iv = 0;
    if (sys_timerfd_gettime_k((int)c->a[0], &rem, &iv) != 0) return -ABI_EFAULT;
    abi_put_word(c, p, ABI_ITS_IV_SEC,   (unsigned long)(iv / 1000000000ull));
    abi_put_word(c, p, ABI_ITS_IV_NSEC,  (unsigned long)(iv % 1000000000ull));
    abi_put_word(c, p, ABI_ITS_VAL_SEC,  (unsigned long)(rem / 1000000000ull));
    abi_put_word(c, p, ABI_ITS_VAL_NSEC, (unsigned long)(rem % 1000000000ull));
    return 0;
}

/* setitimer(which, new, old).  `struct itimerval` is microseconds, not
 * nanoseconds — the one place POSIX uses a different unit for the same idea,
 * and a silent factor of 1000 if it is missed. */
static long h_setitimer(struct abi_ctx* c) {
    int which = (int)c->a[0];
    if (which != 0) return -ABI_ENOSYS;         /* only ITIMER_REAL → SIGALRM */
    unsigned long newp = c->a[1], oldp = c->a[2];

    if (oldp) {
        if (!abi_user_w_ok(oldp, abi_itimerspec_bytes(c))) return -ABI_EFAULT;
        uint64_t v = 0, iv = 0;
        sys_getitimer_ns(&v, &iv);
        abi_put_word(c, oldp, ABI_ITS_IV_SEC,   (unsigned long)(iv / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_IV_NSEC,  (unsigned long)((iv % 1000000000ull) / 1000));
        abi_put_word(c, oldp, ABI_ITS_VAL_SEC,  (unsigned long)(v / 1000000000ull));
        abi_put_word(c, oldp, ABI_ITS_VAL_NSEC, (unsigned long)((v % 1000000000ull) / 1000));
    }
    if (!newp) return 0;                        /* query-only form */
    if (!vmm_user_access_ok((uintptr_t)newp, abi_itimerspec_bytes(c), 0))
        return -ABI_EFAULT;

    uint64_t iv_ns = (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_IV_NSEC) * 1000ull;
    uint64_t v_ns  = (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_SEC) * 1000000000ull
                   + (uint64_t)abi_get_word(c, newp, ABI_ITS_VAL_NSEC) * 1000ull;
    return sys_setitimer_ns(v_ns, iv_ns) == 0 ? 0 : -ABI_EFAULT;
}

struct abi_iovec { void* base; unsigned long len; };

/* readv/writev: a vector, not a buffer.  Looping over sys_read/sys_write is
 * the whole implementation, but it must stop on a SHORT read — otherwise the
 * next iovec is filled from a later part of the stream and the caller silently
 * gets reordered data. */
static long h_writev(struct abi_ctx* c) {
    const struct abi_iovec* v = (const struct abi_iovec*)c->a[1];
    int n = (int)c->a[2];
    long total = 0;
    for (int i = 0; i < n && v; i++) {
        long w = sys_write((int)c->a[0], v[i].base, (size_t)v[i].len);
        if (w < 0) return total ? total : w;
        total += w;
    }
    return total;
}
static long h_readv(struct abi_ctx* c) {
    const struct abi_iovec* v = (const struct abi_iovec*)c->a[1];
    int n = (int)c->a[2];
    long total = 0;
    for (int i = 0; i < n && v; i++) {
        long r = sys_read((int)c->a[0], v[i].base, (size_t)v[i].len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((unsigned long)r < v[i].len) break;   /* short read → done */
    }
    return total;
}

/* ioctl: ENOTTY, not ENOSYS.  The distinction matters — musl's isatty() reads
 * ENOTTY as "this is not a terminal" and carries on with block buffering,
 * while ENOSYS is an unknown-call error it has no story for.  Answering the
 * question correctly is not the same as implementing the call. */
static long h_ioctl(struct abi_ctx* c) { (void)c; return -ABI_ENOTTY; }

/* brk: report failure so a libc falls back to mmap.  d-os has no program
 * break, and pretending otherwise would hand out addresses nothing backs. */
static long h_brk(struct abi_ctx* c) { (void)c; return 0; }

static long h_mmap(struct abi_ctx* c) {
    return sys_mmap_full(c->a[0], (size_t)c->a[1], (int)c->a[2], (int)c->a[3],
                         (int)c->a[4], (uint64_t)c->a[5]);
}

/* set_tid_address returns the caller's tid.  d-os has no separate tid space,
 * so the pid is the honest answer — the same one gettid gives. */
static long h_sigprocmask(struct abi_ctx* c) { (void)c; return 0; }

/* wait4(pid, status, options, rusage) — rusage is ignored (d-os collects no
 * per-process resource accounting yet); reporting it as unsupported would fail
 * a shell that only ever wants the exit status.
 *
 * The status word is an ENCODING, not the exit code: a guest reads it through
 * WIFEXITED/WEXITSTATUS, which look for the code in bits 8..15 and the signal
 * in bits 0..6.  Handing back the raw code happens to work for 0 and is wrong
 * for every other value — the kind of bug that hides until a program checks
 * whether its child succeeded. */
static long h_wait(struct abi_ctx* c) {
    int code = 0;
    int pid = task_wait((int)c->a[0], &code);
    if (c->a[1]) {
        /* The status slot is the GUEST's pointer — validate it here, where its
         * origin is known.  (§M46's lesson, three times over.) */
        if (!abi_user_w_ok(c->a[1], sizeof(int))) return -ABI_EFAULT;
        *(int*)(uintptr_t)c->a[1] = (code & 0xFF) << 8;   /* WIFEXITED form */
    }
    return pid;
}

/* execve(path, argv, envp) — envp is not honoured yet (the initial stack
 * carries a fixed default environment, see build_initial_stack).  Does not
 * return on success. */
static long h_execve(struct abi_ctx* c) {
    return proc_execve((const char*)(uintptr_t)c->a[0],
                       (char* const*)(uintptr_t)c->a[1]);
}

static long h_settid(struct abi_ctx* c) {
    (void)c;
    struct task* t = task_current();
    return t ? t->pid : 0;
}

/* exit: does NOT return.  Declared in the vocabulary precisely so the shims
 * do not each have to remember that. */
static long h_exit(struct abi_ctx* c) {
    struct task* t = task_current();
    if (t && t->user_task) { fd_close_all(); task_exit_code((int)c->a[0]); }
    return 0;   /* unreachable for a user task */
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
    [ABI_GETTID]   = { "gettid",   h_getpid },      /* no separate tid space */
    [ABI_EXIT]     = { "exit",     h_exit },
    [ABI_READV]    = { "readv",    h_readv },
    [ABI_WRITEV]   = { "writev",   h_writev },
    [ABI_IOCTL]    = { "ioctl",    h_ioctl },
    [ABI_BRK]      = { "brk",      h_brk },
    [ABI_MMAP]     = { "mmap",     h_mmap },
    [ABI_SET_TID_ADDRESS] = { "set_tid_address", h_settid },
    [ABI_SIGPROCMASK]     = { "sigprocmask",     h_sigprocmask },
    [ABI_TIMERFD_CREATE]  = { "timerfd_create",  h_timerfd_create  },
    [ABI_TIMERFD_SETTIME] = { "timerfd_settime", h_timerfd_settime },
    [ABI_TIMERFD_GETTIME] = { "timerfd_gettime", h_timerfd_gettime },
    [ABI_SETITIMER]       = { "setitimer",       h_setitimer       },
    [ABI_WAIT]            = { "wait",            h_wait },
    [ABI_EXECVE]          = { "execve",          h_execve },
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
