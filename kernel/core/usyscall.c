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
 * User-pointer note: the dispatcher runs with the user's address space loaded
 * (its pages supervisor-readable) and the kernel mapped in every space, so a
 * direct dereference works.  A hardened copy_from/to_user is a later
 * refinement.
 * ============================================================================= */

#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "fd.h"
#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "waitq.h"
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE          4096u
#define MMAP_BASE_OFFSET   0x00400000u   /* mmap region: 4 MiB above the image */

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

long sys_write(int fd, const void* buf, size_t n) {
    if (fd == 1 || fd == 2) {                 /* stdout / stderr → console */
        const char* s = (const char*)buf;
        for (size_t i = 0; i < n; i++) console_putchar(s[i]);
        return (long)n;
    }
    if (fd == 0) return -1;                    /* can't write stdin */
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_VFS)  return (long)vfs_write(o->file, buf, n);
    if (o->kind == FD_SOCK) return usock_send(o->sock, buf, n, NULL);
    return -1;                                 /* shm: not write(2)-able */
}

long sys_read(int fd, void* buf, size_t n) {
    if (fd == 0) return 0;                      /* stdin: EOF for now (no tty) */
    if (fd == 1 || fd == 2) return -1;
    struct ofile* o = fd_lookup(fd);
    if (!o) return -1;
    if (o->kind == FD_VFS)  return (long)vfs_read(o->file, buf, n);
    /* Sockets read(2) with POSIX blocking semantics (block == 1): an empty
     * read waits for the peer to send (or close → 0/EOF). */
    if (o->kind == FD_SOCK) return usock_recv(o->sock, buf, n, 1, NULL);
    return -1;
}

int sys_open(const char* path, int flags) {
    if (!path) return -1;
    struct file* f = vfs_open(path, flags ? flags : VFS_RDONLY);
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
 * user VA (bump-allocated from task->mmap_cursor) or -1. */
long sys_mmap(size_t len, int fd) {
    struct task* t = task_current();
    if (!t || !t->mm) return -1;

    int n = (int)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    if (n <= 0) n = 1;
    if (t->mmap_cursor == 0)
        t->mmap_cursor = vmm_user_base() + MMAP_BASE_OFFSET;
    uintptr_t va = t->mmap_cursor;

    if (fd < 0) {
        for (int i = 0; i < n; i++) {
            uint32_t fr = pmm_alloc_frame();
            if (!fr) return -1;
            uint8_t* p = (uint8_t*)(uintptr_t)fr;
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
    t->mmap_cursor += (uintptr_t)n * PAGE_SIZE;
    return (long)va;
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

/* ---- unix sockets + fd passing (stage 5) ---------------------------------- */

int sys_socketpair(int* fds) {
    if (!fds) return -1;
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

long sys_send(int fd, const void* buf, size_t n, int passfd) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_SOCK) return -1;
    struct ofile* pf = (passfd >= 0) ? fd_lookup(passfd) : NULL;
    return usock_send(o->sock, buf, n, pf);
}

long sys_recv(int fd, void* buf, size_t n, int* passfd_out) {
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
int sys_poll(struct pollfd* pfds, int nfds, int timeout) {
    if (!pfds || nfds < 0) return -1;

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

void fd_close_all(void) {
    struct task* t = task_current();
    if (!t) return;
    for (int fd = 3; fd < TASK_MAX_FDS; fd++) {
        if (t->fds[fd]) { ofile_unref(t->fds[fd]); t->fds[fd] = NULL; }
    }
    t->mmap_cursor = 0;
}
