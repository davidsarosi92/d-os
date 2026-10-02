/* =============================================================================
 * flock.c — BSD advisory file locks (flock(2)), §M90.
 *
 * Why it exists: bbolt — the database under containerd's metadata, and under
 * a great deal of Go software — opens its file and takes flock(LOCK_EX) before
 * anything else, so that two processes cannot both believe they own it.  With
 * no flock every containerd plugin that touches metadata failed to load.
 *
 * What it is, precisely:
 *   - A lock belongs to an OPEN FILE DESCRIPTION (struct ofile), as on Linux:
 *     dup'd and forked descriptors share it, a second open() of the same file
 *     is a different owner and conflicts.
 *   - It is on the INODE: two paths to the same file (a hard link) collide.
 *   - LOCK_SH may be held by many owners at once; LOCK_EX by one, and only
 *     when nobody else holds anything.  Asking again CONVERTS the owner's
 *     lock (not atomically on Linux either — a conversion may block).
 *   - LOCK_NB answers EWOULDBLOCK instead of waiting.  A blocking request
 *     sleeps on one queue and is woken by every release; a kill ends the wait
 *     (EINTR), so a process stuck behind a dead peer's lock can be stopped.
 *   - Released by LOCK_UN, or when the last descriptor of the description is
 *     closed (ofile_unref calls flock_release) — which is also what frees a
 *     crashed holder's locks, because its descriptors are closed at exit.
 *
 * What it is NOT: POSIX record locks (fcntl F_SETLK) — byte ranges, owned by
 * the PROCESS, released by ANY close of the file.  Different semantics on
 * purpose in POSIX; they are not served by this table.
 *
 * Bounded (FLOCK_MAX entries); a request that would need an entry beyond it
 * gets ENOLCK rather than an allocation on a path many programs hit at start.
 * ============================================================================= */

#include "flock.h"
#include "fd.h"
#include "vfs.h"
#include "waitq.h"
#include "task.h"
#include <stddef.h>
#include <stdint.h>

#define FLOCK_MAX 64

struct flk {
    struct inode* ino;          /* NULL = free slot */
    struct ofile* owner;
    int           excl;         /* 1 = LOCK_EX, 0 = LOCK_SH */
};

/* One queue; its lock guards the table too (waitq.h: the queue lock IS the
 * condition's lock, which is what makes check-then-sleep race-free). */
static struct waitq g_flock_wq = WAITQ_INIT;
static struct flk   g_flk[FLOCK_MAX];

static struct inode* inode_of(struct ofile* o) {
    if (!o || o->kind != FD_VFS || !o->file) return NULL;
    return o->file->inode;
}

/* Caller holds the queue lock. */
static struct flk* own_locked(struct inode* ino, struct ofile* o) {
    for (int i = 0; i < FLOCK_MAX; i++)
        if (g_flk[i].ino == ino && g_flk[i].owner == o) return &g_flk[i];
    return NULL;
}
static int conflicts_locked(struct inode* ino, struct ofile* o, int excl) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct flk* f = &g_flk[i];
        if (f->ino != ino || f->owner == o) continue;
        if (excl || f->excl) return 1;
    }
    return 0;
}

#define LOCK_SH_ 1
#define LOCK_EX_ 2
#define LOCK_NB_ 4
#define LOCK_UN_ 8

int flock_op(struct ofile* o, int op) {
    struct inode* ino = inode_of(o);
    /* Not a file (a socket, a pipe): Linux accepts flock on any descriptor;
     * with nothing shared to protect here the honest answer is "granted". */
    if (!ino) return o ? 0 : -9;                               /* EBADF */
    int nb = (op & LOCK_NB_) != 0;
    op &= ~LOCK_NB_;
    if (op != LOCK_SH_ && op != LOCK_EX_ && op != LOCK_UN_) return -22;   /* EINVAL */

    uint32_t f = waitq_lock(&g_flock_wq);
    struct flk* mine = own_locked(ino, o);
    if (op == LOCK_UN_) {
        if (mine) { mine->ino = NULL; mine->owner = NULL; waitq_wake_all(&g_flock_wq); }
        waitq_unlock(&g_flock_wq, f);
        return 0;
    }
    int excl = (op == LOCK_EX_);
    /* A conversion first gives up what it held (Linux does the same), so a
     * waiter that only needed our shared lock gone can proceed. */
    if (mine && mine->excl != excl) {
        mine->ino = NULL; mine->owner = NULL; mine = NULL;
        waitq_wake_all(&g_flock_wq);
    }
    if (mine) { waitq_unlock(&g_flock_wq, f); return 0; }    /* already held as asked */
    while (conflicts_locked(ino, o, excl)) {
        if (nb) { waitq_unlock(&g_flock_wq, f); return -11; }   /* EWOULDBLOCK */
        if (task_should_stop()) { waitq_unlock(&g_flock_wq, f); return -4; }   /* EINTR */
        waitq_block(&g_flock_wq);                 /* re-acquires before returning */
    }
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (g_flk[i].ino) continue;
        g_flk[i].ino = ino; g_flk[i].owner = o; g_flk[i].excl = excl;
        waitq_unlock(&g_flock_wq, f);
        return 0;
    }
    waitq_unlock(&g_flock_wq, f);
    return -37;                                               /* ENOLCK */
}

void flock_release(struct ofile* o) {
    if (!o || o->kind != FD_VFS) return;
    uint32_t f = waitq_lock(&g_flock_wq);
    int any = 0;
    for (int i = 0; i < FLOCK_MAX; i++)
        if (g_flk[i].owner == o) { g_flk[i].ino = NULL; g_flk[i].owner = NULL; any = 1; }
    if (any) waitq_wake_all(&g_flock_wq);
    waitq_unlock(&g_flock_wq, f);
}
