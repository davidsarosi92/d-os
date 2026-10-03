/* =============================================================================
 * fifo.c — named pipes (POSIX FIFOs), §M90.
 *
 * WHY.  containerd hands a container its stdout and stderr through FIFOs:
 * the client (dockerd) does `mkfifo …-stdout`, opens the read end, and the
 * shim opens the write end and gives it to the container's process.  Without
 * them `docker run` stops at "failed to open stdout fifo".  A shell's `mkfifo`
 * and every program that coordinates through a named pipe need the same.
 *
 * WHAT A FIFO IS.  A directory entry whose inode is INODE_FIFO (made by
 * mknod S_IFIFO / mkfifo, vfs_mkfifo).  The name carries no data: opening it
 * joins ONE in-memory pipe shared by every open of that inode, created at the
 * first open and discarded — with whatever it still held — when the last open
 * goes away.  That is Linux's behaviour, and it is why the pipe hangs off the
 * INODE (`inode->fifo`) and not off the name or a descriptor.
 *
 * THE RULES (Linux fs/pipe.c), each of which a program depends on:
 *   open O_RDONLY          waits until a writer opens (any writer since this
 *                          open began — a writer that came and went counts)
 *   open O_WRONLY          waits until a reader opens
 *   open O_RDONLY|O_NONBLOCK  returns at once
 *   open O_WRONLY|O_NONBLOCK  ENXIO when nobody reads
 *   open O_RDWR            never waits (it is both ends itself)
 *   read                   data; 0 (EOF) once no writer is left; else waits
 *                          (EAGAIN non-blocking)
 *   write                  EPIPE once no reader is left; a write of at most
 *                          PIPE_BUF bytes is never split
 *   poll, read side        POLLIN with data; POLLHUP when no writer is left
 *                          AND one has existed since this open — a reader that
 *                          opened non-blocking before any writer must NOT see
 *                          a hang-up, or an event loop reads EOF and stops
 *                          before the writer ever arrives
 *   poll, write side       POLLOUT while PIPE_BUF bytes fit; POLLERR when no
 *                          reader is left
 *
 * THE OPEN PATH.  A FIFO is opened as an ordinary VFS file first (so the
 * permission check, the inode hold that defers an unlink, /proc/self/fd/N and
 * fstat all work exactly as for any file), and the resulting FD_FIFO ofile is
 * then attached to the inode's pipe here.  `ofile->file` stays the VFS open;
 * the bytes never go near the filesystem.  O_PATH opens are NOT attached —
 * they are names, not ends (containerd's fifo package opens one to fstat it
 * and then re-opens through /proc/self/fd/N).
 *
 * LOCKING.  The pipe's waitq lock guards everything in it (waitq.h's condvar
 * discipline): readers, writers and openers all park on that one queue and
 * every change wakes them all to re-check.  `g_fifo_lock` guards only the
 * inode → pipe link and the pipe's user count, so creation and destruction
 * cannot race an open.
 * ============================================================================= */

#include "fifo.h"
#include "fd.h"
#include "vfs.h"
#include "kmalloc.h"
#include "waitq.h"
#include "lock.h"
#include "task.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

#define FIFO_CAP  65536u            /* Linux's default pipe capacity     */
#define PIPE_BUF_ 4096u             /* writes up to this are atomic      */

struct fifo {
    struct waitq wq;                /* its lock guards everything below */
    uint8_t*     buf;
    uint32_t     head, count;       /* ring: read cursor, bytes held    */
    int          readers, writers;  /* attached ends                    */
    uint32_t     r_gen, w_gen;      /* reader / writer opens ever       */
    int          users;             /* attached ofiles (g_fifo_lock)    */
    struct inode* ino;
};

static spinlock_t g_fifo_lock = SPINLOCK_INIT;

static void fifo_changed(struct fifo* p) {
    /* Called with p->wq held: wake every parked reader, writer and opener —
     * each re-checks its own condition — and tell poll/epoll waiters.  The
     * readiness wake is global (fd_readiness_changed ignores its argument),
     * which is what lets one pipe serve several ofiles without naming them. */
    waitq_wake_all(&p->wq);
}

/* Undo an attach (a failed open, or the last close of this end). */
static void fifo_put(struct fifo* p, unsigned role) {
    uint32_t f = waitq_lock(&p->wq);
    if (role & FIFO_R) p->readers--;
    if (role & FIFO_W) p->writers--;
    fifo_changed(p);
    waitq_unlock(&p->wq, f);
    fd_readiness_changed(NULL);

    uint32_t g = spin_lock_irqsave(&g_fifo_lock);
    int last = --p->users == 0;
    if (last && p->ino && p->ino->fifo == p) p->ino->fifo = NULL;
    spin_unlock_irqrestore(&g_fifo_lock, g);
    if (last) {                     /* nobody can reach it any more */
        kfree(p->buf);
        kfree(p);
    }
}

int fifo_attach(struct ofile* o, unsigned role, int nonblock) {
    if (!o || !o->file || !o->file->inode || !(role & (FIFO_R | FIFO_W))) return -22;
    struct inode* ino = o->file->inode;

    /* Find or create the pipe.  The allocation happens outside the spinlock
     * (kmalloc may sleep); a racing opener that created one first wins and
     * ours is thrown away. */
    struct fifo* fresh = NULL;
    for (;;) {
        uint32_t g = spin_lock_irqsave(&g_fifo_lock);
        struct fifo* p = (struct fifo*)ino->fifo;
        if (!p && fresh) { ino->fifo = p = fresh; fresh = NULL; }
        if (p) {
            p->users++;
            spin_unlock_irqrestore(&g_fifo_lock, g);
            if (fresh) { kfree(fresh->buf); kfree(fresh); }
            o->fifo = p;
            break;
        }
        spin_unlock_irqrestore(&g_fifo_lock, g);
        fresh = (struct fifo*)kcalloc(1, sizeof *fresh);
        uint8_t* b = fresh ? (uint8_t*)kmalloc(FIFO_CAP) : NULL;
        if (!fresh || !b) { kfree(fresh); kfree(b); return -12; }
        fresh->buf = b;
        fresh->ino = ino;
        waitq_init(&fresh->wq);
    }
    struct fifo* p = o->fifo;

    uint32_t f = waitq_lock(&p->wq);
    if ((role & FIFO_W) && !(role & FIFO_R) && nonblock && p->readers == 0) {
        waitq_unlock(&p->wq, f);
        o->fifo = NULL;
        fifo_put(p, 0);
        return -6;                                    /* ENXIO */
    }
    if (role & FIFO_R) { p->readers++; p->r_gen++; }
    if (role & FIFO_W) { p->writers++; p->w_gen++; }
    uint32_t r_seen = p->r_gen, w_seen = p->w_gen;
    fifo_changed(p);                                  /* an opener waiting for us */

    /* Wait for the other end, unless this open is both ends or non-blocking.
     * "A writer has opened since this open began" is a GENERATION change, not
     * "a writer is present now": a writer that opens, writes and closes while
     * we sleep has still answered us, and its data is waiting. */
    int killed = 0;
    if (!nonblock && role == FIFO_R) {
        while (p->writers == 0 && p->w_gen == w_seen && !(killed = task_should_stop()))
            waitq_block(&p->wq);
    } else if (!nonblock && role == FIFO_W) {
        while (p->readers == 0 && p->r_gen == r_seen && !(killed = task_should_stop()))
            waitq_block(&p->wq);
    }
    o->fifo_role  = role;
    o->fifo_wseen = (role == FIFO_R && nonblock) ? w_seen : w_seen - 1;
    waitq_unlock(&p->wq, f);
    if (killed) {
        o->fifo = NULL; o->fifo_role = 0;
        fifo_put(p, role);
        return -4;                                    /* EINTR */
    }
    fd_readiness_changed(NULL);
    return 0;
}

void fifo_detach(struct ofile* o) {
    if (!o || !o->fifo) return;
    struct fifo* p = o->fifo;
    unsigned role = o->fifo_role;
    o->fifo = NULL; o->fifo_role = 0;
    fifo_put(p, role);
}

long fifo_read(struct ofile* o, void* buf, size_t n, int block) {
    struct fifo* p = o ? o->fifo : NULL;
    if (!p || !(o->fifo_role & FIFO_R)) return -9;    /* EBADF */
    if (n == 0) return 0;
    uint32_t f = waitq_lock(&p->wq);
    for (;;) {
        if (p->count) {
            uint32_t k = p->count < n ? p->count : (uint32_t)n;
            for (uint32_t i = 0; i < k; i++)
                ((uint8_t*)buf)[i] = p->buf[(p->head + i) % FIFO_CAP];
            p->head = (p->head + k) % FIFO_CAP;
            p->count -= k;
            fifo_changed(p);                          /* a writer may proceed */
            waitq_unlock(&p->wq, f);
            fd_readiness_changed(NULL);
            return (long)k;
        }
        if (p->writers == 0) { waitq_unlock(&p->wq, f); return 0; }   /* EOF */
        if (!block)          { waitq_unlock(&p->wq, f); return -11; } /* EAGAIN */
        if (task_should_stop()) { waitq_unlock(&p->wq, f); return -4; }
        waitq_block(&p->wq);
    }
}

long fifo_write(struct ofile* o, const void* buf, size_t n, int block) {
    struct fifo* p = o ? o->fifo : NULL;
    if (!p || !(o->fifo_role & FIFO_W)) return -9;    /* EBADF */
    if (n == 0) return 0;
    size_t done = 0;
    uint32_t f = waitq_lock(&p->wq);
    while (done < n) {
        if (p->readers == 0) {
            waitq_unlock(&p->wq, f);
            return done ? (long)done : -32;           /* EPIPE */
        }
        uint32_t room = FIFO_CAP - p->count;
        /* A write of at most PIPE_BUF bytes is ATOMIC: it waits for room for
         * all of it rather than interleaving with another writer's. */
        size_t want = (n <= PIPE_BUF_) ? n : 1;
        if (room >= want && room > 0) {
            uint32_t k = (n - done) < room ? (uint32_t)(n - done) : room;
            uint32_t tail = (p->head + p->count) % FIFO_CAP;
            for (uint32_t i = 0; i < k; i++)
                p->buf[(tail + i) % FIFO_CAP] = ((const uint8_t*)buf)[done + i];
            p->count += k;
            done += k;
            fifo_changed(p);                          /* a reader has data */
            continue;
        }
        if (!block) { waitq_unlock(&p->wq, f); fd_readiness_changed(NULL);
                      return done ? (long)done : -11; }
        if (task_should_stop()) { waitq_unlock(&p->wq, f); return done ? (long)done : -4; }
        waitq_block(&p->wq);
    }
    waitq_unlock(&p->wq, f);
    fd_readiness_changed(NULL);
    return (long)done;
}

uint32_t fifo_readiness(struct ofile* o) {
    struct fifo* p = o ? o->fifo : NULL;
    if (!p) return POLLERR;
    uint32_t r = 0;
    uint32_t f = waitq_lock(&p->wq);
    if (o->fifo_role & FIFO_R) {
        if (p->count) r |= POLLIN;
        /* Linux: HUP only when a writer has opened since this open (see the
         * header) — fifo_wseen is the writer generation this open has seen. */
        if (p->writers == 0 && p->w_gen != o->fifo_wseen) r |= POLLHUP;
    }
    if (o->fifo_role & FIFO_W) {
        if (FIFO_CAP - p->count >= PIPE_BUF_) r |= POLLOUT;
        if (p->readers == 0) r |= POLLERR;
    }
    waitq_unlock(&p->wq, f);
    return r;
}
