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
#include "printf.h"
#include "epoll.h"        /* EPOLL_CTL_* — the guest's own numbers */
#include "syscall.h"
#include "task.h"
#include "fd.h"
#include "proc.h"
#include "kmalloc.h"
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
#define ABI_EINVAL 22
#define ABI_ENOMEM 12

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
/* §M56.1 — rt_sigprocmask, for real.
 *
 * This was `return 0` — accept and forget.  That is not a harmless stub: a
 * program that blocks SIGPIPE around a write to a closed pipe, or blocks
 * SIGALRM while it touches the state its handler reads, was getting neither,
 * silently.  The kernel had the pending mask all along; what it lacked was
 * the blocked one.
 *
 * The mask is a guest `sigset_t`, which is 128 bytes on Linux — but only the
 * low 32 bits can name a signal this kernel has, so we read and write the
 * first word and leave the rest alone.  Writing zeroes over the whole thing
 * would be worse than ignoring it: a libc that keeps state in the high words
 * would have it silently cleared. */
#define ABI_SIG_BLOCK   0
#define ABI_SIG_UNBLOCK 1
#define ABI_SIG_SETMASK 2

/* THE OFF-BY-ONE THAT IS NOT AN OFF-BY-ONE.
 *
 * A Linux `sigset_t` stores signal N at bit N-1: SIGHUP (1) is bit 0.  This
 * kernel's `sig_pending` / `sig_blocked` store signal N at bit N, because they
 * are built with `1u << sig` and nothing ever needed to disagree.  Both
 * conventions are self-consistent; copying the word across without shifting is
 * what is wrong, and it fails SILENTLY — SIGALRM (14) blocked by the guest
 * arrives as bit 14 in the kernel, which is SIGCHLD's slot here, so the mask
 * appears to be set and simply never matches.
 *
 * It cost a test that reported `pending=0` on all three arches with no other
 * symptom.  These two functions are the entire fix, and they exist as named
 * functions rather than inline shifts so the next signal-shaped operation has
 * something obvious to call. */
static uint32_t abi_sigset_to_kernel(uint64_t guest) { return (uint32_t)(guest << 1); }
static uint64_t abi_sigset_to_guest (uint32_t kern)  { return (uint64_t)kern >> 1; }

/* How much of a guest `sigset_t` is worth touching.
 *
 * The declared type is 128 bytes, but Linux only defines 64 signals, so the
 * kernel ABI passes `sigsetsize` and every libc passes 8.  This kernel has 32
 * signals and no real-time signals at all, so bits 32..63 can never be pending
 * here — reporting them as ZERO is the truth, not a loss of information, which
 * is why the full 8 bytes are written rather than only the first 4.  Anything
 * the guest keeps beyond `sigsetsize` is its own business and is left alone. */
#define ABI_SIGSET_BYTES 8

static int abi_sigset_read(struct abi_ctx* c, unsigned long p, uint64_t* out) {
    (void)c;
    if (!vmm_user_access_ok((uintptr_t)p, ABI_SIGSET_BYTES, 0)) return 0;
    const uint8_t* b = (const uint8_t*)(uintptr_t)p;
    uint64_t v = 0;
    for (int i = 0; i < ABI_SIGSET_BYTES; i++) v |= (uint64_t)b[i] << (8 * i);
    *out = v;
    return 1;
}

static int abi_sigset_write(struct abi_ctx* c, unsigned long p, uint64_t v) {
    if (!abi_user_w_ok(p, ABI_SIGSET_BYTES)) return 0;
    (void)c;
    uint8_t* b = (uint8_t*)(uintptr_t)p;
    for (int i = 0; i < ABI_SIGSET_BYTES; i++) b[i] = (uint8_t)(v >> (8 * i));
    return 1;
}

static long h_sigprocmask(struct abi_ctx* c) {
    struct task* t = task_current();
    if (!t) return 0;
    int how = (int)c->a[0];
    unsigned long setp = c->a[1], oldp = c->a[2];

    uint32_t old = t->sig_blocked;
    if (oldp && !abi_sigset_write(c, oldp, abi_sigset_to_guest(old)))
        return -ABI_EFAULT;
    if (!setp) return 0;                        /* query only */

    uint64_t gw = 0;
    if (!abi_sigset_read(c, setp, &gw)) return -ABI_EFAULT;
    uint32_t nw = abi_sigset_to_kernel(gw);
    uint32_t nb;
    switch (how) {
    case ABI_SIG_BLOCK:   nb = old |  nw; break;
    case ABI_SIG_UNBLOCK: nb = old & ~nw; break;
    case ABI_SIG_SETMASK: nb = nw;        break;
    default: return -ABI_EINVAL;
    }
    /* SIGKILL is never blockable — see the delivery path's note.  Masking it
     * here rather than only at delivery means `sigprocmask(SIG_BLOCK, full)`
     * followed by a query reports the truth. */
    nb &= ~(1u << 9);                           /* SIGKILL */
    t->sig_blocked = nb;

    /* Unblocking may have made an already-pending signal deliverable, and the
     * task is about to return to ring 3 — where the delivery check runs — so
     * nothing else is needed here.  Stating it because the absence of a wake
     * looks like an omission. */
    return 0;
}

/* wait4(pid, status, options, rusage) — rusage is ignored (d-os collects no
 * per-process resource accounting yet); reporting it as unsupported would fail
 * a shell that only ever wants the exit status.
 *
 * The status word is an ENCODING, not the exit code: a guest reads it through
 * WIFEXITED/WEXITSTATUS, which look for the code in bits 8..15 and the signal
 * in bits 0..6.  Handing back the raw code happens to work for 0 and is wrong
 * for every other value — the kind of bug that hides until a program checks
 * whether its child succeeded. */
/* ---------------------------------------------------------------------------
 * §M56 — epoll.
 *
 * The whole arch-specific content of this is ONE number: how big the guest's
 * `struct epoll_event` is.  Everything else — the flags, the ctl ops, the
 * timeout convention — is identical across the three ABIs, which is exactly
 * the split §M50's engine exists to expose.
 *
 * The guest layout is { u32 events; u64 data; }, packed on x86 (12 bytes, data
 * at offset 4, UNALIGNED on a 64-bit host) and unpacked on arm64 (16 bytes,
 * data at offset 8).  We therefore read and write `data` BYTEWISE rather than
 * through a u64*: on x86_64 that pointer would be misaligned, and while x86
 * tolerates it, writing the kernel so that it only works on forgiving hardware
 * is how an arch port later fails for no visible reason.
 * ------------------------------------------------------------------------- */
static unsigned long abi_epoll_ev_bytes(const struct abi_ctx* c) {
    /* Default to the x86 packed size rather than 0 if a map predates the
     * field — a zero here would make every bounds check pass trivially. */
    if (c->map && c->map->epoll_event_bytes) return c->map->epoll_event_bytes;
    return 12u;
}

/* Offset of the `data` member: right after the u32 when packed, 8-aligned
 * when not.  Derived from the struct SIZE, which is the only thing the map
 * needs to carry. */
static unsigned long abi_epoll_data_off(const struct abi_ctx* c) {
    return abi_epoll_ev_bytes(c) == 12u ? 4u : 8u;
}

static uint64_t abi_ld64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);   /* LE guests */
    return v;
}
static void abi_st64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t abi_ld32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void abi_st32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static long h_epoll_create(struct abi_ctx* c) {
    /* epoll_create's `size` hint and epoll_create1's flags are both accepted
     * and ignored: the size hint has been advisory since Linux 2.6.8, and the
     * only flag is EPOLL_CLOEXEC, which is meaningful once this kernel has
     * close-on-exec at all. */
    (void)c;
    return sys_epoll_create();
}

static long h_epoll_ctl(struct abi_ctx* c) {
    int epfd = (int)c->a[0], op = (int)c->a[1], fd = (int)c->a[2];
    unsigned long evp = c->a[3];
    uint64_t kev[2] = { 0, 0 };

    if (op != EPOLL_CTL_DEL) {
        unsigned long sz = abi_epoll_ev_bytes(c);
        if (!evp || !vmm_user_access_ok((uintptr_t)evp, sz, 0)) return -ABI_EFAULT;
        const uint8_t* p = (const uint8_t*)(uintptr_t)evp;
        kev[0] = abi_ld32(p);
        kev[1] = abi_ld64(p + abi_epoll_data_off(c));
    }
    return sys_epoll_ctl_k(epfd, op, fd, (uint32_t)kev[0], kev[1]);
}

static long h_epoll_wait(struct abi_ctx* c) {
    int epfd = (int)c->a[0];
    unsigned long outp = c->a[1];
    int maxevents = (int)c->a[2];
    int timeout   = (int)c->a[3];
    unsigned long maskp = c->a[4];

    /* §M56.1 — epoll_pwait's mask, honoured.  The whole reason the call exists
     * is that "unblock this signal" and "start waiting" must be ONE step: do
     * them separately and a signal arriving in between is delivered while the
     * program is not yet waiting, so the wait it then enters has nothing left
     * to wake it.  Swapping the mask around the wait closes exactly that
     * window — which is why it could not be done until the blocked mask was
     * real (it was a `return 0` stub until this milestone). */
    struct task* t = task_current();
    uint32_t saved_mask = 0;
    int mask_swapped = 0;
    if (maskp && t) {
        uint64_t gw = 0;
        if (!abi_sigset_read(c, maskp, &gw)) return -ABI_EFAULT;
        saved_mask = t->sig_blocked;
        t->sig_blocked = abi_sigset_to_kernel(gw) & ~(1u << 9);  /* SIGKILL */
        mask_swapped = 1;
    }

    if (maxevents <= 0 || maxevents > 256) return -ABI_EINVAL;
    unsigned long sz = abi_epoll_ev_bytes(c);
    if (!outp || !abi_user_w_ok(outp, sz * (unsigned long)maxevents))
        return -ABI_EFAULT;

    uint64_t* k = (uint64_t*)kmalloc(sizeof(uint64_t) * 2 * (size_t)maxevents);
    if (!k) { if (mask_swapped) t->sig_blocked = saved_mask; return -ABI_ENOMEM; }
    int n = sys_epoll_wait_k(epfd, k, maxevents, timeout);
    if (mask_swapped) t->sig_blocked = saved_mask;   /* restore on EVERY path */
    if (n > 0) {
        uint8_t* out = (uint8_t*)(uintptr_t)outp;
        unsigned long doff = abi_epoll_data_off(c);
        for (int i = 0; i < n; i++) {
            uint8_t* e = out + (unsigned long)i * sz;
            abi_st32(e, (uint32_t)k[i * 2 + 0]);
            abi_st64(e + doff, k[i * 2 + 1]);
        }
    }
    kfree(k);
    return n;
}

/* rt_sigpending(set, sigsetsize).  Reports signals that ARRIVED while blocked
 * — the proof that sigprocmask defers rather than discards. */
static long h_sigpending(struct abi_ctx* c) {
    struct task* t = task_current();
    unsigned long p = c->a[0];
    if (!p) return -ABI_EFAULT;
    /* Only the signals that are BOTH pending and blocked: an unblocked
     * pending signal is one the task simply has not returned to ring 3 to
     * collect yet, and reporting it would be a race, not information. */
    uint64_t v = t ? abi_sigset_to_guest(t->sig_pending & t->sig_blocked) : 0;
    if (!abi_sigset_write(c, p, v)) return -ABI_EFAULT;
    return 0;
}

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

/* ===========================================================================
 * The BSD socket surface (§M24 second half).
 *
 * ONE marshalling of `struct sockaddr_in`, shared by every architecture and
 * every entry convention.  Before this it existed twice — once in each x86
 * layer — and a third copy was exactly what the aarch64 port would have
 * needed, for a struct whose layout is the same everywhere.
 *
 * AND IT IS THE SAME EVERYWHERE, which is worth stating because the last
 * struct through this pipeline was not: `struct epoll_event` is 12 bytes on
 * i386 and amd64 but 16 on arm64 (§M56), so a size derived from the word width
 * passes on two arches and fails on the third.  `sockaddr_in` has no such
 * trap — 16 bytes, fixed fields, network byte order — and `socklen_t` is a
 * 32-bit unsigned on every ABI here.  A shared marshaller is therefore CORRECT
 * rather than merely convenient, and saying so is how the next person knows
 * not to go looking for a per-arch case that was never needed.
 * ======================================================================== */

#define ABI_AF_INET        2
#define ABI_SOCK_NONBLOCK  0x800      /* Linux O_NONBLOCK, socket-type flag   */
#define ABI_SOCK_CLOEXEC   0x80000
#define ABI_EAFNOSUPPORT   97
#define ABI_EOPNOTSUPP     95
#define ABI_ENOTCONN       107
#define ABI_EAGAIN         11

struct abi_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;                /* network byte order                   */
    uint32_t sin_addr;                /* network byte order                   */
    uint8_t  sin_zero[8];
};

static uint16_t abi_ntohs(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static uint32_t abi_ntohl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static int abi_user_r_ok(unsigned long uptr, unsigned long len) {
    return uptr && vmm_user_access_ok((uintptr_t)uptr, (uintptr_t)len, 0);
}

/* guest sockaddr_in → host-order (ip, port).  Returns 0 on success. */
static int abi_addr_in(unsigned long uaddr, uint32_t* ip, int* port) {
    if (!abi_user_r_ok(uaddr, sizeof(struct abi_sockaddr_in))) return -1;
    const struct abi_sockaddr_in* sa = (const struct abi_sockaddr_in*)(uintptr_t)uaddr;
    if (sa->sin_family != ABI_AF_INET) return -2;
    *ip   = abi_ntohl(sa->sin_addr);
    *port = (int)abi_ntohs(sa->sin_port);
    return 0;
}

/* host-order (ip, port) → guest sockaddr_in, honouring the caller's socklen.
 *
 * Linux writes at most `*addrlen` bytes and then stores the address's TRUE
 * size, which is how a caller learns it was truncated.  Reporting the
 * truncated length instead would make every short buffer look like a success. */
static void abi_addr_out(unsigned long uaddr, unsigned long ulen,
                         uint32_t ip, int port) {
    if (!uaddr || !ulen) return;
    if (!abi_user_r_ok(ulen, sizeof(uint32_t))) return;
    uint32_t room = *(uint32_t*)(uintptr_t)ulen;
    struct abi_sockaddr_in sa;
    sa.sin_family = ABI_AF_INET;
    sa.sin_port   = abi_ntohs((uint16_t)port);
    sa.sin_addr   = abi_ntohl(ip);
    for (int i = 0; i < 8; i++) sa.sin_zero[i] = 0;
    uint32_t n = room < sizeof sa ? room : (uint32_t)sizeof sa;
    if (n && abi_user_w_ok(uaddr, n)) {
        uint8_t* dst = (uint8_t*)(uintptr_t)uaddr;
        const uint8_t* src = (const uint8_t*)&sa;
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    }
    if (abi_user_w_ok(ulen, sizeof(uint32_t)))
        *(uint32_t*)(uintptr_t)ulen = (uint32_t)sizeof sa;
}

static long h_socket(struct abi_ctx* c) {
    int domain = (int)c->a[0];
    int type   = (int)c->a[1];
    if (domain != ABI_AF_INET) return -ABI_EAFNOSUPPORT;
    int fd = sys_socket(domain, type & 0xFF, (int)c->a[2]);
    if (fd < 0) return -ABI_EOPNOTSUPP;
    /* SOCK_NONBLOCK is not decoration: musl's resolver drains its socket with
     * `while (recvmsg(...) >= 0)` and needs the EAGAIN only a non-blocking
     * socket produces (§M39). */
    if (type & ABI_SOCK_NONBLOCK) sys_socket_setnonblock(fd, 1);
    return fd;
}

static long h_bind(struct abi_ctx* c) {
    uint32_t ip; int port;
    int r = abi_addr_in(c->a[1], &ip, &port);
    if (r == -1) return -ABI_EFAULT;
    if (r == -2) return -ABI_EAFNOSUPPORT;
    return sys_bind((int)c->a[0], ip, port) == 0 ? 0 : -ABI_EINVAL;
}

static long h_connect(struct abi_ctx* c) {
    uint32_t ip; int port;
    int r = abi_addr_in(c->a[1], &ip, &port);
    if (r == -1) return -ABI_EFAULT;
    if (r == -2) return -ABI_EAFNOSUPPORT;
    /* ECONNREFUSED (111) is the honest answer now that a closed port answers
     * with an RST rather than with silence — the stack cannot tell us which
     * yet, so the generic failure stands and the distinction is written down
     * as the next thing to carry through. */
    return sys_connect((int)c->a[0], ip, port) == 0 ? 0 : -111;
}

static long h_listen(struct abi_ctx* c) {
    return sys_listen((int)c->a[0], (int)c->a[1]) == 0 ? 0 : -ABI_EINVAL;
}

/* accept(fd, addr, addrlen) and accept4(fd, addr, addrlen, flags). */
static long h_accept_common(struct abi_ctx* c, int flags) {
    uint32_t ip = 0; int port = 0;
    int fd = sys_accept_k((int)c->a[0], &ip, &port);
    if (fd == -ABI_EAGAIN) return -ABI_EAGAIN;
    if (fd < 0) return -ABI_EINVAL;
    abi_addr_out(c->a[1], c->a[2], ip, port);
    if (flags & ABI_SOCK_NONBLOCK) sys_socket_setnonblock(fd, 1);
    return fd;
}
static long h_accept (struct abi_ctx* c) { return h_accept_common(c, 0); }
static long h_accept4(struct abi_ctx* c) { return h_accept_common(c, (int)c->a[3]); }

static long h_getsockname(struct abi_ctx* c) {
    uint32_t ip = 0; int port = 0;
    if (sys_getsockname_k((int)c->a[0], &ip, &port) != 0) return -ABI_EINVAL;
    abi_addr_out(c->a[1], c->a[2], ip, port);
    return 0;
}

static long h_getpeername(struct abi_ctx* c) {
    uint32_t ip = 0; int port = 0;
    if (sys_getpeername_k((int)c->a[0], &ip, &port) != 0) return -ABI_ENOTCONN;
    abi_addr_out(c->a[1], c->a[2], ip, port);
    return 0;
}

/* send/recv are sendto/recvfrom without an address, and on a CONNECTED socket
 * that is exactly a write/read — which is where the stream payload has always
 * gone (sys_read/sys_write route FD_NETSOCK to the TCP engine). */
static long h_send(struct abi_ctx* c) {
    return sys_write((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2]);
}
static long h_recv(struct abi_ctx* c) {
    return sys_read((int)c->a[0], (void*)c->a[1], (size_t)c->a[2]);
}

static long h_sendto(struct abi_ctx* c) {
    if (!c->a[4]) return sys_write((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2]);
    uint32_t ip; int port;
    int r = abi_addr_in(c->a[4], &ip, &port);
    if (r == -1) return -ABI_EFAULT;
    if (r == -2) return -ABI_EAFNOSUPPORT;
    return sys_sendto((int)c->a[0], (const void*)c->a[1], (size_t)c->a[2], ip, port);
}

static long h_recvfrom(struct abi_ctx* c) {
    uint32_t ip = 0; int port = 0;
    long n = sys_recvfrom_u((int)c->a[0], (uintptr_t)c->a[1], (size_t)c->a[2],
                            &ip, &port);
    if (n >= 0 && c->a[4]) abi_addr_out(c->a[4], c->a[5], ip, port);
    return n;
}

static long h_shutdown(struct abi_ctx* c) {
    return sys_shutdown((int)c->a[0], (int)c->a[1]);
}

/* No socket options are honoured, and reporting success is deliberate: musl's
 * getaddrinfo and every TLS setup set SO_RCVTIMEO / TCP_NODELAY and treat a
 * failure as fatal, while ignoring them costs at most a timeout that never
 * fires.  The day one of them changes behaviour, it stops being a stub. */
static long h_setsockopt(struct abi_ctx* c) { (void)c; return 0; }
static long h_getsockopt(struct abi_ctx* c) { (void)c; return 0; }

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
    [ABI_EPOLL_CREATE]    = { "epoll_create",    h_epoll_create    },
    [ABI_EPOLL_CTL]       = { "epoll_ctl",       h_epoll_ctl       },
    [ABI_EPOLL_WAIT]      = { "epoll_wait",      h_epoll_wait      },
    [ABI_SIGPENDING]      = { "sigpending",      h_sigpending      },
    [ABI_WAIT]            = { "wait",            h_wait },
    [ABI_EXECVE]          = { "execve",          h_execve },
    /* §M24 — the socket surface, shared by all three arches at once. */
    [ABI_SOCKET]       = { "socket",       h_socket       },
    [ABI_BIND]         = { "bind",         h_bind         },
    [ABI_CONNECT]      = { "connect",      h_connect      },
    [ABI_LISTEN]       = { "listen",       h_listen       },
    [ABI_ACCEPT]       = { "accept",       h_accept       },
    [ABI_ACCEPT4]      = { "accept4",      h_accept4      },
    [ABI_GETSOCKNAME]  = { "getsockname",  h_getsockname  },
    [ABI_GETPEERNAME]  = { "getpeername",  h_getpeername  },
    [ABI_SEND]         = { "send",         h_send         },
    [ABI_SENDTO]       = { "sendto",       h_sendto       },
    [ABI_RECV]         = { "recv",         h_recv         },
    [ABI_RECVFROM]     = { "recvfrom",     h_recvfrom     },
    [ABI_SHUTDOWN]     = { "shutdown",     h_shutdown     },
    [ABI_SETSOCKOPT]   = { "setsockopt",   h_setsockopt   },
    [ABI_GETSOCKOPT]   = { "getsockopt",   h_getsockopt   },
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
