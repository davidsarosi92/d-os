/* =============================================================================
 * usyscall.c — portable user-syscall handlers over the per-process fd table
 * (M25 stage 3/4).
 *
 * The arch-specific dispatchers (kernel/hal/<arch>/syscall.c) decode the
 * trapframe (int 0x80 / svc) into a number + args and call these.  The logic
 * — fd-table lookup, console routing for fds 0/1/2, VFS/shm dispatch, anon +
 * shared-memory mmap — is arch-neutral, so it lives here once.  fds 0/1/2 are
 * the implicit console stdin/stdout/stderr; fds >= 3 index task->fds (generic
 * ofile objects: VFS file / shm / socket).
 *
 * USER-POINTER DISCIPLINE (§1.1) — two layers, both required:
 *   1. GATE: while a task is inside a syscall entered from ring 3
 *      (task->in_user_syscall, set by the arch dispatcher) every pointer
 *      argument is checked with vmm_user_access_ok before use, so a bad pointer
 *      returns an error instead of faulting the kernel or reaching kernel
 *      memory.  In-kernel callers of the same handlers (the shell self-tests)
 *      are not gated — see user_ptr_gate_armed below.
 *   2. FAULT FIXUP: the copies themselves run through the uaccess primitives
 *      (kernel/hal/<arch>/uaccess.c), whose instructions are registered in the
 *      exception table — so even a range that becomes invalid BETWEEN the check
 *      and the copy (a concurrent munmap, a revoked COW page) returns -EFAULT
 *      instead of taking a ring-0 page fault.
 *   3. BOUNCE BUFFERS: the bulk payloads (read/write/send/recv/getdents/poll/
 *      getrandom) are never handed to the VFS / socket / console layers as raw
 *      ring-3 pointers.  Those layers dereference the buffer deep inside their
 *      own call chains — far away from any exception-table-registered
 *      instruction — so layer 2 could not cover them: a range that went bad
 *      mid-transfer would still take a ring-0 #PF and (with the default
 *      kernel.fault_policy = halt) kill the box.  The gated wrapper now copies
 *      through a kernel chunk in both directions and calls a *_k core that only
 *      ever sees kernel memory.  The copies themselves are layer-2 protected,
 *      so a concurrent munmap turns into a short count or -EFAULT.
 * ============================================================================= */

#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "netlink.h"   /* §M90 */
#include "fd.h"
#include "timerfd.h"
#include "eventfd.h"       /* §M53 stage 3 — timer descriptors */
#include "epoll.h"          /* §M56 — readiness sets            */
#include "vmm.h"
#include "pcache.h"
#include "printf.h"
#include "audit.h"     /* §M71 — the boundary audit registers here */
#include "uaccess.h"   /* §1.1 — fault-safe user copies (exception table) */
#include "pmm.h"
#include "kmap.h"
#include "console.h"
#include "vc.h"
#include "waitq.h"
#include "net.h"
#include "hal_api.h"
#include "kmalloc.h"
#include "rtc.h"
#include "timer.h"
#include "ktimer.h"
#include "random.h"
#include "vma.h"
#include "lock.h"
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE          4096u

/* §M46/security — validated user<->kernel copies (see vmm.h).  Gate every ring-3
 * pointer through these so a bad pointer returns -1 (→ -EFAULT) instead of a
 * kernel-mode #PF (which the fault policy would turn into a whole-box halt — a
 * package freezing the system) or a read/write of kernel memory. */
int copy_from_user(void* dst, uintptr_t user_src, uintptr_t len) {
    if (!vmm_user_access_ok(user_src, len, 0)) return -1;   /* cheap pre-check */
    return uaccess_copy_in(dst, user_src, (size_t)len);     /* fault-safe copy */
}
int copy_to_user(uintptr_t user_dst, const void* src, uintptr_t len) {
    if (!vmm_user_access_ok(user_dst, len, 1)) return -1;
    return uaccess_copy_out(user_dst, src, (size_t)len);
}

/* Is the current caller's pointer argument a RING-3 pointer at all?
 *
 * The sys_* helpers below are DUAL-USE: an arch dispatcher calls them with raw
 * ring-3 pointers, but in-kernel code (the shell's self-tests, and drivers that
 * reuse the fd/socket layer) calls the very same functions with KERNEL buffers.
 * Only the dispatcher knows which case it is — it entered from a ring-3 trap —
 * so it flags the task for the duration (task->in_user_syscall) and we validate
 * only then.  Gating unconditionally rejected every in-kernel caller, which is
 * what broke fdtest/socktest/polltest and — through ld.so's fstat of each
 * shared object — NetSurf.  Kernel-pointer arguments coming FROM a dispatcher
 * that also handles user pointers (e.g. fstat into a kernel `struct kstat`) use
 * the explicit *_k cores instead. */
static inline int user_ptr_gate_armed(void) {
    struct task* t = task_current();
    return t && t->in_user_syscall;
}

/* Convenience predicates for gating a syscall's user pointer(s). */
static inline int user_r(const void* p, uintptr_t len) {
    if (!user_ptr_gate_armed()) return 1;
    return vmm_user_access_ok((uintptr_t)p, len, 0);
}
static inline int user_w(const void* p, uintptr_t len) {
    if (!user_ptr_gate_armed()) return 1;
    return vmm_user_access_ok((uintptr_t)p, len, 1);
}

/* Copy a NUL-terminated string from a user pointer into a kernel buffer, at most
 * `max` bytes (always NUL-terminates).  Validates page-by-page so a bad/unmapped
 * string can't fault the kernel.  Returns the length copied, or -1 on a bad
 * pointer.  Used for path arguments before handing them to the VFS. */
static int strncpy_from_user(char* dst, const char* uptr, size_t max) {
    if (max == 0 || !uptr) return -1;
    uintptr_t base = (uintptr_t)uptr;
    if (!user_ptr_gate_armed()) {              /* kernel caller → plain copy */
        size_t i = 0;
        for (; i + 1 < max && uptr[i]; i++) dst[i] = uptr[i];
        dst[i] = 0;
        return (int)i;
    }
    /* Cheap pre-check on the first page (rejects the obvious bad pointer with a
     * clean error), then the FAULT-SAFE walk: uaccess_str_in stops at the NUL
     * and survives an unmapped page mid-string via the exception table, so a
     * string that straddles into unmapped memory returns -1 instead of faulting
     * the kernel — no need to pre-walk every page here. */
    if (!vmm_user_access_ok(base & ~(uintptr_t)0xFFF, 1, 0)) return -1;
    long n = uaccess_str_in(dst, base, max);
    return (n < 0) ? -1 : (int)n;
}

/* Public form of the above, for an arch dispatcher that must turn a ring-3 path
 * argument into a kernel string before calling a *_k core (see below). */
int copy_str_from_user(char* dst, uintptr_t user_src, uintptr_t max) {
    return strncpy_from_user(dst, (const char*)user_src, (size_t)max);
}

/* ---------------------------------------------------------------------------
 * BOUNCE BUFFERS (§1.1 layer 3 — see the file header).
 *
 * A kernel staging chunk for one bulk transfer.  Small transfers (the common
 * case: a printf, a 2-byte DNS length prefix, a one-line read) use an on-stack
 * array so the hot path never touches the allocator; anything bigger takes a
 * kmalloc'd chunk that is released by bounce_fini.  The chunk is deliberately
 * capped: a program asking to read 64 MiB must not make the kernel allocate
 * 64 MiB, so the wrapper loops over BOUNCE_CHUNK-sized pieces instead.
 *
 * Kernel stacks here are small, so BOUNCE_SMALL stays well under a page.
 * --------------------------------------------------------------------------- */
#define BOUNCE_CHUNK   4096u        /* max kernel staging chunk per iteration  */
#define BOUNCE_SMALL   192u         /* <= this rides the stack, no kmalloc     */

struct bounce {
    uint8_t  inln[BOUNCE_SMALL];
    uint8_t* buf;                   /* points at inln or at the kmalloc'd area */
    size_t   cap;
    int      heap;                  /* 1 → buf must be kfree'd                 */
};

/* Size the chunk for a transfer of `want` bytes.  Never fails: if kmalloc
 * cannot satisfy the request we fall back to the inline buffer and the caller
 * simply performs more (smaller) iterations. */
static void bounce_init(struct bounce* b, size_t want) {
    size_t cap = (want > BOUNCE_CHUNK) ? BOUNCE_CHUNK : want;
    if (cap <= BOUNCE_SMALL) {
        b->buf = b->inln; b->cap = (cap ? cap : 1); b->heap = 0;
        return;
    }
    void* p = kmalloc(cap);
    if (p) { b->buf = (uint8_t*)p; b->cap = cap; b->heap = 1; }
    else   { b->buf = b->inln;     b->cap = BOUNCE_SMALL; b->heap = 0; }
}
static void bounce_fini(struct bounce* b) {
    if (b->heap && b->buf) kfree(b->buf);
    b->buf = NULL; b->heap = 0;
}

/* May the read side keep looping to fill the caller's whole buffer?
 *
 * Only for a regular VFS file: those never block, so looping just returns more
 * data.  For a socket / pipe / stdin a short read is the CORRECT answer — the
 * first chunk is what was available — and looping for more would block a caller
 * that asked for a big buffer but only expected whatever had arrived.  That
 * distinction is why this is a per-fd question and not a global policy. */
/* fd_lookup is declared in fd.h — it stopped being private to this file when
 * epoll needed to resolve a descriptor without re-implementing the table. */
static int read_may_loop(int fd) {
    struct ofile* o = fd_lookup(fd);
    if (!o) return 0;                           /* console stdin is line-oriented */
    return (o->kind == FD_VFS);
}

/* SYS_PRINT — write a NUL-terminated RING-3 string to the console.
 *
 * §1.1: every arch dispatcher used to walk the raw user pointer ("the identity
 * map covers everything the user could hand us") — which is exactly how a
 * program passes an unmapped/kernel address and takes the whole box down with a
 * ring-0 #PF.  Copy it in through the validated string copy instead, in chunks
 * so an arbitrarily long string still works, with a hard cap so a non-NUL-
 * terminated one cannot spin forever.  Returns 0, or -1 on a bad pointer. */
long sys_print(const char* user_str) {
    if (!user_str) return -1;
    char buf[128];
    uintptr_t p = (uintptr_t)user_str;
    for (int chunk = 0; chunk < 1024; chunk++) {      /* ≤ 128 KiB of text */
        int n = strncpy_from_user(buf, (const char*)p, sizeof buf);
        if (n < 0) return -1;
        for (int i = 0; i < n; i++) console_putchar(buf[i]);
        /* strncpy_from_user returns the index of the NUL it found (< max-1), or
         * max-1 when it filled the buffer without seeing one → keep going. */
        if (n < (int)sizeof buf - 1) return 0;
        p += (uintptr_t)n;
    }
    return 0;
}

/* Resolve `fd` to its ofile, or NULL if out of range / not open.
 *
 * §M59 — 0/1/2 ARE TABLE SLOTS.  They used to be rejected outright, with the
 * console reached by number inside sys_read/sys_write, and the consequence was
 * not a missing corner but a missing FEATURE: `dup2(fd, 1)` returned -1, so
 * shell REDIRECTION could not work even in principle.  `sh -c "echo x > f"`
 * reported success and wrote to the terminal, which is how a redirection into
 * /dev/clipboard came to be reported as a clipboard bug.
 *
 * A NULL entry at 0/1/2 still means "the console": that is the DEFAULT, not a
 * special case, and it is what keeps every program that never redirects
 * working exactly as before. */
struct ofile* fd_lookup(int fd) {
    struct task* t = task_current();
    if (!t || fd < 0 || fd >= TASK_MAX_FDS) return NULL;
    return t->fds[fd];
}

/* ---- §M89: the shared descriptor table (contract in fd.h) ----------------- */
struct fdtable {
    int           refs;
    spinlock_t    lock;
    struct ofile* fd[TASK_MAX_FDS];
    uint32_t      cloexec;              /* §M90 — bit i: close fd[i] at exec */
};

/* §M90 — CLOSE-ON-EXEC.  This kernel had none: every descriptor survived
 * execve.  Mostly that only leaked, but one idiom depends on it absolutely —
 * Go (and posix_spawn in musl) learn whether a fork+exec WORKED from a pipe
 * whose write end is close-on-exec in the child: a successful exec closes it
 * and the parent reads EOF; a failure writes the errno into it.  Without
 * close-on-exec the parent waits for an EOF that never comes (dockerd starting
 * containerd).  One bit per slot, beside the table it describes; 32 slots, so
 * one word. */
_Static_assert(TASK_MAX_FDS <= 32, "fd_cloexec is one 32-bit mask");
static uint32_t* fd_cx(struct task* t) {
    return t->fdt ? &t->fdt->cloexec : &t->fd_cloexec_inline;
}

/* Lock the CURRENT task's table for a slot update: a no-op for a private
 * table (nobody else can see it), the table's lock for a shared one. */
static uint32_t fdt_lock(struct task* t) {
    return t->fdt ? spin_lock_irqsave(&t->fdt->lock) : 0;
}
static void fdt_unlock(struct task* t, uint32_t fl) {
    if (t->fdt) spin_unlock_irqrestore(&t->fdt->lock, fl);
}

struct fdtable* fdtable_share(struct task* parent) {
    if (!parent) return NULL;
    if (!parent->fdt) {
        struct fdtable* ft = (struct fdtable*)kcalloc(1, sizeof *ft);
        if (!ft) return NULL;
        spin_lock_init(&ft->lock);
        ft->refs = 1;                                  /* the parent's */
        /* MOVE, not copy: the references the inline slots held now belong to
         * the shared table.  Only the parent is running on its table at this
         * moment — it has no threads yet, by definition. */
        for (int i = 0; i < TASK_MAX_FDS; i++) { ft->fd[i] = parent->fds_inline[i]; parent->fds_inline[i] = NULL; }
        ft->cloexec = parent->fd_cloexec_inline;        /* the bits move with the slots */
        parent->fd_cloexec_inline = 0;
        parent->fdt = ft;
        parent->fds = ft->fd;
    }
    __atomic_add_fetch(&parent->fdt->refs, 1, __ATOMIC_ACQ_REL);
    return parent->fdt;
}

void fdtable_adopt(struct task* t, struct fdtable* ft) {
    if (!t || !ft) return;
    t->fdt = ft;
    t->fds = ft->fd;
}

void fdtable_put(struct fdtable* ft) {
    if (!ft || __atomic_sub_fetch(&ft->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    for (int i = 0; i < TASK_MAX_FDS; i++) if (ft->fd[i]) ofile_unref(ft->fd[i]);
    kfree(ft);
}

/* Install `o` in the lowest free real-fd slot (>= 3).  Consumes the reference
 * on success; on failure returns -1 (caller unrefs). */
static int fd_install(struct ofile* o) {
    struct task* t = task_current();
    if (!t) return -1;
    uint32_t fl = fdt_lock(t);
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
        if (!t->fds[fd]) {
            t->fds[fd] = o;
            *fd_cx(t) &= ~(1u << fd);           /* a new descriptor is inherited by exec */
            fdt_unlock(t, fl);
            return fd;
        }
    }
    fdt_unlock(t, fl);
    return -1;
}

/* Forward decls — FD_NETSOCK stream I/O (defined with the socket layer below). */
static long netsock_write(struct netsock* ns, const void* buf, size_t n);
static long netsock_read (struct netsock* ns, void* buf, size_t n);

/* Core: `buf` is always KERNEL memory (see the *_k note in syscall.h). */
long sys_write_k(int fd, const void* buf, size_t n) {
    /* A REDIRECTED std stream is just an ofile; the console is what a slot with
     * nothing in it means. */
    struct ofile* wo = fd_lookup(fd);
    if (((fd == 1 || fd == 2) && !wo) || (wo && wo->kind == FD_CONSOLE)) {
        const char* s = (const char*)buf;
        /* §M43: also capture into the task's buffer if one is set (Editor
         * "Compile & Run" reads it back), leaving room for a NUL terminator. */
        struct task* me = task_current();
        /* §M59 — the same serialisation kprintf has had since §M57.  Without
         * it a program's write and a kernel message on another CPU interleaved
         * CHARACTER BY CHARACTER ("wlclipin: it: rofeaped 'wl-server'..."),
         * which is §M57's shredded-log hazard on the path §M57 did not cover.
         * The payload is bounded by sys_write's staging chunk. */
        uint32_t cfl;
        int held = console_out_begin(&cfl);
        for (size_t i = 0; i < n; i++) {
            if (me && me->cap_buf && me->cap_len < me->cap_cap - 1)
                me->cap_buf[me->cap_len++] = s[i];
            console_putchar(s[i]);
        }
        console_out_end(cfl, held);
        if (me && me->cap_buf && me->cap_len < me->cap_cap)
            me->cap_buf[me->cap_len] = '\0';
        return (long)n;
    }
    if (fd == 0) return -1;                    /* can't write stdin */
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_VFS)  return (long)vfs_write(o->file, buf, n);
    /* §M90 — a STREAM write: blocks while the peer is full (EAGAIN when
     * non-blocking), EPIPE once it is gone.  It returned 0 when the ring was
     * full, which a writer can read as nothing at all. */
    if (o->kind == FD_SOCK) return usock_write(o->sock, buf, n, !o->nonblock);
    if (o->kind == FD_NETSOCK) return netsock_write(o->nsock, buf, n);
    if (o->kind == FD_EVENT) return eventfd_write(o->efd, buf, n, !o->nonblock);   /* §M90 */
    if (o->kind == FD_NETLINK) return nl_send(o->nl, buf, n);                      /* §M90 */
    return -1;                                 /* shm: not write(2)-able */
}

/* write(2) from a RING-3 buffer.  Stages the payload through a kernel chunk so
 * the console / VFS / socket sinks below only ever dereference kernel memory.
 * Short writes are honoured: if the sink takes less than we offered, that is
 * the caller's return value and we stop — exactly what write(2) promises. */
long sys_write(int fd, const void* buf, size_t n) {
    if (!user_ptr_gate_armed()) return sys_write_k(fd, buf, n);   /* kernel caller */
    if (n && !user_r(buf, n)) return -1;      /* §1.1 — validate the user buffer */
    if (n == 0) return sys_write_k(fd, buf, 0);

    struct bounce b; bounce_init(&b, n);
    uintptr_t src = (uintptr_t)buf;
    size_t done = 0;
    long rc = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > b.cap) chunk = b.cap;
        /* Fault-safe copy: a range that went bad since the pre-check (a
         * concurrent munmap on another CPU) returns an error, not a #PF. */
        if (copy_from_user(b.buf, src + done, chunk) != 0) { rc = -1; break; }
        long w = sys_write_k(fd, b.buf, chunk);
        if (w < 0) { rc = w; break; }
        done += (size_t)w;
        if ((size_t)w < chunk) break;          /* sink took less → short write */
    }
    bounce_fini(&b);
    return done ? (long)done : rc;             /* partial progress wins over the error */
}

/* Cooked line read from the focused virtual console — a minimal line-discipline
 * stdin (echo + backspace) so an interactive program (a musl `sh`) can read a
 * line from the keyboard.  Blocks on the vc input ring (vc_getchar) until Enter;
 * returns the line INCLUDING the trailing '\n', up to `cap` bytes. */
static long stdin_read_line(char* buf, size_t cap) {
    struct vc* v = vc_focused();
    if (!v || cap == 0) return 0;
    size_t len = 0;
    for (;;) {
        /* §1.5 — honour a pending kill so a process blocked in read(0) is
         * force-killable (End-task) even if no line ever arrives. */
        if (task_should_stop()) return (long)len;
        char c = vc_getchar(v);
        if (c == '\n') {
            vc_putchar(v, '\n');
            if (len < cap) buf[len++] = '\n';
            return (long)len;
        }
        if (c == '\b' || c == 127) {            /* backspace / DEL */
            if (len > 0) { len--; vc_putchar(v, '\b'); }
            continue;
        }
        if (len < cap) { buf[len++] = c; vc_putchar(v, c); }
    }
}

/* Core: `buf` is always KERNEL memory (see the *_k note in syscall.h). */
long sys_read_k(int fd, void* buf, size_t n) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind == FD_CONSOLE) {
        if (o || fd == 0) return stdin_read_line((char*)buf, n);   /* cooked stdin */
        return -1;                       /* console out is not readable */
    }
    if (o->kind == FD_VFS)  return (long)vfs_read(o->file, buf, n);
    /* Sockets read(2) with POSIX blocking semantics (block == 1): an empty
     * read waits for the peer to send (or close → 0/EOF). */
    /* §M56.1 — one flag, checked in one place.  `block` is the inverse of the
     * description's O_NONBLOCK; before this the socket layer carried its own
     * copy and every other kind ignored the flag entirely. */
    int block = !o->nonblock;
    if (o->kind == FD_SOCK) {
        long r = usock_recv(o->sock, buf, n, block, NULL);
        /* A non-blocking read that found nothing must say EAGAIN, not 0: zero
         * means END OF FILE, and a drain loop told "EOF" by an empty pipe
         * whose writer is very much alive stops for good. */
        if (!block && r == 0 && usock_peer_open(o->sock)) return -SOCK_EAGAIN;
        return r;
    }
    if (o->kind == FD_NETSOCK) return netsock_read(o->nsock, buf, n);
    /* §M53 stage 3 — reading a timerfd yields the expiration COUNT and resets
     * it.  Blocking by default, like every other read here; a caller that
     * wants the non-blocking form uses poll(2), which is the whole point of
     * the descriptor existing. */
    if (o->kind == FD_EVENT) return eventfd_read(o->efd, buf, n, block);   /* §M90 */
    if (o->kind == FD_NETLINK) return nl_recv(o->nl, buf, n, block, 0, 0); /* §M90 */
    if (o->kind == FD_TIMER) {
        long r = timerfd_read(o->tfd, buf, n, block);
        /* Reading a timerfd RESETS its expiration count, so it stops being
         * readable — the mirror image of the expiry that made it readable.
         * A poller holding a cached "readable" would otherwise spin. */
        if (r > 0) fd_readiness_changed(o);
        return r;
    }
    return -1;
}

/* read(2) into a RING-3 buffer — the mirror image of sys_write above.  The
 * source fills a kernel chunk, which is then copied out.  If the copy-out
 * fails after some bytes have already been delivered we return the short count
 * rather than -EFAULT: the data is gone from the file/socket either way, and a
 * short read is a result the caller must already handle. */
long sys_read(int fd, void* buf, size_t n) {
    if (!user_ptr_gate_armed()) return sys_read_k(fd, buf, n);    /* kernel caller */
    if (n && !user_w(buf, n)) return -1;      /* §1.1 — validate the user buffer */
    if (n == 0) return 0;

    int loop = read_may_loop(fd);
    struct bounce b; bounce_init(&b, n);
    uintptr_t dst = (uintptr_t)buf;
    size_t done = 0;
    long rc = 0;
    do {
        size_t chunk = n - done;
        if (chunk > b.cap) chunk = b.cap;
        long r = sys_read_k(fd, b.buf, chunk);
        if (r < 0) { rc = r; break; }
        if (r == 0) break;                                  /* EOF */
        if (copy_to_user(dst + done, b.buf, (size_t)r) != 0) { rc = -1; break; }
        done += (size_t)r;
        if ((size_t)r < chunk) break;                        /* short read → done */
    } while (loop && done < n);
    bounce_fini(&b);
    return done ? (long)done : rc;
}

int sys_open(const char* path, int flags) {
    if (!path) return -1;
    char kpath[256];                                        /* §1.1 */
    if (strncpy_from_user(kpath, path, sizeof kpath) < 0) return -1;
    struct file* f = vfs_open(kpath, flags ? flags : VFS_RDONLY);
    if (!f) return -1;
    struct ofile* o = ofile_from_file(f);
    if (!o) { vfs_close(f); return -1; }
    int fd = fd_install(o);
    if (fd < 0) { ofile_unref(o); return -1; }
    return fd;
}

/* §M73 — the directory calls ring 3 never had.  Each copies its path(s) in
 * first (§1.1) and answers 0 or the negative error the VFS's refusal means:
 * -1 not found, -2 exists, -5 not permitted (the VFS's own codes). */
int sys_mkdir(const char* upath, int mode) {
    char kp[256];
    if (!upath || strncpy_from_user(kp, upath, sizeof kp) < 0) return -1;
    int r = vfs_mkdir(kp);
    if (r == 0 && mode > 0) vfs_chmod(kp, (uint32_t)mode & 07777u);
    return r;
}
int sys_link(const char* uold, const char* unew) {
    char ko[256], kn[256];
    if (!uold || !unew || strncpy_from_user(ko, uold, sizeof ko) < 0 ||
        strncpy_from_user(kn, unew, sizeof kn) < 0) return -1;
    return vfs_link(ko, kn);
}
int sys_symlink(const char* utarget, const char* ulink) {
    char kt[256], kl[256];
    if (!utarget || !ulink || strncpy_from_user(kt, utarget, sizeof kt) < 0 ||
        strncpy_from_user(kl, ulink, sizeof kl) < 0) return -1;
    return vfs_symlink(kt, kl);
}
int sys_chmod(const char* upath, int mode) {
    char kp[256];
    if (!upath || strncpy_from_user(kp, upath, sizeof kp) < 0) return -1;
    return vfs_chmod(kp, (uint32_t)mode & 07777u);
}
/* §M90 — chown: owner and/or group of a path (-1 = leave as is).  The VFS
 * applies the policy (admin only, see vfs_chown) and says which refusal it
 * was: -1 no such path, -2 not permitted, -3 the volume refused to persist. */
int sys_chown(const char* upath, int uid, int gid) {
    char kp[256];
    if (!upath || strncpy_from_user(kp, upath, sizeof kp) < 0) return -1;
    return vfs_chown(kp, uid, gid);
}
int sys_unlink(const char* upath) {
    char kp[256];
    if (!upath || strncpy_from_user(kp, upath, sizeof kp) < 0) return -1;
    return vfs_unlink(kp);
}

int sys_close(int fd) {
    struct ofile* o = fd_lookup(fd);
    if (!o) {
        /* Closing a std stream that was never redirected is a no-op rather than
         * an error: the console has no object to release, and a shell closes
         * descriptors it did not open all the time. */
        if (fd >= 0 && fd <= 2) return 0;
        return -1;
    }
    struct task* t = task_current();
    uint32_t fl = fdt_lock(t);
    if (t->fds[fd] != o) { fdt_unlock(t, fl); return -1; }   /* another thread closed it */
    t->fds[fd] = NULL;
    *fd_cx(t) &= ~(1u << fd);
    fdt_unlock(t, fl);
    ofile_unref(o);
    return 0;
}

long sys_lseek(int fd, long off, int whence) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_VFS) return -1;
    struct file* f = o->file;
    uint64_t base;
    switch (whence) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = f->pos; break;
        case SEEK_END: base = f->inode ? f->inode->size : 0; break;
        default: return -1;
    }
    long np = (long)base + off;
    if (np < 0) return -1;
    f->pos = (uint64_t)np;
    return np;
}

/* Map memory into the calling task's user space: `fd < 0` → a fresh anonymous
 * region; otherwise map the shared-memory object behind `fd`.  Returns the
 * user VA (bump-allocated from the ADDRESS SPACE's cursor) or -1. */
long sys_mmap(size_t len, int fd) {
    struct task* t = task_current();
    if (!t || !t->mm) return -1;

    int n = (int)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    if (n <= 0) n = 1;
    /* §M89 — the address comes from the reservation set, so a native program
     * and a Linux-ABI one in the same space can never be handed one range. */
    uintptr_t va = vma_reserve_eager(t->mm, (size_t)n, 3 /* read|write */);
    if (!va) return -1;

    if (fd < 0) {
        for (int i = 0; i < n; i++) {
            pmm_phys_t fr = pmm_alloc_frame_user();   /* §M86 — may be highmem */
            if (!fr) return -1;
            kmap_zero_frame(fr);
            if (vmm_space_map(t->mm, va + (uintptr_t)i * PAGE_SIZE, fr,
                              VMM_USER | VMM_WRITABLE) != 0) {
                pmm_free_frame(fr);
                return -1;
            }
        }
    } else {
        struct ofile* o = fd_lookup(fd);
        if (!o || o->kind != FD_SHM || !o->shm) return -1;
        struct shm* s = o->shm;
        int cnt = n < s->nframes ? n : s->nframes;
        for (int i = 0; i < cnt; i++) {
            /* VMM_SHARED: the shm object owns these frames, so the space's
             * teardown must not free them. */
            if (vmm_space_map(t->mm, va + (uintptr_t)i * PAGE_SIZE, s->frames[i],
                              VMM_USER | VMM_WRITABLE | VMM_SHARED) != 0)
                return -1;
        }
        n = cnt;
    }
    return (long)va;
}

/* §M37 → §M89 — mmap / munmap / mprotect for the Linux ABI are the vma
 * layer's (kernel/mem/vma.c): reservations, demand-zero anonymous pages, true
 * PROT_NONE, lazily filled private file mappings, and addresses that are
 * reused after munmap.  These wrappers keep the names every caller knows.
 * Results are an address or a NEGATIVE ERRNO. */
long sys_mmap_full(uintptr_t addr, size_t len, int prot, int flags,
                   int fd, uint64_t offset) {
    return vma_mmap(addr, len, prot, flags, fd, offset);
}

long sys_munmap(uintptr_t addr, size_t len) { return vma_munmap(addr, len); }

long sys_mprotect(uintptr_t addr, size_t len, int prot) { return vma_mprotect(addr, len, prot); }

int sys_memfd(size_t size) {
    struct shm* s = shm_create(size);
    if (!s) return -1;
    struct ofile* o = ofile_from_shm(s);   /* takes its own ref */
    shm_unref(s);                          /* drop our create ref → ofile owns it */
    if (!o) return -1;
    int fd = fd_install(o);
    if (fd < 0) { ofile_unref(o); return -1; }
    return fd;
}

/* §M40 — ftruncate() on a memfd.  Linux's memfd_create hands back a zero-length
 * object and the caller sizes it with ftruncate; a Wayland client does exactly
 * that before passing the fd to wl_shm_create_pool. */
int sys_memfd_resize(int fd, size_t size) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_SHM || !o->shm) return -1;
    return shm_grow(o->shm, size);
}

/* ---- unix sockets + fd passing (stage 5) ---------------------------------- */

int sys_socketpair(int* fds) {
    if (!fds || !user_w(fds, 2 * sizeof(int))) return -1;   /* §1.1/2.4 */
    return sys_socketpair_k(fds);
}

/* The core, for a caller whose `fds` is a KERNEL array (the §M50 pipe handler
 * copies the pair out itself).  §M46's rule: the check belongs where the
 * pointer's origin is known, so the gated wrapper above checks and this does
 * not — calling the wrapper with a kernel array from inside a ring-3 syscall
 * is refused, which is how musl's pipe() came to fail on i386 (§M59). */
int sys_socketpair_k(int* fds) {
    struct usock *ua, *ub;
    if (usock_pair(&ua, &ub) != 0) return -1;

    struct ofile *oa = ofile_from_sock(ua), *ob = ofile_from_sock(ub);
    if (!oa || !ob) {                          /* OOM — tear the pair back down */
        if (oa) ofile_unref(oa); else usock_close(ua);
        if (ob) ofile_unref(ob); else usock_close(ub);
        return -1;
    }
    int a = fd_install(oa);
    int b = fd_install(ob);
    if (a < 0 || b < 0) {
        if (a >= 0) sys_close(a); else ofile_unref(oa);
        if (b >= 0) sys_close(b); else ofile_unref(ob);
        return -1;
    }
    fds[0] = a; fds[1] = b;
    return 0;
}

/* M34 — pipe(fds): a connected byte channel.  Backed by the same usock ring
 * as socketpair (bidirectional under the hood); fds[0] is the read end and
 * fds[1] the write end by convention.  Inherited across fork (ofile refs),
 * so the classic "child writes, parent reads" works. */
int sys_pipe(int* fds) {
    return sys_socketpair(fds);
}
int sys_pipe_k(int* fds) {
    return sys_socketpair_k(fds);
}

/* M34 — dup2(oldfd, newfd): make newfd refer to oldfd's object (closing any
 * prior newfd).  Real fds only (>= 3); the std streams have no ofile yet. */
int sys_dup2(int oldfd, int newfd) {
    if (oldfd == newfd) return (fd_lookup(oldfd) || (oldfd >= 0 && oldfd <= 2)) ? newfd : -1;
    int fresh;
    struct ofile* o = fd_dup_source(oldfd, &fresh);
    if (!o) return -1;
    /* 0/1/2 included: `dup2(fd, 1)` is exactly how every shell implements `>`.
     * Refusing it is what made redirection impossible (see fd_lookup). */
    if (newfd < 0 || newfd >= TASK_MAX_FDS) { if (fresh) ofile_unref(o); return -1; }
    struct task* t = task_current();
    struct ofile* nw = fresh ? o : ofile_ref(o);
    uint32_t fl = fdt_lock(t);
    struct ofile* old = t->fds[newfd];
    t->fds[newfd] = nw;
    *fd_cx(t) &= ~(1u << newfd);             /* POSIX: dup2 clears FD_CLOEXEC */
    fdt_unlock(t, fl);
    if (old) ofile_unref(old);               /* outside the lock: may close a file */
    return newfd;
}

/* §M40 — dup the descriptor into the lowest free slot >= `minfd` (POSIX
 * F_DUPFD / F_DUPFD_CLOEXEC).  Returns the new fd, or -1.
 *
 * This is not an obscure corner: libwayland DUPLICATES every descriptor it is
 * asked to send (wl_closure_marshal → wl_os_dupfd_cloexec), so without it a
 * Wayland client cannot pass its shm pool at all.  Worse, an fcntl that
 * "succeeds" by returning 0 is indistinguishable from a successful dup to fd 0,
 * so the failure was completely silent — the pool arrived carrying descriptor
 * zero.  Unimplemented commands that yield a DESCRIPTOR must fail loudly. */
int sys_dupfd(int fd, int minfd) {
    int fresh;
    struct ofile* o = fd_dup_source(fd, &fresh);   /* an empty std slot → the console */
    if (!o) return -1;
    struct task* t = task_current();
    if (!t) { if (fresh) ofile_unref(o); return -1; }
    /* 0/1/2 are RESERVED for the console and deliberately absent from the table
     * (fd_lookup rejects them, fd_install starts at 3).  A dup must obey the
     * same convention: handing back 0 produces a descriptor that looks valid to
     * the caller and can never be looked up again — which is precisely how a
     * Wayland client ended up passing descriptor zero to the compositor. */
    if (minfd < 3) minfd = 3;
    struct ofile* nw = fresh ? o : ofile_ref(o);
    uint32_t fl = fdt_lock(t);
    for (int i = minfd; i < TASK_MAX_FDS; i++) {
        if (t->fds[i]) continue;
        t->fds[i] = nw;
        *fd_cx(t) &= ~(1u << i);
        fdt_unlock(t, fl);
        return i;
    }
    fdt_unlock(t, fl);
    ofile_unref(nw);
    return -1;
}

/* M34 — kill(pid, sig): post `sig` to task `pid`.  Delivery happens when that
 * task next returns to user mode (hal/x86/signal.c).  A task blocked in a
 * syscall won't notice until it returns (no EINTR yet — a follow-up). */
int sys_kill(int pid, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    struct task* t = task_find(pid);
    if (!t) return -1;
    /* §audit#6 — credential rule (pre-§M32, no uid/gid yet): a ring-3 caller may
     * only signal a USER task that is itself or one of its descendants — never a
     * kernel thread, pid 0, or init.  Without this, any package could kill the
     * system's daemons (or another package) by pid. */
    struct task* me = task_current();
    if (me && me->user_task) {
        if (!t->user_task || t->pid == 0 || t->pid == task_reaper_pid()) return -1;
        if (t != me) {
            int p = t->ppid, ok = 0;
            for (int i = 0; i < 64 && p > 0; i++) {   /* walk t's ppid chain up */
                if (p == me->pid) { ok = 1; break; }
                struct task* pt = task_find(p);
                if (!pt) break;
                p = pt->ppid;
            }
            if (!ok) return -1;
        }
    }
    /* §M72 — STOP and CONT are not DELIVERED, they are ACTED ON: neither
     * runs a handler (STOP cannot be caught or ignored, and CONT's effect is
     * the resumption itself), so they go straight to the scheduler.  They
     * used to be named in all three signal.c files and implemented nowhere:
     * a SIGSTOP set a pending bit whose default action was to terminate. */
    if (sig == SIGSTOP || sig == SIGTSTP) return task_stop(pid) == 0 ? 0 : -1;
    if (sig == SIGCONT) { task_cont(pid); return 0; }
    t->sig_pending |= (1u << sig);
    return 0;
}

/* M34 — sigaction(sig, handler, restorer): set the disposition of `sig` and
 * remember the libc SYS_SIGRETURN trampoline.  Returns the previous handler. */
long sys_sigaction(int sig, long handler, long restorer) {
    struct task* t = task_current();
    if (!t || sig <= 0 || sig >= NSIG) return -1;
    long old = (long)t->sig_handler[sig];
    t->sig_handler[sig] = (uintptr_t)handler;
    if (restorer) t->sig_restorer = (uintptr_t)restorer;
    return old;
}

/* ---- POSIX syscall breadth (M36) — the surface a real libc needs ---------- */

/* ---------------------------------------------------------------------------
 * *_k cores — KERNEL-pointer entry points.
 *
 * A dispatcher that translates a d-os result into a foreign ABI layout (the
 * Linux personality: kstat → struct stat64, ktimespec → timespec/timeval,
 * source ip/port → sockaddr_in) fills a KERNEL struct first and marshals it out
 * itself.  Those calls must NOT be gated as ring-3 pointers — but the gated
 * wrapper is still the only thing a real user program can reach, so the ring-3
 * boundary stays closed.  The wrappers below are the ring-3 entry points; these
 * cores are the in-kernel API.  (`kpath` is likewise a kernel string — the
 * caller copies the user path in with copy_str_from_user first.)
 * ------------------------------------------------------------------------- */
int sys_stat_k(const char* kpath, struct kstat* out) {
    if (!kpath || !out) return -1;
    struct file* f = vfs_open(kpath, VFS_RDONLY);
    if (!f) return -1;
    if (f->inode) {
        out->size = (uint32_t)f->inode->size;
        out->type = (int)f->inode->type;
        out->mode = (f->inode->type == INODE_DIR) ? 0755 : 0644;
    } else { out->size = 0; out->type = 0; out->mode = 0644; }
    vfs_close(f);
    return 0;
}

/* §M73 — the full answer, from the INODE rather than from an open (a stat
 * needs no read permission on the file itself, only the lookup). */
static void stat_full_of(const struct inode* in, struct kstat_full* o) {
    o->size  = in->size;
    o->ino   = (uint64_t)((uintptr_t)in >> 3);
    o->nlink = in->type == INODE_DIR ? 2 : 1;          /* ramfs keeps the true count private */
    uint32_t perm = in->mode ? (in->mode & 07777u) : (in->type == INODE_DIR ? 0755u : 0644u);
    uint32_t fmt  = in->type == INODE_DIR ? KS_IFDIR
                  : in->type == INODE_DEVICE ? KS_IFCHR
                  : in->type == INODE_SYMLINK ? KS_IFLNK : KS_IFREG;
    if (in->type == INODE_SYMLINK) perm = 0777u;
    o->mode = fmt | perm;
    o->uid  = in->owner_uid < 0 ? 0 : in->owner_uid;
    o->gid  = in->owner_gid < 0 ? 0 : in->owner_gid;
}
int sys_stat_full_k(const char* kpath, struct kstat_full* out) {
    if (!kpath || !out) return -1;
    struct dentry* d = vfs_resolve(kpath);
    if (!d || !d->inode) return -1;
    stat_full_of(d->inode, out);
    return 0;
}
/* §M89 — lstat: the link itself when the last component is one. */
int sys_lstat_full_k(const char* kpath, struct kstat_full* out) {
    if (!kpath || !out) return -1;
    struct dentry* d = vfs_resolve_nofollow(kpath);
    if (!d || !d->inode) return -1;
    stat_full_of(d->inode, out);
    return 0;
}
int sys_fstat_full_k(int fd, struct kstat_full* out) {
    if (!out) return -1;
    for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t*)out)[i] = 0;
    struct ofile* o = fd_lookup(fd);
    if (!o) {
        if (fd >= 0 && fd <= 2) { out->mode = KS_IFCHR | 0620u; out->nlink = 1; return 0; }  /* the console */
        return -1;
    }
    out->nlink = 1;
    out->ino   = (uint64_t)((uintptr_t)o >> 3);
    switch (o->kind) {
    case FD_VFS:
        if (o->file && o->file->inode) { stat_full_of(o->file->inode, out); return 0; }
        out->mode = KS_IFREG | 0644u; return 0;
    case FD_SOCK: case FD_NETSOCK: out->mode = KS_IFSOCK | 0777u; return 0;
    case FD_CONSOLE:               out->mode = KS_IFCHR | 0620u;  return 0;
    default:                       out->mode = KS_IFREG | 0600u; return 0;   /* shm, timer, epoll */
    }
}

int sys_fstat_k(int fd, struct kstat* out) {
    if (!out) return -1;
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_VFS && o->file && o->file->inode) {
        out->size = (uint32_t)o->file->inode->size;
        out->type = (int)o->file->inode->type;
        out->mode = (o->file->inode->type == INODE_DIR) ? 0755 : 0644;
    } else { out->size = 0; out->type = 0; out->mode = 0644; }
    return 0;
}

/* Ring-3 entry points: gate the user pointers, then run the core. */
int sys_stat(const char* path, struct kstat* out) {
    if (!path || !out || !user_w(out, sizeof(*out))) return -1;   /* §1.1 */
    char kpath[256];
    if (strncpy_from_user(kpath, path, sizeof kpath) < 0) return -1;
    return sys_stat_k(kpath, out);
}

int sys_fstat(int fd, struct kstat* out) {
    if (!out || !user_w(out, sizeof(*out))) return -1;   /* §1.1 */
    return sys_fstat_k(fd, out);
}

/* Pack directory entries into `buf` as [reclen(2) | type(1) | name\0] records.
 * Core: `buf` is always KERNEL memory. */
long sys_getdents_k(int fd, void* buf, size_t cap) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_VFS || !o->file) return -1;
    uint8_t* out = (uint8_t*)buf;
    size_t used = 0;
    struct dirent de;
    while (vfs_readdir(o->file, &de) > 0) {
        int nlen = 0; while (de.name[nlen]) nlen++;
        size_t reclen = 2 + 1 + (size_t)nlen + 1;
        if (used + reclen > cap) break;
        out[used]     = (uint8_t)(reclen & 0xFF);
        out[used + 1] = (uint8_t)(reclen >> 8);
        out[used + 2] = (uint8_t)de.type;
        for (int i = 0; i < nlen; i++) out[used + 3 + i] = (uint8_t)de.name[i];
        out[used + 3 + nlen] = 0;
        used += reclen;
    }
    return (long)used;
}

/* getdents(2)/getdents64(2) into a RING-3 buffer.
 *
 * Records are packed into a kernel chunk and copied out in one go.  A caller
 * offering more than BOUNCE_CHUNK simply gets the entries that fit in the
 * chunk — legal for both calls, and the caller is already looping until the
 * result is 0.  Directory position has advanced by then, so a failed copy-out
 * costs those entries; that is the -EFAULT case and the program is buggy. */
static long getdents_bounced(int fd, void* buf, size_t cap,
                             long (*core)(int, void*, size_t)) {
    if (!buf || !cap) return -1;
    if (!user_ptr_gate_armed()) return core(fd, buf, cap);        /* kernel caller */
    if (!user_w(buf, cap)) return -1;                             /* §1.1/4.4 */

    struct bounce b; bounce_init(&b, cap);
    long used = core(fd, b.buf, b.cap);
    if (used > 0 && copy_to_user((uintptr_t)buf, b.buf, (size_t)used) != 0) used = -1;
    bounce_fini(&b);
    return used;
}

long sys_getdents(int fd, void* buf, size_t cap) {
    return getdents_bounced(fd, buf, cap, sys_getdents_k);
}

/* Linux getdents64 packing (for the Linux-ABI backend, kernel/hal/x86/
 * linux_abi.c — musl's readdir uses SYS_getdents64).  Same VFS iteration as
 * sys_getdents, but emits the Linux `struct linux_dirent64` layout:
 *   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[] (NUL-term).
 * Records are 8-byte aligned; d_type uses the Linux DT_* values. */
long sys_getdents64_k(int fd, void* buf, size_t cap) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_VFS || !o->file) return -1;
    uint8_t* out = (uint8_t*)buf;
    size_t used = 0;
    uint64_t ino = 1;
    struct dirent de;
    while (vfs_readdir(o->file, &de) > 0) {
        int nlen = 0; while (de.name[nlen]) nlen++;
        size_t reclen = 19 + (size_t)nlen + 1;
        reclen = (reclen + 7) & ~(size_t)7;                 /* 8-byte align */
        if (used + reclen > cap) break;
        uint8_t* r = out + used;
        for (int i = 0; i < 8; i++) r[i]     = (uint8_t)(ino >> (8 * i));       /* d_ino  */
        uint64_t off = used + reclen;
        for (int i = 0; i < 8; i++) r[8 + i] = (uint8_t)(off >> (8 * i));       /* d_off  */
        r[16] = (uint8_t)(reclen & 0xFF);                                        /* d_reclen */
        r[17] = (uint8_t)(reclen >> 8);
        r[18] = (de.type == INODE_DIR) ? 4 :                                     /* DT_DIR  */
                (de.type == INODE_DEVICE) ? 2 :                                  /* DT_CHR  */
                (de.type == INODE_SYMLINK) ? 10 : 8;                             /* DT_LNK / DT_REG */
        for (int i = 0; i < nlen; i++) r[19 + i] = (uint8_t)de.name[i];          /* d_name  */
        r[19 + nlen] = 0;
        used += reclen;
        ino++;
    }
    return (long)used;
}

long sys_getdents64(int fd, void* buf, size_t cap) {
    return getdents_bounced(fd, buf, cap, sys_getdents64_k);
}

static void ustr(char* d, const char* s) {
    int i = 0; while (s[i] && i < 64) { d[i] = s[i]; i++; } d[i] = 0;
}
int sys_uname(struct kutsname* out) {
    if (!out || !user_w(out, sizeof(*out))) return -1;   /* §1.1 */
    ustr(out->sysname,  "d-os");
    ustr(out->nodename, "d-os");
    ustr(out->release,  "0.1");
    ustr(out->version,  "M36 userland");
    /* The REAL architecture, from the one place that knows it.  This used to be
     * hardcoded "i386", so `uname -m` lied on x86_64 and aarch64 — and a libc
     * or build script that branches on it would have made the wrong choice. */
    ustr(out->machine,  hal_arch_name());
    return 0;
}

static uint32_t rtc_to_epoch(const struct rtc_time* t) {
    static const int mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    long days = 0;
    for (int y = 1970; y < (int)t->year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) days += 1;
    }
    for (int m = 1; m < (int)t->month; m++) {
        days += mdays[m - 1];
        if (m == 2 && (((int)t->year % 4 == 0 && (int)t->year % 100 != 0) ||
                       (int)t->year % 400 == 0)) days += 1;
    }
    days += (int)t->day - 1;
    return (uint32_t)(days * 86400L + (long)t->hour * 3600L +
                      (long)t->min * 60L + (long)t->sec);
}

int sys_clock_gettime_k(int which, struct ktimespec* out) {
    if (!out) return -1;
    if (which == CLOCK_MONOTONIC) {
        /* §M53 — nanoseconds from the real clock source, not the 1 ms tick.
         * The tick version returned the same value for every call inside a
         * millisecond, so two timestamps taken microseconds apart compared
         * EQUAL and any duration measured across it was quantised to ±1 ms. */
        uint64_t ns = timer_now_ns();
        out->sec  = (uint32_t)(ns / 1000000000ull);
        out->nsec = (uint32_t)(ns % 1000000000ull);
        return 0;
    }
    /* CLOCK_REALTIME: the RTC gives whole seconds only, so the sub-second part
     * comes from the monotonic clock.  That makes successive reads strictly
     * increasing (which callers assume) at the cost of the fraction not being
     * phase-aligned to the RTC's own second boundary — a trade every kernel
     * makes until it grows an NTP-style discipline. */
    struct rtc_time t;
    if (rtc_read(&t) != 0) { out->sec = 0; out->nsec = 0; return 0; }
    out->sec  = rtc_to_epoch(&t);
    out->nsec = (uint32_t)(timer_now_ns() % 1000000000ull);
    return 0;
}

int sys_clock_gettime(int which, struct ktimespec* out) {
    if (!out || !user_w(out, sizeof(*out))) return -1;   /* §1.1 */
    return sys_clock_gettime_k(which, out);
}

int sys_nanosleep(unsigned ms) {
    task_msleep(ms);
    return 0;
}

/* §M53 — clock_nanosleep(clock, flags, request).
 *
 * `abs_time` selects the two POSIX behaviours, and the difference is not
 * cosmetic: a RELATIVE sleep restarted after a signal drifts (each restart
 * re-measures from "now", so the total is longer than asked), while an
 * ABSOLUTE one does not.  Every periodic loop that must not drift — a frame
 * pump, a poller, a libc's own timing helpers — is written against the
 * absolute form, which is why it exists at all.
 *
 * `ns` is the deadline when abs_time, otherwise the delay.  Returns 0 on
 * completion, -EINTR-shaped -1 if the task was asked to stop. */
long sys_clock_nanosleep_ns(int which, int abs_time, uint64_t ns) {
    (void)which;   /* MONOTONIC and REALTIME share one timeline here */
    uint64_t deadline = abs_time ? ns : timer_now_ns() + ns;
    return task_sleep_until_ns(deadline);
}

/* §M39 — getrandom(buf, n, flags): fill `buf` with CSPRNG bytes.  Never blocks
 * (our pool is seeded at boot); flags (GRND_NONBLOCK/GRND_RANDOM) are ignored. */
long sys_getrandom(void* buf, size_t n, unsigned flags) {
    (void)flags;
    if (!buf) return -1;
    if (!user_ptr_gate_armed()) { random_bytes(buf, n); return (long)n; }
    if (!user_w(buf, n)) return -1;   /* §1.1 */

    /* Generate into a kernel chunk and copy out — random_bytes() writes through
     * the pointer itself, so a ring-3 buffer must never reach it. */
    struct bounce b; bounce_init(&b, n);
    size_t done = 0;
    long rc = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > b.cap) chunk = b.cap;
        random_bytes(b.buf, chunk);
        if (copy_to_user((uintptr_t)buf + done, b.buf, chunk) != 0) { rc = -1; break; }
        done += chunk;
    }
    bounce_fini(&b);
    return done ? (long)done : rc;
}

/* Core: `buf` is always KERNEL memory (see the *_k note in syscall.h). */
long sys_send_k(int fd, const void* buf, size_t n, int passfd) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_SOCK) return -1;
    struct ofile* pf = (passfd >= 0) ? fd_lookup(passfd) : NULL;
    return usock_send(o->sock, buf, n, pf);
}

/* send(2) on a unix socket from a RING-3 buffer.  Unlike write(2) this carries
 * an optional passed fd, which belongs to the FIRST chunk only — so once a
 * chunk has gone out the descriptor must not travel again. */
long sys_send(int fd, const void* buf, size_t n, int passfd) {
    if (!user_ptr_gate_armed()) return sys_send_k(fd, buf, n, passfd);
    if (n && !user_r(buf, n)) return -1;   /* §1.1 */
    if (n == 0) return sys_send_k(fd, buf, 0, passfd);

    struct bounce b; bounce_init(&b, n);
    size_t done = 0;
    long rc = 0;
    while (done < n) {
        size_t chunk = n - done;
        if (chunk > b.cap) chunk = b.cap;
        if (copy_from_user(b.buf, (uintptr_t)buf + done, chunk) != 0) { rc = -1; break; }
        long w = sys_send_k(fd, b.buf, chunk, done ? -1 : passfd);
        if (w < 0) { rc = w; break; }
        done += (size_t)w;
        if ((size_t)w < chunk) break;
    }
    bounce_fini(&b);
    return done ? (long)done : rc;
}

/* Core: `buf` is always KERNEL memory (see the *_k note in syscall.h). */
long sys_recv_k(int fd, void* buf, size_t n, int* passfd_out) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_SOCK) { if (passfd_out) *passfd_out = -1; return -1; }

    struct ofile* passed = NULL;
    /* recv(2) blocks like read(2) when the endpoint is empty and the peer is
     * still open (block == 1). */
    long r = usock_recv(o->sock, buf, n, 1, &passed);

    if (passfd_out) {
        *passfd_out = -1;
        if (passed) {
            /* The travelling reference is now ours; install it (consuming the
             * ref) as a new fd in this task's table. */
            int nfd = fd_install(passed);
            if (nfd < 0) ofile_unref(passed);
            else         *passfd_out = nfd;
        }
    } else if (passed) {
        ofile_unref(passed);                   /* caller didn't want it */
    }
    return r;
}

/* recv(2) on a unix socket into a RING-3 buffer.  A single chunk only: what the
 * endpoint had is the right answer for a datagram-ish socket, and looping would
 * block a caller that offered a big buffer.  The passed-fd out-parameter is a
 * kernel local here and copied out after the payload. */
long sys_recv(int fd, void* buf, size_t n, int* passfd_out) {
    if (!user_ptr_gate_armed()) return sys_recv_k(fd, buf, n, passfd_out);
    if (n && !user_w(buf, n)) return -1;   /* §1.1 */
    if (passfd_out && !user_w(passfd_out, sizeof(int))) return -1;

    struct bounce b; bounce_init(&b, n ? n : 1);
    size_t chunk = (n > b.cap) ? b.cap : n;
    int kpass = -1;
    long r = sys_recv_k(fd, b.buf, chunk, passfd_out ? &kpass : NULL);
    if (r > 0 && copy_to_user((uintptr_t)buf, b.buf, (size_t)r) != 0) r = -1;
    bounce_fini(&b);
    if (passfd_out) copy_to_user((uintptr_t)passfd_out, &kpass, sizeof kpass);
    return r;
}

/* recv into a RING-3 payload buffer, with any passed descriptor landing in the
 * caller's KERNEL local.  Same split as sys_recvfrom_u, and needed for the same
 * reason: the Linux personality's recvmsg owns the descriptor (it marshals it
 * into the client's SCM_RIGHTS block itself) while the payload pointer is the
 * client's.  Handing sys_recv a kernel `passfd_out` made its user-pointer check
 * reject the call — the FOURTH time this exact shape has bitten (see the sys_*_k
 * cores, linux_sendmsg and hostorder_to_sockaddr): a validity check belongs
 * where the pointer's ORIGIN is known. */
long sys_recv_u(int fd, uintptr_t ubuf, size_t n, int* kpassfd_out) {
    if (kpassfd_out) *kpassfd_out = -1;
    if (n && !vmm_user_access_ok(ubuf, n, 1)) return -1;

    struct bounce b; bounce_init(&b, n ? n : 1);
    size_t chunk = (n > b.cap) ? b.cap : n;
    long r = sys_recv_k(fd, b.buf, chunk, kpassfd_out);
    if (r > 0 && copy_to_user(ubuf, b.buf, (size_t)r) != 0) r = -1;
    bounce_fini(&b);
    return r;
}

/* ---- poll / readiness (stage 6 + Tier A.3 blocking) ----------------------- */

/* Global "some fd's readiness changed" wait-queue.  A task blocked in a
 * (timeout < 0) poll parks here; the socket layer raises fd_readiness_signal
 * after a send/close so the poller wakes and re-scans.  One shared queue (not
 * per-fd) keeps poll's multi-fd wait simple — a woken poller just re-snapshots
 * all its fds, which is what a level-triggered poll does anyway. */
static struct waitq readiness_wq = WAITQ_INIT;

/* Did the last readiness scan look at a network socket?
 *
 * A counter rather than a flag, and sampled around the caller's OWN first scan,
 * so "my fds include a socket" is answered by what this call actually looked
 * at.  A concurrent scan on another CPU can only produce a FALSE POSITIVE —
 * which starts a poller that promptly parks again — and that is the harmless
 * direction: a false negative would be a wait that never receives anything. */
static volatile uint32_t g_netsock_scans;

/* §M90 — every readiness event, counted.  Bumped under the queue lock by the
 * one routine every producer already calls, so it is exactly as complete as
 * the wake-ups poll(2) depends on — which is what makes it safe to build
 * edge-triggered epoll on (see epoll.c). */
static volatile uint32_t g_readiness_seq;
uint32_t fd_readiness_seq(void) { return g_readiness_seq; }

void fd_readiness_signal(void) {
    uint32_t f = waitq_lock(&readiness_wq);
    g_readiness_seq++;
    waitq_wake_all(&readiness_wq);
    waitq_unlock(&readiness_wq, f);
}

/* See fd.h — the single definition of readiness, shared by poll and epoll. */
uint32_t fd_readiness(int fd) {
    /* §M73 — 0/1/2 are looked up too: since §M59 they are ordinary slots, and
     * skipping the lookup answered a stdin REDIRECTED to a pipe from the
     * keyboard. */
    return fd_readiness_of(fd, fd_lookup(fd));
}

uint32_t fd_readiness_of(int fd, struct ofile* o) {
    uint32_t r = 0;

    if (o && o->kind == FD_CONSOLE) {           /* §M73 — a dup of the console */
        struct vc* v = vc_focused();
        if (v && vc_can_read_line(v)) r |= POLLIN;
        return r | POLLOUT;
    }
    if (fd == 0 && !o) {
        /* stdin is COOKED (stdin_read_line blocks until Enter), so it becomes
         * readable when a whole LINE is buffered, not when a byte is.  Saying
         * "readable" on the first keystroke would be worse than saying nothing:
         * the caller's read(2) would then block until Enter anyway — a poll
         * that lies about which reads will not block is the one thing a poll
         * must never do. */
        struct vc* v = vc_focused();
        if (v && vc_can_read_line(v)) r |= POLLIN;
        return r;
    }
    if ((fd == 1 || fd == 2) && !o)
        return POLLOUT;                         /* console out: always writable */

    /* POLLNVAL is the honest answer for a descriptor that is not open, and it
     * is reported unrequested: a loop watching an fd it has already closed
     * would otherwise wait forever on something that can never be ready. */
    if (!o) return POLLNVAL;

    switch (o->kind) {
    case FD_SOCK:
        if (usock_can_read(o->sock))  r |= POLLIN;
        if (usock_can_write(o->sock)) r |= POLLOUT;
        /* §M56.1 — the peer endpoint is gone.  RDHUP says "no more data will
         * ever arrive"; HUP additionally says the buffered data is drained, so
         * there is nothing left at all.  Reporting them separately is what
         * lets a reader finish what is already queued before it closes down —
         * collapsing the two would throw away the tail of every conversation
         * whose writer closed promptly. */
        /* §M90 — a LISTENING socket has no peer by nature: it is ready only
         * when a connection waits (POLLIN, above), never "hung up".  Reporting
         * HUP made it look ready forever, and Go's netpoller — woken by every
         * readiness event — retried accept() in a loop that never slept
         * (dockerd serving its API socket at full CPU). */
        if (!usock_peer_open(o->sock) && !usock_is_listener(o->sock)) {
            r |= POLLRDHUP;
            if (!usock_can_read(o->sock)) r |= POLLHUP;
        }
        break;
    /* §M53 stage 3 — a timerfd is readable exactly while it has uncollected
     * expirations, and is never writable.  This is what lets a loop wait for a
     * deadline and its sockets in the SAME call instead of choosing. */
    case FD_TIMER:
        if (timerfd_can_read(o->tfd)) r |= POLLIN;
        break;
    /* §M90 — an eventfd: readable while its counter is non-zero, writable
     * while one more can be added (Go's netpoller wakes itself through one). */
    case FD_EVENT:
        if (eventfd_can_read(o->efd))  r |= POLLIN;
        if (eventfd_can_write(o->efd)) r |= POLLOUT;
        break;
    /* §M90 — a netlink socket: readable while a reply waits; a request is
     * answered at once, so it is always writable. */
    case FD_NETLINK:
        if (nl_can_read(o->nl)) r |= POLLIN;
        r |= POLLOUT;
        break;
    /* §M56 — an AF_INET socket used to fall through to "always ready", so a
     * loop polling one span at full speed and every epoll_wait on it returned
     * instantly. */
    case FD_NETSOCK:
        g_netsock_scans++;
        r |= netsock_readiness(o->nsock);
        break;
    case FD_EPOLL:
        /* An epoll set is itself pollable: readable when it has events.  This
         * is what makes nesting one loop inside another possible, and costs a
         * single call because the set already knows. */
        if (epoll_has_events(o->ep)) r |= POLLIN;
        break;
    case FD_VFS:
    case FD_SHM:
    default:
        r |= POLLIN | POLLOUT;                   /* regular files never block */
        break;
    }
    return r;
}

/* Fill each pollfd's revents with the currently-ready events; return the
 * number of fds with any reportable event set.
 *
 * POLLERR/POLLHUP/POLLNVAL go into revents whether or not they were requested
 * — POSIX requires it, and a loop that could not see a hangup would have to
 * attempt a read to discover EOF, which is the blocking it used poll to avoid.
 * POLLRDHUP, by contrast, is Linux's and IS request-gated: a program that does
 * not ask for it must not have its revents grow a bit it never expected. */
#define POLL_ALWAYS (POLLERR | POLLHUP | POLLNVAL)

static int poll_snapshot(struct pollfd* pfds, int nfds) {
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        struct pollfd* pf = &pfds[i];
        pf->revents = 0;
        if (pf->fd < 0) continue;                /* negative fd: ignored, POSIX */
        uint32_t r = fd_readiness(pf->fd);
        uint32_t want = (uint32_t)(unsigned short)pf->events | POLL_ALWAYS;
        pf->revents = (short)(r & want);
        if (pf->revents) ready++;
    }
    return ready;
}

/* The deadline half of a finite readiness wait.  Interrupt context (ktimer's
 * contract): take the queue lock and wake, which is what stops the timer from
 * slipping past a task that has decided to park but not yet parked. */
static void readiness_deadline_fired(struct ktimer* t) {
    struct waitq* wq = (struct waitq*)t->arg;
    uint32_t f = waitq_lock(wq);
    waitq_wake_all(wq);
    waitq_unlock(wq, f);
}

/* See fd.h.  The single blocking loop behind poll(2) and epoll_wait(2). */
int fd_readiness_wait(int (*scan)(void* ctx), void* ctx, int timeout_ms) {
    uint32_t seen0 = g_netsock_scans;
    int ready = scan(ctx);
    if (ready > 0 || timeout_ms == 0) return ready;

    /* If this wait involves a SOCKET, tell the network stack somebody is
     * waiting: its poller only runs while somebody is (§M55), and nothing
     * else in this path would ever have said so.  Without it a poll on a
     * socket waits out its whole timeout while the answer sits unread in the
     * NIC — which is precisely how musl's resolver stopped resolving. */
    int wants_net = (g_netsock_scans != seen0);
    if (wants_net) net_waiter_enter();

    /* A FINITE timeout is a real bounded wait now (§M56).  It used to be
     * treated as a snapshot — documented, but a program asking to wait 200 ms
     * got an immediate 0, so every correct event loop written against it
     * became a busy loop.  A timeout that returns early is not a conservative
     * approximation of one that waits; it is a different function. */
    uint64_t deadline = 0;
    struct ktimer t = { 0, 0, 0, 0, 0 };
    if (timeout_ms > 0) {
        deadline = timer_now_ns() + (uint64_t)timeout_ms * 1000000ull;
        ktimer_arm(&t, deadline, readiness_deadline_fired, &readiness_wq);
    }

    for (;;) {
        if (timeout_ms > 0 && timer_now_ns() >= deadline) break;
        /* §1.5 — a kill must not wait out a multi-second timeout. */
        if (task_should_stop()) break;

        /* Re-scan under the queue lock so a change that races the scan is not
         * lost: every producer makes its fd ready BEFORE taking this lock to
         * signal, so holding it and still seeing nothing means any wake can
         * only arrive after we are parked (waitq.h's contract). */
        uint32_t f = waitq_lock(&readiness_wq);
        ready = scan(ctx);
        if (ready > 0) { waitq_unlock(&readiness_wq, f); break; }
        waitq_block(&readiness_wq);
        waitq_unlock(&readiness_wq, f);
        ready = 0;
    }

    /* Cancel unconditionally: on the ready path it has not fired and its
     * callback would otherwise reference a queue this frame is leaving; on the
     * timeout path it already has, and cancelling is a no-op. */
    if (timeout_ms > 0) ktimer_cancel(&t);
    if (wants_net) net_waiter_leave();
    return ready;
}

/* poll(2).  timeout == 0: non-blocking snapshot (the Wayland event-loop tick).
 * timeout  < 0: block until at least one fd is ready.  timeout > 0: a real
 * bounded wait (§M56 — see fd_readiness_wait for why this had to stop being a
 * snapshot). */
struct poll_scan_ctx { struct pollfd* pfds; int nfds; };
static int poll_scan(void* c) {
    struct poll_scan_ctx* s = (struct poll_scan_ctx*)c;
    return poll_snapshot(s->pfds, s->nfds);
}

/* Core: `pfds` is always KERNEL memory (see the *_k note in syscall.h). */
int sys_poll_k(struct pollfd* pfds, int nfds, int timeout) {
    struct poll_scan_ctx c = { pfds, nfds };
    return fd_readiness_wait(poll_scan, &c, timeout);
}

/* poll(2) over a RING-3 pollfd array.  The array is read-modify-write, so it is
 * copied in, worked on as kernel memory (poll_snapshot writes revents on every
 * pass, and a blocking poll parks between passes — with a raw user pointer the
 * kernel would be writing to ring-3 memory across a scheduling point), and
 * copied back out once. */
int sys_poll(struct pollfd* pfds, int nfds, int timeout) {
    if (!pfds || nfds < 0 || nfds > 1024) return -1;          /* §1.1 + bound */
    if (!user_ptr_gate_armed()) return sys_poll_k(pfds, nfds, timeout);
    if (nfds == 0) return 0;

    uintptr_t bytes = (uintptr_t)nfds * sizeof(*pfds);
    if (!user_w(pfds, bytes)) return -1;

    struct pollfd* k = (struct pollfd*)kmalloc((size_t)bytes);
    if (!k) return -1;
    int r = -1;
    if (copy_from_user(k, (uintptr_t)pfds, bytes) == 0) {
        r = sys_poll_k(k, nfds, timeout);
        if (copy_to_user((uintptr_t)pfds, k, (size_t)bytes) != 0) r = -1;
    }
    kfree(k);
    return r;
}

/* ---------------------------------------------------------------------------
 * §M53 stage 3 — timerfd syscalls.
 *
 * Nanoseconds, not a struct: `struct itimerspec` is four words whose WIDTH
 * depends on the guest (16 bytes on i386, 32 on a 64-bit ABI), and baking that
 * layout into the native call would push a guest's marshalling problem into
 * the kernel's own interface.  The personality layer converts; the native ABI
 * speaks the one unit the clock speaks.
 * --------------------------------------------------------------------------- */
/* §M90 — eventfd(initval, flags): EFD_SEMAPHORE = 1, EFD_NONBLOCK =
 * O_NONBLOCK (0x800); EFD_CLOEXEC is accepted (this system keeps no
 * close-on-exec set for such descriptors). */
int sys_eventfd_create(uint64_t init, int flags) {
    struct eventfd* e = eventfd_create_obj(init, flags & 1);
    if (!e) return -12;
    struct ofile* o = ofile_from_eventfd(e);
    if (!o) { eventfd_close(e); return -12; }
    if (flags & 0x800) o->nonblock = 1;
    int fd = fd_install(o);
    if (fd < 0) ofile_unref(o);
    return fd;
}

int sys_timerfd_create(void) {
    struct timerfd* tf = timerfd_create_obj();
    if (!tf) return -1;
    struct ofile* o = ofile_from_timerfd(tf);
    if (!o) { timerfd_close(tf); return -1; }
    int fd = fd_install(o);
    if (fd < 0) ofile_unref(o);
    return fd;
}

/* `abs` selects an absolute deadline on the timer_now_ns() timeline.  value 0
 * disarms.  Returns 0, or -1 on a bad descriptor. */
int sys_timerfd_settime(int fd, int abs, uint64_t value_ns, uint64_t interval_ns) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_TIMER) return -1;
    return timerfd_set(o->tfd, abs, value_ns, interval_ns);
}

/* Reports the time REMAINING (Linux's semantics), not the deadline that was
 * set — a caller that wanted the deadline back already has it. */
int sys_timerfd_gettime_k(int fd, uint64_t* remaining_ns, uint64_t* interval_ns) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_TIMER) return -1;
    return timerfd_get(o->tfd, remaining_ns, interval_ns);
}

/* Ring-3 entry points.  The times travel as a two-element u64 array rather than
 * as register arguments: a nanosecond value does not fit one register on i386,
 * and packing it into a REGISTER PAIR would make the native ABI's shape depend
 * on the word size — exactly the arch-specific detail every other syscall here
 * has been kept free of. */
int sys_timerfd_settime_u(int fd, int abs, const uint64_t* times) {
    uint64_t t[2] = { 0, 0 };
    if (!times) return -1;
    if (user_ptr_gate_armed()) {
        if (!user_r(times, sizeof t)) return -1;
        if (copy_from_user(t, (uintptr_t)times, sizeof t) != 0) return -1;
    } else {
        t[0] = times[0]; t[1] = times[1];
    }
    return sys_timerfd_settime(fd, abs, t[0], t[1]);
}

int sys_timerfd_gettime(int fd, uint64_t* out) {
    uint64_t t[2] = { 0, 0 };
    if (!out) return -1;
    if (sys_timerfd_gettime_k(fd, &t[0], &t[1]) != 0) return -1;
    if (user_ptr_gate_armed()) {
        if (!user_w(out, sizeof t)) return -1;
        return copy_to_user((uintptr_t)out, t, sizeof t) == 0 ? 0 : -1;
    }
    out[0] = t[0]; out[1] = t[1];
    return 0;
}

/* ---------------------------------------------------------------------------
 * §M56 — epoll syscalls.
 *
 * The native calls carry each event as a flat u64 PAIR (events, data) rather
 * than a `struct epoll_event`, for the same reason SYS_TIMERFD_SETTIME takes
 * u64[2]: that struct's size is a property of the GUEST — 12 bytes on i386 and
 * amd64, 16 on arm64, because Linux packs it on x86_64 specifically so the 32-
 * and 64-bit layouts agree and does not pack it elsewhere.  Baking one of those
 * layouts into the kernel's own interface would push a guest's marshalling
 * problem inward; the personality layer converts and the native ABI stays flat.
 * --------------------------------------------------------------------------- */
int sys_epoll_create(void) {
    struct epoll* ep = epoll_create_obj();
    if (!ep) return -1;
    struct ofile* o = ofile_from_epoll(ep);
    if (!o) { epoll_close(ep); return -1; }
    int fd = fd_install(o);
    if (fd < 0) ofile_unref(o);
    return fd;
}

int sys_epoll_ctl_k(int epfd, int op, int fd, uint32_t events, uint64_t data) {
    struct ofile* o = fd_lookup(epfd);
    if (!o || o->kind != FD_EPOLL) return -9;            /* -EBADF */
    /* Watching an epoll set with itself is a cycle with no bottom; Linux
     * rejects it and so do we, at the one place that can see both fds. */
    if (fd == epfd) return -22;                          /* -EINVAL */
    return epoll_ctl_obj(o->ep, op, fd, events, data);
}

int sys_epoll_ctl_u(int epfd, int op, int fd, const uint64_t* ev) {
    uint64_t k[2] = { 0, 0 };
    if (op != EPOLL_CTL_DEL) {
        if (!ev) return -22;
        if (user_ptr_gate_armed()) {
            if (!user_r(ev, sizeof k)) return -14;       /* -EFAULT */
            if (copy_from_user(k, (uintptr_t)ev, sizeof k) != 0) return -14;
        } else {
            k[0] = ev[0]; k[1] = ev[1];
        }
    }
    return sys_epoll_ctl_k(epfd, op, fd, (uint32_t)k[0], k[1]);
}

/* `out` is a KERNEL array of 2*maxevents u64s: (events, data) per slot. */
int sys_epoll_wait_k(int epfd, uint64_t* out, int maxevents, int timeout_ms) {
    struct ofile* o = fd_lookup(epfd);
    if (!o || o->kind != FD_EPOLL) return -9;
    if (!out || maxevents <= 0 || maxevents > 256) return -22;

    struct epoll_ev* evs = (struct epoll_ev*)kmalloc(sizeof(*evs) * (size_t)maxevents);
    if (!evs) return -12;                                /* -ENOMEM */
    int n = epoll_wait_obj(o->ep, evs, maxevents, timeout_ms);
    for (int i = 0; i < n && i < maxevents; i++) {
        out[i * 2 + 0] = evs[i].events;
        out[i * 2 + 1] = evs[i].data;
    }
    kfree(evs);
    return n;
}

/* Ring-3 form.  The result is staged in kernel memory and copied out ONCE:
 * epoll_wait parks between scans, and writing into ring-3 memory across a
 * scheduling point is exactly what §M46's bounce-buffer rule exists to stop. */
int sys_epoll_wait_u(int epfd, uintptr_t uout, int maxevents, int timeout_ms) {
    if (maxevents <= 0 || maxevents > 256) return -22;
    size_t bytes = sizeof(uint64_t) * 2 * (size_t)maxevents;
    if (user_ptr_gate_armed() && !vmm_user_access_ok(uout, bytes, 1)) return -14;

    uint64_t* k = (uint64_t*)kmalloc(bytes);
    if (!k) return -12;
    int n = sys_epoll_wait_k(epfd, k, maxevents, timeout_ms);
    if (n > 0) {
        size_t used = sizeof(uint64_t) * 2 * (size_t)n;
        if (user_ptr_gate_armed()) {
            if (copy_to_user(uout, k, used) != 0) n = -14;
        } else {
            uint64_t* d = (uint64_t*)uout;
            for (size_t i = 0; i < used / sizeof(uint64_t); i++) d[i] = k[i];
        }
    }
    kfree(k);
    return n;
}

int sys_setitimer_u(const uint64_t* times) {
    uint64_t t[2] = { 0, 0 };
    if (!times) return -1;
    if (user_ptr_gate_armed()) {
        if (!user_r(times, sizeof t)) return -1;
        if (copy_from_user(t, (uintptr_t)times, sizeof t) != 0) return -1;
    } else {
        t[0] = times[0]; t[1] = times[1];
    }
    return sys_setitimer_ns(t[0], t[1]);
}

void fd_close_all(void) {
    struct task* t = task_current();
    if (!t) return;
    /* §M89 — a thread leaving a SHARED table closes nothing: the others are
     * still using those descriptors.  The last one out closes them all. */
    if (t->fdt) {
        struct fdtable* ft = t->fdt;
        t->fdt = NULL;
        t->fds = t->fds_inline;
        t->fd_cloexec_inline = 0;
        fdtable_put(ft);
        return;
    }
    /* §M90 — FROM 0, not 3.  Since §M59 a redirected 0/1/2 is an ordinary
     * slot holding a real ofile (an empty one is the console and is skipped
     * here), and a program started with its output on a PIPE holds the
     * pipe's write end in slot 1.  Starting at 3 left it open forever when
     * the program exited: the reader never saw EOF (dockerd waited for the
     * end of `docker-init --version` until the API never came up).  The
     * shared-table path above (fdtable_put) always closed every slot, which
     * is why a multi-threaded child's pipes did reach EOF. */
    /* An EXCURSION (proc_exec_elf, the self-tests) runs on a host task that
     * outlives the program and keeps its own std streams: from 3 there. */
    for (int fd = t->user_task ? 0 : 3; fd < TASK_MAX_FDS; fd++) {
        if (t->fds[fd]) { ofile_unref(t->fds[fd]); t->fds[fd] = NULL; }
    }
    t->fd_cloexec_inline = 0;
}

/* ---- network sockets (M24 socket API — AF_INET) --------------------------- */
/*
 * A minimal BSD-sockets surface over the in-kernel net stack (net.c).  Slice 1:
 * SOCK_DGRAM (UDP).  A netsock owns a local UDP port and a small ring of
 * received datagrams; net.c's per-port binding pushes arriving datagrams into
 * the ring (in the receiving task's context — RX is polled, so no locking).
 *
 * Addresses are passed as a host-order IPv4 + a port integer rather than a
 * `struct sockaddr_in` — a deliberate simplification for the teaching ABI; a
 * sockaddr marshalling layer is a later refinement (§M36 libc / §M39).
 * (AF_INET / SOCK_* come from syscall.h.)
 */
#define NS_RXSLOTS   4
#define NS_DGRAM_MAX 1500

struct ns_dgram {
    uint32_t src_ip; uint16_t src_port; uint16_t len; uint8_t data[NS_DGRAM_MAX];
};
struct netsock {
    int      type;
    uint32_t local_ip;                  /* bind address; 0 = every address   */
    uint16_t local_port;
    int      bound;
    int      nonblock;                  /* SOCK_NONBLOCK / O_NONBLOCK        */
    struct ns_dgram rx[NS_RXSLOTS];
    volatile int rx_head, rx_tail;      /* head = produce, tail = consume */
    /* SOCK_STREAM (TCP).  §M24.9 — a socket now names ONE connection out of
     * the stack's table instead of implying the only one that could exist.
     * `conn` is an active connection (connected or accepted); `lsock` is a
     * passive one.  They are separate fields rather than a union with a flag
     * because every operation is legal on exactly one of them, and a listening
     * socket that answered read() with a connection's data would be a bug
     * nobody would find by reading the call site. */
    struct tcp_conn* conn;
    struct tcp_conn* lsock;
    int      rd_shut;                   /* shutdown(SHUT_RD): reads end here  */
    int      connected;
    uint32_t peer_ip; uint16_t peer_port;
};

static uint16_t g_ephem_port = 0xC000;

/* How long a blocking datagram recv waits.  A real duration, not a spin count
 * (§M55) — the old 40-million-iteration bound meant seconds on real hardware
 * and minutes under emulation, from the same source line. */
#define NS_RECV_TIMEOUT_MS 5000
/* How long a BLOCKING accept waits before reporting that nobody called.  A
 * bound rather than "forever" for the same reason every other wait in this
 * kernel has one: a task parked with no deadline is a task nothing but a kill
 * can retrieve. */
#define NS_ACCEPT_TIMEOUT_MS 30000

/* The awaited condition, evaluated by net_wait_cond under the stack lock. */
static int ns_has_dgram(void* a) {
    struct netsock* ns = (struct netsock*)a;
    return ns->rx_head != ns->rx_tail;
}

/* §M56 — readiness for fd_readiness().  Before this an AF_INET socket fell
 * through poll's default and was reported PERMANENTLY ready, so any loop
 * polling one spun at full speed; it went unnoticed because nothing polled a
 * network socket until epoll made it the obvious thing to do.
 *
 * A datagram socket is readable when its RX ring has one.  A stream socket's
 * readiness lives in the stack (one connection today, §M55's open item), so we
 * ask there.  Writability is "connected", which is as honest as a stack with
 * no send buffer can be. */
uint32_t netsock_readiness(struct netsock* ns) {
    if (!ns) return POLLNVAL;
    if (ns->type != SOCK_STREAM)
        return (ns->rx_head != ns->rx_tail ? POLLIN : 0) | POLLOUT;

    /* §M24.10 — a LISTENING socket is "readable" when a connection is waiting
     * to be accepted.  That is the convention every event loop is written
     * against (accept() is the read of a listening socket), and without it a
     * server built on epoll blocks forever on a set that never reports. */
    if (ns->lsock) return net_tcp_pending(ns->lsock) > 0 ? POLLIN : 0;
    if (ns->rd_shut) return POLLIN | POLLHUP;    /* readable, and it is EOF   */

    if (!ns->conn) return 0;                     /* not connected: nothing yet */
    int rd = 0, wr = 0, fin = 0, rst = 0, up = 0;
    net_tcp_state(ns->conn, &rd, &wr, &fin, &rst, &up);
    uint32_t r = wr ? POLLOUT : 0;               /* honest now: the send buffer
                                                  * can actually fill up      */
    if (rd) r |= POLLIN;
    /* §M56.2 — an RST is an ERROR, not an end of file.  Without this a refused
     * or dropped connection looks exactly like a server that answered with
     * nothing, and a loop cannot tell "done" from "broken". */
    if (rst) r |= POLLERR;
    /* §M56.1 — the peer's FIN.  A stream reader MUST be able to see this without
     * reading, or it can only discover EOF by attempting the read it used the
     * event loop to avoid.  POLLIN stays set while unread bytes remain, so the
     * loop drains the tail first and only then sees POLLHUP. */
    if (fin) {
        r |= POLLRDHUP;
        if (!rd) r |= POLLHUP;
    }
    return r;
}

/* net.c UDP-binding callback: enqueue an arriving datagram (drop if the ring
 * is full).  Runs on the poller task with the STACK LOCK HELD (§M55), which is
 * what makes this ring safe against the consumer below — and is also why it
 * must not block or allocate. */
static void ns_udp_cb(uint32_t src_ip, uint16_t src_port,
                      const uint8_t* data, uint32_t len, void* ctx) {
    struct netsock* ns = (struct netsock*)ctx;
    int nx = (ns->rx_head + 1) % NS_RXSLOTS;
    if (nx == ns->rx_tail) return;      /* full → drop */
    struct ns_dgram* d = &ns->rx[ns->rx_head];
    uint32_t n = len > NS_DGRAM_MAX ? NS_DGRAM_MAX : len;
    for (uint32_t i = 0; i < n; i++) d->data[i] = data[i];
    d->src_ip = src_ip; d->src_port = src_port; d->len = (uint16_t)n;
    ns->rx_head = nx;
}

/* Called from ofile_unref (fd.c) when the last descriptor closes. */
void netsock_close(struct netsock* ns) {
    if (!ns) return;
    if (ns->bound) net_udp_unbind(ns->local_port);
    /* Both handles, and in this order: closing the listener refuses every peer
     * still sitting in its backlog, which is the right thing to do to a
     * connection whose server has gone away — leaving them established and
     * unowned is how a client ends up talking to nobody. */
    if (ns->conn)  { net_tcp_close(ns->conn);  ns->conn  = NULL; }
    if (ns->lsock) { net_tcp_close(ns->lsock); ns->lsock = NULL; }
    kfree(ns);
}

static int ns_ensure_bound(struct netsock* ns, uint16_t port) {
    if (ns->bound) return 0;
    ns->local_port = port ? port : g_ephem_port++;
    if (net_udp_bind(ns->local_port, ns_udp_cb, ns) != 0) return -1;
    ns->bound = 1;
    return 0;
}

int sys_socket(int domain, int type, int proto) {
    (void)proto;
    if (domain != AF_INET)  return -1;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -1;
    struct netsock* ns = (struct netsock*)kcalloc(1, sizeof *ns);
    if (!ns) return -1;
    ns->type = type;
    struct ofile* o = ofile_from_netsock(ns);
    if (!o) { kfree(ns); return -1; }
    int fd = fd_install(o);
    if (fd < 0) { ofile_unref(o); return -1; }
    return fd;
}

/* ---------------------------------------------------------------------------
 * O_NONBLOCK on a socket.
 *
 * This is not a nicety.  musl's DNS resolver opens its socket with
 * SOCK_NONBLOCK and then drains it with `while (recvmsg(...) >= 0)` — it relies
 * on the SECOND call failing with EAGAIN to know the burst is over.  A blocking
 * socket therefore does not merely stall that one call: getaddrinfo never
 * returns, so every musl program that resolves a name hangs.  Honouring the
 * flag is what makes the recv loop terminate.
 * ------------------------------------------------------------------------- */
/* §M40 — which KIND of object an fd refers to.  A personality layer's recvmsg /
 * sendmsg has to route by kind: a UNIX socket (a Wayland display connection) and
 * an AF_INET socket need completely different primitives, and handling only the
 * latter made libwayland's first read fail with a bare -1 — which musl turned
 * into EPERM, a spectacularly misleading errno for "wrong fd type". */
/* §M90 — AF_UNIX sockets with NAMES (socket/bind/listen/accept/connect on a
 * path); the semantics are usock.c's.  Negative returns are Linux errnos. */
int sys_unix_socket(int nonblock) {
    struct usock* u = usock_new();
    if (!u) return -12;
    struct ofile* o = ofile_from_sock(u);
    if (!o) { usock_close(u); return -12; }
    o->nonblock = nonblock ? 1 : 0;
    int fd = fd_install(o);
    if (fd < 0) { ofile_unref(o); return -24; }               /* EMFILE */
    return fd;
}
static struct usock* unix_of(int fd) {
    struct ofile* o = fd_lookup(fd);
    return (o && o->kind == FD_SOCK) ? o->sock : NULL;
}
int sys_unix_bind(int fd, const char* name) {
    struct usock* u = unix_of(fd);
    return u ? usock_bind(u, name) : -88;                       /* ENOTSOCK */
}
int sys_unix_listen(int fd, int backlog) {
    struct usock* u = unix_of(fd);
    return u ? usock_listen(u, backlog) : -88;
}
int sys_unix_connect(int fd, const char* name) {
    struct usock* u = unix_of(fd);
    return u ? usock_connect(u, name) : -88;
}
int sys_unix_accept(int fd, int nonblock_new) {
    struct ofile* lo = fd_lookup(fd);
    if (!lo || lo->kind != FD_SOCK) return -88;
    struct usock* s = NULL;
    int r = usock_accept(lo->sock, !lo->nonblock, &s);
    if (r < 0) return r;
    struct ofile* o = ofile_from_sock(s);
    if (!o) { usock_close(s); return -12; }
    o->nonblock = nonblock_new ? 1 : 0;
    int nfd = fd_install(o);
    if (nfd < 0) { ofile_unref(o); return -24; }
    return nfd;
}
const char* sys_unix_name(int fd) {
    struct usock* u = unix_of(fd);
    return u ? usock_name(u) : NULL;
}

int sys_fd_kind(int fd) {
    struct ofile* o = fd_lookup(fd);
    return o ? (int)o->kind : -1;
}

/* §M56.1 — O_NONBLOCK for ANY descriptor, not just an AF_INET socket.
 *
 * The name is historical: it was a socket-only knob, and every other kind
 * silently ignored the flag — so a musl program that set O_NONBLOCK on a pipe
 * got a blocking pipe and no error, which is the one failure an event loop
 * cannot survive (every fd it drains is one it must not block on).  The flag
 * now lives on the open file description, where POSIX says it belongs; the
 * netsock's own copy is kept in step so the socket paths that read it directly
 * do not have to be rewritten in the same change. */
int sys_socket_setnonblock(int fd, int on) {
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    o->nonblock = on ? 1 : 0;
    if (o->kind == FD_NETSOCK && o->nsock) o->nsock->nonblock = o->nonblock;
    return 0;
}

int sys_socket_getnonblock(int fd) {
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    return o->nonblock;
}

/* bind(fd, ip, port).
 *
 * The address argument is new in §M24.10 and it is not decoration: a server
 * binds 0.0.0.0 to serve every interface and 127.0.0.1 to serve only the local
 * one, and the difference is the whole of a listening socket's security
 * posture.  The old two-argument form silently discarded it. */
int sys_bind(int fd, uint32_t ip, int port) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    ns->local_ip = ip;
    if (ns->type == SOCK_STREAM) {
        /* A stream socket's port is claimed by listen()/connect(), where the
         * connection table can refuse a clash; remembering it is all bind
         * does. */
        ns->local_port = (uint16_t)port;
        ns->bound = 1;
        return 0;
    }
    return ns_ensure_bound(ns, (uint16_t)port);
}

/* M24 — connect(fd, ip, port): TCP handshake for a SOCK_STREAM socket. */
int sys_connect(int fd, uint32_t ip, int port) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    /* §M90 — connect() on a DATAGRAM socket names its default peer: write and
     * read then mean sendto/recvfrom with that peer.  Go's resolver opens its
     * DNS socket exactly so ("dial udp 10.0.2.3:53"), and it used to be
     * refused — no name could be resolved by a Go program (docker pull). */
    if (ns->type == SOCK_DGRAM) {
        if (ns_ensure_bound(ns, 0) != 0) return -1;
        ns->connected = (ip || port) ? 1 : 0;   /* AF_UNSPEC (0,0) dissolves it */
        ns->peer_ip = ip; ns->peer_port = (uint16_t)port;
        return 0;
    }
    if (ns->type != SOCK_STREAM) return -1;
    if (ns->conn) return -1;                     /* already connected          */
    struct tcp_conn* c = net_tcp_connect(ip, (uint16_t)port, 0);
    if (!c) return -1;
    ns->conn = c;
    ns->connected = 1; ns->peer_ip = ip; ns->peer_port = (uint16_t)port;
    return 0;
}

/* listen(fd, backlog) — turn a bound socket into a passive one. */
int sys_listen(int fd, int backlog) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    if (ns->type != SOCK_STREAM || ns->conn || ns->lsock) return -1;
    if (!ns->local_port) return -1;              /* listen without bind        */
    struct tcp_conn* l = net_tcp_listen(ns->local_ip, ns->local_port, backlog);
    if (!l) return -1;
    ns->lsock = l;
    return 0;
}

/* accept(fd, &ip, &port) → a NEW descriptor for the accepted connection.
 *
 * Every pointer here is a KERNEL one; the personality layer marshals the
 * client's sockaddr itself (the *_k / *_u split — see syscall.h).  A blocking
 * accept waits; a non-blocking one answers EAGAIN, which is what lets an event
 * loop call it only when its poll said the listener was readable. */
int sys_accept_k(int fd, uint32_t* ip_out, int* port_out) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ls = o->nsock;
    if (!ls->lsock) return -1;                   /* not a listening socket     */

    struct tcp_conn* c = net_tcp_accept(ls->lsock,
                                        ls->nonblock ? 0 : NS_ACCEPT_TIMEOUT_MS);
    if (!c) return ls->nonblock ? -SOCK_EAGAIN : -1;

    struct netsock* ns = (struct netsock*)kcalloc(1, sizeof *ns);
    if (!ns) { net_tcp_close(c); return -1; }
    ns->type = SOCK_STREAM;
    ns->conn = c;
    ns->connected = 1;
    net_tcp_peer(c, &ns->peer_ip, &ns->peer_port);
    net_tcp_local(c, &ns->local_ip, &ns->local_port);

    struct ofile* no = ofile_from_netsock(ns);
    if (!no) { net_tcp_close(c); kfree(ns); return -1; }
    int nfd = fd_install(no);
    if (nfd < 0) { ofile_unref(no); return -1; }
    if (ip_out)   *ip_out   = ns->peer_ip;
    if (port_out) *port_out = ns->peer_port;
    return nfd;
}

/* shutdown(fd, how) — 0 = SHUT_RD, 1 = SHUT_WR, 2 = both.
 *
 * Both halves are REAL.  Answering "success" to SHUT_RD while still delivering
 * data would be the §M56.1 stub shape: nothing fails, so nothing is
 * investigated, while every program that used it got no shutdown.  The read
 * half is enforced locally (further reads report end of stream) because that
 * is precisely what it means — it is a promise about THIS socket, not a
 * message to the peer. */
int sys_shutdown(int fd, int how) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    if (how == 0 || how == 2) ns->rd_shut = 1;
    if (how == 1 || how == 2) {
        if (!ns->conn) return -1;
        net_tcp_shutdown(ns->conn);
    }
    return 0;
}

/* getsockname / getpeername — kernel-pointer cores, same split as accept. */
int sys_getsockname_k(int fd, uint32_t* ip_out, int* port_out) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    uint32_t ip = ns->local_ip; uint16_t port = ns->local_port;
    if (ns->conn) net_tcp_local(ns->conn, &ip, &port);
    /* §M90 — a connected datagram socket answers with the address it sends
     * from (Go asks right after connect, to know its own end). */
    if (!ns->conn && ns->type == SOCK_DGRAM && !ip && ns->connected) {
        struct net_device* dev = net_route(ns->peer_ip);
        if (!dev) dev = net_primary();
        if (dev) ip = dev->ip;
    }
    if (ip_out)   *ip_out   = ip;
    if (port_out) *port_out = port;
    return 0;
}

int sys_getpeername_k(int fd, uint32_t* ip_out, int* port_out) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    if (ns->type == SOCK_DGRAM && ns->connected) {   /* §M90 — its default peer */
        if (ip_out)   *ip_out   = ns->peer_ip;
        if (port_out) *port_out = ns->peer_port;
        return 0;
    }
    if (!ns->conn) return -1;                    /* not connected              */
    uint32_t ip = 0; uint16_t port = 0;
    net_tcp_peer(ns->conn, &ip, &port);
    if (ip_out)   *ip_out   = ip;
    if (port_out) *port_out = port;
    return 0;
}

/* The GATED entries.  Same split as sendto/recvfrom and for the same reason
 * (§M46, §M47.2): the *_k cores take kernel pointers and have in-kernel
 * callers, so a ring-3 check inside them would reject every one of those; the
 * check belongs where the pointer's ORIGIN is known. */
static int ns_addr_out(uint32_t* ip_out, int* port_out, uint32_t ip, int port) {
    if (ip_out) {
        if (!user_w(ip_out, sizeof *ip_out)) return -1;
        if (copy_to_user((uintptr_t)ip_out, &ip, sizeof ip) != 0) return -1;
    }
    if (port_out) {
        if (!user_w(port_out, sizeof *port_out)) return -1;
        if (copy_to_user((uintptr_t)port_out, &port, sizeof port) != 0) return -1;
    }
    return 0;
}

int sys_accept(int fd, uint32_t* ip_out, int* port_out) {
    if (!user_ptr_gate_armed()) return sys_accept_k(fd, ip_out, port_out);
    uint32_t ip = 0; int port = 0;
    int r = sys_accept_k(fd, &ip, &port);
    if (r >= 0 && ns_addr_out(ip_out, port_out, ip, port) != 0) return -1;
    return r;
}

int sys_getsockname(int fd, uint32_t* ip_out, int* port_out) {
    if (!user_ptr_gate_armed()) return sys_getsockname_k(fd, ip_out, port_out);
    uint32_t ip = 0; int port = 0;
    int r = sys_getsockname_k(fd, &ip, &port);
    if (r == 0 && ns_addr_out(ip_out, port_out, ip, port) != 0) return -1;
    return r;
}

int sys_getpeername(int fd, uint32_t* ip_out, int* port_out) {
    if (!user_ptr_gate_armed()) return sys_getpeername_k(fd, ip_out, port_out);
    uint32_t ip = 0; int port = 0;
    int r = sys_getpeername_k(fd, &ip, &port);
    if (r == 0 && ns_addr_out(ip_out, port_out, ip, port) != 0) return -1;
    return r;
}

/* Stream read/write over a connected SOCK_STREAM socket (called by
 * sys_read/sys_write when the fd is FD_NETSOCK). */
static long ns_dgram_recv(struct netsock* ns, void* buf, size_t n, uint32_t* ip_out, int* port_out);
static long netsock_write(struct netsock* ns, const void* buf, size_t n) {
    if (ns->type == SOCK_DGRAM) {                /* §M90 — a connected datagram socket */
        if (!ns->connected) return -1;
        struct net_device* dev = net_primary();
        if (!dev || n > 65507u) return -1;
        if (net_udp_send(dev, ns->peer_ip, ns->local_port, ns->peer_port, buf, n) != 0) return -1;
        return (long)n;
    }
    if (ns->type != SOCK_STREAM || !ns->conn) return -1;
    int r = net_tcp_send(ns->conn, buf, (uint32_t)n, ns->nonblock);
    return (r == NET_EAGAIN) ? -SOCK_EAGAIN : r;
}
static long netsock_read(struct netsock* ns, void* buf, size_t n) {
    if (ns->type == SOCK_DGRAM) return ns_dgram_recv(ns, buf, n, NULL, NULL);   /* §M90 */
    if (ns->type != SOCK_STREAM || !ns->conn) return -1;
    if (ns->rd_shut) return 0;                   /* shutdown(SHUT_RD) = EOF   */
    int r = net_tcp_recv(ns->conn, buf, (uint32_t)n, ns->nonblock, 0);
    /* NET_EAGAIN is "nothing yet", and it is NOT zero: zero means the peer
     * closed, and a drain loop told EOF by a live connection stops for good
     * (§M56.1's pipe lesson, one layer up). */
    if (r == NET_EAGAIN) return -SOCK_EAGAIN;
    return r;
}

/* Core: `buf` is always KERNEL memory (see the *_k note in syscall.h). */
long sys_sendto_k(int fd, const void* buf, size_t n, uint32_t ip, int port) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    if (ns_ensure_bound(ns, 0) != 0) return -1;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    if (net_udp_send(dev, ip, ns->local_port, (uint16_t)port, buf, n) != 0) return -1;
    return (long)n;
}

/* sendto(2) from a RING-3 buffer.  A datagram is ATOMIC — it must go out as one
 * packet — so unlike the stream paths this cannot be chunked: the payload is
 * staged whole or the call fails.  UDP_MAX_PAYLOAD bounds what a program can
 * make the kernel allocate in one call. */
#define UDP_MAX_PAYLOAD  65507u

long sys_sendto(int fd, const void* buf, size_t n, uint32_t ip, int port) {
    if (!user_ptr_gate_armed()) return sys_sendto_k(fd, buf, n, ip, port);
    if (n > UDP_MAX_PAYLOAD) return -1;
    if (n && !user_r(buf, n)) return -1;   /* §1.1 */
    if (n == 0) return sys_sendto_k(fd, buf, 0, ip, port);

    void* k = kmalloc(n);
    if (!k) return -1;
    long rc = (copy_from_user(k, (uintptr_t)buf, n) == 0)
              ? sys_sendto_k(fd, k, n, ip, port) : -1;
    kfree(k);
    return rc;
}

/* Core: every pointer is KERNEL memory — `buf` included (it used to accept a
 * caller-validated user buffer; that is now the job of sys_recvfrom_u, so the
 * datagram copy below can never touch ring-3 memory). */
long sys_recvfrom_k(int fd, void* buf, size_t n, uint32_t* ip_out, int* port_out) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    return ns_dgram_recv(o->nsock, buf, n, ip_out, port_out);
}
/* One datagram (the body recvfrom always had; read() on a connected datagram
 * socket shares it since §M90). */
static long ns_dgram_recv(struct netsock* ns, void* buf, size_t n, uint32_t* ip_out, int* port_out) {
    struct net_device* dev = net_primary();
    if (!dev) return -1;

    /* A NON-BLOCKING socket gets exactly one pump of the device — enough to
     * pick up anything already on the wire, which is what a real IRQ-driven
     * driver would have done for us — and then reports EAGAIN.  Without that
     * distinction musl's resolver, which drains its socket with
     * `while (recvmsg(...) >= 0)`, waits out the full blocking timeout on a
     * socket it explicitly asked not to block on. */
    if (ns->nonblock) {
        if (!ns_has_dgram(ns)) net_pump_once();
        if (!ns_has_dgram(ns)) return -SOCK_EAGAIN;
    } else if (!net_wait_cond(ns_has_dgram, ns, NS_RECV_TIMEOUT_MS)) {
        return -1;                                     /* timeout */
    }

    /* Dequeue under the stack lock — the producer (ns_udp_cb) runs on the
     * poller task, so the "is there one" and the copy have to be one step. */
    uint32_t f = net_lock();
    if (ns->rx_head == ns->rx_tail) { net_unlock(f); return -1; }
    struct ns_dgram* d = &ns->rx[ns->rx_tail];
    uint32_t cnt = (d->len < n) ? d->len : (uint32_t)n;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < cnt; i++) out[i] = d->data[i];
    if (ip_out)   *ip_out   = d->src_ip;
    if (port_out) *port_out = d->src_port;
    ns->rx_tail = (ns->rx_tail + 1) % NS_RXSLOTS;
    net_unlock(f);
    return (long)cnt;
}

/* recvfrom into a RING-3 payload buffer, with the source address landing in the
 * caller's KERNEL locals.  That split is what the Linux personality needs: it
 * marshals (ip, port) into the client's `struct sockaddr_in` itself, but the
 * payload pointer is the client's.  sys_recvfrom (the d-os-native entry) is a
 * thin wrapper that also copies the address out to ring 3. */
long sys_recvfrom_u(int fd, uintptr_t ubuf, size_t n, uint32_t* ip_out, int* port_out) {
    if (n > UDP_MAX_PAYLOAD) n = UDP_MAX_PAYLOAD;
    if (n && !vmm_user_access_ok(ubuf, n, 1)) return -1;   /* §1.1 */
    if (n == 0) return sys_recvfrom_k(fd, NULL, 0, ip_out, port_out);

    void* k = kmalloc(n);
    if (!k) return -1;
    long r = sys_recvfrom_k(fd, k, n, ip_out, port_out);
    if (r > 0 && copy_to_user(ubuf, k, (size_t)r) != 0) r = -1;
    kfree(k);
    return r;
}

long sys_recvfrom(int fd, void* buf, size_t n, uint32_t* ip_out, int* port_out) {
    if (!user_ptr_gate_armed())
        return sys_recvfrom_k(fd, buf, n, ip_out, port_out);   /* kernel caller */
    if (ip_out && !user_w(ip_out, sizeof(*ip_out))) return -1;
    if (port_out && !user_w(port_out, sizeof(*port_out))) return -1;

    uint32_t kip = 0; int kport = 0;
    long r = sys_recvfrom_u(fd, (uintptr_t)buf, n, &kip, &kport);
    if (r >= 0) {
        if (ip_out   && copy_to_user((uintptr_t)ip_out,   &kip,   sizeof kip)   != 0) return -1;
        if (port_out && copy_to_user((uintptr_t)port_out, &kport, sizeof kport) != 0) return -1;
    }
    return r;
}


/* §M71 — one task's share of violation D (see au_boundary). */
struct au_leak_ctx { int bad; int verbose; };

static void au_gate_leak_cb(const struct task* t, int is_current, void* ctx) {
    struct au_leak_ctx* c = (struct au_leak_ctx*)ctx;
    (void)is_current;
    /* THE CURRENT TASK IS CHECKED TOO, and the first version of this skipped
     * it — reasoning that probe C above arms the gate on ourselves.  It does,
     * and it RESTORES it before this runs, so the exclusion protected nothing
     * and cost everything: `audit` is typed FROM a shell, so the shell is the
     * current task, so the one task a leaked flag could be observed on was the
     * one being skipped.  The falsifier reported a clean pass against a
     * deliberately broken machine, which is §M57's "a test that cannot fail is
     * not evidence" reached by a different route. */
    if (!t) return;
    if (t->in_user_syscall && !t->user_task) {
        kprintf("!! audit boundary: kernel task '%s' (pid %d) carries the ring-3 "
                "pointer gate — a dispatcher did not restore it\n",
                t->name ? t->name : "?", t->pid);
        c->bad++;
    }
}

/* =============================================================================
 * §M71 — THE RING-3 POINTER BOUNDARY AUDIT.
 *
 * THE INVARIANT: the boundary §M46 built is actually in place, and no task is
 * carrying the gate flag that should not be.
 *
 * §M47.2 IS WHY THIS EXISTS, and it is the sharpest example in the tree of a
 * defect that reading cannot find: `linux_syscall_dispatch` never set
 * `task->in_user_syscall` on EITHER x86 arch, so §M46's first boundary layer
 * was OFF for every musl program — coreutils, sh, TLS, NetSurf, Wayland.
 * **Nothing failed visibly**, which is exactly why it survived two milestones.
 * A boundary that is declared, documented, tested by a self-test that happens
 * to take the other path, and simply never armed, is indistinguishable from a
 * working one until somebody attacks it.
 *
 * WHAT CAN AND CANNOT BE CHECKED FROM HERE, stated plainly because a pass must
 * not be read as more than it is.  Whether every syscall ENTRY PATH arms the
 * gate is a property of six dispatchers, and an audit running on some other
 * task cannot observe it — `faulttest` is what exercises that, per personality.
 * What IS checkable is everything that can silently REGRESS underneath it:
 * whether the fixup table survived the link, whether the predicate still
 * refuses a kernel address, and whether the flag is being leaked.
 *
 * HOW TO MAKE IT FAIL: `boundarytest` below arms the gate on this task and
 * leaves it armed, which is precisely the leak violation D describes.
 * =========================================================================== */

static int au_boundary(int verbose) {
    int bad = 0;

    /* A — THE EXCEPTION TABLE SURVIVED THE LINK.  §M46's second layer is a
     * `.ex_table` section built entirely out of entries nothing REFERENCES by
     * name, so it exists only because the linker script says KEEP().  Lose
     * that and every fault during a copy_from_user panics the kernel instead
     * of returning -EFAULT — the failure §M46 exists to prevent, reintroduced
     * by a link-script edit that no compiler would flag. */
    long fixups = __stop_ex_table - __start_ex_table;
    if (fixups <= 0) {
        kprintf("!! audit boundary: the uaccess exception table is EMPTY — a "
                "fault inside a user copy will panic instead of returning -EFAULT\n");
        bad++;
    } else if (verbose) {
        kprintf("       exception table: %d fixup(s)\n", (int)fixups);
    }

    /* B — THE PREDICATE STILL REFUSES A KERNEL ADDRESS.  Probed rather than
     * assumed: audit.h rule 2 says report what was observed.  Its own address
     * is used as the kernel pointer, so the test needs nothing set up and
     * cannot be defeated by a layout change. */
    uintptr_t kaddr = (uintptr_t)&au_boundary;
    if (vmm_user_access_ok(kaddr, 4, 0)) {
        kprintf("!! audit boundary: vmm_user_access_ok ACCEPTED a kernel address "
                "(%x) — the ring-3 pointer gate would pass kernel memory\n",
                (unsigned)kaddr);
        bad++;
    } else if (verbose) {
        kprintf("       gate refuses a kernel address: yes\n");
    }

    /* C — AND SO DOES THE COPY ITSELF.  Separate from B because they can
     * diverge: the predicate is one function and `copy_from_user` is the path
     * every syscall actually takes.  §M46's own lesson — *a validity CHECK is
     * not a guarantee* — cuts both ways, so the wrapper is exercised too. */
    struct task* me = task_current();
    int prev = me ? me->in_user_syscall : 0;
    if (me) me->in_user_syscall = 1;        /* pretend we came from ring 3 */
    char sink[4];
    int rc = copy_from_user(sink, kaddr, sizeof sink);
    if (me) me->in_user_syscall = prev;
    if (rc == 0) {
        kprintf("!! audit boundary: copy_from_user SUCCEEDED from a kernel "
                "address while the gate was armed\n");
        bad++;
    } else if (verbose) {
        kprintf("       copy_from_user refuses a kernel address: yes\n");
    }

    /* D — NOBODY IS CARRYING THE FLAG WHO SHOULD NOT BE.  Every dispatcher
     * saves the previous value and restores it on the way out; a KERNEL task
     * left with the gate armed means one of those restores did not happen, and
     * the consequence is not a security hole but its mirror image — every
     * in-kernel caller of a shared `sys_*` starts failing, which is what broke
     * fdtest/socktest/polltest and, through ld.so's fstat of each shared
     * object, NetSurf. */
    struct au_leak_ctx lc = { 0, verbose };
    task_for_each(au_gate_leak_cb, &lc);
    bad += lc.bad;

    return bad ? bad : AUDIT_OK;
}

AUDIT(boundary) = {
    "ring3-boundary",
    "the uaccess fixup table is linked in, the pointer gate refuses kernel "
    "addresses, and no kernel task is carrying the gate flag",
    au_boundary
};

/* §M71 — the falsifier for violation D.  Arms the gate on this task and leaves
 * it armed, which is exactly the leaked-flag state.  `boundarytest off` clears
 * it again; leaving it set makes every in-kernel `sys_*` call on this shell
 * start failing, which is itself worth seeing once. */
void usyscall_boundary_test(int on) {
    struct task* me = task_current();
    if (!me) return;
    me->in_user_syscall = on ? 1 : 0;
    kprintf("boundarytest: gate %s on '%s' — `audit ring3-boundary` must %s\n",
            on ? "LEFT ARMED" : "cleared", me->name ? me->name : "?",
            on ? "fail" : "pass");
}

/* §M90 — what getsockopt(SOL_SOCKET, ...) needs to answer truthfully about a
 * descriptor: its family (Linux numbering: 1 = AF_UNIX, 2 = AF_INET), its type
 * (1 = SOCK_STREAM, 2 = SOCK_DGRAM) and whether it is listening.  0, or -1 when
 * `fd` is not a socket at all (the caller turns that into ENOTSOCK). */
int sys_socket_info(int fd, int* family, int* type, int* listening) {
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_SOCK) {
        *family = 1; *type = 1;                   /* AF_UNIX streams only */
        *listening = usock_is_listener(o->sock);
        return 0;
    }
    if (o->kind == FD_NETSOCK && o->nsock) {
        *family = 2; *type = o->nsock->type;
        *listening = o->nsock->lsock != NULL;
        return 0;
    }
    if (o->kind == FD_NETLINK) {                  /* §M90 — AF_NETLINK, SOCK_RAW */
        *family = 16; *type = 3; *listening = 0;
        return 0;
    }
    return -1;
}

/* §M90 — close-on-exec, see the note at struct fdtable. */
int fd_set_cloexec(int fd, int on) {
    struct task* t = task_current();
    if (!t || fd < 0 || fd >= TASK_MAX_FDS) return -1;
    uint32_t fl = fdt_lock(t);
    /* An EMPTY 0/1/2 is the console (§M59), a valid descriptor: Go's runtime
     * checks fds 0-2 with F_GETFD at startup and dies "cannot open standard
     * fds" when it hears EBADF. */
    int ok = t->fds[fd] != NULL || fd <= 2;
    if (ok) { if (on) *fd_cx(t) |= 1u << fd; else *fd_cx(t) &= ~(1u << fd); }
    fdt_unlock(t, fl);
    return ok ? 0 : -1;
}
int fd_get_cloexec(int fd) {
    struct task* t = task_current();
    if (!t || fd < 0 || fd >= TASK_MAX_FDS) return -1;
    uint32_t fl = fdt_lock(t);
    int r = (t->fds[fd] || fd <= 2) ? (int)((*fd_cx(t) >> fd) & 1u) : -1;
    fdt_unlock(t, fl);
    return r;
}
uint32_t fd_cloexec_mask(struct task* t) {
    if (!t) return 0;
    uint32_t fl = fdt_lock(t);
    uint32_t m = *fd_cx(t);
    fdt_unlock(t, fl);
    return m;
}
void fd_cloexec_restore(struct task* t, uint32_t mask) {
    if (t) *fd_cx(t) = mask;            /* a fork child's own, still private table */
}
/* At the point of no return in execve: close every marked descriptor.  The
 * references are taken out under the lock and released after it, because
 * releasing one may close a file or a socket (which wakes, and may sleep). */
void fd_close_on_exec(void) {
    struct task* t = task_current();
    if (!t) return;
    struct ofile* gone[TASK_MAX_FDS];
    int n = 0;
    uint32_t fl = fdt_lock(t);
    uint32_t m = *fd_cx(t);
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (!(m & (1u << fd))) continue;
        if (t->fds[fd]) gone[n++] = t->fds[fd];
        t->fds[fd] = NULL;
    }
    *fd_cx(t) = 0;
    fdt_unlock(t, fl);
    for (int i = 0; i < n; i++) ofile_unref(gone[i]);
}

/* §M90 — the path of a DIRECTORY descriptor, for the *at calls.  0, -9
 * (EBADF) when `fd` is not an open file, -20 (ENOTDIR) when it is not a
 * directory. */
int sys_fd_dirpath(int fd, char* out, size_t cap) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_VFS || !o->file || !o->file->dentry) return -9;
    if (!o->file->inode || o->file->inode->type != INODE_DIR) return -20;
    return vfs_dentry_path(o->file->dentry, out, cap) == 0 ? 0 : -2;
}

/* §M90 — fsync / fdatasync / syncfs on a descriptor (see vfs_fsync_file).
 * Anything that is not a file has nothing to write back: 0, as on Linux for a
 * pipe is EINVAL — but no caller here depends on that, and bbolt calls it
 * only on its database file.  -9 EBADF for a bad descriptor, -5 EIO when the
 * device refused a write. */
int sys_fsync(int fd) {
    struct ofile* o = fd_lookup(fd);
    if (!o) return (fd >= 0 && fd <= 2) ? 0 : -9;
    if (o->kind != FD_VFS || !o->file) return 0;
    return vfs_fsync_file(o->file) == 0 ? 0 : -5;
}

/* §M90 — socket(AF_NETLINK, …, proto).  -93 EPROTONOSUPPORT for a protocol
 * other than NETLINK_ROUTE (see netlink.c for what it answers). */
int sys_netlink_socket(int proto, int nonblock) {
    struct nlsock* s = nl_create(proto);
    if (!s) return -93;
    struct ofile* o = ofile_from_netlink(s);
    if (!o) { nl_close(s); return -12; }
    o->nonblock = nonblock ? 1 : 0;
    int fd = fd_install(o);
    if (fd < 0) { ofile_unref(o); return -24; }
    return fd;
}

/* §M90 — a fork's copy of the descriptor table, UNDER THE TABLE'S LOCK.
 *
 * fork.c used to walk parent->fds taking a reference on each entry with no
 * lock.  With a SHARED table (any multi-threaded parent — every Go program)
 * another thread can close a descriptor in the middle of that walk: it clears
 * the slot and drops the last reference while the forking thread is taking
 * one on the same pointer — a reference to freed memory, inherited by the
 * child.  When the child later released it, it decremented whatever object
 * the allocator had put there since: a different open file, closed under its
 * owner, closed again by the owner — the double close that took the machine
 * down after dockerd came up (it forks runc/docker-init while its other
 * threads open and close files).  close() clears the slot under this same
 * lock, so a reference taken here is always taken while the table still holds
 * one.  `out` receives TASK_MAX_FDS entries; the close-on-exec mask comes
 * from the same instant. */
void fd_snapshot_for_fork(struct task* parent, struct ofile** out, uint32_t* cloexec) {
    uint32_t fl = fdt_lock(parent);
    for (int i = 0; i < TASK_MAX_FDS; i++)
        out[i] = parent->fds[i] ? ofile_ref(parent->fds[i]) : NULL;
    if (cloexec) *cloexec = *fd_cx(parent);
    fdt_unlock(parent, fl);
}

/* §M90 — the n-th open descriptor of the CALLING task (for /proc/self/fd),
 * or -1 past the last.  0, 1 and 2 are always open: an empty std slot is the
 * console. */
int fd_nth_open(int n) {
    struct task* t = task_current();
    if (!t || n < 0) return -1;
    int found = -1, k = 0;
    uint32_t fl = fdt_lock(t);
    for (int fd = 0; fd < TASK_MAX_FDS; fd++) {
        if (!(fd <= 2 || t->fds[fd])) continue;
        if (k++ == n) { found = fd; break; }
    }
    fdt_unlock(t, fl);
    return found;
}
