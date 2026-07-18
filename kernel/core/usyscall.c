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
#include "vc.h"
#include "waitq.h"
#include "net.h"
#include "hal_api.h"
#include "kmalloc.h"
#include "rtc.h"
#include "timer.h"
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

/* Forward decls — FD_NETSOCK stream I/O (defined with the socket layer below). */
static long netsock_write(struct netsock* ns, const void* buf, size_t n);
static long netsock_read (struct netsock* ns, void* buf, size_t n);

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
    if (o->kind == FD_NETSOCK) return netsock_write(o->nsock, buf, n);
    return -1;                                 /* shm: not write(2)-able */
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

long sys_read(int fd, void* buf, size_t n) {
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

    int n = (int)((len + PAGE_SIZE - 1) / PAGE_SIZE);
    if (n <= 0) n = 1;

    uint32_t vf = VMM_USER;
    if (prot & PROT_WRITE) vf |= VMM_WRITABLE;
    if (prot & PROT_EXEC)  vf |= VMM_EXEC;

    /* Target VA: MAP_FIXED honors the caller's addr; else bump-allocate. */
    uintptr_t va;
    if ((flags & MAP_FIXED) && addr) {
        va = addr & ~(uintptr_t)(PAGE_SIZE - 1);
    } else {
        if (t->mmap_cursor == 0)
            t->mmap_cursor = vmm_user_base() + MMAP_BASE_OFFSET;
        va = t->mmap_cursor;
        t->mmap_cursor += (uintptr_t)n * PAGE_SIZE;
    }

    /* File to read from for a file-backed mapping (else anonymous zero-fill). */
    struct file* file = NULL;
    if (fd >= 0 && !(flags & MAP_ANONYMOUS)) {
        struct ofile* o = fd_lookup(fd);
        if (!o || o->kind != FD_VFS || !o->file) return -1;
        file = o->file;
    }

    for (int i = 0; i < n; i++) {
        uintptr_t page_va = va + (uintptr_t)i * PAGE_SIZE;
        /* MAP_FIXED may overlay an earlier reservation — drop the old PTE so
         * the fresh frame maps cleanly. */
        if (flags & MAP_FIXED) vmm_space_unmap(t->mm, page_va);

        uint32_t fr = pmm_alloc_frame();
        if (!fr) return -1;
        uint8_t* p = (uint8_t*)(uintptr_t)fr;         /* kernel identity view  */
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

/* M34 — kill(pid, sig): post `sig` to task `pid`.  Delivery happens when that
 * task next returns to user mode (hal/x86/signal.c).  A task blocked in a
 * syscall won't notice until it returns (no EINTR yet — a follow-up). */
int sys_kill(int pid, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    struct task* t = task_find(pid);
    if (!t) return -1;
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

int sys_stat(const char* path, struct kstat* out) {
    if (!path || !out) return -1;
    struct file* f = vfs_open(path, VFS_RDONLY);
    if (!f) return -1;
    if (f->inode) {
        out->size = (uint32_t)f->inode->size;
        out->type = (int)f->inode->type;
        out->mode = (f->inode->type == INODE_DIR) ? 0755 : 0644;
    } else { out->size = 0; out->type = 0; out->mode = 0644; }
    vfs_close(f);
    return 0;
}

int sys_fstat(int fd, struct kstat* out) {
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

/* Pack directory entries into `buf` as [reclen(2) | type(1) | name\0] records. */
long sys_getdents(int fd, void* buf, size_t cap) {
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

/* Linux getdents64 packing (for the Linux-ABI backend, kernel/hal/x86/
 * linux_abi.c — musl's readdir uses SYS_getdents64).  Same VFS iteration as
 * sys_getdents, but emits the Linux `struct linux_dirent64` layout:
 *   u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[] (NUL-term).
 * Records are 8-byte aligned; d_type uses the Linux DT_* values. */
long sys_getdents64(int fd, void* buf, size_t cap) {
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

static void ustr(char* d, const char* s) {
    int i = 0; while (s[i] && i < 64) { d[i] = s[i]; i++; } d[i] = 0;
}
int sys_uname(struct kutsname* out) {
    if (!out) return -1;
    ustr(out->sysname,  "d-os");
    ustr(out->nodename, "d-os");
    ustr(out->release,  "0.1");
    ustr(out->version,  "M36 userland");
    ustr(out->machine,  "i386");        /* i386-first; arch string later */
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

int sys_clock_gettime(int which, struct ktimespec* out) {
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

int sys_nanosleep(unsigned ms) {
    task_msleep(ms);
    return 0;
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
    return dev ? net_tcp_recv(dev, buf, (uint32_t)n) : -1;
}

long sys_sendto(int fd, const void* buf, size_t n, uint32_t ip, int port) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    if (ns_ensure_bound(ns, 0) != 0) return -1;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    if (net_udp_send(dev, ip, ns->local_port, (uint16_t)port, buf, n) != 0) return -1;
    return (long)n;
}

long sys_recvfrom(int fd, void* buf, size_t n, uint32_t* ip_out, int* port_out) {
    struct ofile* o = fd_lookup(fd);
    if (!o || o->kind != FD_NETSOCK) return -1;
    struct netsock* ns = o->nsock;
    struct net_device* dev = net_primary();
    if (!dev) return -1;
    /* Poll the RX ring until a datagram lands (bounded — no IRQ RX yet). */
    for (uint32_t spins = 0; spins < 40000000u; spins++) {
        if (ns->rx_head != ns->rx_tail) break;
        if (dev->poll) dev->poll(dev);
        hal_cpu_pause();
    }
    if (ns->rx_head == ns->rx_tail) return -1;     /* timeout */
    struct ns_dgram* d = &ns->rx[ns->rx_tail];
    uint32_t cnt = (d->len < n) ? d->len : (uint32_t)n;
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t i = 0; i < cnt; i++) out[i] = d->data[i];
    if (ip_out)   *ip_out   = d->src_ip;
    if (port_out) *port_out = d->src_port;
    ns->rx_tail = (ns->rx_tail + 1) % NS_RXSLOTS;
    return (long)cnt;
}
