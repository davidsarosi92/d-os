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
#include "fd.h"
#include "vmm.h"
#include "uaccess.h"   /* §1.1 — fault-safe user copies (exception table) */
#include "pmm.h"
#include "console.h"
#include "vc.h"
#include "waitq.h"
#include "net.h"
#include "hal_api.h"
#include "kmalloc.h"
#include "rtc.h"
#include "timer.h"
#include "random.h"
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE          4096u
#define MMAP_BASE_OFFSET   0x08000000u   /* mmap region: base+128 MiB (above the
                                          * image/interp/stack; see proc.c layout) */

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
static struct ofile* fd_lookup(int fd);        /* defined with the fd table below */
static int read_may_loop(int fd) {
    if (fd < 3) return 0;                       /* stdin is line-oriented       */
    struct ofile* o = fd_lookup(fd);
    return (o && o->kind == FD_VFS);
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

/* Resolve a real fd (>= 3) to its ofile, or NULL if out of range / not open. */
static struct ofile* fd_lookup(int fd) {
    struct task* t = task_current();
    if (!t || fd < 3 || fd >= TASK_MAX_FDS) return NULL;
    return t->fds[fd];
}

/* Install `o` in the lowest free real-fd slot (>= 3).  Consumes the reference
 * on success; on failure returns -1 (caller unrefs). */
static int fd_install(struct ofile* o) {
    struct task* t = task_current();
    if (!t) return -1;
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
        if (!t->fds[fd]) { t->fds[fd] = o; return fd; }
    }
    return -1;
}

/* Forward decls — FD_NETSOCK stream I/O (defined with the socket layer below). */
static long netsock_write(struct netsock* ns, const void* buf, size_t n);
static long netsock_read (struct netsock* ns, void* buf, size_t n);

/* Core: `buf` is always KERNEL memory (see the *_k note in syscall.h). */
long sys_write_k(int fd, const void* buf, size_t n) {
    if (fd == 1 || fd == 2) {                 /* stdout / stderr → console */
        const char* s = (const char*)buf;
        /* §M43: also capture into the task's buffer if one is set (Editor
         * "Compile & Run" reads it back), leaving room for a NUL terminator. */
        struct task* me = task_current();
        for (size_t i = 0; i < n; i++) {
            if (me && me->cap_buf && me->cap_len < me->cap_cap - 1)
                me->cap_buf[me->cap_len++] = s[i];
            console_putchar(s[i]);
        }
        if (me && me->cap_buf && me->cap_len < me->cap_cap)
            me->cap_buf[me->cap_len] = '\0';
        return (long)n;
    }
    if (fd == 0) return -1;                    /* can't write stdin */
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_VFS)  return (long)vfs_write(o->file, buf, n);
    if (o->kind == FD_SOCK) return usock_send(o->sock, buf, n, NULL);
    if (o->kind == FD_NETSOCK) return netsock_write(o->nsock, buf, n);
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
    if (fd == 0) return stdin_read_line((char*)buf, n);   /* cooked stdin */
    if (fd == 1 || fd == 2) return -1;
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_VFS)  return (long)vfs_read(o->file, buf, n);
    /* Sockets read(2) with POSIX blocking semantics (block == 1): an empty
     * read waits for the peer to send (or close → 0/EOF). */
    if (o->kind == FD_SOCK) return usock_recv(o->sock, buf, n, 1, NULL);
    if (o->kind == FD_NETSOCK) return netsock_read(o->nsock, buf, n);
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

int sys_close(int fd) {
    if (fd >= 0 && fd <= 2) return 0;          /* std streams: no-op */
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    task_current()->fds[fd] = NULL;
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
    if (vmm_space_mmap_cursor(t->mm) == 0)
        vmm_space_set_mmap_cursor(t->mm, vmm_user_base() + MMAP_BASE_OFFSET);
    uintptr_t va = vmm_space_mmap_cursor(t->mm);

    if (fd < 0) {
        for (int i = 0; i < n; i++) {
            pmm_phys_t fr = pmm_alloc_frame();
            if (!fr) return -1;
            uint8_t* p = (uint8_t*)phys_to_virt(fr);
            for (int b = 0; b < (int)PAGE_SIZE; b++) p[b] = 0;
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
    vmm_space_set_mmap_cursor(t->mm, va + (uintptr_t)n * PAGE_SIZE);
    return (long)va;
}

/* §M37 — full mmap for the Linux ABI (what musl's ld.so needs to load a .so):
 * honors `addr`+MAP_FIXED, maps file-backed regions (reads `len` bytes from the
 * VFS fd starting at `offset`), and translates prot → VMM flags so a text
 * segment is mapped executable.  Anonymous (fd<0 or MAP_ANONYMOUS) works too.
 * Returns the mapped user VA or -1.  (mprotect is a no-op elsewhere, which is
 * fine: every PT_LOAD is mapped here with its own prot; mprotect only tightens
 * RELRO afterwards.) */
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20

long sys_mmap_full(uintptr_t addr, size_t len, int prot, int flags,
                   int fd, uint64_t offset) {
    struct task* t = task_current();
    if (!t || !t->mm) return -1;
    if (len > (256u << 20)) return -1;         /* §4.5 — cap a single mapping   */

    int n = (int)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    if (n <= 0) n = 1;

    uint32_t vf = VMM_USER;
    if (prot & PROT_WRITE) vf |= VMM_WRITABLE;
    if (prot & PROT_EXEC)  vf |= VMM_EXEC;

    /* Target VA: MAP_FIXED honors the caller's addr; else bump-allocate. */
    uintptr_t va;
    if ((flags & MAP_FIXED) && addr) {
        va = addr & ~(uintptr_t)(PAGE_SIZE - 1);
        /* §2.5 — a fixed mapping must stay inside the user address range and
         * not wrap; the target space is the task's own, so this only guards
         * against a garbage/kernel addr, not against self-overwrite. */
        if (va < vmm_user_base() || va + (uintptr_t)n * PAGE_SIZE < va) return -1;
    } else {
        if (vmm_space_mmap_cursor(t->mm) == 0)
            vmm_space_set_mmap_cursor(t->mm, vmm_user_base() + MMAP_BASE_OFFSET);
        va = vmm_space_mmap_cursor(t->mm);
        vmm_space_set_mmap_cursor(t->mm, va + (uintptr_t)n * PAGE_SIZE);
    }

    /* File to read from for a file-backed mapping (else anonymous zero-fill).
     *
     * §M40 — a memfd (FD_SHM) is a THIRD case and must be handled separately:
     * its whole purpose is that both sides see the SAME frames, so it maps the
     * object's existing pages rather than allocating fresh ones.  Missing this
     * branch is why an upstream Wayland client's mmap of its shm pool failed —
     * the native sys_mmap had it, this Linux-facing entry point did not. */
    struct file* file = NULL;
    if (fd >= 0 && !(flags & MAP_ANONYMOUS)) {
        struct ofile* o = fd_lookup(fd);
        if (!o) return -1;
        if (o->kind == FD_SHM && o->shm) {
            struct shm* sh = o->shm;
            int first = (int)(offset / PAGE_SIZE);
            for (int i = 0; i < n; i++) {
                int idx = first + i;
                if (idx >= sh->nframes) break;      /* short object: stop here */
                /* VMM_SHARED: the shm object owns these frames, so tearing the
                 * address space down must not free them. */
                if (vmm_space_map(t->mm, va + (uintptr_t)i * PAGE_SIZE,
                                  sh->frames[idx],
                                  vf | VMM_WRITABLE | VMM_SHARED) != 0)
                    return -1;
            }
            return (long)va;
        }
        if (o->kind != FD_VFS || !o->file) return -1;
        file = o->file;
    }

    for (int i = 0; i < n; i++) {
        uintptr_t page_va = va + (uintptr_t)i * PAGE_SIZE;
        /* MAP_FIXED may overlay an earlier reservation — drop the old PTE so
         * the fresh frame maps cleanly. */
        if (flags & MAP_FIXED) vmm_space_unmap(t->mm, page_va);

        pmm_phys_t fr = pmm_alloc_frame();
        if (!fr) return -1;
        uint8_t* p = (uint8_t*)phys_to_virt(fr);      /* kernel direct map     */
        for (int b = 0; b < (int)PAGE_SIZE; b++) p[b] = 0;

        if (file) {
            /* Positioned read; restore the fd cursor (musl owns it). */
            uint64_t save = file->pos;
            file->pos = offset + (uint64_t)i * PAGE_SIZE;
            vfs_read(file, p, PAGE_SIZE);             /* short tail → stays 0  */
            file->pos = save;
        }

        if (vmm_space_map(t->mm, page_va, fr, vf) != 0) {
            pmm_free_frame(fr);
            return -1;
        }
    }
    return (long)va;
}

/* §M37 — mprotect(addr,len,prot): change protection of already-mapped user
 * pages (musl's mallocng maps PROT_NONE then mprotects to R/W; ld.so tightens
 * RELRO to read-only after relocation).  Pages not mapped are skipped. */
long sys_mprotect(uintptr_t addr, size_t len, int prot) {
    struct task* t = task_current();
    if (!t || !t->mm) return -1;
    uintptr_t start = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t end   = (addr + len + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
    uint32_t vf = VMM_USER;                        /* user pages stay user */
    if (prot & PROT_WRITE) vf |= VMM_WRITABLE;
    if (prot & PROT_EXEC)  vf |= VMM_EXEC;
    for (uintptr_t va = start; va < end; va += PAGE_SIZE)
        vmm_space_protect(t->mm, va, vf);          /* ignore unmapped pages */
    return 0;
}

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

/* M34 — dup2(oldfd, newfd): make newfd refer to oldfd's object (closing any
 * prior newfd).  Real fds only (>= 3); the std streams have no ofile yet. */
int sys_dup2(int oldfd, int newfd) {
    struct ofile* o = fd_lookup(oldfd);
    if (!o) return -1;
    if (oldfd == newfd) return newfd;
    if (newfd < 3 || newfd >= TASK_MAX_FDS) return -1;
    struct task* t = task_current();
    if (t->fds[newfd]) { ofile_unref(t->fds[newfd]); t->fds[newfd] = NULL; }
    t->fds[newfd] = ofile_ref(o);
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
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    struct task* t = task_current();
    if (!t) return -1;
    /* 0/1/2 are RESERVED for the console and deliberately absent from the table
     * (fd_lookup rejects them, fd_install starts at 3).  A dup must obey the
     * same convention: handing back 0 produces a descriptor that looks valid to
     * the caller and can never be looked up again — which is precisely how a
     * Wayland client ended up passing descriptor zero to the compositor. */
    if (minfd < 3) minfd = 3;
    for (int i = minfd; i < TASK_MAX_FDS; i++) {
        if (t->fds[i]) continue;
        t->fds[i] = ofile_ref(o);
        return i;
    }
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
                (de.type == INODE_DEVICE) ? 2 : 8;                               /* DT_CHR/DT_REG */
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
        uint64_t ms = timer_ticks_ms();
        out->sec  = (uint32_t)(ms / 1000);
        out->nsec = (uint32_t)((ms % 1000) * 1000000u);
        return 0;
    }
    struct rtc_time t;
    if (rtc_read(&t) != 0) { out->sec = 0; out->nsec = 0; return 0; }
    out->sec  = rtc_to_epoch(&t);
    out->nsec = 0;
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

void fd_readiness_signal(void) {
    uint32_t f = waitq_lock(&readiness_wq);
    waitq_wake_all(&readiness_wq);
    waitq_unlock(&readiness_wq, f);
}

/* Fill each pollfd's revents with the currently-ready events; return the
 * number of fds with any requested event set (the readiness snapshot). */
static int poll_snapshot(struct pollfd* pfds, int nfds) {
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        struct pollfd* pf = &pfds[i];
        pf->revents = 0;
        int rd = 0, wr = 0;

        if (pf->fd == 0) {                         /* stdin: never ready today */
            rd = 0; wr = 0;
        } else if (pf->fd == 1 || pf->fd == 2) {   /* console out: always writable */
            wr = 1;
        } else {
            struct ofile* o = fd_lookup(pf->fd);
            if (o) {
                if (o->kind == FD_SOCK) { rd = usock_can_read(o->sock);
                                          wr = usock_can_write(o->sock); }
                else                    { rd = 1; wr = 1; }   /* VFS/shm: ready */
            }
        }
        if ((pf->events & POLLIN)  && rd) pf->revents |= POLLIN;
        if ((pf->events & POLLOUT) && wr) pf->revents |= POLLOUT;
        if (pf->revents) ready++;
    }
    return ready;
}

/* poll(2).  timeout == 0: non-blocking snapshot (the Wayland event-loop tick).
 * timeout  < 0: block until at least one fd is ready (the classic "wait for an
 * event" loop).  timeout  > 0: a finite millisecond wait is not honoured yet
 * (needs a timed wakeup — deferred with cron/watchdog's timed-sleep); treated
 * as a snapshot so it never blocks past the caller's intent. */
/* Core: `pfds` is always KERNEL memory (see the *_k note in syscall.h). */
int sys_poll_k(struct pollfd* pfds, int nfds, int timeout) {
    for (;;) {
        int ready = poll_snapshot(pfds, nfds);
        if (ready > 0 || timeout >= 0) return ready;   /* ready, or non-blocking */

        /* timeout < 0 → block until readiness changes, then re-scan.  Re-check
         * under the queue lock so a signal that races our snapshot isn't lost:
         * the socket layer makes an fd ready BEFORE it takes readiness_wq to
         * signal, so if we hold the lock and still see nothing ready, a wake
         * can only arrive after we park. */
        uint32_t f = waitq_lock(&readiness_wq);
        if (poll_snapshot(pfds, nfds) > 0) { waitq_unlock(&readiness_wq, f); continue; }
        waitq_block(&readiness_wq);
        waitq_unlock(&readiness_wq, f);
    }
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

void fd_close_all(void) {
    struct task* t = task_current();
    if (!t) return;
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
        if (t->fds[fd]) { ofile_unref(t->fds[fd]); t->fds[fd] = NULL; }
    }
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
    uint16_t local_port;
    int      bound;
    int      nonblock;                  /* SOCK_NONBLOCK / O_NONBLOCK        */
    struct ns_dgram rx[NS_RXSLOTS];
    volatile int rx_head, rx_tail;      /* head = produce, tail = consume */
    /* SOCK_STREAM (TCP) state. */
    int      connected;
    uint32_t peer_ip; uint16_t peer_port;
};

static uint16_t g_ephem_port = 0xC000;

/* net.c UDP-binding callback: enqueue an arriving datagram (drop if the ring
 * is full).  Runs in the receiver's task context (RX polled). */
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
    if (ns->type == SOCK_STREAM && ns->connected) {
        struct net_device* dev = net_primary();
        if (dev) net_tcp_close(dev);
    }
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
int sys_fd_kind(int fd) {
    struct ofile* o = fd_lookup(fd);
    return o ? (int)o->kind : -1;
}

int sys_socket_setnonblock(int fd, int on) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    o->nsock->nonblock = on ? 1 : 0;
    return 0;
}

int sys_socket_getnonblock(int fd) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    return o->nsock->nonblock;
}

int sys_bind(int fd, int port) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    return ns_ensure_bound(o->nsock, (uint16_t)port);
}

/* M24 — connect(fd, ip, port): TCP handshake for a SOCK_STREAM socket. */
int sys_connect(int fd, uint32_t ip, int port) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    if (ns->type != SOCK_STREAM) return -1;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    if (net_tcp_connect(dev, ip, (uint16_t)port) != 0) return -1;
    ns->connected = 1; ns->peer_ip = ip; ns->peer_port = (uint16_t)port;
    return 0;
}

/* Stream read/write over a connected SOCK_STREAM socket (called by
 * sys_read/sys_write when the fd is FD_NETSOCK). */
static long netsock_write(struct netsock* ns, const void* buf, size_t n) {
    if (ns->type != SOCK_STREAM || !ns->connected) return -1;
    struct net_device* dev = net_primary();
    return dev ? net_tcp_send(dev, buf, (uint32_t)n) : -1;
}
static long netsock_read(struct netsock* ns, void* buf, size_t n) {
    if (ns->type != SOCK_STREAM || !ns->connected) return -1;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    long r = net_tcp_recv(dev, buf, (uint32_t)n);
    /* Same contract as the datagram path: a non-blocking stream reports EAGAIN
     * rather than "connection produced nothing", so a caller's drain loop can
     * tell "no data right now" from "error". */
    if (r <= 0 && ns->nonblock) return -SOCK_EAGAIN;
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
    struct netsock* ns = o->nsock;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    /* Poll the RX ring until a datagram lands (bounded — no IRQ RX yet).
     *
     * A NON-BLOCKING socket gets exactly one pump of the device — enough to
     * pick up anything already on the wire, which is what a real IRQ-driven
     * driver would have done for us — and then reports EAGAIN instead of
     * spinning.  Without that distinction the bounded spin still "returns", but
     * only after tens of millions of iterations: on emulated i386 that is
     * minutes, which is indistinguishable from a hang and is exactly how this
     * broke musl's resolver. */
    if (ns->nonblock) {
        if (ns->rx_head == ns->rx_tail && dev->poll) dev->poll(dev);
        if (ns->rx_head == ns->rx_tail) return -SOCK_EAGAIN;
    } else {
        for (uint32_t spins = 0; spins < 40000000u; spins++) {
            if (ns->rx_head != ns->rx_tail) break;
            if (dev->poll) dev->poll(dev);
            hal_cpu_pause();
        }
        if (ns->rx_head == ns->rx_tail) return -1;     /* timeout */
    }
    struct ns_dgram* d = &ns->rx[ns->rx_tail];
    uint32_t cnt = (d->len < n) ? d->len : (uint32_t)n;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < cnt; i++) out[i] = d->data[i];
    if (ip_out)   *ip_out   = d->src_ip;
    if (port_out) *port_out = d->src_port;
    ns->rx_tail = (ns->rx_tail + 1) % NS_RXSLOTS;
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
